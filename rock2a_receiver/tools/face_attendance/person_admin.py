#!/usr/bin/env python3
"""Local-only bootstrap utility for the small initial attendance roster."""

from __future__ import annotations

import argparse
from pathlib import Path

try:
    from .store import AttendanceStore, Person
except ImportError:
    from store import AttendanceStore, Person


def main() -> int:
    parser = argparse.ArgumentParser(description="Manage face-attendance persons on ROCK 2A")
    parser.add_argument("--database", required=True, type=Path)
    parser.add_argument("--timezone", default="Asia/Shanghai")
    subparsers = parser.add_subparsers(dest="command", required=True)
    upsert = subparsers.add_parser("upsert", help="create or update an enabled person")
    upsert.add_argument("person_id", type=int)
    upsert.add_argument("name")
    upsert.add_argument("role", choices=("student", "teacher"))
    args = parser.parse_args()

    store = AttendanceStore(args.database, args.timezone)
    try:
        if args.command == "upsert":
            store.upsert_person(Person(args.person_id, args.name, args.role))
            print("person %d updated" % args.person_id)
    finally:
        store.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
