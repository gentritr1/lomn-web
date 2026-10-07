"""run_decomp wrapper logic that needs no Ghidra: cache path, process-group kill on timeout, analysis marker key."""
import os
import sys
import time
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "decomp"))
import run_decomp  # noqa: E402


def test_default_cache_has_no_dot_element(monkeypatch):
    for plat in ("darwin", "linux"):
        monkeypatch.setattr(sys, "platform", plat)
        p = run_decomp.default_cache()
        assert not any(part.startswith(".") for part in p.resolve().parts), p


def test_check_cache_path_rejects_dot_elements(tmp_path):
    run_decomp.check_cache_path(tmp_path / "ok" / "proj")  # must not raise
    with pytest.raises(SystemExit, match=r"path element starting with '\.'"):
        run_decomp.check_cache_path(tmp_path / ".hidden" / "decomp" / "proj")


def test_ghidra_timeout_kills_process_group(tmp_path, monkeypatch):
    home = tmp_path / "ghidra"
    (home / "support").mkdir(parents=True)
    pidfile = tmp_path / "child.pid"
    script = home / "support" / "analyzeHeadless"
    script.write_text(f"#!/bin/bash\nsleep 300 &\necho $! > '{pidfile}'\nsleep 300\n")
    script.chmod(0o755)
    monkeypatch.setattr(run_decomp, "GHIDRA_HOME", home)
    monkeypatch.setenv("LOMN_GHIDRA_TIMEOUT_S", "2")
    t0 = time.monotonic()
    with pytest.raises(SystemExit, match="timed out"):
        run_decomp.ghidra(tmp_path / "proj", [], {}, tmp_path / "log.txt")
    assert time.monotonic() - t0 < 35
    pid = int(pidfile.read_text())
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            break
        time.sleep(0.1)
    with pytest.raises(ProcessLookupError):
        os.kill(pid, 0)


def test_analysis_key_tracks_inputs(tmp_path):
    dec = tmp_path / "dec"
    dec.mkdir()
    (dec / "types.txt").write_text("t")
    (dec / "symbols.txt").write_text("s")
    scripts = tmp_path / "scripts"
    scripts.mkdir()
    (scripts / "A.java").write_text("a")
    (scripts / "B.java").write_text("b")
    k = run_decomp.analysis_key("sha", dec, scripts)
    assert k == run_decomp.analysis_key("sha", dec, scripts)
    assert k != run_decomp.analysis_key("sha2", dec, scripts)
    (scripts / "B.java").write_text("b2")
    k2 = run_decomp.analysis_key("sha", dec, scripts)
    assert k2 != k
    (dec / "symbols.txt").write_text("s2")
    assert run_decomp.analysis_key("sha", dec, scripts) != k2
