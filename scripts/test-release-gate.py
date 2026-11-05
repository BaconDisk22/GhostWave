#!/usr/bin/env python3
"""Ensure the prototype's release gate fails closed with an actionable reason."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
result = subprocess.run(
    [sys.executable, str(root / "scripts/check-release-readiness.py")],
    cwd=root, capture_output=True, text=True, check=False,
)
output = result.stdout + result.stderr
if result.returncode == 0 or "BLOCKED" not in output:
    print(output, file=sys.stderr)
    raise SystemExit("FAIL: release gate did not explicitly block")
print("PASS: prototype release gate is blocked")
