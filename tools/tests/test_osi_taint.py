"""M2a-2 T5: tools/research/osi_taint.py, the A10 taint pass of the Python oracle (docs/osi/natives.md §7.3).

- The propagation corpus `tests/osi/taint/` (no game data): one program per propagation path of the plan's Task 5
  semantics, one per sink kind that can fire, the implicit-flow rule and one negative program (a tainted value only
  popped). Each `.expect.json` lists the exact sinks `{kind, pc, keys}` and the verdict of every tagged native.
- The hook guard (no game data): the 541 conformance programs run with the taint wrappers write byte-identical traces.
- The falsifier (plan Review focus 5, C2), on the real Base.osi, Setup + 30 frames with typed stubs; it skips with
  `SKIPPED (no LOMN_DATA)` without game data:
  - every flip-probe CHANGED native is UNSAFE in the taint report;
  - every CHANGED-NULL native is tier 1 in the merged overrides or a "must (game-returning)" native of Tasks 7-9.
- Fix 1 (the T5 review): `refstore` and `errpath` sinks (t-refstore-*), the tier1 output over-approximation
  (t-tier1-output), a tier1 CHANGED native satisfies the falsifier, the type-only annotation (t-type-only), and every
  op whose result carries a tag is pinned (test_every_result_op_is_pinned).
Every number pinned for the real Base.osi was measured in this task with
  osi_taint.py --osi $LOMN_DATA/install/data/Base.osi --frames 600 --keys "down@100 down@160 enter@220 esc@280"
    --overrides <an empty list>
and is repeated, with the command, in docs/knowledge/measurements.md ("M2a-2 Task 5")."""
import json
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(REPO / "tools" / "research"), str(REPO / "tools" / "osi")]
import osi_taint  # noqa: E402
import osi_vm  # noqa: E402

CORPUS = REPO / "tests" / "osi" / "taint"
PROGRAMS = sorted(CORPUS.glob("*.asm"))
A9_KEYS = "down@100 down@160 enter@220 esc@280"

# The propagation paths of the plan's Task 5 "Semantics" list; each needs at least one corpus program.
PATHS = {"stack copies", "locals", "object members", "arrays", "globals", "arithmetic", "compares", "logic",
         "conversions", "string concatenation", "script call arguments and returns", "by-ref writes", "copy natives"}

# "must (game-returning)" natives of the plan's Tasks 7-9 tables (docs/superpowers/plans/2026-10-09-lomn-web-m2a2.md,
# the "Must / decides" column of the rows whose Ret is `game`). Until those tasks land, a CHANGED-NULL native listed
# here satisfies the falsifier in place of a tier1 row in data/osi/native_overrides.json.
MUST_GAME_RETURNING = frozenset([
    "gctimer.create", "gcgame.getmainview", "gcgame.getkeymapper", "gcgame.getmp3player", "gcviewport.getcamera",
    "gcmodeldirector.gettoa", "gcmodeldirector.getcharacter", "sctilefont.create", "sctextobject.create",
    "gcbigsprite.loadhudsprite", "scslosislkcubicspline.create",
])


# --------------------------------------------------------------------------------------------------- the corpus
def test_corpus_exists_with_every_path_and_a_negative():
    specs = [json.loads(p.with_name(p.name[:-4] + ".expect.json").read_text()) for p in PROGRAMS]
    paths = [s["path"] for s in specs if s.get("kind") == "propagation"]
    assert len(paths) >= 12, paths
    assert PATHS <= set(paths), sorted(PATHS - set(paths))
    neg = [s for s in specs if s.get("kind") == "negative"]
    assert neg and all(s["expect"]["sinks"] == [] and set(s["expect"]["verdicts"].values()) == {"safe"} for s in neg)
    for s in specs:
        if s.get("kind") == "propagation":
            assert any(k == "UNSAFE" for k in s["expect"]["verdicts"].values()), s["id"]


@pytest.mark.parametrize("asm", PROGRAMS, ids=[p.stem for p in PROGRAMS])
def test_corpus_program(asm):
    assert osi_taint.run_program(asm) == []


def test_corpus_runner_detects_a_wrong_expectation(tmp_path):
    """The runner compares sinks and verdicts exactly: a missing sink, an extra key and a wrong verdict each fail."""
    src = CORPUS / "t-locals.asm"
    spec = json.loads(src.with_name("t-locals.expect.json").read_text())
    (tmp_path / "x.asm").write_text(src.read_text())
    bad = dict(spec, expect=dict(spec["expect"], sinks=[]))
    (tmp_path / "x.expect.json").write_text(json.dumps(bad))
    assert any("sink" in f for f in osi_taint.run_program(tmp_path / "x.asm"))
    bad = dict(spec, expect=dict(spec["expect"], verdicts={"t.src": "safe"}))
    (tmp_path / "x.expect.json").write_text(json.dumps(bad))
    assert any("verdict" in f for f in osi_taint.run_program(tmp_path / "x.asm"))


# --------------------------------------------------------------------------------------------------- the hook
def test_wrap_handlers_hook_changes_no_trace(tmp_path, monkeypatch):
    """Vm.wrap_handlers with the taint wrappers: all conformance programs give byte-identical traces (the taint pass
    only reads the VM; its member-name lookups restore FindMemberID's cache)."""
    conf = REPO / "tests" / "osi" / "conformance"
    plain, tainted = tmp_path / "plain", tmp_path / "taint"
    assert osi_vm.conform(conf, plain) == 0
    monkeypatch.setattr(osi_vm, "Vm", osi_taint.TaintVm)
    assert osi_vm.conform(conf, tainted) == 0
    a = sorted(p.relative_to(plain) for p in plain.rglob("*.trace"))
    b = sorted(p.relative_to(tainted) for p in tainted.rglob("*.trace"))
    assert a == b and len(a) >= 500
    diff = [str(p) for p in a if (plain / p).read_bytes() != (tainted / p).read_bytes()]
    assert diff == []


def test_wrap_handlers_identity_wrap_is_a_no_op():
    image = osi_vm_image(".function main 0\n    PushConstanti8 3\n    PushConstanti8 4\n    Add\n    Return\n")
    reg = osi_vm.NativeRegistry(osi_vm.TypeTable.load(osi_vm.DEFAULT_TYPES))
    seen = []
    vm = osi_vm.Vm(image, reg, osi_vm.Heap(None))
    vm.wrap_handlers(lambda op, h: (lambda v: (seen.append(op), h(v))[1]))
    r = vm.call("main")
    assert r.ok and r.result == osi_vm.Variant(osi_vm.T_INT, 7) and seen == [0x43, 0x43, 0x70, 0x52]


def osi_vm_image(text):
    import osiasm
    return osiasm.assemble(text)[0]


# --------------------------------------------------------------------------------------------------- the report
def test_report_lines_format():
    r = osi_taint.Report()
    for k in ("b.two", "a.one", "d.four"):
        r.tag(k)
    r.sink("branch", 0x1234, -1, frozenset(["b.two", "a.one"]))
    r.sink("branch", 0x1234, 5, frozenset(["c.three"]))            # the same site later: keys merge, first frame stays
    r.sink("process-result", 0x40, 7, frozenset(["a.one"]))
    r.sink("name", 0x50, 7, frozenset())                            # an untainted value is no sink
    r.sink("branch", 0x60, 7, frozenset(["?d.four"]))               # `?k`: k reached it through its type only
    r.sink("branch", 0x70, 8, frozenset(["?a.one", "b.two"]))       # mixed: a.one by type, b.two by value
    r.sink("refstore", 0x80, 9, frozenset(["?b.two", "b.two"]))     # by type and by value: by value
    r.sink("errpath", 0x90, 9, frozenset(["a.one"]))
    assert r.lines() == [
        "taint: natives tainted 3, sinks 6 (branch 3, gamevar 0, tier1-arg 0, process-result 1, name 0, refstore 1, "
        "errpath 1), type-only 1",
        "sink branch pc 00001234 frame     -1 <- a.one,b.two,c.three",
        "sink process-result pc 00000040 frame      7 <- a.one",
        "sink branch pc 00000060 frame      7 <- d.four type-only",
        "sink branch pc 00000070 frame      8 <- a.one,b.two type-only:a.one",
        "sink refstore pc 00000080 frame      9 <- b.two",
        "sink errpath pc 00000090 frame      9 <- a.one",
        "native b.two verdict UNSAFE",
        "native a.one verdict UNSAFE",
        "native d.four verdict UNSAFE type-only",
    ]
    r.sink("branch", 0x60, 9, frozenset(["d.four"]))                 # the same site later by value: no longer type-only
    assert r.lines()[3] == "sink branch pc 00000060 frame      7 <- d.four"
    assert r.lines()[-1] == "native d.four verdict UNSAFE"
    r.tag("e.five")
    assert r.lines()[-1] == "native e.five verdict safe"


def test_cli_real_natives_fails_closed_when_nothing_is_bound(tmp_path):
    (tmp_path / "s.osi").write_bytes(osi_vm_image(".function Setup 0\n    PushConstanti8 1\n    Return\n"))
    p = subprocess.run([sys.executable, str(REPO / "tools" / "research" / "osi_taint.py"), "--osi",
                        str(tmp_path / "s.osi"), "--frames", "1", "--real-natives"], capture_output=True, text=True,
                       timeout=60)
    assert p.returncode != 0 and "nothing is bound" in p.stderr


def test_cli_runs_a_synthetic_script(tmp_path):
    """Setup + frames through the CLI: a Process whose result comes from a stub is a process-result sink."""
    text = (".function Setup 0\n    PushConstanti8 1\n    Return\n"
            ".function Process 3\n    CallGameFunction gcgame.getmainview 0\n    Pop\n"
            "    CallGameFunction gcsaver.cheatmenu 0\n"
            "r:\n    Return\n")
    image, info = osiasm_assemble(text)
    (tmp_path / "s.osi").write_bytes(image)
    (tmp_path / "ov.json").write_text("[]")
    p = subprocess.run([sys.executable, str(REPO / "tools" / "research" / "osi_taint.py"), "--osi",
                        str(tmp_path / "s.osi"), "--frames", "3", "--overrides", str(tmp_path / "ov.json")],
                       capture_output=True, text=True, timeout=60)
    assert p.returncode == 0, p.stderr
    assert p.stdout.splitlines() == [
        "taint: natives tainted 2, sinks 1 (branch 0, gamevar 0, tier1-arg 0, process-result 1, name 0, refstore 0, "
        "errpath 0), type-only 0",
        "sink process-result pc %08x frame      0 <- gcsaver.cheatmenu" % info["labels"]["r"],
        "native gcgame.getmainview verdict safe",
        "native gcsaver.cheatmenu verdict UNSAFE",
    ]


def osiasm_assemble(text):
    import osiasm
    return osiasm.assemble(text)


# --------------------------------------------------------------------------------------------------- the falsifier
@pytest.fixture(scope="module")
def base_osi(lomn_data):
    return (lomn_data / "install" / "data" / "Base.osi").read_bytes()


@pytest.fixture(scope="module")
def flip30(base_osi):
    import flip_probe
    return flip_probe.probe(base_osi, str(osi_vm.DEFAULT_NATIVES), str(osi_vm.DEFAULT_MODS), str(osi_vm.DEFAULT_TYPES), 30)


def merged_tier1() -> set:
    rows = json.loads((REPO / "data" / "osi" / "native_overrides.json").read_text())
    return {r["key"] for r in rows if r["tier"] == "tier1"}


def test_flip_implies_sink(base_osi, flip30):
    """Review focus 5: a CHANGED native with no sink is a taint bug; a CHANGED-NULL native must be tier 1."""
    rep = osi_taint.run_base(base_osi, frames=30, keys="", overrides=REPO / "data" / "osi" / "native_overrides.json")
    assert rep.stop == "none"
    bad = osi_taint.flip_violations(flip30, rep, merged_tier1(), MUST_GAME_RETURNING)
    assert bad == []
    assert sum(r.verdict == "CHANGED" for r in flip30) >= 1          # the check is not vacuous
    assert sum(r.verdict == "CHANGED-NULL" for r in flip30) >= 1


def test_flip_implies_sink_bites_a_taint_bug(base_osi, flip30, monkeypatch):
    """The falsifier's mutation control: a taint pass that loses the tag through 0x22 (SetVariableValue stores no
    tag) calls branch-relevant natives safe, and flip_violations must name them."""
    monkeypatch.setattr(osi_taint.Tracker, "_b_set_var", lambda self, op, h: h)
    rep = osi_taint.run_base(base_osi, frames=30, keys="", overrides=None)
    bad = osi_taint.flip_violations(flip30, rep, set(), MUST_GAME_RETURNING)
    assert any(b.startswith("CHANGED") and "no sink" in b for b in bad), bad


def test_flip_violations_rules():
    from flip_probe import Result
    rep = osi_taint.Report()
    for k in ("a.x", "b.y", "c.z", "d.w"):
        rep.tag(k)
    rep.sink("branch", 0x10, -1, frozenset(["a.x"]))
    res = [Result("CHANGED", "a.x", 3, "none"), Result("CHANGED", "b.y", 3, "none"),
           Result("CHANGED-NULL", "c.z", -9, "error"), Result("CHANGED-NULL", "d.w", -9, "error"),
           Result("SAME", "e.v", 0, "none"), Result("CHANGED", "f.u", 3, "none")]
    bad = osi_taint.flip_violations(res, rep, {"c.z"}, frozenset())
    assert bad == ["CHANGED b.y: no sink (verdict safe)", "CHANGED-NULL d.w: not tier1 and not a must (game-returning) native",
                   "CHANGED f.u: no sink (verdict not tagged)"]
    # I2 (fix 1): a tier1 native is never tagged; a CHANGED tier1 native satisfies "flip implies sink" by its tier
    assert osi_taint.flip_violations(res, rep, {"c.z", "b.y", "f.u"}, frozenset()) == [
        "CHANGED-NULL d.w: not tier1 and not a must (game-returning) native"]
    rep.tier1_hit.update(["d.w", "f.u"])                              # tier1 natives the run hit (overrides or bound)
    assert osi_taint.flip_violations(res, rep, {"c.z"}, frozenset()) == ["CHANGED b.y: no sink (verdict safe)"]


def test_flip_implies_sink_with_a_tier1_override(base_osi, flip30, tmp_path):
    """I2 on real data: with stringtoidentifier tier1 (a scratch override, as Tasks 7-9 will merge it) the CHANGED native
    is no longer tagged, and the falsifier still holds (before fix 1: `CHANGED …: no sink (verdict not tagged)`)."""
    ov = tmp_path / "ov.json"
    ov.write_text(json.dumps([{"key": "scslosifoundation.stringtoidentifier", "tier": "tier1"}]))
    rep = osi_taint.run_base(base_osi, frames=30, keys="", overrides=ov)
    assert rep.stop == "none" and "scslosifoundation.stringtoidentifier" not in rep.tagged
    assert "scslosifoundation.stringtoidentifier" in rep.tier1_hit
    assert osi_taint.flip_violations(flip30, rep, set(), MUST_GAME_RETURNING) == []


# M4 (fix 1): every op whose result carries a tag is pinned: forcing that op's result tag to empty breaks a program.
RESULT_BUILDERS = ("binary", "unary", "typetest", "eq", "div", "get_member", "get_this", "get_this_fn", "get_member_str",
                   "get_member_fn_str", "get_var", "pull", "dup", "get_array", "native")


def test_every_result_op_is_pinned(monkeypatch):
    ops = sorted(op for op, b in osi_taint._BUILDERS.items() if b in RESULT_BUILDERS)
    survivors = []
    orig = osi_taint.Tracker.wrap
    for op in ops:
        def wrap(self, o, h, op=op):
            w = orig(self, o, h)
            if o != op:
                return w

            def forced(vm):
                r = w(vm)
                self.ts[vm.depth - 1] = osi_taint.EMPTY
                return r
            return forced
        monkeypatch.setattr(osi_taint.Tracker, "wrap", wrap)
        if not any(osi_taint.run_program(p) for p in PROGRAMS):
            survivors.append("%02x" % op)
    monkeypatch.setattr(osi_taint.Tracker, "wrap", orig)
    m4 = {0x62, 0x63, 0x64, 0x74, 0x92, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBF}
    assert len(ops) >= 49 and m4 <= set(ops) and survivors == []    # the review's 19 unpinned ops (M4) included


# --------------------------------------------------------------------------------------------------- the A9 path
def test_a9_stub_report(base_osi, tmp_path):
    """The stub run of the A9 path (600 frames, the A9 keys, an empty overrides list): measured values (D6), pinned so
    that a change to the pass shows up; the per-native verdicts are in docs/knowledge/measurements.md."""
    (tmp_path / "ov.json").write_text("[]")
    rep = osi_taint.run_base(base_osi, frames=600, keys=A9_KEYS, overrides=tmp_path / "ov.json")
    assert rep.stop == "none" and rep.frames == 600 and rep.instructions == 390706   # the A6 total (measurements.md)
    assert rep.lines()[0] == A9_HEAD
    assert sorted(k for k, v in rep.verdicts().items() if v.startswith("UNSAFE")) == A9_UNSAFE
    assert sorted(k for k, v in rep.verdicts().items() if v == "UNSAFE type-only") == A9_TYPE_ONLY


A9_HEAD = ("taint: natives tainted 69, sinks 38 (branch 38, gamevar 0, tier1-arg 0, process-result 0, name 0, refstore 0, "
           "errpath 0), type-only 27")
# fix 1 (M6): the UNSAFE natives whose every A9 sink tests only the type (`== nothing`): measured, not a target
A9_TYPE_ONLY = sorted([
    "gcgame.getresolutionwidth", "gcgame.getresolutionheight", "gchuddirector.addbigsprite", "gctimer.create",
    "gcgame.getmp3player", "gcmodeldirector.gettoa", "scslosislkcubicspline.create",
])
A9_UNSAFE = sorted([
    "bettersaver.getbooleanvalue", "gcareadirector.getcurrentlevel", "gcgame.getmp3player", "gcgame.getresolutionheight",
    "gcgame.getresolutionwidth", "gchuddirector.addbigsprite", "gclegocamera.getcameraaimode", "gcmodeldirector.gettoa",
    "gcsaver.cheatmenu", "gcsaver.getammo", "gctimer.create", "scslosifileio.doesfileexist",
    "scslosifoundation.stringtoidentifier", "scslosislkcubicspline.create",
])
