# Firmware artifact provenance

This is a specification, not an authorization to flash or release a binary.

Each future board-specific build must record commit SHA, target board and PCB revision, bootloader compatibility, toolchain and dependency versions, artifact SHA-256, build log URL, size and flash partition limits, and pin-evidence review.

CI must reject ambiguous board labels and generic compile-probe binaries masquerading as device firmware. Signing, reproducibility, boot tests, and physical hardware evidence must be reviewed separately. The release gate stays fail-closed.
