#!/usr/bin/env python3
"""Exercise fail-closed manifest validation without modifying the repository."""
import json
import subprocess
import sys
import tempfile
from pathlib import Path
root = Path(__file__).resolve().parents[1]
for board, name, metadata in (
    ("t1000e", "verify-t1000e-board.py", {"board_revision": "rev-A", "schematic_url": "https://example.org/schematic", "bootloader": "reviewed"}),
    ("rak4631", "verify-rak4631-board.py", {"module_revision": "rev-A", "baseboard_revision": "rev-B", "schematic_url": "https://example.org/schematic", "bootloader": "reviewed"}),
):
    original = (root / "scripts" / name).read_text()
    with tempfile.TemporaryDirectory() as td:
        temp = Path(td)
        (temp / "scripts").mkdir()
        (temp / "firmware" / board).mkdir(parents=True)
        script = temp / "scripts" / name
        script.write_text(original)
        manifest = temp / "firmware" / board / "board-verified.json"
        def check(data, expected):
            manifest.write_text(json.dumps(data))
            result = subprocess.run([sys.executable, str(script)], capture_output=True, text=True, check=False)
            if (result.returncode == 0) != expected:
                raise SystemExit(f"FAIL: {board} expected success={expected}: {result.stdout} {result.stderr}")
        good = dict(metadata, sx1262_cs=1, sx1262_dio1=2, sx1262_busy=3, sx1262_reset=4)
        check(good, True)
        check(dict(good, sx1262_busy=1), False)
        check(dict(good, sx1262_reset=-1), False)
        check(dict(good, sx1262_cs="TODO"), False)
        check(dict(good, schematic_url="TODO"), False)
        print(f"PASS: {board} pin collision and placeholder checks")
