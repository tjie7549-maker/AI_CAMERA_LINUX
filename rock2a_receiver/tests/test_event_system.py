#!/usr/bin/env python3

import json
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1] / "tools" / "qwen_vision"
sys.path.insert(0, str(TOOLS))

from event_engine import EventEngine
from event_protocol import normalize_detection, normalize_track
from event_store import EventStore
from frame_cache import FrameCache, FrameCandidate
from npu_result_server import NpuResultServer, parse_line


def track(track_id: int, x: float = 0.1, confidence: float = 0.9):
    return {
        "track_id": track_id,
        "class": "person",
        "confidence": confidence,
        "bbox": {"x": x, "y": 0.1, "w": 0.2, "h": 0.6},
        "age_frames": 5,
        "missed_frames": 0,
    }


def message(now_ms: int, tracks, frame_id: int = 1):
    return {
        "schema_version": 1,
        "message_type": "track.update",
        "camera_id": "rv1106-01",
        "source": "local_npu",
        "frame_id": frame_id,
        "captured_at_ms": now_ms,
        "produced_at_ms": now_ms,
        "tracks": list(tracks),
    }


class ProtocolTests(unittest.TestCase):
    def test_normalizes_and_clips_bbox(self):
        value = normalize_track(track(7, x=0.9))
        self.assertEqual(value["track_id"], 7)
        self.assertAlmostEqual(value["bbox"]["w"], 0.1)
        self.assertIsNone(normalize_track({"track_id": 0, "bbox": {}}))

    def test_versioned_and_legacy_protocol(self):
        current = normalize_detection(message(1000, [track(1)]), now_ms=1000)
        self.assertEqual(current["tracks"][0]["track_id"], 1)
        legacy = normalize_detection({"type": "npu", "peopleCount": 2,
                                      "timestamp": 1000}, now_ms=1000)
        self.assertTrue(legacy["legacy"])
        self.assertEqual(legacy["people_count"], 2)
        self.assertEqual(legacy["tracks"], [])
        self.assertIsNone(normalize_detection({"schema_version": 99,
                                               "message_type": "track.update"}))
        unsafe = message(1000, [track(1)])
        unsafe["camera_id"] = "../../camera"
        self.assertIsNone(normalize_detection(unsafe, now_ms=1000))

    def test_npu_latest_keeps_tracks_and_legacy_display_fields(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            raw = message(1000, [track(7)], frame_id=99)
            raw.update({"type": "npu", "peopleCount": 1,
                        "sentinelActive": True, "displayAwake": True,
                        "latencyMs": 18.5})
            self.assertEqual(parse_line(json.dumps(raw).encode())["frame_id"], 99)
            server = NpuResultServer("127.0.0.1", 0, root / "npu_latest.json",
                                     root / "npu_display.json", root / "events.log")
            server.handle(raw)
            latest = json.loads((root / "npu_latest.json").read_text())
            display = json.loads((root / "npu_display.json").read_text())
            self.assertEqual(latest["schema_version"], 1)
            self.assertEqual(latest["tracks"][0]["track_id"], 7)
            self.assertEqual(latest["result"]["people_count"], 1)
            self.assertTrue(latest["sentinel_active"])
            self.assertNotIn("tracks", display)


class EventEngineTests(unittest.TestCase):
    def setUp(self):
        self.engine = EventEngine(end_grace_seconds=2, stale_seconds=100,
                                  initial_delay_seconds=3,
                                  cooldown_seconds=120,
                                  long_refresh_seconds=300)

    def test_scenario_a_single_event_survives_miss_and_grace(self):
        out = self.engine.ingest(message(1000, [track(1)]), now_ms=1000)
        event_id = out[0]["event_id"]
        self.assertEqual(out[0]["result"]["event_state"], "new")
        self.assertEqual(self.engine.events[event_id].state, "active")
        self.engine.ingest(message(2000, [], 2), now_ms=2000)
        self.engine.ingest(message(2500, [track(1, 0.12)], 3), now_ms=2500)
        self.assertEqual(self.engine.active_by_camera["rv1106-01"], event_id)
        self.engine.ingest(message(4000, [], 4), now_ms=4000)
        ended = self.engine.tick(now_ms=6100)
        self.assertEqual(ended[-1]["message_type"], "event.end")
        self.assertEqual(self.engine.events[event_id].state, "ended")

    def test_scenario_b_people_changes_stay_one_event(self):
        first = self.engine.ingest(message(1000, [track(1)]), now_ms=1000)[0]
        event_id = first["event_id"]
        self.engine.ingest(message(2000, [track(1), track(2, 0.6)]), now_ms=2000)
        self.engine.ingest(message(3000, [track(2, 0.55)]), now_ms=3000)
        self.engine.ingest(message(4000, []), now_ms=4000)
        self.engine.tick(now_ms=6100)
        event = self.engine.events[event_id]
        self.assertEqual(event.max_people, 2)
        self.assertEqual(set(event.all_tracks), {1, 2})
        self.assertEqual(len(self.engine.events), 1)

    def test_scenario_c_reentry_after_grace_creates_new_event(self):
        old_id = self.engine.ingest(message(1000, [track(1)]), now_ms=1000)[0]["event_id"]
        self.engine.ingest(message(2000, []), now_ms=2000)
        self.engine.tick(now_ms=4100)
        new_id = self.engine.ingest(message(14100, [track(2)], 2), now_ms=14100)[0]["event_id"]
        self.assertNotEqual(old_id, new_id)

    def test_stale_message_cannot_create_event_and_network_loss_interrupts(self):
        stale = self.engine.ingest(message(1000, [track(1)]), now_ms=102000)
        self.assertEqual(stale[0]["message_type"], "health")
        self.assertFalse(self.engine.events)
        self.engine = EventEngine(stale_seconds=2)
        event_id = self.engine.ingest(message(5000, [track(1)]), now_ms=5000)[0]["event_id"]
        ended = self.engine.tick(now_ms=7100)
        self.assertEqual(ended[0]["result"]["event_state"], "interrupted")
        self.assertEqual(self.engine.events[event_id].state, "interrupted")

    def test_cloud_policy_is_event_driven_and_bounded(self):
        event_id = self.engine.ingest(message(1000, [track(1)]), now_ms=1000)[0]["event_id"]
        event = self.engine.events[event_id]
        event.best = FrameCandidate(Path("frame.jpg"), 1000, 1, 0.9, 0.2)
        self.assertEqual(self.engine.due_cloud_events(now_ms=3000), [])
        self.assertEqual(self.engine.due_cloud_events(now_ms=4000), [event])
        event.cloud_inflight = False
        event.last_cloud_at = 4000
        event.cloud_pending = False
        self.engine.ingest(message(5000, [track(1), track(2)], 2), now_ms=5000)
        self.assertEqual(self.engine.due_cloud_events(now_ms=100000), [])
        self.assertEqual(self.engine.due_cloud_events(now_ms=124001), [event])
        event.cloud_inflight = False
        event.last_cloud_at = 124001
        event.cloud_pending = False
        self.assertEqual(self.engine.due_cloud_events(now_ms=424002), [event])
        event.cloud_inflight = False
        event.cloud_state = "failed"
        self.engine.ingest(message(425000, []), now_ms=425000)
        self.engine.tick(now_ms=427100)
        self.assertEqual(event.state, "ended")
        self.assertLess(event.cloud_attempts + 3, 20)


class FrameAndStoreTests(unittest.TestCase):
    def test_frame_score_prefers_clear_exposed_confident_image(self):
        good = FrameCandidate(Path("good"), 1, 1, 0.95, 0.18,
                              brightness=128, variance=100, difference=0.5)
        bad = FrameCandidate(Path("bad"), 1, 2, 0.5, 0.01,
                             brightness=2, variance=1, difference=0.0)
        self.assertGreater(good.score, bad.score)

    def test_nearest_frame_and_promotion(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            ring = root / "ring"
            ring.mkdir()
            (ring / "frame_1100_9.jpg").write_bytes(b"jpeg")
            (ring / "frame_1100_9.json").write_text(json.dumps({
                "receiver_frame_id": 9, "brightness": 120,
                "variance": 80, "difference": 0.2,
            }))
            cache = FrameCache(root / "event_frames", root / "latest.jpg", ring)
            candidate = cache.capture("evt_test_1000_a", 77, 1000, 0.9, 0.2)
            self.assertEqual(candidate.frame_id, 77)
            self.assertEqual(candidate.receiver_frame_id, 9)
            self.assertEqual(candidate.offset_ms, 100)
            promoted = cache.promote("evt_test_1000_a", candidate)
            self.assertTrue(promoted.path.is_file())
            candidate.path.unlink()
            self.assertTrue(promoted.path.is_file())

    def test_sqlite_idempotency_and_restart_recovery(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "events.db"
            store = EventStore(path)
            record = {"event_id": "evt_test_1000_abc", "camera_id": "test",
                      "state": "active", "started_at": 1000, "ended_at": None,
                      "duration_ms": 0, "max_people": 1, "warning": False,
                      "warning_reason": "", "summary": "", "best_image_path": None,
                      "best_frame_id": 0, "frame_match": "approximate"}
            store.upsert_event(record)
            store.upsert_event(record)
            store.upsert_track(record["event_id"], track(1), 1100)
            store.upsert_track(record["event_id"], track(1, confidence=0.95), 1200)
            recognition = {"request_id": "req-1", "event_id": record["event_id"],
                           "source": "cloud", "frame_id": 1,
                           "recognition_backend": "cloud", "success": True,
                           "server_latency_ms": 100, "usage": {}, "result": {}}
            store.record_recognition(recognition)
            store.record_recognition(recognition)
            event = store.get_event(record["event_id"])
            self.assertEqual(len(event["tracks"]), 1)
            self.assertEqual(len(event["recognitions"]), 1)
            store.close()
            reopened = EventStore(path)
            reopened.close_open_events()
            self.assertEqual(reopened.get_event(record["event_id"])["state"],
                             "interrupted")
            reopened.close()


if __name__ == "__main__":
    unittest.main()
