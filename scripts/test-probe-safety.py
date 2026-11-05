#!/usr/bin/env python3
"""Regression wrapper for the intentionally non-transmitting compile probe."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
result = subprocess.run(
    [sys.executable, str(root / "scripts/check-probe-no-tx.py")],
    cwd=root, capture_output=True, text=True, check=False,
)
if result.returncode or "PASS:" not in result.stdout:
    print(result.stdout + result.stderr, file=sys.stderr)
    raise SystemExit("FAIL: probe transmission safety check")
print("PASS: probe safety contract")
