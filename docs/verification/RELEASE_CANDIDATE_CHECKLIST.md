# Release candidate checklist — NOT READY

This is a future checklist. No flashable GhostWave hardware firmware or release is authorized.

- [ ] Verified T1000-E board and bootloader support
- [ ] Verified RAK4631 baseboard and SX1262 pin assignments
- [ ] Build provenance and binary hash for each hardware target
- [ ] Physical receive, encrypted messaging, SOS and recovery evidence
- [ ] Power-loss and persistence validation
- [ ] Regional RF compliance and operator-controlled TX enablement
- [ ] Documented backup, rollback and recovery steps for each device
- [ ] Security review of key handling, nonces and logging
- [ ] Independent reviewer approval and deliberate release-gate change

The generic nRF52840 compile probe is not a device artifact and must not be packaged as one.
