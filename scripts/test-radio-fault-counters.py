#!/usr/bin/env python3
"""Host-only fault-counter reference model; no physical radio driver."""
import unittest

class Counters:
    allowed = frozenset(("init_failure", "busy_timeout", "spi_error", "rx_crc_failure", "rx_timeout", "tx_timeout", "reset_attempt", "recovery", "oversized_frame"))
    def __init__(self):
        self.counts = {name: 0 for name in self.allowed}
    def increment(self, name):
        if name not in self.allowed:
            raise ValueError("unknown fault")
        self.counts[name] = min(self.counts[name] + 1, 0xffffffff)
    def snapshot(self):
        return dict(self.counts)

class TestCounters(unittest.TestCase):
    def test_independent_counts(self):
        c = Counters()
        c.increment("busy_timeout")
        c.increment("busy_timeout")
        c.increment("recovery")
        self.assertEqual(c.snapshot()["busy_timeout"], 2)
        self.assertEqual(c.snapshot()["recovery"], 1)
    def test_reject_unknown(self):
        with self.assertRaises(ValueError):
            Counters().increment("private_key")
    def test_saturate(self):
        c = Counters()
        c.counts["spi_error"] = 0xffffffff
        c.increment("spi_error")
        self.assertEqual(c.counts["spi_error"], 0xffffffff)
if __name__ == "__main__":
    unittest.main()
