#include <stdio.h>

/* ISP/RKAIQ 生命周期与 AE 控制模块：初始化传感器调优上下文，并响应 daemon
 *
 * 发来的自动曝光或“曝光行数 + 模拟增益”手动控制请求。 */
#include <stdlib.h>

#include "ai_cam.h"
#include "rk_aiq_user_api2_ae.h"
#include "rk_debug.h"

/* Calibrated against SC3336's active 25 FPS timing: 320 requested lines must
 * read back as 320 lines (not the 358 caused by the nominal pixel clock). */
#define SC3336_LINE_TIME_SECONDS (1.0f / 40800.0f)
#define SC3336_GAIN_UNIT 128.0f

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
    rk_aiq_uapi2_sysctl_preInit_devBufCnt(static_info.sensor_info.sensor_name, "rkraw_rx", 2);
    ret = rk_aiq_uapi2_sysctl_preInit_scene(static_info.sensor_info.sensor_name, "normal", "");
    if (ret != XCAM_RETURN_NO_ERROR) {
        RK_LOGE("rk_aiq_uapi2_sysctl_preInit_scene failed: %d", ret);
        return RK_FAILURE;
    }

    app->aiq_ctx =
        rk_aiq_uapi2_sysctl_init(static_info.sensor_info.sensor_name, app->config.iq_file_dir,
                                 ai_cam_isp_error_callback, ai_cam_isp_sof_callback);
    if (!app->aiq_ctx) {
        RK_LOGE("rk_aiq_uapi2_sysctl_init failed");
        return RK_FAILURE;
    }

    if (rk_aiq_uapi2_sysctl_prepare(app->aiq_ctx, 0, 0, RK_AIQ_WORKING_MODE_NORMAL) ||
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

int ai_cam_isp_set_auto_ae(AiCamApp *app) {
    Uapi_ExpSwAttrV2_t attr;
    if (!app || !app->aiq_ctx ||
        rk_aiq_user_api2_ae_getExpSwAttr(app->aiq_ctx, &attr) != XCAM_RETURN_NO_ERROR)
        return RK_FAILURE;
    attr.AecOpType = RK_AIQ_OP_MODE_AUTO;
    attr.stManual.LinearAE.ManualGainEn = false;
    attr.stManual.LinearAE.ManualTimeEn = false;
    return rk_aiq_user_api2_ae_setExpSwAttr(app->aiq_ctx, attr) == XCAM_RETURN_NO_ERROR
               ? RK_SUCCESS
               : RK_FAILURE;
}

int ai_cam_isp_set_manual_ae(AiCamApp *app, int exposure_lines, int analogue_gain) {
    Uapi_ExpSwAttrV2_t attr;
    if (!app || !app->aiq_ctx || exposure_lines < 1 || analogue_gain < 128 ||
        rk_aiq_user_api2_ae_getExpSwAttr(app->aiq_ctx, &attr) != XCAM_RETURN_NO_ERROR)
        return RK_FAILURE;
    attr.AecOpType = RK_AIQ_OP_MODE_MANUAL;
    attr.stManual.LinearAE.ManualGainEn = true;
    attr.stManual.LinearAE.ManualTimeEn = true;
    attr.stManual.LinearAE.ManualIspDgainEn = false;
    attr.stManual.LinearAE.GainValue = (float)analogue_gain / SC3336_GAIN_UNIT;
    attr.stManual.LinearAE.TimeValue = (float)exposure_lines * SC3336_LINE_TIME_SECONDS;
    return rk_aiq_user_api2_ae_setExpSwAttr(app->aiq_ctx, attr) == XCAM_RETURN_NO_ERROR
               ? RK_SUCCESS
               : RK_FAILURE;
}
