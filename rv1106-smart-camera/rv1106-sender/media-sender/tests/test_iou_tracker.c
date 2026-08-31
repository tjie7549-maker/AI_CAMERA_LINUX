#include <assert.h>
#include <stdio.h>

#include "iou_tracker.h"

static TrackerDetection box(float x, float y, float w, float h)
{
    TrackerDetection value = {x, y, w, h, 0.9f, 0};
    return value;
}

int main(void)
{
    IouTracker tracker;
    iou_tracker_init(&tracker, 0.3f, 2, 2);
    TrackerDetection first[] = {box(0.1f, 0.1f, 0.2f, 0.5f)};
    iou_tracker_update(&tracker, first, 1);
    assert(tracker.count == 1 && tracker.tracks[0].track_id == 1);
    assert(iou_tracker_confirmed_count(&tracker) == 0);
    TrackerDetection moved[] = {box(0.12f, 0.1f, 0.2f, 0.5f)};
    iou_tracker_update(&tracker, moved, 1);
    assert(tracker.count == 1 && tracker.tracks[0].track_id == 1);
    assert(iou_tracker_confirmed_count(&tracker) == 1);
    iou_tracker_update(&tracker, NULL, 0);
    iou_tracker_update(&tracker, moved, 1);
    assert(tracker.count == 1 && tracker.tracks[0].track_id == 1);
    iou_tracker_update(&tracker, NULL, 0);
    iou_tracker_update(&tracker, NULL, 0);
    iou_tracker_update(&tracker, NULL, 0);
    assert(tracker.count == 0);
    TrackerDetection two[] = {box(0.1f, 0.1f, 0.2f, 0.5f), box(0.7f, 0.1f, 0.2f, 0.5f)};
    iou_tracker_update(&tracker, two, 2);
    assert(tracker.count == 2 && tracker.tracks[0].track_id == 2 && tracker.tracks[1].track_id == 3);
    assert(iou_tracker_iou(&two[0], &two[1]) == 0.0f);
    const TrackerDetection full = iou_tracker_from_letterbox(
        0.0f, 70.0f, 320.0f, 180.0f, 0.8f, 0, 320, 320, 384, 216);
    assert(full.x == 0.0f && full.y == 0.0f);
    assert(full.w == 1.0f && full.h == 1.0f);

    IouTracker crossing;
    iou_tracker_init(&crossing, 0.2f, 2, 1);
    TrackerDetection apart[] = {box(0.10f, 0.1f, 0.25f, 0.5f),
                                box(0.65f, 0.1f, 0.25f, 0.5f)};
    TrackerDetection closer[] = {box(0.18f, 0.1f, 0.25f, 0.5f),
                                 box(0.57f, 0.1f, 0.25f, 0.5f)};
    TrackerDetection reversed[] = {box(0.49f, 0.1f, 0.25f, 0.5f),
                                   box(0.26f, 0.1f, 0.25f, 0.5f)};
    iou_tracker_update(&crossing, apart, 2);
    iou_tracker_update(&crossing, closer, 2);
    iou_tracker_update(&crossing, reversed, 2);
    assert(crossing.count == 2);
    assert(crossing.tracks[0].track_id == 1 && crossing.tracks[0].x == 0.26f);
    assert(crossing.tracks[1].track_id == 2 && crossing.tracks[1].x == 0.49f);
    puts("iou_tracker tests passed");
    return 0;
}
