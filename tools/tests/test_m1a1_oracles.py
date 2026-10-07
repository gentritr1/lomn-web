"""M1a-1 acceptance A1–A6: lomn_datacheck (C++) vs independent Python oracles over the real install."""
import re
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
EXE = REPO / "build" / "native" / "lomn_datacheck"


def test_datacheck_matches_oracles(lomn_data):
    if not EXE.exists():
        pytest.skip("SKIPPED (build lomn_datacheck first: make native)")
    r = subprocess.run([sys.executable, str(REPO / "tools/research/oracle/check_datacheck.py"),
                        "--exe", str(EXE), "--install", str(lomn_data / "install")],
                       capture_output=True, text=True, timeout=1200)
    assert r.returncode == 0, r.stdout[-4000:] + r.stderr[-2000:]
    m = re.search(r"(\d+)/(\d+) checks passed", r.stdout)
    assert m, r.stdout[-2000:]
    passed, total = int(m.group(1)), int(m.group(2))
    assert passed == total and total >= 39, f"{passed}/{total} checks passed"
