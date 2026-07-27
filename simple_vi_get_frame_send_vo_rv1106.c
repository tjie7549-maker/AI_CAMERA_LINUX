
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <time.h>
#include <unistd.h>

#include <rk_aiq_user_api2_sysctl.h>

#include "rk_debug.h"
#include "rk_defines.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_vi.h"
#include "rk_mpi_vo.h"

static bool quit = false;
static	int ViWidth = 2304;
static	int ViHeight = 1296;
static	int VoWidth = 720;
static	int VoHeight = 720;
static	int s32chnlId = 0;
static	int VoLayer = 0;
static	int VoDev = 0;
static	int VoChn = 0;
static	int VoRotationDegrees = 180;
static	ROTATION_E VoRotation = ROTATION_180;
static const char *IqFileDir = "/oem/usr/share/iqfiles";
static rk_aiq_sys_ctx_t *g_aiq_ctx;

static RK_U64 get_monotonic_time_us(void) {
	struct timespec time = {0, 0};

	clock_gettime(CLOCK_MONOTONIC, &time);
	return (RK_U64)time.tv_sec * 1000000 + (RK_U64)time.tv_nsec / 1000;
}

static void sigterm_handler(int sig) {
	fprintf(stderr, "signal %d\n", sig);
	quit = true;
}

static XCamReturn isp_sof_callback(rk_aiq_metas_t *meta) {
	return XCAM_RETURN_NO_ERROR;
}

static XCamReturn isp_error_callback(rk_aiq_err_msg_t *msg) {
	if (msg->err_code == XCAM_RETURN_BYPASS)
		quit = true;
	return XCAM_RETURN_NO_ERROR;
}

static int isp_init(int cam_id, const char *iq_file_dir) {
	rk_aiq_static_info_t static_info;

	rk_aiq_uapi2_sysctl_enumStaticMetasByPhyId(cam_id, &static_info);
	printf("#IQ File Dir: %s\n", iq_file_dir);
	printf("#ISP Sensor: %s\n", static_info.sensor_info.sensor_name);

	g_aiq_ctx = rk_aiq_uapi2_sysctl_init(static_info.sensor_info.sensor_name,
	                                     iq_file_dir, isp_error_callback,
	                                     isp_sof_callback);
	if (!g_aiq_ctx) {
		RK_LOGE("rk_aiq_uapi2_sysctl_init failed");
		return RK_FAILURE;
	}

	if (rk_aiq_uapi2_sysctl_prepare(g_aiq_ctx, 0, 0,
	                                RK_AIQ_WORKING_MODE_NORMAL)) {
		RK_LOGE("rk_aiq_uapi2_sysctl_prepare failed");
		rk_aiq_uapi2_sysctl_deinit(g_aiq_ctx);
		g_aiq_ctx = NULL;
		return RK_FAILURE;
	}

	if (rk_aiq_uapi2_sysctl_start(g_aiq_ctx)) {
		RK_LOGE("rk_aiq_uapi2_sysctl_start failed");
		rk_aiq_uapi2_sysctl_deinit(g_aiq_ctx);
		g_aiq_ctx = NULL;
		return RK_FAILURE;
	}

	return RK_SUCCESS;
}

static void isp_deinit(void) {
	if (!g_aiq_ctx)
		return;

	rk_aiq_uapi2_sysctl_stop(g_aiq_ctx, false);
	rk_aiq_uapi2_sysctl_deinit(g_aiq_ctx);
	g_aiq_ctx = NULL;
}

static void *GetMediaBuffer0(void *arg) {
	printf("========%s========\n", __func__);
	int s32Ret;
	RK_S32 waitTime = 1000;
	int pipeId = 0;
	VIDEO_FRAME_INFO_S stViFrame;
	RK_U64 stat_start_us = 0;
	RK_U64 latency_sum_us = 0;
	RK_U64 latency_max_us = 0;
	RK_U32 frame_count = 0;
	RK_U32 latency_count = 0;

	while (!quit) {
		s32Ret = RK_MPI_VI_GetChnFrame(pipeId, s32chnlId, &stViFrame, waitTime);
		if (s32Ret == RK_SUCCESS) {
			RK_U64 now_us = get_monotonic_time_us();

			if (stat_start_us == 0)
				stat_start_us = now_us;
			frame_count++;
			if (stViFrame.stVFrame.u64PTS > 0 && now_us >= stViFrame.stVFrame.u64PTS) {
				RK_U64 latency_us = now_us - stViFrame.stVFrame.u64PTS;
				latency_sum_us += latency_us;
				if (latency_us > latency_max_us)
					latency_max_us = latency_us;
				latency_count++;
			}

			if (now_us - stat_start_us >= 1000000) {
				double elapsed_s = (double)(now_us - stat_start_us) / 1000000.0;
				double fps = frame_count / elapsed_s;
				if (latency_count > 0) {
					printf("#Stats: FPS=%.2f, VI latency avg=%.2f ms, max=%.2f ms\n",
					       fps, (double)latency_sum_us / latency_count / 1000.0,
					       (double)latency_max_us / 1000.0);
				} else {
					printf("#Stats: FPS=%.2f, VI latency unavailable (PTS is 0)\n", fps);
				}
				stat_start_us = now_us;
				latency_sum_us = 0;
				latency_max_us = 0;
				frame_count = 0;
				latency_count = 0;
			}

			RK_MPI_VO_SendFrame(VoLayer, VoChn, &stViFrame, -1);

			// 7.release the frame
			s32Ret = RK_MPI_VI_ReleaseChnFrame(pipeId, s32chnlId, &stViFrame);
			if (s32Ret != RK_SUCCESS) {
				RK_LOGE("RK_MPI_VI_ReleaseChnFrame fail %x", s32Ret);
			}
		} else {
			RK_LOGE("RK_MPI_VI_GetChnFrame timeout %x", s32Ret);
		}
	}

	return NULL;
}

// demo板dev默认都是0，根据不同的channel 来选择不同的vi节点
int vi_dev_init() {
	printf("%s\n", __func__);
	int ret = 0;
	int devId = 0;
	int pipeId = devId;

	VI_DEV_ATTR_S stDevAttr;
	VI_DEV_BIND_PIPE_S stBindPipe;
	memset(&stDevAttr, 0, sizeof(stDevAttr));
	memset(&stBindPipe, 0, sizeof(stBindPipe));
	// 0. get dev config status
	ret = RK_MPI_VI_GetDevAttr(devId, &stDevAttr);
	if (ret == RK_ERR_VI_NOT_CONFIG) {
		// 0-1.config dev
		ret = RK_MPI_VI_SetDevAttr(devId, &stDevAttr);
		if (ret != RK_SUCCESS) {
			printf("RK_MPI_VI_SetDevAttr %x\n", ret);
			return -1;
		}
	} else {
		printf("RK_MPI_VI_SetDevAttr already\n");
	}
	// 1.get dev enable status
	ret = RK_MPI_VI_GetDevIsEnable(devId);
	if (ret != RK_SUCCESS) {
		// 1-2.enable dev
		ret = RK_MPI_VI_EnableDev(devId);
		if (ret != RK_SUCCESS) {
			printf("RK_MPI_VI_EnableDev %x\n", ret);
			return -1;
		}
		// 1-3.bind dev/pipe
		stBindPipe.u32Num = 1;
		stBindPipe.PipeId[0] = pipeId;
		ret = RK_MPI_VI_SetDevBindPipe(devId, &stBindPipe);
		if (ret != RK_SUCCESS) {
			printf("RK_MPI_VI_SetDevBindPipe %x\n", ret);
			return -1;
		}
	} else {
		printf("RK_MPI_VI_EnableDev already\n");
	}

	return 0;
}

int vi_chn_init(int channelId, int width, int height) {
	int ret;
	int buf_cnt = 2;
	// VI init
	VI_CHN_ATTR_S vi_chn_attr;
	memset(&vi_chn_attr, 0, sizeof(vi_chn_attr));
	vi_chn_attr.stIspOpt.u32BufCount = buf_cnt;
	vi_chn_attr.stIspOpt.enMemoryType =
	    VI_V4L2_MEMORY_TYPE_DMABUF; // VI_V4L2_MEMORY_TYPE_MMAP;
	vi_chn_attr.stSize.u32Width = width;
	vi_chn_attr.stSize.u32Height = height;
	vi_chn_attr.enPixelFormat = RK_FMT_YUV420SP;
	vi_chn_attr.enCompressMode = COMPRESS_MODE_NONE; // COMPRESS_AFBC_16x16;
	vi_chn_attr.u32Depth = 2; //0, get fail, 1 - u32BufCount, can get, if bind to other device, must be < u32BufCount
	ret = RK_MPI_VI_SetChnAttr(0, channelId, &vi_chn_attr);
	ret |= RK_MPI_VI_EnableChn(0, channelId);
	if (ret) {
		printf("ERROR: create VI error! ret=%d\n", ret);
		return ret;
	}

	return ret;
}

static int vo_init(int VoLayer, int VoDev, int VoChn, int Width, int Height) {
	int ret = RK_SUCCESS;
	VO_PUB_ATTR_S            stVoPubAttr;
	VO_VIDEO_LAYER_ATTR_S    stLayerAttr;
	VO_CHN_ATTR_S            stChnAttr;

	ret = RK_MPI_VO_BindLayer(VoLayer, VoDev, VO_LAYER_MODE_GRAPHIC);
	if (ret != RK_SUCCESS) {
		RK_LOGE("RK_MPI_VO_BindLayer failed, ret = %x", ret);
		return ret;
	}

	memset(&stVoPubAttr, 0, sizeof(VO_PUB_ATTR_S));
	memset(&stLayerAttr, 0, sizeof(VO_VIDEO_LAYER_ATTR_S));
	memset(&stChnAttr, 0, sizeof(VO_CHN_ATTR_S));

	stVoPubAttr.enIntfType = VO_INTF_DEFAULT;
	stVoPubAttr.enIntfSync = VO_OUTPUT_DEFAULT;

	ret = RK_MPI_VO_SetPubAttr(VoDev, &stVoPubAttr);
	if (ret != RK_SUCCESS) {
		RK_LOGE("RK_MPI_VO_SetPubAttr failed, ret = %x", ret);
		return ret;
	}

	ret = RK_MPI_VO_Enable(VoDev);
	if (ret != RK_SUCCESS) {
		RK_LOGE("RK_MPI_VO_Enable failed, ret = %x", ret);
		return ret;
	}

	/* Enable Layer */
	stLayerAttr.enPixFormat      = RK_FMT_RGB888;
	stLayerAttr.enCompressMode   = COMPRESS_AFBC_16x16;
	stLayerAttr.stDispRect.s32X  = 0;
	stLayerAttr.stDispRect.s32Y  = 0;
	stLayerAttr.stDispRect.u32Width   = Width;
	stLayerAttr.stDispRect.u32Height  = Height;
	stLayerAttr.stImageSize.u32Width  = Width;
	stLayerAttr.stImageSize.u32Height = Height;
	stLayerAttr.u32DispFrmRt          = 25;

	ret = RK_MPI_VO_SetLayerAttr(VoLayer, &stLayerAttr);
	if (ret != RK_SUCCESS) {
		RK_LOGE("RK_MPI_VO_SetLayerAttr failed, ret = %x", ret);
		return ret;
	}

	RK_MPI_VO_SetLayerSpliceMode(VoLayer, VO_SPLICE_MODE_RGA);

	ret = RK_MPI_VO_EnableLayer(VoLayer);
	if (ret != RK_SUCCESS) {
		RK_LOGE("RK_MPI_VO_EnableLayer failed, ret = %x", ret);
		return ret;
	}

	stChnAttr.stRect.s32X = 0;
	stChnAttr.stRect.s32Y = 0;
	stChnAttr.stRect.u32Width = Width;
	stChnAttr.stRect.u32Height = Height;
	stChnAttr.u32FgAlpha = 255;
	stChnAttr.u32BgAlpha = 0;
	stChnAttr.enMirror = MIRROR_NONE;
	stChnAttr.enRotation = VoRotation;
	stChnAttr.u32Priority = 1;

	ret = RK_MPI_VO_SetChnAttr(VoLayer, VoChn, &stChnAttr);
	if (ret != RK_SUCCESS) {
		RK_LOGE("RK_MPI_VO_SetChnAttr failed, ret = %x", ret);
		return ret;
	}

	ret = RK_MPI_VO_EnableChn(VoLayer, VoChn);
	if (ret != RK_SUCCESS) {
		RK_LOGE("RK_MPI_VO_EnableChn failed, ret = %x", ret);
		return ret;
	}

	RK_LOGE("Create vo [dev: %d, layer: %d, chn: %d] success!",
				VoDev, VoLayer, VoChn);
	return ret;
}

static int vo_deinit(int VoLayer, int VoDev, int VoChn) {
	int ret = 0;

	ret = RK_MPI_VO_DisableChn(VoLayer, VoChn);
	if (ret != RK_SUCCESS) {
		RK_LOGE("RK_MPI_VO_DisableChn failed, ret = %x", ret);
		return ret;
	}

	ret = RK_MPI_VO_DisableLayer(VoLayer);
	if (ret != RK_SUCCESS) {
		RK_LOGE("RK_MPI_VO_DisableLayer failed, ret = %x", ret);
		return ret;
	}

	ret = RK_MPI_VO_Disable(VoDev);
	if (ret != RK_SUCCESS) {
		RK_LOGE("RK_MPI_VO_Disable failed, ret = %x", ret);
		return ret;
	}

	ret = RK_MPI_VO_UnBindLayer(VoLayer, VoDev);
	if (ret != RK_SUCCESS) {
		RK_LOGE("RK_MPI_VO_UnBindLayer failed, ret = %x", ret);
		return ret;
	}

	RK_MPI_VO_CloseFd();

	RK_LOGE("Destroy vo [dev: %d, layer: %d, chn: %d] success!",
			VoDev, VoLayer, VoChn);
	return ret;
}


static RK_CHAR optstr[] = "?::a:w:h:W:H:r:I:l:d:";
static void print_usage(const RK_CHAR *name) {
	printf("usage example:\n");
	printf("\t%s -a /oem/usr/share/iqfiles -w 2304 -h 1296 -W 720 -H 720 -r 180 -I 0 -l 0 -d 0\n", name);
	printf("\t-a | --aiq: IQ file directory, Default:/oem/usr/share/iqfiles\n");
	printf("\t-w | --vi-width: VI width, Default:2304\n");
	printf("\t-h | --vi-height: VI height, Default:1296\n");
	printf("\t-W | --vo-width: VO width, Default:720\n");
	printf("\t-H | --vo-height: VO height, Default:720\n");
	printf("\t-r | --rotation: VO rotation, Value:0,90,180,270, Default:180\n");
	printf("\t-I | --camid: camera ctx id, Default 0. "
	       "0:rkisp_mainpath,1:rkisp_selfpath,2:rkisp_bypasspath\n");
	printf("\t-l | --layer: Vo layer, Default 0. ");
	printf("\t-d | --device: Vo device, Default 0. ");
}

int main(int argc, char *argv[]) {
	RK_S32 s32Ret = RK_FAILURE;
	int c;
	int ret = -1;
	while ((c = getopt(argc, argv, optstr)) != -1) {
		switch (c) {
		case 'a':
			IqFileDir = optarg;
			break;
		case 'w':
			ViWidth = atoi(optarg);
			break;
		case 'h':
			ViHeight = atoi(optarg);
			break;
		case 'W':
			VoWidth = atoi(optarg);
			break;
		case 'H':
			VoHeight = atoi(optarg);
			break;
		case 'r':
			VoRotationDegrees = atoi(optarg);
			switch (VoRotationDegrees) {
			case 0:
				VoRotation = ROTATION_0;
				break;
			case 90:
				VoRotation = ROTATION_90;
				break;
			case 180:
				VoRotation = ROTATION_180;
				break;
			case 270:
				VoRotation = ROTATION_270;
				break;
			default:
				printf("Invalid rotation: %d. Use 0, 90, 180, or 270.\n",
				       VoRotationDegrees);
				return -1;
			}
			break;
		case 'I':
			s32chnlId = atoi(optarg);
			break;
		case 'l':
			VoLayer = atoi(optarg);
			break;
		case 'd':
			VoDev = atoi(optarg);
			break;
		case '?':
		default:
			print_usage(argv[0]);
			return -1;
		}
	}

	printf("#VI Resolution: %dx%d\n", ViWidth, ViHeight);
	printf("#VO Resolution: %dx%d\n", VoWidth, VoHeight);
	printf("#VO Rotation: %d\n", VoRotationDegrees);
	printf("#IQ File Dir: %s\n", IqFileDir);
	printf("#CameraIdx: %d\n\n", s32chnlId);
	printf("#Vo Layer: %d\n\n", VoLayer);
	printf("#Vo Devices: %d\n\n", VoDev);

	signal(SIGINT, sigterm_handler);

	s32Ret = isp_init(0, IqFileDir);
	if (s32Ret != RK_SUCCESS) {
		RK_LOGE("isp_init failed!");
		goto __FAILED;
	}

	if (RK_MPI_SYS_Init() != RK_SUCCESS) {
		RK_LOGE("rk mpi sys init fail!");
		goto __FAILED;
	}

	s32Ret = vi_dev_init();
	if (s32Ret != RK_SUCCESS) {
		RK_LOGE("vi_dev_init failed!");
		goto __FAILED;
	}

	s32Ret = vi_chn_init(s32chnlId, ViWidth, ViHeight);
	if (s32Ret != RK_SUCCESS) {
		RK_LOGE("vi_chn_init failed!");
		goto __FAILED;
	}

	s32Ret = vo_init(VoLayer, VoDev, VoChn, VoWidth, VoHeight);
	if (s32Ret != RK_SUCCESS) {
		RK_LOGE("vo_init failed!");
		goto __FAILED_VI_CHN;
	}

	pthread_t main_thread;
	pthread_create(&main_thread, NULL, GetMediaBuffer0, NULL);

	while (!quit) {
		usleep(50000);
	}
	pthread_join(main_thread, NULL);

	s32Ret = RK_MPI_VI_DisableChn(0, s32chnlId);
	RK_LOGE("RK_MPI_VI_DisableChn %x", s32Ret);

	s32Ret = RK_MPI_VI_DisableDev(0);
	RK_LOGE("RK_MPI_VI_DisableDev %x", s32Ret);

	vo_deinit(VoLayer, VoDev, VoChn);
	ret = 0;
	goto __FAILED;

__FAILED_VI_CHN:
	s32Ret = RK_MPI_VI_DisableChn(0, s32chnlId);
	RK_LOGE("RK_MPI_VI_DisableChn %x", s32Ret);

	s32Ret = RK_MPI_VI_DisableDev(0);
	RK_LOGE("RK_MPI_VI_DisableDev %x", s32Ret);

__FAILED:
	RK_LOGE("test running exit:%d", s32Ret);
	RK_MPI_SYS_Exit();
	isp_deinit();

	return ret;
}
