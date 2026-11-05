# SX1262 receive-path acceptance

This document is a checklist, not a working receiver.

## Host requirements
- Reject empty, oversized, and truncated frames before routing.
- Preserve packet length and byte order across driver and router boundaries.
- Distinguish receive timeout, CRC failure, and radio reset.
- Handle duplicate frames without duplicate app delivery.
- Test malformed input using deterministic host mocks.

## Hardware requirements
- Verify IRQ, BUSY, FIFO, TCXO, antenna and RF switch behavior against exact board revision.
- Capture serial logs for receive-only smoke tests before any TX is enabled.
- Validate recovery after sleep and reset.

The T1000-E probe remains nonflashable and the release gate remains blocked.
