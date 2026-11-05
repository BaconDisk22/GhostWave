#!/usr/bin/env python3
"""Pure host regression for bounded exponential retry schedules."""
def retry_delay_ms(attempt: int, base_ms: int = 1000, cap_ms: int = 60000) -> int:
    if attempt < 0 or base_ms <= 0 or cap_ms < base_ms:
        raise ValueError("invalid retry policy")
    return min(cap_ms, base_ms * (1 << min(attempt, 32)))

expected = [1000, 2000, 4000, 8000, 16000, 32000, 60000, 60000]
actual = [retry_delay_ms(i) for i in range(len(expected))]
if actual != expected:
    raise SystemExit(f"FAIL: retry delays {actual} != {expected}")
if retry_delay_ms(1000000) != 60000:
    raise SystemExit("FAIL: retry delay exceeded cap")
for bad in (-1,):
    try:
        retry_delay_ms(bad)
    except ValueError:
        pass
    else:
        raise SystemExit("FAIL: negative retry attempt accepted")
print("PASS: bounded retry reference model")
