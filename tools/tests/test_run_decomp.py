"""run_decomp wrapper logic that needs no Ghidra: cache path, process-group kill on timeout, analysis marker key."""
import os
import subprocess
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


def _fake_ghidra(tmp_path, monkeypatch) -> Path:
    """analyzeHeadless stand-in: starts a background child (pid in a file), then sleeps itself."""
    home = tmp_path / "ghidra"
    (home / "support").mkdir(parents=True)
    pidfile = tmp_path / "child.pid"
    script = home / "support" / "analyzeHeadless"
    script.write_text(f"#!/bin/bash\nsleep 300 &\necho $! > '{pidfile}'\nsleep 300\n")
    script.chmod(0o755)
    monkeypatch.setattr(run_decomp, "GHIDRA_HOME", home)
    return pidfile


def _assert_gone(pidfile: Path) -> None:
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


def test_ghidra_timeout_kills_process_group(tmp_path, monkeypatch):
    pidfile = _fake_ghidra(tmp_path, monkeypatch)
    monkeypatch.setenv("LOMN_GHIDRA_TIMEOUT_S", "2")
    t0 = time.monotonic()
    with pytest.raises(SystemExit, match="timed out"):
        run_decomp.ghidra(tmp_path / "proj", [], {}, tmp_path / "log.txt")
    assert time.monotonic() - t0 < 35
    _assert_gone(pidfile)


def test_ghidra_interrupt_kills_process_group(tmp_path, monkeypatch):
    pidfile = _fake_ghidra(tmp_path, monkeypatch)
    real_wait = subprocess.Popen.wait
    calls = []

    def wait(self, timeout=None):
        if not calls:
            calls.append(1)
            deadline = time.monotonic() + 10  # let the fake start its child before the "Ctrl-C"
            while not pidfile.exists() and time.monotonic() < deadline:
                time.sleep(0.05)
            raise KeyboardInterrupt
        return real_wait(self, timeout)

    monkeypatch.setattr(subprocess.Popen, "wait", wait)
    with pytest.raises(KeyboardInterrupt):
        run_decomp.ghidra(tmp_path / "proj", [], {}, tmp_path / "log.txt")
    _assert_gone(pidfile)


def test_analysis_key_tracks_inputs(tmp_path):
    dec = tmp_path / "dec"
    dec.mkdir()
    (dec / "types.txt").write_text("t")
    (dec / "symbols.txt").write_text("s")
    scripts = tmp_path / "scripts"
    scripts.mkdir()
    for n in run_decomp.ANALYSIS_SCRIPTS:
        (scripts / n).write_text(n)
    (scripts / "ExportCorpus.java").write_text("e")
    k = run_decomp.analysis_key("sha", dec, scripts)
    assert k == run_decomp.analysis_key("sha", dec, scripts)
    assert k != run_decomp.analysis_key("sha2", dec, scripts)
    (scripts / "ExportCorpus.java").write_text("e2")  # export-only script: must NOT force re-analysis
    assert run_decomp.analysis_key("sha", dec, scripts) == k
    (scripts / "ApplyCVLocals.java").write_text("changed")
    k2 = run_decomp.analysis_key("sha", dec, scripts)
    assert k2 != k
    (dec / "symbols.txt").write_text("s2")
    assert run_decomp.analysis_key("sha", dec, scripts) != k2
