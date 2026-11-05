# Hardware evidence policy

GhostWave host tests, virtual mesh runs, and generic nRF52840 compilation **do not** qualify a device firmware image.

## Evidence needed before hardware readiness
- Record exact device and baseboard revision, manufacturer pinout source, bootloader, and recovery method.
- Review SPI, DIO1, BUSY, RESET, TCXO, RF switch, antenna and power sequencing against that revision.
- Record firmware commit, build tool versions, binary hash and device serial logs.
- Verify boot and receive-only behavior first; RF transmission requires compliant regional settings and a controlled test setup.
- Verify encrypted delivery, SOS acknowledgement, retry limits, restart recovery and rollback on physical devices.
- Record failures and open follow-up issues; never mark untested rows as passed.

## Release ownership
Release approval requires explicit human review of hardware evidence and a separate change to the fail-closed release gate. No documentation or CI change alone grants approval.
