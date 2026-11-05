#!/usr/bin/env python3
"""Host-side identity file security policy tests; never stores real keys."""
import os
import stat
import tempfile
from pathlib import Path

def secure_mode(path: Path) -> bool:
    try:
        st = path.lstat()
    except OSError:
        return False
    if not stat.S_ISREG(st.st_mode):
        return False
    mode = stat.S_IMODE(st.st_mode)
    return (mode & 0o077) == 0 and (mode & 0o600) == 0o600

with tempfile.TemporaryDirectory() as td:
    root = Path(td)
    key = root / "identity.seed"
    assert not secure_mode(key)
    fd = os.open(key, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    try:
        os.write(fd, b"test-only-not-a-real-key")
    finally:
        os.close(fd)
    assert secure_mode(key)
    for mode in (0o644, 0o640, 0o666, 0o400, 0o000):
        key.chmod(mode)
        assert not secure_mode(key), oct(mode)
    key.chmod(0o600)
    assert secure_mode(key)
    link = root / "identity.link"
    link.symlink_to(key)
    assert not secure_mode(link), "symlink must not be accepted as key file"
    key.unlink()
    key.mkdir()
    assert not secure_mode(key), "directory must not be accepted as key file"
print("PASS: identity file permission and symlink policy")
