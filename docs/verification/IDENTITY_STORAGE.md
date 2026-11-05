# Persistent identity acceptance gates

This document defines tests, not an implemented storage driver.

- Persist Ed25519 seed only in protected storage, never plaintext logs.
- Preserve node identity across cold boot and interrupted writes.
- Reject corrupted or truncated identity records without silently generating a replacement.
- Test atomic update and power-loss recovery with an injectable storage backend.
- Verify peer key pinning and key-change alerts survive reboot.
- No release until tests run on both supported boards.
