#!/usr/bin/env python3
"""Host regression: unverified board manifests must block board bring-up."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
for board, script in [
    ("t1000e", "verify-t1000e-board.py"),
    ("rak4631", "verify-rak4631-board.py"),
]:
    manifest = root / "firmware" / board / "board-verified.json"
    if manifest.exists():
        print(f"SKIP {board}: manifest exists; review its evidence manually")
        continue
    proc = subprocess.run(
        [sys.executable, str(root / "scripts" / script)],
        capture_output=True, text=True, check=False,
    )
    if proc.returncode == 0 or "BLOCKED" not in proc.stderr + proc.stdout:
        raise SystemExit(f"FAIL: {board} missing manifest did not block")
    print(f"PASS: {board} missing manifest blocks bring-up")
