# GhostWave hardware qualification

**All evidence pending. No flashing or transmission authorized.**

| Check | T1000-E | RAK4631 |
|---|---|---|
| Board revision and vendor schematic | Pending | Pending |
| SPI, NSS, BUSY, DIO1, RESET pins | Pending | Pending |
| TCXO, RF switch and power sequencing | Pending | Pending |
| Antenna and regional settings | Pending | Pending |
| Receive-only smoke test | Pending | Pending |
| Controlled RF transmission test | Pending | Pending |
| IRQ timeout and reset recovery | Pending | Pending |
| Encrypted message and SOS delivery | Pending | Pending |
| Reproducible firmware and rollback | Pending | Pending |

For each result, record the exact device revision, schematic source, commit SHA, firmware build, test environment, RF settings, logs, date and reviewer. Compilation and simulation are not physical validation. The T1000-E probe must never be flashed; no RAK4631 GhostWave firmware target has been qualified. The release gate stays blocked.
