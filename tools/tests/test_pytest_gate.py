"""T8 P15: the `make test-tools` gate (tools/pytest_gate.py) on synthetic pytest reports. The tolerance for skips without
LOMN_DATA is anchored to the reason `(no LOMN_DATA)` at the END of the line, and xfail / xpass fail the gate unless
allowlisted. Each case is a log as `pytest -q -rsxX` prints it."""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
import pytest_gate  # noqa: E402

DATA_SKIP = "SKIPPED [3] tools/tests/conftest.py:17: SKIPPED (no LOMN_DATA)"
NODE_AND_DATA_SKIP = "SKIPPED [1] tools/tests/test_byo_fingerprint.py:97: needs node and LOMN_DATA"
NODE_SKIP = "SKIPPED [1] tools/tests/test_byo_fingerprint.py:33: node not found"


def report(*lines: str, summary: str) -> list[str]:
    return ["........s.....", "=========================== short test summary info ===========================", *lines, summary]


def run(lines: list[str], **kw):
    return pytest_gate.gate(lines, **kw)


def test_clean_run_passes() -> None:
    ok, msg = run(report(summary="120 passed in 4.50s"))
    assert ok and "0 pytest skip(s)" in msg


def test_no_data_skips_are_tolerated_only_without_lomn_data() -> None:
    lines = report(DATA_SKIP, summary="100 passed, 3 skipped in 4.50s")
    assert run(lines, no_data=True)[0]
    ok, msg = run(lines, no_data=False)  # LOMN_DATA is set but the tests still skipped on it: a real failure
    assert not ok and "unexpected skip" in msg


def test_a_reason_that_only_mentions_lomn_data_does_not_hide_a_missing_node() -> None:
    # Before T8 P15 the gate matched /LOMN_DATA/ anywhere in the line, so "needs node and LOMN_DATA" passed with LOMN_DATA unset.
    ok, msg = run(report(NODE_AND_DATA_SKIP, summary="100 passed, 1 skipped in 1.00s"), no_data=True)
    assert not ok and "needs node and LOMN_DATA" in msg
    ok, msg = run(report(NODE_SKIP, summary="100 passed, 1 skipped in 1.00s"), no_data=True)
    assert not ok and "node not found" in msg
    # the tolerated text must be the END of the line, not the middle of it
    middle = "SKIPPED [1] tools/tests/x.py:1: SKIPPED (no LOMN_DATA) but also node is missing"
    assert not run(report(middle, summary="1 skipped in 0.10s"), no_data=True)[0]


def test_the_allowlist_still_works_and_counts_are_cross_checked() -> None:
    lines = report(NODE_SKIP, summary="100 passed, 1 skipped in 1.00s")
    assert run(lines, allow_skip="node not found")[0]
    ok, msg = run(report(DATA_SKIP, summary="100 passed, 2 skipped in 1.00s"), no_data=True)  # report format changed / a skip is unlisted
    assert not ok and "summary says 2 skipped but the report lists 3" in msg


def test_xfail_fails_the_gate_unless_allowlisted() -> None:
    lines = report("XFAIL tools/tests/test_x.py::test_known_gap - tracked in #12", summary="100 passed, 1 xfailed in 1.00s")
    ok, msg = run(lines)
    assert not ok and "unexpected xfail" in msg
    assert run(lines, allow_xfail="test_known_gap")[0]
    assert not run(lines, allow_xfail="some_other_test")[0]


def test_xpass_fails_the_gate_unless_allowlisted() -> None:
    lines = report("XPASS tools/tests/test_x.py::test_stale_marker - was broken", summary="100 passed, 1 xpassed in 1.00s")
    ok, msg = run(lines)
    assert not ok and "unexpected xpass" in msg
    assert run(lines, allow_xfail="test_stale_marker")[0]


def test_an_xfail_missing_from_the_report_is_caught_by_the_summary_cross_check() -> None:
    # pytest run without -rx: the summary says 1 xfailed but no XFAIL line is listed.
    ok, msg = run(report(summary="100 passed, 1 xfailed in 1.00s"))
    assert not ok and "summary says 1 xfailed but the report lists 0" in msg


def test_cli_exit_codes(tmp_path: Path) -> None:
    good, bad = tmp_path / "good.log", tmp_path / "bad.log"
    good.write_text("\n".join(report(DATA_SKIP, summary="100 passed, 3 skipped in 1.00s")) + "\n")
    bad.write_text("\n".join(report(NODE_AND_DATA_SKIP, summary="100 passed, 1 skipped in 1.00s")) + "\n")
    cmd = [sys.executable, str(REPO / "tools" / "pytest_gate.py")]
    assert subprocess.run(cmd + [str(good), "--no-data"], capture_output=True, text=True).returncode == 0
    assert subprocess.run(cmd + [str(good)], capture_output=True, text=True).returncode == 1
    r = subprocess.run(cmd + [str(bad), "--no-data"], capture_output=True, text=True)
    assert r.returncode == 1 and "needs node and LOMN_DATA" in r.stdout
