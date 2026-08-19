import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from protocol import parse_verify_request
from store import AttendanceStore, Person


class ProtocolTests(unittest.TestCase):
    def test_accepts_strict_face_request(self):
        request = parse_verify_request({
            "x-attendance-request-id": "face-00000001",
            "x-attendance-type": "check_in",
            "x-face-width-px": "120",
            "x-face-quality": "0.8",
            "x-camera-id": "rv1106-01",
        }, b"\xff\xd8test")
        self.assertEqual(request.attendance_type, "check_in")

    def test_rejects_small_face_and_bad_id(self):
        headers = {"x-attendance-request-id": "bad", "x-attendance-type": "check_in",
                   "x-face-width-px": "100", "x-face-quality": "0.8"}
        with self.assertRaises(ValueError):
            parse_verify_request(headers, b"\xff\xd8test")


class StoreTests(unittest.TestCase):
    def test_request_retry_and_daily_dedup_are_atomic(self):
        with tempfile.TemporaryDirectory() as directory:
            store = AttendanceStore(Path(directory) / "attendance.db")
            store.upsert_person(Person(1, "Test User", "student"))
            first = store.record("face-00000001", 1, "check_in", 42.0, "rv1106-01", 1760000000000)
            retry = store.record("face-00000001", 1, "check_in", 42.0, "rv1106-01", 1760000001000)
            duplicate = store.record("face-00000002", 1, "check_in", 43.0, "rv1106-01", 1760000002000)
            checkout = store.record("face-00000003", 1, "check_out", 44.0, "rv1106-01", 1760000003000)
            self.assertEqual((first.status, retry.status, duplicate.status, checkout.status),
                             ("recorded", "recorded", "duplicate", "recorded"))
            store.close()


if __name__ == "__main__":
    unittest.main()
