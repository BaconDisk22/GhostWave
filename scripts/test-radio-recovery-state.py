#!/usr/bin/env python3
"""Host-only recovery model: not firmware or RF hardware validation."""
import unittest

class Recovery:
    def __init__(self, max_attempts=3):
        if not isinstance(max_attempts, int) or isinstance(max_attempts, bool) or max_attempts < 0:\n            raise ValueError('invalid retry budget')\n        self.max_attempts = max_attempts
        self.attempts = 0
        self.ready = False
    def initialize(self, pins_verified, driver_ok):
        self.ready = bool(pins_verified and driver_ok)
        if self.ready:
            self.attempts = 0
        return self.ready
    def fault(self):
        self.ready = False
    def retry(self, pins_verified, driver_ok):
        if self.attempts >= self.max_attempts:
            return False
        self.attempts += 1
        return self.initialize(pins_verified, driver_ok)

class RecoveryTests(unittest.TestCase):
    def test_unverified_pins_fail_closed(self):
        r = Recovery()
        self.assertFalse(r.initialize(False, True))
        self.assertFalse(r.ready)
    def test_driver_failure_requires_recovery(self):
        r = Recovery()
        self.assertTrue(r.initialize(True, True))
        r.fault()
        self.assertFalse(r.ready)
        self.assertTrue(r.retry(True, True))
    def test_invalid_budgets(self):\n        for budget in (-1, None, True, 1.5):\n            with self.assertRaises(ValueError):\n                Recovery(budget)\n    def test_zero_budget(self):\n        r = Recovery(0)\n        self.assertFalse(r.retry(True, True))\n        self.assertFalse(r.ready)\n    def test_retry_bounded(self):
        r = Recovery(2)
        self.assertFalse(r.retry(True, False))
        self.assertFalse(r.retry(True, False))
        self.assertFalse(r.retry(True, True))
        self.assertEqual(r.attempts, 2)

if __name__ == '__main__':
    unittest.main()
