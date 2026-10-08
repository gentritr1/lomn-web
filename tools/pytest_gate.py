#!/usr/bin/env python3
"""The `make test-tools` gate over pytest's own report (M1b T7, tightened in T8 P15).

Input: the log of `pytest -q -rsxX tools` (build/pytest-tools.log). A skipped test is not a pass and an expected failure is
not a pass either, so the gate fails unless every skip / xfail / xpass is accounted for:

  SKIPPED [n] file:line: reason   allowed when the whole line matches --allow-skip (a regex; EMPTY = nothing is allowed
                                  that way) OR, with --no-data (LOMN_DATA is unset), when the line ENDS with the reason
                                  `(no LOMN_DATA)`. The data-free subset is all that can run then. The tolerance is anchored
                                  to the end of the line (T8 P15): a reason that merely mentions LOMN_DATA ("needs node and
                                  LOMN_DATA") no longer hides a missing node.
  XFAIL path::test - reason       failure unless the line matches --allow-xfail (EMPTY = none allowed).
  XPASS path::test - reason       the same: an unexpectedly passing xfail is a stale marker, so it fails too.

Cross-check: the sum of the [n] skip counts, the XFAIL lines and the XPASS lines must equal the "N skipped", "N xfailed",
"N xpassed" of pytest's summary line, so a changed report format (or a missing -r flag) cannot hide any of them.
Exit 0 when everything is accounted for, 1 otherwise; the last line printed says why.
"""
from __future__ import annotations

import argparse
import re
import sys

SKIP_LINE = re.compile(r"^SKIPPED \[(\d+)\]")
NODATA_SUFFIX = re.compile(r"\(no LOMN_DATA\)\s*$")
SUMMARY = re.compile(r"\bin [0-9.]+s\b")


def summary_count(lines: list[str], word: str) -> int:
    n = 0
    for line in lines:
        if SUMMARY.search(line):
            m = re.search(r"(\d+) " + word + r"\b", line)
            if m:
                n = int(m.group(1))
    return n


def gate(lines: list[str], allow_skip: str = "", allow_xfail: str = "", no_data: bool = False) -> tuple[bool, str]:
    skip_re = re.compile(allow_skip) if allow_skip else None
    xfail_re = re.compile(allow_xfail) if allow_xfail else None
    listed_skip = bad_skip = listed_xfail = listed_xpass = bad_x = 0
    out: list[str] = []
    for line in lines:
        m = SKIP_LINE.match(line)
        if m:
            n = int(m.group(1))
            listed_skip += n
            if (skip_re and skip_re.search(line)) or (no_data and NODATA_SUFFIX.search(line)):
                continue
            bad_skip += n
            out.append("  unexpected skip: " + line.rstrip())
        elif line.startswith("XFAIL ") or line.startswith("XPASS "):
            if line.startswith("XFAIL "):
                listed_xfail += 1
            else:
                listed_xpass += 1
            if xfail_re and xfail_re.search(line):
                continue
            bad_x += 1
            out.append("  unexpected " + line[:5].lower() + ": " + line.rstrip())
    for word, listed in (("skipped", listed_skip), ("xfailed", listed_xfail), ("xpassed", listed_xpass)):
        said = summary_count(lines, word)
        if said != listed:
            out.append(f"test-tools: summary says {said} {word} but the report lists {listed}")
            return False, "\n".join(out)
    if bad_skip:
        out.append(f"test-tools: FAILED, {bad_skip} unexpected pytest skip(s) (a skip is not a pass); "
                   f"allowlist PYTEST_SKIP_ALLOW is {allow_skip or 'empty'}")
    if bad_x:
        out.append(f"test-tools: FAILED, {bad_x} unexpected xfail/xpass (an expected failure is not a pass either); "
                   f"allowlist PYTEST_XFAIL_ALLOW is {allow_xfail or 'empty'}")
    if bad_skip or bad_x:
        return False, "\n".join(out)
    out.append(f"test-tools: {listed_skip} pytest skip(s), {listed_xfail} xfail(s), {listed_xpass} xpass(es), all allowed")
    return True, "\n".join(out)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("--allow-skip", default="")
    ap.add_argument("--allow-xfail", default="")
    ap.add_argument("--no-data", action="store_true", help="LOMN_DATA is unset: '(no LOMN_DATA)' skips are tolerated")
    a = ap.parse_args(argv)
    with open(a.log, encoding="utf-8", errors="replace") as f:
        ok, msg = gate(f.read().splitlines(), a.allow_skip, a.allow_xfail, a.no_data)
    print(msg)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
