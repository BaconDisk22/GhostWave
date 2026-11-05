#!/usr/bin/env python3
"""Host-side identity-file permission regression; no keys are generated."""
import os
import stat
import tempfile
from pathlib import Path

def secure_mode(path: Path) -> bool:
    mode = stat.S_IMODE(path.stat().st_mode)
    return (mode & 0o077) == 0 and (mode & 0o600) == 0o600

with tempfile.TemporaryDirectory() as td:
    path = Path(td) / "identity.seed"
    fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    os.write(fd, b"test-only-not-a-real-key")
    os.close(fd)
    if not secure_mode(path):
        raise SystemExit("FAIL: private identity file not restricted")
    path.chmod(0o644)
    if secure_mode(path):
        raise SystemExit("FAIL: world-readable identity file accepted")
    path.chmod(0o600)
    if not secure_mode(path):
        raise SystemExit("FAIL: restored private permissions rejected")
print("PASS: identity file permission policy")
