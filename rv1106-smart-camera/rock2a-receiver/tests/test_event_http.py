#!/usr/bin/env python3

import json
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1] / "tools" / "qwen_vision"
sys.path.insert(0, str(TOOLS))

from event_protocol import MAX_MESSAGE_BYTES
from event_service import EventServer


class EventHttpTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.runtime = Path(self.temporary.name)
        latest = self.runtime / "latest.jpg"
        latest.write_bytes(b"jpeg")
        self.server = EventServer(
            ("127.0.0.1", 0), self.runtime, latest, None, False, 3, 120, 300, 0.15, 10, 30, 1
        )
        self.thread = threading.Thread(
            target=self.server.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True
        )
        self.thread.start()
        self.base = "http://127.0.0.1:%d" % self.server.server_address[1]

    def tearDown(self):
        self.server.shutdown()
        self.thread.join(timeout=2)
        self.server.close_resources()
        self.temporary.cleanup()

    def request(self, path, method="GET", payload=None, raw=None):
        data = (
            raw
            if raw is not None
            else (json.dumps(payload).encode() if payload is not None else None)
        )
        request = urllib.request.Request(
            self.base + path, data=data, method=method, headers={"Content-Type": "application/json"}
        )
        try:
            with urllib.request.urlopen(request, timeout=2) as response:
                return response.status, response.read(), response.headers
        except urllib.error.HTTPError as error:
            return error.code, error.read(), error.headers

    def create_event(self):
        now = int(time.time() * 1000)
        payload = {
            "schema_version": 1,
            "message_type": "track.update",
            "camera_id": "rv1106-01",
            "source": "local_npu",
            "frame_id": 12,
            "captured_at_ms": now,
            "produced_at_ms": now,
            "tracks": [
                {
                    "track_id": 1,
                    "class": "person",
                    "confidence": 0.9,
                    "bbox": {"x": 0.1, "y": 0.1, "w": 0.2, "h": 0.6},
                    "age_frames": 4,
                    "missed_frames": 0,
                }
            ],
        }
        status, body, _ = self.request("/ingest", "POST", payload=payload)
        self.assertEqual(status, 202)
        return json.loads(body)["events"][0]["event_id"]

    def test_health_metrics_query_image_and_idempotent_save(self):
        event_id = self.create_event()
        status, body, _ = self.request("/health")
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(body)["status"], "ok")
        status, body, _ = self.request("/metrics")
        metrics = body.decode()
        self.assertEqual(status, 200)
        for name in (
            "ai_camera_events_total",
            "ai_camera_tracks_active",
            "ai_camera_cloud_latency_ms",
            "ai_camera_storage_free_bytes",
        ):
            self.assertIn(name, metrics)
        status, body, _ = self.request("/events?state=active&limit=1")
        event = json.loads(body)["events"][0]
        self.assertEqual(event["event_id"], event_id)
        self.assertEqual(event["current_people"], 1)
        status, image, headers = self.request("/events/%s/image" % event_id)
        self.assertEqual((status, image, headers.get_content_type()), (200, b"jpeg", "image/jpeg"))
        status, body, _ = self.request("/events/%s/save" % event_id, "POST", payload={})
        self.assertFalse(json.loads(body)["already_saved"])
        status, body, _ = self.request("/events/%s/save" % event_id, "POST", payload={})
        self.assertTrue(json.loads(body)["already_saved"])

    def test_rejects_bad_queries_ids_json_and_oversized_body(self):
        self.assertEqual(self.request("/events?limit=bad")[0], 400)
        self.assertEqual(self.request("/events?before=bad")[0], 400)
        self.assertEqual(self.request("/events?state=wrong")[0], 400)
        self.assertEqual(self.request("/events/../../etc/passwd")[0], 404)
        self.assertEqual(self.request("/ingest", "POST", raw=b"{")[0], 400)
        oversized = b"{" + b" " * MAX_MESSAGE_BYTES + b"}"
        self.assertEqual(self.request("/ingest", "POST", raw=oversized)[0], 400)

    def test_cloud_failure_retries_without_breaking_event(self):
        event_id = self.create_event()

        class FailingCloud:
            calls = 0

            def analyze_image_bytes(self, *_):
                self.calls += 1
                raise RuntimeError("offline")

        cloud = FailingCloud()
        self.server.cloud_client = cloud
        event = self.server.engine.events[event_id]
        event.cloud_inflight = True
        self.server._recognize_event(event_id)
        self.assertEqual(cloud.calls, 2)
        self.assertEqual(self.server.cloud_requests, 2)
        self.assertEqual(self.server.cloud_failures, 1)
        self.assertFalse(event.cloud_inflight)
        self.assertEqual(event.cloud_state, "failed")
        self.assertEqual(event.state, "active")


if __name__ == "__main__":
    unittest.main()
