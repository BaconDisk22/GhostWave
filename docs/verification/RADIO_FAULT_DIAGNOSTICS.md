# Radio fault diagnostics

Proposed observable counters: initialization failure, busy timeout, SPI error, RX CRC failure, RX timeout, TX timeout, reset attempts, successful recovery, dropped oversized frames.

Every diagnostic should distinguish host mock output from physical device output. Avoid logging message plaintext, secret identity seeds, or raw private keys. Recovery must be bounded, with explicit state transitions and timestamps.

Acceptance: deterministic host tests for counter increments, reset behavior, and privacy-safe logs. Hardware verification remains pending.
