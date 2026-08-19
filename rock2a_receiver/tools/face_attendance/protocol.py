"""Small, strict protocol helpers shared by the attendance service and tests."""

from __future__ import annotations

import re
from dataclasses import dataclass

MAX_FACE_JPEG_BYTES = 512 * 1024
REQUEST_ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{7,127}$")
ATTENDANCE_TYPES = frozenset({"check_in", "check_out"})


@dataclass(frozen=True)
class VerifyRequest:
    request_id: str
    attendance_type: str
    face_width_px: int
    quality_score: float
    camera_id: str
    image: bytes


def parse_verify_request(headers: dict[str, str], image: bytes) -> VerifyRequest:
    """Validate the JPEG request before an image decoder sees it."""
    request_id = headers.get("x-attendance-request-id", "")
    attendance_type = headers.get("x-attendance-type", "")
    camera_id = headers.get("x-camera-id", "rv1106-01")
    if not REQUEST_ID_RE.fullmatch(request_id):
        raise ValueError("invalid X-Attendance-Request-Id")
    if attendance_type not in ATTENDANCE_TYPES:
        raise ValueError("X-Attendance-Type must be check_in or check_out")
    if not 112 <= _integer(headers.get("x-face-width-px"), 0) <= 4096:
        raise ValueError("X-Face-Width-Px must be between 112 and 4096")
    quality = _number(headers.get("x-face-quality"), -1.0)
    if not 0.0 <= quality <= 1.0:
        raise ValueError("X-Face-Quality must be between 0 and 1")
    if not 1 <= len(camera_id) <= 64 or not re.fullmatch(r"[A-Za-z0-9._-]+", camera_id):
        raise ValueError("invalid X-Camera-Id")
    if not 4 <= len(image) <= MAX_FACE_JPEG_BYTES or not image.startswith(b"\xff\xd8"):
        raise ValueError("body must be a JPEG no larger than 512 KiB")
    return VerifyRequest(request_id, attendance_type, _integer(headers.get("x-face-width-px"), 0),
                         quality, camera_id, image)


def _integer(value: str | None, default: int) -> int:
    try:
        return int(value or "")
    except ValueError:
        return default


def _number(value: str | None, default: float) -> float:
    try:
        return float(value or "")
    except ValueError:
        return default
