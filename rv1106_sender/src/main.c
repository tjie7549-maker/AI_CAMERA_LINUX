#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "ai_cam.h"

static void print_usage(const char *name) {
	printf("usage: %s [-a iq_dir] [-w vi_width] [-h vi_height] [-W vo_width] "
	       "[-H vo_height] [-r rotation] [-I vi_channel] [-l vo_layer] "
	       "[-d vo_device] [-o h264_file] [-n encoded_frames]\n", name);
}

int main(int argc, char *argv[]) {
	AiCamApp app = {0};
	sigset_t shutdown_signals;
	struct timespec wait_timeout = {0, 50000000};
	int option;

	ai_cam_default_config(&app.config);
	while ((option = getopt(argc, argv, "a:w:h:W:H:r:I:l:d:o:n:")) != -1) {
		switch (option) {
		case 'a': app.config.iq_file_dir = optarg; break;
		case 'w': app.config.vi_width = atoi(optarg); break;
		case 'h': app.config.vi_height = atoi(optarg); break;
		case 'W': app.config.vo_width = atoi(optarg); break;
		case 'H': app.config.vo_height = atoi(optarg); break;
		case 'r': app.config.vo_rotation_degrees = atoi(optarg); break;
		case 'I': app.config.vi_channel = atoi(optarg); break;
		case 'l': app.config.vo_layer = atoi(optarg); break;
		case 'd': app.config.vo_device = atoi(optarg); break;
		case 'o': app.config.h264_output_path = optarg; break;
		case 'n':
			app.config.venc_frame_limit = atoi(optarg);
			if (app.config.venc_frame_limit <= 0) {
				fprintf(stderr, "encoded frame count must be positive\n");
				return 1;
			}
			break;
		default:
			print_usage(argv[0]);
			return 1;
		}
	}

	printf("#VI: %dx%d, LCD: %dx%d, rotation: %d\n", app.config.vi_width,
	       app.config.vi_height, app.config.vo_width, app.config.vo_height,
	       app.config.vo_rotation_degrees);
	printf("#VENC: H.264 %dx%d, %d kbps -> %s\n", app.config.venc_width,
	       app.config.venc_height, app.config.venc_bitrate_kbps,
	       app.config.h264_output_path);
	printf("#RTSP main: rtsp://0.0.0.0:554/live/0 (%dx%d, %d kbps)\n",
	       app.config.venc_width, app.config.venc_height, app.config.venc_bitrate_kbps);
	printf("#RTSP sub: rtsp://0.0.0.0:554/live/1 (%dx%d, %d kbps)\n",
	       app.config.sub_venc_width, app.config.sub_venc_height,
	       app.config.sub_venc_bitrate_kbps);
	/* An RTSP client may disconnect while a VENC worker is sending a frame. */
	signal(SIGPIPE, SIG_IGN);
	sigemptyset(&shutdown_signals);
	sigaddset(&shutdown_signals, SIGINT);
	sigaddset(&shutdown_signals, SIGTERM);
	/*
	 * Worker threads inherit this mask.  Keep Ctrl+C out of media ioctls and
	 * consume it synchronously here before starting the orderly shutdown.
	 */
	if (pthread_sigmask(SIG_BLOCK, &shutdown_signals, NULL) != 0) {
		fprintf(stderr, "failed to block shutdown signals\n");
		return 1;
	}
	if (ai_cam_start(&app) != RK_SUCCESS)
		return 1;

	while (!ai_cam_is_stopping(&app)) {
		int signal_number = sigtimedwait(&shutdown_signals, NULL, &wait_timeout);

		if (signal_number == SIGINT || signal_number == SIGTERM)
			ai_cam_request_stop(&app);
		else if (signal_number < 0 && errno != EAGAIN && errno != EINTR) {
			perror("sigtimedwait");
			ai_cam_request_stop(&app);
		}
	}
	ai_cam_stop(&app);
	return app.runtime_failed ? 1 : 0;
}
