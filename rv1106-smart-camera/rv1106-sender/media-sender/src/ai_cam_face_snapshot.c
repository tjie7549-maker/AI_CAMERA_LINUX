/*
 * Optional high-resolution frame grabber for the separate attendance UI.
 *
 * Protocol (local Unix socket only):
 *   request:  "capture <x> <y> <w> <h>\n"  (normalised 0..1 rectangle)
 *   reply:    "OK <width> <height> <byte_count>\n" followed by NV12 bytes
 *          or "ERR <reason>\n"
 *
 * The rectangle is deliberately supplied by the caller.  A face detector can
 * publish its latest box to the attendance UI later; this media process does
 * not claim that a person detector is a face detector.
 */
#include <errno.h>

/* 按需人脸 ROI 抓拍服务：通过 Unix Socket 接收归一化坐标，从 VPSS 人脸支路
 * 获取高分辨率 NV12
 * 帧并返回裁剪图像，避免持续占用高分辨率缓冲。 */
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "ai_cam.h"
#include "rk_debug.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_vpss.h"

#define FACE_SNAPSHOT_TIMEOUT_MS 400
#define FACE_REQUEST_MAX 96

static int write_all(int fd, const void *data, size_t bytes) {
    const unsigned char *cursor = data;
    while (bytes > 0) {
        ssize_t written = write(fd, cursor, bytes);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return -1;
        cursor += written;
        bytes -= (size_t)written;
    }
    return 0;
}

static int normalised_crop(float x, float y, float w, float h, int image_width, int image_height,
                           int *crop_x, int *crop_y, int *crop_width, int *crop_height) {
    int left, top, right, bottom;

    if (!(x >= 0.0f && y >= 0.0f && w > 0.0f && h > 0.0f && x + w <= 1.0f && y + h <= 1.0f))
        return -1;
    left = ((int)(x * image_width)) & ~1;
    top = ((int)(y * image_height)) & ~1;
    right = ((int)((x + w) * image_width)) & ~1;
    bottom = ((int)((y + h) * image_height)) & ~1;
    if (right > image_width)
        right = image_width & ~1;
    if (bottom > image_height)
        bottom = image_height & ~1;
    if (right - left < 112 || bottom - top < 112)
        return -1;
    *crop_x = left;
    *crop_y = top;
    *crop_width = right - left;
    *crop_height = bottom - top;
    return 0;
}

static int capture_nv12_roi(AiCamApp *app, float x, float y, float w, float h,
                            unsigned char **result, size_t *result_bytes, int *result_width,
                            int *result_height) {
    VIDEO_FRAME_INFO_S frame;
    int ret;
    int fd;
    int width, height, stride;
    int crop_x, crop_y, crop_width, crop_height;
    size_t mapped_bytes;
    unsigned char *mapped = MAP_FAILED;
    unsigned char *copy = NULL;
    int row;

    memset(&frame, 0, sizeof(frame));
    ret = RK_MPI_VPSS_GetChnFrame(AI_CAM_VPSS_GRP, AI_CAM_VPSS_FACE_CHN, &frame,
                                  FACE_SNAPSHOT_TIMEOUT_MS);
    if (ret != RK_SUCCESS)
        return -1;
    width = (int)frame.stVFrame.u32Width;
    height = (int)frame.stVFrame.u32Height;
    stride = (int)frame.stVFrame.u32VirWidth;
    if (width <= 0 || height <= 0 || stride < width || (width & 1) || (height & 1))
        goto failed;
    if (normalised_crop(x, y, w, h, width, height, &crop_x, &crop_y, &crop_width, &crop_height) !=
        0)
        goto failed;
    fd = RK_MPI_MB_Handle2Fd(frame.stVFrame.pMbBlk);
    if (fd < 0)
        goto failed;
    mapped_bytes = (size_t)stride * (size_t)height * 3 / 2;
    mapped = mmap(NULL, mapped_bytes, PROT_READ, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED)
        goto failed;
    *result_bytes = (size_t)crop_width * (size_t)crop_height * 3 / 2;
    copy = malloc(*result_bytes);
    if (!copy)
        goto failed;
    for (row = 0; row < crop_height; row++)
        memcpy(copy + (size_t)row * crop_width, mapped + (size_t)(crop_y + row) * stride + crop_x,
               crop_width);
    for (row = 0; row < crop_height / 2; row++)
        memcpy(copy + (size_t)crop_width * crop_height + (size_t)row * crop_width,
               mapped + (size_t)stride * height + (size_t)(crop_y / 2 + row) * stride + crop_x,
               crop_width);
    munmap(mapped, mapped_bytes);
    RK_MPI_VPSS_ReleaseChnFrame(AI_CAM_VPSS_GRP, AI_CAM_VPSS_FACE_CHN, &frame);
    *result = copy;
    *result_width = crop_width;
    *result_height = crop_height;
    return 0;

failed:
    if (mapped != MAP_FAILED)
        munmap(mapped, mapped_bytes);
    free(copy);
    RK_MPI_VPSS_ReleaseChnFrame(AI_CAM_VPSS_GRP, AI_CAM_VPSS_FACE_CHN, &frame);
    return -1;
}

static void serve_client(AiCamApp *app, int client_fd) {
    char request[FACE_REQUEST_MAX] = {0};
    char header[80];
    float x, y, w, h;
    unsigned char *nv12 = NULL;
    size_t nv12_bytes = 0;
    int width = 0, height = 0;
    ssize_t got = read(client_fd, request, sizeof(request) - 1);

    if (got <= 0 || sscanf(request, "capture %f %f %f %f", &x, &y, &w, &h) != 4 ||
        capture_nv12_roi(app, x, y, w, h, &nv12, &nv12_bytes, &width, &height) != 0) {
        write_all(client_fd, "ERR capture_failed\n", 19);
        return;
    }
    snprintf(header, sizeof(header), "OK %d %d %lu\n", width, height, (unsigned long)nv12_bytes);
    if (write_all(client_fd, header, strlen(header)) == 0)
        write_all(client_fd, nv12, nv12_bytes);
    free(nv12);
}

static void *face_snapshot_thread(void *arg) {
    AiCamApp *app = arg;
    while (!ai_cam_is_stopping(app)) {
        fd_set readable;
        struct timeval timeout = {0, 200000};
        int ready;
        int client_fd;
        FD_ZERO(&readable);
        FD_SET(app->face_snapshot_server, &readable);
        ready = select(app->face_snapshot_server + 1, &readable, NULL, NULL, &timeout);
        if (ready <= 0)
            continue;
        client_fd = accept(app->face_snapshot_server, NULL, NULL);
        if (client_fd < 0)
            continue;
        serve_client(app, client_fd);
        close(client_fd);
    }
    return NULL;
}

int ai_cam_face_snapshot_start(AiCamApp *app) {
    struct sockaddr_un address;
    int fd;
    if (!app->config.face_snapshot_socket ||
        strlen(app->config.face_snapshot_socket) >= sizeof(address.sun_path))
        return RK_FAILURE;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return RK_FAILURE;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, app->config.face_snapshot_socket, sizeof(address.sun_path) - 1);
    unlink(address.sun_path);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(fd, 2) != 0) {
        close(fd);
        unlink(address.sun_path);
        return RK_FAILURE;
    }
    app->face_snapshot_server = fd;
    if (pthread_create(&app->face_snapshot_thread, NULL, face_snapshot_thread, app) != 0) {
        close(fd);
        unlink(address.sun_path);
        return RK_FAILURE;
    }
    app->face_snapshot_thread_started = true;
    return RK_SUCCESS;
}

void ai_cam_face_snapshot_stop(AiCamApp *app) {
    if (!app->face_snapshot_thread_started)
        return;
    /* select wakes within 200 ms; do not close a descriptor still used by it. */
    pthread_join(app->face_snapshot_thread, NULL);
    close(app->face_snapshot_server);
    unlink(app->config.face_snapshot_socket);
    app->face_snapshot_thread_started = false;
}
