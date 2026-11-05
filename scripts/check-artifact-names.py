#!/usr/bin/env python3
"""Fail if generic compile probes are advertised as flashable device firmware."""
from pathlib import Path
import sys
root = Path(__file__).resolve().parents[1]
bad = []
for workflow in (root / ".github/workflows").glob("*.yml"):
    content = workflow.read_text().lower()
    if "nrf52840_compile_probe" in content and ("release-t1000e" in content or "release-rak4631" in content):
        bad.append(str(workflow.relative_to(root)))
if bad:
    print("BLOCKED: compile probe labeled as device firmware:", ", ".join(bad), file=sys.stderr)
    sys.exit(1)
print("No misleading generic probe release labels detected")
