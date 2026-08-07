/*
 * npu_detect: local person detection on RV1106 using preview shared memory.
 * Reads /ai_cam_preview (RGB888), letterboxes to 320x320, runs yolov5n int8
 * on the RKNPU, and pushes newline-delimited JSON to a ROCK 2A listener.
 * Build with -DNPU_DETECT_TEST_IMAGE for the standalone --image smoke test.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "rknn_api.h"
#include "preview_shm_protocol.h"

#ifdef NPU_DETECT_TEST_IMAGE
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#endif

#define MODEL_SIZE 320
#define MAX_BOXES 64
#define CONF_THRESHOLD 0.25f
#define NMS_THRESHOLD 0.45f
#define PERSON_CLASS 0
#define OBJ_CLASS_NUM 80
#define PROP_BOX_SIZE 85

typedef struct {
    float x, y, w, h, score;
} Box;

static volatile int g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

static double now_epoch_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

static inline float deqnt(int8_t q, int zp, float scale)
{
    return ((float)q - (float)zp) * scale;
}

static inline int8_t qnt(float f32, int zp, float scale)
{
    float q = f32 / scale + (float)zp;
    if (q > 127.0f) return 127;
    if (q < -128.0f) return -128;
    return (int8_t)(q + 0.5f);
}

static void letterbox_rgb(const unsigned char *src, int sw, int sh, int stride,
                          unsigned char *dst)
{
    const int S = MODEL_SIZE;
    float scale = fminf((float)S / sw, (float)S / sh);
    int nw = (int)(sw * scale);
    int nh = (int)(sh * scale);
    int off_x = (S - nw) / 2;
    int off_y = (S - nh) / 2;
    memset(dst, 114, (size_t)S * S * 3);
    for (int y = 0; y < nh; y++) {
        float syf = ((float)y + 0.5f) / scale - 0.5f;
        int sy0 = (int)floorf(syf);
        float fy = syf - sy0;
        if (sy0 < 0) sy0 = 0;
        if (sy0 >= sh - 1) sy0 = sh - 2;
        for (int x = 0; x < nw; x++) {
            float sxf = ((float)x + 0.5f) / scale - 0.5f;
            int sx0 = (int)floorf(sxf);
            float fx = sxf - sx0;
            if (sx0 < 0) sx0 = 0;
            if (sx0 >= sw - 1) sx0 = sw - 2;
            const unsigned char *p00 = src + (size_t)sy0 * stride + (size_t)sx0 * 3;
            const unsigned char *p10 = p00 + 3;
            const unsigned char *p01 = p00 + stride;
            const unsigned char *p11 = p01 + 3;
            unsigned char *d = dst + ((size_t)(y + off_y) * S + (size_t)(x + off_x)) * 3;
            for (int c = 0; c < 3; c++) {
                float top = p00[c] * (1.0f - fx) + p10[c] * fx;
                float bot = p01[c] * (1.0f - fx) + p11[c] * fx;
                d[c] = (unsigned char)(top * (1.0f - fy) + bot * fy + 0.5f);
            }
        }
    }
}

typedef struct {
    int grid;
    int stride;
    int anchor_w[3], anchor_h[3];
    int8_t *data;
    int zp;
    float scale;
} HeadInfo;

static void decode_head(const HeadInfo *head, Box *boxes, int *count)
{
    const int grid = head->grid;
    const int align_c = PROP_BOX_SIZE * 3;
    int8_t thres_i8 = qnt(CONF_THRESHOLD, head->zp, head->scale);
    for (int gy = 0; gy < grid; gy++) {
        for (int gx = 0; gx < grid; gx++) {
            const int8_t *base = head->data + ((size_t)gy * grid + gx) * align_c;
            for (int a = 0; a < 3; a++) {
                const int8_t *p = base + a * PROP_BOX_SIZE;
                int8_t obj_i8 = p[4];
                if (obj_i8 < thres_i8) continue;
                int8_t person_i8 = p[5 + PERSON_CLASS];
                float score = deqnt(obj_i8, head->zp, head->scale) *
                              deqnt(person_i8, head->zp, head->scale);
                if (score < CONF_THRESHOLD) continue;
                float bx = (deqnt(p[0], head->zp, head->scale) * 2.0f - 0.5f + gx) * head->stride;
                float by = (deqnt(p[1], head->zp, head->scale) * 2.0f - 0.5f + gy) * head->stride;
                float bw = deqnt(p[2], head->zp, head->scale) * 2.0f;
                float bh = deqnt(p[3], head->zp, head->scale) * 2.0f;
                bw = bw * bw * head->anchor_w[a];
                bh = bh * bh * head->anchor_h[a];
                float x1 = bx - bw / 2.0f, y1 = by - bh / 2.0f;
                float x2 = bx + bw / 2.0f, y2 = by + bh / 2.0f;
                if (x1 < 0) x1 = 0; if (y1 < 0) y1 = 0;
                if (x2 > MODEL_SIZE) x2 = MODEL_SIZE;
                if (y2 > MODEL_SIZE) y2 = MODEL_SIZE;
                if (x2 <= x1 || y2 <= y1) continue;
                if (*count >= MAX_BOXES) return;
                boxes[*count].x = x1;
                boxes[*count].y = y1;
                boxes[*count].w = x2 - x1;
                boxes[*count].h = y2 - y1;
                boxes[*count].score = score;
                (*count)++;
            }
        }
    }
}

static int cmp_box(const void *a, const void *b)
{
    float sa = ((const Box *)a)->score;
    float sb = ((const Box *)b)->score;
    return sa < sb ? 1 : (sa > sb ? -1 : 0);
}

static float iou(const Box *a, const Box *b)
{
    float ix = fmaxf(0, fminf(a->x + a->w, b->x + b->w) - fmaxf(a->x, b->x));
    float iy = fmaxf(0, fminf(a->y + a->h, b->y + b->h) - fmaxf(a->y, b->y));
    float inter = ix * iy;
    float ua = a->w * a->h + b->w * b->h - inter;
    return ua > 0 ? inter / ua : 0;
}

static int nms(Box *boxes, int count)
{
    qsort(boxes, count, sizeof(Box), cmp_box);
    int keep = 0;
    for (int i = 0; i < count; i++) {
        int suppressed = 0;
        for (int j = 0; j < keep; j++) {
            if (iou(&boxes[i], &boxes[j]) > NMS_THRESHOLD) {
                suppressed = 1;
                break;
            }
        }
        if (!suppressed) boxes[keep++] = boxes[i];
    }
    return keep;
}

static int connect_rock(const char *ip, int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1 ||
        connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int send_json(int fd, const char *json, int len)
{
    const char *p = json;
    int left = len;
    while (left > 0) {
        ssize_t n = send(fd, p, (size_t)left, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        p += n;
        left -= (int)n;
    }
    return 0;
}


static int preview_receive_fds(int fds[2])
{
    int sock = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (sock < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, PREVIEW_FD_SOCKET_PATH, sizeof(addr.sun_path) - 1);
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(sock);
        return -1;
    }
    PreviewFdMessage msg;
    struct iovec iov;
    struct msghdr hdr;
    char control[CMSG_SPACE(2 * sizeof(int))];
    struct cmsghdr *cmsg;
    memset(&msg, 0, sizeof(msg));
    memset(&hdr, 0, sizeof(hdr));
    memset(control, 0, sizeof(control));
    iov.iov_base = &msg;
    iov.iov_len = sizeof(msg);
    hdr.msg_iov = &iov;
    hdr.msg_iovlen = 1;
    hdr.msg_control = control;
    hdr.msg_controllen = sizeof(control);
    ssize_t n = recvmsg(sock, &hdr, 0);
    close(sock);
    if (n != (ssize_t)sizeof(msg) ||
        msg.magic != PREVIEW_FD_MESSAGE_MAGIC ||
        msg.version != PREVIEW_SHM_VERSION ||
        msg.pixel_format != PREVIEW_SHM_PIXFMT_RGB888 ||
        msg.buffer_count != PREVIEW_SHM_BUFFER_COUNT) {
        return -1;
    }
    cmsg = CMSG_FIRSTHDR(&hdr);
    if (!cmsg || cmsg->cmsg_level != SOL_SOCKET ||
        cmsg->cmsg_type != SCM_RIGHTS ||
        cmsg->cmsg_len < CMSG_LEN(2 * sizeof(int))) {
        return -1;
    }
    memcpy(fds, CMSG_DATA(cmsg), 2 * sizeof(int));
    return 0;
}

static long long read_avail_kb(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char line[256];
    long long val = -1;
    if (!f) return -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "MemAvailable:", 13) == 0) {
            val = atoll(line + 13);
            break;
        }
    }
    fclose(f);
    return val;
}

int main(int argc, char **argv)
{
    const char *model_path = "/root/userdata/npu_detect/yolov5n_320.rknn";
    const char *shm_name = "/ai_cam_preview";
    const char *server_ip = "192.168.50.1";
    int server_port = 9010;
    int interval_ms = 300;
    const char *test_image = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--model") && i + 1 < argc) model_path = argv[++i];
        else if (!strcmp(argv[i], "--shm") && i + 1 < argc) shm_name = argv[++i];
        else if (!strcmp(argv[i], "--server-ip") && i + 1 < argc) server_ip = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) server_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--interval-ms") && i + 1 < argc) interval_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--image") && i + 1 < argc) test_image = argv[++i];
        else {
            fprintf(stderr, "usage: %s [--model M] [--shm NAME] [--server-ip IP] [--port P] [--interval-ms N] [--image FILE]\n", argv[0]);
            return 2;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    rknn_context ctx = 0;
    int ret = rknn_init(&ctx, (char *)model_path, 0, 0, NULL);
    if (ret < 0) {
        fprintf(stderr, "rknn_init fail! ret=%d\n", ret);
        return 1;
    }

    rknn_input_output_num io_num;
    ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret != RKNN_SUCC || io_num.n_input != 1 || io_num.n_output != 3) {
        fprintf(stderr, "unexpected io num in=%d out=%d ret=%d\n", io_num.n_input, io_num.n_output, ret);
        return 1;
    }
    rknn_tensor_attr input_attrs[1], output_attrs[3];
    memset(input_attrs, 0, sizeof(input_attrs));
    memset(output_attrs, 0, sizeof(output_attrs));
    input_attrs[0].index = 0;
    ret = rknn_query(ctx, RKNN_QUERY_NATIVE_INPUT_ATTR, &input_attrs[0], sizeof(rknn_tensor_attr));
    if (ret != RKNN_SUCC) { fprintf(stderr, "query input fail\n"); return 1; }
    for (int i = 0; i < io_num.n_output; i++) {
        output_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_NATIVE_NHWC_OUTPUT_ATTR, &output_attrs[i], sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) { fprintf(stderr, "query output %d fail\n", i); return 1; }
    }
    input_attrs[0].type = RKNN_TENSOR_UINT8;
    input_attrs[0].fmt = RKNN_TENSOR_NHWC;
    rknn_tensor_mem *input_mem = rknn_create_mem(ctx, input_attrs[0].size_with_stride);
    if (!input_mem) { fprintf(stderr, "create input mem fail\n"); return 1; }
    if (rknn_set_io_mem(ctx, input_mem, &input_attrs[0]) < 0) { fprintf(stderr, "set input mem fail\n"); return 1; }
    rknn_tensor_mem *output_mems[3];
    for (int i = 0; i < io_num.n_output; i++) {
        output_mems[i] = rknn_create_mem(ctx, output_attrs[i].size_with_stride);
        if (!output_mems[i] || rknn_set_io_mem(ctx, output_mems[i], &output_attrs[i]) < 0) {
            fprintf(stderr, "set output mem %d fail\n", i);
            return 1;
        }
    }

    HeadInfo heads[3];
    const int anchor_list[3][6] = {
        {10, 13, 16, 30, 33, 23},
        {30, 61, 62, 45, 59, 119},
        {116, 90, 156, 198, 373, 326},
    };
    for (int i = 0; i < 3; i++) {
        heads[i].grid = (i == 0) ? 40 : (i == 1 ? 20 : 10);
        heads[i].stride = (i == 0) ? 8 : (i == 1 ? 16 : 32);
        for (int a = 0; a < 3; a++) {
            heads[i].anchor_w[a] = anchor_list[i][a * 2];
            heads[i].anchor_h[a] = anchor_list[i][a * 2 + 1];
        }
        heads[i].data = (int8_t *)output_mems[i]->virt_addr;
        heads[i].zp = output_attrs[i].zp;
        heads[i].scale = output_attrs[i].scale;
    }

    unsigned char *frame_rgb = NULL;
    unsigned char *input_rgb = (unsigned char *)malloc((size_t)MODEL_SIZE * MODEL_SIZE * 3);
    if (!input_rgb) return 1;
    void *shm_map = MAP_FAILED;
    int shm_fd = -1;
    int preview_w = 384, preview_h = 216, preview_stride = 1152;
    unsigned char *stbi_pix = NULL;
    unsigned char *buf_maps[2] = {NULL, NULL};
    int buf_fds[2] = {-1, -1};
    size_t buf_bytes = 0;
    if (!test_image) {
        shm_fd = shm_open(shm_name, O_RDONLY, 0);
        if (shm_fd < 0) { fprintf(stderr, "shm_open %s fail: %s\n", shm_name, strerror(errno)); return 1; }
        struct stat st;
        if (fstat(shm_fd, &st) != 0 || st.st_size < (off_t)sizeof(PreviewShmHeader)) {
            fprintf(stderr, "bad shm size\n");
            return 1;
        }
        shm_map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, shm_fd, 0);
        if (shm_map == MAP_FAILED) { fprintf(stderr, "mmap fail\n"); return 1; }
        PreviewShmHeader *hdr = (PreviewShmHeader *)shm_map;
        if (hdr->magic != PREVIEW_SHM_MAGIC) { fprintf(stderr, "bad shm magic\n"); return 1; }
        preview_w = (int)hdr->width;
        preview_h = (int)hdr->height;
        preview_stride = (int)hdr->stride;
        fprintf(stderr, "preview %dx%d stride=%d buffers=%d\n", preview_w, preview_h, preview_stride, hdr->buffer_count);
        if (preview_receive_fds(buf_fds) != 0) {
            fprintf(stderr, "receive preview fds fail\n");
            return 1;
        }
        buf_bytes = (size_t)preview_h * preview_stride;
        for (int bi = 0; bi < 2; bi++) {
            buf_maps[bi] = mmap(NULL, buf_bytes, PROT_READ, MAP_SHARED, buf_fds[bi], 0);
            if (buf_maps[bi] == MAP_FAILED) {
                fprintf(stderr, "mmap buf %d fail\n", bi);
                return 1;
            }
        }
    } else {
#ifdef NPU_DETECT_TEST_IMAGE
        int w = 0, h = 0, ch = 0;
        stbi_pix = stbi_load(test_image, &w, &h, &ch, 3);
        if (!stbi_pix) { fprintf(stderr, "stbi_load fail: %s\n", test_image); return 1; }
        preview_w = w; preview_h = h; preview_stride = w * 3;
        frame_rgb = stbi_pix;
        fprintf(stderr, "test image %dx%d\n", w, h);
#else
        fprintf(stderr, "--image requires NPU_DETECT_TEST_IMAGE build\n");
        return 1;
#endif
    }

    int sock = -1;
    int loop = 0;
    long long total_infer_ms = 0;
    long long total_frames = 0;
    while (!g_stop) {
        double frame_start = now_ms();
        if (!test_image) {
            PreviewShmHeader *hdr = (PreviewShmHeader *)shm_map;
            uint32_t seq = __atomic_load_n(&hdr->sequence, __ATOMIC_ACQUIRE);
            if ((seq & 1U) || hdr->producer_online == 0) {
                usleep(50000);
                continue;
            }
            uint32_t idx = hdr->active_index % hdr->buffer_count;
            frame_rgb = buf_maps[idx];
        }
        if (!frame_rgb) break;

        letterbox_rgb(frame_rgb, preview_w, preview_h, preview_stride, input_rgb);
        memcpy(input_mem->virt_addr, input_rgb, (size_t)MODEL_SIZE * MODEL_SIZE * 3);
        double t0 = now_ms();
        ret = rknn_run(ctx, NULL);
        double t1 = now_ms();
        if (ret != RKNN_SUCC) { fprintf(stderr, "rknn_run fail! ret=%d\n", ret); break; }
        total_infer_ms += (long long)(t1 - t0);
        total_frames++;

        Box boxes[MAX_BOXES];
        int count = 0;
        for (int i = 0; i < 3; i++) decode_head(&heads[i], boxes, &count);
        count = nms(boxes, count);

        float scale = fminf((float)MODEL_SIZE / preview_w, (float)MODEL_SIZE / preview_h);
        int persons = count;
        char json[512];
        int pos = snprintf(json, sizeof(json),
                           "{\"type\":\"npu\",\"source\":\"local\",\"timestamp\":%.0f,"
                           "\"model\":\"yolov5n-320\",\"success\":true,\"latencyMs\":%.1f,"
                           "\"peopleCount\":%d,\"objects\":%s,\"warning\":%s,"
                           "\"summary\":\"本地NPU检测：人×%d\",\"scene\":\"端侧NPU\"}\n",
                           now_epoch_ms(), t1 - t0, persons,
                           persons ? "[\"person\"]" : "[]",
                           persons ? "true" : "false", persons);
        (void)scale;

        if (sock < 0) sock = connect_rock(server_ip, server_port);
        if (sock >= 0) {
            if (send_json(sock, json, pos) != 0) {
                close(sock);
                sock = -1;
            }
        }

        double total = now_ms() - frame_start;
        fprintf(stderr, "[npu] frame=%lld persons=%d boxes=%d infer=%.1fms loop=%.1fms avail=%lldKB\n",
                (long long)total_frames, persons, count, t1 - t0, total,
                read_avail_kb());
        if (test_image) break;
        int sleep_ms = interval_ms - (int)total;
        if (sleep_ms > 0) usleep((useconds_t)sleep_ms * 1000);
        loop++;
        if (loop % 20 == 0) {
            fprintf(stderr, "[npu] avg infer=%.1fms over %lld frames\n",
                    total_frames ? (double)total_infer_ms / total_frames : 0.0, total_frames);
        }
    }

    if (sock >= 0) close(sock);
    if (stbi_pix) free(stbi_pix);
    for (int bi = 0; bi < 2; bi++) {
        if (buf_maps[bi] && buf_maps[bi] != MAP_FAILED) munmap(buf_maps[bi], buf_bytes);
        if (buf_fds[bi] >= 0) close(buf_fds[bi]);
    }
    if (shm_map != MAP_FAILED) munmap(shm_map, 0);
    if (shm_fd >= 0) close(shm_fd);
    free(input_rgb);
    rknn_destroy(ctx);
    fprintf(stderr, "[npu] exit, frames=%lld avg_infer=%.1fms\n",
            total_frames, total_frames ? (double)total_infer_ms / total_frames : 0.0);
    return 0;
}
