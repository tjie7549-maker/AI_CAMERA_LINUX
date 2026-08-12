"""Small SQLite persistence layer for event-oriented camera state."""

from __future__ import annotations

import json
import sqlite3
import threading
import time
from pathlib import Path
from typing import Any

SCHEMA_VERSION = 1


class EventStore:
    def __init__(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        self.connection = sqlite3.connect(path, timeout=5, check_same_thread=False)
        self.connection.row_factory = sqlite3.Row
        self.connection.execute("PRAGMA foreign_keys=ON")
        self.connection.execute("PRAGMA busy_timeout=5000")
        self.connection.execute("PRAGMA journal_mode=WAL")
        self._lock = threading.RLock()
        self._migrate()

    def _migrate(self) -> None:
        with self._lock, self.connection:
            self.connection.execute("CREATE TABLE IF NOT EXISTS schema_version (version INTEGER NOT NULL)")
            if not self.connection.execute("SELECT 1 FROM schema_version").fetchone():
                self.connection.execute("INSERT INTO schema_version VALUES (?)", (SCHEMA_VERSION,))
            version = int(self.connection.execute(
                "SELECT version FROM schema_version LIMIT 1").fetchone()[0])
            if version > SCHEMA_VERSION:
                raise RuntimeError("events.db schema is newer than this service")
            self.connection.execute("""CREATE TABLE IF NOT EXISTS events (
                event_id TEXT PRIMARY KEY, camera_id TEXT NOT NULL, state TEXT NOT NULL,
                started_at INTEGER NOT NULL, ended_at INTEGER, duration_ms INTEGER,
                max_people INTEGER NOT NULL DEFAULT 0, warning INTEGER NOT NULL DEFAULT 0,
                warning_reason TEXT NOT NULL DEFAULT '', summary TEXT NOT NULL DEFAULT '',
                best_image_path TEXT, best_frame_id INTEGER NOT NULL DEFAULT 0,
                frame_match TEXT NOT NULL DEFAULT 'approximate', created_at INTEGER NOT NULL,
                updated_at INTEGER NOT NULL)""")
            self.connection.execute("""CREATE TABLE IF NOT EXISTS tracks (
                event_id TEXT NOT NULL REFERENCES events(event_id) ON DELETE CASCADE,
                track_id INTEGER NOT NULL, first_seen_at INTEGER NOT NULL, last_seen_at INTEGER NOT NULL,
                max_confidence REAL NOT NULL, PRIMARY KEY(event_id, track_id))""")
            self.connection.execute("""CREATE TABLE IF NOT EXISTS recognitions (
                request_id TEXT PRIMARY KEY, event_id TEXT REFERENCES events(event_id), source TEXT NOT NULL,
                frame_id INTEGER NOT NULL, backend TEXT NOT NULL, success INTEGER NOT NULL,
                latency_ms INTEGER NOT NULL, input_tokens INTEGER NOT NULL DEFAULT 0,
                output_tokens INTEGER NOT NULL DEFAULT 0, result_json TEXT NOT NULL, created_at INTEGER NOT NULL)""")
            self.connection.execute("""CREATE TABLE IF NOT EXISTS event_updates (
                id INTEGER PRIMARY KEY AUTOINCREMENT, event_id TEXT NOT NULL REFERENCES events(event_id),
                update_type TEXT NOT NULL, payload_json TEXT NOT NULL, created_at INTEGER NOT NULL)""")
            self.connection.execute(
                "CREATE INDEX IF NOT EXISTS events_state_started ON events(state, started_at DESC)")
            self.connection.execute(
                "CREATE INDEX IF NOT EXISTS recognitions_event ON recognitions(event_id, created_at)")
            self.connection.execute(
                "CREATE INDEX IF NOT EXISTS updates_event ON event_updates(event_id, created_at)")
            if version < SCHEMA_VERSION:
                self.connection.execute("UPDATE schema_version SET version=?",
                                        (SCHEMA_VERSION,))

    def close_open_events(self) -> None:
        now = int(time.time() * 1000)
        with self._lock, self.connection:
            self.connection.execute("UPDATE events SET state='interrupted', ended_at=?, duration_ms=? - started_at, updated_at=? WHERE state IN ('new','active','ending')", (now, now, now))

    def upsert_event(self, event: dict[str, Any]) -> None:
        now = int(time.time() * 1000)
        with self._lock, self.connection:
            self.connection.execute("""INSERT INTO events(event_id,camera_id,state,started_at,ended_at,duration_ms,max_people,warning,warning_reason,summary,best_image_path,best_frame_id,frame_match,created_at,updated_at)
                VALUES(:event_id,:camera_id,:state,:started_at,:ended_at,:duration_ms,:max_people,:warning,:warning_reason,:summary,:best_image_path,:best_frame_id,:frame_match,:created_at,:updated_at)
                ON CONFLICT(event_id) DO UPDATE SET state=excluded.state,ended_at=excluded.ended_at,duration_ms=excluded.duration_ms,max_people=excluded.max_people,warning=excluded.warning,warning_reason=excluded.warning_reason,summary=excluded.summary,best_image_path=excluded.best_image_path,best_frame_id=excluded.best_frame_id,frame_match=excluded.frame_match,updated_at=excluded.updated_at""", {
                "ended_at": None, "duration_ms": None, "warning": 0, "warning_reason": "", "summary": "",
                "best_image_path": None, "best_frame_id": 0, "frame_match": "approximate", "created_at": now,
                "updated_at": now, **event})

    def upsert_track(self, event_id: str, track: dict[str, Any], now: int) -> None:
        with self._lock, self.connection:
            self.connection.execute("""INSERT INTO tracks(event_id,track_id,first_seen_at,last_seen_at,max_confidence) VALUES(?,?,?,?,?)
                ON CONFLICT(event_id,track_id) DO UPDATE SET last_seen_at=excluded.last_seen_at,max_confidence=MAX(max_confidence,excluded.max_confidence)""",
                (event_id, track["track_id"], now, now, track["confidence"]))

    def append_update(self, event_id: str, update_type: str, payload: dict[str, Any]) -> None:
        with self._lock, self.connection:
            self.connection.execute("INSERT INTO event_updates(event_id,update_type,payload_json,created_at) VALUES(?,?,?,?)",
                (event_id, update_type, json.dumps(payload, ensure_ascii=False, separators=(",", ":")), int(time.time() * 1000)))

    def record_recognition(self, document: dict[str, Any]) -> None:
        usage = document.get("usage", {}) if isinstance(document.get("usage"), dict) else {}
        with self._lock, self.connection:
            self.connection.execute("""INSERT INTO recognitions(request_id,event_id,source,frame_id,backend,success,latency_ms,input_tokens,output_tokens,result_json,created_at)
                VALUES(?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(request_id) DO NOTHING""",
                (document["request_id"], document.get("event_id"), document.get("source", "cloud"), document.get("frame_id", 0),
                 document.get("recognition_backend", "cloud"), int(bool(document.get("success"))), int(document.get("server_latency_ms", 0)),
                 int(usage.get("input_tokens", 0)), int(usage.get("output_tokens", 0)),
                 json.dumps(document.get("result", {}), ensure_ascii=False), int(time.time() * 1000)))

    def list_events(self, limit: int = 20, before: int | None = None, state: str | None = None) -> list[dict[str, Any]]:
        query, values = "SELECT * FROM events", []
        clauses = []
        if before is not None: clauses.append("started_at < ?"); values.append(before)
        if state: clauses.append("state = ?"); values.append(state)
        if clauses: query += " WHERE " + " AND ".join(clauses)
        with self._lock:
            rows = self.connection.execute(query + " ORDER BY started_at DESC LIMIT ?", (*values, max(1, min(limit, 100)))).fetchall()
        return [dict(row) for row in rows]

    def get_event(self, event_id: str) -> dict[str, Any] | None:
        with self._lock:
            row = self.connection.execute("SELECT * FROM events WHERE event_id=?", (event_id,)).fetchone()
            if not row: return None
            result = dict(row)
            result["tracks"] = [dict(value) for value in self.connection.execute(
                "SELECT * FROM tracks WHERE event_id=? ORDER BY track_id", (event_id,))]
            result["recognitions"] = [dict(value) for value in self.connection.execute(
                "SELECT * FROM recognitions WHERE event_id=? ORDER BY created_at", (event_id,))]
        return result

    def count_events(self) -> int:
        with self._lock:
            return int(self.connection.execute("SELECT COUNT(*) FROM events").fetchone()[0])

    def close(self) -> None:
        with self._lock:
            self.connection.close()
