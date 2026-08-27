#!/usr/bin/env python3
"""Event API, event-driven cloud recognition and bounded image persistence."""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from tempfile import NamedTemporaryFile
from typing import Any

from event_engine import EventEngine
from event_protocol import MAX_MESSAGE_BYTES, envelope, normalize_detection
from event_store import EventStore
from frame_cache import FrameCache

EVENT_ID_RE = re.compile(r"^evt_[A-Za-z0-9-]+_[0-9]+_[0-9a-f]+$")


def atomic_json(path: Path, document: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with NamedTemporaryFile("w", encoding="utf-8", dir=path.parent, delete=False) as temporary:
        json.dump(document, temporary, ensure_ascii=False, separators=(",", ":")); temporary.write("\n")
        name = temporary.name
    os.replace(name, path)


class EventServer(ThreadingHTTPServer):
    def __init__(self, address: tuple[str, int], runtime: Path, latest_image: Path,
                 frame_ring_dir: Path | None,
                 cloud_enabled: bool, initial_delay: int, cooldown: int, long_refresh: int,
                 scene_change_threshold: float, end_grace: int,
                 detection_stale: int, max_retries: int) -> None:
        super().__init__(address, Handler)
        self.runtime = runtime
        self.store = EventStore(runtime / "events.db")
        self.store.close_open_events()
        self.engine = EventEngine(end_grace_seconds=end_grace, initial_delay_seconds=initial_delay,
                                  cooldown_seconds=cooldown, long_refresh_seconds=long_refresh,
                                  stale_seconds=detection_stale,
                                  scene_change_threshold=scene_change_threshold)
        self.frames = FrameCache(runtime / "event_frames", latest_image, frame_ring_dir)
        self.latest_path = runtime / "event_latest.json"
        self.recognition_latest_path = runtime / "event_recognition_latest.json"
        self.cloud_enabled, self.max_retries = cloud_enabled, max(0, max_retries)
        self.lock = threading.RLock()
        self.stop_event = threading.Event()
        self.started_at = time.monotonic()
        self.cloud_requests = self.cloud_failures = self.best_frame_updates = 0
        self.npu_messages_invalid = 0
        self.cloud_latency_ms = self.detection_to_event_latency_ms = 0
        self.cloud_client = None
        if cloud_enabled:
            try:
                from qwen_vision_client import QwenVisionClient
                from test_fixed_image import load_qwen_env
                load_qwen_env()
                self.cloud_client = QwenVisionClient()
            except Exception as error:
                print("[event] cloud disabled: %s" % error, flush=True)
                self.cloud_enabled = False
        threading.Thread(target=self._ticker, daemon=True).start()

    def publish(self, message: dict[str, Any]) -> None:
        event_id = message.get("event_id", "")
        if event_id:
            event = self.engine.events.get(event_id)
            if event:
                best = event.best
                record = {"event_id": event.event_id, "camera_id": event.camera_id, "state": event.state,
                          "started_at": event.started_at, "ended_at": event.last_seen_at if event.state in {"ended", "interrupted"} else None,
                          "duration_ms": max(0, event.last_seen_at - event.started_at), "max_people": event.max_people,
                          "warning": event.warning, "warning_reason": event.warning_reason, "summary": event.summary,
                          "best_image_path": str(best.path) if best else None, "best_frame_id": best.frame_id if best else 0,
                          "frame_match": "approximate"}
                self.store.upsert_event(record)
                for track in event.all_tracks.values(): self.store.upsert_track(event.event_id, track, event.last_seen_at)
                self.store.append_update(event.event_id, message["message_type"], message)
        atomic_json(self.latest_path, message)

    def ingest(self, raw: dict[str, Any]) -> tuple[bool, list[dict[str, Any]]]:
        message = normalize_detection(raw)
        if not message:
            self.npu_messages_invalid += 1
            return False, []
        best_track = max(message["tracks"], key=lambda item: item["confidence"], default=None)
        with self.lock:
            output = self.engine.ingest(message)
            active_id = self.engine.active_by_camera.get(message["camera_id"])
            event = self.engine.events.get(active_id) if active_id else None
            if event and best_track:
                bbox = best_track["bbox"]
                candidate = self.frames.capture(event.event_id, message["frame_id"],
                                                message.get("received_at_ms", message["captured_at_ms"]),
                                                best_track["confidence"], bbox["w"] * bbox["h"])
                if self.engine.consider_candidate(event, candidate):
                    event.best = self.frames.promote(event.event_id, event.best)
                    self.best_frame_updates += 1
                    output.append(self.engine._event_message(event, "event.update", message["frame_id"], message["captured_at_ms"]))
            for item in output: self.publish(item)
            for item in output:
                if item.get("message_type") == "event.new":
                    self.detection_to_event_latency_ms = max(
                        0, int(item["produced_at_ms"]) -
                        int(message.get("received_at_ms", message["captured_at_ms"]))
                    )
        return True, output

    def _ticker(self) -> None:
        while not self.stop_event.wait(1):
            with self.lock:
                for message in self.engine.tick(): self.publish(message)
                if self.cloud_enabled and self.cloud_client:
                    for event in self.engine.due_cloud_events():
                        threading.Thread(target=self._recognize_event, args=(event.event_id,), daemon=True).start()

    def _recognize_event(self, event_id: str) -> None:
        with self.lock:
            event = self.engine.events.get(event_id)
            if not event or not event.best: return
            image_path, frame_id = event.best.path, event.best.frame_id
            captured_at_ms = event.best.captured_at_ms
            frame_offset_ms = event.best.offset_ms
            try:
                image_bytes = image_path.read_bytes()
            except OSError as error:
                self.cloud_failures += 1
                event.cloud_inflight = False
                event.cloud_state = "failed"
                event.cloud_attempts += 1
                event.last_cloud_at = int(time.time() * 1000)
                event.warning_reason = "best frame unavailable: %s" % str(error)[:120]
                self.publish(self.engine._event_message(event, "event.update", frame_id,
                                                       captured_at_ms))
                return
        request_id = "event-%d-%s" % (int(time.time() * 1000), event_id.rsplit("_", 1)[-1])
        started = time.monotonic()
        try:
            result = None
            for attempt in range(self.max_retries + 1):
                with self.lock:
                    self.cloud_requests += 1
                try:
                    result = self.cloud_client.analyze_image_bytes(
                        image_bytes, "image/jpeg")
                except Exception:
                    if attempt >= self.max_retries:
                        raise
                    time.sleep(min(4, 2 ** attempt))
                    continue
                if result.get("success") or attempt >= self.max_retries:
                    break
                time.sleep(min(4, 2 ** attempt))
            assert result is not None
            document = envelope("recognition.result", event.camera_id, source="cloud",
                                frame_id=frame_id, captured_at_ms=captured_at_ms,
                                event_id=event_id, request_id=request_id, result=result.get("result", {}),
                                frame_match="approximate", frame_offset_ms=frame_offset_ms,
                                recognition_backend="cloud", success=bool(result.get("success")),
                                model=result.get("model", "qwen3-vl-flash"), latency_ms=result.get("latency_ms", 0),
                                server_latency_ms=int((time.monotonic() - started) * 1000), usage=result.get("usage", {}),
                                error_type=result.get("error_type", ""), error_message=result.get("error_message", ""))
        except Exception as error:
            document = envelope("recognition.result", event.camera_id, source="cloud",
                                frame_id=frame_id, captured_at_ms=captured_at_ms,
                                event_id=event_id, request_id=request_id,
                                frame_match="approximate", frame_offset_ms=frame_offset_ms,
                                recognition_backend="cloud", success=False,
                                server_latency_ms=int((time.monotonic() - started) * 1000),
                                error={"type": "cloud", "message": str(error)[:200]})
        with self.lock:
            self.cloud_latency_ms = int(document.get(
                "server_latency_ms", (time.monotonic() - started) * 1000))
            if not document.get("success"):
                self.cloud_failures += 1
        self.store.record_recognition(document)
        self.store.append_update(event_id, "recognition.result", document)
        atomic_json(self.recognition_latest_path, document)
        with self.lock:
            current = self.engine.events.get(event_id)
            if current:
                semantic = document.get("result", {})
                current.warning = bool(semantic.get("warning", False))
                current.warning_reason = str(semantic.get("warning_reason", ""))
                current.summary = str(semantic.get("summary", ""))
                current.cloud_state = "complete" if document.get("success") else "failed"
                current.cloud_inflight = False
                current.last_cloud_at = int(time.time() * 1000)
                current.cloud_attempts += 1
                self.publish(self.engine._event_message(current, "event.update", frame_id,
                                                       current.best.captured_at_ms if current.best else 0))

    def metrics(self) -> str:
        with self.lock:
            active = len(self.engine.active_by_camera)
            tracks = sum(len(self.engine.events[key].tracks)
                         for key in self.engine.active_by_camera.values())
            messages_total = self.engine.messages_total
            messages_invalid = self.engine.messages_invalid + self.npu_messages_invalid
            cloud_requests = self.cloud_requests
            cloud_failures = self.cloud_failures
            cloud_latency = self.cloud_latency_ms
            event_latency = self.detection_to_event_latency_ms
            best_updates = self.best_frame_updates
        free = shutil.disk_usage(self.runtime).free
        rtsp_frames = 0
        if self.frames.ring_dir and self.frames.ring_dir.is_dir():
            metadata = sorted(self.frames.ring_dir.glob("frame_*.json"))
            if metadata:
                try: rtsp_frames = int(json.loads(metadata[-1].read_text()).get("receiver_frame_id", 0))
                except (OSError, ValueError, json.JSONDecodeError): pass
        values = {"ai_camera_events_total": self.store.count_events(), "ai_camera_active_events": active,
                  "ai_camera_tracks_active": tracks, "ai_camera_npu_messages_total": messages_total,
                  "ai_camera_npu_messages_invalid_total": messages_invalid,
                  "ai_camera_cloud_requests_total": cloud_requests, "ai_camera_cloud_failures_total": cloud_failures,
                  "ai_camera_cloud_latency_ms": cloud_latency,
                  "ai_camera_detection_to_event_latency_ms": event_latency,
                  "ai_camera_rtsp_frames_total": rtsp_frames,
                  "ai_camera_best_frame_updates_total": best_updates, "ai_camera_storage_free_bytes": free,
                  "ai_camera_service_uptime_seconds": int(time.monotonic() - self.started_at)}
        return "".join("%s %s\n" % item for item in values.items())

    def close_resources(self) -> None:
        self.stop_event.set()
        self.server_close()
        self.store.close()


class Handler(BaseHTTPRequestHandler):
    server: EventServer
    def log_message(self, *_: Any) -> None: pass
    def send_json(self, status: int, payload: Any) -> None:
        data = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode()
        self.send_response(status); self.send_header("Content-Type", "application/json; charset=utf-8"); self.send_header("Content-Length", str(len(data))); self.end_headers(); self.wfile.write(data)
    def read_json(self, allow_empty: bool = False) -> dict[str, Any] | None:
        try: length = int(self.headers.get("Content-Length", "0"))
        except ValueError: return None
        if length == 0 and allow_empty: return {}
        if length <= 0 or length > MAX_MESSAGE_BYTES: return None
        try:
            value = json.loads(self.rfile.read(length).decode("utf-8"))
            return value if isinstance(value, dict) else None
        except (UnicodeDecodeError, json.JSONDecodeError): return None
    def do_GET(self) -> None:
        path, _, query = self.path.partition("?")
        if path == "/health":
            with self.server.lock:
                active_events = len(self.server.engine.active_by_camera)
            self.send_json(200, {"status": "ok", "active_events": active_events,
                                 "cloud_enabled": self.server.cloud_enabled}); return
        if path == "/metrics":
            data = self.server.metrics().encode(); self.send_response(200); self.send_header("Content-Type", "text/plain; version=0.0.4"); self.send_header("Content-Length", str(len(data))); self.end_headers(); self.wfile.write(data); return
        if path == "/events":
            from urllib.parse import parse_qs
            params = parse_qs(query)
            try: limit = int(params.get("limit", [20])[0])
            except (TypeError, ValueError): self.send_json(400, {"error": "invalid limit"}); return
            before = params.get("before", [None])[0]; state = params.get("state", [None])[0]
            if before and not before.isdigit():
                self.send_json(400, {"error": "invalid before"}); return
            if state and state not in {"new", "active", "ending", "ended", "interrupted"}:
                self.send_json(400, {"error": "invalid state"}); return
            events = self.server.store.list_events(
                limit, int(before) if before and before.isdigit() else None, state)
            with self.server.lock:
                for item in events:
                    current = self.server.engine.events.get(item["event_id"])
                    if current:
                        item.update({
                            "current_people": current.current_people,
                            "track_count": len(current.tracks),
                            "cloud_state": current.cloud_state,
                            "duration_ms": max(0, current.last_seen_at - current.started_at),
                        })
            self.send_json(200, {"events": events}); return
        parts = path.strip("/").split("/")
        if len(parts) in {2, 3} and parts[0] == "events" and EVENT_ID_RE.fullmatch(parts[1]):
            event = self.server.store.get_event(parts[1])
            if not event: self.send_json(404, {"error": "event not found"}); return
            if len(parts) == 3 and parts[2] == "image":
                image = Path(event.get("best_image_path") or "")
                allowed = (self.server.runtime / "event_best").resolve()
                try: image_allowed = image.resolve().is_relative_to(allowed)
                except (OSError, ValueError): image_allowed = False
                if not image_allowed or not image.is_file(): self.send_json(404, {"error": "image not found"}); return
                data = image.read_bytes(); self.send_response(200); self.send_header("Content-Type", "image/jpeg"); self.send_header("Content-Length", str(len(data))); self.end_headers(); self.wfile.write(data); return
            self.send_json(200, event); return
        self.send_json(404, {"error": "not found"})
    def do_POST(self) -> None:
        if self.path == "/ingest":
            payload = self.read_json()
            if payload is None:
                self.server.npu_messages_invalid += 1
                self.send_json(400, {"error": "invalid or too large json"}); return
            accepted, output = self.server.ingest(payload); self.send_json(202 if accepted else 400, {"accepted": accepted, "events": output}); return
        parts = self.path.strip("/").split("/")
        if len(parts) == 3 and parts[0] == "events" and parts[2] == "save" and EVENT_ID_RE.fullmatch(parts[1]):
            if self.read_json(allow_empty=True) is None:
                self.send_json(400, {"error": "invalid or too large json"}); return
            event = self.server.store.get_event(parts[1]); source = Path(event.get("best_image_path") or "") if event else Path()
            allowed = (self.server.runtime / "event_best").resolve()
            try: source_allowed = source.resolve().is_relative_to(allowed)
            except (OSError, ValueError): source_allowed = False
            if not event or not source_allowed or not source.is_file(): self.send_json(404, {"error": "event image not found"}); return
            target = self.server.runtime / "saved_results" / "events" / parts[1]; target.mkdir(parents=True, exist_ok=True)
            already_saved = (target / "event.json").is_file()
            shutil.copyfile(source, target / "best.jpg"); atomic_json(target / "event.json", event); self.send_json(200, {"success": True, "already_saved": already_saved, "saved_relative_path": str(target.relative_to(self.server.runtime))}); return
        self.send_json(404, {"error": "not found"})


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="192.168.50.1"); parser.add_argument("--port", type=int, default=9011)
    parser.add_argument("--runtime", type=Path, required=True); parser.add_argument("--latest-image", type=Path, required=True)
    parser.add_argument("--frame-ring-dir", type=Path)
    parser.add_argument("--cloud-enabled", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--initial-delay-seconds", type=int, default=3); parser.add_argument("--cooldown-seconds", type=int, default=120)
    parser.add_argument("--long-refresh-seconds", type=int, default=300)
    parser.add_argument("--scene-change-threshold", type=float, default=0.15)
    parser.add_argument("--end-grace-seconds", type=int, default=10)
    parser.add_argument("--detection-stale-seconds", type=int, default=5)
    parser.add_argument("--cloud-max-retries", type=int, default=2)
    args = parser.parse_args()
    if args.initial_delay_seconds < 0 or args.cooldown_seconds < 1 or args.long_refresh_seconds < 1:
        parser.error("cloud timing values are out of range")
    if args.end_grace_seconds < 0 or args.detection_stale_seconds < 1:
        parser.error("event timeout values are out of range")
    if not 0.0 <= args.scene_change_threshold <= 1.0:
        parser.error("--scene-change-threshold must be between 0 and 1")
    server = EventServer((args.host, args.port), args.runtime, args.latest_image, args.frame_ring_dir,
                         args.cloud_enabled, args.initial_delay_seconds, args.cooldown_seconds,
                         args.long_refresh_seconds, args.scene_change_threshold,
                         args.end_grace_seconds, args.detection_stale_seconds,
                         args.cloud_max_retries)
    try: server.serve_forever(poll_interval=0.5)
    except KeyboardInterrupt: pass
    finally: server.close_resources()
    return 0

if __name__ == "__main__": raise SystemExit(main())
