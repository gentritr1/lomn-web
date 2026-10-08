"""mutate4.run() must hand the caller EVERY failure line. It used to keep only the last 1,500 characters, so a mutation
that broke several checks lost its first (targeted) FAIL line and was scored MISSED although the targeted check failed
(entry #23 "C1 main_web: a lost device does not reach the page state", final M1 gate 2026-10-08: 7 FAIL lines, the C1
line first)."""
from __future__ import annotations

import shlex
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "research" / "oracle"))
import mutate4  # noqa: E402


def test_run_keeps_the_first_of_many_long_fail_lines(tmp_path):
    lines = ["FAIL C1: GPU process crash -> state error with the reason and a Reload button :: area-ready"]
    lines += [f"FAIL other check {i} :: " + "x" * 300 for i in range(8)]
    script = tmp_path / "emit.py"
    script.write_text("import sys\n" + "".join(f"print({l!r})\n" for l in lines) + "sys.exit(1)\n")
    rc, out, _ = mutate4.run(f"{shlex.quote(sys.executable)} {shlex.quote(str(script))}", str(tmp_path), 60)
    assert rc == 1
    assert "FAIL C1: GPU process crash" in out
    assert out.count("FAIL") == len(lines)


def test_run_without_fail_lines_still_returns_the_tail(tmp_path):
    rc, out, _ = mutate4.run(shlex.quote(sys.executable) + " -c \"print('plain output'); raise SystemExit(2)\"", str(tmp_path), 60)
    assert rc == 2 and "plain output" in out
