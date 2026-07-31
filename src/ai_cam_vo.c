#include <string.h>

#include "ai_cam.h"
#include "rk_debug.h"
#include "rk_mpi_vo.h"

static int ai_cam_rotation(int degrees, ROTATION_E *rotation) {
	switch (degrees) {
	case 0:
		*rotation = ROTATION_0;
		return RK_SUCCESS;
	case 90:
		*rotation = ROTATION_90;
		return RK_SUCCESS;
	case 180:
		*rotation = ROTATION_180;
		return RK_SUCCESS;
	case 270:
		*rotation = ROTATION_270;
		return RK_SUCCESS;
	default:
		return RK_FAILURE;
	}
}

int ai_cam_vo_start(AiCamApp *app) {
	VO_PUB_ATTR_S pub_attr;
	VO_VIDEO_LAYER_ATTR_S layer_attr;
	VO_CHN_ATTR_S chn_attr;
	RK_U32 disp_buf_len = 3;
	ROTATION_E rotation;
	bool layer_bound = false;
	bool dev_enabled = false;
	bool layer_enabled = false;
	int ret;

	ret = ai_cam_rotation(app->config.vo_rotation_degrees, &rotation);
	if (ret != RK_SUCCESS)
		return ret;
	ret = RK_MPI_VO_BindLayer(app->config.vo_layer, app->config.vo_device,
	                           VO_LAYER_MODE_GRAPHIC);
	if (ret != RK_SUCCESS)
		return ret;
	layer_bound = true;

	memset(&pub_attr, 0, sizeof(pub_attr));
	memset(&layer_attr, 0, sizeof(layer_attr));
	memset(&chn_attr, 0, sizeof(chn_attr));
	pub_attr.enIntfType = VO_INTF_DEFAULT;
	pub_attr.enIntfSync = VO_OUTPUT_DEFAULT;
	ret = RK_MPI_VO_SetPubAttr(app->config.vo_device, &pub_attr);
	if (ret != RK_SUCCESS)
		goto failed;
	ret = RK_MPI_VO_Enable(app->config.vo_device);
	if (ret != RK_SUCCESS)
		goto failed;
	dev_enabled = true;
	ret = RK_MPI_VO_SetLayerDispBufLen(app->config.vo_layer, disp_buf_len);
	if (ret != RK_SUCCESS)
		goto failed;

	layer_attr.enPixFormat = RK_FMT_RGB888;
	layer_attr.enCompressMode = COMPRESS_AFBC_16x16;
	layer_attr.stDispRect.u32Width = app->config.vo_width;
	layer_attr.stDispRect.u32Height = app->config.vo_height;
	layer_attr.stImageSize.u32Width = app->config.vo_width;
	layer_attr.stImageSize.u32Height = app->config.vo_height;
	layer_attr.u32DispFrmRt = 25;
	ret = RK_MPI_VO_SetLayerAttr(app->config.vo_layer, &layer_attr);
	if (ret != RK_SUCCESS)
		goto failed;
	RK_MPI_VO_SetLayerSpliceMode(app->config.vo_layer, VO_SPLICE_MODE_RGA);
	ret = RK_MPI_VO_EnableLayer(app->config.vo_layer);
	if (ret != RK_SUCCESS)
		goto failed;
	layer_enabled = true;

	chn_attr.stRect.u32Width = app->config.vo_width;
	chn_attr.stRect.u32Height = app->config.vo_height;
	chn_attr.u32FgAlpha = 255;
	chn_attr.enMirror = MIRROR_NONE;
	chn_attr.enRotation = rotation;
	chn_attr.u32Priority = 1;
	ret = RK_MPI_VO_SetChnAttr(app->config.vo_layer, app->config.vo_channel, &chn_attr);
	if (ret != RK_SUCCESS)
		goto failed;
	ret = RK_MPI_VO_EnableChn(app->config.vo_layer, app->config.vo_channel);
	if (ret != RK_SUCCESS)
		goto failed;
	app->vo_initialized = true;
	return RK_SUCCESS;

failed:
	if (layer_enabled)
		RK_MPI_VO_DisableLayer(app->config.vo_layer);
	if (dev_enabled)
		RK_MPI_VO_Disable(app->config.vo_device);
	if (layer_bound)
		RK_MPI_VO_UnBindLayer(app->config.vo_layer, app->config.vo_device);
	RK_MPI_VO_CloseFd();
	return ret;
}

void ai_cam_vo_stop(AiCamApp *app) {
	if (!app->vo_initialized)
		return;
	RK_MPI_VO_DisableChn(app->config.vo_layer, app->config.vo_channel);
	RK_MPI_VO_DisableLayer(app->config.vo_layer);
	RK_MPI_VO_Disable(app->config.vo_device);
	RK_MPI_VO_UnBindLayer(app->config.vo_layer, app->config.vo_device);
	RK_MPI_VO_CloseFd();
	app->vo_initialized = false;
}
