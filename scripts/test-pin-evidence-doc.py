#!/usr/bin/env python3
"""Static documentation check, not hardware qualification."""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
text = (root / "docs/hardware/BOARD_PIN_EVIDENCE.md").read_text()
required = ["PCB revision", "schematic URL", "SPI bus", "NSS", "RESET", "BUSY", "DIO1", "TCXO", "RF switch", "Reviewer"]
missing = [item for item in required if item not in text]
if missing:
    raise SystemExit("Missing pin-evidence fields: " + ", ".join(missing))
print("Pin-evidence documentation fields present; no hardware verified.")
