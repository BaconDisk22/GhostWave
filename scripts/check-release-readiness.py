#!/usr/bin/env python3
"""Fail closed until real hardware and release requirements are verified."""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
required = [root / 'firmware' / 't1000e' / 'platformio.ini', root / 'docs' / 'DESIGN.md', root / 'firmware' / 'tests']
missing = [str(p.relative_to(root)) for p in required if not p.exists()]
if missing:
    print('Missing prototype files:', ', '.join(missing), file=sys.stderr)
    sys.exit(1)
print('GhostWave remains a prototype. Hardware qualification and release approval are not recorded.')
print('Release gate: BLOCKED (intentional). No production release is authorized.')
sys.exit(1)
