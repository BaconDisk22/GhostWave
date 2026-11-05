# Hardware qualification matrix

All entries are **unverified** until backed by device-specific evidence. This checklist does not authorize flashing, transmission, or release.

| Requirement | Seeed T1000-E | RAK4631 |
| --- | --- | --- |
| Exact board revision identified | Pending | Pending |
| Verified manufacturer schematic and pin map | Pending | Pending |
| SX1262 SPI / BUSY / DIO1 / RESET | Pending | Pending |
| TCXO and power sequencing | Pending | Pending |
| RF switch and antenna configuration | Pending | Pending |
| Boot and recovery behavior | Pending | Pending |
| Receive-only smoke test | Pending | Pending |
| TX with compliant settings and appropriate test setup | Pending | Pending |
| Channel busy / timeout / radio reset | Pending | Pending |
| Multi-node encrypted delivery and SOS | Pending | Pending |
| Reproducible build and rollback path | Pending | Pending |

## Evidence record

For each device, capture: board model and revision; vendor schematic URL and revision; firmware commit; build environment; antenna/test setup; regulatory region and settings; serial logs; expected and observed results; reviewer and date.

## Safety constraints

- Do not reuse a pin mapping from another board or assume two revisions match.
- Never flash the current T1000-E build probe; it is not device firmware.
- RAK4631 does not yet have a verified GhostWave hardware target.
- Do not mark a row passed based solely on compilation or host simulation.
- Keep `scripts/check-release-readiness.py` blocking release until evidence is reviewed.
