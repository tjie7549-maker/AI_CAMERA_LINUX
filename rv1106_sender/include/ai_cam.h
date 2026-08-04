#ifndef AI_CAM_H
#define AI_CAM_H

#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdbool.h>

#include <rk_aiq_user_api2_sysctl.h>

#include "rk_defines.h"
#include "rk_mpi_venc.h"
#include "rk_mpi_mmz.h"
#include "rtsp_demo.h"
#include "preview_shm_protocol.h"

#define AI_CAM_VI_DEV 0
#define AI_CAM_VI_PIPE 0
#define AI_CAM_VPSS_GRP 0
#define AI_CAM_VPSS_DISPLAY_CHN 0
#define AI_CAM_VPSS_ENCODE_CHN 1
#define AI_CAM_VPSS_SUB_ENCODE_CHN 2
#define AI_CAM_VPSS_PREVIEW_CHN 3
#define AI_CAM_VENC_CHN 0
#define AI_CAM_SUB_VENC_CHN 1

typedef struct {
	int vi_width;
	int vi_height;
	int vo_width;
	int vo_height;
	int vi_channel;
	int vo_layer;
	int vo_device;
	int vo_channel;
	int vo_rotation_degrees;
	const char *iq_file_dir;
	const char *h264_output_path;
	int venc_width;
	int venc_height;
	int source_fps;
	int venc_fps;
	int venc_bitrate_kbps;
	int sub_venc_width;
	int sub_venc_height;
	int sub_venc_fps;
	int sub_venc_bitrate_kbps;
	RK_S32 venc_frame_limit;
	bool enable_vo;
	const char *preview_shm_name;
	int preview_width;
	int preview_height;
	int preview_fps;
} AiCamConfig;

typedef struct {
	RK_U32 frame_count;
	RK_U32 latency_count;
	RK_U64 latency_sum_us;
	RK_U64 latency_max_us;
} AiCamStats;

typedef struct {
	AiCamConfig config;
	rk_aiq_sys_ctx_t *aiq_ctx;
	volatile sig_atomic_t stop_requested;
	volatile sig_atomic_t runtime_failed;
	bool isp_initialized;
	bool sys_initialized;
	bool vi_dev_initialized;
	bool vi_chn_initialized;
	bool vpss_initialized;
	bool venc_initialized;
	bool vpss_venc_bound;
	bool sub_venc_initialized;
	bool vpss_sub_venc_bound;
	bool vo_initialized;
	bool rtsp_initialized;
	bool rtsp_mutex_initialized;
	bool stats_mutex_initialized;
	bool vo_thread_started;
	bool venc_thread_started;
	bool sub_venc_thread_started;
	bool forwarding_thread_started;
	bool preview_initialized;
	bool preview_thread_started;
	bool preview_fd_thread_started;
	rtsp_demo_handle rtsp_demo;
	rtsp_session_handle rtsp_main_session;
	rtsp_session_handle rtsp_sub_session;
	pthread_mutex_t rtsp_mutex;
	pthread_mutex_t stats_mutex;
	AiCamStats main_rtsp_stats;
	AiCamStats sub_rtsp_stats;
	pthread_t vo_thread;
	pthread_t venc_thread;
	pthread_t sub_venc_thread;
	pthread_t forwarding_thread;
	pthread_t preview_thread;
	pthread_t preview_fd_thread;
	int preview_shm_fd;
	int preview_fd_server;
	size_t preview_shm_bytes;
	size_t preview_image_bytes;
	PreviewShmHeader *preview_header;
	MB_BLK preview_blocks[PREVIEW_SHM_BUFFER_COUNT];
	RK_S32 preview_block_fds[PREVIEW_SHM_BUFFER_COUNT];
	RK_U32 preview_rga_handles[PREVIEW_SHM_BUFFER_COUNT];
} AiCamApp;

void ai_cam_default_config(AiCamConfig *config);
int ai_cam_start(AiCamApp *app);
void ai_cam_stop(AiCamApp *app);
void ai_cam_request_stop(AiCamApp *app);
bool ai_cam_is_stopping(const AiCamApp *app);
int ai_cam_stats_start(AiCamApp *app);
void ai_cam_stats_stop(AiCamApp *app);
void ai_cam_stats_record_rtsp(AiCamApp *app, bool is_main_stream, RK_U64 now_us,
				      RK_U64 stream_pts);
void ai_cam_stats_print_period(AiCamApp *app, const AiCamStats *lcd_stats,
				       RK_U64 elapsed_us);

int ai_cam_isp_start(AiCamApp *app);
void ai_cam_isp_stop(AiCamApp *app);
int ai_cam_vi_start(AiCamApp *app);
void ai_cam_vi_stop(AiCamApp *app);
int ai_cam_vpss_start(AiCamApp *app);
void ai_cam_vpss_stop(AiCamApp *app);
int ai_cam_vo_start(AiCamApp *app);
void ai_cam_vo_stop(AiCamApp *app);
int ai_cam_venc_start(AiCamApp *app);
void ai_cam_venc_stop(AiCamApp *app);
int ai_cam_venc_bind_vpss(AiCamApp *app);
void ai_cam_venc_unbind_vpss(AiCamApp *app);
void *ai_cam_venc_write_stream(void *arg);
void *ai_cam_sub_venc_write_stream(void *arg);
int ai_cam_rtsp_start(AiCamApp *app);
void ai_cam_rtsp_stop(AiCamApp *app);
int ai_cam_preview_start(AiCamApp *app);
void ai_cam_preview_stop(AiCamApp *app);

#endif
