#ifndef PREVIEW_SHM_PROTOCOL_H
#define PREVIEW_SHM_PROTOCOL_H

#include <stdint.h>

#define PREVIEW_SHM_MAGIC 0x50525657U
#define PREVIEW_SHM_VERSION 1U
#define PREVIEW_SHM_BUFFER_COUNT 2U
#define PREVIEW_SHM_PIXFMT_RGB888 1U
#define PREVIEW_FD_MESSAGE_MAGIC 0x50524644U
#define PREVIEW_FD_SOCKET_PATH "/tmp/ai_cam_preview.sock"

/*
 * Shared by the RV1106 producer and the Qt consumer.  Writers publish an odd
 * sequence while filling a buffer and an even sequence after publication.
 */
typedef struct __attribute__((aligned(64))) {
    uint32_t magic;
    uint32_t version;
    uint32_t header_size;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t pixel_format;
    uint32_t buffer_count;
    uint32_t active_index;
    uint32_t producer_online;
    uint32_t sequence;
    uint64_t last_frame_id;
    uint64_t last_frame_monotonic_ns;
    uint32_t reserved[15];
} PreviewShmHeader;

#define PREVIEW_SHM_IMAGE_BYTES(width, height, stride) ((uint64_t)(height) * (stride))
#define PREVIEW_SHM_TOTAL_BYTES(width, height, stride) \
    (sizeof(PreviewShmHeader) +                        \
     PREVIEW_SHM_BUFFER_COUNT * PREVIEW_SHM_IMAGE_BYTES((width), (height), (stride)))

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t pixel_format;
    uint32_t buffer_count;
    uint32_t reserved;
} PreviewFdMessage;

#endif
