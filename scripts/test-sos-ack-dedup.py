#!/usr/bin/env python3
"""Bounded reference SOS ACK deduplication; not router integration."""
from collections import OrderedDict

class AckTracker:
    def __init__(self, capacity=128):
        if capacity < 1:
            raise ValueError("capacity")
        self.capacity = capacity
        self.seen = OrderedDict()

    def accept(self, origin, sequence, responder):
        if any(not isinstance(v, int) or not 0 <= v <= 0xffffffff for v in (origin, responder)):
            raise ValueError("invalid node ID")
        if not isinstance(sequence, int) or not 0 <= sequence <= 65535:
            raise ValueError("invalid sequence")
        key = (origin, sequence, responder)
        if key in self.seen:
            return False
        self.seen[key] = None
        if len(self.seen) > self.capacity:
            self.seen.popitem(last=False)
        return True

t = AckTracker(2)
assert t.accept(10, 1, 20)
assert not t.accept(10, 1, 20)
assert t.accept(10, 1, 21)
assert t.accept(10, 2, 20)
assert len(t.seen) == 2
assert t.accept(10, 1, 20), "evicted entry should be accepted again"
for values in [(10, -1, 20), (10, 65536, 20), (-1, 1, 20)]:
    try:
        t.accept(*values)
    except ValueError:
        pass
    else:
        raise AssertionError("invalid SOS ACK accepted")
print("PASS: bounded SOS ACK reference model")
