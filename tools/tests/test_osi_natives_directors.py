"""M2a-2 T8b: the directors / saver / Beta 1.5.0 mods native models of the Python oracle
(tools/research/osi_natives/directors.py), written from docs/osi/natives.md (§1.2 the ABI, §1.3 the citation format, §2 game
objects, §4/§5 the re-entrant chains and D16, §9 saver and mods) and from the asm of the canonical exe and of BetterSaver.dll,
read as data with objdump (never run). Never from the C++ engine.

For every native: an asm-pin test (objdump of the cited range, ranged so that the decode starts at the function and is not a
linear sweep; data constants by address from the PE image) and a behaviour test (the native called from a script through the
0x82 handler, so the argument conversion, the S/N lines and the natives.log are the real ones; the re-entrant chains with an
injected area source, since the oracle host has no area data).

Pins need LOMN_DATA (they skip with `SKIPPED (no LOMN_DATA)`); the behaviour tests of the pure models do not.
"""
import configparser
import json
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "osi"))
sys.path.insert(0, str(REPO / "tools" / "research"))

import osiasm  # noqa: E402
import osi_vm  # noqa: E402
from osi_vm import NOTHING, Variant, VmError  # noqa: E402

TYPES = osi_vm.TypeTable.load(REPO / "data" / "osi" / "types.json")
OVERRIDES = REPO / "data" / "osi" / "native_overrides.json"
M32 = 0xFFFFFFFF
NULL_ID = 0x3F3F3F3F


def dr():
    from osi_natives import directors
    return directors


def fw():
    from osi_natives import framework
    return framework


# ----------------------------------------------------------------------------------------------- harness
def make_vm(asm, install=None, deterministic=True, with_log=True, skip_video=True):
    """The oracle VM with the real native registry and only this group bound."""
    f, d = fw(), dr()
    image, _ = osiasm.assemble(asm)
    tr = osi_vm.ListTrace()
    heap = osi_vm.Heap(tr)
    reg = osi_vm.NativeRegistry.load(osi_vm.DEFAULT_NATIVES, osi_vm.DEFAULT_MODS, TYPES)
    vm = osi_vm.Vm(image, reg, heap, tr)
    log = [] if with_log else None
    vfs = f.Vfs(install) if install is not None else None
    ctx = f.HostContext(vm, heap, vfs, f.HostFlags(deterministic=deterministic, skip_video=skip_video), log)
    d.bind(reg, ctx)
    return vm, ctx, tr, log


def run_main(asm, **kw):
    vm, ctx, tr, log = make_vm(asm, **kw)
    r = vm.call("main", [])
    assert r.ok, vm.last_error and vm.last_error.message
    return r.result, vm, ctx, tr, log


def call0(native, **kw):
    return run_main(".function main 0\nCallGameFunction %s 0\nReturn\n" % native, **kw)


def call_i(native, *ints, **kw):
    body = "".join("PushConstanti32 %d\n" % (i if i < 0x80000000 else i - (1 << 32)) for i in ints)
    return run_main(".function main 0\n%sCallGameFunction %s %d\nReturn\n" % (body, native, len(ints)), **kw)


def call_s(native, text, **kw):
    return run_main('.function main 0\nPushConstantString "%s"\nCallGameFunction %s 1\nReturn\n' % (text, native), **kw)


def logs(log, native):
    return [l for l in log if re.match(r"^\d+ %s " % re.escape(native), l)]


# ----------------------------------------------------------------------------------------------- asm pins
@pytest.fixture(scope="session")
def image(canonical_exe):
    import exc
    return exc.Image.from_file(canonical_exe)


@pytest.fixture(scope="session")
def dll_image(lomn_data):
    import exc
    p = lomn_data / "install" / "mods" / "BetterSaver.dll"
    if not p.exists():
        pytest.skip("SKIPPED (no BetterSaver.dll)")
    return exc.Image.from_file(p)


@pytest.fixture(scope="session")
def dll_path(lomn_data):
    p = lomn_data / "install" / "mods" / "BetterSaver.dll"
    if not p.exists():
        pytest.skip("SKIPPED (no BetterSaver.dll)")
    return p


@pytest.fixture(scope="session")
def disasm(canonical_exe):
    if shutil.which("objdump") is None:
        pytest.skip("SKIPPED (no objdump)")
    cache = {}

    def run(start, stop, exe=canonical_exe):
        key = (str(exe), start, stop)
        if key not in cache:
            out = subprocess.run(["objdump", "-d", "-M", "intel", "--start-address=%#x" % start,
                                  "--stop-address=%#x" % stop, str(exe)], capture_output=True, text=True,
                                 check=True, timeout=60).stdout
            rows = {}
            for ln in out.splitlines():
                m = re.match(r"^\s*([0-9a-f]+):\s+[0-9a-f ]+\t(.*)$", ln)
                if m:
                    rows[int(m.group(1), 16)] = re.sub(r"\s+", " ", m.group(2).strip())
            cache[key] = rows
        return cache[key]
    return run


def pin(disasm, start, stop, expect, exe=None):
    """expect = {addr: normalised 'mnemonic operands'}: every one must be the decode of the instruction at that address."""
    rows = disasm(start, stop) if exe is None else disasm(start, stop, exe)
    bad = {}
    for a, text in expect.items():
        got = rows.get(a)
        if got is not None:
            got = re.sub(r" <[^>]*>$", "", got)
        if got != text:
            bad[a] = (text, got)
    assert not bad, "asm pin mismatch (address: (expected, decoded)): " + ", ".join(
        "%#x: %r vs %r" % (a, e, g) for a, (e, g) in sorted(bad.items()))


# ===============================================================================================================
# 0. registration, citations and the overrides rows
# ===============================================================================================================
TIER1_KEYS = sorted(["gcgame.getmainview", "gcgame.getkeymapper", "gcgame.getmp3player", "gcviewport.getcamera",
                     "gcareadirector.getcurrentlevel", "gcareadirector.setcurrentlevel", "gcareadirector.setcurrentarea",
                     "gcareadirector.getscriptfluffstring", "gcsaver.cheatmenu", "gcsaver.getammo",
                     "bettersaver.getbooleanvalue", "bettersaver.setbooleanvalue",
                     "bettersaver.save", "gchuddirector.playcinematic"])
TIER2_KEYS = ["gckeymapper.setusecontext", "gcmp3player.play", "gcmp3player.setupforplay", "gcmp3player.stop",
              "gcsaver.setenergy", "gcsaver.sethealth", "gcsaver.setmask"]
MOD_KEYS = ["bettersaver.getbooleanvalue", "bettersaver.save", "bettersaver.setbooleanvalue"]


def test_group_registers_itself_and_binds_exactly_the_tier1_rows():
    d, f = dr(), fw()
    assert "directors" in f._GROUPS
    reg = osi_vm.NativeRegistry.load(osi_vm.DEFAULT_NATIVES, osi_vm.DEFAULT_MODS, TYPES)
    vm = osi_vm.Vm(osiasm.assemble(".function main 0\nReturn\n")[0], reg, osi_vm.Heap(None), None)
    ctx = f.HostContext(vm, vm.heap, None)
    d.bind(reg, ctx)
    implemented = sorted(e.key for e in reg.entries.values() if e.tier == "implemented")
    assert implemented == TIER1_KEYS


def test_every_bound_key_has_a_citation_in_the_natives_md_1_3_form():
    d = dr()
    assert sorted(d.CITES) == TIER1_KEYS
    for key, cite in d.CITES.items():
        assert cite.isascii(), key                                  # the overrides are pure ASCII
        if key in MOD_KEYS:
            assert re.match(r"^// body BetterSaver\.dll 0x[0-9a-f]+, method 0x[0-9a-f]+ <[^>]+>"
                            r"( -> 0x[0-9a-f]+ <[^>]+>)*; asm pin: \S", cite), key
        else:
            assert re.match(r"^// body 0x[0-9a-f]+, method 0x[0-9a-f]+ <[^>]+>( -> 0x[0-9a-f]+ <[^>]+>)*; asm pin: \S",
                            cite), key
        assert not re.search(r"0x0[0-9a-f]|0X|0x[0-9a-f]*[A-F]", cite), key   # shortest lowercase hex (natives.md §1.3)


def _addrs(text):
    """Code addresses (5+ hex digits) in a cite, as numbers: 0x18 or 0x68 are shifts and offsets, not addresses."""
    return {int(m, 16) for m in re.findall(r"0x([0-9a-fA-F]{5,})", text)}


def test_committed_overrides_rows_are_valid_sorted_ascii_and_agree_with_the_module():
    """The merged overrides file carries both sides' rows (wording may differ). Checked per key: `tier` exactly, the absence
    of `default` on tier1, and that the body address the registry names is in the cite."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("osi_registry_t8b", REPO / "tools" / "osi" / "registry.py")
    reg = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(reg)
    rows = json.loads((REPO / "data" / "osi" / "natives.json").read_text())
    mods = json.loads((REPO / "data" / "osi" / "natives_beta150.json").read_text())
    assert OVERRIDES.read_bytes().isascii()
    data = json.loads(OVERRIDES.read_text())
    assert reg.check_overrides(data, rows, mods) == []
    by_key = {r["key"]: r for r in data}
    keys = [r["key"] for r in data]
    assert keys == sorted(keys) and len(set(keys)) == len(keys)
    bodies = {r["key"]: int(r["body"], 16) for r in rows + mods if r.get("winner", True)}
    for k in TIER1_KEYS:
        assert by_key[k]["tier"] == "tier1" and "default" not in by_key[k], k
        assert bodies[k] in _addrs(by_key[k]["cite"]), k
        assert _addrs(dr().CITES[k]) <= _addrs(by_key[k]["cite"]) | {0}, (k, sorted(hex(a) for a in
                                                                          _addrs(dr().CITES[k]) - _addrs(by_key[k]["cite"])))
    for k in TIER2_KEYS:
        assert by_key[k]["tier"] == "tier2" and "default" not in by_key[k], k
        assert bodies[k] in _addrs(by_key[k]["cite"]) and dr().TIER2[k] == bodies[k], k
    for k in ("gcsaver.cheatmenu", "gcareadirector.getscriptfluffstring"):
        assert by_key[k]["note"]
    assert "ini -> M2a-3" in by_key["gcsaver.cheatmenu"]["note"]


def test_the_merged_tables_carry_the_tiers():
    rows = {r["key"]: r for r in json.loads((REPO / "data" / "osi" / "natives.json").read_text()) if r["winner"]}
    mods = {r["key"]: r for r in json.loads((REPO / "data" / "osi" / "natives_beta150.json").read_text()) if r["winner"]}
    for k in TIER1_KEYS:
        assert (mods if k in MOD_KEYS else rows)[k]["tier"] == "tier1", k
    for k in TIER2_KEYS:
        assert rows[k]["tier"] == "tier2", k


# ===============================================================================================================
# 1. gcgame.getmainview / getkeymapper / getmp3player   (singletons: identity, G 1, no H)
# ===============================================================================================================
def test_singleton_getters_asm_pins(disasm, image):
    # GetMainView 0x43ae20: the main view id [0x8391b0] looked up in the screen's view list by ScScreen::GetViewPort
    pin(disasm, 0x43ae20, 0x43ae42, {0x43ae26: "mov edx, dword ptr [0x8391b0]", 0x43ae32: "mov ecx, dword ptr [0x70fa64]",
                                     0x43ae39: "call 0x49d3c0"})
    pin(disasm, 0x49d3c0, 0x49d41f, {0x49d3cd: "mov edx, dword ptr [ebx + 0x2a404]", 0x49d3f0: "mov edx, dword ptr [ecx]",
                                     0x49d3f2: "cmp ebx, edx"})
    pin(disasm, 0x4366e0, 0x4366fb, {0x4366e3: "call 0x43ae20", 0x4366ed: "call 0x436700"})
    pin(disasm, 0x4367f0, 0x43680b, {0x4367f3: "call 0x43ae90", 0x4367fd: "call 0x436810"})
    pin(disasm, 0x43ae90, 0x43ae96, {0x43ae90: "mov eax, dword ptr [0x83aa98]"})                 # GetKeyMapper
    pin(disasm, 0x436940, 0x43695b, {0x436943: "call 0x43aea0", 0x43694d: "call 0x436960"})
    pin(disasm, 0x43aea0, 0x43aea6, {0x43aea0: "mov eax, dword ptr [0x83879c]"})                 # GetMP3Player
    # the three CreateVariant overloads: null -> {0x0f, 0}, else the AddRef `inc dword [edi+4]` (natives.md §2.1)
    pin(disasm, 0x436700, 0x436740, {0x436713: "mov word ptr [esi], 0xf", 0x436730: "inc dword ptr [edi + 0x4]"})
    pin(disasm, 0x436810, 0x436850, {0x436823: "mov word ptr [esi], 0xf", 0x436840: "inc dword ptr [edi + 0x4]"})
    pin(disasm, 0x436960, 0x4369a0, {0x436973: "mov word ptr [esi], 0xf", 0x436990: "inc dword ptr [edi + 0x4]"})
    # the holders natives.md §2.4 cites: SetUpControllers +1 at 0x439583 and SetUpDatabase +1 at 0x43a40c are inc [reg+4]
    pin(disasm, 0x439581, 0x4395a9, {0x439583: "inc dword ptr [edi + 0x4]", 0x4395a3: "mov dword ptr [0x83aa98], edi"})
    pin(disasm, 0x43a40a, 0x43a432, {0x43a40c: "inc dword ptr [edi + 0x4]"})


def test_singleton_getters_return_one_handle_with_one_engine_reference():
    for native, tname, tid in (("gcgame.getmainview", "GcViewPort", 0x28), ("gcgame.getkeymapper", "GcKeyMapper", 0x3E),
                               ("gcgame.getmp3player", "GcMP3Player", 0x3F)):
        assert TYPES.id(tname) == tid
        # n-game-identity with the real native: the same live object twice is ONE handle
        r, vm, ctx, tr, log = run_main(".function main 0\nCallGameFunction %s 0\nCallGameFunction %s 0\nEqualTo\nReturn\n"
                                       % (native, native))
        assert r == Variant(0x10, 1), native
        assert tr.lines.count("A g 00000001 %02x" % tid) == 1 and "A g 00000002 %02x" % tid not in tr.lines
        assert log[0] == "00000000 %s 0 -> %04x:00000001 G 1" % (native, tid)         # G 1: the engine's reference
        assert log[1] == "00000001 %s 0 -> %04x:00000001 G 1" % (native, tid)
        assert not any(l.startswith(("H ", "C ", "X ")) for l in log)                  # no engine reference change inside
        (h, obj), = ctx._objects.items()
        assert vm.heap.engine(h) == 1 and obj.type() == tid


def test_singleton_survives_dropping_every_script_reference():
    r, vm, ctx, tr, log = run_main(".function main 0\nCallGameFunction gcgame.getkeymapper 0\nPop\n"
                                   "CallGameFunction gcgame.getkeymapper 0\nPop\nPushConstanti8 1\nReturn\n")
    assert "F g 00000001" not in tr.lines and len(ctx._objects) == 1                    # the engine reference keeps it
    assert tr.lines.count("A g 00000001 3e") == 1
    h, = ctx._objects
    assert vm.heap.engine(h) == 1


def test_the_three_singletons_are_distinct_objects_and_stable_across_frames():
    asm = (".function main 0\nCallGameFunction gcgame.getmainview 0\nCallGameFunction gcgame.getkeymapper 0\n"
           "CallGameFunction gcgame.getmp3player 0\nPop\nPop\nPop\nPushConstanti8 1\nReturn\n")
    vm, ctx, tr, log = make_vm(asm)
    for _ in range(3):
        assert vm.call("main", []).ok
    handles = [l.split("-> ")[1].split()[0] for l in log]
    assert handles[:3] == ["0028:00000001", "003e:00000002", "003f:00000003"] == handles[3:6] == handles[6:9]
    assert sorted(o.type() for o in ctx._objects.values()) == [0x28, 0x3E, 0x3F]


def test_getmp3player_is_not_null_and_not_nothing():
    r, vm, ctx, tr, log = run_main(".function main 0\nCallGameFunction gcgame.getmp3player 0\nPushNothing\nEqualTo\nReturn\n")
    assert r == Variant(0x10, 0)                                  # != nothing: the CHANGED branch of the A9 path


# ===============================================================================================================
# 2. gcviewport.getcamera
# ===============================================================================================================
def test_getcamera_asm_pins(disasm):
    pin(disasm, 0x4474e0, 0x447500, {0x4474e3: "lea edx, [ebp + 0x10]", 0x4474e6: "mov ecx, dword ptr [edx]",
                                     0x4474e8: "call 0x448080", 0x4474f2: "call 0x447500"})
    # GetCamera: the node [this+0x30] gets a temporary reference (+1 0x4480aa, -1 0x4480d6); the camera is [node+0x1c]
    pin(disasm, 0x448080, 0x4480f0, {0x44809e: "mov edi, dword ptr [ebx + 0x30]", 0x4480aa: "inc dword ptr [edi + 0x4]",
                                     0x4480ca: "mov edi, dword ptr [edx + 0x1c]", 0x4480d6: "dec dword ptr [ecx + 0x4]"})
    pin(disasm, 0x447500, 0x447540, {0x447513: "mov word ptr [esi], 0xf", 0x447530: "inc dword ptr [edi + 0x4]"})


def test_getcamera_of_the_main_view_is_the_singleton_camera():
    asm = (".function main 0\nCallGameFunction gcgame.getmainview 0\nCallGameFunction gcviewport.getcamera 1\n"
           "CallGameFunction gcgame.getmainview 0\nCallGameFunction gcviewport.getcamera 1\nEqualTo\nReturn\n")
    r, vm, ctx, tr, log = run_main(asm)
    assert r == Variant(0x10, 1)
    assert logs(log, "gcviewport.getcamera")[0] == "00000001 gcviewport.getcamera 1 -> 002a:00000002 G 1"
    assert TYPES.id("GcLegoCamera") == 0x2A and tr.lines.count("A g 00000002 2a") == 1
    assert not any(l.startswith(("H ", "C ", "X ")) for l in log)       # the node's temporary reference is not script-visible


def test_getcamera_of_another_view_is_another_camera_and_stays_alive():
    asm = ".function main 0\nReturn\n.function g 1\nGetVariableValue 0\nCallGameFunction gcviewport.getcamera 1\nReturn\n"
    vm, ctx, tr, log = make_vm(asm)
    d = dr()
    tid = TYPES.id("GcViewPort")
    vps = [ctx.create_variant(d.GcViewPort(tid, 1), tid) for _ in range(2)]
    handles = []
    for v in vps + vps:
        assert vm.call("g", [v]).ok
        handles.append(logs(log, "gcviewport.getcamera")[-1].split("-> ")[1].split()[0])
    assert handles[0] != handles[1] and handles[:2] == handles[2:]       # one camera per view, stable
    assert all(h.startswith("002a:") for h in handles)


# ===============================================================================================================
# 3. gcareadirector.getcurrentlevel / setcurrentlevel
# ===============================================================================================================
def test_level_asm_pins(disasm, image):
    pin(disasm, 0x486c40, 0x486c6a, {0x486c46: "mov edx, dword ptr [0x83902c]", 0x486c5c: "call 0x407df0"})
    pin(disasm, 0x407df0, 0x407e08, {0x407df9: "mov edx, dword ptr [edx]", 0x407dfb: "mov word ptr [ebx], 0x10"})
    pin(disasm, 0x486c10, 0x486c35, {0x486c1b: "call 0x489a60", 0x486c21: "mov word ptr [ebx], 0xf"})
    pin(disasm, 0x489a60, 0x489b00, {
        0x489a96: "call 0x617810", 0x489ab4: "mov edx, dword ptr [0x83902c]", 0x489aba: "cmp dword ptr [0x70b3bc], edx",
        0x489ac0: "je 0x489ae3", 0x489ac2: "mov edx, dword ptr [0x83b0b4]", 0x489ac8: "cmp dword ptr [0x70b3c0], edx",
        0x489ace: "je 0x489ae3", 0x489add: "call 0x489560", 0x489af6: "mov dword ptr [0x83902c], edx"})
    # the level number goes to the saver: name[3] - '0' (a byte) through GcSaver::SetLevel
    pin(disasm, 0x48a44f, 0x48a470, {0x48a455: "movzx eax, byte ptr [eax + 0x3]", 0x48a459: "add eax, 0xffffffd0",
                                     0x48a45f: "call 0x5d5740"})
    pin(disasm, 0x5d5740, 0x5d574b, {0x5d5740: "mov dl, byte ptr [esp + 0x4]", 0x5d5744: "mov byte ptr [0x74748c], dl"})
    # RemoveArea: Get, CleanUpArea, erase, the area becomes the null id
    pin(disasm, 0x489560, 0x489600, {0x489574: "call 0x48a810", 0x489589: "call 0x5017d0", 0x48959c: "call 0x650680",
                                     0x4895a1: "mov dword ptr [0x83b0b4], 0x3f3f3f3f"})
    # the null id and the initial level/area: .data and the static initialisers
    assert image.u32(0x70b3bc) == NULL_ID and image.u32(0x70b3c0) == NULL_ID
    pin(disasm, 0x48db4b, 0x48db6a, {0x48db4b: "mov dword ptr [0x83902c], 0x3f3f3f3f",
                                     0x48db5f: "mov dword ptr [0x83b0b4], 0x3f3f3f3f"})
    # SetCurrentLevel catches (and ignores) its SLK-load errors: the flags are written only with 0 in the whole function
    # (the locals [ebp-0x364] / [ebp-0x368] of the material block), so nothing is rethrown or retried
    pin(disasm, 0x489b9b, 0x489ba2, {0x489b9b: "mov byte ptr [ebp - 0x364], 0x0"})
    pin(disasm, 0x489bc5, 0x489bcc, {0x489bc5: "mov byte ptr [ebp - 0x368], 0x0"})


def _writers(disasm, start, stop, frame_off):
    """Every instruction in [start, stop) that mentions the frame local [ebp - frame_off]."""
    rows = disasm(start, stop)
    return {a: t for a, t in rows.items() if re.search(r"\[ebp - %#x\]" % frame_off, t)}


def test_setcurrentlevel_never_rethrows_or_retries(disasm):
    """The rethrow flag [ebp-0x364] and the retry flag [ebp-0x368] of the material block are only ever tested and zeroed in
    0x489a60-0x48a48e (a catch pad would have to store 1 to enable them)."""
    for off in (0x364, 0x368, 0x35c, 0x360, 0x354, 0x358):          # the six flags of the three blocks
        stores = {a: t for a, t in _writers(disasm, 0x489a60, 0x48a48e, off).items() if t.startswith("mov")}
        assert stores and all(t.endswith(", 0x0") for t in stores.values()), (hex(off), stores)


def test_getcurrentlevel_starts_at_the_null_id_and_follows_setcurrentlevel():
    r, vm, ctx, tr, log = call0("gcareadirector.getcurrentlevel")
    assert r == Variant(0x10, NULL_ID) and log[0] == "00000000 gcareadirector.getcurrentlevel 0 -> 0010:3f3f3f3f"
    r, vm, ctx, tr, log = run_main('.function main 0\nPushConstantString "lev0"\nCallGameFunction '
                                   'gcareadirector.setcurrentlevel 1\nPop\nCallGameFunction '
                                   'gcareadirector.getcurrentlevel 0\nReturn\n')
    assert r == Variant(0x10, 0x6C657630)                           # 'lev0' as StringToIdentifier builds it
    assert log[0] == "00000000 gcareadirector.setcurrentlevel 1 -> 000f:00000000"
    assert log[1] == "00000001 gcareadirector.getcurrentlevel 0 -> 0010:6c657630"


def test_setcurrentlevel_round_trip_for_every_name_shape():
    cases = {"lev0": 0x6C657630, "lev7": 0x6C657637, "ABCD": 0x41424344, "lev": M32, "lev00": M32, "": M32, "x": M32}
    for name, want in cases.items():
        r, *_ = run_main('.function main 0\nPushConstantString "%s"\nCallGameFunction gcareadirector.setcurrentlevel 1\n'
                         'Pop\nCallGameFunction gcareadirector.getcurrentlevel 0\nReturn\n' % name)
        assert r == Variant(0x10, want), name


def test_setcurrentlevel_writes_the_saver_level_byte_from_character_3():
    d = dr()
    for name, want in (("lev0", 0), ("lev3", 3), ("lev7", 7), ("abc", 0xD0), ("lev\xff", 0xCF)):
        vm, ctx, tr, log = make_vm('.function main 0\nPushConstantString "%s"\nCallGameFunction '
                                   'gcareadirector.setcurrentlevel 1\nReturn\n' % name)
        assert vm.call("main", []).ok
        assert ctx.subsystem(d.Saver).level == want, name


def test_setcurrentlevel_makes_no_engine_call_when_the_area_is_null():
    # the A9 path: level and area both null (natives.md §4: no C record)
    r, vm, ctx, tr, log = run_main('.function main 0\nPushConstantString "lev0"\nCallGameFunction '
                                   'gcareadirector.setcurrentlevel 1\nReturn\n')
    assert not any(l.startswith(("C ", "X ")) for l in log)


# ===============================================================================================================
# 4. gcareadirector.setcurrentarea  and the re-entrant chains
# ===============================================================================================================
def test_setcurrentarea_asm_pins(disasm, image):
    pin(disasm, 0x486ef0, 0x486f15, {0x486efb: "call 0x48a540", 0x486f01: "mov word ptr [ebx], 0xf"})
    pin(disasm, 0x48a540, 0x48a5a0, {0x48a55d: "call 0x617810", 0x48a575: "cmp eax, 0x83b0b4",
                                     0x48a57f: "mov dword ptr [0x83b0b4], edx", 0x48a588: "call 0x4892f0",
                                     0x48a593: "call 0x5d5500"})
    pin(disasm, 0x5d5500, 0x5d5520, {0x5d5506: "mov dword ptr [0x747490], edx", 0x5d550c: "call 0x5d45d0"})
    pin(disasm, 0x4892f0, 0x489458, {0x4892fe: "mov byte ptr [0x83b40f], 0x1", 0x48935a: "call 0x4fedd0",
                                     0x48938c: "call 0x48a7e0"})
    # the three LoadArea blocks that reach the VM (natives.md §5.2)
    pin(disasm, 0x4ff930, 0x4ff940, {0x4ff93e: "call 0x53fc60"})
    pin(disasm, 0x5002e0, 0x5002f0, {0x5002ec: "call 0x53f3f0"})
    pin(disasm, 0x500c00, 0x500c0d, {0x500c0b: "call 0x6261c0"})
    # ResetData leaves the saver's area at the null id and the level byte at 0 (so the Setup defaults)
    pin(disasm, 0x5d5490, 0x5d5500, {0x5d5493: "mov byte ptr [0x74748c], 0x0",
                                     0x5d549a: "mov dword ptr [0x747490], 0x3f3f3f3f",
                                     0x5d54ec: "mov dword ptr [0x8391a0], 0x0"})


def test_setcurrentarea_pins_the_swallow_chain(disasm):
    # the SetupMoveable guard: MainCharacterID() == the motion id -> SetUnderWater(byte [0x73c5a8])
    pin(disasm, 0x574ee2, 0x574f00, {0x574ee5: "call 0x43af90", 0x574eeb: "mov ecx, dword ptr [esi + 0x14]",
                                     0x574ef1: "cmp ecx, edx", 0x574ef3: "jne 0x574f02",
                                     0x574ef5: "mov al, byte ptr [0x73c5a8]", 0x574efd: "call 0x589050"})
    pin(disasm, 0x43af90, 0x43b000, {0x43af91: "cmp dword ptr [0x7032b8], 0x0", 0x43af9e: "mov edx, dword ptr [0x7796f4]",
                                     0x43afb8: "cmp dword ptr [edx + 0x1d0], 0x0"})
    # SetUnderWater: both branches call OnEvent(0x36, b), b = 1 (push 1 0x58908b) or 0 (push 0 0x5891a1)
    pin(disasm, 0x589050, 0x589160, {0x58905c: "cmp byte ptr [ebp + 0x8], 0x0", 0x58907b: "push 0x36",
                                     0x58908b: "push 0x1", 0x589094: "call 0x407f30", 0x589154: "call 0x60b320"})
    pin(disasm, 0x589178, 0x589250, {0x589194: "push 0x36", 0x5891a1: "push 0x0", 0x5891a7: "call 0x407f30",
                                     0x589249: "call 0x60b320"})


def test_setcurrentarea_pins_the_removal_chain(disasm, dll_path, image):
    # CleanUpArea dispatches by the entry's director; the model director's Remove is the only call that reaches the VM
    pin(disasm, 0x5017d0, 0x501900, {0x5017e3: "mov ecx, dword ptr [esi + 0x4]", 0x501813: "mov edx, dword ptr [0x83aa9c]",
                                     0x501819: "cmp ecx, edx", 0x501830: "call 0x47f320"})
    pin(disasm, 0x501dc0, 0x501e70, {0x501de4: "cmp ebx, edx", 0x501e23: "mov edx, dword ptr [0x83aa9c]",
                                     0x501e43: "call 0x47f320"})
    # GcModelDirector::Remove: not active -> return; Get(id) null -> return; the erase; the Toa id is exempt; OnEvent(0x2b, id)
    pin(disasm, 0x47f320, 0x47f340, {0x47f32c: "cmp dword ptr [0x83aa4c], 0x0", 0x47f333: "jne 0x47f340"})
    pin(disasm, 0x47f340, 0x47f380, {0x47f365: "call 0x480f70", 0x47f36b: "test eax, eax"})
    pin(disasm, 0x47f4f3, 0x47f550, {0x47f50c: "mov ecx, 0x782a04", 0x47f511: "call 0x4de2e0"})
    pin(disasm, 0x47f5ae, 0x47f690, {0x47f5b3: "mov edx, dword ptr [0x782884]", 0x47f5b9: "cmp ecx, edx", 0x47f5bb: "je 0x47f685",
                                     0x47f5c1: "push 0x2b", 0x47f5d9: "push eax", 0x47f5de: "call 0x407df0",
                                     0x47f67b: "push 0x70a8b6", 0x47f680: "call 0x60b320"})
    assert image.read(0x70a8b6, 8) == b"OnEvent\0"
    pin(disasm, 0x482d43, 0x482d4e, {0x482d43: "mov dword ptr [0x782884], 0x636e7431"})        # 'cnt1'
    # BetterSaver's CleanupLoad (0x10001ae0): Get(&[0x747490]); BetterSaver's vector twice per match; the exe lists once
    pin(disasm, 0x10001ae0, 0x10001be0, {
        0x10001ae1: "push dword ptr [0x1000a078]", 0x10001ae7: "mov eax, 0x48a810", 0x10001af5: "je 0x10001bd9",
        0x10001b25: "mov edx, dword ptr [ecx + 8*edi + 0x4]", 0x10001b29: "cmp edx, dword ptr [eax]",
        0x10001b2d: "mov esi, 0x2", 0x10001b38: "mov eax, 0x501dc0", 0x10001b3d: "call eax", 0x10001b48: "jne 0x10001b32",
        0x10001b5c: "mov edx, dword ptr [0x1000a054]", 0x10001b69: "mov esi, 0x8384a0", 0x10001b7f: "mov eax, 0x501dc0",
        0x10001ba4: "mov esi, 0x7f79f0", 0x10001bbf: "mov eax, 0x501dc0"}, exe=dll_path)


def test_bettersaver_pointer_table_names_the_exe_globals(dll_image):
    u = lambda va: struct.unpack("<I", dll_image.read(va, 4))[0]
    assert u(0x1000a078) == 0x747490 and u(0x1000a07c) == 0x74748c          # the saver's area id and level byte
    assert u(0x1000a040) == 0x8391A0                                         # the condition bits
    assert u(0x1000a054) == 0x83B401 and u(0x1000a050) == 0x83B3FA           # the two list counts
    assert u(0x1000a034) == 0x83B380                                         # the quest bytes (SetQuest: 0x83b37f + level)


def _area_asm(n_calls=1, level_first=False):
    return ('.function main 0\nPushConstantString "frnt"\nCallGameFunction gcareadirector.setcurrentarea 1\nPop\n'
            'PushConstanti8 1\nReturn\n.function OnEvent 2\nPushConstanti8 1\nReturn\n')


def test_setcurrentarea_default_a9_path_state_and_no_engine_call():
    d = dr()
    vm, ctx, tr, log = make_vm(_area_asm())
    assert vm.call("main", []).ok
    ad, sv = ctx.subsystem(d.AreaDirector), ctx.subsystem(d.Saver)
    assert ad.area == 0x66726E74 and ad.level == NULL_ID                      # 'frnt'
    assert sv.area == 0x66726E74 and set(ad.loaders) == {0x66726E74} and ad.loading is True
    assert logs(log, "gcareadirector.setcurrentarea")[0] == "00000000 gcareadirector.setcurrentarea 1 -> 000f:00000000"
    assert not any(l.startswith(("C ", "X ", "H ")) for l in log)             # natives.md §4: none on the path


def test_setcurrentarea_hostile_names_follow_stringtoidentifier():
    d = dr()
    for name, want in (("lev0", 0x6C657630), ("toolong", M32), ("", M32)):
        vm, ctx, tr, log = make_vm('.function main 0\nPushConstantString "%s"\nCallGameFunction '
                                   'gcareadirector.setcurrentarea 1\nReturn\n' % name)
        assert vm.call("main", []).ok
        assert ctx.subsystem(d.AreaDirector).area == want and ctx.subsystem(d.Saver).area == want


def test_setcurrentarea_twice_keeps_the_first_loader():
    d = dr()
    asm = ('.function main 0\nPushConstantString "frnt"\nCallGameFunction gcareadirector.setcurrentarea 1\nPop\n'
           'PushConstantString "frnt"\nCallGameFunction gcareadirector.setcurrentarea 1\nReturn\n')
    vm, ctx, tr, log = make_vm(asm)
    ad = ctx.subsystem(d.AreaDirector)
    first = []
    ad.area_source = lambda aid: first.append(aid) or []
    assert vm.call("main", []).ok
    assert len(first) == 2 and len(ad.loaders) == 1                           # Add refuses the duplicate id (0x48938c)


def _onevent_logger():
    # OnEvent(a0, a1): returns 1; the script side of every chain below
    return '.function OnEvent 2\nPushConstanti8 1\nReturn\n'


def _model_world(vm, ctx, ids, toa=False):
    d = dr()
    mo = ctx.subsystem(d.ModelObjects)
    for i in ids:
        mo.objects[i] = object()
    if toa:
        mo.objects[d.ModelObjects.TOA_ID] = object()
    return mo


def test_setcurrentlevel_with_area_and_level_set_removes_the_area_through_the_model_director():
    """The re-entrant chain 0x489add -> 0x489560 -> 0x5017d0 -> 0x47f320 -> OnEvent(0x2b, id): synchronous, in entry order,
    one natives.log C record per call at the cited point, the Toa's own id exempt (0x47f5b3)."""
    d = dr()
    asm = ('.function main 0\nPushConstantString "lev0"\nCallGameFunction gcareadirector.setcurrentlevel 1\nPop\n'
           'PushConstantString "frnt"\nCallGameFunction gcareadirector.setcurrentarea 1\nPop\n'
           'PushConstantString "lev1"\nCallGameFunction gcareadirector.setcurrentlevel 1\nPop\n'
           'CallGameFunction gcareadirector.getcurrentlevel 0\nReturn\n' + _onevent_logger())
    vm, ctx, tr, log = make_vm(asm)
    ids = [0x6F626A31, 0x6F626A32, d.ModelObjects.TOA_ID, 0x6F626A33]
    ad = ctx.subsystem(d.AreaDirector)
    ad.area_source = lambda aid: [d.AreaObject("objects", ids[0], 1), d.AreaObject("characters", ids[1], 1),
                                  d.AreaObject("objects", ids[2], 1), d.AreaObject("hives", ids[3], 1, director="area")]
    mo = _model_world(vm, ctx, ids[:3])
    r = vm.call("main", [])
    assert r.ok and r.result == Variant(0x10, 0x6C657631)
    cs = [l for l in log if l.startswith("C ")]
    # the second setcurrentlevel is native seq 2: object 'obj1' (objects block first), then the characters block's 'obj2'
    assert len(cs) == 2 and all(l.startswith("C 00000002 OnEvent 0010:0000002b ") for l in cs)
    assert cs[0].split()[4] == "0010:6f626a31" and cs[1].split()[4] == "0010:6f626a32"
    assert all(" ".join(l.split()[5:]) == " ".join(["000f:00000000"] * 8) for l in cs)    # a2..a9 nothing
    assert ad.area == NULL_ID and ad.loaders == {} and ad.level == 0x6C657631             # 0x4895a1, the map erase, 0x489af6
    assert mo.objects == {}                                                              # all three erased, the Toa without an event


def test_removal_guards_each_branch():
    d = dr()
    asm = ('.function main 0\nPushConstantString "lev0"\nCallGameFunction gcareadirector.setcurrentlevel 1\nPop\n'
           'PushConstantString "lev1"\nCallGameFunction gcareadirector.setcurrentlevel 1\nReturn\n' + _onevent_logger())

    def run(level_set=True, area_set=True, active=True, in_map=True):
        vm, ctx, tr, log = make_vm(asm if level_set else asm.replace('PushConstantString "lev0"\nCallGameFunction '
                                                                     'gcareadirector.setcurrentlevel 1\nPop\n', "", 1))
        ad = ctx.subsystem(d.AreaDirector)
        if area_set:
            ad.area = 0x66726E74
            ad.loaders[0x66726E74] = d.AreaLoader()
            ad.loaders[0x66726E74].entries.append(d.AreaInfo(0x6F626A31, "model"))
        mo = ctx.subsystem(d.ModelObjects)
        mo.active = active
        if in_map:
            mo.objects[0x6F626A31] = object()
        if not level_set:
            pass
        assert vm.call("main", []).ok
        return [l for l in log if l.startswith("C ")], ad

    cs, ad = run()
    assert len(cs) == 1 and ad.area == NULL_ID                                  # both set: removed
    cs, ad = run(level_set=False)                                               # [0x83902c] is null at the first call...
    assert cs == [] or len(cs) == 1                                              # ...but the second call sees lev1 as current
    cs, ad = run(area_set=False)
    assert cs == []                                                              # area null: 0x489ace skips
    cs, ad = run(active=False)
    assert cs == [] and ad.area == NULL_ID                                       # [0x83aa4c] == 0: Remove returns, the area is still removed
    cs, ad = run(in_map=False)
    assert cs == []                                                              # Get(id) null: Remove returns


def test_removearea_without_a_loader_is_undefined_behaviour():
    d = dr()
    asm = ('.function main 0\nPushConstantString "lev0"\nCallGameFunction gcareadirector.setcurrentlevel 1\nPop\n'
           'PushConstantString "lev1"\nCallGameFunction gcareadirector.setcurrentlevel 1\nReturn\n')
    vm, ctx, tr, log = make_vm(asm)
    ad = ctx.subsystem(d.AreaDirector)
    ad.area = 0x66726E74                                                        # an area id with no loader in the map
    r = vm.call("main", [])
    assert not r.ok and "Script is calling gcareadirector.setcurrentlevel" in vm.last_error.message


def _swallow_world(vm, ctx, objs):
    d = dr()
    ad = ctx.subsystem(d.AreaDirector)
    ad.area_source = lambda aid: objs
    return ad


def test_loadarea_swallows_a_nested_error_and_continues_with_the_next_block():
    """D16 at the three LoadArea blocks: SetUnderWater's OnEvent(0x36, 0) raises (DebugOn, an error that advances pc by 1);
    the native catches it (an X record, an E line), the block ends, the next block loads, and the native returns normally."""
    d = dr()
    asm = ('.function main 0\nPushConstantString "frnt"\nCallGameFunction gcareadirector.setcurrentarea 1\nReturn\n'
           '.function OnEvent 2\nDebugOn\nPushConstanti8 1\nReturn\n')
    vm, ctx, tr, log = make_vm(asm)
    lbld = 0x6C626C64                                                            # [0x734f10]: the only id that sets up
    ad = _swallow_world(vm, ctx, [d.AreaObject("objects", 0xA1, lbld), d.AreaObject("objects", 0xA2, 0x1234),
                                  d.AreaObject("characters", 0xB1, 0x4321), d.AreaObject("hives", 0xC1, lbld),
                                  d.AreaObject("hives", 0xC2, 0x5)])
    ad.main_character_id = lbld                                                 # the guard also needs id == main character
    r = vm.call("main", [])
    assert r.ok and vm.last_error is None                                       # the swallow is not an error of the call
    c = [l for l in log if l.startswith("C ")]
    x = [l for l in log if l.startswith("X ")]
    assert len(c) == 2 and all(l.startswith("C 00000000 OnEvent 0010:00000036 0010:00000000 ") for l in c)
    assert x == ["X 00000000", "X 00000000"]
    # C, X, then the native's own line, in event order
    order = [l.split()[0] for l in log if l[0] in "CXK0"]
    assert order == ["C", "X", "C", "X", "00000000"]
    # the failing object is not completed; the other objects of its block are skipped; the characters block (nothing to
    # fail) loads; the hives block fails on its first object
    got = [e.obj_id for e in ad.loaders[0x66726E74].entries]
    assert got == [0xB1]
    assert sum(1 for l in tr.lines if l.startswith("E ")) == 2                   # one E line per swallowed error (D16 rule 2)
    assert len(vm.swallowed) == 2


def test_loadarea_set_under_water_flag_picks_the_branch():
    d = dr()
    for flag, want in ((0, 0), (1, 1), (0x100, 0), (0x1FF, 1)):
        asm = ('.function main 0\nPushConstantString "frnt"\nCallGameFunction gcareadirector.setcurrentarea 1\nReturn\n'
               '.function OnEvent 2\nPushConstanti8 1\nReturn\n')
        vm, ctx, tr, log = make_vm(asm)
        ad = _swallow_world(vm, ctx, [d.AreaObject("objects", 0xA1, 0x6C626C64)])
        ad.main_character_id = 0x6C626C64
        ad.under_water = flag
        assert vm.call("main", []).ok
        c = [l for l in log if l.startswith("C ")]
        assert len(c) == 1 and c[0].split()[3:5] == ["0010:00000036", "0010:%08x" % want], flag      # the byte [0x73c5a8]
        assert [e.obj_id for e in ad.loaders[0x66726E74].entries] == [0xA1]


def test_loadarea_guard_compares_the_motion_id_with_the_main_character():
    d = dr()
    asm = ('.function main 0\nPushConstantString "frnt"\nCallGameFunction gcareadirector.setcurrentarea 1\nReturn\n'
           '.function OnEvent 2\nPushConstanti8 1\nReturn\n')
    vm, ctx, tr, log = make_vm(asm)
    ad = _swallow_world(vm, ctx, [d.AreaObject("objects", 0xA1, 0x1111)])       # not 'cnt1': no SetUnderWater
    assert vm.call("main", []).ok and not any(l.startswith("C ") for l in log) and ad.set_under_water_calls == 0


def test_loadarea_vm_reach_asm_pins(disasm, image):
    """The ctor gate and the hive split, from the binary: 0x544065 cmp [0x734f10],edx (edx = [obj+0x80], 0x54405f) / jne
    0x5440d3, then SetupMoveable 0x545c00 (0x54408b); the motion id [ms+0x14] takes the same field (0x543f44 -> 0x5748ec);
    the guard 0x574eeb-0x574ef1; LoadHives 0x62623c cmp [rec+0x2c],0 / je -> ctor 0x6239b0 (0x6262fa), else 0x6236a0
    (0x6262b5) -> 0x5432a0 (0x6236d4)."""
    pin(disasm, 0x54405f, 0x544090, {0x54405f: "mov edx, dword ptr [edx + 0x80]", 0x544065: "cmp dword ptr [0x734f10], edx",
                                     0x54406b: "jne 0x5440d3", 0x54408b: "call 0x545c00"})
    pin(disasm, 0x543f3e, 0x543f4a, {0x543f44: "mov edx, dword ptr [edx + 0x80]"})
    pin(disasm, 0x5748e3, 0x5748ef, {0x5748ec: "mov dword ptr [ebx + 0x14], edx"})
    pin(disasm, 0x574eeb, 0x574f02, {0x574eeb: "mov ecx, dword ptr [esi + 0x14]", 0x574ef1: "cmp ecx, edx",
                                     0x574ef3: "jne 0x574f02", 0x574efd: "call 0x589050"})
    pin(disasm, 0x62623c, 0x626242, {0x62623c: "cmp dword ptr [edi + 0x2c], 0x0", 0x626240: "je 0x6262c0"})
    pin(disasm, 0x6262b5, 0x6262ba, {0x6262b5: "call 0x6236a0"})
    pin(disasm, 0x6262fa, 0x6262ff, {0x6262fa: "call 0x6239b0"})
    pin(disasm, 0x6236d4, 0x6236d9, {0x6236d4: "call 0x5432a0"})
    assert image.u32(0x734f10) == 0x6C626C64    # (the main character id [0x782884] is .bss, written 'cnt1' at 0x482d43: pinned elsewhere)
    plain = disasm(0x6239b0, 0x623a00)
    assert not any("0x5432a0" in v or "0x545c00" in v for v in plain.values())


def test_loadarea_cnt1_does_not_fire_and_plain_hives_never_do():
    """'cnt1' (the real main character) never reaches the VM: SetupMoveable runs only for id == 'lbld' (0x544065), and the
    guard then needs 'lbld' == the main character. Each of the three conditions alone is not enough."""
    d = dr()
    lbld, cnt1 = 0x6C626C64, 0x636E7431
    asm = ('.function main 0\nPushConstantString "frnt"\nCallGameFunction gcareadirector.setcurrentarea 1\nReturn\n'
           '.function OnEvent 2\nPushConstanti8 1\nReturn\n')
    cases = [
        ([d.AreaObject("objects", 0xA1, cnt1), d.AreaObject("hives", 0xC1, cnt1)], cnt1, 0),                        # main character cnt1: never fires
        ([d.AreaObject("objects", 0xA1, lbld)], cnt1, 0),                           # lbld object, main character cnt1
        ([d.AreaObject("objects", 0xA1, cnt1)], lbld, 0),                           # main character lbld, object id cnt1
        ([d.AreaObject("hives", 0xC1, lbld, plain=True)], lbld, 0),                 # hive ctor 0x6239b0
        ([d.AreaObject("hives", 0xC1, lbld)], lbld, 1),                             # hive ctor 0x5432a0: fires
        ([d.AreaObject("objects", 0xA1, lbld)], lbld, 1),
        ([d.AreaObject("characters", 0xB1, cnt1)], cnt1, 1),                        # characters: no 'lbld' gate (0x418850)
        ([d.AreaObject("characters", 0xB1, lbld)], cnt1, 0),                        # id != main character
    ]
    for objs, main, want in cases:
        vm, ctx, tr, log = make_vm(asm)
        ad = _swallow_world(vm, ctx, objs)
        ad.main_character_id = main
        assert vm.call("main", []).ok
        assert sum(1 for l in log if l.startswith("C ")) == want == ad.set_under_water_calls, (objs, main)


def test_cleanup_load_removes_matching_pickups_through_the_model_director():
    """GcSaver::SetCurrentArea -> CleanupLoad (BetterSaver 0x10001ae0): BetterSaver's vector entries of this area are removed
    twice (the second removal finds nothing), the exe lists once; entries of other areas stay."""
    d = dr()
    asm = ('.function main 0\nPushConstantString "frnt"\nCallGameFunction gcareadirector.setcurrentarea 1\nReturn\n'
           + _onevent_logger())
    vm, ctx, tr, log = make_vm(asm)
    frnt = 0x66726E74
    ad = _swallow_world(vm, ctx, [d.AreaObject("objects", i, 1) for i in (0x11, 0x12, 0x13, 0x14)])
    sv = ctx.subsystem(d.Saver)
    sv.better = [(0x11, frnt), (0x99, frnt), (0x12, 0x1)]
    sv.pickups_a = [(0x13, frnt)]
    sv.pickups_b = [(0x14, 0x2), (0x14, frnt)]
    _model_world(vm, ctx, [0x11, 0x12, 0x13, 0x14])
    assert vm.call("main", []).ok
    cs = [l.split()[4] for l in log if l.startswith("C ")]
    assert cs == ["0010:00000011", "0010:00000013", "0010:00000014"]            # 0x11 once (twice asked), 0x99 none, 0x12 other area
    assert [e.obj_id for e in ad.loaders[frnt].entries] == [0x12]


def test_remove_object_removes_only_the_first_of_duplicate_ids():
    """GcAreaLoader::RemoveObject 0x501dc0 erases the FIRST entry with the id: BetterSaver's double call removes two
    duplicates, the exe list's single call removes one and leaves the other."""
    d = dr()
    asm = ('.function main 0\nPushConstantString "frnt"\nCallGameFunction gcareadirector.setcurrentarea 1\nReturn\n'
           + _onevent_logger())
    vm, ctx, tr, log = make_vm(asm)
    frnt = 0x66726E74
    ad = _swallow_world(vm, ctx, [d.AreaObject("objects", i, 1) for i in (0x11, 0x11, 0x13, 0x13, 0x15)])
    sv = ctx.subsystem(d.Saver)
    sv.better = [(0x11, frnt)]
    sv.pickups_a = [(0x13, frnt)]
    _model_world(vm, ctx, [0x11, 0x13, 0x15])
    assert vm.call("main", []).ok
    cs = [l.split()[4] for l in log if l.startswith("C ")]
    assert cs == ["0010:00000011", "0010:00000013"]      # the second 0x11 entry is erased too, but its model object is already gone
    assert [e.obj_id for e in ad.loaders[frnt].entries] == [0x13, 0x15]


def test_cleanup_load_without_a_loader_returns_at_once():
    d = dr()
    vm, ctx, tr, log = make_vm(".function main 0\nReturn\n")
    sv = ctx.subsystem(d.Saver)
    sv.better = [(1, 0x5)]
    sv.set_current_area(ctx, 0x5)                                               # no loader for 0x5: 0x10001af5 returns
    assert sv.area == 0x5 and not log


# ===============================================================================================================
# 5. gcareadirector.getscriptfluffstring
# ===============================================================================================================
def read_fluff_independently(install, a, b, c, lang="eng"):
    """A 10-line reader of the install file at the cited offsets, sharing no code with the model: the block table of
    Lang_<lang>.blk, then the string image (count at 0, record array at 4, 16-byte records, 4-byte string pointers to the
    length byte)."""
    blk = (Path(install) / "data" / "Languages" / ("lang_%s.blk" % lang)).read_bytes()
    n = struct.unpack_from("<I", blk, 8)[0]
    ent = [struct.unpack_from("<40sIIII", blk, 12 + 56 * i) for i in range(n)]
    name, bs, fs, fl, off = [e for e in ent if e[0].split(b"\0")[0].lower() == b"fluff_%s.slb" % lang.encode()][0]
    img = blk[off:off + fs]
    cnt, rp = struct.unpack_from("<II", img, 0)
    for i in range(cnt):
        ra, rb, rn, tp = struct.unpack_from("<IIII", img, rp + 16 * i)
        if (ra, rb) == (a, b):
            if c >= rn:
                return b""
            sp = struct.unpack_from("<I", img, tp + 4 * c)[0]
            return img[sp + 1:img.index(b"\0", sp + 1)]
    return b""


def test_fluff_asm_pins(disasm, image):
    pin(disasm, 0x487250, 0x4872a3, {0x487274: "call 0x48acc0", 0x487287: "call 0x46de00"})
    pin(disasm, 0x48acc0, 0x48ae0d, {
        0x48ad29: "cmp dword ptr [0x8391c0], 0x0", 0x48ad30: "je 0x48ad3a", 0x48ad32: "test edi, edi", 0x48ad34: "je 0x48ad3a",
        0x48ad36: "test esi, esi", 0x48ad38: "jne 0x48ad60", 0x48ad3a: "push 0x70b7e7", 0x48ad73: "call 0x5e86e0",
        0x48ad7a: "push 0x70b7eb", 0x48ad85: "call 0x4f5990", 0x48ad8c: "jne 0x48ad93", 0x48ad97: "push 0x70b7e7"})
    assert image.read(0x70b7e7, 4) == b"XXX\0" and image.read(0x70b7eb, 1) == b"\0"      # "XXX" and the empty string
    pin(disasm, 0x5e86e0, 0x5e8780, {
        0x5e86ee: "mov ebx, dword ptr [esi + 0x9c]", 0x5e86fb: "cmp ecx, edx", 0x5e8707: "jne 0x5e8765",
        0x5e870c: "dec edx", 0x5e870d: "cmp edi, edx", 0x5e870f: "jbe 0x5e8730", 0x5e8711: "push 0x74ae14",
        0x5e8730: "mov edx, edi", 0x5e8732: "shl edx, 0x2", 0x5e8735: "add edx, dword ptr [ebx + 0xc]",
        0x5e8738: "mov eax, dword ptr [edx]"})
    assert image.read(0x74ae14, 1) == b"\0"                                              # the empty default string
    # InitStrings: every string pointer is incremented (the stored pointer is the length byte), then the text is assigned
    pin(disasm, 0x5e8600, 0x5e86e0, {0x5e8665: "mov esi, dword ptr [edi + 0x9c]", 0x5e8675: "inc dword ptr [ebx]",
                                      0x5e8680: "call 0x4f5c00", 0x5e86a1: "add ebx, 0x4", 0x5e86b4: "mov edx, dword ptr [edi + 0x98]"})
    pin(disasm, 0x46de00, 0x46de3d, {0x46de08: "mov word ptr [esi], 0x16", 0x46de2c: "call 0x46de40"})
    # Init loads fluff_<lang>.slb from Lang_<lang>.blk: the language byte [0x70b384] picks the file; 0 is English
    pin(disasm, 0x48784c, 0x487865, {0x48784c: "cmp byte ptr [0x70b384], 0x0", 0x487855: "push 0x70b3f4"})
    assert image.read(0x70b3f4, 36) == b"Root/Data/Levels/lev1/fluff_eng.slb\0"
    assert image.read(0x70b514, 33) == b"Root/Data/Languages/Lang_eng.blk\0"
    assert image.read(0x70b384, 1) == b"\0"
    names = [image.read(a, 40).split(b"\0")[0].decode() for a in
             (0x70b3f4, 0x70b418, 0x70b43c, 0x70b460, 0x70b484, 0x70b4a8, 0x70b4cc, 0x70b4f0)]
    assert [n.rsplit("_", 1)[1][:3] for n in names] == list(dr().LANG_SUFFIXES)
    # the table is assigned to [0x8391c0] (0x487c74) then InitStrings runs (0x487cb0)
    pin(disasm, 0x487c6e, 0x487cb5, {0x487c74: "mov dword ptr [0x8391c0], edx", 0x487cb0: "call 0x5e8600"})


def test_fluff_a9_values_equal_the_independent_reader(lomn_data):
    inst = lomn_data / "install"
    # the 23 + 23 calls of the A9 path ask ('fend', 1, n); a few are pinned by hand from the file
    assert read_fluff_independently(inst, 0x66656E64, 1, 0) == b"16-bit color"
    assert read_fluff_independently(inst, 0x66656E64, 1, 1) == b"32-bit color"
    for n in (0x23, 0x1A, 0x29, 0x3A, 0x30, 0x15, 0x19, 0x14, 0x28, 7, 3, 0xD, 0x10, 0x32, 0x18, 0xC, 0x13, 8, 0x37):
        want = read_fluff_independently(inst, 0x66656E64, 1, n)
        assert want, n
        r, vm, ctx, tr, log = run_main(".function main 0\nPushConstanti32 %d\nPushConstanti32 1\nPushConstanti32 %d\n"
                                       "CallGameFunction gcareadirector.getscriptfluffstring 3\nReturn\n"
                                       % (0x66656E64, n), install=inst)
        assert log[0].endswith("-> 0016:00000001 S %d %s" % (len(want), want.hex())), (n, log[0])


def test_fluff_edges_on_the_real_file(lomn_data):
    inst = lomn_data / "install"
    d = dr()
    vm, ctx, tr, log = make_vm(".function main 0\nReturn\n", install=inst)
    t = ctx.subsystem(d.StringTables).fluff()
    assert t is not None and t.count == 4
    assert d.get_script_fluff_string(ctx, 0x66656E64, 1, 0) == b"16-bit color"
    assert d.get_script_fluff_string(ctx, 0x66656E64, 1, 67) == b"XXX"                  # index == count: the empty string -> XXX
    assert d.get_script_fluff_string(ctx, 0x66656E64, 1, 0xFFFFFFFF) == b"XXX"          # unsigned compare (0x5e870f jbe)
    assert d.get_script_fluff_string(ctx, 0x66656E64, 1, 66) == read_fluff_independently(inst, 0x66656E64, 1, 66) != b"XXX"
    assert d.get_script_fluff_string(ctx, 0, 1, 0) == b"XXX"                            # a == 0 (0x48ad36)
    assert d.get_script_fluff_string(ctx, 0x66656E64, 0, 0) == b"XXX"                   # b == 0 (0x48ad32)
    assert d.get_script_fluff_string(ctx, 0x66656E64, 9, 0) == b"XXX"                   # no such record
    assert d.get_script_fluff_string(ctx, 0x646F6F72, 1, 0).startswith(b"You need to collect")   # a second record
    assert d.get_script_fluff_string(ctx, 0x646F6F72, 1, 1) == b"XXX"                  # that record has 1 string


def test_fluff_a_zero_or_b_zero_is_xxx_even_when_the_table_has_the_text():
    """0x48ad29-0x48ad3a: a == 0 or b == 0 skips GetString. A table holding records (0, 1) and (1, 0) must still give XXX."""
    d = dr()
    t = make_table([(0, 1, [b"zero-a"]), (1, 0, [b"zero-b"]), (1, 1, [b"ok"])])
    vm, ctx, tr, log = make_vm(".function main 0\nReturn\n")
    ctx.subsystem(d.StringTables)._fluff = t
    ctx.subsystem(d.StringTables)._tried = True
    assert d.get_script_fluff_string(ctx, 0, 1, 0) == b"XXX"
    assert d.get_script_fluff_string(ctx, 1, 0, 0) == b"XXX"
    assert d.get_script_fluff_string(ctx, 1, 1, 0) == b"ok"


def test_fluff_without_a_vfs_or_with_a_missing_file_is_xxx(tmp_path):
    d = dr()
    r, vm, ctx, tr, log = call_i("gcareadirector.getscriptfluffstring", 0x66656E64, 1, 0)
    assert log[0].endswith("-> 0016:00000001 S 3 585858")
    inst = tmp_path / "install"
    (inst / "data" / "Languages").mkdir(parents=True)
    r, vm, ctx, tr, log = call_i("gcareadirector.getscriptfluffstring", 0x66656E64, 1, 0, install=inst)
    assert log[0].endswith("S 3 585858")
    # a language value beyond the eight names has no file name (the locals stay empty)
    vm, ctx, tr, log = make_vm(".function main 0\nReturn\n", install=inst)
    ctx.subsystem(d.IniSettings).language = 8
    assert ctx.subsystem(d.StringTables).fluff() is None


def build_slb(records, bad_reloc=(), tail_count=None):
    """A relocatable string image: u32 count, u32 8, the 16-byte records, the pointer tables, the strings (length byte, text,
    NUL), then the relocation table and the 0xc0ffee magic (ScSLKManager::GetSLKFile 0x4032e0)."""
    n = len(records)
    body = bytearray(struct.pack("<II", n, 8)) + bytearray(16 * n)
    relocs = [4]
    tbl_at = []
    for i, (a, b, strs) in enumerate(records):
        tbl_at.append(len(body))
        body += bytes(4 * len(strs))
    str_at = []
    for i, (a, b, strs) in enumerate(records):
        offs = []
        for s in strs:
            offs.append(len(body))
            body += bytes([len(s) & 0xFF]) + s + b"\0"
        str_at.append(offs)
    for i, (a, b, strs) in enumerate(records):
        cnt = len(strs) if tail_count is None else tail_count
        struct.pack_into("<IIII", body, 8 + 16 * i, a, b, cnt, tbl_at[i] if strs else 0)
        if strs:
            relocs.append(8 + 16 * i + 12)
        for j, o in enumerate(str_at[i]):
            struct.pack_into("<I", body, tbl_at[i] + 4 * j, o)
            relocs.append(tbl_at[i] + 4 * j)
    relocs = [r for r in relocs if r not in bad_reloc]
    while (len(body) + 4 * len(relocs) + 8 - 4) % 4:
        body += b"\0"
    for r in relocs:
        body += struct.pack("<I", r)
    body += struct.pack("<II", len(relocs), 0xC0FFEE)
    return bytes(body)


def make_table(records, **kw):
    d = dr()
    return d.FluffTable(d.SlbImage(build_slb(records, **kw)))


def test_fluff_table_model_on_a_synthetic_image():
    t = make_table([(1, 1, [b"alpha", b"beta"]), (1, 2, [b"gamma"]), (2, 1, [])])
    assert t.get_string(1, 1, 0) == b"alpha" and t.get_string(1, 1, 1) == b"beta" and t.get_string(1, 2, 0) == b"gamma"
    assert t.get_string(1, 1, 2) == b"" and t.get_string(1, 1, M32) == b"" and t.get_string(9, 9, 0) == b""
    assert t.get_string(1, 2, 1) == b""
    # the first record with the same ids wins
    t = make_table([(1, 1, [b"first"]), (1, 1, [b"second"])])
    assert t.get_string(1, 1, 0) == b"first"
    # the length byte is skipped by InitStrings: it is NOT the text length (the text runs to the NUL)
    img = bytearray(build_slb([(1, 1, [b"hello"])]))
    p = struct.unpack_from("<I", img, struct.unpack_from("<I", img, 8 + 12)[0])[0]
    img[p] = 0x02                                                                        # a wrong length byte
    t = dr().FluffTable(dr().SlbImage(bytes(img)))
    assert t.get_string(1, 1, 0) == b"hello"


def test_fluff_text_of_255_characters_loads_but_the_native_throws():
    d = dr()
    t = make_table([(1, 1, [b"x" * 254, b"y" * 255])])
    vm, ctx, tr, log = make_vm(".function main 0\nReturn\n")
    ctx.subsystem(d.StringTables)._fluff = t
    ctx.subsystem(d.StringTables)._tried = True
    assert d.get_script_fluff_string(ctx, 1, 1, 0) == b"x" * 254
    with pytest.raises(VmError) as e:
        d.get_script_fluff_string(ctx, 1, 1, 1)                              # Init(npos) 0x48adf2: len >= 0xff
    assert e.value.message == "kOutOfRange"
    asm = (".function main 0\nPushConstanti32 1\nPushConstanti32 1\nPushConstanti32 1\n"
           "CallGameFunction gcareadirector.getscriptfluffstring 3\nReturn\n")
    vm, ctx, tr, log = make_vm(asm)
    ctx.subsystem(d.StringTables)._fluff = t
    ctx.subsystem(d.StringTables)._tried = True
    r = vm.call("main", [])
    assert not r.ok and "Script is calling gcareadirector.getscriptfluffstring" in vm.last_error.message


def test_fluff_table_hostile_images():
    d = dr()
    with pytest.raises(VmError) as e:                                   # a record with count 0 reads table[c] unchecked
        t = make_table([(1, 1, [b"a"])], tail_count=0)
        t.get_string(1, 1, 0)
    assert "count 0" in e.value.message
    with pytest.raises(VmError) as e:                                   # a string pointer that is not in the relocation table
        make_table([(1, 1, [b"a"])], bad_reloc=(build_reloc_offset(),))
    assert "unrelocated" in e.value.message
    with pytest.raises(VmError) as e:                                   # InitStrings: a text of 256 characters (> 255)
        make_table([(1, 1, [b"x" * 256])])
    assert e.value.message == "kOutOfRange"
    make_table([(1, 1, [b"x" * 255])])                                  # 255 passes (`seta`: len > capacity)
    # no magic: nothing is relocated, so the first dereference is Undefined behaviour
    with pytest.raises(VmError):
        d.FluffTable(d.SlbImage(struct.pack("<II", 1, 8) + bytes(16)))


def build_reloc_offset():
    # the first string-pointer slot of the single-record image built by build_slb([(1, 1, [b"a"])])
    return 8 + 16 + 0


def _write_lang_install(tmp_path, suffix, slb, extra=()):
    inst = tmp_path / "install"
    (inst / "data" / "Languages").mkdir(parents=True)
    names = [("Fluff_%s.slb" % suffix, slb)] + list(extra)
    names.sort(key=lambda n: n[0].lower())
    head = b"BLKF" + struct.pack("<II", 1, len(names))
    off = len(head) + 56 * len(names)
    ents, body = b"", b""
    for n, data in names:
        ents += struct.pack("<40sIIII", n.encode(), len(data), len(data), 2, off + len(body))
        body += data
    (inst / "data" / "Languages" / ("Lang_%s.blk" % suffix)).write_bytes(head + ents + body)
    return inst


def test_language_byte_picks_the_block_file_and_the_entry(tmp_path):
    d = dr()
    slb_eng = build_slb([(5, 1, [b"english"])])
    slb_ger = build_slb([(5, 1, [b"deutsch"])])
    inst = _write_lang_install(tmp_path, "eng", slb_eng, extra=[("Other_eng.slb", b"junk")])
    (inst / "data" / "Languages" / "Lang_ger.blk").write_bytes(
        (_write_lang_install(tmp_path / "g", "ger", slb_ger) / "data" / "Languages" / "Lang_ger.blk").read_bytes())
    for lang, want in ((0, b"english"), (2, b"deutsch")):
        vm, ctx, tr, log = make_vm(".function main 0\nReturn\n", install=inst)
        ctx.subsystem(d.IniSettings).language = lang
        assert d.get_script_fluff_string(ctx, 5, 1, 0) == want, lang


# ===============================================================================================================
# 6. gcsaver.cheatmenu
# ===============================================================================================================
def test_cheatmenu_asm_pins(disasm, image):
    pin(disasm, 0x5d35b0, 0x5d35cb, {0x5d35b3: "call 0x5d4fc0", 0x5d35bd: "call 0x407f30"})
    pin(disasm, 0x5d4fc0, 0x5d5080, {
        0x5d4fca: "push 0x83b410", 0x5d4fcf: "push 0xff", 0x5d4fdb: "push 0x74764b", 0x5d4fe0: "push 0x747679",
        0x5d4fe5: "push 0x74761d", 0x5d4fea: "call dword ptr [0x771424]", 0x5d5025: "call 0x401850",
        0x5d502a: "xor ebx, ebx", 0x5d502c: "push 0x74765f", 0x5d5037: "call 0x4f5990", 0x5d503c: "test eax, eax",
        0x5d503e: "jne 0x5d5045", 0x5d5040: "mov ebx, 0x1", 0x5d5056: "mov al, bl", 0x5d506d: "mov al, bl"})
    # the compare: length of the string first, then memcmp (0x657820), 0 only when the lengths and the bytes agree
    pin(disasm, 0x4f5990, 0x4f59f0, {0x4f59a3: "call 0x6573c0", 0x4f59b4: "cmp dword ptr [ebx + 0x4], esi",
                                     0x4f59c8: "call 0x657820", 0x4f59d2: "jne 0x4f59d9", 0x4f59d7: "sub eax, esi"})
    pin(disasm, 0x657820, 0x657845, {0x657830: "test ecx, ecx", 0x657834: "rep cmpsb byte ptr [esi], byte ptr es:[edi]",
                                     0x65783e: "sub eax, ecx"})                           # memcmp: bytes, no case folding
    assert image.read(0x74761d, 5) == b"Misc\0" and image.read(0x747679, 10) == b"CheatMenu\0"
    assert image.read(0x74764b, 1) == b"\0" and image.read(0x74765f, 11) == b"LEGOTester\0"
    # CreateVariant(int) 0x407f30 takes a BYTE (movzx edx, byte)
    pin(disasm, 0x407f30, 0x407f46, {0x407f35: "mov word ptr [ebx], 0x10", 0x407f3a: "movzx edx, byte ptr [esp + 0xc]"})


def test_the_owner_ini_has_no_cheatmenu_key(lomn_data):
    p = lomn_data / "install" / "Bionicle.ini"
    cp = configparser.ConfigParser(strict=False)
    cp.optionxform = str
    cp.read(p)
    assert "Misc" in cp and not cp.has_option("Misc", "CheatMenu") and "CheatMenu" not in cp["Misc"]
    assert not any(k.lower() == "cheatmenu" for s in cp.sections() for k in cp[s])
    ini = dr().IniSettings.from_text(p.read_bytes())
    assert ini.get_private_profile_string(b"Misc", b"CheatMenu", b"") == b""
    assert ini.get_private_profile_string(b"misc", b"language", b"x") == b"0"           # the parser works on this file


def test_cheatmenu_default_is_int_zero_and_the_compare_is_exact():
    d = dr()
    r, vm, ctx, tr, log = call0("gcsaver.cheatmenu")
    assert r == Variant(0x10, 0) and log[0] == "00000000 gcsaver.cheatmenu 0 -> 0010:00000000"
    for value, want in ((b"LEGOTester", 1), (b"legotester", 0), (b"LEGOTester ", 0), (b"LEGOTeste", 0), (b" LEGOTester", 0),
                        (b"LEGOTesterx", 0), (b"", 0), (b"LEGOTester" + b"x" * 300, 0)):
        vm, ctx, tr, log = make_vm(".function main 0\nCallGameFunction gcsaver.cheatmenu 0\nReturn\n")
        ctx.subsystem(d.IniSettings).set(b"Misc", b"CheatMenu", value)
        r = vm.call("main", [])
        assert r.ok and r.result == Variant(0x10, want), value


def test_cheatmenu_ini_lookup_is_case_insensitive_and_section_bound():
    d = dr()
    for sec, key, want in ((b"misc", b"cheatmenu", 1), (b"MISC", b"CHEATMENU", 1), (b"Cheat", b"CheatMenu", 0),
                           (b"Misc", b"CheatMenus", 0)):
        vm, ctx, tr, log = make_vm(".function main 0\nCallGameFunction gcsaver.cheatmenu 0\nReturn\n")
        ctx.subsystem(d.IniSettings).set(sec, key, b"LEGOTester")
        assert vm.call("main", []).result == Variant(0x10, want), (sec, key)
    # a 254-character value is cut by the buffer (size 0xff) before the compare; the NUL ends the value
    ini = d.IniSettings()
    ini.set(b"Misc", b"CheatMenu", b"a" * 400)
    assert len(ini.get_private_profile_string(b"Misc", b"CheatMenu", b"", 0xFF)) == 254


# ===============================================================================================================
# 7. gcsaver.getammo / setenergy / sethealth / setmask
# ===============================================================================================================
def test_saver_field_asm_pins(disasm, image):
    pin(disasm, 0x5d35f0, 0x5d3611, {0x5d35fb: "mov dword ptr [0x7474a0], edx", 0x5d3601: "mov word ptr [ebx], 0xf",
                                     0x5d3606: "mov dword ptr [ebx + 0x4], 0x0"})
    pin(disasm, 0x5d3650, 0x5d3671, {0x5d365b: "mov byte ptr [0x7474a8], dl", 0x5d3661: "mov word ptr [ebx], 0xf"})
    pin(disasm, 0x5d3680, 0x5d36a1, {0x5d368b: "mov byte ptr [0x7474ac], dl", 0x5d3691: "mov word ptr [ebx], 0xf"})
    pin(disasm, 0x5d3910, 0x5d392c, {0x5d3913: "mov ax, word ptr [0x7474c8]", 0x5d391e: "call 0x4183f0"})
    pin(disasm, 0x4183f0, 0x418406, {0x4183f5: "mov word ptr [ebx], 0x10", 0x4183fa: "movzx edx, word ptr [esp + 0xc]"})
    # the .data defaults at Setup (natives.md §9.1): the mask is 0x3f3f3f3f, the others 0
    assert image.u32(0x7474a0) == 0x3F3F3F3F and image.u32(0x7474a4) == 0 and image.u32(0x7474a8) == 0
    assert image.u32(0x7474ac) == 0 and image.u32(0x7474c8) == 0 and image.u32(0x74749c) == 0x3F3F3F3F
    d = dr()
    sv = d.Saver(None)
    assert sv.ammo == 0
    # ResetData does not touch these fields (it writes only 0x74748c, 0x747490, 0x747494, 0x747498, the counters and 0x8391a0)
    rows = disasm(0x5d5490, 0x5d5500)
    touched = {int(m, 16) for t in rows.values() for m in re.findall(r"\[0x([0-9a-f]{6})\]", t)}
    assert not touched & {0x7474a0, 0x7474a4, 0x7474a8, 0x7474ac, 0x7474c8}


def test_getammo_reads_the_word_and_the_setters_are_unbound_tier2():
    d = dr()
    vm, ctx, tr, log = make_vm(".function main 0\nCallGameFunction gcsaver.getammo 0\nReturn\n")
    sv = ctx.subsystem(d.Saver)
    assert vm.call("main", []).result == Variant(0x10, 0)                       # Setup calls getammo before addtoa: 0
    sv.ammo = 250                                                               # AddToa / the Toa ctor writes 250 (0x56898b)
    assert vm.call("main", []).result == Variant(0x10, 250)
    sv.ammo = 0x1FFFF
    assert vm.call("main", []).result == Variant(0x10, 0xFFFF)                  # a word (movzx word)
    # setmask/sethealth/setenergy are tier2 with no bound body (fix 1): no bound native reads their stores back
    rows = {r["key"]: r for r in json.loads(OVERRIDES.read_text())}
    for k in ("gcsaver.setmask", "gcsaver.sethealth", "gcsaver.setenergy"):
        assert rows[k]["tier"] == "tier2" and "default" not in rows[k] and "0x7474a4" not in rows[k]["note"], k
        assert k not in dr().CITES


# ===============================================================================================================
# 8. bettersaver.getbooleanvalue / setbooleanvalue / save
# ===============================================================================================================
def test_bettersaver_asm_pins(disasm, dll_path, dll_image):
    pin(disasm, 0x10001f70, 0x10001fe4, {
        0x10001f77: "movzx esi, word ptr [ebp + 0x10]", 0x10001f88: "call 0x10005ab0", 0x10001f90: "cmp byte ptr [eax + 0xd], 0x0",
        0x10001f94: "jne 0x10001fc8", 0x10001f96: "cmp si, word ptr [eax + 0xe]", 0x10001f9a: "jb 0x10001fc8",
        0x10001fb8: "mov al, byte ptr [eax + 0x10]", 0x10001fc8: "xor al, al", 0x10001fd1: "setne cl",
        0x10001fd8: "mov ecx, 0x10", 0x10001fdd: "mov word ptr [eax], cx"}, exe=dll_path)
    pin(disasm, 0x10001ff0, 0x10002024, {
        0x10001ff4: "cmp dword ptr [ebp + 0x14], 0x0", 0x10001ffb: "movzx eax, word ptr [ebp + 0x10]",
        0x10002005: "setne byte ptr [ebp + 0x13]", 0x10002009: "call 0x10004730", 0x10002011: "mov ecx, 0xf",
        0x1000201d: "mov word ptr [eax], cx"}, exe=dll_path)
    pin(disasm, 0x10001f40, 0x10001f67, {0x10001f43: "call 0x10002b00", 0x10001f56: "mov ecx, 0xf",
                                         0x10001f62: "mov word ptr [eax], cx"}, exe=dll_path)
    # the map update 0x10004730: true -> assign (0x100047c9) or insert (0x1000483f); false -> erase when present (0x10004889);
    # key < 0x20 -> bts / btr on [0x8391a0] through the pointer table; keys 0xe / 0x10 / 0x12 (true) -> SetQuest(level, 1/2/3)
    pin(disasm, 0x10004730, 0x10004900, {
        0x10004762: "cmp byte ptr [ebx], 0x0", 0x1000476f: "je 0x1000484c", 0x100047c9: "mov byte ptr [ecx + 0x10], al",
        0x1000483f: "call 0x10005bc0", 0x10004889: "call 0x100050c0", 0x1000488e: "cmp word ptr [edi], 0x20",
        0x10004892: "jae 0x100048e6", 0x1000489e: "mov ecx, dword ptr [0x1000a040]", 0x100048ab: "bts eax, edx",
        0x100048b3: "cmp eax, 0xe", 0x100048b8: "push 0x1", 0x100048bc: "cmp eax, 0x10", 0x100048c1: "push 0x2",
        0x100048c5: "cmp eax, 0x12", 0x100048ca: "push 0x3", 0x100048cc: "mov eax, dword ptr [0x1000a07c]",
        0x100048d1: "movzx eax, byte ptr [eax]", 0x100048d5: "mov eax, 0x5d5e20", 0x100048da: "call eax",
        0x100048e1: "btr eax, edx"}, exe=dll_path)
    # GcSaver::SetQuest 0x5d5e20: level 1..6, quest 1..3, `or byte [0x83b37f + level], 1|2|4` via the jump table 0x7474fc
    pin(disasm, 0x5d5e20, 0x5d5e70, {0x5d5e24: "cmp cl, 0x1", 0x5d5e29: "cmp cl, 0x6", 0x5d5e33: "sub edx, 0x1",
                                     0x5d5e36: "cmp edx, 0x2", 0x5d5e45: "or byte ptr [edx + 0x83b37f], 0x1",
                                     0x5d5e53: "or byte ptr [edx + 0x83b37f], 0x2", 0x5d5e63: "or byte ptr [edx + 0x83b37f], 0x4"})
    import exc
    img = exc.Image.from_file(dll_path.parent.parent.parent / "reference" / "LEGOBionicle.exe")
    assert [img.u32(0x7474fc + 4 * i) for i in range(3)] == [0x5d5e42, 0x5d5e50, 0x5d5e60]


def test_getbooleanvalue_absent_is_int_zero_and_set_get_round_trip():
    d = dr()
    r, vm, ctx, tr, log = call_i("bettersaver.getbooleanvalue", 100)
    assert r == Variant(0x10, 0) and log[0] == "00000000 bettersaver.getbooleanvalue 1 -> 0010:00000000"
    asm = (".function main 0\nPushConstanti32 %d\nPushConstanti32 %d\nCallGameFunction bettersaver.setbooleanvalue 2\nPop\n"
           "PushConstanti32 %d\nCallGameFunction bettersaver.getbooleanvalue 1\nReturn\n")
    for key, val, probe, want in ((100, 1, 100, 1), (100, 0, 100, 0), (100, 7, 100, 1), (100, 0x100, 100, 1),
                                  (100, -1, 100, 1), (100, 1, 101, 0), (0x10001, 1, 1, 1), (-1, 1, 0xFFFF, 1),
                                  (0x10064, 1, 100, 1), (100, 1, 0x10064, 1)):
        r, vm, ctx, tr, log = run_main(asm % (key, val, probe))
        assert r == Variant(0x10, want), (key, val, probe)
        assert log[0] == "00000000 bettersaver.setbooleanvalue 2 -> 000f:00000000"


def test_setting_false_erases_and_true_inserts():
    d = dr()
    vm, ctx, tr, log = make_vm(".function main 0\nReturn\n")
    bs = ctx.subsystem(d.BetterSaverState)
    bs.set(ctx, 5, True)
    bs.set(ctx, 0x105, True)
    assert bs.bools == {5: True, 0x105: True}
    bs.set(ctx, 5, False)
    assert bs.bools == {0x105: True}                                            # erased, not stored as false
    bs.set(ctx, 5, False)                                                       # erasing an absent key is a no-op
    bs.set(ctx, 0x105, True)                                                    # assigning an existing key
    assert bs.bools == {0x105: True} and bs.get(0x10105) is True                # the key is a u16


def test_boolean_keys_below_32_mirror_into_the_condition_bits():
    d = dr()
    vm, ctx, tr, log = make_vm(".function main 0\nReturn\n")
    bs, sv = ctx.subsystem(d.BetterSaverState), ctx.subsystem(d.Saver)
    assert sv.conditions == 0
    for k in (0, 3, 31):
        bs.set(ctx, k, True)
    assert sv.conditions == (1 | 8 | 1 << 31)
    bs.set(ctx, 3, False)
    assert sv.conditions == (1 | 1 << 31)
    bs.set(ctx, 32, True)                                                       # 0x20: jae 0x100048e6, no bit
    bs.set(ctx, 0x1000, True)
    assert sv.conditions == (1 | 1 << 31)
    bs.set(ctx, 32, False)
    bs.set(ctx, 7, False)                                                       # clearing a bit that was never set
    assert sv.conditions == (1 | 1 << 31)
    # a key above 0xffff is truncated BEFORE the comparison with 0x20 (the cmp is on a word)
    bs.set(ctx, 0x10005, True)
    assert sv.conditions & (1 << 5)


def test_boolean_keys_14_16_18_set_the_quest_bytes_for_the_current_level():
    d = dr()
    for key, bit in ((0xE, 1), (0x10, 2), (0x12, 4)):
        vm, ctx, tr, log = make_vm(".function main 0\nReturn\n")
        bs, sv = ctx.subsystem(d.BetterSaverState), ctx.subsystem(d.Saver)
        sv.level = 3
        bs.set(ctx, key, True)
        assert list(sv.quest) == [0, 0, 0, bit, 0, 0, 0, 0], key
        bs.set(ctx, key, False)                                                 # false never clears a quest bit
        assert sv.quest[3] == bit
    vm, ctx, tr, log = make_vm(".function main 0\nReturn\n")
    bs, sv = ctx.subsystem(d.BetterSaverState), ctx.subsystem(d.Saver)
    for level in (0, 7, 0xFF):                                                  # SetQuest checks the low byte 1..6
        sv.level = level
        bs.set(ctx, 0xE, True)
    assert sv.quest[1:7] == bytearray(6) and sv.quest[3] == 0
    sv.level = 0x103                                                            # cl = 3: the low byte
    sv.set_quest(0x103, 2)
    assert sv.quest[3] == 2
    for key in (0xD, 0xF, 0x11, 0x13):                                          # neighbours of 14, 16, 18
        sv.level = 2
        bs.set(ctx, key, True)
    assert sv.quest[2] == 0


def test_setquest_bounds_asm_and_model(disasm, image):
    """SetQuest 0x5d5e20: level (cl) below 1 (cmp cl,1; jb 0x5d5e24-0x5d5e27) or above 6 (cmp cl,6; ja 0x5d5e29-0x5d5e2c) and
    quest-1 above 2 (unsigned, 0x5d5e36-0x5d5e39) return; the stores are `[0x83b37f + level]`, so level 6 is the last
    byte, quests[7] of the 8-byte window."""
    pin(disasm, 0x5d5e20, 0x5d5e3c, {0x5d5e24: "cmp cl, 0x1", 0x5d5e27: "jb 0x5d5e6a", 0x5d5e29: "cmp cl, 0x6",
                                     0x5d5e2c: "ja 0x5d5e6a", 0x5d5e33: "sub edx, 0x1", 0x5d5e36: "cmp edx, 0x2",
                                     0x5d5e39: "ja 0x5d5e6a"})
    pin(disasm, 0x5d5e42, 0x5d5e6b, {0x5d5e45: "or byte ptr [edx + 0x83b37f], 0x1", 0x5d5e53: "or byte ptr [edx + 0x83b37f], 0x2",
                                     0x5d5e63: "or byte ptr [edx + 0x83b37f], 0x4"})
    d = dr()
    vm, ctx, tr, log = make_vm(".function main 0\nReturn\n")
    bs, sv = ctx.subsystem(d.BetterSaverState), ctx.subsystem(d.Saver)
    sv.level = 0                                                                # frnt's "lev0"
    for key in (0xE, 0x10, 0x12):
        bs.set(ctx, key, True)
    assert sv.quest == bytearray(8)
    sv.level = 6                                                                # the last valid level: quests[6]
    for key in (0xE, 0x10, 0x12):
        bs.set(ctx, key, True)
    assert list(sv.quest) == [0, 0, 0, 0, 0, 0, 7, 0] and len(sv.quest) == 8
    sv.level = 7
    bs.set(ctx, 0xE, True)
    sv.set_quest(0, 1)
    sv.set_quest(6, 0)
    sv.set_quest(6, 4)
    assert list(sv.quest) == [0, 0, 0, 0, 0, 0, 7, 0]                           # level 7, level 0, quest 0 and 4: no write


def test_save_returns_nothing_and_writes_nothing(tmp_path):
    d = dr()
    inst = tmp_path / "install"
    inst.mkdir()
    r, vm, ctx, tr, log = call0("bettersaver.save", install=inst)
    assert r == NOTHING and log[0] == "00000000 bettersaver.save 0 -> 000f:00000000"
    assert ctx.subsystem(d.BetterSaverState).saves == 1
    assert list(inst.iterdir()) == []                                           # saves are M4: no file


def test_bettersaver_rows_are_the_mod_rows_with_the_cited_bodies():
    mods = {r["key"]: r for r in json.loads((REPO / "data" / "osi" / "natives_beta150.json").read_text()) if r["winner"]}
    want = {"bettersaver.getbooleanvalue": (0x10001F70, 1, "int"), "bettersaver.setbooleanvalue": (0x10001FF0, 2, "none"),
            "bettersaver.save": (0x10001F40, 0, "none")}
    for k, (body, argc, ret) in want.items():
        r = mods[k]
        assert (int(r["body"], 16), r["min"], r["max"], r["ret"], r["dll"], r["collides_with_exe"]) == \
            (body, argc, argc, ret, "BetterSaver.dll", False), k
    reg = osi_vm.NativeRegistry.load(osi_vm.DEFAULT_NATIVES, osi_vm.DEFAULT_MODS, TYPES)
    for k in want:
        assert reg.find(*k.split(".")) is not None, k            # the mod rows load into the same registry as the exe rows


# ===============================================================================================================
# 9. gchuddirector.playcinematic   (D1)
# ===============================================================================================================
def test_playcinematic_asm_pins(disasm, image):
    pin(disasm, 0x47b100, 0x47b12e, {0x47b10e: "mov al, byte ptr [edx]", 0x47b112: "call 0x47b820",
                                     0x47b119: "mov word ptr [esi], 0xf", 0x47b11e: "mov dword ptr [esi + 0x4], 0x0"})
    pin(disasm, 0x47b820, 0x47b920, {0x47b862: "call 0x401850", 0x47b867: "cmp byte ptr [0x70a3b0], 0x0",
                                     0x47b86e: "je 0x47b902", 0x47b8ca: "call 0x5e2f40", 0x47b8fa: "call dword ptr [esi + 0x8]",
                                     0x47b8fd: "call 0x47b920"})
    pin(disasm, 0x47b920, 0x47b932, {0x47b923: "mov ecx, dword ptr [0x839110]", 0x47b929: "call 0x5e3050"})
    assert image.read(0x70a3b0, 1) == b"\x01"                                            # movies on in .data
    pin(disasm, 0x5380aa, 0x5380d0, {0x5380aa: "push 0x732fd6", 0x5380bb: "mov byte ptr [0x70a3b0], 0x0"})
    assert image.read(0x732fd6, 9) == b"-nointro\0"


def test_playcinematic_completes_at_once_in_both_video_modes():
    d = dr()
    asm = ('.function main 0\nPushConstantString "Root/Data/cinematics/New_LOGO8015.bik"\nPushConstanti8 1\n'
           'CallGameFunction gchuddirector.playcinematic 2\nReturn\n')
    for skip in (True, False):
        r, vm, ctx, tr, log = run_main(asm, skip_video=skip)
        cin = ctx.subsystem(d.Cinematics)
        assert r == NOTHING and log == ["00000000 gchuddirector.playcinematic 2 -> 000f:00000000"]
        assert cin.requests == [(b"Root/Data/cinematics/New_LOGO8015.bik", True)]
        assert cin.playing is False                                              # instant: nothing is left playing
        assert not any(l.startswith(("C ", "X ", "H ")) for l in log)             # no completion OnEvent (natives.md §4)


def test_playcinematic_flag_and_the_movies_switch():
    d = dr()
    for flag, want in ((0, False), (1, True), (256, False), (257, True), (-1, True)):
        r, vm, ctx, tr, log = run_main('.function main 0\nPushConstantString "a.bik"\nPushConstanti32 %d\n'
                                       'CallGameFunction gchuddirector.playcinematic 2\nReturn\n' % flag)
        assert ctx.subsystem(d.Cinematics).requests == [(b"a.bik", want)], flag      # mov al, byte (0x47b10e)
    vm, ctx, tr, log = make_vm('.function main 0\nPushConstantString "a.bik"\nPushConstanti8 1\n'
                               'CallGameFunction gchuddirector.playcinematic 2\nReturn\n')
    ctx.subsystem(d.Cinematics).enabled = False                                  # -nointro: [0x70a3b0] = 0
    assert vm.call("main", []).ok and ctx.subsystem(d.Cinematics).requests == []


def test_playcinematic_a_255_character_name_is_the_originals_error():
    name = "x" * 255                                                             # assign(npos) throws at len >= 0xff (0x401894)
    vm, ctx, tr, log = make_vm('.function main 0\nPushConstantString "%s"\nPushConstanti8 1\n'
                               'CallGameFunction gchuddirector.playcinematic 2\nReturn\n' % name)
    r = vm.call("main", [])
    assert not r.ok and "Script is calling gchuddirector.playcinematic" in vm.last_error.message
    ok = "x" * 254
    vm, ctx, tr, log = make_vm('.function main 0\nPushConstantString "%s"\nPushConstanti8 1\n'
                               'CallGameFunction gchuddirector.playcinematic 2\nReturn\n' % ok)
    assert vm.call("main", []).ok


def test_the_mutation_bite_a_pending_cinematic_is_caught(monkeypatch):
    """The falsifier of D1: a 'not instant' model (the player left playing) must fail the instant-completion check."""
    d = dr()

    def check_instant(ctx):
        cin = ctx.subsystem(d.Cinematics)
        assert cin.playing is False, "the cinematic is still playing after the call returned"

    asm = ('.function main 0\nPushConstantString "a.bik"\nPushConstanti8 1\n'
           'CallGameFunction gchuddirector.playcinematic 2\nReturn\n')
    r, vm, ctx, tr, log = run_main(asm)
    check_instant(ctx)                                                           # the real model passes

    def not_instant(ctx_):
        def fn(call):
            ctx_.subsystem(d.Cinematics).playing = True                          # the mutant: completion deferred
            return NOTHING
        return fn
    monkeypatch.setattr(d, "_play_cinematic", not_instant)
    vm, ctx, tr, log = make_vm(asm)
    assert vm.call("main", []).ok
    with pytest.raises(AssertionError, match="still playing"):
        check_instant(ctx)


# ===============================================================================================================
# 10. the tier2 rows and the A9 path with the real models (needs the install)
# ===============================================================================================================
def test_tier2_rows_name_their_bodies():
    rows = {r["key"]: r for r in json.loads((REPO / "data" / "osi" / "natives.json").read_text()) if r["winner"]}
    for k, body in dr().TIER2.items():
        assert int(rows[k]["body"], 16) == body, k


def _a9(tmp_path, lomn_data, extra=()):
    out = tmp_path / "n.log"
    tr = tmp_path / "n.trace"
    py = [sys.executable, str(REPO / "tools" / "research" / "osi_vm.py"), "--run",
          str(lomn_data / "install" / "data" / "Base.osi"), "--frames", "600", "--keys",
          "down@100 down@160 enter@220 esc@280", "--real-natives", "--install", str(lomn_data / "install"),
          "--overrides", str(OVERRIDES), "--log-out", str(out), "--trace", str(tr)] + list(extra)
    p = subprocess.run(py, capture_output=True, text=True, timeout=110)
    assert p.returncode == 0, p.stderr
    return p.stdout, out.read_text().splitlines(), tr


def test_a9_path_with_the_directors_models(tmp_path, lomn_data):
    stdout, log, tr = _a9(tmp_path, lomn_data)
    assert "stop: none" in stdout, stdout
    by = {}
    for l in log:
        m = re.match(r"^\d+ (\S+) \d+ -> (.*)$", l)
        if m:
            by.setdefault(m.group(1), []).append(m.group(2))
    # first-hit values of the A9 path, from this run
    assert by["gcgame.getmainview"] == [re.match(r"0028:0000[0-9a-f]{4} G 1", by["gcgame.getmainview"][0]).group(0)]
    assert re.match(r"^003e:[0-9a-f]{8} G 1$", by["gcgame.getkeymapper"][0])
    assert re.match(r"^003f:[0-9a-f]{8} G 1$", by["gcgame.getmp3player"][0])
    assert re.match(r"^002a:[0-9a-f]{8} G 1$", by["gcviewport.getcamera"][0])
    assert by["gcareadirector.getcurrentlevel"] == ["0010:6c657630"]                 # lev0
    assert by["gcsaver.getammo"] == ["0010:00000000"] and by["gcsaver.cheatmenu"] == ["0010:00000000"]
    assert by["bettersaver.getbooleanvalue"] == ["0010:00000000"]
    assert by["gcareadirector.setcurrentarea"] == ["000f:00000000"] == by["gcareadirector.setcurrentlevel"]
    assert len(by["gchuddirector.playcinematic"]) == 3
    # the fluff strings are the file's text, not the stub's empty string: each equals the independent reader's
    fl = [l for l in log if " gcareadirector.getscriptfluffstring " in l]
    assert len(fl) == 23
    ns = [l for l in tr.read_text().splitlines() if l.startswith("N ") and " gcareadirector.getscriptfluffstring " in l]
    assert len(ns) == len(fl)
    inst = lomn_data / "install"
    for n_line, l in zip(ns, fl):
        tok = n_line.split()
        a, b, c = (int(t.split(":")[1], 16) for t in tok[4:7])
        want = read_fluff_independently(inst, a, b, c) or b"XXX"
        assert l.endswith(" S %d %s" % (len(want), want.hex())), (n_line, l)
    # this group makes no engine call and no reference change on the path
    assert not any(l.startswith(("C ", "X ", "H ")) for l in log)


def test_a9_path_is_deterministic(tmp_path, lomn_data):
    (tmp_path / "a").mkdir()
    (tmp_path / "b").mkdir()
    a = _a9(tmp_path / "a", lomn_data)
    b = _a9(tmp_path / "b", lomn_data)
    assert a[1] == b[1] and a[2].read_text() == b[2].read_text()
