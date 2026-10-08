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
    (dec / "seed_addrs.txt").write_text("0x00401000\n")
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


def test_analysis_key_covers_seeds_and_scripts(tmp_path, monkeypatch):
    """M2a-0: the seed list, the seed extractor and both new analysis scripts shape the cached project, so each must
    force re-analysis when it changes (otherwise a stale cache silently drops the seeds or the sret fix)."""
    import shutil
    dec = tmp_path / "dec"
    dec.mkdir()
    (dec / "types.txt").write_text("t")
    (dec / "symbols.txt").write_text("s")
    (dec / "seed_addrs.txt").write_text("0x00401000\n0x005fab30\n")
    real = Path(run_decomp.__file__).resolve().parent / "ghidra"
    scripts = tmp_path / "scripts"
    scripts.mkdir()
    for n in run_decomp.ANALYSIS_SCRIPTS:
        shutil.copy(real / n, scripts / n)
    extractor = tmp_path / "seed_addrs.py"
    extractor.write_text("# extractor\n")
    monkeypatch.setattr(run_decomp, "SEED_EXTRACTOR", extractor, raising=False)
    k = run_decomp.analysis_key("sha", dec, scripts)
    (dec / "seed_addrs.txt").write_text("0x00401000\n0x005fab31\n")  # one byte changed
    k_seed = run_decomp.analysis_key("sha", dec, scripts)
    assert k_seed != k
    seen = {k, k_seed}
    for name in ("SeedFunctions.java", "ApplySret.java"):
        p = scripts / name
        p.write_text((p.read_text() if p.exists() else "") + "// changed\n")
        k_n = run_decomp.analysis_key("sha", dec, scripts)
        assert k_n not in seen, name
        seen.add(k_n)
    extractor.write_text("# extractor, rule changed\n")
    assert run_decomp.analysis_key("sha", dec, scripts) not in seen, "seed_addrs.py"


def _key_inputs(tmp_path):
    dec = tmp_path / "dec"
    dec.mkdir()
    for n in ("types.txt", "symbols.txt", "seed_addrs.txt"):
        (dec / n).write_text(n)
    scripts = tmp_path / "scripts"
    scripts.mkdir()
    for n in run_decomp.ANALYSIS_SCRIPTS:
        (scripts / n).write_text(n)
    return dec, scripts


def test_analysis_key_covers_ghidra_argv_env_and_version(tmp_path, monkeypatch):
    """Dropping or reordering a script flag, renaming an env key, or a different Ghidra must force re-analysis:
    otherwise the old project is reused and the stats are re-read from the old analyze.log."""
    dec, scripts = _key_inputs(tmp_path)
    home = tmp_path / "ghidra"
    (home / "Ghidra").mkdir(parents=True)
    props = home / "Ghidra" / "application.properties"
    props.write_text("application.name=Ghidra\napplication.version=12.1.4\n")
    monkeypatch.setattr(run_decomp, "GHIDRA_HOME", home)
    assert run_decomp.ghidra_version() == "12.1.4"
    k = run_decomp.analysis_key("sha", dec, scripts)
    seen = {k}

    def changed(label):
        k2 = run_decomp.analysis_key("sha", dec, scripts)
        assert k2 not in seen, label
        seen.add(k2)

    argv = tuple(run_decomp.ANALYSIS_ARGV)
    i = argv.index("ApplySret.java")
    with monkeypatch.context() as m:
        m.setattr(run_decomp, "ANALYSIS_ARGV", argv[:i - 1] + argv[i + 1:])  # -postScript ApplySret.java dropped
        changed("ApplySret dropped")
    j = argv.index("SeedFunctions.java")
    with monkeypatch.context() as m:  # SeedFunctions moved before ApplyCVSymbols
        m.setattr(run_decomp, "ANALYSIS_ARGV", ("-preScript", "ApplyCVTypes2.java", "-preScript", "SeedFunctions.java",
                                                "-preScript", "ApplyCVSymbols.java") + argv[j + 1:])
        changed("pre-scripts reordered")
    env = dict(run_decomp.ANALYSIS_ENV)
    with monkeypatch.context() as m:
        env2 = {("SEEDS" if kk == "SEED_ADDRS" else kk): v for kk, v in env.items()}
        m.setattr(run_decomp, "ANALYSIS_ENV", env2)
        changed("SEED_ADDRS renamed")
    assert run_decomp.analysis_key("sha", dec, scripts) == k  # contexts restored
    props.write_text("application.version=12.2\n")
    changed("Ghidra 12.2")
    props.unlink()
    assert run_decomp.ghidra_version() == "unknown"
    changed("Ghidra version unknown")


def test_analysis_argv_scripts_are_keyed():
    """Every script the analysis command runs is in ANALYSIS_SCRIPTS (so its content is hashed), and run() and the key
    use the same env mapping."""
    named = {a for a in run_decomp.ANALYSIS_ARGV if a.endswith(".java")}
    assert named and named <= set(run_decomp.ANALYSIS_SCRIPTS)
    assert set(run_decomp.analysis_env(Path("/d"))) == set(run_decomp.ANALYSIS_ENV)


def _fake_log(cache, seeds="SEED created=1448 existing=0 failed=0", sret="SRET retyped=63 failed=0"):
    cache.mkdir(parents=True, exist_ok=True)
    (cache / "analyze.log").write_text(
        "INFO  ApplyCVTypes2.java> CV2 structs=473 members applied=1814 failed=3 (GhidraScript)\n"
        "INFO  ApplyCVSymbols.java> CV labels applied=7042 skipped=16 (GhidraScript)\n"
        f"INFO  SeedFunctions.java> {seeds} (GhidraScript)\n"
        "INFO  ApplyCVLocals.java> CV locals: functions=580 params=259 locals=565 failed=441 (GhidraScript)\n"
        "INFO  ApplyD3D8Types.java> D3D8 interfaces=9 globals typed=4 (GhidraScript)\n"
        f"INFO  ApplySret.java> {sret} (GhidraScript)\n")


def test_finish_analysis_writes_marker_only_when_complete(tmp_path):
    cache = tmp_path / "cache"
    _fake_log(cache)
    stats = run_decomp.finish_analysis(cache, "KEY", n_seeds=1448)
    assert stats["ghidra_seeds_created"] == 1448 and stats["ghidra_sret_retyped"] == 63
    assert (cache / "analysis.ok").read_text() == "KEY\n"
    for label, kw, n, msg in (
        ("seed failed", dict(seeds="SEED created=1447 existing=0 failed=1"), 1448, "incomplete"),
        ("sret failed", dict(sret="SRET retyped=62 failed=1"), 1448, "incomplete"),
        ("seed totals", dict(seeds="SEED created=1446 existing=1 failed=0"), 1448, "seed totals"),
        ("stale seed list", {}, 1449, "seed totals"),
        ("nothing retyped", dict(sret="SRET retyped=0 failed=0"), 1448, "retyped"),
        ("stat missing", dict(sret="(ApplySret did not run)"), 1448, "missing"),
    ):
        c = tmp_path / label.replace(" ", "_")
        _fake_log(c, **kw)
        with pytest.raises(SystemExit, match=msg):
            run_decomp.finish_analysis(c, "KEY", n_seeds=n)
        assert not (c / "analysis.ok").exists(), label
