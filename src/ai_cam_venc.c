#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "ai_cam.h"
#include "rk_debug.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_vpss.h"

#define AI_CAM_WAIT_MS 200
#define AI_CAM_MAX_VENC_PACKS 8

static RK_U64 ai_cam_now_us(void) {
	struct timespec time = {0, 0};

	clock_gettime(CLOCK_MONOTONIC, &time);
	return (RK_U64)time.tv_sec * 1000000 + (RK_U64)time.tv_nsec / 1000;
}

static bool ai_cam_is_h264_start_code(const RK_U8 *data, RK_U32 length) {
	return length >= 4 && data[0] == 0 && data[1] == 0 &&
	       (data[2] == 1 || (data[2] == 0 && data[3] == 1));
}

static RK_U8 *ai_cam_venc_payload(VENC_PACK_S *pack) {
	RK_U8 *data = RK_MPI_MB_Handle2VirAddr(pack->pMbBlk);

	if (!data)
		return NULL;
	if (pack->u32Offset < pack->u32Len &&
	    ai_cam_is_h264_start_code(data + pack->u32Offset,
	                              pack->u32Len - pack->u32Offset))
		return data + pack->u32Offset;

	/* The RV1106 SDK may return an MB mapping already positioned at valid data. */
	return data;
}

static int ai_cam_venc_start_channel(VENC_CHN channel, int width, int height,
	                                 int fps, int bitrate_kbps) {
	VENC_CHN_ATTR_S attr;
	VENC_RECV_PIC_PARAM_S recv_param;
	int ret;

	memset(&attr, 0, sizeof(attr));
	attr.stVencAttr.enType = RK_VIDEO_ID_AVC;
	attr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
	attr.stVencAttr.u32Profile = H264E_PROFILE_HIGH;
	attr.stVencAttr.u32MaxPicWidth = width;
	attr.stVencAttr.u32MaxPicHeight = height;
	attr.stVencAttr.u32PicWidth = width;
	attr.stVencAttr.u32PicHeight = height;
	attr.stVencAttr.u32VirWidth = width;
	attr.stVencAttr.u32VirHeight = height;
	attr.stVencAttr.u32StreamBufCnt = 3;
	attr.stVencAttr.u32BufSize = width * height * 3 / 2;
	attr.stVencAttr.enMirror = MIRROR_NONE;
	attr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
	attr.stRcAttr.stH264Cbr.u32Gop = fps * 2;
	attr.stRcAttr.stH264Cbr.u32BitRate = bitrate_kbps;
	attr.stRcAttr.stH264Cbr.u32SrcFrameRateNum = fps;
	attr.stRcAttr.stH264Cbr.u32SrcFrameRateDen = 1;
	attr.stRcAttr.stH264Cbr.fr32DstFrameRateNum = fps;
	attr.stRcAttr.stH264Cbr.fr32DstFrameRateDen = 1;
	attr.stGopAttr.enGopMode = VENC_GOPMODE_NORMALP;
	ret = RK_MPI_VENC_CreateChn(channel, &attr);
	if (ret != RK_SUCCESS)
		return ret;

	memset(&recv_param, 0, sizeof(recv_param));
	recv_param.s32RecvPicNum = -1;
	ret = RK_MPI_VENC_StartRecvFrame(channel, &recv_param);
	if (ret != RK_SUCCESS)
		RK_MPI_VENC_DestroyChn(channel);
	return ret;
}

static void ai_cam_venc_stop_channel(VENC_CHN channel) {
	RK_MPI_VENC_StopRecvFrame(channel);
	RK_MPI_VENC_DestroyChn(channel);
}

int ai_cam_venc_start(AiCamApp *app) {
	int ret = ai_cam_venc_start_channel(AI_CAM_VENC_CHN, app->config.venc_width,
	                                    app->config.venc_height, app->config.venc_fps,
	                                    app->config.venc_bitrate_kbps);
	if (ret != RK_SUCCESS)
		return ret;
	app->venc_initialized = true;

	ret = ai_cam_venc_start_channel(AI_CAM_SUB_VENC_CHN, app->config.sub_venc_width,
	                                app->config.sub_venc_height, app->config.sub_venc_fps,
	                                app->config.sub_venc_bitrate_kbps);
	if (ret != RK_SUCCESS) {
		ai_cam_venc_stop_channel(AI_CAM_VENC_CHN);
		app->venc_initialized = false;
		return ret;
	}
	app->sub_venc_initialized = true;
	return RK_SUCCESS;
}

void ai_cam_venc_stop(AiCamApp *app) {
	if (app->sub_venc_initialized) {
		ai_cam_venc_stop_channel(AI_CAM_SUB_VENC_CHN);
		app->sub_venc_initialized = false;
	}
	if (app->venc_initialized) {
		ai_cam_venc_stop_channel(AI_CAM_VENC_CHN);
		app->venc_initialized = false;
	}
}

static int ai_cam_venc_bind_channel(VPSS_CHN vpss_channel, VENC_CHN venc_channel) {
	MPP_CHN_S src = {RK_ID_VPSS, AI_CAM_VPSS_GRP, vpss_channel};
	MPP_CHN_S dst = {RK_ID_VENC, 0, venc_channel};

	return RK_MPI_SYS_Bind(&src, &dst);
}

static void ai_cam_venc_unbind_channel(VPSS_CHN vpss_channel, VENC_CHN venc_channel) {
	MPP_CHN_S src = {RK_ID_VPSS, AI_CAM_VPSS_GRP, vpss_channel};
	MPP_CHN_S dst = {RK_ID_VENC, 0, venc_channel};

	RK_MPI_SYS_UnBind(&src, &dst);
}

int ai_cam_venc_bind_vpss(AiCamApp *app) {
	int ret = ai_cam_venc_bind_channel(AI_CAM_VPSS_ENCODE_CHN, AI_CAM_VENC_CHN);
	if (ret != RK_SUCCESS)
		return ret;
	app->vpss_venc_bound = true;

	ret = ai_cam_venc_bind_channel(AI_CAM_VPSS_SUB_ENCODE_CHN, AI_CAM_SUB_VENC_CHN);
	if (ret != RK_SUCCESS) {
		ai_cam_venc_unbind_channel(AI_CAM_VPSS_ENCODE_CHN, AI_CAM_VENC_CHN);
		app->vpss_venc_bound = false;
		return ret;
	}
	app->vpss_sub_venc_bound = true;
	return RK_SUCCESS;
}

void ai_cam_venc_unbind_vpss(AiCamApp *app) {
	if (app->vpss_sub_venc_bound) {
		ai_cam_venc_unbind_channel(AI_CAM_VPSS_SUB_ENCODE_CHN, AI_CAM_SUB_VENC_CHN);
		app->vpss_sub_venc_bound = false;
	}
	if (app->vpss_venc_bound) {
		ai_cam_venc_unbind_channel(AI_CAM_VPSS_ENCODE_CHN, AI_CAM_VENC_CHN);
		app->vpss_venc_bound = false;
	}
}

static void *ai_cam_write_venc_stream(AiCamApp *app, VENC_CHN venc_channel,
	                                   rtsp_session_handle rtsp_session,
	                                   const char *output_path, bool stop_at_limit) {
	VENC_STREAM_S stream;
	VENC_PACK_S packs[AI_CAM_MAX_VENC_PACKS];
	FILE *file = NULL;
	RK_S32 frame_count = 0;
	RK_U64 stat_start_us = 0;
	RK_U64 latency_sum_us = 0;
	RK_U64 latency_max_us = 0;
	RK_U32 latency_count = 0;

	if (output_path) {
		file = fopen(output_path, "wb");
		if (!file) {
			RK_LOGE("open %s failed: %s", output_path, strerror(errno));
			ai_cam_request_stop(app);
			return NULL;
		}
	}

	while (!ai_cam_is_stopping(app)) {
		int ret;
		RK_U32 index;
		RK_U64 stream_pts = 0;

		memset(&stream, 0, sizeof(stream));
		memset(packs, 0, sizeof(packs));
		stream.pstPack = packs;
		ret = RK_MPI_VENC_GetStream(venc_channel, &stream, AI_CAM_WAIT_MS);
		if (ret != RK_SUCCESS)
			continue;

		for (index = 0; index < stream.u32PackCount && index < AI_CAM_MAX_VENC_PACKS;
		     ++index) {
			VENC_PACK_S *pack = &stream.pstPack[index];
			RK_U8 *data = ai_cam_venc_payload(pack);

			if (!data || !pack->u32Len)
				continue;
			if (!stream_pts && pack->u64PTS)
				stream_pts = pack->u64PTS;
			if (file && fwrite(data, 1, pack->u32Len, file) != pack->u32Len) {
				RK_LOGE("write %s failed: %s", output_path, strerror(errno));
				ai_cam_request_stop(app);
			}
			if (app->rtsp_initialized && rtsp_session) {
				pthread_mutex_lock(&app->rtsp_mutex);
				rtsp_tx_video(rtsp_session, data, pack->u32Len, pack->u64PTS);
				pthread_mutex_unlock(&app->rtsp_mutex);
			}
		}
		if (app->rtsp_initialized && rtsp_session) {
			pthread_mutex_lock(&app->rtsp_mutex);
			rtsp_do_event(app->rtsp_demo);
			pthread_mutex_unlock(&app->rtsp_mutex);
		}
		if (stream_pts) {
			RK_U64 now_us = ai_cam_now_us();
			if (!stat_start_us)
				stat_start_us = now_us;
			if (now_us >= stream_pts) {
				RK_U64 latency_us = now_us - stream_pts;
				latency_sum_us += latency_us;
				if (latency_us > latency_max_us)
					latency_max_us = latency_us;
				latency_count++;
			}
			if (now_us - stat_start_us >= 1000000) {
				printf("#Stats: RTSP %s FPS=%.2f, capture-to-RTSP-submit avg=%.2f ms, max=%.2f ms\n",
				       output_path ? "/live/0" : "/live/1",
				       frame_count / ((double)(now_us - stat_start_us) / 1000000.0),
				       latency_count ? (double)latency_sum_us / latency_count / 1000.0 : 0.0,
				       (double)latency_max_us / 1000.0);
				stat_start_us = now_us;
				latency_sum_us = 0;
				latency_max_us = 0;
				latency_count = 0;
				frame_count = 0;
			}
		}
		ret = RK_MPI_VENC_ReleaseStream(venc_channel, &stream);
		if (ret != RK_SUCCESS)
			RK_LOGE("RK_MPI_VENC_ReleaseStream(%d) failed, ret = %x", venc_channel, ret);

		frame_count++;
		if (stop_at_limit && app->config.venc_frame_limit >= 0 &&
		    frame_count >= app->config.venc_frame_limit)
			ai_cam_request_stop(app);
	}

	if (file) {
		fflush(file);
		fclose(file);
	}
	return NULL;
}

void *ai_cam_venc_write_stream(void *arg) {
	AiCamApp *app = arg;

	return ai_cam_write_venc_stream(app, AI_CAM_VENC_CHN, app->rtsp_main_session,
	                                app->config.h264_output_path, true);
}

void *ai_cam_sub_venc_write_stream(void *arg) {
	AiCamApp *app = arg;

	return ai_cam_write_venc_stream(app, AI_CAM_SUB_VENC_CHN, app->rtsp_sub_session,
	                                NULL, false);
}
