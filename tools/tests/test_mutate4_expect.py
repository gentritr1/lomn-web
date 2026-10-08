"""T8 P17: mutation #29 ("I2 main_web: expectation from the snapped p50 again (circular)") may only be scored CAUGHT by the
two I2 spin checks. Its EXPECT pattern was the bare r"I2", which also matches the load-sensitive "every unspun page ... is NOT
DROPPING" check, so on a loaded host (unspun pages at 52.7 fps) #29 could be CAUGHT with no spin check failing. The FAIL
lines below are from a real run (mutate4 --only 29, 60 Hz display, loadavg 7-30, 2026-10-07 23:07), including that false one."""
from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "research" / "oracle"))
import mutate4  # noqa: E402

NAME = "I2 main_web: expectation from the snapped p50 again (circular)"
SPIN_FAILS = [
    "FAIL I2: a 33 ms spin per frame -> fps below 0.95 x the display rate measured on that page -> DROPPING :: display 60 Hz, fps 29.422, expected 122.358 intervals, dropping false",
    "FAIL I2: ?debug=1&debug_spin=33 makes DROPPING fire :: fps 29.913 vs display 60 Hz, spin 33",
]
LOAD_FLAKE = ('FAIL I2: every unspun page (13 sampled) is NOT DROPPING against the display rate measured on that same page :: '
              'DROPPING: [{"url":"index.html?area=levt/test&pick=1","dropping":true,"fps":52.711,"hz":60,"snapped":true}]')
OTHER_I2 = [
    "FAIL I2: the display rate is measured before the first area; the unspun run is not DROPPING :: display 0 Hz",
    "FAIL I2: snap rule: standard rates only within +-3 %, raw and unsnapped otherwise; 58..62 Hz never reads as 72 :: x",
    "FAIL I2: 10 repeated measurements on one page give the same snapped rate (and the boot measurement agrees) :: x",
    "FAIL I2: the old circular expectation hides (or understates) the drop :: x",
]


def hit(line: str) -> bool:
    return re.search(mutate4.EXPECT[NAME], line) is not None


def test_both_spin_checks_are_targeted() -> None:
    assert all(hit(l) for l in SPIN_FAILS)


def test_the_load_sensitive_all_pages_check_is_not_a_bite() -> None:
    assert not hit(LOAD_FLAKE)


def test_other_i2_checks_are_not_the_spin_checks_either() -> None:
    assert not any(hit(l) for l in OTHER_I2)
