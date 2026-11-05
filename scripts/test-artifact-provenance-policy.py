#!/usr/bin/env python3
"""Host-side provenance manifest schema reference; no firmware is qualified."""
import unittest

REQUIRED = {"commit", "board", "pcb_revision", "bootloader", "toolchain", "sha256", "build_log", "flash_limit", "pin_review"}
FORBIDDEN_BOARD = {"", "generic", "nrf52840_compile_probe", "adafruit_feather_nrf52840"}

def validate(manifest):
    if not isinstance(manifest, dict) or not REQUIRED.issubset(manifest):
        return False
    if any(not isinstance(manifest[k], str) or not manifest[k].strip() for k in REQUIRED):
        return False
    if manifest["board"].lower() in FORBIDDEN_BOARD:
        return False
    digest = manifest["sha256"]
    return len(digest) == 64 and all(c in "0123456789abcdef" for c in digest)

class TestPolicy(unittest.TestCase):
    def setUp(self):
        self.good = {k: "reviewed" for k in REQUIRED}
        self.good.update(board="RAK4631", sha256="a" * 64)
    def test_complete(self):
        self.assertTrue(validate(self.good))
    def test_missing(self):
        for key in REQUIRED:
            bad = dict(self.good)
            del bad[key]
            self.assertFalse(validate(bad))
    def test_probe_rejected(self):
        bad = dict(self.good, board="nrf52840_compile_probe")
        self.assertFalse(validate(bad))
    def test_digest_rejected(self):
        self.assertFalse(validate(dict(self.good, sha256="not-a-digest")))
if __name__ == "__main__":
    unittest.main()
