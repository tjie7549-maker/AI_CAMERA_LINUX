#include <string.h>

/* VI 采集模块：配置 RV1106 视频输入设备为 DMA-BUF NV12 输出，
 * 供后续 VPSS
 * 分流，避免应用层复制原始摄像头帧。 */

#include "ai_cam.h"
#include "rk_debug.h"
#include "rk_mpi_vi.h"

int ai_cam_vi_start(AiCamApp *app) {
    VI_DEV_ATTR_S dev_attr;
    VI_DEV_BIND_PIPE_S bind_pipe;
    VI_CHN_ATTR_S chn_attr;
    int ret;

    memset(&dev_attr, 0, sizeof(dev_attr));
    memset(&bind_pipe, 0, sizeof(bind_pipe));
    ret = RK_MPI_VI_GetDevAttr(AI_CAM_VI_DEV, &dev_attr);
    if (ret == RK_ERR_VI_NOT_CONFIG) {
        ret = RK_MPI_VI_SetDevAttr(AI_CAM_VI_DEV, &dev_attr);
        if (ret != RK_SUCCESS)
            return ret;
    }

    ret = RK_MPI_VI_GetDevIsEnable(AI_CAM_VI_DEV);
    if (ret != RK_SUCCESS) {
        ret = RK_MPI_VI_EnableDev(AI_CAM_VI_DEV);
        if (ret != RK_SUCCESS)
            return ret;
        bind_pipe.u32Num = 1;
        bind_pipe.PipeId[0] = AI_CAM_VI_PIPE;
        ret = RK_MPI_VI_SetDevBindPipe(AI_CAM_VI_DEV, &bind_pipe);
        if (ret != RK_SUCCESS) {
            RK_MPI_VI_DisableDev(AI_CAM_VI_DEV);
            return ret;
        }
    }
    app->vi_dev_initialized = true;

    memset(&chn_attr, 0, sizeof(chn_attr));
    chn_attr.stIspOpt.u32BufCount = 2;
    chn_attr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    chn_attr.stSize.u32Width = app->config.vi_width;
    chn_attr.stSize.u32Height = app->config.vi_height;
    chn_attr.enPixelFormat = RK_FMT_YUV420SP;
    chn_attr.enCompressMode = COMPRESS_MODE_NONE;
    chn_attr.u32Depth = 2;
    ret = RK_MPI_VI_SetChnAttr(AI_CAM_VI_PIPE, app->config.vi_channel, &chn_attr);
    if (ret != RK_SUCCESS)
        return ret;
    ret = RK_MPI_VI_EnableChn(AI_CAM_VI_PIPE, app->config.vi_channel);
    if (ret != RK_SUCCESS)
        return ret;
    app->vi_chn_initialized = true;
    return RK_SUCCESS;
}

void ai_cam_vi_stop(AiCamApp *app) {
    if (app->vi_chn_initialized) {
        RK_LOGE("RK_MPI_VI_DisableChn %x",
                RK_MPI_VI_DisableChn(AI_CAM_VI_PIPE, app->config.vi_channel));
        app->vi_chn_initialized = false;
    }
    if (app->vi_dev_initialized) {
        RK_LOGE("RK_MPI_VI_DisableDev %x", RK_MPI_VI_DisableDev(AI_CAM_VI_DEV));
        app->vi_dev_initialized = false;
    }
}
