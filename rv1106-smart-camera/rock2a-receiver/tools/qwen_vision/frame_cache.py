"""Bounded event-frame cache. Frame/NPU matching is deliberately approximate."""

from __future__ import annotations

import shutil
import json
from dataclasses import replace
from dataclasses import dataclass
from pathlib import Path


@dataclass
class FrameCandidate:
    path: Path
    captured_at_ms: int
    frame_id: int
    confidence: float
    area: float
    brightness: float = 128.0
    variance: float = 64.0
    difference: float = 1.0
    offset_ms: int = 0
    receiver_frame_id: int = 0

    @property
    def score(self) -> float:
        exposure = max(0.0, 1.0 - abs(self.brightness - 128.0) / 128.0)
        sharpness = min(1.0, self.variance / 64.0)
        area = 1.0 - min(1.0, abs(self.area - 0.18) / 0.18)
        return 0.45 * self.confidence + 0.2 * max(0.0, area) + 0.2 * sharpness + 0.1 * exposure + 0.05 * min(1.0, self.difference)


class FrameCache:
    def __init__(self, root: Path, latest_image: Path, ring_dir: Path | None = None,
                 max_frames: int = 32) -> None:
        self.root, self.latest_image, self.ring_dir, self.max_frames = root, latest_image, ring_dir, max_frames
        self.root.mkdir(parents=True, exist_ok=True)

    def capture(self, event_id: str, frame_id: int, captured_at_ms: int, confidence: float, area: float) -> FrameCandidate | None:
        source = self.latest_image
        source_time = captured_at_ms
        receiver_frame = 0
        brightness, variance = 128.0, 64.0
        difference = 1.0
        if self.ring_dir and self.ring_dir.is_dir():
            choices: list[tuple[int, Path]] = []
            for item in self.ring_dir.glob("frame_*.jpg"):
                parts = item.stem.split("_")
                if len(parts) >= 3 and parts[1].isdigit():
                    choices.append((int(parts[1]), item))
            if choices:
                source_time, source = min(choices, key=lambda value: abs(value[0] - captured_at_ms))
                try:
                    meta = json.loads(source.with_suffix(".json").read_text(encoding="utf-8"))
                    receiver_frame = int(meta.get("receiver_frame_id", 0))
                    brightness = float(meta.get("brightness", brightness))
                    variance = float(meta.get("variance", variance))
                    difference = float(meta.get("difference", difference))
                except (OSError, ValueError, TypeError, json.JSONDecodeError):
                    pass
        if not source.is_file(): return None
        target = self.root / ("%s_%d_%d.jpg" % (event_id, captured_at_ms, frame_id))
        shutil.copyfile(source, target)
        files = sorted(self.root.glob("*.jpg"), key=lambda item: item.stat().st_mtime)
        for old in files[:-self.max_frames]: old.unlink(missing_ok=True)
        return FrameCandidate(target, source_time, frame_id, confidence, area,
                              brightness=brightness, variance=variance,
                              difference=max(0.0, min(1.0, difference)),
                              offset_ms=abs(source_time - captured_at_ms),
                              receiver_frame_id=receiver_frame)

    def promote(self, event_id: str, candidate: FrameCandidate) -> FrameCandidate:
        """Copy a winning candidate outside the bounded scratch ring."""
        best_dir = self.root.parent / "event_best"
        best_dir.mkdir(parents=True, exist_ok=True)
        target = best_dir / (event_id + ".jpg")
        temporary = target.with_suffix(".jpg.tmp")
        shutil.copyfile(candidate.path, temporary)
        temporary.replace(target)
        return replace(candidate, path=target)
