#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "ai_cam.h"
#include "rk_debug.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"

#define AI_CAM_WAIT_MS 200

int ai_cam_venc_start(AiCamApp *app) {
	VENC_CHN_ATTR_S attr;
	VENC_RECV_PIC_PARAM_S recv_param;
	int ret;

	memset(&attr, 0, sizeof(attr));
	attr.stVencAttr.enType = RK_VIDEO_ID_AVC;
	attr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
	attr.stVencAttr.u32Profile = H264E_PROFILE_HIGH;
	attr.stVencAttr.u32MaxPicWidth = app->config.venc_width;
	attr.stVencAttr.u32MaxPicHeight = app->config.venc_height;
	attr.stVencAttr.u32PicWidth = app->config.venc_width;
	attr.stVencAttr.u32PicHeight = app->config.venc_height;
	attr.stVencAttr.u32VirWidth = app->config.venc_width;
	attr.stVencAttr.u32VirHeight = app->config.venc_height;
	attr.stVencAttr.u32StreamBufCnt = 3;
	attr.stVencAttr.u32BufSize = app->config.venc_width * app->config.venc_height * 3 / 2;
	attr.stVencAttr.enMirror = MIRROR_NONE;
	attr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
	attr.stRcAttr.stH264Cbr.u32Gop = app->config.venc_fps * 2;
	attr.stRcAttr.stH264Cbr.u32BitRate = app->config.venc_bitrate_kbps;
	attr.stRcAttr.stH264Cbr.u32SrcFrameRateNum = app->config.venc_fps;
	attr.stRcAttr.stH264Cbr.u32SrcFrameRateDen = 1;
	attr.stRcAttr.stH264Cbr.fr32DstFrameRateNum = app->config.venc_fps;
	attr.stRcAttr.stH264Cbr.fr32DstFrameRateDen = 1;
	attr.stGopAttr.enGopMode = VENC_GOPMODE_NORMALP;
	ret = RK_MPI_VENC_CreateChn(AI_CAM_VENC_CHN, &attr);
	if (ret != RK_SUCCESS)
		return ret;

	memset(&recv_param, 0, sizeof(recv_param));
	recv_param.s32RecvPicNum = -1;
	ret = RK_MPI_VENC_StartRecvFrame(AI_CAM_VENC_CHN, &recv_param);
	if (ret != RK_SUCCESS) {
		RK_MPI_VENC_DestroyChn(AI_CAM_VENC_CHN);
		return ret;
	}
	app->venc_initialized = true;
	return RK_SUCCESS;
}

void ai_cam_venc_stop(AiCamApp *app) {
	if (!app->venc_initialized)
		return;
	RK_MPI_VENC_StopRecvFrame(AI_CAM_VENC_CHN);
	RK_MPI_VENC_DestroyChn(AI_CAM_VENC_CHN);
	app->venc_initialized = false;
}

int ai_cam_venc_bind_vpss(AiCamApp *app) {
	MPP_CHN_S src = {RK_ID_VPSS, AI_CAM_VPSS_GRP, AI_CAM_VPSS_ENCODE_CHN};
	MPP_CHN_S dst = {RK_ID_VENC, 0, AI_CAM_VENC_CHN};
	int ret = RK_MPI_SYS_Bind(&src, &dst);

	if (ret == RK_SUCCESS)
		app->vpss_venc_bound = true;
	return ret;
}

void ai_cam_venc_unbind_vpss(AiCamApp *app) {
	MPP_CHN_S src = {RK_ID_VPSS, AI_CAM_VPSS_GRP, AI_CAM_VPSS_ENCODE_CHN};
	MPP_CHN_S dst = {RK_ID_VENC, 0, AI_CAM_VENC_CHN};

	if (!app->vpss_venc_bound)
		return;
	RK_MPI_SYS_UnBind(&src, &dst);
	app->vpss_venc_bound = false;
}

void *ai_cam_venc_write_stream(void *arg) {
	AiCamApp *app = arg;
	VENC_STREAM_S stream;
	VENC_PACK_S pack;
	FILE *file = fopen(app->config.h264_output_path, "wb");
	RK_S32 frame_count = 0;

	if (!file) {
		RK_LOGE("open %s failed: %s", app->config.h264_output_path, strerror(errno));
		ai_cam_request_stop(app);
		return NULL;
	}
	while (!ai_cam_is_stopping(app)) {
		void *data;
		int ret;

		memset(&stream, 0, sizeof(stream));
		memset(&pack, 0, sizeof(pack));
		stream.pstPack = &pack;
		ret = RK_MPI_VENC_GetStream(AI_CAM_VENC_CHN, &stream, AI_CAM_WAIT_MS);
		if (ret != RK_SUCCESS)
			continue;
		data = RK_MPI_MB_Handle2VirAddr(stream.pstPack->pMbBlk);
		if (data && stream.pstPack->u32Len > 0) {
			if (fwrite(data, 1, stream.pstPack->u32Len, file) != stream.pstPack->u32Len)
				ai_cam_request_stop(app);
			frame_count++;
		}
		RK_MPI_VENC_ReleaseStream(AI_CAM_VENC_CHN, &stream);
		if (app->config.venc_frame_limit >= 0 &&
		    frame_count >= app->config.venc_frame_limit)
			ai_cam_request_stop(app);
	}
	fflush(file);
	fclose(file);
	return NULL;
}
