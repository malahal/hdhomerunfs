#include <stdio.h>
#include <stdlib.h>
#include "hdhomerun/hdhomerun.h"

static volatile sig_atomic_t sigabort_flag = FALSE;
static volatile sig_atomic_t siginfo_flag = FALSE;

static void sigabort_handler(int arg)
{
	sigabort_flag = TRUE;
}

static void siginfo_handler(int arg)
{
	siginfo_flag = TRUE;
}

static void register_signal_handlers(sig_t sigpipe_handler, sig_t sigint_handler, sig_t siginfo_handler)
{
#if defined(SIGPIPE)
	signal(SIGPIPE, sigpipe_handler);
#endif
#if defined(SIGINT)
	signal(SIGINT, sigint_handler);
#endif
#if defined(SIGINFO)
	signal(SIGINFO, siginfo_handler);
#endif
}

static int cmd_scan(const char *device_str, const char *tuner_str)
{
	struct hdhomerun_device_t *hd;
	hd = hdhomerun_device_create_from_str(device_str, NULL);

	if (!hd) {
		fprintf(stderr, "failed to connect to device %s\n", device_str);
		return -1;
	}

	if (hdhomerun_device_set_tuner_from_str(hd, tuner_str) <= 0) {
		fprintf(stderr, "invalid tuner number\n");
		return -1;
	}

	char *ret_error;
	if (hdhomerun_device_tuner_lockkey_request(hd, &ret_error) <= 0) {
		fprintf(stderr, "failed to lock tuner\n");
		if (ret_error) {
			fprintf(stderr, "%s\n", ret_error);
		}
		return -1;
	}

	hdhomerun_device_set_tuner_target(hd, "none");

	char *channelmap;
	if (hdhomerun_device_get_tuner_channelmap(hd, &channelmap) <= 0) {
		fprintf(stderr, "failed to query channelmap from device\n");
		return -1;
	}

	const char *channelmap_scan_group = hdhomerun_channelmap_get_channelmap_scan_group(channelmap);
	if (!channelmap_scan_group) {
		fprintf(stderr, "unknown channelmap '%s'\n", channelmap);
		return -1;
	}

	if (hdhomerun_device_channelscan_init(hd, channelmap_scan_group) <= 0) {
		fprintf(stderr, "failed to initialize channel scan\n");
		return -1;
	}

	FILE *fp = stdout;

	register_signal_handlers(sigabort_handler, sigabort_handler, siginfo_handler);

	fprintf(fp, "[global]\ntuners = %s:%s\n\n[channelmap]\n", device_str, tuner_str);

	int ret = 0;
	while (!sigabort_flag) {
		struct hdhomerun_channelscan_result_t result;
		ret = hdhomerun_device_channelscan_advance(hd, &result);
		if (ret <= 0) {
			break;
		}

		ret = hdhomerun_device_channelscan_detect(hd, &result);
		if (ret < 0) {
			break;
		}
		if (ret == 0) {
			continue;
		}

		int i;
		for (i = 0; i < result.program_count; i++) {
			struct hdhomerun_channelscan_program_t *program = &result.programs[i];
			if (program->virtual_major > 0) {
				fprintf(fp, "%d.%d = %d %d %s\n", program->virtual_major, program->virtual_minor, result.frequency, program->program_number, program->name, result.channel_str, program->program_str);
			}
		}
	}

	hdhomerun_device_tuner_lockkey_release(hd);

	if (fp) {
		fclose(fp);
	}
	if (ret < 0) {
		fprintf(stderr, "communication error sending request to hdhomerun device\n");
	}
	return ret;
}


int main(int argc, char* argv[])
{
	struct hdhomerun_discover_device_t result_list[64];
	int count;

	if (argc < 3) {
		fprintf(stderr, "usage: %s <device> <tuner>\n\ndefaulting to first discoverable tuner...\n", argv[0]);

		count = hdhomerun_discover_find_devices_custom(0, HDHOMERUN_DEVICE_TYPE_TUNER, HDHOMERUN_DEVICE_ID_WILDCARD, result_list, 64);
		if (count < 0) {
			fprintf(stderr, "error sending discover request\n");
			return -1; 
		}
		if (count == 0) {
			printf("no devices found\n");
			return 0;
		}

		if (count > 0) {
			struct hdhomerun_discover_device_t *result = &result_list[0];
			char ip[16] = {0};

			sprintf(ip, "%u.%u.%u.%u", (unsigned int)(result->ip_addr >> 24) & 0x0FF, (unsigned int)(result->ip_addr >> 16) & 0x0FF,
			                           (unsigned int)(result->ip_addr >> 8) & 0x0FF, (unsigned int)(result->ip_addr >> 0) & 0x0FF);
			cmd_scan(ip, "0");
		}
	} else {
		cmd_scan(argv[1], argv[2]);
	}

	return 0;
}

