"""Versioned, defensive wire protocol for local detections and event messages."""

from __future__ import annotations

import time
import re
from typing import Any

SCHEMA_VERSION = 1
CAMERA_ID_DEFAULT = "rv1106-01"
MAX_MESSAGE_BYTES = 65536
MAX_TRACKS = 64
CAMERA_ID_RE = re.compile(r"^[A-Za-z0-9-]{1,64}$")


def _integer(value: Any, default: int = 0) -> int:
    try:
        value = int(value)
    except (TypeError, ValueError):
        return default
    return value if value >= 0 else default


def _number(value: Any, default: float = 0.0) -> float:
    try:
        value = float(value)
    except (TypeError, ValueError):
        return default
    return value


def _clamp(value: Any) -> float:
    return max(0.0, min(1.0, _number(value)))


def normalize_track(track: Any) -> dict[str, Any] | None:
    if not isinstance(track, dict):
        return None
    track_id = _integer(track.get("track_id"))
    if not track_id:
        return None
    bbox = track.get("bbox")
    if not isinstance(bbox, dict):
        return None
    x, y = _clamp(bbox.get("x")), _clamp(bbox.get("y"))
    w, h = _clamp(bbox.get("w")), _clamp(bbox.get("h"))
    w, h = min(w, 1.0 - x), min(h, 1.0 - y)
    if w <= 0.0 or h <= 0.0:
        return None
    return {
        "track_id": track_id,
        "class": "person" if track.get("class", "person") == "person" else str(track.get("class")),
        "confidence": _clamp(track.get("confidence")),
        "bbox": {"x": x, "y": y, "w": w, "h": h},
        "age_frames": _integer(track.get("age_frames")),
        "missed_frames": _integer(track.get("missed_frames")),
    }


def normalize_detection(message: Any, now_ms: int | None = None) -> dict[str, Any] | None:
    """Accept schema v1 or legacy peopleCount NPU messages without trusting extras."""
    if not isinstance(message, dict):
        return None
    if message.get("type") not in {None, "npu"} and message.get("message_type") not in {"detection", "track.update"}:
        return None
    now_ms = int(time.time() * 1000) if now_ms is None else now_ms
    schema_version = _integer(message.get("schema_version"), 0)
    if schema_version not in {0, SCHEMA_VERSION}:
        return None
    camera_id = str(message.get("camera_id") or CAMERA_ID_DEFAULT)
    if not CAMERA_ID_RE.fullmatch(camera_id):
        return None
    tracks = []
    raw_tracks = message.get("tracks", [])
    if isinstance(raw_tracks, list):
        for raw in raw_tracks[:MAX_TRACKS]:
            normalized = normalize_track(raw)
            if normalized:
                tracks.append(normalized)
    people = _integer(message.get("peopleCount", message.get("people_count", len(tracks))))
    return {
        "schema_version": SCHEMA_VERSION,
        "message_type": "track.update",
        "camera_id": camera_id,
        "source": "local_npu",
        "frame_id": _integer(message.get("frame_id")),
        "captured_at_ms": _integer(message.get("captured_at_ms", message.get("timestamp", now_ms)), now_ms),
        "produced_at_ms": _integer(message.get("produced_at_ms", now_ms), now_ms),
        "received_at_ms": _integer(message.get("received_at_ms", now_ms), now_ms),
        "tracks": tracks,
        "people_count": people,
        "sentinel_active": bool(message.get("sentinelActive", message.get("sentinel_active", False))),
        "display_awake": bool(message.get("displayAwake", message.get("display_awake", False))),
        "latency_ms": _number(message.get("latencyMs", message.get("latency_ms", 0))),
        "legacy": schema_version == 0,
    }


def envelope(message_type: str, camera_id: str, *, source: str = "local_npu",
             frame_id: int = 0, captured_at_ms: int = 0, **extra: Any) -> dict[str, Any]:
    now = int(time.time() * 1000)
    return {
        "schema_version": SCHEMA_VERSION,
        "message_type": message_type,
        "camera_id": camera_id,
        "source": source,
        "frame_id": int(frame_id),
        "captured_at_ms": int(captured_at_ms),
        "produced_at_ms": now,
        "request_id": "",
        "event_id": "",
        "tracks": [],
        "result": {},
        "health": {},
        **extra,
    }
