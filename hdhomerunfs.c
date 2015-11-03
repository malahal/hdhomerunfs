/*
 * Fuse file system driver for HDHOMERUN network tuner.
 */
#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <signal.h>
#include <dirent.h>
#include <stdlib.h>
#include <sys/wait.h>

#include "hdhomerun/hdhomerun.h"

/*
 * channel map:
 *
 * First entry is the channel file name that appears in the fuse FS.
 * Second one is physical the RF channel (or frequency) 
 * Third entry is the 'program' number in the above RF stream.
 *
 */
struct vchannel {
	char *name;
	char *channel;
	int  program;
} vchannel;

/* Globals */
static struct vchannel *vchannels;
static int num_vchannels;
static char *save_file_name;
static int hdhomerun_tuner;
static char *hdhomerun_id;
static int save_file_fd = -1;
static int save_process_pid = -1;
static int last_open_file_index = -1;
static int read_counter = 0;
static int debug = 0;


/*
 * Some players notably WDTV Live media player reads 10MB at the end of
 * a file before starting the play. We set the channel file size to
 * MAX_FILE_SIZE + ZERO_SIZE + FAKE_SIZE.
 *
 * Any read upto MAX_FILE_SIZE is real and we wait for the save_file to grow.
 * Any read from MAX_FILE_SIZE to MAX_FILE_SIZE+ZERO_SIZE is returned zeros.
 * Any read from MAX_FILE_SIZE+ZERO_SIZE and beyond is a FAKE read and
 * returned with the first part of the file!
 *
 * FAKE_SIZE should be at least 10MB for WDTV Live, but we make it 100MB
 * here!
 */
#define MAX_FILE_SIZE (8 * 1024 * 1024 * 1024ULL)
#define ZERO_SIZE (100 * 1024 * 1024ull)
#define FAKE_SIZE (100 * 1024 * 1024ULL)

static int path_index(const char *path)
{
	int i;

	for (i = 0; i < num_vchannels; i++) {
		if (strcmp(path, vchannels[i].name) == 0) {
			break;
		}
	}

	return i;
}

static int channel_file(const char *path)
{
	return path_index(path) != num_vchannels;
}

static int hdhr_getattr(const char *path, struct stat *stbuf)
{
	int res;
	int index;

	memset(stbuf, 0, sizeof(struct stat));
	if (strcmp(path, "/") == 0) {
		stbuf->st_mode = S_IFDIR | 0555;
		stbuf->st_nlink = 2;
		return 0;
	}

	res = 0;
	index = path_index(path);
	if (index == last_open_file_index) {
		/* TODO: Just fake it like below and avoid a stat ??? */
		stat(save_file_name, stbuf);
		stbuf->st_size = MAX_FILE_SIZE + ZERO_SIZE + FAKE_SIZE;
	} else if (index != num_vchannels) {
		stbuf->st_mode = S_IFREG | 0444;
		stbuf->st_nlink = 1;
		stbuf->st_size = MAX_FILE_SIZE + ZERO_SIZE + FAKE_SIZE;
	} else {
		res = -ENOENT;
	}

	return res;
}

static int hdhr_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi)
{
	(void) offset;
	(void) fi;
	int i;

	if (strcmp(path, "/") == 0) {
		filler(buf, ".", NULL, 0);
		filler(buf, "..", NULL, 0);
		for (i = 0; i < num_vchannels; i++) {
			filler(buf, vchannels[i].name + 1, NULL, 0);
		}
	}

	return 0;
}

static int hdhr_open(const char *path, struct fuse_file_info *fi)
{
	if (debug) {
		printf("open called for path: %s\n", path);
	}
	if (channel_file(path)) {
	    return 0;
	}
	return -ENOENT;
}

static int hdhr_release(const char *path, struct fuse_file_info *fi)
{
	if (debug) {
		printf("close called for path: %s\n", path);
	}
	if (channel_file(path)) {
	    return 0;
	}
	return -ENOENT;
}

static pid_t hdhomerun_save(void)
{
	pid_t pid;
	struct hdhomerun_device_t *hd;
	char tuner[10] = {0};

	pid = fork();
	if (pid == 0) { /* Child */

	hd = hdhomerun_device_create_from_str(hdhomerun_id, NULL);
	sprintf(tuner, "%d", hdhomerun_tuner);
	if (hdhomerun_device_set_tuner_from_str(hd, tuner) <= 0) {
		fprintf(stderr, "invalid tuner number\n");
		return NULL;
	}

	int ret = hdhomerun_device_stream_start(hd);
	if (ret <= 0) {
		fprintf(stderr, "unable to start stream\n");
		return NULL;
	}
	/* loop */
	while (1) {
		size_t actual_size;
		uint8_t *ptr = hdhomerun_device_stream_recv(hd, VIDEO_DATA_BUFFER_SIZE_1S, &actual_size);
		if (ptr) {
			FILE *out = fopen(save_file_name, "a+");
			int ret = fwrite(ptr, actual_size, 1, out);
			fclose(out);
		}
		usleep(64000);
	}

	/* cleanup */
	hdhomerun_device_stream_stop(hd);
	hdhomerun_device_destroy(hd);

	} else if (pid != -1) { /* Parent */
		return pid;
	} else {
		return -1;
	}
}


/* helper function to set device parameters */
static int hdhr_set(struct hdhomerun_device_t *hd, char *item, char *value)
{
	char *ret_error;
	if (hdhomerun_device_set_var(hd, item, value, NULL, &ret_error) < 0) {
		fprintf(stderr, "communication error sending request to hdhomerun device\n");
		return -1;
	}

	if (ret_error) {
		fprintf(stderr, "%s\n", ret_error);
		return 0;
	}
	return 1;
}

/* This function must be run while blocking ALARM signal */
static int hdhr_set_save(int index)
{
	char item[100];
	char value[100];
	char *ret_error;

	struct hdhomerun_device_t *hd;
	const char *model;

	hd = hdhomerun_device_create_from_str(hdhomerun_id, NULL);
	model = hdhomerun_device_get_model_str(hd);
	if (!model) {
		fprintf(stderr, "unable to connect to device\n");
		hdhomerun_device_destroy(hd);
		return 0;
	}
	if (debug) {
		fprintf(stderr, "found hdhr model: %s\n", model);
	}

	sprintf(item, "/tuner%d/channel", hdhomerun_tuner);
	sprintf(value, "8vsb:%s", vchannels[index].channel);
	if (debug) {
		printf("Executing: %s:%s\n", item, value);
	}
	if (hdhr_set(hd, item, value) < 1) {
		return 0;
	}

	sprintf(item, "/tuner%d/program", hdhomerun_tuner);
	sprintf(value, "%d", vchannels[index].program);
	if (debug) {
		printf("Executing: %s:%s\n", item, value);
	}
	if (hdhr_set(hd, item, value) < 1) {
		return 0;
	}

	if (save_file_fd < 0) {
					save_file_fd = open(save_file_name, O_RDWR | O_CREAT, 0755);
					if (save_file_fd < 0) {
									return 0;
					}
	}

	hdhomerun_device_destroy(hd);

	if (save_process_pid != -1) {
		kill(save_process_pid, SIGKILL);
		waitpid(save_process_pid, NULL, 0);
		save_process_pid = -1;
	}

	ftruncate(save_file_fd, 0);
	save_process_pid = hdhomerun_save();
	if (save_process_pid == -1) {
		return 0;
	}
	last_open_file_index = index;

	return 1;
}

off_t save_file_size(void)
{
	struct stat buf;

	if (fstat(save_file_fd, &buf) == 0) {
		return buf.st_size;
	}
	return 0; /* As good as an error */
}

static int hdhr_read(const char *path, char *buf, size_t size, off_t offset,
		     struct fuse_file_info *fi)
{
	size_t len;
	(void) fi;
	off_t save_size;
	int index, retry;
	sigset_t sigset;

	index = path_index(path);

	sigemptyset(&sigset);
	sigaddset(&sigset, SIGALRM);
	sigprocmask(SIG_BLOCK, &sigset, NULL);
	read_counter++;
	if (save_process_pid == -1 || last_open_file_index != index) {
		hdhr_set_save(index);
	}
	sigprocmask(SIG_UNBLOCK, &sigset, NULL);

	if (save_process_pid == -1) {
		return -EIO;
	}

	if (offset < MAX_FILE_SIZE) {
		retry = 5; /* limit the wait */
		save_size = save_file_size();
		while (offset + size > save_size && retry--) {
			if (debug) {
				printf("SLEEPING to grow - saved size: %llu, "
				       "offset: %llu, size: %zu\n",
				       save_size, offset, size);
			}
			sleep(1);
			save_size = save_file_size();
		}
	}

	if (offset < save_size) {
		if (offset + size > save_size) {
			if (debug) {
				printf("Going to be a SHORT read - "
				       "saved size: %llu, offset: %llu, "
				       "size: %zu\n",
				       save_size, offset, size);
			}
			size = save_size - offset;
		}
		lseek(save_file_fd, offset, SEEK_SET);
		size = read(save_file_fd, buf, size);
	} else if (offset >= MAX_FILE_SIZE + ZERO_SIZE) {
		/* FAKE region */
		if (debug) {
			printf("Going to be a FAKE read - saved size: %llu, "
			       "offset: %llu, size: %zu\n",
			       save_size, offset, size);
		}
		lseek(save_file_fd, 0, SEEK_SET);
		size = read(save_file_fd, buf, size);
	} else {
		/* 
		 * This is either ZERO region read or we could not wait
		 * for the actual data to materialize.
		 */
		size = 0; /* Reached end of the file really! */
	}

	return size;
}

static void set_up_alarm(void);
static void sig_handler(int signum)
{
	off_t save_size;
	static int old_read_counter;

	if (debug) {
		printf("alarm handler called; old: %d, new: %d\n",
		       old_read_counter, read_counter);
	}

	if (read_counter == old_read_counter) {
		/* No reads since the last alarm */
		if (save_process_pid != -1) {
			if (debug) {
				printf("killing pid: %d\n", save_process_pid);
			}
			kill(save_process_pid, SIGKILL);
			waitpid(save_process_pid, NULL, 0);
			save_process_pid = -1;
		}
	}
	old_read_counter = read_counter;

	/* 
	 * TODO: Ideally we would like to save the stream in two
	 * files for circular ring implementation! For now, truncate
	 * it and let the user restart watching!
	 */
	save_size = save_file_size();
	if (save_size > MAX_FILE_SIZE) {
		truncate(save_file_name, 0);
	}

	set_up_alarm();
}

static void set_up_alarm(void)
{
	(void)signal(SIGALRM, sig_handler);
	alarm(10 * 60);
}

static void *hdhr_init(struct fuse_conn_info *conn)
{
	set_up_alarm();
	return NULL;
}

static void hdhr_destroy(void *arg)
{
	if (debug) {
		printf("destroy called\n");
	}
	if (save_process_pid != -1) {
		kill(save_process_pid, SIGKILL);
		waitpid(save_process_pid, NULL, 0);
		save_process_pid = -1;
	}
}

static struct fuse_operations hdhr_ops = {
	.getattr	= hdhr_getattr,
	.readdir	= hdhr_readdir,
	.open		= hdhr_open,
	.release	= hdhr_release,
	.read		= hdhr_read,
	.init           = hdhr_init,
	.destroy        = hdhr_destroy,
};

static void add_channel(char *vchannel, char *pchannel, char *program,
			char *name)
{
	struct vchannel *channel;
	char channel_name[100];

	vchannels = realloc(vchannels, sizeof(struct vchannel) *
			    (num_vchannels + 1));
	channel = &vchannels[num_vchannels];
	snprintf(channel_name, sizeof(channel_name), "/%s-%s.ts", vchannel,
		 name);
	channel->name = strdup(channel_name);
	channel->channel = strdup(pchannel);
	channel->program = strtol(program, NULL, 10);
	num_vchannels++;
}

#define MAXLINE 100
static int read_config(const char *conffile)
{
	FILE *fp;
	char line[MAXLINE];
	char buf[MAXLINE];
	char *token;
	char *vchannel, *pchannel, *program, *name;

	fp = fopen(conffile, "r");
	if (fp == NULL) {
		fprintf(stderr, "fopen of %s failed: %s\n", conffile,
		       strerror(errno));
		exit(1);
	}
	while (fgets(line, sizeof(buf), fp) != NULL) {
		strcpy(buf, line);
		token = strtok(buf, " \t\n");
		if (token == NULL) {
			continue;
		} else if (token[0] == '#' || token[0] == ';') {
			continue;
		} else if (strcmp(token, "[global]") == 0) {
			continue;
		} else if (strcmp(token, "[channelmap]") == 0) {
			continue;
		} else if (strcmp(token, "tuners") == 0) {
			hdhomerun_id = strdup(strtok(NULL, "=: \t\n"));
			hdhomerun_tuner = atoi(strtok(NULL, ":, \t\n"));
		} else {
			vchannel = token;
			pchannel = strtok(NULL, "= \t");
			program = strtok(NULL, " \t");
			name = strtok(NULL, " \t\n");
			if (!vchannel || !pchannel || !program || !name) {
				fprintf(stderr, "incorrect syntax in file %s "
					"line: %s\n", conffile, line);
				return 0;
			} else if (atoi(program) == 0) {
				fprintf(stderr, "incorrect channel program: "
					"%s, in file %s line: %s\n", program,
					conffile, line);
				return 0;
			}
			add_channel(vchannel, pchannel, program, name);
		}
	}

	if (hdhomerun_id) {
		return 1;
	} else {
		return 0;
	}
}

int main(int argc, char *argv[])
{
	char *conffile, *mountpoint;
	int i, single_threaded = 0;
	char opt;

	/*
	 * If single threaded option (-s) is not passed, add it here
	 * as we fail to work in multi-threaded environment. -d option
	 * is for debugging, so change it to -f before passing them
	 * to fuse.
	 *
	 * All the options should come first. Non option arguments are
	 * the config file name and mount point.
	 */
	for (i = 1; i < argc && argv[i][0] == '-'; i++) {
		opt = argv[i][1];
		if (opt == 'o') {
			i++; /* -o takes a parameter */
		} else if (opt == 'd') { /* Our debug option */
			debug = 1;
			argv[i][1] = 'f'; /* Change it to foreground!!! */
		}
	}

	if ((argc - i) != 3) {
		fprintf(stderr, "%s [options] savefile conffile mountpoint\n",
			argv[0]);
		exit(1);
	}

	save_file_name = argv[i];
	conffile = argv[i+1];
	mountpoint = argv[i+2];

	/*
	 * We could move argv pointers around, but duplicate
	 * options work fine with fuse, so just replace
	 * the savefile and conffile args with "-s" option
	 * as we need single threaded event loop.
	 */
	argv[i] = "-s";
	argv[i+1] = "-s";

	if (fopen(save_file_name, "w") == NULL) {
		fprintf(stderr, "Can't open %s file for writing: %s\n",
			save_file_name, strerror(errno));
		exit(2);
	}
	if (!read_config(conffile)) {
		fprintf(stderr, "error in config file, please fix it\n");
		exit(2);
	}

	return fuse_main(argc, argv, &hdhr_ops, NULL);
}
