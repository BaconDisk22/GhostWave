#!/usr/bin/env python3
"""Require reviewed, non-placeholder T1000-E pin evidence before board bring-up."""
import json
from pathlib import Path
root = Path(__file__).resolve().parents[1]
manifest = root / "firmware/t1000e/board-verified.json"
required = {"board_revision", "schematic_url", "bootloader", "sx1262_cs", "sx1262_dio1", "sx1262_busy", "sx1262_reset"}
if not manifest.exists():
    raise SystemExit("BLOCKED: T1000-E verified pin manifest is missing; generic compile probe is not flashable")
try:
    data = json.loads(manifest.read_text())
except (ValueError, OSError) as exc:
    raise SystemExit(f"BLOCKED: invalid T1000-E manifest: {exc}")
if not isinstance(data, dict):
    raise SystemExit("BLOCKED: T1000-E manifest must be a JSON object")
missing = sorted(k for k in required if data.get(k) is None or data.get(k) == "")
if missing:
    raise SystemExit("BLOCKED: incomplete T1000-E manifest: " + ", ".join(missing))
pins = [data[k] for k in ("sx1262_cs", "sx1262_dio1", "sx1262_busy", "sx1262_reset")]
if any(type(p) is not int or p < 0 for p in pins) or len(set(pins)) != 4:
    raise SystemExit("BLOCKED: T1000-E SX1262 pins must be distinct nonnegative integers")
for field in ("board_revision", "schematic_url", "bootloader"):
    if not isinstance(data[field], str) or data[field].strip().lower() in {"todo", "unknown", "tbd", "placeholder"}:
        raise SystemExit(f"BLOCKED: T1000-E {field} needs reviewed evidence")
print("T1000-E manifest fields structurally valid; physical verification still required")
