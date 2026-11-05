#!/usr/bin/env python3
"""Fail closed if the nonflashable nRF52840 probe gains RF TX plumbing."""
from pathlib import Path
import re
root = Path(__file__).resolve().parents[1]
source = (root / "firmware/t1000e/src/main.cpp").read_text()
config = (root / "firmware/t1000e/platformio.ini").read_text()
assert "not firmware for any device" in source, "missing nonflashable warning"
assert re.search(r"bool transmit\([^)]*\) override\s*\{\s*return false;", source), "probe must reject TX"
assert "nrf52840_compile_probe" in config, "probe environment missing"
assert "adafruit_feather_nrf52840" in config, "unexpected board target; review before changing"
assert not re.search(r"\b(?:radio\.begin|radio\.transmit|startTransmit)\s*\(", source), "radio TX call in probe"
print("Compile-only probe remains nontransmitting and nonflashable.")
