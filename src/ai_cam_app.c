#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ai_cam.h"
#include "rk_debug.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_vi.h"
#include "rk_mpi_vo.h"
#include "rk_mpi_vpss.h"

#define AI_CAM_WAIT_MS 200
#define AI_CAM_VI_NO_FRAME_TIMEOUT_US 10000000

static RK_U64 ai_cam_now_us(void) {
	struct timespec time = {0, 0};

	clock_gettime(CLOCK_MONOTONIC, &time);
	return (RK_U64)time.tv_sec * 1000000 + (RK_U64)time.tv_nsec / 1000;
}

static void *ai_cam_forward_frames(void *arg) {
	AiCamApp *app = arg;
	VIDEO_FRAME_INFO_S vi_frame;
	VIDEO_FRAME_INFO_S display_frame;
	RK_U64 stat_start_us = 0;
	RK_U64 latency_sum_us = 0;
	RK_U64 latency_max_us = 0;
	RK_U64 last_vi_frame_us = ai_cam_now_us();
	RK_U32 frame_count = 0;
	RK_U32 latency_count = 0;

	while (!ai_cam_is_stopping(app)) {
		int ret = RK_MPI_VI_GetChnFrame(AI_CAM_VI_PIPE, app->config.vi_channel,
		                                 &vi_frame, AI_CAM_WAIT_MS);
		if (ret != RK_SUCCESS) {
			if (ai_cam_now_us() - last_vi_frame_us >= AI_CAM_VI_NO_FRAME_TIMEOUT_US) {
				RK_LOGE("VI has not produced a frame for 10 seconds");
				app->runtime_failed = 1;
				ai_cam_request_stop(app);
				break;
			}
			continue;
		}
		last_vi_frame_us = ai_cam_now_us();
		ret = RK_MPI_VPSS_SendFrame(AI_CAM_VPSS_GRP, 0, &vi_frame, AI_CAM_WAIT_MS);
		RK_MPI_VI_ReleaseChnFrame(AI_CAM_VI_PIPE, app->config.vi_channel, &vi_frame);
		if (ret != RK_SUCCESS)
			continue;

		ret = RK_MPI_VPSS_GetChnFrame(AI_CAM_VPSS_GRP, AI_CAM_VPSS_DISPLAY_CHN,
		                              &display_frame, AI_CAM_WAIT_MS);
		if (ret != RK_SUCCESS)
			continue;

		if (app->vo_initialized) {
			ret = RK_MPI_VO_SendFrame(app->config.vo_layer, app->config.vo_channel,
			                          &display_frame, AI_CAM_WAIT_MS);
			if (ret != RK_SUCCESS && !ai_cam_is_stopping(app))
				RK_LOGE("RK_MPI_VO_SendFrame failed, ret = %x", ret);
			if (ret == RK_SUCCESS) {
				RK_U64 now_us = ai_cam_now_us();
				if (!stat_start_us)
					stat_start_us = now_us;
				frame_count++;
				if (display_frame.stVFrame.u64PTS > 0 &&
				    now_us >= display_frame.stVFrame.u64PTS) {
					RK_U64 latency_us = now_us - display_frame.stVFrame.u64PTS;
					latency_sum_us += latency_us;
					if (latency_us > latency_max_us)
						latency_max_us = latency_us;
					latency_count++;
				}
				if (now_us - stat_start_us >= 1000000) {
					printf("#Stats: LCD submit FPS=%.2f, capture-to-VO-submit avg=%.2f ms, max=%.2f ms\n",
					       frame_count / ((double)(now_us - stat_start_us) / 1000000.0),
					       latency_count ? (double)latency_sum_us / latency_count / 1000.0 : 0.0,
					       (double)latency_max_us / 1000.0);
					stat_start_us = now_us;
					latency_sum_us = 0;
					latency_max_us = 0;
					frame_count = 0;
					latency_count = 0;
				}
			}
		}
		RK_MPI_VPSS_ReleaseChnFrame(AI_CAM_VPSS_GRP, AI_CAM_VPSS_DISPLAY_CHN,
		                            &display_frame);
	}
	return NULL;
}

static void *ai_cam_start_vo(void *arg) {
	AiCamApp *app = arg;
	int ret = ai_cam_vo_start(app);

	if (ret != RK_SUCCESS && !ai_cam_is_stopping(app)) {
		RK_LOGE("VO initialization failed, ret = %x", ret);
		ai_cam_request_stop(app);
	}
	return NULL;
}

void ai_cam_default_config(AiCamConfig *config) {
	memset(config, 0, sizeof(*config));
	config->vi_width = 2304;
	config->vi_height = 1296;
	config->vo_width = 720;
	config->vo_height = 720;
	config->vo_rotation_degrees = 180;
	config->iq_file_dir = "/oem/usr/share/iqfiles";
	config->h264_output_path = "test.h264";
	config->venc_width = 1280;
	config->venc_height = 720;
	config->source_fps = 30;
	config->venc_fps = 25;
	config->venc_bitrate_kbps = 2048;
	config->sub_venc_width = 640;
	config->sub_venc_height = 360;
	config->sub_venc_fps = 20;
	config->sub_venc_bitrate_kbps = 1024;
	config->venc_frame_limit = -1;
}

void ai_cam_request_stop(AiCamApp *app) {
	app->stop_requested = 1;
}

bool ai_cam_is_stopping(const AiCamApp *app) {
	return app->stop_requested != 0;
}

int ai_cam_start(AiCamApp *app) {
	int ret;

	ret = ai_cam_isp_start(app);
	if (ret != RK_SUCCESS)
		goto failed;
	app->isp_initialized = true;
	ret = RK_MPI_SYS_Init();
	if (ret != RK_SUCCESS)
		goto failed;
	app->sys_initialized = true;
	ret = ai_cam_vi_start(app);
	if (ret != RK_SUCCESS)
		goto failed;
	ret = ai_cam_vpss_start(app);
	if (ret != RK_SUCCESS)
		goto failed;
	ret = ai_cam_venc_start(app);
	if (ret != RK_SUCCESS)
		goto failed;
	ret = ai_cam_venc_bind_vpss(app);
	if (ret != RK_SUCCESS)
		goto failed;
	ret = ai_cam_rtsp_start(app);
	if (ret != RK_SUCCESS)
		goto failed;
	if (pthread_create(&app->venc_thread, NULL, ai_cam_venc_write_stream, app) != 0) {
		ret = RK_FAILURE;
		goto failed;
	}
	app->venc_thread_started = true;
	if (pthread_create(&app->sub_venc_thread, NULL, ai_cam_sub_venc_write_stream, app) != 0) {
		ret = RK_FAILURE;
		goto failed;
	}
	app->sub_venc_thread_started = true;
	if (pthread_create(&app->forwarding_thread, NULL, ai_cam_forward_frames, app) != 0) {
		ret = RK_FAILURE;
		goto failed;
	}
	app->forwarding_thread_started = true;
	if (pthread_create(&app->vo_thread, NULL, ai_cam_start_vo, app) != 0) {
		ret = RK_FAILURE;
		goto failed;
	}
	app->vo_thread_started = true;
	return RK_SUCCESS;

failed:
	RK_LOGE("media pipeline start failed, ret = %x", ret);
	ai_cam_stop(app);
	return ret;
}

void ai_cam_stop(AiCamApp *app) {
	ai_cam_request_stop(app);
	if (app->vo_thread_started) {
		if (!app->vo_initialized)
			pthread_kill(app->vo_thread, SIGINT);
		pthread_join(app->vo_thread, NULL);
		app->vo_thread_started = false;
	}
	if (app->forwarding_thread_started) {
		pthread_join(app->forwarding_thread, NULL);
		app->forwarding_thread_started = false;
	}
	if (app->venc_thread_started) {
		pthread_join(app->venc_thread, NULL);
		app->venc_thread_started = false;
	}
	if (app->sub_venc_thread_started) {
		pthread_join(app->sub_venc_thread, NULL);
		app->sub_venc_thread_started = false;
	}
	ai_cam_rtsp_stop(app);
	ai_cam_venc_unbind_vpss(app);
	ai_cam_venc_stop(app);
	ai_cam_vo_stop(app);
	ai_cam_vpss_stop(app);
	ai_cam_vi_stop(app);
	if (app->sys_initialized) {
		RK_MPI_SYS_Exit();
		app->sys_initialized = false;
	}
	if (app->isp_initialized) {
		ai_cam_isp_stop(app);
		app->isp_initialized = false;
	}
}
