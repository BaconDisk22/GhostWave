#!/usr/bin/env python3
"""Reference policy test only: not a production nonce allocator."""
import struct
def nonce(src, msg_id):
    if not (0 <= src <= 0xffffffff and 0 <= msg_id <= 0xffffffff):
        raise ValueError("out of range")
    return struct.pack("<III", src, msg_id, 0)

cases = [(1, 1), (1, 2), (2, 1), (0xffffffff, 0xffffffff), (0, 0)]
assert len({nonce(*pair) for pair in cases}) == len(cases)
assert all(len(nonce(*pair)) == 12 for pair in cases)
assert nonce(1, 2) == bytes([1, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0])
for pair in [(-1, 1), (1, -1), (2**32, 1), (1, 2**32)]:
    try:
        nonce(*pair)
    except ValueError:
        pass
    else:
        raise AssertionError("out-of-range nonce component accepted")
assert nonce(1, 1) == nonce(1, 1), "same tuple must reproduce same nonce"
print("PASS: nonce layout reference; production must persist message ID without reuse")
