#!/usr/bin/env python3
"""Host-only qualification policy regression; never certifies hardware."""
import unittest

REQUIRED = frozenset(("device", "pcb_revision", "firmware_sha256", "bootloader_evidence", "pinout_evidence", "receive_test", "sos_test", "recovery_test", "operator_approval"))
def qualified(record):
    if not isinstance(record, dict) or not REQUIRED.issubset(record):
        return False
    if any(not isinstance(record[k], str) or not record[k].strip() for k in REQUIRED):
        return False
    if record["operator_approval"] != "approved":
        return False
    if any(record[k] != "pass" for k in ("receive_test", "sos_test", "recovery_test")):
        return False
    digest = record["firmware_sha256"]
    return len(digest) == 64 and all(c in "0123456789abcdef" for c in digest)

class TestQualification(unittest.TestCase):
    def setUp(self):
        self.good = {k: "verified" for k in REQUIRED}
        self.good.update(firmware_sha256="a"*64, receive_test="pass", sos_test="pass", recovery_test="pass", operator_approval="approved")
    def test_missing_fields_rejected(self):
        for key in REQUIRED:
            bad = dict(self.good)
            del bad[key]
            self.assertFalse(qualified(bad))
    def test_pending_rejected(self):
        for key in ("receive_test", "sos_test", "recovery_test"):
            self.assertFalse(qualified(dict(self.good, **{key:"pending"})))
    def test_no_approval_rejected(self):
        self.assertFalse(qualified(dict(self.good, operator_approval="pending")))
    def test_complete_schema(self):
        self.assertTrue(qualified(self.good))
if __name__ == "__main__":
    unittest.main()
