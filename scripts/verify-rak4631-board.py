#!/usr/bin/env python3
"""Require reviewed RAK4631/WisBlock pin evidence before board bring-up."""
import json
from pathlib import Path
root = Path(__file__).resolve().parents[1]
manifest = root / "firmware/rak4631/board-verified.json"
required = {"module_revision", "baseboard_revision", "schematic_url", "bootloader", "sx1262_cs", "sx1262_dio1", "sx1262_busy", "sx1262_reset"}
if not manifest.exists():
    raise SystemExit("BLOCKED: RAK4631 verified board manifest missing; do not flash generic probe")
try:
    data = json.loads(manifest.read_text())
except (ValueError, OSError) as exc:
    raise SystemExit(f"BLOCKED: invalid RAK4631 manifest: {exc}")
if not isinstance(data, dict):
    raise SystemExit("BLOCKED: RAK4631 manifest must be a JSON object")
missing = sorted(k for k in required if data.get(k) is None or data.get(k) == "")
if missing:
    raise SystemExit("BLOCKED: incomplete RAK4631 manifest: " + ", ".join(missing))
pins = [data[k] for k in ("sx1262_cs", "sx1262_dio1", "sx1262_busy", "sx1262_reset")]
if any(type(p) is not int or p < 0 for p in pins) or len(set(pins)) != 4:
    raise SystemExit("BLOCKED: RAK4631 SX1262 pins must be distinct nonnegative integers")
for field in ("module_revision", "baseboard_revision", "schematic_url", "bootloader"):
    if not isinstance(data[field], str) or data[field].strip().lower() in {"todo", "unknown", "tbd", "placeholder"}:
        raise SystemExit(f"BLOCKED: RAK4631 {field} needs reviewed evidence")
print("RAK4631 manifest fields structurally valid; physical verification still required")
