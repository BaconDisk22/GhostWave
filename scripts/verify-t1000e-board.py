#!/usr/bin/env python3
"""Fail closed until a real T1000-E board manifest is supplied and reviewed."""
import json
from pathlib import Path
root = Path(__file__).resolve().parents[1]
manifest = root / "firmware/t1000e/board-verified.json"
required = {"board_revision", "schematic_url", "bootloader", "sx1262_cs", "sx1262_dio1", "sx1262_busy", "sx1262_reset"}
if not manifest.exists():
    raise SystemExit("BLOCKED: T1000-E verified pin manifest is missing; generic compile probe is not flashable")
data = json.loads(manifest.read_text())
missing = sorted(k for k in required if not data.get(k))
if missing:
    raise SystemExit("BLOCKED: incomplete T1000-E manifest: " + ", ".join(missing))
print("Manifest fields present; hardware verification still required")
