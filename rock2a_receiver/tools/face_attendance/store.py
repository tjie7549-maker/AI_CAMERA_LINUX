"""Transactional SQLite storage for identity and attendance records."""

from __future__ import annotations

import sqlite3
import threading
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from zoneinfo import ZoneInfo


@dataclass(frozen=True)
class Person:
    person_id: int
    name: str
    role: str


@dataclass(frozen=True)
class AttendanceOutcome:
    status: str
    person: Person
    attendance_type: str
    checked_at_ms: int
    score: float


class AttendanceStore:
    """Owns the sole durable source of truth for attendance decisions."""

    def __init__(self, path: Path, timezone: str = "Asia/Shanghai") -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        self.connection = sqlite3.connect(path, timeout=5, check_same_thread=False)
        self.connection.row_factory = sqlite3.Row
        self.connection.execute("PRAGMA foreign_keys=ON")
        self.connection.execute("PRAGMA journal_mode=WAL")
        self.connection.execute("PRAGMA busy_timeout=5000")
        self.lock = threading.RLock()
        self.zone = ZoneInfo(timezone)
        self._migrate()

    def _migrate(self) -> None:
        with self.lock, self.connection:
            self.connection.executescript("""
                CREATE TABLE IF NOT EXISTS persons (
                    person_id INTEGER PRIMARY KEY,
                    name TEXT NOT NULL,
                    role TEXT NOT NULL CHECK(role IN ('student', 'teacher')),
                    enabled INTEGER NOT NULL DEFAULT 1 CHECK(enabled IN (0, 1))
                );
                CREATE TABLE IF NOT EXISTS attendance_records (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    request_id TEXT NOT NULL UNIQUE,
                    person_id INTEGER NOT NULL REFERENCES persons(person_id),
                    attendance_day TEXT NOT NULL,
                    attendance_type TEXT NOT NULL CHECK(attendance_type IN ('check_in', 'check_out')),
                    checked_at_ms INTEGER NOT NULL,
                    mode TEXT NOT NULL,
                    score REAL NOT NULL,
                    camera_id TEXT NOT NULL,
                    created_at_ms INTEGER NOT NULL,
                    UNIQUE(person_id, attendance_day, attendance_type)
                );
            """)

    def upsert_person(self, person: Person) -> None:
        if person.person_id <= 0 or not person.name.strip() or person.role not in {"student", "teacher"}:
            raise ValueError("invalid person")
        with self.lock, self.connection:
            self.connection.execute("""
                INSERT INTO persons(person_id, name, role, enabled) VALUES (?, ?, ?, 1)
                ON CONFLICT(person_id) DO UPDATE SET name=excluded.name, role=excluded.role, enabled=1
            """, (person.person_id, person.name.strip(), person.role))

    def record(self, request_id: str, person_id: int, attendance_type: str,
               score: float, camera_id: str, now_ms: int) -> AttendanceOutcome:
        """Insert once. A database constraint, not a read-then-write check, provides idempotency."""
        with self.lock, self.connection:
            person = self._person(person_id)
            if person is None:
                raise LookupError("recognized person is absent or disabled")
            existing = self.connection.execute("""
                SELECT r.*, p.name, p.role FROM attendance_records r
                JOIN persons p ON p.person_id=r.person_id WHERE r.request_id=?
            """, (request_id,)).fetchone()
            if existing:
                return self._outcome(existing, "recorded" if existing["request_id"] == request_id else "duplicate")
            local = datetime.fromtimestamp(now_ms / 1000, self.zone)
            day = local.date().isoformat()
            try:
                self.connection.execute("""
                    INSERT INTO attendance_records(request_id, person_id, attendance_day, attendance_type,
                                                   checked_at_ms, mode, score, camera_id, created_at_ms)
                    VALUES (?, ?, ?, ?, ?, 'face', ?, ?, ?)
                """, (request_id, person.person_id, day, attendance_type, now_ms, score, camera_id, now_ms))
            except sqlite3.IntegrityError:
                row = self.connection.execute("""
                    SELECT r.*, p.name, p.role FROM attendance_records r
                    JOIN persons p ON p.person_id=r.person_id
                    WHERE r.person_id=? AND r.attendance_day=? AND r.attendance_type=?
                """, (person.person_id, day, attendance_type)).fetchone()
                if row is None:
                    raise
                return self._outcome(row, "duplicate")
            return AttendanceOutcome("recorded", person, attendance_type, now_ms, score)

    def _person(self, person_id: int) -> Person | None:
        row = self.connection.execute(
            "SELECT person_id, name, role FROM persons WHERE person_id=? AND enabled=1", (person_id,)).fetchone()
        return Person(int(row["person_id"]), str(row["name"]), str(row["role"])) if row else None

    @staticmethod
    def _outcome(row: sqlite3.Row, status: str) -> AttendanceOutcome:
        return AttendanceOutcome(status, Person(int(row["person_id"]), str(row["name"]), str(row["role"])),
                                 str(row["attendance_type"]), int(row["checked_at_ms"]), float(row["score"]))

    def close(self) -> None:
        with self.lock:
            self.connection.close()
