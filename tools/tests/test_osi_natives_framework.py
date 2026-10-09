"""M2a-2 T4: the oracle's native framework (tools/research/osi_natives/framework.py and the osi_vm.py changes), written
from docs/osi/natives.md (§1.2 the native ABI, §2 game objects/D13, §3 natives.log v2/D14 and its replay rules, §5.4 D16,
§8.3 the name bound), docs/osi/trace.md §6, docs/osi/conformance.md rules 15-22 and the error-PC table of
docs/osi/opcodes.md. Never from the C++ engine (two-engine independence).

The eight framework behaviours (plan Task 4, the same as Task 3's): the heap's engine references; create_variant (null,
first mint, identity); release to 0 (F g, then the host-object delete); the log v2 records byte for byte against the
worked example of natives.md §3.4; the engine-call nesting high-water; D16 (n-reenter-nested-error-swallowed); the
parked N2 case (FindMemberID with an empty symbol table); the handle wrap guard. Then the replay rules, the name bound,
the error-PC pins, the runner keys (group n on a temp copy without `pending`) and the run flags."""
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "osi"))
sys.path.insert(0, str(REPO / "tools" / "research"))

import osiasm  # noqa: E402
import osi_vm  # noqa: E402
from osi_vm import NOTHING, Variant  # noqa: E402

TYPES = osi_vm.TypeTable.load(REPO / "data" / "osi" / "types.json")
GROUP_N = REPO / "tests" / "osi" / "conformance" / "n"
NOTHING10 = ["000f:00000000"] * 10


def fw():
    from osi_natives import framework
    return framework


def I(n):
    return Variant(0x10, n & 0xFFFFFFFF)


def _vm(asm=".function main 0\nReturn\n", reg=None):
    image, info = osiasm.assemble(asm)
    tr = osi_vm.ListTrace()
    heap = osi_vm.Heap(tr)
    reg = reg or osi_vm.NativeRegistry(TYPES)
    vm = osi_vm.Vm(image, reg, heap, tr)
    return vm, heap, tr, info


def _ctx(vm, log=None):
    f = fw()
    return f.HostContext(vm, vm.heap, None, f.HostFlags(), log)


def _entry(cls, name, n, fn, types=None):
    tys = tuple(types or ()) + (0x0F,) * (10 - len(types or ()))
    return osi_vm.NativeEntry(cls, name, (cls + "." + name).lower(), n, n, tys, "test", 0, None, fn, "test")


# ------------------------------------------------------------------------------------------- 1. heap engine refs
def test_heap_stores_engine_plus_variant_references():               # D13; natives.md §2.4 "total = engine + variants"
    tr = osi_vm.ListTrace()
    heap = osi_vm.Heap(tr)
    h = heap.new_game(0x27, 1)
    assert tr.lines == ["A g 00000001 27"] and heap.games[h][0] == 2 and heap.engine(h) == 1
    heap.dispose(Variant(0x27, h))                                    # the script drops its only variant
    assert h in heap.games and tr.lines == ["A g 00000001 27"]
    heap.release(h)                                                   # the engine lets go: total 0 -> F g
    assert tr.lines[-1] == "F g 00000001" and h not in heap.games
    h2 = heap.new_game(0x27)                                          # engine_refs omitted = M2a-1's new_game(type)
    assert h2 == 2 and heap.games[h2][0] == 1 and heap.engine(h2) == 0
    heap.add_ref(h2)
    assert heap.games[h2][0] == 2 and heap.engine(h2) == 1
    with pytest.raises(osi_vm.VmError):
        heap.add_ref(99)                                              # not live


# ------------------------------------------------------------------------------------------- 2. create_variant
def test_create_variant_null_first_mint_and_identity():              # natives.md §1.2 Results, §2.1, §2.3
    f = fw()
    vm, heap, tr, _ = _vm()
    ctx = _ctx(vm)
    assert ctx.create_variant(None) == NOTHING and tr.lines == []    # null -> {0x0f, 0}, no handle, no A g
    toa = f.GameObject(0x37, 1)
    assert toa.handle == 0                                            # minted at the first CreateVariant, not at construction
    v = ctx.create_variant(toa)
    assert v == Variant(0x37, 1) and toa.handle == 1 and tr.lines == ["A g 00000001 37"]
    assert heap.games[1][0] == 2 and heap.engine(1) == 1             # engine 1 + this variant
    v2 = ctx.create_variant(toa, 0x22)                                # the same Toa returned as GcCharacter (§2.3)
    assert v2 == Variant(0x22, 1) and tr.lines == ["A g 00000001 37"] and heap.games[1][0] == 3
    vm.push(v)
    vm.push(NOTHING)
    call = osi_vm.NativeCall(vm, None, 0, [0, 1])
    assert ctx.object_arg(call, 0) is toa and ctx.object_arg(call, 1) is None
    assert ctx.object_arg(call, 0, f.GameObject) is toa


# ------------------------------------------------------------------------------------------- 3. release to 0
def test_release_to_zero_writes_F_g_then_deletes_the_host_object():  # natives.md §2.2 (Dispose 0x60a94c, Free 0x60a957)
    f = fw()
    vm, heap, tr, _ = _vm()
    ctx = _ctx(vm)
    events = []

    class Sprite(f.GameObject):
        def on_delete(self):
            events.append(("delete", list(tr.lines)))

    s = Sprite(0x26, 0)
    v = ctx.create_variant(s)                                         # total 1 (the variant)
    ctx.add_ref(s)                                                    # the engine stores it: engine 1, total 2
    heap.dispose(v)                                                   # the script drops it: total 1, still live
    assert 1 in heap.games and events == [] and s.handle == 1
    ctx.release(s)                                                    # 0 -> F g, THEN the host object is deleted
    assert tr.lines[-1] == "F g 00000001"
    assert events == [("delete", ["A g 00000001 26", "F g 00000001"])]
    assert s.handle == 0 and s.deleted
    t = f.GameObject(0x26, 0)
    assert ctx.create_variant(t).value == 2                           # handles are never reused (D7)


# ------------------------------------------------------------------------------------------- 4. log v2 (worked example)
def _worked_example_lines():
    text = (REPO / "docs" / "osi" / "natives.md").read_text()
    sec = text[text.index("### 3.4 Worked example"):]
    block = sec[sec.index("```\n") + 4:]
    return block[:block.index("```")].rstrip("\n").split("\n")


WORKED_ASM = """.function onevent 10 @ev
.function frame0 0 @f0
.function setup 0
    CallGameFunction gcgame.getkeymapper 0
    Pop
    PushConstanti8 0
    PushConstanti8 0
    PushConstantf32 5.0
    CallGameFunction gctimer.create 3
    Pop
    PushConstantString "onua"
    CallGameFunction gcmodeldirector.addtoa 1
    Pop
    CallGameFunction gcmodeldirector.gettoa 0
    Pop
    PushConstanti32 0x636e7431
    PushConstanti8 0
    CallGameFunction gcmodeldirector.show 2
    Pop
    PushConstanti32 0x636e7431
    CallGameFunction gcmodeldirector.getcharacter 1
    Pop
    PushConstanti8 1
    PushConstanti8 2
    PushConstanti8 3
    PushConstanti8 4
    PushConstanti8 5
    CallGameFunction gcbigsprite.loadhudsprite 5
    CallGameFunction gchuddirector.addbigsprite 1
    Pop
    PushNothing
    Return
ev:
    PushConstanti8 1
    PushConstanti8 2
    CallGameFunction sctextobject.setstring 2
    Pop
    PushNothing
    Return
f0:
    CallGameFunction gcsaver.getmask 0
    Return
"""
WORKED_SIG = [("gcgame", "getkeymapper", ()), ("gctimer", "create", (0x10, 0x10, 0x12)),
              ("gcmodeldirector", "addtoa", (0x16,)), ("sctextobject", "setstring", (0x10, 0x10)),
              ("gcmodeldirector", "gettoa", ()), ("gcmodeldirector", "show", (0x10, 0x10)),
              ("gcmodeldirector", "getcharacter", (0x10,)), ("gcbigsprite", "loadhudsprite", (0x10,) * 5),
              ("gchuddirector", "addbigsprite", (0x26,)), ("gcsaver", "getmask", ())]


def _worked_registry(live: bool):
    """The natives of the worked example, modelled with the framework (live) or as bodies that must never run (replay)."""
    f = fw()
    reg = osi_vm.NativeRegistry(TYPES)
    st = {}

    def body(key):
        def fn(call):
            ctx = call.vm.host
            if key == "gcgame.getkeymapper":
                return ctx.create_variant(f.GameObject(0x3E, 1))      # held by the global ScAutoRefPtr (§2.4): G 1
            if key == "gctimer.create":
                return ctx.create_variant(f.GameObject(0x27, 0))      # constructed by the native: G 0
            if key == "gcmodeldirector.addtoa":
                ctx.engine_call("OnEvent", [I(0x32), I(250)])        # §4: (int 0x32, int 250), nothing x 8
                return I(1)
            if key == "gcmodeldirector.gettoa":
                st["toa"] = f.GameObject(0x37, 1)                     # the model-director map: G 1
                return ctx.create_variant(st["toa"])
            if key == "gcmodeldirector.show":
                ctx.add_ref(st["toa"])                                # GcBAGroup::SetCharacter +1 (0x54b344)
                return NOTHING
            if key == "gcmodeldirector.getcharacter":
                return ctx.create_variant(st["toa"], 0x22)            # the same Toa as GcCharacter (§2.3)
            if key == "gcbigsprite.loadhudsprite":
                return ctx.create_variant(f.GameObject(0x26, 0))
            if key == "gchuddirector.addbigsprite":
                ctx.add_ref(ctx.object_arg(call, 0))                  # stores its argument: +1
                return NOTHING
            if key == "gcsaver.getmask":
                return I(0x3F3F3F3F)
            return NOTHING
        return fn

    for cls, name, tys in WORKED_SIG:
        key = cls + "." + name
        reg.add(_entry(cls, name, len(tys), body(key) if live else None, list(tys)))
    return reg


def _run_worked(reg, log=None):
    vm, heap, tr, info = _vm(WORKED_ASM, reg)
    reg.return_log = log
    _ctx(vm, log)
    r = vm.call("setup", [])
    assert r.ok, vm.last_error and vm.last_error.message
    vm.begin_frame(0)
    r = vm.call("frame0", [])
    assert r.ok, vm.last_error and vm.last_error.message
    return vm, tr


def test_log_v2_records_byte_exact_against_the_worked_example():    # natives.md §3.1, §3.2, §3.4; trace.md §6 v2
    log = []
    _run_worked(_worked_registry(True), log)
    want = _worked_example_lines()
    assert len(want) == 14
    assert log == want


def test_replay_of_the_worked_example_applies_H_performs_C_and_checks_G(tmp_path):   # natives.md §3.3 (I6)
    log = []
    _, live = _run_worked(_worked_registry(True), log)
    p = tmp_path / "natives.log"
    p.write_text("\n".join(log) + "\n")
    reg = _worked_registry(False)
    reg.replay_from(p)
    _, rep = _run_worked(reg)
    assert rep.lines == live.lines
    assert any(l.startswith("C OnEvent 10 ") for l in rep.lines)     # the C 2 record was performed inside addtoa
    assert reg.replay_pos == len(log)


def _replay_mutated(tmp_path, edit):
    log = []
    _run_worked(_worked_registry(True), log)
    p = tmp_path / "natives.log"
    p.write_text("\n".join(edit(list(log))) + "\n")
    reg = _worked_registry(False)
    reg.replay_from(p)
    with pytest.raises(osi_vm.ReplayDesync) as ei:
        _run_worked(reg)
    return str(ei.value)


def test_replay_G_mismatch_is_a_desync(tmp_path):
    m = _replay_mutated(tmp_path, lambda l: [x.replace("0022:00000003 G 2", "0022:00000003 G 3") for x in l])
    assert m == "G check at seq 6: handle 3, engine count 2, logged G 3"


def test_replay_without_the_H_record_fails_the_next_G_check(tmp_path):  # H is authoritative, G a checked snapshot
    m = _replay_mutated(tmp_path, lambda l: [x for x in l if x != "H 0037:00000003 +1"])
    assert m == "G check at seq 6: handle 3, engine count 1, logged G 2"


def test_replay_C_of_another_native_is_a_desync(tmp_path):
    m = _replay_mutated(tmp_path, lambda l: [x.replace("C 00000002 OnEvent", "C 00000001 OnEvent") for x in l])
    assert m.startswith("replay desync at seq 2: C 00000001")


def test_replay_X_after_a_call_that_did_not_raise_is_a_desync(tmp_path):   # review m-2
    def edit(l):
        i = l.index("00000003 sctextobject.setstring 2 -> 000f:00000000")
        return l[:i + 1] + ["X 00000002"] + l[i + 1:]
    assert _replay_mutated(tmp_path, edit) == "replay desync at seq 2: X 00000002 after a nested call that did not raise"


def test_replay_missing_K_is_a_desync(tmp_path):                     # K is written for every frame
    m = _replay_mutated(tmp_path, lambda l: [x for x in l if not x.startswith("K ")])
    assert m.startswith("replay desync at frame 0: expected K") and "00000009 gcsaver.getmask" in m


def test_replay_accepts_a_v1_log(tmp_path):                          # natives.md §3.3: no G means 0, nothing checked
    asm = ".function main 0\nCallGameFunction t.o 0\nCallGameFunction t.o 0\nPop\nPop\nPushNothing\nReturn\n"
    reg = osi_vm.NativeRegistry(TYPES)
    reg.add(_entry("t", "o", 0, None))
    p = tmp_path / "v1.log"
    p.write_text("00000000 t.o 0 -> 0027:00000001\n00000001 t.o 0 -> 0027:00000001\n")
    reg.replay_from(p)
    vm, heap, tr, _ = _vm(asm, reg)
    assert vm.call("main", []).ok
    assert [l for l in tr.lines if l[:2] in ("A ", "F ")] == ["A g 00000001 27", "F g 00000001"]
    vm.begin_frame(0)                                                 # v1: no K expected


# ------------------------------------------------------------------------------------------- 5. nesting high-water
DEPTH_ASM = """.function inner 0 @inner
.function main 0
    CallGameFunction t.re 0
    Return
inner:
    CallGameFunction t.re 0
    Return
"""


def test_engine_call_nesting_high_water_and_C_seq():                 # D15 (max_nesting), natives.md §1.2 Nested calls
    reg = osi_vm.NativeRegistry(TYPES)

    def re_fn(call):
        if call.vm.context_depth < 3:
            call.vm.host.engine_call("inner", [])
        return I(7)
    reg.add(_entry("t", "re", 0, re_fn))
    log = []
    reg.return_log = log
    vm, heap, tr, _ = _vm(DEPTH_ASM, reg)
    _ctx(vm, log)
    assert vm.max_nesting == 0
    assert vm.call("main", []).ok
    assert vm.max_nesting == 3                                        # main + 2 nested engine calls in flight at most
    assert log == ["C 00000000 inner " + " ".join(NOTHING10), "C 00000001 inner " + " ".join(NOTHING10),
                   "00000002 t.re 0 -> 0010:00000007", "00000001 t.re 0 -> 0010:00000007",
                   "00000000 t.re 0 -> 0010:00000007"]


# ------------------------------------------------------------------------------------------- 6. D16
SWALLOW_ASM = """.function onevent 0 @ev
.function main 0
    PushConstanti8 1
c:
    CallGameFunction t.sw 0
after_c:
    Pop
    PushConstanti8 2
    Return
ev:
    DebugOn
    PushConstanti8 7
    Return
"""


def test_swallowing_engine_call_writes_E_P_and_X_and_continues_at_the_error_pc(tmp_path):   # D16 rules 1-4
    reg = osi_vm.NativeRegistry(TYPES)

    def sw(call):
        r = call.vm.host.engine_call("onevent", [], swallow=True)
        assert not r.ok
        return I(5)
    reg.add(_entry("t", "sw", 0, sw))
    log = []
    reg.return_log = log
    vm, heap, tr, info = _vm(SWALLOW_ASM, reg)
    _ctx(vm, log)
    res = vm.call("main", [])
    ev = info["labels"]["ev"]
    assert res.ok and res.result == I(7)
    i = tr.lines.index('E %08x 02    3 "Debugger not enabled! but encountered DebugOn"' % ev)
    assert tr.lines[i - 1].startswith("I %08x 02" % ev) and tr.lines[i + 1] == "P %08x 64" % ev
    assert sum(1 for l in tr.lines if l.startswith("E ")) == 1 and vm.last_error is None
    assert [(e.message, e.pc, e.op, e.depth, e.top) for e in vm.swallowed] == [
        ("Debugger not enabled! but encountered DebugOn", ev, 0x02, 3, Variant(0xFF, info["labels"]["c"] + 6))]
    assert vm.context_depth == 1 and vm.pc == info["labels"]["after_c"] and vm.depth == 2
    assert log == ["C 00000000 onevent " + " ".join(NOTHING10), "X 00000000", "00000000 t.sw 0 -> 0010:00000005"]
    # the replay applies the swallow only because the X record is there (natives.md §3.3, review m-2)
    p = tmp_path / "sw.log"
    p.write_text("\n".join(log) + "\n")
    reg2 = osi_vm.NativeRegistry(TYPES)
    reg2.add(_entry("t", "sw", 0, None))
    reg2.replay_from(p)
    vm2, _, tr2, _ = _vm(SWALLOW_ASM, reg2)
    assert vm2.call("main", []).ok and tr2.lines == tr.lines
    p.write_text("\n".join(l for l in log if not l.startswith("X ")) + "\n")
    reg3 = osi_vm.NativeRegistry(TYPES)
    reg3.add(_entry("t", "sw", 0, None))
    reg3.replay_from(p)
    vm3, _, tr3, _ = _vm(SWALLOW_ASM, reg3)
    r3 = vm3.call("main", [])                                          # no X: the error propagates out of the native
    assert not r3.ok and vm3.last_error.message == "Script is calling t.sw"


def _uncompared_copy(tmp_path, ids):
    c = tmp_path / "corpus" / "n"
    c.mkdir(parents=True)
    for i in ids:
        shutil.copy(GROUP_N / (i + ".asm"), c / (i + ".asm"))
        d = json.loads((GROUP_N / (i + ".expect.json")).read_text())
        d.pop("pending", None)
        (c / (i + ".expect.json")).write_text(json.dumps(d))
    return c


# Since the merge of Tasks 3+4 (2026-10-09) every group-n program is active; the list is all of group n, so the
# parametrized test below never silently shrinks to zero cases.
PENDING_N = sorted(p.name[:-len(".expect.json")] for p in GROUP_N.glob("*.expect.json"))


def test_the_group_n_programs_this_task_serves_are_all_active():
    assert len(PENDING_N) == 13 and "n-reenter-nested-error-swallowed" in PENDING_N
    assert not [p for p in PENDING_N if "pending" in json.loads((GROUP_N / (p + ".expect.json")).read_text(encoding="latin-1"))]


@pytest.mark.parametrize("pid", PENDING_N)
def test_group_n_program_passes_on_the_oracle_without_its_pending_key(tmp_path, pid):   # rules 15-22
    c = _uncompared_copy(tmp_path, [pid])
    assert osi_vm.run_program(c / (pid + ".asm")) == []


def test_d16_the_corrupted_swallow_key_bites(tmp_path):              # Task 10 mutated-key check, on one key here
    c = _uncompared_copy(tmp_path, ["n-reenter-nested-error-swallowed"])
    p = c / "n-reenter-nested-error-swallowed.expect.json"
    d = json.loads(p.read_text())
    d["natives"][0]["swallow"] = False
    p.write_text(json.dumps(d))
    assert osi_vm.run_program(c / "n-reenter-nested-error-swallowed.asm") != []


# ------------------------------------------------------------------------------------------- 7. parked N2
def test_find_member_id_with_an_empty_symbol_table():                # 0x609c57 xor esi,esi; 0x609cb0 stores it
    image, _ = osiasm.assemble(".function main 0\nReturn\n")
    s = osi_vm.Script(image)
    assert s.symbols == []
    assert s.find_member_id(b"x") == 0xFFFF                           # the miss returns 0xffff (0x609baf) ...
    assert s.member_cache == [(None, 0xFFFF)]                         # ... and caches a null symbol pointer
    with pytest.raises(osi_vm.UndefinedBehaviour):                    # the next cache scan would strcmp it (0x609c20)
        s.find_member_id(b"x")


# ------------------------------------------------------------------------------------------- 8. wrap guard
@pytest.mark.parametrize("kind", ["o", "a", "s", "g"])
def test_handle_space_exhaustion_is_an_error_not_a_wrap(kind):       # D7 handles never reused (parked N4)
    heap = osi_vm.Heap()
    new = {"o": lambda: heap.new_object(0, 0), "a": lambda: heap.new_array(0), "s": lambda: heap.new_string(b""),
           "g": lambda: heap.new_game(0x27)}[kind]
    heap.next[kind] = 0xFFFFFFFE                                      # test-only seam
    assert new() == 0xFFFFFFFE
    with pytest.raises(osi_vm.VmError) as ei:
        new()
    assert ei.value.message == "Undefined: handle space exhausted"


# ------------------------------------------------------------------------------------------- name bound (§8.3)
def _hostile(asm_body, name_len, pre=""):
    """String entry 0 is 255 'a': with a NUL terminator (strlen 255), or with the terminator byte 'a' and an empty entry
    after it (strlen 256: the C-string read runs into the next entry, natives.md §8.3; corpus n-name-256)."""
    raw = '.rawstring "%s" 0x61\n.strings ""\n' % ("a" * 255) if name_len == 256 else '.strings "%s"\n' % ("a" * 255)
    return pre + raw + ".function main 0\n" + asm_body.replace("NAME", "a" * 255) + "\n"


@pytest.mark.parametrize("mn,obj", [("GetMemberValueFromString", "PushConstanti8 1"),
                                    ("SetMemberValueFromString", "PushConstanti8 1")])
def test_member_name_copy_bound_0x18_0x1a(mn, obj):                  # 0x5fef3b / 0x5ff49b; operator= seta 0x4f5c27
    body = ("PushConstanti8 5\n" if mn.startswith("Set") else "") + obj + '\nPushConstantString "NAME"\nat:\n' + mn + "\nReturn"
    vm, res, tr, info = _run(_hostile(body, 256))
    e = vm.last_error
    assert not res.ok and e.message == "kOutOfRange" and e.pc == info["labels"]["at"]
    assert vm.snapshot().pc == info["labels"]["at"] + 1               # +1 (opcodes.md error-PC table)
    vm, res, tr, info = _run(_hostile(body, 255))                     # 255 passes the copy: the object check fails
    assert not res.ok and vm.last_error.message in ("Member property of non member variable",
                                                      "Member property of non object variable")


def test_member_function_name_copy_bound_0x19():                     # 0x5ff2e7: after the object check (0x5ff2c2)
    pre = ".class C props(p)\n"
    body = 'CreateObject C\nPushConstantString "NAME"\nat:\nGetMemberFunctionFromString\nReturn'
    vm, res, tr, info = _run(_hostile(body, 256, pre=pre))
    assert vm.last_error.message == "kOutOfRange" and vm.snapshot().pc == info["labels"]["at"] + 1
    vm, res, tr, info = _run(_hostile(body, 255, pre=pre))
    assert vm.last_error.message == "Call to undefined member method"


def _run(asm):
    vm, heap, tr, info = _vm(asm)
    return vm, vm.call("main", []), tr, info


# ------------------------------------------------------------------------------------------- error PCs (rule 15)
def test_0x82_string_index_error_is_at_plus_5_with_the_instruction_pc():   # rule 15(c); T2a-pc oracle change 1
    vm, res, tr, info = _run(".function main 0\nat:\n.byte 0x82 0x63 0x00 0x00 0x00 0x00\nReturn\n")
    e = vm.last_error
    at = info["labels"]["at"]
    assert not res.ok and e.message == "Undefined behaviour: string-table index 99 out of range"
    assert (e.pc, e.op) == (at, 0x82) and vm.snapshot().pc == at + 5


# ------------------------------------------------------------------------------------------- run flags and K
RERUN_ASM = REPO / "engine" / "tests" / "data" / "osi_a6_rerun.asm"


def test_run_flags_real_natives_install_overrides_and_K_lines(tmp_path):    # plan Task 4 Files; natives.md §3.1 K
    osi = tmp_path / "rerun.osi"
    image, _ = osiasm.assemble(RERUN_ASM.read_text())
    osi.write_bytes(image)
    py = [sys.executable, str(REPO / "tools" / "research" / "osi_vm.py"), "--run", str(osi), "--frames", "6",
          "--keys", "esc@1"]
    a = subprocess.run(py + ["--digest", str(tmp_path / "a.digest"), "--log-out", str(tmp_path / "a.log")],
                       capture_output=True, text=True, timeout=110)
    assert a.returncode == 0, a.stderr
    (tmp_path / "ov.json").write_text("[]")
    inst = tmp_path / "install"
    inst.mkdir()
    b = subprocess.run(py + ["--real-natives", "--install", str(inst), "--overrides", str(tmp_path / "ov.json"),
                             "--digest", str(tmp_path / "b.digest")], capture_output=True, text=True, timeout=110)
    assert b.returncode == 0, b.stderr
    c = subprocess.run(py + ["--replay", str(tmp_path / "a.log"), "--digest", str(tmp_path / "c.digest")],
                       capture_output=True, text=True, timeout=110)
    assert c.returncode == 0 and "stop: none" in c.stdout, c.stdout + c.stderr
    d = [[l for l in (tmp_path / n).read_text().splitlines() if l.startswith("D ")] for n in ("a.digest", "b.digest", "c.digest")]
    assert d[0] and d[0] == d[1] == d[2]
    log = (tmp_path / "a.log").read_text().splitlines()
    assert [l for l in log if l.startswith("K ")] == ["K %6d" % k for k in range(6)]


def test_overrides_set_tier_and_default_on_the_named_row(tmp_path):   # natives.md §7.2 (the merge, done at load)
    reg = osi_vm.NativeRegistry.load(osi_vm.DEFAULT_NATIVES, osi_vm.DEFAULT_MODS, TYPES)
    ov = tmp_path / "ov.json"
    ov.write_text(json.dumps([{"key": "gcsaver.cheatmenu", "tier": "tier2", "default": {"type": "0x10", "value": 3},
                               "cite": "body 0x5d35b0", "note": "test"}]))
    osi_vm.apply_overrides(reg, ov)
    e = reg.find("gcsaver", "cheatmenu")
    assert e.tier == "tier2" and e.default == I(3)
    ov.write_text(json.dumps([{"key": "no.such", "tier": "tier2", "cite": "x", "note": "x"}]))
    with pytest.raises(LookupError):
        osi_vm.apply_overrides(reg, ov)


# ------------------------------------------------------------------------------------------- groups, Vfs, block files
def test_register_group_and_bind_natives():
    f = fw()
    vm, heap, tr, _ = _vm(reg=osi_vm.NativeRegistry.load(osi_vm.DEFAULT_NATIVES, osi_vm.DEFAULT_MODS, TYPES))
    ctx = _ctx(vm)
    seen = []

    def group(reg, c):
        seen.append(c)
        reg.bind("gcsaver.getmask", lambda call: I(0x3F3F3F3F))
    f.register_group("t4-test", group)
    try:
        f.bind_natives(vm.natives, ctx)
    finally:
        f.unregister_group("t4-test")
    assert seen == [ctx] and vm.natives.find("gcsaver", "getmask").tier == "implemented"


def test_subsystem_is_lazy_and_one_per_context():
    vm, heap, tr, _ = _vm()
    ctx = _ctx(vm)

    class Saver:
        def __init__(self, c):
            self.ctx = c
    a = ctx.subsystem(Saver)
    assert a is ctx.subsystem(Saver) and a.ctx is ctx and ctx.flags.deterministic and ctx.flags.skip_video


def test_vfs_normalises_and_resolves_root_case_insensitively(tmp_path):    # lomn::fs contract comment (fs.h)
    f = fw()
    (tmp_path / "Data" / "Art").mkdir(parents=True)
    (tmp_path / "Data" / "Art" / "Art.blk").write_bytes(b"xyz")
    v = f.Vfs(tmp_path)
    assert f.Vfs.normalize("Root\\\\Data//./art/../Art/ART.BLK") == "root/data/art/art.blk"
    assert f.Vfs.normalize("user/../x") == "x"
    assert v.exists("Root/Data/Art/Art.blk") and v.exists("root/DATA/art/art.blk")
    assert v.read("Root/data/art/art.blk") == b"xyz"
    assert not v.exists("Root/Data/AreaViewer.txt") and v.read("Root/Data/AreaViewer.txt") is None
    assert not v.exists("root/../Data/Art/Art.blk") and not v.exists("Root/Data/Art")   # BadMount; a directory


def test_block_files_read_the_blk_table(tmp_path):                   # tools/research/blk.py format; natives.md §8.2
    import struct
    f = fw()
    (tmp_path / "data").mkdir()
    ents = [("Box1.x", 10, 10, 2, 124), ("FRNT_OBJ.slb", 396, 396, 2, 134)]
    blob = struct.pack("<4sII", b"BLKF", 1, len(ents))
    for n, bs, fsz, fl, off in ents:
        blob += struct.pack("<40sIIII", n.encode("latin-1"), bs, fsz, fl, off)
    (tmp_path / "data" / "Test.blk").write_bytes(blob)
    b = f.BlockFiles(tmp_path)
    assert [e.name for e in b.entries("Root/Data/test.blk")] == ["Box1.x", "FRNT_OBJ.slb"]
    assert b.find("Root/Data/Test.blk", "frnt_obj.SLB").size == 396
    assert b.find("Root/Data/Test.blk", "frnt_hive.slb") is None
    assert b.entries("Root/Data/missing.blk") is None


def test_block_files_on_the_install(lomn_data):                      # natives.md §5.3 / §8.2 (EXECUTED listings)
    b = fw().BlockFiles(lomn_data / "install")
    frnt = b.entries("Root/Data/Levels/lev0/frnt.blk")
    assert len(frnt) == 16 and b.find("Root/Data/Levels/lev0/frnt.blk", "FRNT_HIVE.slb") is None
    assert b.entries("Root/Data/Art/Art.blk") is not None
    assert b.find("Root/Data/Art/Art.blk", "AreaViewer.txt") is None


# =========================================================================================== fix round 1 (review)
def _spec_vm(asm, specs, log=None):
    reg = osi_vm.NativeRegistry(TYPES)
    for sp in specs:
        reg.add(osi_vm.test_native_entry(sp, TYPES, reg))
    vm, heap, tr, info = _vm(asm, reg)
    if log is not None:
        reg.return_log = log
    _ctx(vm, log)
    return vm, heap, tr, info


SW_SPEC = {"cls": "t", "name": "sw", "min": 0, "max": 0, "types": [], "returns": {"type": "0x10", "value": 5},
           "swallow": True, "calls": {"entry": "onevent", "args": []}}


def test_d15_cap_counts_engine_calls_in_flight_not_context_depth():  # I1(a); controller ruling on D15
    """Three swallows leave context_depth at 3 for good (D16 rule 5). An unbounded re-entrant chain must still reach
    64 engine calls IN FLIGHT before the nesting-cap error; a cap on context_depth would stop it at 61."""
    asm = """.function onevent 0 @ev
.function inner 0 @inner
.function main 0
    CallGameFunction t.sw 0
    Return
inner:
    CallGameFunction t.re 0
    Return
ev:
    DebugOn
    PushConstanti8 7
    Return
"""
    re_spec = {"cls": "t", "name": "re", "min": 0, "max": 0, "types": [], "returns": {"type": "0x10", "value": 1},
               "calls": {"entry": "inner", "args": []}}
    vm, heap, tr, info = _spec_vm(asm, [SW_SPEC, re_spec])
    for _ in range(3):
        assert vm.call("main", []).ok
    assert vm.context_depth == 3 and vm.max_nesting == 2
    i0 = len(tr.lines)
    r = vm.call("inner", [])
    chain = tr.lines[i0:]
    assert not r.ok and vm.last_error.message == "Script is calling t.re"
    assert vm.max_nesting == 64                                       # the 65th call is refused before it starts
    assert sum(1 for l in chain if l.startswith("C inner ")) == 64
    assert vm.context_depth == 3 + 64
    assert sum(1 for l in chain if l.startswith("E ")) == 1


def test_d16_rule6_reset_restores_the_outermost_entry_state_over_leftovers():   # I1(b); D16 rule 6
    """n-swallow-writes leaves [M0, int 9] and context_depth 1. A second call then errors; reset_after_error() must
    pop only that call's own region and restore ITS entry state, not M2a-1's (context_depth 0, rescan, refcount 0)."""
    asm = (GROUP_N / "n-swallow-writes.asm").read_text().replace(
        ".function main2 0 @m2", ".function boom 0 @boom\n.function main2 0 @m2") + "boom:\n    PushConstanti8 4\n    DebugOff\n    Return\n"
    spec = json.loads((GROUP_N / "n-swallow-writes.expect.json").read_text())
    vm, heap, tr, info = _spec_vm(asm, spec["natives"])
    assert vm.call("main", []).ok
    assert (vm.depth, vm.top(), vm.context_depth, vm.pc, vm.frame) == (2, I(9), 1, info["labels"]["after_c"], 1)
    r = vm.call("boom", [])
    assert not r.ok and vm.last_error.message == "Debugger not enabled! but encountered DebugOff"
    assert vm.depth == 4 and vm.context_depth == 2
    vm.reset_after_error()
    assert (vm.depth, vm.top(), vm.context_depth, vm.pc, vm.frame) == (2, I(9), 1, info["labels"]["after_c"], 1)


def test_host_level_engine_call_writes_the_literal_minus_one_seq():  # m1; natives.md §3.1 reserved C -1 / X -1
    vm, heap, tr, info = _vm(SWALLOW_ASM)
    log = []
    ctx = _ctx(vm, log)
    assert vm.native_seq() == -1
    r = ctx.engine_call("onevent", [I(3)], swallow=True)             # a D16 host route: no native executing
    assert not r.ok and len(vm.swallowed) == 1 and vm.last_error is None
    assert sum(1 for l in tr.lines if l.startswith("E ")) == 1        # D10's E line of the outermost call, once
    assert log == ["C -1 onevent 0010:00000003 " + " ".join(NOTHING10[1:]), "X -1"]
    rec = osi_vm.parse_log_record(log[0])
    assert rec["kind"] == "C" and rec["seq"] == -1 and rec["name"] == "onevent"
    assert osi_vm.parse_log_record("X -1") == {"kind": "X", "seq": -1, "raw": "X -1"}


def test_replay_H_with_a_type_other_than_the_minting_type_is_a_desync(tmp_path):   # m2
    m = _replay_mutated(tmp_path, lambda l: [x.replace("H 0037:00000003 +1", "H 0022:00000003 +1") for x in l])
    assert m == "replay desync at seq 5: H 0022:00000003 +1: handle 3 was minted as type 0037"


def test_replay_H_minus_one_below_the_engine_count_is_an_early_free_desync(tmp_path):   # m2
    def edit(l):
        i = l.index("H 0026:00000004 +1")
        return l[:i] + ["H 0026:00000004 -1"] + l[i:]                 # the sprite's engine count is 0 here
    m = _replay_mutated(tmp_path, edit)
    assert m == "replay desync at seq 8: H 0026:00000004 -1 would take engine count 0 below 0 (an early free)"


def test_replay_trailing_records_are_a_desync(tmp_path):              # m3: the whole log must be consumed
    log = []
    _run_worked(_worked_registry(True), log)
    p = tmp_path / "natives.log"
    p.write_text("\n".join(log + ["H 0037:00000003 +1", "00000010 gcsaver.getmask 0 -> 0010:00000000"]) + "\n")
    reg = _worked_registry(False)
    reg.replay_from(p)
    _run_worked(reg)
    with pytest.raises(osi_vm.ReplayDesync) as ei:
        reg.replay_finish()
    assert str(ei.value) == "replay desync at the end of the run: 2 records not consumed, first H 0037:00000003 +1"
    reg2 = _worked_registry(False)
    p.write_text("\n".join(log) + "\n")
    reg2.replay_from(p)
    _run_worked(reg2)
    reg2.replay_finish()                                              # exactly consumed: no error


def test_run_base_replay_with_trailing_records_fails(tmp_path):      # m3 through the CLI
    osi = tmp_path / "rerun.osi"
    osi.write_bytes(osiasm.assemble(RERUN_ASM.read_text())[0])
    py = [sys.executable, str(REPO / "tools" / "research" / "osi_vm.py"), "--run", str(osi), "--frames", "6",
          "--keys", "esc@1"]
    a = subprocess.run(py + ["--log-out", str(tmp_path / "a.log")], capture_output=True, text=True, timeout=110)
    assert a.returncode == 0, a.stderr
    log = tmp_path / "a.log"
    log.write_text(log.read_text() + "K      6\n")
    c = subprocess.run(py + ["--replay", str(log)], capture_output=True, text=True, timeout=110)
    assert c.returncode == 1
    assert "stop: replay desync: replay desync at the end of the run: 1 records not consumed, first K      6" in c.stdout


def test_calls_depth_reads_context_depth_after_a_swallow():         # m4 (controller ruling: conformance.md literal)
    """After the swallow, onevent continues with context_depth 2 (main + the swallowed call's leftover) while only
    one engine call is in flight. depth 2 therefore makes NO nested call: g stays 0."""
    asm = """.globals g
.function inner 0 @inner
.function onevent 0 @ev
.function main 0
    PushConstant0
    SetVariableValue global:g
    CallGameFunction t.sw 0
    Return
ev:
    DebugOn
    CallGameFunction t.re 0
    Return
inner:
    IncrementVariable global:g
    PushNothing
    Return
"""
    re_spec = {"cls": "t", "name": "re", "min": 0, "max": 0, "types": [], "returns": {"type": "0x10", "value": 7},
               "calls": {"entry": "inner", "args": [], "depth": 2}}
    seen = []
    vm, heap, tr, info = _spec_vm(asm, [SW_SPEC, re_spec])
    orig = vm.natives.invoke

    def spy(call):
        if call.fn_text == b"re":
            seen.append((call.vm.context_depth, call.vm._nest))
        return orig(call)
    vm.natives.invoke = spy
    r = vm.call("main", [])
    assert r.ok and r.result == I(7)
    assert seen == [(2, 1)]                                           # the two readings differ here
    assert vm.global_value("g") == I(0) and not any(l.startswith("C inner ") for l in tr.lines)
    assert vm.context_depth == 1


def test_release_of_a_handled_object_writes_H_minus_one_then_F_g():  # m5
    f = fw()
    vm, heap, tr, _ = _vm()
    log = []
    ctx = _ctx(vm, log)
    at_delete = []

    class Sprite(f.GameObject):
        def on_delete(self):
            at_delete.append((list(log), tr.lines[-1]))

    s = Sprite(0x26, 0)
    v = ctx.create_variant(s)
    ctx.add_ref(s)
    heap.dispose(v)
    ctx.release(s)
    assert log == ["H 0026:00000001 +1", "H 0026:00000001 -1"]
    assert tr.lines == ["A g 00000001 26", "F g 00000001"]
    assert at_delete == [(["H 0026:00000001 +1", "H 0026:00000001 -1"], "F g 00000001")]
