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

/* TODO: Read from a config file someday */
static char *save_file_name = "./hdhomerun_live.ts";
static char *hdhomerun_id = "192.168.1.133";
static char *hdhomerun_tuner = "1";
static char *hdhomerun_config = "./hdhomerun_config";

struct vchannel {
	char *name;
	char *channel;
	char *program;
} vchannel;

/*
 * channel map:
 *
 * First entry is the channel file name that appears in the fuse FS.
 * Second one is physical RF channel (or frequency) that channel
 * 	value that hdhomerun_config takes.
 * Third entry is the 'program' number in the above RF stream.
 *
 * The second and third fields are passed to hdhomerun_config
 */
static struct vchannel vchannels[] = {
	{"/KATU-2.2.ts", "8vsb:43", "4"},
	{"/KGW-8.1.ts",  "8vsb:8", "3"},
	{"/WEATHER-8.2.ts",  "8vsb:8", "4"},

	{"/OPB-10.2.ts", "8vsb:10", "4"},
	{"/OPB-Plus-10.2.ts", "8vsb:10", "5"},
	{"/OPB-FM-10.4.ts", "8vsb:10", "6"},

	{"/QUBO-22.2.ts", "8vsb:22", "4"},

	{"/KPDX-49.1.ts", "8vsb:30", "3"},
	{"/KPTV-12.1.ts", "8vsb:12", "3"},
};
#define NUM_VCHANNELS (sizeof(vchannels)/sizeof(vchannels[0]))

/**************************************************************************/
/************ END OF CONFIG DATA ******************************************/
/**************************************************************************/

/* Globals */
static int save_file_fd = -1;
static int save_process_pid = -1;
static int last_open_file_index = -1;
static int read_counter = 0;
static int debug = 0;

#define MAX_FILE_SIZE (8 * 1024 * 1024 * 1024ULL)

static int path_index(const char *path)
{
	int i;

	for (i = 0; i < NUM_VCHANNELS; i++) {
		if (strcmp(path, vchannels[i].name) == 0) {
			break;
		}
	}

	return i;
}

static int hdhr_getattr(const char *path, struct stat *stbuf)
{
	int res;
	int index;

	memset(stbuf, 0, sizeof(struct stat));
	if (strcmp(path, "/") == 0) {
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
		return 0;
	}

	res = 0;
	index = path_index(path);
	if (index == last_open_file_index) {
		/* TODO: Just fake it like below and avoid a stat ??? */
		stat(save_file_name, stbuf);
		stbuf->st_size = 2 * MAX_FILE_SIZE;
	} else if (index != NUM_VCHANNELS) {
		stbuf->st_mode = S_IFREG | 0444;
		stbuf->st_nlink = 1;
		stbuf->st_size = 2 * MAX_FILE_SIZE;
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
		for (i = 0; i < NUM_VCHANNELS; i++) {
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
	if (path_index(path) != NUM_VCHANNELS) { /* Channel file */
	    return 0;
	}
	return -ENOENT;
}

static int hdhr_release(const char *path, struct fuse_file_info *fi)
{
	if (debug) {
		printf("close called for path: %s\n", path);
	}
	if (path_index(path) != NUM_VCHANNELS) { /* Channel file */
	    return 0;
	}
	return -ENOENT;
}

static pid_t hdhomerun_save(void)
{
	pid_t pid;
	char tuner[10];

	pid = fork();
	if (pid == 0) { /* Child */
		sprintf(tuner, "/tuner%s", hdhomerun_tuner);
		if (execl(hdhomerun_config, hdhomerun_config, hdhomerun_id,
			  "save", tuner, save_file_name, (char *)NULL) == -1) {
			/* TODO */
		}
	} else if (pid != -1) { /* Parent */
		return pid;
	} else {
		return -1;
	}
}

/* This function must be run while blocking ALARM signal */
static int hdhr_set_save(int index)
{
	char cmd[100];

	sprintf(cmd, "%s %s set /tuner%s/channel %s", hdhomerun_config,
	        hdhomerun_id, hdhomerun_tuner, vchannels[index].channel);
	if (debug) {
		printf("Executing: %s\n", cmd);
	}
	if (system(cmd) != 0) {
		return 0;
	}

	sprintf(cmd, "%s %s set /tuner%s/program %s", hdhomerun_config,
	        hdhomerun_id, hdhomerun_tuner, vchannels[index].program);
	if (debug) {
		printf("Executing: %s\n", cmd);
	}
	if (system(cmd) != 0) {
		return 0;
	}

	if (save_file_fd < 0) {
		save_file_fd = open(save_file_name, O_RDWR | O_CREAT, 0755);
		if (save_file_fd < 0) {
			return 0;
		}
	}

	if (save_process_pid != -1) {
		kill(save_process_pid, SIGKILL);
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
	int index;
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

	save_size = save_file_size();
	if (offset <= save_size) {
		while (offset + size > save_size) {
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
		read(save_file_fd, buf, size);
	} else {
		if (debug) {
			printf("Going to be a FAKE read - saved size: %llu, "
			       "offset: %llu, size: %zu\n",
			       save_size, offset, size);
		}
		lseek(save_file_fd, 0, SEEK_SET);
		size = read(save_file_fd, buf, size);
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
		} else if (opt == 's') {
			single_threaded = 1;
		} else if (opt == 'd') {
			debug = 1;
			argv[i][1] = 'f'; /* Change it to foreground!!! */
		}
	}

	if ((argc -i) != 2) {
		fprintf(stderr, "%s [options] conffile mountpoint\n",
			argv[0]);
		exit(1);
	}

	conffile = argv[i];
	mountpoint = argv[i+1];
	if (!single_threaded) {
		argv[i] = "-s";
	} else {
		argv[i] = mountpoint;
		argv[i+1] = NULL;
	}

	return fuse_main(argc, argv, &hdhr_ops, NULL);
}
