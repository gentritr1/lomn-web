"""M2a-2 T1: tools/osi/reach.py, the static reachability walks.

Synthetic tests (a hand-built call graph and a hand-assembled script) check the rules without game data. The pinned tests
read gen/decomp (skip with `SKIPPED (run make decomp first)`) and, for the script walk, the real Base.osi (skip with
`SKIPPED (no LOMN_DATA)`). Every pinned number was measured in this task with
  reach.py --vm-reentry | tail -1            reach.py --argc-violations --0x19
and is repeated, with the commands, in docs/knowledge/measurements.md ("M2a-2 baseline")."""
import importlib.util
import json
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(REPO / "tools" / "osi")]
import osiasm  # noqa: E402


def _load(name):
    spec = importlib.util.spec_from_file_location("osi_" + name, REPO / "tools" / "osi" / (name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = mod
    spec.loader.exec_module(mod)
    return mod


reach = _load("reach")
DATA = REPO / "data" / "osi"
CALL, CALLRET = 0x60B320, 0x60B450


# ----------------------------------------------------------------------------------------------- the corpus walk

def corpus_files(tmp_path):
    """Natives N1 -> M -> K -> Call (depth 3), N2 -> CallWithReturn (depth 1), N3 -> L -> (an external edge, skipped),
    N4 -> K (through a second route of length 2: N4 -> K -> Call is depth 2). External = an address not in functions.jsonl."""
    funcs = {0x1000: "n1_body", 0x1100: "n2_body", 0x1200: "n3_body", 0x1300: "n4_body", 0x2000: "mid", 0x2100: "leaf",
             0x2200: "k", CALL: "ScOSIVirtualMachine::Call", CALLRET: "ScOSIVirtualMachine::CallWithReturn",
             0x4000: "other"}
    edges = {0x1000: [0x2000], 0x2000: [0x2200], 0x2200: [CALL], 0x1100: [CALLRET], 0x1200: [0x2100],
             0x2100: [0x9999999], 0x1300: [0x2200], 0x4000: [CALL, CALLRET], CALL: [CALLRET]}
    (tmp_path / "functions.jsonl").write_text("".join(
        json.dumps({"addr": "0x%08x" % a, "name": n, "size": 1, "ok": True, "lines": 1}) + "\n" for a, n in funcs.items()))
    (tmp_path / "calls.jsonl").write_text("".join(
        json.dumps({"from": "0x%08x" % a, "to": ["0x%08x" % t for t in edges.get(a, [])]}) + "\n" for a in funcs))
    rows = [{"key": "n%d.f" % i, "body": "0x%08x" % b, "winner": True} for i, b in
            ((1, 0x1000), (2, 0x1100), (3, 0x1200), (4, 0x1300))]
    rows.append({"key": "dup.f", "body": "0x00001000", "winner": False})                # a losing duplicate: not counted
    return rows


def test_reentry_depth_and_chain(tmp_path):
    rows = corpus_files(tmp_path)
    c = reach.load_corpus(tmp_path)
    res = reach.vm_reentry(c, rows)
    assert [(r.key, r.depth) for r in res] == [("n2.f", 1), ("n4.f", 2), ("n1.f", 3)]       # sorted by (depth, key)
    n1 = [r for r in res if r.key == "n1.f"][0]
    assert [c.names[a] for a in n1.chain] == ["n1_body", "mid", "k", "ScOSIVirtualMachine::Call"]
    assert reach.format_reentry(n1, c) == ("reentry n1.f depth 3 body 0x00001000 chain n1_body -> mid -> k -> "
                                           "ScOSIVirtualMachine::Call")


def test_reentry_external_edges_are_skipped(tmp_path):
    rows = corpus_files(tmp_path)
    c = reach.load_corpus(tmp_path)
    assert 0x9999999 not in c.names and all(t in c.names for to in c.calls.values() for t in to)
    assert "n3.f" not in [r.key for r in reach.vm_reentry(c, rows)]


def test_direct_callers_of_both_targets(tmp_path):
    corpus_files(tmp_path)
    c = reach.load_corpus(tmp_path)
    assert reach.direct_callers(c) == {0x2200, 0x1100, 0x4000, CALL}      # Call itself calls CallWithReturn


def test_indirect_entries_are_closure_functions_nobody_calls(tmp_path):
    """I-1: the closure holds `other` (0x4000), which calls both targets but has no caller and is no native body: it can
    only be entered through a vtable or a callback, so natives that reach it are invisible to the direct-call count."""
    rows = corpus_files(tmp_path)
    c = reach.load_corpus(tmp_path)
    ind = reach.indirect_entries(c, rows)
    assert [(i.addr, c.names[i.addr], i.depth) for i in ind] == [(0x4000, "other", 1)]


def test_find_entries_vtable_slot_and_code_reference(tmp_path):
    """Entry lookup on an exe image: a function address stored in a run of function addresses is a vtable slot; one
    pushed in code (0x68 imm32) is a callback registration. A function nobody mentions has no entry."""
    import struct
    sys.path[:0] = [str(REPO / "tools" / "osi")]
    import exc
    funcs = {0x401000: "register", 0x401100: "A::slot0", 0x401200: "A::slot1", 0x401300: "cb", 0x401400: "lonely"}
    text = bytearray(0x500)
    text[0:6] = b"\x68" + struct.pack("<I", 0x401300) + b"\xc3"                  # push cb ; ret  (in `register`)
    rdata = struct.pack("<IIII", 0x401100, 0x401200, 0, 0)                       # a vtable at 0x600000: slots 0 and 1
    img = exc.Image([(0x401000, bytes(text)), (0x600000, rdata)])
    corpus = reach.Corpus(funcs, {})
    got = reach.find_entries(img, corpus, [0x401100, 0x401200, 0x401300, 0x401400])
    assert got[0x401100] == ["vtable 0x00600000 slot 0 (slot 0 = A::slot0)"]
    assert got[0x401200] == ["vtable 0x00600000 slot 1 (slot 0 = A::slot0)"]
    assert got[0x401300] == ["code reference at 0x00401001 in register (push imm32)"]
    assert got[0x401400] == []


def test_format_indirect(tmp_path):
    rows = corpus_files(tmp_path)
    c = reach.load_corpus(tmp_path)
    ind = reach.indirect_entries(c, rows)
    assert reach.format_indirect(ind[0], c, ["vtable 0x00600000 slot 3 (slot 0 = x)"]) == (
        "indirect 0x00004000 other depth 1 entered: vtable 0x00600000 slot 3 (slot 0 = x)")
    assert reach.format_indirect(ind[0], c, []).endswith("entered: no reference found in the exe")
    assert reach.format_indirect(ind[0], c, None).endswith("entered: n/a (no exe)")


def test_reentry_summary_line_is_labelled_a_direct_call_floor(tmp_path):
    rows = corpus_files(tmp_path)
    c = reach.load_corpus(tmp_path)
    res = reach.vm_reentry(c, rows)
    assert reach.reentry_summary(res, rows, c, on_path={"n1.f", "n2.f", "n3.f"}) == (
        "vm-reentry (direct-call floor): 3 of 4 natives reach 0x60b320/0x60b450 through direct calls (direct callers of "
        "the targets: 4); 1 closure functions are entered only through vtables or callbacks, so more natives may "
        "re-enter; on the stub path 2: n2.f 1, n1.f 3")
    assert reach.reentry_summary(res, rows, c, on_path=None).endswith("on the stub path n/a (no Base.osi)")


def test_stub_path_natives_honours_the_tables(tmp_path):
    """m-6: the natives table of the CLI reaches the oracle run. A script that calls t.f is only runnable with a table
    that registers it."""
    image, _ = osiasm.assemble("""
.strings "t" "f"
.function Setup 0
    CallGameFunction t.f 0
    Pop
    PushConstant0
    Return
.function Process 3
    PushConstanti8 1
    Return
""")
    osi = tmp_path / "x.osi"
    osi.write_bytes(image)
    n = tmp_path / "n.json"
    n.write_text(json.dumps([{"cls": "t", "name": "f", "key": "t.f", "min": 0, "max": 0, "types": [None] * 10, "ret": "int",
                              "ret_class": None, "winner": True, "default": None, "sets_fpu_pc64": False, "body": "0x1"}]))
    assert reach.stub_path_natives(osi, natives=n, mods=None, types=DATA / "types.json", frames=1) == {"t.f"}


def test_load_corpus_missing_files(tmp_path):
    assert reach.load_corpus(tmp_path) is None


# ----------------------------------------------------------------------------------------------- the script walk

SCRIPT = """
.strings "t" "f"
.function Setup 0
    CreateObject A
    Dup
    GetMemberFunction .run
    JumpAbsolute 1
    Pop
    CreateObject A
    Dup
    GetMemberFunction .dyn
    JumpAbsolute 1
    Pop
    CreateObject A
    PushConstantString "vialiteral"
    GetMemberFunctionFromString
    JumpAbsolute 1
    Pop
    Dup
    GetMemberFunction .hidden
    Pop
    PushConstanti32 @gfn
    JumpRelative 0
    Pop
    PushConstant0
    Return
.function Process 3
    PushConstanti8 1
    Return
.function Input 5
    PushConstanti8 1
    Return
.function OnEvent 10
    PushConstanti8 1
    Return
.function gfn 0
gfn:
    CallGameFunction t.f 2
    Pop
    PushConstant0
    Return
.function orphan 0
    CallGameFunction t.f 3
    Pop
    PushConstant0
    Return
.class A props(f)
.method run
    PushConstant0
    Return
.method dyn
    GetThisMemberValue .f
    GetMemberFunctionFromString
    JumpAbsolute 1
    Return
.method vialiteral
    CallGameFunction t.f 2
    Pop
    PushConstant0
    Return
.method walknever
    CallGameFunction t.f 2
    Pop
    PushConstant0
    Return
.class B props(f)
.method hidden
    CallGameFunction t.f 2
    Pop
    PushConstant0
    Return
"""
ROWS = [{"key": "t.f", "cls": "t", "name": "f", "min": 1, "max": 1, "winner": True}]


def assemble_walk():
    image, _ = osiasm.assemble(SCRIPT)
    return reach.Walk(image)


@pytest.fixture(scope="module")
def walk():
    return assemble_walk()


def test_roots_are_the_four_engine_entry_names(walk):
    assert sorted(walk.names(o)[0] for o in walk.roots) == ["input", "onevent", "process", "setup"]


def test_functions_reached_by_a_pushed_offset_and_jump(walk):
    reached = {n for o in walk.reachable for n in walk.names(o)}
    assert "gfn" in reached and "orphan" not in reached


def test_methods_need_a_constructed_class_and_a_taken_symbol(walk):
    reached = {n for o in walk.reachable for n in walk.names(o)}
    assert {"a.run", "a.dyn", "a.vialiteral"} <= reached          # 0x13 A + 0x12 .run / .dyn / the literal 0x19
    assert "a.walknever" not in reached                            # A is constructed but nothing takes this symbol
    assert "b.hidden" not in reached                               # the symbol IS taken (0x12 .hidden), B is never built


def test_0x19_sources(walk):
    sites = walk.sites_0x19()
    assert [(s.names, s.source, s.resolved) for s in sites] == [          # in pc order: Setup precedes the methods
        (["setup"], 'literal "vialiteral"', "vialiteral"), (["a.dyn"], "GetThisMemberValue f", None)]
    assert [s.verdict for s in sites] == ["REACHABLE", "REACHABLE"]
    assert reach.format_0x19(sites[1]).startswith("0x19 pc 0x")
    assert "=> UNRESOLVED" in reach.format_0x19(sites[1]) and "=> RESOLVED vialiteral" in reach.format_0x19(sites[0])
    assert 'source literal "vialiteral"' in reach.format_0x19(sites[0])


def test_argc_sites_and_reasons(walk):
    sites = walk.argc_sites(ROWS)
    got = {tuple(s.names): (s.verdict, s.wild) for s in sites}
    assert got == {
        ("gfn",): ("REACHABLE", "REACHABLE"),
        ("orphan",): ("UNREACHABLE", "UNREACHABLE"),
        ("a.vialiteral",): ("REACHABLE", "REACHABLE"),
        ("a.walknever",): ("UNREACHABLE", "REACHABLE"),      # only an UNRESOLVED 0x19 in reachable a.dyn could take it
        ("b.hidden",): ("UNREACHABLE", "UNREACHABLE"),
    }
    by = {tuple(s.names): s for s in sites}
    assert "never called" in by[("orphan",)].reason
    assert "never constructed" in by[("b.hidden",)].reason
    assert "symbol" in by[("a.walknever",)].reason and "never taken" in by[("a.walknever",)].reason
    assert by[("gfn",)].reason.startswith("reached: setup")
    assert (by[("gfn",)].key, by[("gfn",)].argc, by[("gfn",)].lo, by[("gfn",)].hi) == ("t.f", 2, 1, 1)
    line = reach.format_argc(by[("orphan",)])
    assert line.startswith("UNREACHABLE t.f/3 pc 0x") and " in orphan " in line


def test_argc_summary_line(walk):
    sites = walk.argc_sites(ROWS)
    assert reach.argc_summary(sites) == (
        "argc-violations: 5 sites, 2 REACHABLE, 3 UNREACHABLE (M2a-0 claim 'all statically unreachable': REFUTED); "
        "with an UNRESOLVED 0x19 allowed to name any method: 1 more REACHABLE")


# ----------------------------------------------------------------------------------------------- the real files

@pytest.fixture(scope="module")
def corpus():
    c = reach.load_corpus(REPO / "gen" / "decomp")
    if c is None:
        pytest.skip("SKIPPED (run make decomp first)")
    return c


@pytest.fixture(scope="module")
def winners():
    return [r for r in json.loads((DATA / "natives.json").read_text()) if r["winner"]]


def test_real_vm_reentry_count_and_depths(corpus, winners):
    """Measured here: 35 of 1,049 natives reach 0x60b320 / 0x60b450 through direct calls; 71 direct callers."""
    res = reach.vm_reentry(corpus, winners)
    assert len(winners) == 1049 and len(res) == 35
    assert len(reach.direct_callers(corpus)) == 71
    by = {r.key: r.depth for r in res}
    for key, depth in (("gcmodeldirector.addtoa", 3), ("gccharacter.setupmoveable", 4),
                       ("gcareadirector.setcurrentlevel", 5), ("gcareadirector.setcurrentarea", 6)):
        assert by[key] == depth, key


def test_real_on_path_natives_are_the_four(corpus, winners, lomn_data):
    p = lomn_data / "install" / "data" / "Base.osi"
    if not p.exists():
        pytest.skip("SKIPPED (no Base.osi in LOMN_DATA)")
    hit = reach.stub_path_natives(p)
    assert len(hit) == 69
    on = [r for r in reach.vm_reentry(corpus, winners) if r.key in hit]
    assert sorted(r.key for r in on) == ["gcareadirector.setcurrentarea", "gcareadirector.setcurrentlevel",
                                         "gccharacter.setupmoveable", "gcmodeldirector.addtoa"]


def test_real_indirect_entries(corpus, winners):
    """49 functions of the 226-function closure have no caller and are no native body (EXECUTED by the review and here);
    among them the ones named in the review."""
    ind = reach.indirect_entries(corpus, winners)
    names = {corpus.names[i.addr] for i in ind}
    assert len(ind) == 49
    assert {"GcTimer::Trigger", "GcLegoCamera::Process", "ScSLOSIProcess::Process", "_SafMainCRTStartup"} <= names
    assert any("vector_deleting_destructor" in n and n.startswith("GcHive") for n in names)


def test_real_indirect_entries_have_exe_references(corpus, winners, canonical_exe):
    sys.path[:0] = [str(REPO / "tools" / "osi")]
    import exc
    img = exc.Image.from_file(canonical_exe)
    ind = reach.indirect_entries(corpus, winners)
    got = reach.find_entries(img, corpus, [i.addr for i in ind])
    by = {corpus.names[i.addr]: got[i.addr] for i in ind}
    assert any(e.startswith("vtable ") for e in by["GcTimer::Trigger"])
    assert by["GcHive::`vector_deleting_destructor'"] and by["GcHive::`vector_deleting_destructor'"][0].startswith("vtable ")


def test_cli_vm_reentry_last_line(corpus, lomn_data):
    r = subprocess.run([sys.executable, str(REPO / "tools" / "osi" / "reach.py"), "--vm-reentry"], capture_output=True,
                       text=True, timeout=110, env={"PATH": "/usr/bin:/bin", "LOMN_DATA": str(lomn_data)})
    assert r.returncode == 0, r.stderr
    last = r.stdout.splitlines()[-1]
    assert last.startswith("vm-reentry (direct-call floor): 35 of 1049 natives reach 0x60b320/0x60b450 through direct "
                           "calls (direct callers of the targets: 71); 49 closure functions are entered only through "
                           "vtables or callbacks, so more natives may re-enter; on the stub path 4: ")
    assert sum(1 for ln in r.stdout.splitlines() if ln.startswith("reentry ")) == 35
    assert sum(1 for ln in r.stdout.splitlines() if ln.startswith("indirect ")) == 49


@pytest.fixture(scope="module")
def real_walk(lomn_data):
    p = lomn_data / "install" / "data" / "Base.osi"
    if not p.exists():
        pytest.skip("SKIPPED (no Base.osi in LOMN_DATA)")
    return reach.Walk(p.read_bytes())


def test_real_argc_violations_are_all_unreachable(real_walk, winners):
    """M2a-0 claimed: 'all statically UNREACHABLE (lego_timer.duration* never looked up; lego3dsound / ss_base3dobject /
    ss_base3dcharacter never constructed; cavedigging only via setuptestlevel, which the engine never calls)'. The walk
    (rule: roots setup/process/input/onevent; a 0x13 builds a class; a 0x12/0x17/0x19 symbol reaches that method of every
    constructed class) agrees: 20 sites, 0 reachable, also with an UNRESOLVED 0x19 allowed to name any method."""
    sites = real_walk.argc_sites(winners)
    assert len(sites) == 20 and {s.key for s in sites} == {
        "gc3dsound.synctocharacter", "gc3dsound.synctoobject", "gcareadirector.hide", "gcmodeldirector.addcharacter",
        "gcmodeldirector.hide", "gcmodeldirector.remove", "gcmodeldirector.setinputstate", "gcmodeldirector.show",
        "gctimer.durationaverage", "gctimer.durationpeek"}
    assert [s.verdict for s in sites].count("UNREACHABLE") == 20
    assert [s.wild for s in sites].count("UNREACHABLE") == 20
    owners = {n.split(".")[0] for s in sites for n in s.names}
    assert owners == {"lego3dsound", "cavedigging", "lego_timer", "ss_base3dobject", "ss_base3dcharacter"}


def test_real_0x19_sites_are_twelve_and_all_unresolved(real_walk):
    sites = real_walk.sites_0x19()
    assert len(sites) == 12
    assert all(s.resolved is None for s in sites)
    assert {n for s in sites for n in s.names} == {"base_safeevent.execute", "base_safeevent.executearguments",
                                                  "gs_tweakobject.incvalue", "gs_tweakobject.decvalue"}
    assert [s.verdict for s in sites].count("UNREACHABLE") == 12


def test_real_walk_totals(real_walk):
    """Closure sizes (3,606 subroutines = 23 functions + 3,583 distinct method bodies)."""
    assert len(real_walk.code) == 3606
    assert len(real_walk.reachable) == 2825
