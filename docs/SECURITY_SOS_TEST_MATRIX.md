# Security and SOS regression acceptance matrix

Each case must have a linked automated regression before release.

| Case | Expected result |
| --- | --- |
| Invalid DATA authentication tag | Reject without delivery |
| Plaintext DATA under require-encryption | Reject without delivery |
| Replayed DATA packet | Deliver at most once |
| Changed signed IDENT key | Warn; retain pinned key |
| Duplicate SOS sequence | Do not notify twice |
| Expired SOS and sequence rollover | Treat fresh message correctly |
| SOS cancelled in busy-channel queue | Never transmit cancelled frame |
| SOS retry delayed by channel contention | Retry timer starts at radio attempt |
| Radio restart during pending SOS | Recover without unintended replay |

Hardware SOS reliability remains unverified. Do not represent GhostWave as a proven emergency service.
