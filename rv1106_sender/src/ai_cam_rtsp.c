#include "ai_cam.h"
#include "rk_debug.h"

int ai_cam_rtsp_start(AiCamApp *app) {
	if (pthread_mutex_init(&app->rtsp_mutex, NULL) != 0) {
		RK_LOGE("RTSP mutex initialization failed");
		return RK_FAILURE;
	}
	app->rtsp_mutex_initialized = true;

	app->rtsp_demo = create_rtsp_demo(554);
	if (!app->rtsp_demo) {
		RK_LOGE("create_rtsp_demo failed");
		return RK_FAILURE;
	}
	app->rtsp_main_session = rtsp_new_session(app->rtsp_demo, "/live/0");
	if (!app->rtsp_main_session)
		goto failed;
	app->rtsp_sub_session = rtsp_new_session(app->rtsp_demo, "/live/1");
	if (!app->rtsp_sub_session)
		goto failed;
	if (rtsp_set_video(app->rtsp_main_session, RTSP_CODEC_ID_VIDEO_H264, NULL, 0) ||
	    rtsp_set_video(app->rtsp_sub_session, RTSP_CODEC_ID_VIDEO_H264, NULL, 0))
		goto failed;
	rtsp_sync_video_ts(app->rtsp_main_session, rtsp_get_reltime(), rtsp_get_ntptime());
	rtsp_sync_video_ts(app->rtsp_sub_session, rtsp_get_reltime(), rtsp_get_ntptime());
	app->rtsp_initialized = true;
	return RK_SUCCESS;

failed:
	RK_LOGE("RTSP session initialization failed");
	ai_cam_rtsp_stop(app);
	return RK_FAILURE;
}

void ai_cam_rtsp_stop(AiCamApp *app) {
	if (app->rtsp_demo)
		rtsp_del_demo(app->rtsp_demo);
	app->rtsp_demo = NULL;
	app->rtsp_main_session = NULL;
	app->rtsp_sub_session = NULL;
	app->rtsp_initialized = false;
	if (app->rtsp_mutex_initialized) {
		pthread_mutex_destroy(&app->rtsp_mutex);
		app->rtsp_mutex_initialized = false;
	}
}
