"""硬件无关的事件状态机。

把连续的本地人形轨迹归并为开始、更新、结束或中断事件；只维护状态，
不直接访问网络、文件或模型，因而可独立单元测试。
"""

from __future__ import annotations

import secrets
import time
from dataclasses import dataclass, field
from typing import Any

from event_protocol import envelope


@dataclass
class Event:
    event_id: str
    camera_id: str
    started_at: int
    state: str = "new"
    tracks: dict[int, dict[str, Any]] = field(default_factory=dict)
    all_tracks: dict[int, dict[str, Any]] = field(default_factory=dict)
    current_people: int = 0
    max_people: int = 0
    last_seen_at: int = 0
    ending_since: int | None = None
    best: Any = None
    last_cloud_at: int = 0
    cloud_inflight: bool = False
    cloud_attempts: int = 0
    cloud_pending: bool = True
    cloud_state: str = "pending"
    warning: bool = False
    warning_reason: str = ""
    summary: str = ""
    last_update_at: int = 0


class EventEngine:
    def __init__(
        self,
        end_grace_seconds: int = 10,
        stale_seconds: int = 5,
        initial_delay_seconds: int = 3,
        cooldown_seconds: int = 120,
        long_refresh_seconds: int = 300,
        scene_change_threshold: float = 0.15,
    ) -> None:
        self.events: dict[str, Event] = {}
        self.active_by_camera: dict[str, str] = {}
        self.end_grace_ms = end_grace_seconds * 1000
        self.stale_ms = stale_seconds * 1000
        self.initial_delay_ms = initial_delay_seconds * 1000
        self.cooldown_ms = cooldown_seconds * 1000
        self.long_refresh_ms = long_refresh_seconds * 1000
        self.scene_change_threshold = scene_change_threshold
        self.messages_total = 0
        self.messages_invalid = 0
        self.last_message_by_camera: dict[str, int] = {}

    def _event_message(
        self, event: Event, kind: str, frame_id: int = 0, captured_at_ms: int = 0
    ) -> dict[str, Any]:
        best = event.best
        result = {
            "event_state": event.state,
            "current_people": event.current_people,
            "max_people": event.max_people,
            "track_count": len(event.tracks),
            "track_ids": sorted(event.all_tracks),
            "duration_ms": max(0, (event.last_seen_at or event.started_at) - event.started_at),
            "best_frame_id": best.frame_id if best else 0,
            "best_frame_offset_ms": best.offset_ms if best else 0,
            "frame_match": "approximate",
            "cloud_state": event.cloud_state,
            "warning": event.warning,
            "warning_reason": event.warning_reason,
            "summary": event.summary,
        }
        return envelope(
            kind,
            event.camera_id,
            frame_id=frame_id,
            captured_at_ms=captured_at_ms,
            event_id=event.event_id,
            tracks=list(event.tracks.values()),
            result=result,
        )

    def ingest(
        self, message: dict[str, Any], candidate: Any = None, now_ms: int | None = None
    ) -> list[dict[str, Any]]:
        self.messages_total += 1
        now = int(time.time() * 1000) if now_ms is None else now_ms
        captured = int(message["captured_at_ms"])
        received = int(message.get("received_at_ms", captured))
        if now - received > self.stale_ms:
            self.messages_invalid += 1
            return [
                envelope(
                    "health",
                    message["camera_id"],
                    health={"status": "stale_detection", "age_ms": now - received},
                )
            ]
        tracks = [track for track in message["tracks"] if track["missed_frames"] == 0]
        camera_id = message["camera_id"]
        self.last_message_by_camera[camera_id] = now
        active_id = self.active_by_camera.get(camera_id)
        event = self.events.get(active_id) if active_id else None
        output: list[dict[str, Any]] = []
        created = bool(tracks and event is None)
        if created:
            event_id = "evt_%s_%d_%s" % (camera_id, now, secrets.token_hex(3))
            event = Event(
                event_id,
                camera_id,
                now,
                state="new",
                last_seen_at=now,
                last_update_at=now,
            )
            self.events[event_id] = event
            self.active_by_camera[camera_id] = event_id
        if event is None:
            return output
        if tracks:
            previous_people = event.current_people
            changed = event.state != "active" or {item["track_id"] for item in tracks} != set(
                event.tracks
            )
            event.ending_since = None
            event.last_seen_at = now
            event.tracks = {item["track_id"]: item for item in tracks}
            event.all_tracks.update(event.tracks)
            event.current_people = len(tracks)
            event.max_people = max(event.max_people, event.current_people)
            if previous_people != event.current_people:
                event.cloud_pending = True
            if created:
                output.append(
                    self._event_message(event, "event.new", message["frame_id"], captured)
                )
                event.state = "active"
            elif changed:
                event.state = "active"
                event.last_update_at = now
                output.append(
                    self._event_message(event, "event.update", message["frame_id"], captured)
                )
            else:
                event.state = "active"
        elif event.state == "active":
            event.state = "ending"
            event.current_people = 0
            event.ending_since = now
            event.last_update_at = now
            output.append(self._event_message(event, "event.update", message["frame_id"], captured))
        return output

    def consider_candidate(self, event: Event, candidate: Any) -> bool:
        if candidate is None:
            return False
        if event.best is None:
            event.best = candidate
            event.cloud_pending = True
            return True
        if candidate.score > event.best.score:
            if candidate.score - event.best.score >= self.scene_change_threshold:
                event.cloud_pending = True
            event.best = candidate
            return True
        return False

    def tick(self, now_ms: int | None = None) -> list[dict[str, Any]]:
        now = int(time.time() * 1000) if now_ms is None else now_ms
        output = []
        for camera_id, event_id in list(self.active_by_camera.items()):
            event = self.events[event_id]
            last_message = self.last_message_by_camera.get(camera_id, event.started_at)
            if event.state in {"active", "ending"} and now - last_message >= self.stale_ms:
                event.state = "interrupted"
                event.last_seen_at = now
                event.current_people = 0
                output.append(self._event_message(event, "event.end"))
                del self.active_by_camera[camera_id]
                continue
            if (
                event.state == "ending"
                and event.ending_since is not None
                and now - event.ending_since >= self.end_grace_ms
            ):
                event.state = "ended"
                event.last_seen_at = now
                output.append(self._event_message(event, "event.end"))
                del self.active_by_camera[camera_id]
                continue
            if event.state in {"active", "ending"} and now - event.last_update_at >= 5000:
                event.last_update_at = now
                output.append(self._event_message(event, "event.update"))
        return output

    def due_cloud_events(self, now_ms: int | None = None) -> list[Event]:
        now = int(time.time() * 1000) if now_ms is None else now_ms
        due = []
        for event_id in self.active_by_camera.values():
            event = self.events[event_id]
            if event.cloud_inflight or not event.best:
                continue
            age = now - event.started_at
            initial = event.last_cloud_at == 0 and age >= self.initial_delay_ms
            changed = (
                event.last_cloud_at > 0
                and event.cloud_pending
                and now - event.last_cloud_at >= self.cooldown_ms
            )
            refresh = event.last_cloud_at > 0 and now - event.last_cloud_at >= self.long_refresh_ms
            if initial or changed or refresh:
                due.append(event)
                event.cloud_inflight = True
                event.cloud_pending = False
                event.cloud_state = "running"
        return due
