#!/usr/bin/env python3
"""Fail CI if the generic nRF52840 compile probe gains a transmitting radio."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
source = (root / "firmware/t1000e/src/main.cpp").read_text()
platformio = (root / "firmware/t1000e/platformio.ini").read_text()
assert "never flash" in source.lower()
assert "MUST NOT be flashed" in platformio
assert "[env:nrf52840_compile_probe]" in platformio
assert re.search(r"bool\s+transmit\s*\([^)]*\)\s*override\s*\{\s*return\s+false\s*;", source), "Probe transmit must remain disabled"
assert "board = adafruit_feather_nrf52840" in platformio
print("PASS: generic compile probe explicitly refuses transmissions")
