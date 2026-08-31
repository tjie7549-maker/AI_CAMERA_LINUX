#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "ai_cam.h"

static void print_usage(const char *name) {
    printf(
        "usage: %s [-a iq_dir] [-w vi_width] [-h vi_height] [-W vo_width] "
        "[-H vo_height] [-r rotation] [-I vi_channel] [-l vo_layer] "
        "[-d vo_device] [-o h264_file] [-n encoded_frames] "
        "[--no-vo --preview-shm NAME --preview-width WIDTH "
        "--preview-height HEIGHT --preview-fps FPS] "
        "[--face-snapshot-socket PATH --face-width WIDTH --face-height HEIGHT]\n",
        name);
}

static int control_socket_create(const char *path) {
    int fd;
    struct sockaddr_un addr;
    if (!path || strlen(path) >= sizeof(addr.sun_path))
        return -1;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    unlink(path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 2) < 0) {
        close(fd);
        unlink(path);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    return fd;
}
static void control_socket_poll(AiCamApp *app, int server_fd) {
    int fd;
    char request[96] = {0};
    int exposure = 0, gain = 0;
    while ((fd = accept(server_fd, NULL, NULL)) >= 0) {
        ssize_t n = read(fd, request, sizeof(request) - 1);
        const char *reply = "error\\n";
        if (n > 0) {
            request[n] = 0;
            if (strcmp(request, "auto\\n") == 0 && ai_cam_isp_set_auto_ae(app) == RK_SUCCESS)
                reply = "ok\\n";
            else if (sscanf(request, "manual %d %d", &exposure, &gain) == 2 &&
                     ai_cam_isp_set_manual_ae(app, exposure, gain) == RK_SUCCESS)
                reply = "ok\\n";
        }
        write(fd, reply, strlen(reply));
        close(fd);
    }
}

int main(int argc, char *argv[]) {
    AiCamApp app = {0};
    sigset_t shutdown_signals;
    struct timespec wait_timeout = {0, 50000000};
    static const struct option long_options[] = {
        {"no-vo", no_argument, NULL, 1000},
        {"preview-shm", required_argument, NULL, 1001},
        {"preview-width", required_argument, NULL, 1002},
        {"preview-height", required_argument, NULL, 1003},
        {"preview-fps", required_argument, NULL, 1004},
        {"isp-control-socket", required_argument, NULL, 1005},
        {"face-snapshot-socket", required_argument, NULL, 1006},
        {"face-width", required_argument, NULL, 1007},
        {"face-height", required_argument, NULL, 1008},
        {"help", no_argument, NULL, 1009},
        {NULL, 0, NULL, 0},
    };
    int option;

    ai_cam_default_config(&app.config);
    while ((option = getopt_long(argc, argv, "a:w:h:W:H:r:I:l:d:o:n:", long_options, NULL)) != -1) {
        switch (option) {
            case 'a':
                app.config.iq_file_dir = optarg;
                break;
            case 'w':
                app.config.vi_width = atoi(optarg);
                break;
            case 'h':
                app.config.vi_height = atoi(optarg);
                break;
            case 'W':
                app.config.vo_width = atoi(optarg);
                break;
            case 'H':
                app.config.vo_height = atoi(optarg);
                break;
            case 'r':
                app.config.vo_rotation_degrees = atoi(optarg);
                break;
            case 'I':
                app.config.vi_channel = atoi(optarg);
                break;
            case 'l':
                app.config.vo_layer = atoi(optarg);
                break;
            case 'd':
                app.config.vo_device = atoi(optarg);
                break;
            case 'o':
                app.config.h264_output_path = optarg;
                break;
            case 'n':
                app.config.venc_frame_limit = atoi(optarg);
                if (app.config.venc_frame_limit <= 0) {
                    fprintf(stderr, "encoded frame count must be positive\n");
                    return 1;
                }
                break;
            case 1000:
                app.config.enable_vo = false;
                break;
            case 1001:
                app.config.preview_shm_name = optarg;
                break;
            case 1002:
                app.config.preview_width = atoi(optarg);
                break;
            case 1003:
                app.config.preview_height = atoi(optarg);
                break;
            case 1004:
                app.config.preview_fps = atoi(optarg);
                break;
            case 1005:
                app.config.isp_control_socket = optarg;
                break;
            case 1006:
                app.config.face_snapshot_socket = optarg;
                break;
            case 1007:
                app.config.face_width = atoi(optarg);
                break;
            case 1008:
                app.config.face_height = atoi(optarg);
                break;
            case 1009:
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }
    if (app.config.preview_shm_name && app.config.enable_vo) {
        fprintf(stderr, "--preview-shm requires --no-vo so Qt is the only LCD owner\n");
        return 1;
    }
    if (app.config.preview_shm_name &&
        (app.config.preview_width < 2 || app.config.preview_height < 2 ||
         app.config.preview_fps < 1 || (app.config.preview_width & 1) ||
         (app.config.preview_height & 1))) {
        fprintf(stderr,
                "preview width/height must be positive even values and FPS must be positive\n");
        return 1;
    }
    if (app.config.face_snapshot_socket &&
        (app.config.face_width < 112 || app.config.face_height < 112 ||
         (app.config.face_width & 1) || (app.config.face_height & 1))) {
        fprintf(stderr, "face width/height must be even values of at least 112\n");
        return 1;
    }

    printf("#VI: %dx%d\n", app.config.vi_width, app.config.vi_height);
    if (app.config.enable_vo)
        printf("#LCD/VO: %dx%d, rotation: %d\n", app.config.vo_width, app.config.vo_height,
               app.config.vo_rotation_degrees);
    else
        printf("#LCD/VO: disabled; Qt owns DRM/LCD\n");
    printf("#VENC: H.264 %dx%d, %d kbps -> %s\n", app.config.venc_width, app.config.venc_height,
           app.config.venc_bitrate_kbps, app.config.h264_output_path);
    printf("#RTSP main: rtsp://0.0.0.0:554/live/0 (%dx%d, %d kbps)\n", app.config.venc_width,
           app.config.venc_height, app.config.venc_bitrate_kbps);
    printf("#RTSP sub: rtsp://0.0.0.0:554/live/1 (%dx%d, %d kbps)\n", app.config.sub_venc_width,
           app.config.sub_venc_height, app.config.sub_venc_bitrate_kbps);
    if (app.config.preview_shm_name)
        printf("#Preview: %s %dx%d RGB888 at %d FPS\n", app.config.preview_shm_name,
               app.config.preview_width, app.config.preview_height, app.config.preview_fps);
    if (app.config.face_snapshot_socket)
        printf("#Face snapshot socket: %s (%dx%d)\n", app.config.face_snapshot_socket,
               app.config.face_width, app.config.face_height);
    /* An RTSP client may disconnect while a VENC worker is sending a frame. */
    signal(SIGPIPE, SIG_IGN);
    sigemptyset(&shutdown_signals);
    sigaddset(&shutdown_signals, SIGINT);
    sigaddset(&shutdown_signals, SIGTERM);
    /*
     * Worker threads inherit this mask.  Keep Ctrl+C out of media ioctls and
     * consume it synchronously here before starting the orderly shutdown.
     */
    if (pthread_sigmask(SIG_BLOCK, &shutdown_signals, NULL) != 0) {
        fprintf(stderr, "failed to block shutdown signals\n");
        return 1;
    }
    if (ai_cam_start(&app) != RK_SUCCESS)
        return 1;
    int control_fd = control_socket_create(app.config.isp_control_socket);
    if (control_fd < 0) {
        fprintf(stderr, "failed to create ISP control socket\\n");
        ai_cam_stop(&app);
        return 1;
    }

    while (!ai_cam_is_stopping(&app)) {
        control_socket_poll(&app, control_fd);
        int signal_number = sigtimedwait(&shutdown_signals, NULL, &wait_timeout);

        if (signal_number == SIGINT || signal_number == SIGTERM)
            ai_cam_request_stop(&app);
        else if (signal_number < 0 && errno != EAGAIN && errno != EINTR) {
            perror("sigtimedwait");
            ai_cam_request_stop(&app);
        }
    }
    close(control_fd);
    unlink(app.config.isp_control_socket);
    ai_cam_stop(&app);
    return app.runtime_failed ? 1 : 0;
}
