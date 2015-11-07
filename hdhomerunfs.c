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
#include "mmapring/mmapring.h"
#include "inih/ini.h"

/*
 * channel map:
 *
 * First entry is the channel file name that appears in the fuse FS.
 * Second one is physical the RF channel (or frequency) 
 * Third entry is the 'program' number in the above RF stream.
 *
 */
struct vchannel {
	char name[64];
	int  channel;
	int  program;
} vchannel;

/* Globals */
static struct vchannel *vchannels;
static int num_vchannels = 0;
static char *save_file_name;
static int hdhomerun_tuner = 0;
static char hdhomerun_id[64];
static pthread_t save_process_thread;
static pthread_mutex_t lock;
static int save_thread_running = 0;
static int last_open_file_index = -1;
static int read_counter = 0;
static int debug = 0;
static mmapring_t *save_ring;


#define MIN_FILE_SIZE (512 * 1024)
#define MAX_FILE_SIZE (64 * 1024 * 1024ULL)

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
	if (index != num_vchannels) {
		off_t save_size = save_ring->written;
		stat(save_file_name, stbuf);

		stbuf->st_mode = S_IFREG | 0444;
		stbuf->st_nlink = 1;
		stbuf->st_size = save_size < MIN_FILE_SIZE ? MIN_FILE_SIZE : save_size;

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

static void *hdhomerun_save(void *edata)
{
	struct hdhomerun_device_t *hd;
	char tuner[10] = {0};

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
	while (save_thread_running) {
		size_t actual_size;
		uint8_t *ptr = hdhomerun_device_stream_recv(hd, VIDEO_DATA_BUFFER_SIZE_1S, &actual_size);
		if (ptr) {
			pthread_mutex_lock(&lock);
			mmapring_append(save_ring, ptr, actual_size);
			pthread_mutex_unlock(&lock);
		}
		usleep(64000);
	}

	/* cleanup */
	hdhomerun_device_stream_stop(hd);
	hdhomerun_device_destroy(hd);

	fprintf(stderr, "save thread exiting...\n");
	pthread_exit(NULL);

	return NULL;
}

static int spawn_thread(void *(*func)(void *), pthread_t *thread_id, void *edata) {
	pthread_attr_t attr;
	int result = 0;

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

	if (0 != pthread_create(thread_id, &attr, func, edata)) {
		result = -1;
	}
	return result;
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
	sprintf(value, "auto:%d", vchannels[index].channel);

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

	hdhomerun_device_destroy(hd);

	if (save_thread_running) {
		fprintf(stderr, "need to kill save thread %x\n", save_process_thread);
		save_thread_running = 0;
		int join_ret = pthread_join(save_process_thread, NULL);
		fprintf(stderr, "got %d from join\n", join_ret);
	}

	pthread_mutex_lock(&lock);
	mmapring_reset(save_ring);
	pthread_mutex_unlock(&lock);

	if (spawn_thread(hdhomerun_save, &save_process_thread, NULL) < 0) {
		return 0;
	}
	save_thread_running = 1;
	last_open_file_index = index;

	return 1;
}

static int hdhr_read(const char *path, char *buf, size_t size, off_t offset,
		struct fuse_file_info *fi)
{
	off_t save_size = save_ring->written;
	int index, retry;
	sigset_t sigset;

	index = path_index(path);

	sigemptyset(&sigset);
	sigaddset(&sigset, SIGALRM);
	sigprocmask(SIG_BLOCK, &sigset, NULL);
	read_counter++;

	if (save_thread_running < 1 || last_open_file_index != index) {
		hdhr_set_save(index);
	}
	sigprocmask(SIG_UNBLOCK, &sigset, NULL);

	if (save_thread_running < 1) {
		return -EIO;
	}

	offset = offset % MAX_FILE_SIZE;
	if (offset < MAX_FILE_SIZE) {
		retry = 5; /* limit the wait */
		pthread_mutex_lock(&lock); 
		save_size = save_ring->written;
		save_size = save_size > MAX_FILE_SIZE ? MAX_FILE_SIZE : save_size;
		pthread_mutex_unlock(&lock);
		while (offset + size > save_size && save_size < MAX_FILE_SIZE && retry--) {
			if (debug) {
				printf("SLEEPING to grow - saved size: %llu, "
				       "offset: %llu, size: %zu\n",
				       save_size, offset, size);
			}
			sleep(1);
			pthread_mutex_lock(&lock);
			save_size = save_ring->written;
			save_size = save_size > MAX_FILE_SIZE ? MAX_FILE_SIZE : save_size;
			pthread_mutex_unlock(&lock);
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
		memcpy(buf, save_ring->base+offset, size);
	} else {
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
		if (debug) {
			printf("stopping save thread\n");
		}
		if (save_thread_running) {
			save_thread_running = 0;
			pthread_join(save_process_thread, NULL);
		}
	}
	old_read_counter = read_counter;

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
		printf("exiting....\n");
	}
	if (save_thread_running) {		
		save_thread_running = 0;
		pthread_join(save_process_thread, NULL);
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

static void add_channel(const char *vchannel, const char *pchannel, const char *program, const char *name)
{
	struct vchannel *channel;

	vchannels = realloc(vchannels, sizeof(struct vchannel) *
			    (num_vchannels + 1));
	channel = &vchannels[num_vchannels];
	snprintf(channel->name, sizeof(channel->name), "/%s-%s.ts", vchannel, name);
	channel->channel = strtol(pchannel, NULL, 10);
	channel->program = strtol(program,  NULL, 10);
	num_vchannels++;
}

static int read_config(void* user, const char* section, const char* name, const char* value)
{
	if (strcmp(section, "global") == 0 && strcmp(name, "tuners") == 0) {
		int delim_span = strcspn(value, ":") + 1;
		if (delim_span >= sizeof(hdhomerun_id)) return 0;
		snprintf(hdhomerun_id, delim_span, value);
		hdhomerun_tuner = strtol(value + delim_span, NULL, 10);
	} else if (strcmp(section, "channelmap") == 0) {
		const char *vchannel, *pchannel, *program, *channel_name;
		vchannel = name;
		pchannel = value;
		program  = strchr(value, ' ');
		if (!program) return 0;	program+=1;
		channel_name = strchr(program+1, ' ');
		if (!channel_name) return 0; channel_name+=1;

		if (!vchannel || !pchannel || !program || !name) {
			fprintf(stderr, "incorrect syntax in config file: %s = %s\n", name, value);
			return 0;
		} else if (strtol(program, NULL, 10) == 0) {
			fprintf(stderr, "incorrect channel program: %s, for channel %s: %s\n", program, name, value);
			return 0;
		}

		add_channel(vchannel, pchannel, program, channel_name);
	} else {
		return 0;  /* unknown section/name, error */
	}
	return 1;
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

	if (ini_parse(conffile, read_config, NULL) < 0 || strlen(hdhomerun_id) < 0) {
		fprintf(stderr, "error in config file, please fix it\n");
		exit(2);
	}

	save_ring = mmapring_create(save_file_name, MAX_FILE_SIZE);
	pthread_mutex_init(&lock, NULL);

	int ret = fuse_main(argc, argv, &hdhr_ops, NULL);

	mmapring_destroy(save_ring);
	free(vchannels);

	return ret;
}
