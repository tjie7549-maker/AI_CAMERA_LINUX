#include <stdio.h>
#include <stdlib.h>

#include "ai_cam.h"
#include "rk_debug.h"

static XCamReturn ai_cam_isp_sof_callback(rk_aiq_metas_t *meta) {
	(void)meta;
	return XCAM_RETURN_NO_ERROR;
}

static XCamReturn ai_cam_isp_error_callback(rk_aiq_err_msg_t *msg) {
	(void)msg;
	return XCAM_RETURN_NO_ERROR;
}

int ai_cam_isp_start(AiCamApp *app) {
	rk_aiq_static_info_t static_info;
	XCamReturn ret;

	rk_aiq_uapi2_sysctl_enumStaticMetasByPhyId(0, &static_info);
	printf("#IQ File Dir: %s\n", app->config.iq_file_dir);
	printf("#ISP Sensor: %s\n", static_info.sensor_info.sensor_name);

	setenv("HDR_MODE", "0", 1);
	rk_aiq_uapi2_sysctl_preInit_devBufCnt(static_info.sensor_info.sensor_name,
	                                      "rkraw_rx", 2);
	ret = rk_aiq_uapi2_sysctl_preInit_scene(static_info.sensor_info.sensor_name,
	                                         "normal", "");
	if (ret != XCAM_RETURN_NO_ERROR) {
		RK_LOGE("rk_aiq_uapi2_sysctl_preInit_scene failed: %d", ret);
		return RK_FAILURE;
	}

	app->aiq_ctx = rk_aiq_uapi2_sysctl_init(static_info.sensor_info.sensor_name,
	                                        app->config.iq_file_dir,
	                                        ai_cam_isp_error_callback,
	                                        ai_cam_isp_sof_callback);
	if (!app->aiq_ctx) {
		RK_LOGE("rk_aiq_uapi2_sysctl_init failed");
		return RK_FAILURE;
	}

	if (rk_aiq_uapi2_sysctl_prepare(app->aiq_ctx, 0, 0,
	                                RK_AIQ_WORKING_MODE_NORMAL) ||
	    rk_aiq_uapi2_sysctl_start(app->aiq_ctx)) {
		RK_LOGE("RKAIQ prepare/start failed");
		rk_aiq_uapi2_sysctl_deinit(app->aiq_ctx);
		app->aiq_ctx = NULL;
		return RK_FAILURE;
	}

	return RK_SUCCESS;
}

void ai_cam_isp_stop(AiCamApp *app) {
	if (!app->aiq_ctx)
		return;

	rk_aiq_uapi2_sysctl_stop(app->aiq_ctx, false);
	rk_aiq_uapi2_sysctl_deinit(app->aiq_ctx);
	app->aiq_ctx = NULL;
}
