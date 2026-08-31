/* rkipc RTSP -> existing RGB shared-preview protocol; never opens a camera. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "preview_shm_protocol.h"

#define PREVIEW_NAME "/ai_cam_preview"
#define PREVIEW_BUFFER0 "/ai_cam_preview.buf0"
#define PREVIEW_BUFFER1 "/ai_cam_preview.buf1"

static volatile sig_atomic_t keep_running = 1;
static pid_t ffmpeg_pid = -1;

static void on_signal(int unused) {
    (void)unused;
    keep_running = 0;
    if (ffmpeg_pid > 0)
        kill(ffmpeg_pid, SIGTERM);
}
static uint64_t monotonic_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}
static void wait_100ms(void) {
    const struct timespec delay = {0, 100000000L};
    nanosleep(&delay, NULL);
}

/* A local RTSP session can stay connected while no packets arrive.  A plain
 * blocking read would then freeze the UI forever, so treat a 2.5 s gap as a
 * broken session and recreate only the ffmpeg client. */
static int read_full(int fd, void *buffer, size_t bytes) {
    unsigned char *cursor = (unsigned char *)buffer;
    while (bytes > 0 && keep_running) {
        struct pollfd pfd;
        int ready;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        do {
            ready = poll(&pfd, 1, 2500);
        } while (ready < 0 && errno == EINTR);
        if (ready <= 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)))
            return -1;
        ssize_t received = read(fd, cursor, bytes);
        if (received > 0) {
            cursor += received;
            bytes -= (size_t)received;
            continue;
        }
        if (received < 0 && errno == EINTR)
            continue;
        return -1;
    }
    return bytes == 0 ? 0 : -1;
}

static void stop_ffmpeg(void) {
    int status, tries;
    if (ffmpeg_pid <= 0)
        return;
    kill(ffmpeg_pid, SIGTERM);
    for (tries = 0; tries < 20; ++tries) {
        if (waitpid(ffmpeg_pid, &status, WNOHANG) == ffmpeg_pid) {
            ffmpeg_pid = -1;
            return;
        }
        wait_100ms();
    }
    kill(ffmpeg_pid, SIGKILL);
    while (waitpid(ffmpeg_pid, &status, 0) < 0 && errno == EINTR) {
    }
    ffmpeg_pid = -1;
}

static int create_shared_buffer(const char *name, size_t bytes, int *fd, void **mapping) {
    shm_unlink(name);
    *fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0660);
    if (*fd < 0 || ftruncate(*fd, (off_t)bytes) != 0)
        return -1;
    *mapping = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, *fd, 0);
    return *mapping == MAP_FAILED ? -1 : 0;
}

static int send_buffer_fds(int client_fd, int buffer_fds[2], uint32_t width, uint32_t height) {
    PreviewFdMessage message;
    struct iovec iov;
    struct msghdr header;
    char control[CMSG_SPACE(sizeof(int) * 2)];
    struct cmsghdr *cmsg;
    memset(&message, 0, sizeof(message));
    message.magic = PREVIEW_FD_MESSAGE_MAGIC;
    message.version = PREVIEW_SHM_VERSION;
    message.width = width;
    message.height = height;
    message.stride = width * 3U;
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
    cmsg->cmsg_len = CMSG_LEN(sizeof(int) * 2);
    memcpy(CMSG_DATA(cmsg), buffer_fds, sizeof(int) * 2);
    return sendmsg(client_fd, &header, 0) == (ssize_t)sizeof(message) ? 0 : -1;
}

static void service_clients(int server_fd, int buffer_fds[2], uint32_t width, uint32_t height) {
    for (;;) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0)
            return;
        (void)send_buffer_fds(client_fd, buffer_fds, width, height);
        close(client_fd);
    }
}

static int start_ffmpeg(const char *url, int *read_fd) {
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0)
        return -1;
    ffmpeg_pid = fork();
    if (ffmpeg_pid < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return -1;
    }
    if (ffmpeg_pid == 0) {
        dup2(pipe_fds[1], STDOUT_FILENO);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        execl("/usr/bin/ffmpeg", "ffmpeg", "-nostdin", "-loglevel", "error", "-rtsp_transport",
              "tcp", "-i", url, "-an", "-vf", "scale=384:216", "-pix_fmt", "rgb24", "-f",
              "rawvideo", "pipe:1", (char *)NULL);
        _exit(127);
    }
    close(pipe_fds[1]);
    *read_fd = pipe_fds[0];
    return 0;
}

int main(int argc, char **argv) {
    const char *url = "rtsp://127.0.0.1/live/0";
    const uint32_t width = 384, height = 216, stride = width * 3U;
    const size_t image_bytes = (size_t)height * stride;
    int header_fd = -1, buffer_fds[2] = {-1, -1}, server_fd = -1, active_index = 0;
    void *header_map = MAP_FAILED, *buffer_maps[2] = {MAP_FAILED, MAP_FAILED};
    PreviewShmHeader *header;
    struct sockaddr_un address;
    uint64_t frame_id = 0;
    if (argc == 3 && strcmp(argv[1], "--url") == 0)
        url = argv[2];
    else if (argc != 1) {
        fprintf(stderr, "usage: %s [--url RTSP_URL]\n", argv[0]);
        return 2;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
    if (create_shared_buffer(PREVIEW_NAME, sizeof(PreviewShmHeader), &header_fd, &header_map) !=
            0 ||
        create_shared_buffer(PREVIEW_BUFFER0, image_bytes, &buffer_fds[0], &buffer_maps[0]) != 0 ||
        create_shared_buffer(PREVIEW_BUFFER1, image_bytes, &buffer_fds[1], &buffer_maps[1]) != 0) {
        perror("create preview shared memory");
        return 3;
    }
    header = (PreviewShmHeader *)header_map;
    memset(header, 0, sizeof(*header));
    header->magic = PREVIEW_SHM_MAGIC;
    header->version = PREVIEW_SHM_VERSION;
    header->header_size = sizeof(*header);
    header->width = width;
    header->height = height;
    header->stride = stride;
    header->pixel_format = PREVIEW_SHM_PIXFMT_RGB888;
    header->buffer_count = PREVIEW_SHM_BUFFER_COUNT;
    header->producer_online = 1;
    unlink(PREVIEW_FD_SOCKET_PATH);
    server_fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, PREVIEW_FD_SOCKET_PATH, sizeof(address.sun_path) - 1);
    if (server_fd < 0 || bind(server_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(server_fd, 4) != 0 || fcntl(server_fd, F_SETFL, O_NONBLOCK) != 0) {
        perror("preview fd socket");
        return 4;
    }
    fprintf(stderr, "RTSP preview bridge: %s -> %ux%u RGB\n", url, width, height);
    while (keep_running) {
        int read_fd = -1;
        if (start_ffmpeg(url, &read_fd) != 0) {
            sleep(1);
            continue;
        }
        while (keep_running && read_full(read_fd, buffer_maps[active_index], image_bytes) == 0) {
            __atomic_store_n(&header->producer_online, 1U, __ATOMIC_RELEASE);
            __atomic_fetch_add(&header->sequence, 1U, __ATOMIC_RELEASE);
            __atomic_store_n(&header->active_index, (uint32_t)active_index, __ATOMIC_RELEASE);
            __atomic_store_n(&header->last_frame_id, ++frame_id, __ATOMIC_RELEASE);
            __atomic_store_n(&header->last_frame_monotonic_ns, monotonic_ns(), __ATOMIC_RELEASE);
            __atomic_fetch_add(&header->sequence, 1U, __ATOMIC_RELEASE);
            service_clients(server_fd, buffer_fds, width, height);
            active_index = 1 - active_index;
        }
        close(read_fd);
        if (keep_running) {
            __atomic_store_n(&header->producer_online, 0U, __ATOMIC_RELEASE);
            fprintf(stderr, "RTSP preview bridge: session stalled; reconnecting\\n");
        }
        stop_ffmpeg();
        if (keep_running)
            sleep(1);
    }
    __atomic_store_n(&header->producer_online, 0U, __ATOMIC_RELEASE);
    stop_ffmpeg();
    if (server_fd >= 0)
        close(server_fd);
    unlink(PREVIEW_FD_SOCKET_PATH);
    if (header_map != MAP_FAILED)
        munmap(header_map, sizeof(PreviewShmHeader));
    if (buffer_maps[0] != MAP_FAILED)
        munmap(buffer_maps[0], image_bytes);
    if (buffer_maps[1] != MAP_FAILED)
        munmap(buffer_maps[1], image_bytes);
    if (header_fd >= 0)
        close(header_fd);
    if (buffer_fds[0] >= 0)
        close(buffer_fds[0]);
    if (buffer_fds[1] >= 0)
        close(buffer_fds[1]);
    shm_unlink(PREVIEW_NAME);
    shm_unlink(PREVIEW_BUFFER0);
    shm_unlink(PREVIEW_BUFFER1);
    return 0;
}
