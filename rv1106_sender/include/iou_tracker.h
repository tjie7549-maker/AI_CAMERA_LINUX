#ifndef IOU_TRACKER_H
#define IOU_TRACKER_H

#include <stdint.h>

#define IOU_TRACKER_MAX_OBJECTS 64

typedef struct {
    float x;
    float y;
    float w;
    float h;
    float confidence;
    int class_id;
} TrackerDetection;

typedef struct {
    uint32_t track_id;
    float x;
    float y;
    float w;
    float h;
    float confidence;
    int class_id;
    uint32_t age_frames;
    uint32_t hits;
    uint32_t missed_frames;
} TrackerTrack;

typedef struct {
    TrackerTrack tracks[IOU_TRACKER_MAX_OBJECTS];
    uint32_t count;
    uint32_t next_track_id;
    float iou_threshold;
    uint32_t max_missed;
    uint32_t min_hits;
} IouTracker;

void iou_tracker_init(IouTracker *tracker, float iou_threshold,
                      uint32_t max_missed, uint32_t min_hits);
float iou_tracker_iou(const TrackerDetection *a, const TrackerDetection *b);
TrackerDetection iou_tracker_from_letterbox(float x, float y, float w, float h,
                                             float confidence, int class_id,
                                             int model_width, int model_height,
                                             int image_width, int image_height);
void iou_tracker_update(IouTracker *tracker, const TrackerDetection *detections,
                        uint32_t detection_count);
int iou_tracker_confirmed_count(const IouTracker *tracker);

#endif
