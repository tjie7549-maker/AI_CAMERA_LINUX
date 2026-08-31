#include "iou_tracker.h"

#include <string.h>

static float minf(float a, float b) {
    return a < b ? a : b;
}
static float maxf(float a, float b) {
    return a > b ? a : b;
}

float iou_tracker_iou(const TrackerDetection *a, const TrackerDetection *b) {
    const float ix = maxf(0.0f, minf(a->x + a->w, b->x + b->w) - maxf(a->x, b->x));
    const float iy = maxf(0.0f, minf(a->y + a->h, b->y + b->h) - maxf(a->y, b->y));
    const float inter = ix * iy;
    const float union_area = a->w * a->h + b->w * b->h - inter;
    return union_area > 0.0f ? inter / union_area : 0.0f;
}

static float clamp01(float value) {
    if (value < 0.0f)
        return 0.0f;
    if (value > 1.0f)
        return 1.0f;
    return value;
}

TrackerDetection iou_tracker_from_letterbox(float x, float y, float w, float h, float confidence,
                                            int class_id, int model_width, int model_height,
                                            int image_width, int image_height) {
    TrackerDetection detection = {0};
    if (model_width <= 0 || model_height <= 0 || image_width <= 0 || image_height <= 0)
        return detection;
    const float scale_x = (float)model_width / image_width;
    const float scale_y = (float)model_height / image_height;
    const float scale = minf(scale_x, scale_y);
    const float pad_x = ((float)model_width - image_width * scale) * 0.5f;
    const float pad_y = ((float)model_height - image_height * scale) * 0.5f;
    const float x1 = clamp01((x - pad_x) / scale / image_width);
    const float y1 = clamp01((y - pad_y) / scale / image_height);
    const float x2 = clamp01((x + w - pad_x) / scale / image_width);
    const float y2 = clamp01((y + h - pad_y) / scale / image_height);
    detection.x = x1;
    detection.y = y1;
    detection.w = maxf(0.0f, x2 - x1);
    detection.h = maxf(0.0f, y2 - y1);
    detection.confidence = confidence;
    detection.class_id = class_id;
    return detection;
}

void iou_tracker_init(IouTracker *tracker, float iou_threshold, uint32_t max_missed,
                      uint32_t min_hits) {
    memset(tracker, 0, sizeof(*tracker));
    tracker->next_track_id = 1;
    tracker->iou_threshold = iou_threshold;
    tracker->max_missed = max_missed;
    tracker->min_hits = min_hits ? min_hits : 1;
}

static TrackerDetection as_detection(const TrackerTrack *track) {
    TrackerDetection detection = {
        .x = track->x,
        .y = track->y,
        .w = track->w,
        .h = track->h,
        .confidence = track->confidence,
        .class_id = track->class_id,
    };
    return detection;
}

static void add_track(IouTracker *tracker, const TrackerDetection *detection) {
    if (tracker->count >= IOU_TRACKER_MAX_OBJECTS)
        return;
    TrackerTrack *track = &tracker->tracks[tracker->count++];
    memset(track, 0, sizeof(*track));
    track->track_id = tracker->next_track_id++;
    track->x = detection->x;
    track->y = detection->y;
    track->w = detection->w;
    track->h = detection->h;
    track->confidence = detection->confidence;
    track->class_id = detection->class_id;
    track->age_frames = 1;
    track->hits = 1;
}

void iou_tracker_update(IouTracker *tracker, const TrackerDetection *detections,
                        uint32_t detection_count) {
    uint8_t matched_tracks[IOU_TRACKER_MAX_OBJECTS] = {0};
    uint8_t matched_detections[IOU_TRACKER_MAX_OBJECTS] = {0};
    if (detection_count > IOU_TRACKER_MAX_OBJECTS)
        detection_count = IOU_TRACKER_MAX_OBJECTS;

    for (;;) {
        float best_iou = tracker->iou_threshold;
        int best_track = -1;
        int best_detection = -1;
        for (uint32_t ti = 0; ti < tracker->count; ++ti) {
            if (matched_tracks[ti])
                continue;
            const TrackerDetection previous = as_detection(&tracker->tracks[ti]);
            for (uint32_t di = 0; di < detection_count; ++di) {
                if (matched_detections[di] || detections[di].class_id != previous.class_id)
                    continue;
                const float overlap = iou_tracker_iou(&previous, &detections[di]);
                if (overlap >= best_iou) {
                    best_iou = overlap;
                    best_track = (int)ti;
                    best_detection = (int)di;
                }
            }
        }
        if (best_track < 0)
            break;
        TrackerTrack *track = &tracker->tracks[best_track];
        const TrackerDetection *detection = &detections[best_detection];
        track->x = detection->x;
        track->y = detection->y;
        track->w = detection->w;
        track->h = detection->h;
        track->confidence = detection->confidence;
        track->age_frames++;
        track->hits++;
        track->missed_frames = 0;
        matched_tracks[best_track] = 1;
        matched_detections[best_detection] = 1;
    }

    for (uint32_t ti = 0; ti < tracker->count; ++ti) {
        if (!matched_tracks[ti]) {
            tracker->tracks[ti].age_frames++;
            tracker->tracks[ti].missed_frames++;
        }
    }
    for (uint32_t di = 0; di < detection_count; ++di)
        if (!matched_detections[di])
            add_track(tracker, &detections[di]);

    uint32_t kept = 0;
    for (uint32_t ti = 0; ti < tracker->count; ++ti) {
        if (tracker->tracks[ti].missed_frames <= tracker->max_missed)
            tracker->tracks[kept++] = tracker->tracks[ti];
    }
    tracker->count = kept;
}

int iou_tracker_confirmed_count(const IouTracker *tracker) {
    int count = 0;
    for (uint32_t i = 0; i < tracker->count; ++i) {
        if (tracker->tracks[i].hits >= tracker->min_hits && tracker->tracks[i].missed_frames == 0)
            ++count;
    }
    return count;
}
