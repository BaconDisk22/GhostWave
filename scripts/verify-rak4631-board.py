#!/usr/bin/env python3
"""Fail closed until RAK4631 and baseboard pin mappings are reviewed."""
import json
from pathlib import Path
root = Path(__file__).resolve().parents[1]
manifest = root / "firmware/rak4631/board-verified.json"
required = {"module_revision", "baseboard_revision", "schematic_url", "bootloader", "sx1262_cs", "sx1262_dio1", "sx1262_busy", "sx1262_reset"}
if not manifest.exists():
    raise SystemExit("BLOCKED: RAK4631 verified board manifest missing; do not flash generic probe")
data = json.loads(manifest.read_text())
missing = sorted(k for k in required if not data.get(k))
if missing:
    raise SystemExit("BLOCKED: incomplete RAK4631 manifest: " + ", ".join(missing))
print("Manifest fields present; physical verification still required")
