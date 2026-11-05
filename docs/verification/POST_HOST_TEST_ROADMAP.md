# GhostWave remaining blockers after host-side PRs

Host-side tests and documentation are not evidence of a functioning radio mesh.

## Required implementation work
1. Obtain manufacturer-confirmed T1000-E and RAK4631 board revisions, bootloader details and radio pin mappings.
2. Implement a real SX1262 driver and receive/IRQ integration with bounded recovery.
3. Create independently verified board-specific firmware targets; keep the generic probe nonflashable.
4. Integrate secure identity persistence and reboot-safe message ID allocation into device storage.
5. Exercise encrypted DATA, SOS, acknowledgement, retries and duplicate suppression end-to-end.
6. Record physical test logs and safety/regulatory evidence for each board.
7. Resolve hardware failures, perform security review and only then consider a release candidate.

## Explicit exclusions
No claim of flash-ready firmware, working RF transmission, successful SOS delivery or release readiness is made by this roadmap. The release gate must remain fail-closed until qualification.
