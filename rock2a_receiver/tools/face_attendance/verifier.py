"""Pluggable face-verification backends.

The service deliberately starts unavailable when no trained verifier is configured.
It must never treat a detected face as proof of identity.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Verification:
    person_id: int | None
    score: float
    message: str


class FaceVerifier:
    def verify(self, image: bytes) -> Verification:  # pragma: no cover - interface
        raise NotImplementedError


class UnavailableVerifier(FaceVerifier):
    def verify(self, image: bytes) -> Verification:
        del image
        return Verification(None, 0.0, "face verifier is not configured")


class OpenCvLbphVerifier(FaceVerifier):
    """Verification for pre-cropped face JPEGs using an offline LBPH model.

    The model is trained by a separate administrative workflow; deployment has no
    enrollment endpoint so the terminal cannot silently alter biometric data.
    """

    def __init__(self, model_path: Path, threshold: float = 65.0) -> None:
        try:
            import cv2  # type: ignore
            import numpy as np
        except ImportError as error:
            raise RuntimeError("OpenCV contrib and NumPy are required for LBPH verification") from error
        if not model_path.is_file():
            raise RuntimeError("LBPH model does not exist: %s" % model_path)
        if not hasattr(cv2, "face"):
            raise RuntimeError("OpenCV was built without the contrib face module")
        if not 20.0 <= threshold <= 200.0:
            raise ValueError("LBPH threshold must be between 20 and 200")
        self.cv2 = cv2
        self.np = np
        self.threshold = threshold
        self.model = cv2.face.LBPHFaceRecognizer_create()
        self.model.read(str(model_path))

    def verify(self, image: bytes) -> Verification:
        encoded = self.np.frombuffer(image, dtype=self.np.uint8)
        matrix = self.cv2.imdecode(encoded, self.cv2.IMREAD_GRAYSCALE)
        if matrix is None or matrix.size == 0:
            return Verification(None, 0.0, "invalid JPEG")
        face = self.cv2.resize(matrix, (200, 200))
        face = self.cv2.equalizeHist(face)
        person_id, distance = self.model.predict(face)
        if distance > self.threshold:
            return Verification(None, float(distance), "face is not registered")
        return Verification(int(person_id), float(distance), "verified")
