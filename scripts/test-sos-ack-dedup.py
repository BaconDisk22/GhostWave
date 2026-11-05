#!/usr/bin/env python3
"""Reference regression for duplicate SOS acknowledgments.

This models intended behavior; integration with the router remains separate.
"""
class AckTracker:
    def __init__(self):
        self._seen = set()

    def accept(self, origin: int, sequence: int, responder: int) -> bool:
        if not (0 <= sequence <= 65535):
            raise ValueError("sequence out of range")
        key = (origin, sequence, responder)
        if key in self._seen:
            return False
        self._seen.add(key)
        return True

tracker = AckTracker()
assert tracker.accept(10, 1, 20)
assert not tracker.accept(10, 1, 20)
assert tracker.accept(10, 1, 21)
assert tracker.accept(10, 2, 20)
assert tracker.accept(11, 1, 20)
print("PASS: SOS ACK reference deduplication")
