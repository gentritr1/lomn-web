"""M2a-2 T6: `lomn_view --install DIR --script 1` runs the game's own script (Root/Data/base.osi) headless through
lomn::osi::ScriptHost: the parser rules (I10: every bad combination is a usage error, exit 2; an unreadable install or
Base.osi is exit 3), the A8 coverage report (byte for byte what tools/osi/coverage.py prints for the run's natives.log),
and "lomn_view adds no behaviour": with --natives stub its digest, natives.log and trace window equal lomn_osi_run's on
the same script. The data-free cases use engine/tests/data/osi_a9_smoke.asm; the real-install cases need LOMN_DATA.
The C++ side needs build/native (make test-tools builds it)."""
import json
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
VIEW = REPO / "build" / "native" / "lomn_view"
OSI_RUN = REPO / "build" / "native" / "lomn_osi_run"
OSIASM = REPO / "tools" / "osi" / "osiasm.py"
COVERAGE = REPO / "tools" / "osi" / "coverage.py"
SMOKE_ASM = REPO / "engine" / "tests" / "data" / "osi_a9_smoke.asm"
KEYS = "down@100 down@160 enter@220 esc@280"


def _run(exe, *args, timeout=100):
    assert exe.is_file(), "build/native is missing: run `make native` first"
    return subprocess.run([str(exe), *map(str, args)], capture_output=True, text=True, timeout=timeout)


def _coverage_lines(stdout):
    return [l for l in stdout.splitlines() if l.startswith("coverage: ") or l.startswith("native ")]


def _coverage_py(log):
    r = subprocess.run([sys.executable, str(COVERAGE), "--log", str(log)], capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stderr
    return r.stdout.splitlines()


def _d_lines(path):
    return [l for l in Path(path).read_text().splitlines() if l.startswith("D ")]


@pytest.fixture(scope="module")
def smoke(tmp_path_factory):
    d = tmp_path_factory.mktemp("osi_a9_smoke")
    (d / "install" / "data").mkdir(parents=True)
    osi = d / "install" / "data" / "Base.osi"
    r = subprocess.run([sys.executable, str(OSIASM), str(SMOKE_ASM), "-o", str(osi)], capture_output=True, text=True,
                       timeout=60)
    assert r.returncode == 0, r.stderr
    return d


# ---- the parser (I10): every bad combination exits 2 and writes nothing ----
BAD = [
    ["--frames", "0"],                      # "Setup only" is --frames 1 (A8)
    ["--frames", "100001"],
    ["--frames", "-1"],
    ["--frames", "1x"],
    ["--level", "x"],
    ["--area", "x"],
    ["--synthetic", "1"],
    ["--survey", "1"],
    ["--manifest", "1"],
    ["--state-log", "S.txt"],
    ["--write-install", "W"],
    ["--texture-cache", "off"],
    ["--backend", "null"],
    ["--width", "64"],                      # a render option has no meaning in the headless script mode
    ["--natives", "fake"],
    ["--natives", ""],
    ["--deterministic", "yes"],
    ["--skip-video", "1"],
    ["--keys", "jump@1"],                   # KEYSPEC through ScriptHost's own parser (Task 3), not a fork
    ["--keys", "down@x"],
    ["--keys", "down@2147483647"],
    ["--trace", "T.txt", "--trace-frames", "-2:3"],
    ["--trace", "T.txt", "--trace-frames", "3:2"],
    ["--trace", "T.txt", "--trace-frames", "1:x"],
    ["--trace", "T.txt", "--trace-frames", "1"],
    ["--trace", "T.txt", "--trace-frames", "-1:-2"],
    ["--trace-frames", "0:1"],              # a window needs --trace
    ["--digest", "same.txt", "--natives-log", "same.txt"],   # two outputs naming one file
    ["--trace", "same.txt", "--out", "same.txt"],
]


@pytest.mark.parametrize("extra", BAD, ids=[" ".join(b) for b in BAD])
def test_bad_combinations_exit_2(smoke, tmp_path, extra):
    args = ["--install", smoke / "install", "--script", "1"]
    args += [str(tmp_path / a) if a.endswith(".txt") or a == "W" else a for a in extra]
    r = _run(VIEW, *args)
    assert r.returncode == 2, (r.returncode, r.stdout, r.stderr)
    assert "unknown option --script" not in r.stderr  # rejected by the --script rules, not as an unknown flag
    assert list(tmp_path.iterdir()) == []


@pytest.mark.parametrize("args", [["--script", "2"], ["--script", "0"], ["--script", ""], ["--script", "1"]],
                         ids=["script 2", "script 0", "script empty", "no install"])
def test_bad_script_values_exit_2(smoke, args):
    r = _run(VIEW, *(args if args == ["--script", "1"] else ["--install", smoke / "install", *args]))
    assert r.returncode == 2, (r.returncode, r.stdout, r.stderr)
    assert "unknown option --script" not in r.stderr


@pytest.mark.parametrize("opt", [["--keys", "down@1"], ["--natives", "stub"], ["--digest", "D"], ["--trace", "T"],
                                 ["--trace-frames", "0:1"], ["--natives-log", "L"], ["--deterministic", "on"],
                                 ["--skip-video", "on"]])
def test_script_options_need_script_mode(tmp_path, opt):
    r = _run(VIEW, "--synthetic", "1", "--backend", "null", "--out", tmp_path / "x.txt", *opt)
    assert r.returncode == 2, (r.returncode, r.stdout, r.stderr)


def test_unreadable_install_or_script_exits_3(tmp_path):
    assert _run(VIEW, "--install", tmp_path / "missing", "--script", "1").returncode == 3
    (tmp_path / "empty" / "data").mkdir(parents=True)  # an install without data/Base.osi
    assert _run(VIEW, "--install", tmp_path / "empty", "--script", "1").returncode == 3
    (tmp_path / "bad" / "data").mkdir(parents=True)
    (tmp_path / "bad" / "data" / "Base.osi").write_bytes(b"OSI\0garbage")
    stale = tmp_path / "stale.digest"
    stale.write_text("old\n")
    r = _run(VIEW, "--install", tmp_path / "bad", "--script", "1", "--digest", stale)
    assert r.returncode == 3, (r.stdout, r.stderr)
    assert not stale.exists()  # valid arguments: a failed run never leaves a stale output


# ---- the run on the synthetic script ----
def test_smoke_coverage_equals_coverage_py(smoke, tmp_path):
    log = tmp_path / "v.natives.log"
    r = _run(VIEW, "--install", smoke / "install", "--script", "1", "--natives", "stub", "--frames", "6", "--keys",
             "down@1", "--natives-log", log)
    assert r.returncode == 0, r.stderr
    cov = _coverage_lines(r.stdout)
    assert cov == _coverage_py(log)
    # the hand-computed report of osi_a9_smoke.asm (its header comment)
    assert cov[0] == "coverage: natives distinct 4 (tier1 0, tier2 0, stub 3, mod 1, tier3 0), calls 9, unregistered 0"
    assert [l.split()[1] for l in cov[1:]] == ["gcgame.getresolutionwidth", "gcsaver.clearcinema",
                                                 "gcgame.getresolutionheight", "gcsaver.cheatmenu"]
    lines = r.stdout.splitlines()
    i = lines.index(cov[-1])
    assert lines[i + 1].startswith("nesting high-water ") and lines[i + 2].startswith("stack high-water ")
    assert lines[0] == cov[0]  # stdout order: coverage report, nesting, stack, then the A6-style summary
    assert "stop: none" in lines


def test_smoke_adds_no_behaviour(smoke, tmp_path):
    """--natives stub == lomn_osi_run on the same script: digest, natives.log and the trace window byte for byte."""
    osi = smoke / "install" / "data" / "Base.osi"
    common = ["--frames", "6", "--keys", "down@1 esc@4", "--trace-frames", "-1:2"]
    v = _run(VIEW, "--install", smoke / "install", "--script", "1", "--natives", "stub", *common,
             "--digest", tmp_path / "v.digest", "--natives-log", tmp_path / "v.log", "--trace", tmp_path / "v.trace")
    o = _run(OSI_RUN, osi, *common, "--digest", tmp_path / "o.digest", "--natives-log", tmp_path / "o.log",
             "--trace", tmp_path / "o.trace")
    assert v.returncode == 0 and o.returncode == 0, (v.stderr, o.stderr)
    for kind in ("digest", "log", "trace"):
        assert (tmp_path / ("v." + kind)).read_bytes() == (tmp_path / ("o." + kind)).read_bytes(), kind
    trace = (tmp_path / "v.trace").read_text()
    assert "\nK      2 " in trace and "\nK      3 " not in trace
    # the A6-style summary lines are lomn_osi_run's, without the wall-clock fields (stdout is deterministic)
    for prefix in ("setup: ", "frames ", "distinct_pcs_by_frame ", "process passes: ", "stop: ", "script sha256 "):
        want = [l for l in o.stdout.splitlines() if l.startswith(prefix)]
        assert want and [l for l in v.stdout.splitlines() if l.startswith(prefix)] == want, prefix


def test_smoke_stdout_is_deterministic_and_json_reports_flags(smoke, tmp_path):
    base = ["--install", smoke / "install", "--script", "1", "--natives", "stub", "--frames", "4"]
    a = _run(VIEW, *base, "--out", tmp_path / "a.json")
    b = _run(VIEW, *base, "--deterministic", "off", "--skip-video", "off", "--out", tmp_path / "b.json")
    assert a.returncode == 0 and b.returncode == 0, (a.stderr, b.stderr)
    assert a.stdout == b.stdout  # no wall-clock value on stdout; the stubs read neither flag
    ja, jb = json.loads((tmp_path / "a.json").read_text()), json.loads((tmp_path / "b.json").read_text())
    assert (ja["skip_video"], ja["deterministic"], ja["natives"]) == (True, True, "stub")  # D1/D3 defaults: on
    assert (jb["skip_video"], jb["deterministic"]) == (False, False)
    assert ja["frames"] == 4 and ja["frames_run"] == 4 and ja["stop"] == "none"
    assert not (tmp_path / "a.json.json").exists()


def test_without_out_no_json_is_written(smoke, tmp_path):
    r = subprocess.run([str(VIEW), "--install", str(smoke / "install"), "--script", "1", "--natives", "stub"],
                       capture_output=True, text=True, timeout=60, cwd=tmp_path)
    assert r.returncode == 0, r.stderr
    assert list(tmp_path.iterdir()) == []


def test_unregistered_native_is_counted_and_stops_the_run(tmp_path):
    """A name the registry does not have raises the original's error (D2): the run stops (exit 4, `stop: error ...`), the
    outputs stay as evidence, and the report counts it under `unregistered`, as coverage.py --trace does on the trace."""
    (tmp_path / "install" / "data").mkdir(parents=True)
    asm = tmp_path / "unreg.asm"
    asm.write_text(".function setup 0\n    CallGameFunction gcgame.getresolutionwidth 0\n    Pop\n"
                   "    CallGameFunction nosuch.native 0\n    Pop\n    PushNothing\n    Return\n")
    r = subprocess.run([sys.executable, str(OSIASM), str(asm), "-o", str(tmp_path / "install" / "data" / "Base.osi")],
                       capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stderr
    v = _run(VIEW, "--install", tmp_path / "install", "--script", "1", "--natives", "stub", "--trace", tmp_path / "t.trace")
    assert v.returncode == 4, (v.stdout, v.stderr)
    cov = _coverage_lines(v.stdout)
    assert cov[0] == "coverage: natives distinct 1 (tier1 0, tier2 0, stub 1, mod 0, tier3 0), calls 1, unregistered 1"
    t = subprocess.run([sys.executable, str(COVERAGE), "--trace", str(tmp_path / "t.trace")], capture_output=True,
                       text=True, timeout=60)
    assert t.returncode == 0 and cov == t.stdout.splitlines(), t.stdout
    assert any(l.startswith('stop: error "Call to unregistered Game Function" in Setup') for l in v.stdout.splitlines())


def test_real_natives_default_fails_closed_like_lomn_osi_run(smoke):
    """--natives real is the default (A9). It binds exactly what lomn_osi_run --real-natives binds: while no game-native
    group is linked, both refuse to run (never a silent stub run); lomn_view exits 4 and prints no report."""
    osi = smoke / "install" / "data" / "Base.osi"
    o = _run(OSI_RUN, osi, "--real-natives", "--frames", "2")
    v = _run(VIEW, "--install", smoke / "install", "--script", "1", "--frames", "2")
    assert o.returncode in (0, 2), o.stderr
    if o.returncode == 2:
        assert v.returncode == 4 and "no native group binds a body" in v.stderr and v.stdout == "", (v.stdout, v.stderr)
    else:
        assert v.returncode == 0, v.stderr


# ---- the real install (A8 and the A6 equivalence) ----
def test_real_a6_path_with_stubs_equals_lomn_osi_run(lomn_data, tmp_path):
    install = lomn_data / "install"
    v = _run(VIEW, "--install", install, "--script", "1", "--natives", "stub", "--frames", "600", "--keys", KEYS,
             "--digest", tmp_path / "v.digest", "--natives-log", tmp_path / "v.log")
    o = _run(OSI_RUN, install / "data" / "Base.osi", "--frames", "600", "--keys", KEYS, "--digest",
             tmp_path / "o.digest", "--natives-log", tmp_path / "o.log")
    assert v.returncode == 0 and o.returncode == 0, (v.stderr, o.stderr)
    assert _d_lines(tmp_path / "v.digest") == _d_lines(tmp_path / "o.digest")
    assert len(_d_lines(tmp_path / "v.digest")) == 601  # frame -1 (Setup) + 600 frames
    assert (tmp_path / "v.log").read_bytes() == (tmp_path / "o.log").read_bytes()
    assert _coverage_lines(v.stdout) == _coverage_py(tmp_path / "v.log")


def test_real_a8_setup_frame(lomn_data, tmp_path):
    r = _run(VIEW, "--install", lomn_data / "install", "--script", "1", "--natives", "stub", "--frames", "1",
             "--natives-log", tmp_path / "v.log")
    assert r.returncode == 0, r.stderr
    cov = _coverage_lines(r.stdout)
    assert cov[0].endswith(", unregistered 0") and cov == _coverage_py(tmp_path / "v.log")
    assert any(l.split()[5] == "-1" for l in cov[1:])  # natives first hit in Setup (frame -1)
