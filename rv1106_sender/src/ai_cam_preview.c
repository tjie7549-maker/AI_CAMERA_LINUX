#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <rga/im2d.h>

#include "ai_cam.h"
#include "preview_shm_protocol.h"
#include "rk_debug.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_vpss.h"

#define AI_CAM_PREVIEW_WAIT_MS 200
#define AI_CAM_PREVIEW_SOCKET_WAIT_MS 200

static RK_U64 ai_cam_preview_now_ns(void) {
    struct timespec time = {0, 0};

    clock_gettime(CLOCK_MONOTONIC, &time);
    return (RK_U64)time.tv_sec * 1000000000ULL + (RK_U64)time.tv_nsec;
}

static void ai_cam_preview_publish(AiCamApp *app, RK_U64 frame_id, RK_U64 now_ns,
                                   RK_U32 active_index) {
    PreviewShmHeader *header = app->preview_header;
    RK_U32 sequence = __atomic_load_n(&header->sequence, __ATOMIC_RELAXED);

    if (sequence & 1U)
        sequence++;
    __atomic_store_n(&header->sequence, sequence + 1U, __ATOMIC_RELEASE);
    header->active_index = active_index;
    header->producer_online = 1;
    header->last_frame_id = frame_id;
    header->last_frame_monotonic_ns = now_ns;
    __atomic_store_n(&header->sequence, sequence + 2U, __ATOMIC_RELEASE);
}

static int ai_cam_preview_send_fds(AiCamApp *app, int client_fd) {
    PreviewFdMessage message;
    struct iovec iov;
    struct msghdr header;
    char control[CMSG_SPACE(sizeof(app->preview_block_fds))];
    struct cmsghdr *cmsg;
    ssize_t sent;

    memset(&message, 0, sizeof(message));
    message.magic = PREVIEW_FD_MESSAGE_MAGIC;
    message.version = PREVIEW_SHM_VERSION;
    message.width = app->config.preview_width;
    message.height = app->config.preview_height;
    message.stride = app->config.preview_width * 3U;
    message.pixel_format = PREVIEW_SHM_PIXFMT_RGB888;
    message.buffer_count = PREVIEW_SHM_BUFFER_COUNT;
    memset(&header, 0, sizeof(header));
    memset(control, 0, sizeof(control));
    iov.iov_base = &message;
    iov.iov_len = sizeof(message);
    header.msg_iov = &iov;
    header.msg_iovlen = 1;
    header.msg_control = control;
    header.msg_controllen = sizeof(control);
    cmsg = CMSG_FIRSTHDR(&header);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(app->preview_block_fds));
    memcpy(CMSG_DATA(cmsg), app->preview_block_fds, sizeof(app->preview_block_fds));
    sent = sendmsg(client_fd, &header, MSG_NOSIGNAL);
    return sent == (ssize_t)sizeof(message) ? RK_SUCCESS : RK_FAILURE;
}

static void *ai_cam_preview_fd_server(void *arg) {
    AiCamApp *app = arg;
    struct pollfd poll_fd;

    poll_fd.fd = app->preview_fd_server;
    poll_fd.events = POLLIN;
    while (!ai_cam_is_stopping(app)) {
        int ret = poll(&poll_fd, 1, AI_CAM_PREVIEW_SOCKET_WAIT_MS);

        if (ret <= 0)
            continue;
        if (poll_fd.revents & POLLIN) {
            int client_fd = accept(app->preview_fd_server, NULL, NULL);

            if (client_fd >= 0) {
                if (ai_cam_preview_send_fds(app, client_fd) != RK_SUCCESS)
                    RK_LOGE("preview DMA-BUF fd transfer failed: %s", strerror(errno));
                close(client_fd);
            }
        }
    }
    return NULL;
}

static void *ai_cam_preview_frames(void *arg) {
    AiCamApp *app = arg;
    RK_U64 frame_id = 0;
    RK_U64 stat_start_ns = ai_cam_preview_now_ns();
    RK_U64 next_convert_ns = 0;
    RK_U64 frame_interval_ns = 1000000000ULL / (RK_U64)app->config.preview_fps;
    RK_U32 stat_frames = 0;
    RK_U32 stat_failures = 0;

    while (!ai_cam_is_stopping(app)) {
        VIDEO_FRAME_INFO_S frame;
        RK_U32 active_index;
        RK_S32 source_fd;
        rga_buffer_t source;
        rga_buffer_t target;
        IM_STATUS rga_ret;
        RK_U64 now_ns;
        int ret;

        memset(&frame, 0, sizeof(frame));
        ret = RK_MPI_VPSS_GetChnFrame(AI_CAM_VPSS_GRP, AI_CAM_VPSS_PREVIEW_CHN,
                                      &frame, AI_CAM_PREVIEW_WAIT_MS);
        if (ret != RK_SUCCESS)
            continue;

        now_ns = ai_cam_preview_now_ns();
        if (next_convert_ns && now_ns < next_convert_ns)
            goto release_frame;

        /* Keep a fixed cadence: 25 FPS source frames otherwise quantize a
         * 15 FPS "now + interval" gate down to roughly 12.5 FPS. */
        if (!next_convert_ns)
            next_convert_ns = now_ns;
        do {
            next_convert_ns += frame_interval_ns;
        } while (next_convert_ns <= now_ns);

        active_index = 1U - __atomic_load_n(&app->preview_header->active_index,
                                             __ATOMIC_RELAXED);
        source_fd = RK_MPI_MB_Handle2Fd(frame.stVFrame.pMbBlk);
        if (source_fd < 0) {
            RK_LOGE("preview RK_MPI_MB_Handle2Fd failed: %d", source_fd);
            stat_failures++;
            goto release_frame;
        }

        source = wrapbuffer_fd_t(source_fd, frame.stVFrame.u32Width,
                                 frame.stVFrame.u32Height,
                                 frame.stVFrame.u32VirWidth,
                                 frame.stVFrame.u32VirHeight,
                                 RK_FORMAT_YCbCr_420_SP);
        target = wrapbuffer_fd_t(app->preview_block_fds[active_index],
                                 app->config.preview_width,
                                 app->config.preview_height,
                                 app->config.preview_width,
                                 app->config.preview_height,
                                 RK_FORMAT_RGB_888);
        rga_ret = imcvtcolor_t(source, target, RK_FORMAT_YCbCr_420_SP,
                               RK_FORMAT_RGB_888, IM_YUV_TO_RGB_BT601_LIMIT, 1);
        if (rga_ret != IM_STATUS_SUCCESS) {
            RK_LOGE("preview RGA NV12->RGB888 failed: %s", imStrError_t(rga_ret));
            stat_failures++;
            if (stat_failures >= 5) {
                app->runtime_failed = 1;
                ai_cam_request_stop(app);
            }
            goto release_frame;
        }

        now_ns = ai_cam_preview_now_ns();
        ai_cam_preview_publish(app, ++frame_id, now_ns, active_index);
        stat_frames++;

release_frame:
        RK_MPI_VPSS_ReleaseChnFrame(AI_CAM_VPSS_GRP, AI_CAM_VPSS_PREVIEW_CHN,
                                    &frame);
        now_ns = ai_cam_preview_now_ns();
        if (now_ns - stat_start_ns >= 1000000000ULL) {
            double seconds = (double)(now_ns - stat_start_ns) / 1000000000.0;

            printf("#Stats: Preview %dx%d RGB888 FPS=%.2f, frame=%llu, rga_errors=%u\n",
                   app->config.preview_width, app->config.preview_height,
                   stat_frames / seconds, (unsigned long long)frame_id, stat_failures);
            fflush(stdout);
            stat_start_ns = now_ns;
            stat_frames = 0;
            stat_failures = 0;
        }
    }
    return NULL;
}

static void ai_cam_preview_release_buffers(AiCamApp *app) {
    RK_U32 index;

    for (index = 0; index < PREVIEW_SHM_BUFFER_COUNT; ++index) {
        if (app->preview_rga_handles[index]) {
            releasebuffer_handle(app->preview_rga_handles[index]);
            app->preview_rga_handles[index] = 0;
        }
        if (app->preview_blocks[index]) {
            RK_MPI_MMZ_Free(app->preview_blocks[index]);
            app->preview_blocks[index] = NULL;
        }
        app->preview_block_fds[index] = -1;
    }
}

static int ai_cam_preview_create_socket(AiCamApp *app) {
    struct sockaddr_un address;
    int fd;

    unlink(PREVIEW_FD_SOCKET_PATH);
    fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0)
        return RK_FAILURE;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, PREVIEW_FD_SOCKET_PATH, sizeof(address.sun_path) - 1);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(fd, 2) != 0) {
        close(fd);
        unlink(PREVIEW_FD_SOCKET_PATH);
        return RK_FAILURE;
    }
    app->preview_fd_server = fd;
    return RK_SUCCESS;
}

int ai_cam_preview_start(AiCamApp *app) {
    PreviewShmHeader *header;
    size_t image_bytes;
    size_t total_bytes;
    RK_U32 index;
    int fd;
    int ret;

    if (!app->config.preview_shm_name)
        return RK_SUCCESS;
    if (app->config.preview_width < 2 || app->config.preview_height < 2 ||
        (app->config.preview_width & 1) || (app->config.preview_height & 1))
        return RK_FAILURE;

    image_bytes = (size_t)app->config.preview_width * app->config.preview_height * 3U;
    total_bytes = sizeof(PreviewShmHeader);
    fd = shm_open(app->config.preview_shm_name, O_CREAT | O_RDWR, 0660);
    if (fd < 0) {
        RK_LOGE("shm_open %s failed: %s", app->config.preview_shm_name, strerror(errno));
        return RK_FAILURE;
    }
    if (ftruncate(fd, (off_t)total_bytes) != 0) {
        RK_LOGE("ftruncate %s failed: %s", app->config.preview_shm_name, strerror(errno));
        close(fd);
        return RK_FAILURE;
    }
    header = mmap(NULL, total_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (header == MAP_FAILED) {
        RK_LOGE("mmap %s failed: %s", app->config.preview_shm_name, strerror(errno));
        close(fd);
        return RK_FAILURE;
    }

    memset(header, 0, total_bytes);
    header->magic = PREVIEW_SHM_MAGIC;
    header->version = PREVIEW_SHM_VERSION;
    header->header_size = sizeof(*header);
    header->width = app->config.preview_width;
    header->height = app->config.preview_height;
    header->stride = app->config.preview_width * 3U;
    header->pixel_format = PREVIEW_SHM_PIXFMT_RGB888;
    header->buffer_count = PREVIEW_SHM_BUFFER_COUNT;
    __atomic_store_n(&header->sequence, 0U, __ATOMIC_RELEASE);

    app->preview_shm_fd = fd;
    app->preview_shm_bytes = total_bytes;
    app->preview_image_bytes = image_bytes;
    app->preview_header = header;
    app->preview_fd_server = -1;
    for (index = 0; index < PREVIEW_SHM_BUFFER_COUNT; ++index) {
        app->preview_block_fds[index] = -1;
        ret = RK_MPI_MMZ_Alloc(&app->preview_blocks[index], image_bytes,
                               RK_MMZ_ALLOC_TYPE_CMA | RK_MMZ_ALLOC_UNCACHEABLE);
        if (ret != RK_SUCCESS) {
            RK_LOGE("preview MMZ allocation %u failed: %x", index, ret);
            goto failed;
        }
        app->preview_block_fds[index] = RK_MPI_MMZ_Handle2Fd(app->preview_blocks[index]);
        if (app->preview_block_fds[index] < 0) {
            RK_LOGE("preview MMZ fd %u failed: %d", index, app->preview_block_fds[index]);
            goto failed;
        }
    }
    if (ai_cam_preview_create_socket(app) != RK_SUCCESS) {
        RK_LOGE("preview fd socket %s failed: %s", PREVIEW_FD_SOCKET_PATH, strerror(errno));
        goto failed;
    }
    app->preview_initialized = true;
    if (pthread_create(&app->preview_thread, NULL, ai_cam_preview_frames, app) != 0) {
        RK_LOGE("preview conversion thread creation failed");
        goto failed;
    }
    app->preview_thread_started = true;
    if (pthread_create(&app->preview_fd_thread, NULL, ai_cam_preview_fd_server, app) != 0) {
        RK_LOGE("preview fd server thread creation failed");
        goto failed;
    }
    app->preview_fd_thread_started = true;
    printf("#Preview: VPSS ch%d NV12 -> RGA RGB888 DMA-BUF -> shm metadata %s (%dx%d)\n",
           AI_CAM_VPSS_PREVIEW_CHN, app->config.preview_shm_name,
           app->config.preview_width, app->config.preview_height);
    return RK_SUCCESS;

failed:
    ai_cam_request_stop(app);
    if (app->preview_thread_started) {
        pthread_join(app->preview_thread, NULL);
        app->preview_thread_started = false;
    }
    if (app->preview_fd_thread_started) {
        pthread_join(app->preview_fd_thread, NULL);
        app->preview_fd_thread_started = false;
    }
    if (app->preview_fd_server >= 0) {
        close(app->preview_fd_server);
        app->preview_fd_server = -1;
    }
    unlink(PREVIEW_FD_SOCKET_PATH);
    ai_cam_preview_release_buffers(app);
    munmap(header, total_bytes);
    close(fd);
    app->preview_header = NULL;
    app->preview_shm_fd = -1;
    app->preview_shm_bytes = 0;
    app->preview_image_bytes = 0;
    return RK_FAILURE;
}

void ai_cam_preview_stop(AiCamApp *app) {
    if (!app->preview_initialized)
        return;
    if (app->preview_thread_started) {
        pthread_join(app->preview_thread, NULL);
        app->preview_thread_started = false;
    }
    if (app->preview_fd_thread_started) {
        pthread_join(app->preview_fd_thread, NULL);
        app->preview_fd_thread_started = false;
    }
    __atomic_store_n(&app->preview_header->producer_online, 0U, __ATOMIC_RELEASE);
    __atomic_fetch_add(&app->preview_header->sequence, 2U, __ATOMIC_RELEASE);
    if (app->preview_fd_server >= 0) {
        close(app->preview_fd_server);
        app->preview_fd_server = -1;
    }
    unlink(PREVIEW_FD_SOCKET_PATH);
    ai_cam_preview_release_buffers(app);
    munmap(app->preview_header, app->preview_shm_bytes);
    close(app->preview_shm_fd);
    app->preview_header = NULL;
    app->preview_shm_fd = -1;
    app->preview_shm_bytes = 0;
    app->preview_image_bytes = 0;
    app->preview_initialized = false;
}
