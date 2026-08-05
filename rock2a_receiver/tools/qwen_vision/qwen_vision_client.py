"""Qwen OpenAI-compatible vision client for periodic camera snapshots."""

from __future__ import annotations

import base64
import json
import mimetypes
import os
import time
from pathlib import Path
from typing import Any

from openai import (
    APIConnectionError,
    APIStatusError,
    APITimeoutError,
    AuthenticationError,
    InternalServerError,
    OpenAI,
    RateLimitError,
)

from prompts import SYSTEM_PROMPT, USER_PROMPT


SUPPORTED_SUFFIXES = {".jpg", ".jpeg", ".png", ".webp"}
REQUIRED_RESULT_KEYS = {
    "scene",
    "objects",
    "people_count",
    "warning",
    "warning_reason",
    "summary",
}


class QwenVisionClient:
    """One synchronous vision request. Call it from a low-frequency worker only."""

    def __init__(self) -> None:
        self.api_key = os.environ.get("DASHSCOPE_API_KEY", "")
        self.base_url = os.environ.get("DASHSCOPE_BASE_URL", "")
        self.model = os.environ.get("QWEN_MODEL", "qwen3-vl-flash")
        self._missing = [
            name
            for name, value in (
                ("DASHSCOPE_API_KEY", self.api_key),
                ("DASHSCOPE_BASE_URL", self.base_url),
                ("QWEN_MODEL", os.environ.get("QWEN_MODEL", "")),
            )
            if not value
        ]
        self.client: OpenAI | None = None
        if not self._missing:
            self.client = OpenAI(
                api_key=self.api_key,
                base_url=self.base_url,
                timeout=30.0,
                max_retries=1,
            )

    def analyze_image(self, image_path: str) -> dict[str, Any]:
        """Analyze one local image and always return the documented result shape."""
        started = time.monotonic()
        path = Path(image_path)
        if self._missing:
            return self._failure(
                "environment_missing",
                "missing environment variables: " + ", ".join(self._missing),
                started,
            )
        if not path.is_file():
            return self._failure("file_not_found", "image file does not exist", started)
        if path.suffix.lower() not in SUPPORTED_SUFFIXES:
            return self._failure("unsupported_image", "supported formats: jpg, jpeg, png, webp", started)

        try:
            image_bytes = path.read_bytes()
        except OSError:
            return self._failure("image_read_error", "cannot read image file", started)
        if not image_bytes:
            return self._failure("image_read_error", "image file is empty", started)

        mime_type = mimetypes.guess_type(path.name)[0]
        if path.suffix.lower() in {".jpg", ".jpeg"}:
            mime_type = "image/jpeg"
        if not mime_type:
            return self._failure("unsupported_image", "cannot determine image MIME type", started)

        return self.analyze_image_bytes(image_bytes, mime_type)

    def analyze_image_bytes(
        self, image_bytes: bytes, mime_type: str = "image/jpeg"
    ) -> dict[str, Any]:
        """Analyze an in-memory image for manual frozen-frame recognition."""
        started = time.monotonic()
        if self._missing:
            return self._failure(
                "environment_missing",
                "missing environment variables: " + ", ".join(self._missing),
                started,
            )
        if not image_bytes:
            return self._failure("image_read_error", "image data is empty", started)
        if mime_type not in {"image/jpeg", "image/png", "image/webp"}:
            return self._failure("unsupported_image", "unsupported image MIME type", started)

        data_url = "data:" + mime_type + ";base64," + base64.b64encode(image_bytes).decode("ascii")
        try:
            assert self.client is not None
            response = self.client.chat.completions.create(
                model=self.model,
                messages=[
                    {"role": "system", "content": SYSTEM_PROMPT},
                    {
                        "role": "user",
                        "content": [
                            {"type": "text", "text": USER_PROMPT},
                            {"type": "image_url", "image_url": {"url": data_url}},
                        ],
                    },
                ],
                temperature=0.1,
                max_tokens=300,
            )
        except AuthenticationError:
            return self._failure("authentication_error", "authentication failed", started)
        except APITimeoutError:
            return self._failure("timeout", "API request timed out", started)
        except RateLimitError:
            return self._failure("rate_limit", "API rate limit reached", started)
        except InternalServerError:
            return self._failure("server_error", "API server error", started)
        except APIConnectionError:
            return self._failure("server_error", "cannot connect to API server", started)
        except APIStatusError:
            return self._failure("server_error", "API request was rejected", started)
        except Exception:
            return self._failure("unknown_error", "unexpected API request error", started)

        latency_ms = self._elapsed_ms(started)
        content = response.choices[0].message.content if response.choices else None
        if not content or not content.strip():
            return self._failure("empty_response", "model returned an empty response", started, latency_ms)
        try:
            parsed = json.loads(content)
        except json.JSONDecodeError:
            return self._failure("invalid_json", "model response is not valid JSON", started, latency_ms)
        if not self._valid_result(parsed):
            return self._failure("invalid_json", "model JSON does not match the required schema", started, latency_ms)

        usage = getattr(response, "usage", None)
        input_tokens = int(getattr(usage, "prompt_tokens", 0) or 0)
        output_tokens = int(getattr(usage, "completion_tokens", 0) or 0)
        total_tokens = int(getattr(usage, "total_tokens", input_tokens + output_tokens) or 0)
        return {
            "success": True,
            "model": getattr(response, "model", None) or self.model,
            "latency_ms": latency_ms,
            "usage": {
                "input_tokens": input_tokens,
                "output_tokens": output_tokens,
                "total_tokens": total_tokens,
            },
            "result": parsed,
        }

    def _failure(
        self,
        error_type: str,
        error_message: str,
        started: float,
        latency_ms: int | None = None,
    ) -> dict[str, Any]:
        return {
            "success": False,
            "model": self.model,
            "latency_ms": self._elapsed_ms(started) if latency_ms is None else latency_ms,
            "error_type": error_type,
            "error_message": error_message,
        }

    @staticmethod
    def _elapsed_ms(started: float) -> int:
        return int((time.monotonic() - started) * 1000)

    @staticmethod
    def _valid_result(result: Any) -> bool:
        return (
            isinstance(result, dict)
            and REQUIRED_RESULT_KEYS.issubset(result)
            and isinstance(result["scene"], str)
            and isinstance(result["objects"], list)
            and isinstance(result["people_count"], int)
            and not isinstance(result["people_count"], bool)
            and isinstance(result["warning"], bool)
            and isinstance(result["warning_reason"], str)
            and isinstance(result["summary"], str)
        )
