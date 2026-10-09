"""M2a-2 T1: tools/osi/coverage.py (the A8 coverage report) and tools/osi/flip_probe.py.

The coverage tests use a 3-native synthetic trace (Setup = frame -1, frames 0 and 3) and synthetic registry files, so
they need no game data. The flip-probe tests run the oracle on the real Base.osi and skip with
`SKIPPED (no LOMN_DATA)` without it. Every number pinned for the real Base.osi was measured in this task with:
  flip_probe.py --osi $LOMN_DATA/install/data/Base.osi --frames 30 | awk '{print $1}' | sort | uniq -c
and is repeated, with the commands, in docs/knowledge/measurements.md ("M2a-2 baseline")."""
import importlib.util
import json
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]


def _load(name):
    spec = importlib.util.spec_from_file_location("osi_" + name, REPO / "tools" / "osi" / (name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = mod
    spec.loader.exec_module(mod)
    return mod


coverage = _load("coverage")

ROWS = [
    {"cls": "A", "name": "One", "key": "a.one", "body": "0x00401000", "winner": True},
    {"cls": "B", "name": "Two", "key": "b.two", "body": "0x00402000", "winner": True},
    {"cls": "C", "name": "Three", "key": "c.three", "body": "0x00403000", "winner": True},
    {"cls": "D", "name": "Never", "key": "d.never", "body": "0x00404000", "winner": True},
    {"cls": "E", "name": "Dup", "key": "e.dup", "body": "0x00405000", "winner": False},      # a losing duplicate
]
MODS = [{"cls": "F", "name": "Mod", "key": "f.mod", "body": "0x10001000", "tier": "mod", "winner": True}]
OVERRIDES = [{"key": "b.two", "tier": "tier1", "cite": "x"}, {"key": "c.three", "tier": "tier2", "cite": "y"}]

HDR = "# lomn-osi-trace 1 script=%s natives=%s mods=%s types=%s fpu=pc24" % (("0" * 64,) * 4)
CALL = "C Setup 0 " + " ".join(["000f:00000000"] * 10)
TRACE = [
    HDR,
    CALL,
    "I 00000000 82    1    1 00ff:00000000",
    "N 00000030 A.One 0 -> 000f:00000000",
    "I 00000036 30    1    1 000f:00000000",
    "N 00000040 B.Two 1 0010:00000001 -> 0010:00000000",         # case is preserved in the trace, keys are lower case
    "N 00000050 A.One 0 -> 000f:00000000",
    "K      0 3bda740e 3bda740e 00000000",
    "C Process 3 0012:3bda740e 0012:3bda740e 0010:00000000 " + " ".join(["000f:00000000"] * 7),
    "N 00000060 A.One 0 -> 000f:00000000",
    "K      3 3c5a740e 3bda740e 00000003",
    "N 00000070 C.Three 0 -> 000f:00000000",
    "N 00000080 F.Mod 0 -> 000f:00000000",
    "N 00000090 C.Three 0 -> 000f:00000000",
]


@pytest.fixture()
def files(tmp_path):
    n, m, o = tmp_path / "natives.json", tmp_path / "mods.json", tmp_path / "overrides.json"
    n.write_text(json.dumps(ROWS))
    m.write_text(json.dumps(MODS))
    o.write_text(json.dumps(OVERRIDES))
    return n, m, o


def run_report(lines, files, overrides=True, kind="trace"):
    n, m, o = files
    reg = coverage.load_registry(n, m, o if overrides else None)
    events, unreg = (coverage.parse_trace if kind == "trace" else coverage.parse_log)(lines)
    return coverage.report(events, unreg, reg)


def test_report_grammar_is_byte_exact(files):
    out = run_report(TRACE, files)
    assert out == [
        "coverage: natives distinct 4 (tier1 1, tier2 1, stub 1, mod 1, tier3 0), calls 7, unregistered 0",
        "native %-48s tier %-5s first %6d calls %6d body %s" % ("a.one", "stub", -1, 3, "0x00401000"),
        "native %-48s tier %-5s first %6d calls %6d body %s" % ("b.two", "tier1", -1, 1, "0x00402000"),
        "native %-48s tier %-5s first %6d calls %6d body %s" % ("c.three", "tier2", 3, 2, "0x00403000"),
        "native %-48s tier %-5s first %6d calls %6d body %s" % ("f.mod", "mod", 3, 1, "0x10001000"),
    ]
    assert out[1] == "native a.one" + " " * 43 + " tier stub  first     -1 calls      3 body 0x00401000"


def test_tier_counts_sum_to_distinct(files):
    head = run_report(TRACE, files)[0]
    import re
    distinct = int(re.search(r"distinct (\d+)", head).group(1))
    tiers = {k: int(v) for k, v in re.findall(r"(tier1|tier2|stub|mod|tier3) (\d+)", head)}
    assert sorted(tiers) == ["mod", "stub", "tier1", "tier2", "tier3"]
    assert distinct == sum(tiers.values()) == 4


def test_without_overrides_every_exe_row_is_stub(files):
    head = run_report(TRACE, files, overrides=False)[0]
    assert head == "coverage: natives distinct 4 (tier1 0, tier2 0, stub 3, mod 1, tier3 0), calls 7, unregistered 0"


def test_rows_sort_by_first_frame_then_first_seq(files):
    # c.three is hit first in the file but only at frame 3; the Setup natives sort before it whatever the key order
    lines = [HDR, CALL, "N 00000030 C.Three 0 -> 000f:00000000", "K      0 00000000 00000000 00000000",
             "N 00000040 A.One 0 -> 000f:00000000", "K      1 00000000 00000000 00000001",
             "N 00000050 B.Two 0 -> 000f:00000000"]
    rows = [ln.split()[1] for ln in run_report(lines, files)[1:]]
    assert rows == ["c.three", "a.one", "b.two"]


def test_unregistered_counts_error_lines_and_unknown_keys(files):
    lines = TRACE + ['E 00000090 82    1 "Call to unregistered Game Function"',
                     "N 000000a0 Z.Ghost 0 -> 000f:00000000"]
    head = run_report(lines, files)[0]
    assert head.endswith("calls 8, unregistered 2")
    assert "z.ghost" not in "\n".join(run_report(lines, files))          # unregistered natives have no row/tier


def test_name_token_escapes_are_decoded(files, tmp_path):
    n = tmp_path / "n2.json"
    n.write_text(json.dumps([{"cls": "A", "name": "Sp ace", "key": "a.sp ace", "body": "0x1", "winner": True}]))
    reg = coverage.load_registry(n, None, None)
    events, _ = coverage.parse_trace([HDR, r"N 00000030 A.Sp\x20ace 0 -> 000f:00000000"])
    assert "native a.sp ace" in coverage.report(events, 0, reg)[1]


def test_log_v1_has_no_frames_and_v2_k_lines_set_them(files):
    v1 = ["00000000 A.One 0 -> 000f:00000000", "00000001 B.Two 1 -> 0010:00000000",
          "00000002 A.One 0 -> 000f:00000000"]
    rows = run_report(v1, files, kind="log")
    assert rows[0].startswith("coverage: natives distinct 2 (tier1 1, tier2 0, stub 1, mod 0, tier3 0), calls 3")
    assert all(" first     -1 " in r for r in rows[1:])
    v2 = v1[:1] + ["K      0", "C 00000001 B.Two 0 0 0", "00000001 B.Two 1 -> 0010:00000000 G 0"] + v1[2:] + [
        "K      7", "00000003 C.Three 0 -> 000f:00000000 G 1"]
    rows = run_report(v2, files, kind="log")
    firsts = {r.split()[1]: int(r.split()[5]) for r in rows[1:]}
    assert firsts == {"a.one": -1, "b.two": 0, "c.three": 7}


def test_unknown_tier_in_overrides_is_an_error(files, tmp_path):
    bad = tmp_path / "bad.json"
    bad.write_text(json.dumps([{"key": "a.one", "tier": "gold"}]))
    with pytest.raises(ValueError, match="unknown tier"):
        coverage.load_registry(files[0], files[1], bad)


def test_overrides_may_be_a_dict_keyed_by_native(files, tmp_path):
    d = tmp_path / "d.json"
    d.write_text(json.dumps({"a.one": {"tier": "tier3", "note": "worklist"}}))
    events, unreg = coverage.parse_trace(TRACE)
    head = coverage.report(events, unreg, coverage.load_registry(files[0], files[1], d))[0]
    assert "tier3 1" in head


def test_cli_trace(files, tmp_path):
    t = tmp_path / "t.txt"
    t.write_text("\n".join(TRACE) + "\n")
    n, m, o = files
    r = subprocess.run([sys.executable, str(REPO / "tools" / "osi" / "coverage.py"), "--trace", str(t),
                        "--natives", str(n), "--mods", str(m), "--overrides", str(o)],
                       capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stderr
    assert r.stdout.splitlines()[0].startswith("coverage: natives distinct 4 (tier1 1, tier2 1, stub 1, mod 1, tier3 0)")
    assert len(r.stdout.splitlines()) == 5


def test_cli_needs_exactly_one_source():
    r = subprocess.run([sys.executable, str(REPO / "tools" / "osi" / "coverage.py")], capture_output=True, text=True,
                       timeout=60)
    assert r.returncode != 0


def test_default_files_are_the_committed_tables(tmp_path):
    """The shipped tables: 1,056 rows, 1,049 winners (7 duplicated gchudsprite keys), 19 mod rows; no overrides file yet."""
    reg = coverage.load_registry(REPO / "data" / "osi" / "natives.json", REPO / "data" / "osi" / "natives_beta150.json",
                                 None)
    assert len(reg) == 1049 + 19
    assert sum(1 for v in reg.values() if v["tier"] == "mod") == 19
    assert sum(1 for v in reg.values() if v["tier"] == "stub") == 1049


def test_name_token_unescape_decodes_backslash_first_class(files):
    """trace.md section 1: a backslash prints as `\\\\`, so the name `b\\x41` (a literal backslash) prints as `b\\\\x41`
    and must not turn into `bA`-style output; `\\x41` alone is the byte 0x41."""
    assert coverage._key("A.b\\\\x41") == "a.b\\x41"
    assert coverage._key("A.b\\x41") == "a.ba"
    assert coverage._key("A.b\\\\\\x41") == "a.b\\a"           # backslash, then the escape for 0x41


ERR_TRACE = [
    HDR, CALL,
    "I 00000030 82    1    1 00ff:00000000",
    'E 00000030 82    1 "Script is calling a.one"',                  # a call that failed: no N line
    "K      0 00000000 00000000 00000000",
    "I 00000040 82    1    1 00ff:00000000",
    "N 00000040 B.Two 0 -> 000f:00000000",
    "F s 00000001",
    'E 00000040 82    2 "Script is calling b.two"',                  # the error came after the N line (the pops)
    "I 00000050 82    1    1 00ff:00000000",
    'E 00000050 82    1 "Script is calling c.three"',
    "I 00000060 70    1    1 00ff:00000000",
    'E 00000060 70    1 "Results of comparison expression is not integral"',   # not a native call
]


def test_errored_native_calls_are_counted(files):
    """m-2: an E line at op 0x82 with no N line since the last I line is a call of the native it names; an E line after
    the native's own N line is the same call (no double count)."""
    rows = run_report(ERR_TRACE, files)
    assert rows[0] == "coverage: natives distinct 3 (tier1 1, tier2 1, stub 1, mod 0, tier3 0), calls 3, unregistered 0"
    got = {r.split()[1]: (int(r.split()[5]), int(r.split()[7])) for r in rows[1:]}
    assert got == {"a.one": (-1, 1), "b.two": (0, 1), "c.three": (0, 1)}


def test_errored_call_of_an_unknown_native_is_unregistered(files):
    lines = [HDR, "I 00000030 82    1    1 00ff:00000000", 'E 00000030 82    1 "Script is calling z.ghost"']
    assert run_report(lines, files)[0].endswith("calls 1, unregistered 1")


# ----------------------------------------------------------------------------------------------- flip_probe.py

FLIP_ROWS = [
    {"cls": "t", "name": "flag", "key": "t.flag", "min": 0, "max": 0, "ret": "int", "ret_class": None, "body": "0x1"},
    {"cls": "t", "name": "same", "key": "t.same", "min": 0, "max": 0, "ret": "none", "ret_class": None, "body": "0x2"},
    {"cls": "t", "name": "mk", "key": "t.mk", "min": 0, "max": 0, "ret": "game", "ret_class": "GcTimer", "body": "0x3"},
    {"cls": "t", "name": "mk2", "key": "t.mk2", "min": 0, "max": 0, "ret": "game", "ret_class": "GcTimer", "body": "0x5"},
    {"cls": "t", "name": "use", "key": "t.use", "min": 1, "max": 1, "ret": "none", "ret_class": None, "body": "0x4",
     "types": ["type:GcTimer"]},
]
FLIP_ASM = """
.strings "t" "flag" "same" "mk" "use" "mk2"
.function Setup 0
    CallGameFunction t.flag 0
    CompareAndBranchIfFalse @skip
    CallGameFunction t.same 0
    Pop
skip:
    CallGameFunction t.mk2 0
    PushNothing
    EqualTo
    CompareAndBranchIfFalse @obj
    PushConstant0
    Pop
obj:
    CallGameFunction t.mk 0
    CallGameFunction t.use 1
    Pop
    PushConstant0
    Return
.function Process 3
    PushConstanti8 1
    Return
"""
LOOP_ASM = """
.strings "t" "flag"
.function Setup 0
top:
    CallGameFunction t.flag 0
    CompareAndBranchIfFalse @out
    BranchAlways @top
out:
    PushConstant0
    Return
.function Process 3
    PushConstanti8 1
    Return
"""


@pytest.fixture(scope="module")
def flip():
    return _load("flip_probe")


def _flip_files(tmp_path, asm):
    sys.path[:0] = [str(REPO / "tools" / "osi")]
    import osiasm
    rows = []
    for r in FLIP_ROWS:
        r = dict(r, winner=True, default=None, sets_fpu_pc64=False)
        r["types"] = list(r.get("types", [])) + [None] * (10 - len(r.get("types", [])))
        rows.append(r)
    n = tmp_path / "n.json"
    n.write_text(json.dumps(rows))
    image, _ = osiasm.assemble(asm)
    return image, n


def test_flip_probe_synthetic_verdicts(flip, tmp_path):
    """Hand-derived (instruction counts). Baseline Setup: flag, branch, mk2, PushNothing, EqualTo (false: a game handle is
    not nothing), branch (jumps to obj), mk, use, Pop, Push0, Return = 11, and 2 Process frames of 2: total 15.
    flag -> 1 also runs `same` + Pop: 17 (+2), CHANGED. mk2 -> nothing: EqualTo is true, the branch falls through and
    runs Push0 + Pop: 17 (+2); a game native that changed the path WITHOUT an error stays CHANGED (gctimer.create,
    gcgame.getmp3player on the real script). mk -> nothing: `use` errors on its type:GcTimer argument at instruction 8,
    Setup ends, no frames: 8 (-7), CHANGED-NULL. use -> int 1: popped like the nothing: SAME."""
    image, n = _flip_files(tmp_path, FLIP_ASM)
    res = flip.probe(image, n, None, REPO / "data" / "osi" / "types.json", frames=2, deadline=30)
    assert [(r.verdict, r.key, r.delta) for r in res] == [
        ("CHANGED", "t.flag", 2), ("CHANGED", "t.mk2", 2), ("CHANGED-NULL", "t.mk", -7), ("SAME", "t.use", 0)]
    assert [r.stop for r in res if r.verdict != "CHANGED-NULL"] == ["none"] * 3
    mk = [r for r in res if r.key == "t.mk"][0]
    assert mk.stop.startswith('error "') and "in Setup" in mk.stop
    assert [flip.format_line(r).split()[0] for r in res] == ["CHANGED", "CHANGED", "CHANGED-NULL", "SAME"]
    assert flip.format_line(res[0]).startswith("CHANGED t.flag +2 none")
    assert flip.format_line(res[3]).startswith("SAME t.use +0 none")


def test_flip_probe_alternatives_per_return_class(flip):
    """int and none -> int 1, float -> 1.0, string -> "x", colour -> colour 1, copy -> int 1, game -> nothing (C2)."""
    assert flip.ALTERNATIVE["int"] == flip.ALTERNATIVE["none"] == flip.ALTERNATIVE["copy"] == ("int", 1)
    assert flip.ALTERNATIVE["float"] == ("float", 1.0)
    assert flip.ALTERNATIVE["string"] == ("string", "x")
    assert flip.ALTERNATIVE["colour"] == ("colour", 1)
    assert flip.ALTERNATIVE["game"] is None


def test_flip_probe_deadline_per_run(flip, tmp_path):
    """An alternative return that sends Setup into an endless loop ends at the deadline and says so."""
    image, n = _flip_files(tmp_path, LOOP_ASM)
    res = flip.probe(image, n, None, REPO / "data" / "osi" / "types.json", frames=1, deadline=0.3)
    assert [(r.verdict, r.key, r.stop) for r in res if r.key == "t.flag"] == [("CHANGED", "t.flag", "deadline")]


def test_flip_probe_real_impl_is_not_available_yet(flip, tmp_path):
    image, n = _flip_files(tmp_path, FLIP_ASM)
    with pytest.raises(SystemExit):
        flip.main(["--osi", str(tmp_path / "x.osi"), "--frames", "1", "--natives-impl", "real"])


CHANGED_BY_VALUE = {"bettersaver.getbooleanvalue", "gcareadirector.getcurrentlevel", "gcsaver.cheatmenu",
                    "gcgame.getmp3player", "gctimer.create", "scslosifileio.doesfileexist",
                    "scslosifoundation.stringtoidentifier"}
CHANGED_NULL = {"sctilefont.create", "sctextobject.create", "gcbigsprite.loadhudsprite", "gcgame.getmainview",
                "gcviewport.getcamera", "gcgame.getkeymapper", "gcmodeldirector.gettoa",
                "gcmodeldirector.getcharacter", "scslosislkcubicspline.create"}


@pytest.fixture(scope="module")
def base_osi(lomn_data):
    p = lomn_data / "install" / "data" / "Base.osi"
    if not p.exists():
        pytest.skip("SKIPPED (no Base.osi in LOMN_DATA)")
    return p


@pytest.fixture(scope="module")
def real_flip(flip, base_osi):
    return flip.probe(base_osi.read_bytes(), DATA / "natives.json", DATA / "natives_beta150.json",
                      DATA / "types.json", frames=30, deadline=20)


DATA = REPO / "data" / "osi"


def test_flip_probe_real_driver_matches_run_base(flip, base_osi, capsys):
    """The probe's frame loop is a copy of osi_vm.run_base's: same instruction totals on 3 frames."""
    sys.path[:0] = [str(REPO / "tools" / "research")]
    import osi_vm
    rc = osi_vm.main(["--run", str(base_osi), "--natives", str(DATA / "natives.json"), "--mods",
                      str(DATA / "natives_beta150.json"), "--types", str(DATA / "types.json"), "--frames", "3"])
    out = capsys.readouterr().out
    assert rc == 0
    import re
    setup = int(re.search(r"setup: instructions (\d+), natives (\d+)", out).group(1))
    total = int(re.search(r"frames 3 \(of 3 requested\), instructions (\d+), natives (\d+)", out).group(1))
    run = flip.run_once(base_osi.read_bytes(), DATA / "natives.json", DATA / "natives_beta150.json",
                        DATA / "types.json", frames=3, deadline=30)
    assert run.setup_instructions == setup and run.instructions == total
    assert run.stop == "none"


def test_flip_probe_real_baseline_covers_69_natives(real_flip):
    assert len(real_flip) == 69


def test_flip_probe_real_pinned_sets(real_flip):
    got = {}
    for r in real_flip:
        got.setdefault(r.verdict, set()).add(r.key)
    assert got["CHANGED"] == CHANGED_BY_VALUE
    assert got["CHANGED-NULL"] == CHANGED_NULL
    assert len(got["SAME"]) == 69 - 16


def test_flip_probe_real_pinned_deltas(real_flip):
    by = {r.key: r for r in real_flip}
    assert by["gcsaver.cheatmenu"].delta == -37713
    assert by["scslosifileio.doesfileexist"].delta == 293


def test_flip_probe_real_null_control_and_criteria(flip, base_osi):
    """Null control: two baseline runs are identical, so a SAME/CHANGED verdict is not noise. Measured on the real script:
    no native changes the executed-PC set without changing the instruction total, or the other way round (so the two
    CHANGED criteria agree); 70,222 instructions and 13,534 distinct PCs over Setup + 30 frames."""
    args = (base_osi.read_bytes(), DATA / "natives.json", DATA / "natives_beta150.json", DATA / "types.json")
    a = flip.run_once(*args, frames=30, deadline=30)
    b = flip.run_once(*args, frames=30, deadline=30)
    assert (a.pcs, a.instructions, a.natives) == (b.pcs, b.instructions, b.natives)
    assert (a.instructions, len(a.pcs), len(a.natives)) == (70222, 13534, 69)


def test_flip_probe_null_label_needs_the_rejection_at_the_native_call(flip, tmp_path):
    """m-3: a game native whose nothing makes the script fail somewhere ELSE (a GetObjectClassID (0xbf) on it) is CHANGED, with the error
    in <stop>; only the next native call that rejects the nothing as a type:<Class> argument is CHANGED-NULL."""
    sys.path[:0] = [str(REPO / "tools" / "osi")]
    import osiasm
    asm = """
.strings "t" "mk3" "mk" "use"
.function Setup 0
    CallGameFunction t.mk3 0
    GetObjectClassID
    Pop
    CallGameFunction t.mk 0
    CallGameFunction t.use 1
    Pop
    PushConstant0
    Return
.function Process 3
    PushConstanti8 1
    Return
"""
    rows = [dict(FLIP_ROWS[i], winner=True, default=None, sets_fpu_pc64=False) for i in (2, 4)]
    rows[0].update(name="mk", key="t.mk")
    mk3 = dict(rows[0], name="mk3", key="t.mk3", body="0x9")
    rows = [mk3] + rows
    for r in rows:
        r["types"] = list(r.get("types", [])) + [None] * (10 - len(r.get("types", [])))
    n = tmp_path / "n3.json"
    n.write_text(json.dumps(rows))
    image, _ = osiasm.assemble(asm)
    res = {r.key: r for r in flip.probe(image, n, None, REPO / "data" / "osi" / "types.json", frames=1, deadline=30)}
    assert res["t.mk3"].verdict == "CHANGED" and res["t.mk3"].stop.startswith('error "')
    assert " op bf" in res["t.mk3"].stop
    assert res["t.mk"].verdict == "CHANGED-NULL" and " op 82" in res["t.mk"].stop
