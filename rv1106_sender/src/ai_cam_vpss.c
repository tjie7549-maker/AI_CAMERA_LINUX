#include <string.h>

#include "ai_cam.h"
#include "rk_debug.h"
#include "rk_mpi_vpss.h"

int ai_cam_vpss_start(AiCamApp *app) {
	VPSS_GRP_ATTR_S grp_attr;
	VPSS_CHN_ATTR_S chn_attr;
	VPSS_CROP_INFO_S crop;
	int crop_size = app->config.vi_width < app->config.vi_height ?
	                app->config.vi_width : app->config.vi_height;
	int crop_x;
	int crop_y;
	bool display_enabled = false;
	bool encode_enabled = false;
	bool sub_encode_enabled = false;
	bool preview_enabled = false;
	bool face_enabled = false;
	int ret;

	crop_size &= ~1;
	crop_x = ((app->config.vi_width - crop_size) / 2) & ~1;
	crop_y = ((app->config.vi_height - crop_size) / 2) & ~1;
	if (crop_size < 64 || app->config.vo_width < 64 || app->config.vo_height < 64)
		return RK_FAILURE;

	memset(&grp_attr, 0, sizeof(grp_attr));
	grp_attr.u32MaxW = app->config.vi_width;
	grp_attr.u32MaxH = app->config.vi_height;
	grp_attr.enPixelFormat = RK_FMT_YUV420SP;
	grp_attr.enDynamicRange = DYNAMIC_RANGE_SDR8;
	grp_attr.stFrameRate.s32SrcFrameRate = -1;
	grp_attr.stFrameRate.s32DstFrameRate = -1;
	grp_attr.enCompressMode = COMPRESS_MODE_NONE;
	ret = RK_MPI_VPSS_CreateGrp(AI_CAM_VPSS_GRP, &grp_attr);
	if (ret != RK_SUCCESS)
		return ret;

	if (app->config.enable_vo) {
		memset(&crop, 0, sizeof(crop));
		crop.bEnable = RK_TRUE;
		crop.enCropCoordinate = VPSS_CROP_ABS_COOR;
		crop.stCropRect.s32X = crop_x;
		crop.stCropRect.s32Y = crop_y;
		crop.stCropRect.u32Width = crop_size;
		crop.stCropRect.u32Height = crop_size;
		ret = RK_MPI_VPSS_SetChnCrop(AI_CAM_VPSS_GRP, AI_CAM_VPSS_DISPLAY_CHN, &crop);
		if (ret != RK_SUCCESS)
			goto failed_destroy;

		memset(&chn_attr, 0, sizeof(chn_attr));
		chn_attr.enChnMode = VPSS_CHN_MODE_USER;
		chn_attr.enDynamicRange = DYNAMIC_RANGE_SDR8;
		chn_attr.enPixelFormat = RK_FMT_YUV420SP;
		chn_attr.stFrameRate.s32SrcFrameRate = -1;
		chn_attr.stFrameRate.s32DstFrameRate = -1;
		chn_attr.u32Width = app->config.vo_width;
		chn_attr.u32Height = app->config.vo_height;
		chn_attr.enCompressMode = COMPRESS_MODE_NONE;
		chn_attr.u32Depth = 2;
		chn_attr.u32FrameBufCnt = 3;
		ret = RK_MPI_VPSS_SetChnAttr(AI_CAM_VPSS_GRP, AI_CAM_VPSS_DISPLAY_CHN, &chn_attr);
		if (ret != RK_SUCCESS)
			goto failed_destroy;
		ret = RK_MPI_VPSS_EnableChn(AI_CAM_VPSS_GRP, AI_CAM_VPSS_DISPLAY_CHN);
		if (ret != RK_SUCCESS)
			goto failed_destroy;
		display_enabled = true;
	}

	/* 新增VPSS ch1编码分路：保留 16:9，输出 1280×720 H.264。 */
	memset(&chn_attr, 0, sizeof(chn_attr));
	chn_attr.enChnMode = VPSS_CHN_MODE_USER;
	chn_attr.enDynamicRange = DYNAMIC_RANGE_SDR8;
	chn_attr.enPixelFormat = RK_FMT_YUV420SP;
	chn_attr.stFrameRate.s32SrcFrameRate = -1;
	chn_attr.stFrameRate.s32DstFrameRate = -1;
	chn_attr.u32Width = app->config.venc_width;
	chn_attr.u32Height = app->config.venc_height;
	chn_attr.enCompressMode = COMPRESS_MODE_NONE;
	chn_attr.u32Depth = 2;
	chn_attr.u32FrameBufCnt = 3;
	ret = RK_MPI_VPSS_SetChnAttr(AI_CAM_VPSS_GRP, AI_CAM_VPSS_ENCODE_CHN, &chn_attr);
	if (ret != RK_SUCCESS)
		goto failed;

	memset(&crop, 0, sizeof(crop));
	crop.bEnable = RK_FALSE;
	ret = RK_MPI_VPSS_SetChnCrop(AI_CAM_VPSS_GRP, AI_CAM_VPSS_ENCODE_CHN, &crop);
	if (ret != RK_SUCCESS)
		goto failed;
	ret = RK_MPI_VPSS_EnableChn(AI_CAM_VPSS_GRP, AI_CAM_VPSS_ENCODE_CHN);
	if (ret != RK_SUCCESS)
		goto failed;
	encode_enabled = true;

	memset(&chn_attr, 0, sizeof(chn_attr));
	chn_attr.enChnMode = VPSS_CHN_MODE_USER;
	chn_attr.enDynamicRange = DYNAMIC_RANGE_SDR8;
	chn_attr.enPixelFormat = RK_FMT_YUV420SP;
	chn_attr.stFrameRate.s32SrcFrameRate = -1;
	chn_attr.stFrameRate.s32DstFrameRate = -1;
	chn_attr.u32Width = app->config.sub_venc_width;
	chn_attr.u32Height = app->config.sub_venc_height;
	chn_attr.enCompressMode = COMPRESS_MODE_NONE;
	chn_attr.u32Depth = 2;
	chn_attr.u32FrameBufCnt = 3;
	ret = RK_MPI_VPSS_SetChnAttr(AI_CAM_VPSS_GRP, AI_CAM_VPSS_SUB_ENCODE_CHN,
	                             &chn_attr);
	if (ret != RK_SUCCESS)
		goto failed;
	memset(&crop, 0, sizeof(crop));
	crop.bEnable = RK_FALSE;
	ret = RK_MPI_VPSS_SetChnCrop(AI_CAM_VPSS_GRP, AI_CAM_VPSS_SUB_ENCODE_CHN,
	                             &crop);
	if (ret != RK_SUCCESS)
		goto failed;
	ret = RK_MPI_VPSS_EnableChn(AI_CAM_VPSS_GRP, AI_CAM_VPSS_SUB_ENCODE_CHN);
	if (ret != RK_SUCCESS)
		goto failed;
	sub_encode_enabled = true;

	if (app->config.preview_shm_name) {
		memset(&chn_attr, 0, sizeof(chn_attr));
		chn_attr.enChnMode = VPSS_CHN_MODE_USER;
		chn_attr.enDynamicRange = DYNAMIC_RANGE_SDR8;
		chn_attr.enPixelFormat = RK_FMT_YUV420SP;
		chn_attr.stFrameRate.s32SrcFrameRate = -1;
		chn_attr.stFrameRate.s32DstFrameRate = app->config.preview_fps;
		chn_attr.u32Width = app->config.preview_width;
		chn_attr.u32Height = app->config.preview_height;
		chn_attr.enCompressMode = COMPRESS_MODE_NONE;
		chn_attr.u32Depth = 2;
		chn_attr.u32FrameBufCnt = 3;
		ret = RK_MPI_VPSS_SetChnAttr(AI_CAM_VPSS_GRP, AI_CAM_VPSS_PREVIEW_CHN,
		                             &chn_attr);
		if (ret != RK_SUCCESS)
			goto failed;
		memset(&crop, 0, sizeof(crop));
		crop.bEnable = RK_FALSE;
		ret = RK_MPI_VPSS_SetChnCrop(AI_CAM_VPSS_GRP, AI_CAM_VPSS_PREVIEW_CHN, &crop);
		if (ret != RK_SUCCESS)
			goto failed;
		ret = RK_MPI_VPSS_EnableChn(AI_CAM_VPSS_GRP, AI_CAM_VPSS_PREVIEW_CHN);
		if (ret != RK_SUCCESS)
			goto failed;
		preview_enabled = true;
	}
	if (app->config.face_snapshot_socket) {
		memset(&crop, 0, sizeof(crop));
		crop.bEnable = RK_TRUE;
		crop.enCropCoordinate = VPSS_CROP_ABS_COOR;
		crop.stCropRect.s32X = crop_x;
		crop.stCropRect.s32Y = crop_y;
		crop.stCropRect.u32Width = crop_size;
		crop.stCropRect.u32Height = crop_size;
		ret = RK_MPI_VPSS_SetChnCrop(AI_CAM_VPSS_GRP, AI_CAM_VPSS_FACE_CHN, &crop);
		if (ret != RK_SUCCESS)
			goto failed;
		memset(&chn_attr, 0, sizeof(chn_attr));
		chn_attr.enChnMode = VPSS_CHN_MODE_USER;
		chn_attr.enDynamicRange = DYNAMIC_RANGE_SDR8;
		chn_attr.enPixelFormat = RK_FMT_YUV420SP;
		chn_attr.stFrameRate.s32SrcFrameRate = -1;
		chn_attr.stFrameRate.s32DstFrameRate = 5;
		chn_attr.u32Width = app->config.face_width;
		chn_attr.u32Height = app->config.face_height;
		chn_attr.enCompressMode = COMPRESS_MODE_NONE;
		chn_attr.u32Depth = 1;
		chn_attr.u32FrameBufCnt = 2;
		ret = RK_MPI_VPSS_SetChnAttr(AI_CAM_VPSS_GRP, AI_CAM_VPSS_FACE_CHN, &chn_attr);
		if (ret != RK_SUCCESS)
			goto failed;
		ret = RK_MPI_VPSS_EnableChn(AI_CAM_VPSS_GRP, AI_CAM_VPSS_FACE_CHN);
		if (ret != RK_SUCCESS)
			goto failed;
		face_enabled = true;
	}
	ret = RK_MPI_VPSS_StartGrp(AI_CAM_VPSS_GRP);
	if (ret != RK_SUCCESS)
		goto failed;

	if (app->config.enable_vo)
		printf("#VPSS display: crop [%d %d %d %d] -> scale %dx%d\n", crop_x, crop_y,
		       crop_size, crop_size, app->config.vo_width, app->config.vo_height);
	printf("#VPSS encoder: scale %dx%d NV12\n", app->config.venc_width,
	       app->config.venc_height);
	printf("#VPSS sub encoder: scale %dx%d NV12\n", app->config.sub_venc_width,
	       app->config.sub_venc_height);
	if (app->config.preview_shm_name)
		printf("#VPSS preview: ch%d scale %dx%d NV12 at %d FPS\n",
		       AI_CAM_VPSS_PREVIEW_CHN, app->config.preview_width,
		       app->config.preview_height, app->config.preview_fps);
	if (app->config.face_snapshot_socket)
		printf("#VPSS face capture: ch%d center crop -> %dx%d NV12 on demand\n",
		       AI_CAM_VPSS_FACE_CHN, app->config.face_width, app->config.face_height);
	app->vpss_initialized = true;
	return RK_SUCCESS;

failed:
	if (face_enabled)
		RK_MPI_VPSS_DisableChn(AI_CAM_VPSS_GRP, AI_CAM_VPSS_FACE_CHN);
	if (preview_enabled)
		RK_MPI_VPSS_DisableChn(AI_CAM_VPSS_GRP, AI_CAM_VPSS_PREVIEW_CHN);
	if (sub_encode_enabled)
		RK_MPI_VPSS_DisableChn(AI_CAM_VPSS_GRP, AI_CAM_VPSS_SUB_ENCODE_CHN);
	if (encode_enabled)
		RK_MPI_VPSS_DisableChn(AI_CAM_VPSS_GRP, AI_CAM_VPSS_ENCODE_CHN);
	if (display_enabled)
		RK_MPI_VPSS_DisableChn(AI_CAM_VPSS_GRP, AI_CAM_VPSS_DISPLAY_CHN);
failed_destroy:
	RK_MPI_VPSS_DestroyGrp(AI_CAM_VPSS_GRP);
	return ret;
}

void ai_cam_vpss_stop(AiCamApp *app) {
	if (!app->vpss_initialized)
		return;

	RK_MPI_VPSS_StopGrp(AI_CAM_VPSS_GRP);
	RK_MPI_VPSS_DisableChn(AI_CAM_VPSS_GRP, AI_CAM_VPSS_SUB_ENCODE_CHN);
	RK_MPI_VPSS_DisableChn(AI_CAM_VPSS_GRP, AI_CAM_VPSS_ENCODE_CHN);
	if (app->config.preview_shm_name)
		RK_MPI_VPSS_DisableChn(AI_CAM_VPSS_GRP, AI_CAM_VPSS_PREVIEW_CHN);
	if (app->config.face_snapshot_socket)
		RK_MPI_VPSS_DisableChn(AI_CAM_VPSS_GRP, AI_CAM_VPSS_FACE_CHN);
	if (app->config.enable_vo)
		RK_MPI_VPSS_DisableChn(AI_CAM_VPSS_GRP, AI_CAM_VPSS_DISPLAY_CHN);
	RK_MPI_VPSS_DestroyGrp(AI_CAM_VPSS_GRP);
	app->vpss_initialized = false;
}
