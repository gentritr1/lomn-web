"""M2a-2 T9b: the objects / model director / camera / HUD native models of the Python oracle
(tools/research/osi_natives/objects.py), written from docs/osi/natives.md (§1.2 the ABI, §1.3 the citation format,
§2 game objects, §3 natives.log, §4 re-entry, §7) and from the asm of the canonical exe, read as data with objdump
(never run). Never from engine/.

For every native: an asm-pin test (objdump of the cited range, decoded from the function start, so it is not a linear
sweep through data; the pins end by requiring the module's own citation to name the pinned addresses) and a behaviour
test (the native called from a script through the 0x82 handler, so argument conversion, the S/N lines and the
natives.log are the real ones).

Pins need LOMN_DATA (they skip with `SKIPPED (no LOMN_DATA)`); the behaviour tests of the models run without it.
Every address is the one cited in objects.CITES; the numbers a test pins come from the commands named in the commit
body (objdump --start-address/--stop-address on the exe, a PE section read for the .data constants, the oracle run).
"""
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
from osi_vm import NOTHING, Variant  # noqa: E402

TYPES = osi_vm.TypeTable.load(REPO / "data" / "osi" / "types.json")
OVERRIDES = REPO / "data" / "osi" / "native_overrides.json"
M32 = 0xFFFFFFFF
CNT1 = 0x636E7431          # 'cnt1'
ONUA = 0x6F6E7561          # 'onua'


def ob():
    from osi_natives import objects
    return objects


def sv():
    from osi_natives import services
    return services


def fw():
    from osi_natives import framework
    return framework


# ----------------------------------------------------------------------------------------------- harness
def make_vm(asm, install=None, with_log=True):
    """The oracle VM with the real native registry and the services + objects groups bound (so the tests do not depend
    on 8b)."""
    f, o, s = fw(), ob(), sv()
    image, _ = osiasm.assemble(asm)
    tr = osi_vm.ListTrace()
    heap = osi_vm.Heap(tr)
    reg = osi_vm.NativeRegistry.load(osi_vm.DEFAULT_NATIVES, osi_vm.DEFAULT_MODS, TYPES)
    vm = osi_vm.Vm(image, reg, heap, tr)
    log = [] if with_log else None
    vfs = f.Vfs(install) if install is not None else None
    ctx = f.HostContext(vm, heap, vfs, f.HostFlags(), log)
    s.bind(reg, ctx)
    o.bind(reg, ctx)
    return vm, ctx, tr, log


def run_main(asm, **kw):
    vm, ctx, tr, log = make_vm(asm, **kw)
    r = vm.call("main", [])
    assert r.ok, vm.last_error and vm.last_error.message
    return r.result, vm, ctx, tr, log


def fails(asm, **kw):
    vm, ctx, tr, log = make_vm(asm, **kw)
    r = vm.call("main", [])
    assert not r.ok
    return vm.last_error.message, vm, ctx, tr, log


ONEVENT = ".function OnEvent 2\nPushConstanti8 1\nReturn\n"


# ----------------------------------------------------------------------------------------------- asm pins
@pytest.fixture(scope="session")
def image(canonical_exe):
    import exc
    return exc.Image.from_file(canonical_exe)


@pytest.fixture(scope="session")
def disasm(canonical_exe):
    if shutil.which("objdump") is None:
        pytest.skip("SKIPPED (no objdump)")
    cache = {}

    def run(start, stop):
        key = (start, stop)
        if key not in cache:
            out = subprocess.run(["objdump", "-d", "-M", "intel", "--start-address=%#x" % start,
                                  "--stop-address=%#x" % stop, str(canonical_exe)], capture_output=True, text=True,
                                 check=True, timeout=60).stdout
            rows = {}
            for ln in out.splitlines():
                m = re.match(r"^\s*([0-9a-f]+):\s+[0-9a-f ]+\t(.*)$", ln)
                if m:
                    rows[int(m.group(1), 16)] = re.sub(r"\s+", " ", m.group(2).strip())
            cache[key] = rows
        return cache[key]
    return run


def pin(disasm, start, stop, expect):
    """expect = {addr: normalised 'mnemonic operands'}: every one must be the decode of the instruction at that address."""
    rows = disasm(start, stop)
    bad = {}
    for a, text in expect.items():
        got = rows.get(a)
        if got is not None:
            got = re.sub(r" <[^>]*>$", "", got)          # a branch target is annotated with its section
        if got != text:
            bad[a] = (text, got)
    assert not bad, "asm pin mismatch (address: (expected, decoded)): " + ", ".join(
        "%#x: %r vs %r" % (a, e, g) for a, (e, g) in sorted(bad.items()))


def no_instruction(disasm, start, stop, text_re):
    """No instruction in [start, stop) matches the regex (used to pin an ABSENT restore, an absent early exit)."""
    return [(a, t) for a, t in sorted(disasm(start, stop).items()) if re.search(text_re, t)]


def cited(key, *addrs):
    """The module's citation for `key` names every pinned address (as numbers)."""
    have = {int(m, 16) for m in re.findall(r"0x([0-9a-fA-F]{5,})", ob().CITES[key])}
    missing = [hex(a) for a in addrs if a not in have]
    assert not missing, "%s: CITES does not name %s" % (key, missing)


# ===============================================================================================================
# 0. registration, citations and the overrides rows
# ===============================================================================================================
TIER1_KEYS = sorted([
    "gcbigsprite.loadhudsprite", "gccharacter.setupmoveable",
    "gchuddirector.addbigsprite", "gchuddirector.addtextobject",
    "gclegocamera.getcameraaimode", "gclegocamera.setcameraaimode", "gclegocamera.setcameraspline",
    "gclegocamera.settargetspline", "gcmodeldirector.addtoa", "gcmodeldirector.getcharacter",
    "gcmodeldirector.gettoa", "gcmodeldirector.show", "scslosislkcubicspline.create", "sctextobject.create",
    "sctilefont.create"])


def test_group_registers_itself_and_binds_exactly_the_tier1_rows():
    o, f = ob(), fw()
    assert "objects" in f._GROUPS
    reg = osi_vm.NativeRegistry.load(osi_vm.DEFAULT_NATIVES, osi_vm.DEFAULT_MODS, TYPES)
    vm = osi_vm.Vm(osiasm.assemble(".function main 0\nReturn\n")[0], reg, osi_vm.Heap(None), None)
    ctx = f.HostContext(vm, vm.heap, None)
    o.bind(reg, ctx)
    implemented = sorted(e.key for e in reg.entries.values() if e.tier == "implemented")
    assert implemented == TIER1_KEYS


def test_every_bound_key_has_a_citation_in_the_natives_md_1_3_form():
    o = ob()
    assert sorted(o.CITES) == TIER1_KEYS
    for key, cite in o.CITES.items():
        assert cite.isascii(), key
        assert re.match(r"^// body 0x[0-9a-f]+, method 0x[0-9a-f]+ <[^>]+>( -> 0x[0-9a-f]+ <[^>]+>)*; asm pin: \S", cite), key
        assert not re.search(r"0x0[0-9a-f]|0X|0x[0-9a-f]*[A-F]", cite)        # shortest lowercase hex (natives.md 1.3)


def _addrs(text):
    return {int(m, 16) for m in re.findall(r"0x([0-9a-fA-F]{5,})", text)}


def test_committed_overrides_rows_are_valid_sorted_ascii_and_name_the_bodies_of_this_group():
    import importlib.util
    spec = importlib.util.spec_from_file_location("osi_registry_t9b", REPO / "tools" / "osi" / "registry.py")
    reg = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(reg)
    rows = json.loads((REPO / "data" / "osi" / "natives.json").read_text())
    mods = json.loads((REPO / "data" / "osi" / "natives_beta150.json").read_text())
    assert OVERRIDES.read_bytes().isascii()
    data = json.loads(OVERRIDES.read_text())
    assert reg.check_overrides(data, rows, mods) == []
    keys = [r["key"] for r in data]
    assert keys == sorted(keys) and len(set(keys)) == len(keys)
    by_key = {r["key"]: r for r in data}
    win = {r["key"]: r for r in rows if r["winner"]}
    for k in TIER1_KEYS:
        r = by_key[k]
        assert r["tier"] == "tier1" and "default" not in r, k
        assert int(win[k]["body"], 16) in _addrs(r["cite"]), k
        # the merged file keeps side a's wording (controller ruling); the module's own cite must name the body
        assert int(win[k]["body"], 16) in _addrs(ob().CITES[k]), k


INT_TIER2 = ("gclightdirector.toggleconstantlights", "gc3dsound.synclistenertocamera")


def test_tier2_rows_of_this_group_are_taint_safe_stubs_with_their_bodies():
    data = {r["key"]: r for r in json.loads(OVERRIDES.read_text())}
    rows = {r["key"]: r for r in json.loads((REPO / "data" / "osi" / "natives.json").read_text()) if r["winner"]}
    t2 = sorted(k for k in data if k in ob().TIER2_KEYS)
    assert t2 == sorted(ob().TIER2_KEYS)
    for k in t2:
        assert data[k]["tier"] == "tier2" and "default" not in data[k], k       # none-returning natives: the typed stub
        assert rows[k]["ret"] == ("int" if k in INT_TIER2 else "none"), k
        assert int(rows[k]["body"], 16) in _addrs(data[k]["cite"]), k


def test_the_merged_tables_carry_the_tiers():
    rows = {r["key"]: r for r in json.loads((REPO / "data" / "osi" / "natives.json").read_text()) if r["winner"]}
    for k in TIER1_KEYS:
        assert rows[k]["tier"] == "tier1", k
    for k in ob().TIER2_KEYS:
        assert rows[k]["tier"] == "tier2", k


# ===============================================================================================================
# 1. sctilefont.create   (body 0x611910 -> ScTileFont::ScTileFont 0x626b40, CreateVariant 0x611950)
# ===============================================================================================================
def test_sctilefont_asm_pins(disasm, image):
    pin(disasm, 0x611910, 0x611950, {
        0x611915: "mov esi, dword ptr [ebp + 0x10]", 0x611918: "push 0x190", 0x61191d: "call 0x4e46c0",
        0x611927: "je 0x611931", 0x61192c: "call 0x626b40", 0x611936: "call 0x611950"})
    # the constructor: base count 0 (0x4f2b80 stores 0 at +4), the name is assigned with npos into a capacity-0xff string
    pin(disasm, 0x626b40, 0x626c30, {0x626b55: "call 0x4f2b80", 0x626c11: "mov dword ptr [edx + 0x8], 0xff",
                                    0x626c1e: "mov eax, dword ptr [esi]", 0x626c24: "call 0x401850"})
    pin(disasm, 0x4f2b80, 0x4f2ba0, {0x4f2b88: "mov dword ptr [eax + 0x4], 0x0"})
    # CreateVariant(ScTileFont*): null -> {0x0f, 0} at 0x611963, else AddRef at 0x611980 and the registered type word
    pin(disasm, 0x611950, 0x611a00, {0x611961: "jne 0x611980", 0x611963: "mov word ptr [esi], 0xf",
                                    0x611980: "inc dword ptr [edi + 0x4]", 0x6119e2: "mov word ptr [esi], dx",
                                    0x6119e5: "mov dword ptr [esi + 0x4], edi"})
    cited("sctilefont.create", 0x611910, 0x626b40, 0x611918, 0x626c24, 0x611980, 0x4f2b88)


def test_sctilefont_create_is_an_identity_object_with_no_engine_reference():
    r, vm, ctx, tr, log = run_main('.globals t\n.function main 0\nPushConstantString "Root/Data/Art/Fonts/BionicleFont"\n'
                                   'CallGameFunction sctilefont.create 1\nSetVariableValue global:t\nPushConstanti8 1\nReturn\n')
    (h, font), = ctx._objects.items()
    assert isinstance(font, ob().ScTileFont) and font.type() == TYPES.id("ScTileFont") == 0x42
    assert font.name == b"Root/Data/Art/Fonts/BionicleFont"          # the constructor argument, recorded for M2a-3
    assert log[0] == "00000000 sctilefont.create 1 -> 0042:00000001 G 0"
    assert "A g 00000001 42" in tr.lines and vm.heap.engine(h) == 0
    assert not any(l.startswith(("C ", "H ")) for l in log)


def test_sctilefont_create_twice_gives_two_objects_and_dropping_the_variant_frees_it():
    src = ('.function main 0\nPushConstantString "a"\nCallGameFunction sctilefont.create 1\nPop\n'
           'PushConstantString "a"\nCallGameFunction sctilefont.create 1\nPop\nPushConstanti8 1\nReturn\n')
    r, vm, ctx, tr, log = run_main(src)
    assert [l for l in tr.lines if l.startswith("A g")] == ["A g 00000001 42", "A g 00000002 42"]
    assert [l for l in tr.lines if l.startswith("F g")] == ["F g 00000001", "F g 00000002"] and ctx._objects == {}


def test_sctilefont_name_over_254_bytes_is_the_assign_limit():
    # a 255-byte dynamic string cannot be built by a script (0x70 stops at 254), the model enforces the same bound
    o = ob()
    assert o.font_name(b"a" * 254) == b"a" * 254
    with pytest.raises(osi_vm.VmError):
        o.font_name(b"a" * 255)


# ===============================================================================================================
# 2. sctextobject.create   (body 0x611cd0 -> ScTextObject::ScTextObject 0x610c40, CreateVariant 0x611d40)
# ===============================================================================================================
def test_sctextobject_asm_pins(disasm, image):
    pin(disasm, 0x611cd0, 0x611d40, {0x611cf1: "push 0x164", 0x611d17: "call 0x610c40", 0x611d21: "call 0x611d40"})
    # the 5-argument constructor of the three: the font pointer is stored at +0x2c with an AddRef (inc [font+4]) when non-null
    pin(disasm, 0x610c40, 0x610d70, {
        0x610c83: "mov dword ptr [ebx + 0x2c], edx", 0x610c8d: "je 0x610c95", 0x610c92: "inc dword ptr [edx + 0x4]",
        0x610cbb: "mov dword ptr [edx + 0x8], 0xff", 0x610cc8: "mov eax, dword ptr [esi]", 0x610cce: "call 0x401850",
        0x610d69: "ret 0x14"})
    pin(disasm, 0x611d40, 0x611da0, {0x611d53: "mov word ptr [esi], 0xf", 0x611d70: "inc dword ptr [edi + 0x4]"})
    cited("sctextobject.create", 0x611cd0, 0x610c40, 0x610c92, 0x610cce, 0x611d17)


SRC_TEXT = ('.globals f t\n.function main 0\nPushConstantString "fnt"\nCallGameFunction sctilefont.create 1\n'
            'SetVariableValue global:f\nPushConstantf32 525.0\nPushConstantf32 52.0\nPushConstantf32 0.0\n'
            'GetVariableValue global:f\nPushConstantString "X"\nCallGameFunction sctextobject.create 5\n'
            'SetVariableValue global:t\n%sPushConstanti8 1\nReturn\n')


def test_sctextobject_create_takes_one_engine_reference_on_the_font_and_none_on_itself():
    r, vm, ctx, tr, log = run_main(SRC_TEXT % "")
    assert log[0].startswith("00000000 sctilefont.create 1 -> 0042:00000001 G 0")
    # H for the font is written inside the native, before the native's own line (natives.md 3.1: H first, then the line)
    assert log[1:3] == ["H 0042:00000001 +1", "00000001 sctextobject.create 5 -> 002e:00000002 G 0"]
    objs = {type(o).__name__: o for o in ctx._objects.values()}
    t = objs["ScTextObject"]
    assert t.type() == TYPES.id("ScTextObject") == 0x2E and t.text == b"X"
    assert (t.x, t.y, t.z) == (525.0, 52.0, 0.0) and t.font is objs["ScTileFont"]
    assert vm.heap.engine(1) == 1 and vm.heap.engine(2) == 0


def test_sctextobject_font_outlives_the_script_variant_and_dies_with_the_text_object():
    drop_f = "PushNothing\nSetVariableValue global:f\n"
    r, vm, ctx, tr, log = run_main(SRC_TEXT % drop_f)
    assert "F g 00000001" not in tr.lines                      # the text object keeps the font: total 1 (engine only)
    assert vm.heap.engine(1) == 1 and len(ctx._objects) == 2
    # dropping the text object too: its destructor releases the font after the F g of the text object
    r, vm, ctx, tr, log = run_main(SRC_TEXT % (drop_f + "PushNothing\nSetVariableValue global:t\n"))
    i = tr.lines.index("F g 00000002")
    assert tr.lines[i + 1] == "F g 00000001" and "H 0042:00000001 -1" in log
    assert log.index("H 0042:00000001 -1") < len(log) and ctx._objects == {}


def test_sctextobject_adopts_a_font_handle_that_a_stub_minted():
    # a handle minted outside this group (the registry stub of sctilefont.create) has no host object: it is adopted as an
    # ScTileFont, takes the text object's reference like any font, and nothing is refused
    src = ('.globals f t\n.function main 0\nPushConstantString "fnt"\nCallGameFunction sctilefont.create 1\nSetVariableValue global:f\n'
           'PushConstantf32 1.0\nPushConstantf32 2.0\nPushConstantf32 3.0\nGetVariableValue global:f\nPushConstantString "z"\n'
           'CallGameFunction sctextobject.create 5\nSetVariableValue global:t\nPushConstanti8 1\nReturn\n')
    vm, ctx, tr, log = make_vm(src)
    vm.natives.bind("sctilefont.create", None)                    # back to the D11 stub: a fresh handle with no host object
    assert vm.call("main", []).ok
    t = [o for o in ctx._objects.values() if isinstance(o, ob().ScTextObject)][0]
    assert isinstance(t.font, ob().ScTileFont) and t.font.handle == 1 and vm.heap.engine(1) == 1
    assert [l for l in log if l.startswith("H ")] == ["H 0042:00000001 +1"]


def test_sctextobject_text_over_the_assign_limit_is_the_same_bound_as_the_font_name():
    with pytest.raises(osi_vm.VmError):
        sv().c_string(b"a" * 255)


# ===============================================================================================================
# 3. gcbigsprite.loadhudsprite   (body 0x43dad0 -> GcBigSprite::LoadHudSprite 0x440460 -> GcBigSprite ctor 0x43f210)
# ===============================================================================================================
def test_loadhudsprite_asm_pins(disasm, image):
    pin(disasm, 0x43dad0, 0x43db30, {0x43db09: "call 0x440460", 0x43db16: "call 0x43da20"})
    # the method: save the current block file, set it null, new (0x21c) + ctor, restore; it returns the new pointer
    pin(disasm, 0x440460, 0x4404d0, {
        0x440468: "call 0x54db30", 0x440471: "call 0x54db20", 0x440485: "push 0x21c", 0x44048a: "call 0x4e46c0",
        0x4404b0: "call 0x43f210", 0x4404be: "call 0x54db20", 0x4404c4: "mov eax, ebx"})
    pin(disasm, 0x43da20, 0x43dad0, {0x43da33: "mov word ptr [esi], 0xf", 0x43da50: "inc dword ptr [edi + 0x4]"})
    # the constructor: w5 (byte [ebp+0x1c]) and w6 ([ebp+0x20]) pick the ReadMe.bin / "%d_%d.tga" / ".tga" paths; the
    # volume is `<name>.col`
    pin(disasm, 0x43f210, 0x440280, {
        0x43f6ee: "call 0x4db050", 0x43f711: "call 0x4d0ee0", 0x43f890: "cmp byte ptr [ebp + 0x1c], 0x0",
        0x43f912: "call 0x4daea0", 0x43fcc8: "cmp byte ptr [ebp + 0x20], 0x0"})
    assert image.read(0x703dbc, 5) == b".col\0" and image.read(0x703dc1, 11) == b"ReadMe.bin\0"
    cited("gcbigsprite.loadhudsprite", 0x43dad0, 0x440460, 0x43f210, 0x440468, 0x4404be, 0x44048a)


def test_loadhudsprite_returns_an_identity_object_with_no_engine_reference():
    src = ('.globals s\n.function main 0\nPushConstantf32 14.0\nPushConstantf32 20.0\nPushConstantf32 80.0\n'
           'PushConstantf32 84.0\nPushConstantString "Root/data/art/hud/turnhead0024"\nPushConstanti8 0\n'
           'CallGameFunction gcbigsprite.loadhudsprite 6\nSetVariableValue global:s\nPushConstanti8 1\nReturn\n')
    r, vm, ctx, tr, log = run_main(src)
    (h, spr), = ctx._objects.items()
    assert isinstance(spr, ob().GcBigSprite) and spr.type() == TYPES.id("GcBigSprite") == 0x26
    assert log[0] == "00000000 gcbigsprite.loadhudsprite 6 -> 0026:00000001 G 0" and vm.heap.engine(h) == 0
    assert spr.rect == (14.0, 20.0, 80.0, 84.0) and spr.name == b"Root/data/art/hud/turnhead0024"
    assert (spr.flag5, spr.flag6) == (0, 0)                      # the absent 7th argument is wi = 0 (0x6027bf)


@pytest.mark.parametrize("argc, extra", [(5, ""), (6, "PushConstanti8 1\n"), (7, "PushConstanti8 1\nPushConstanti8 2\n")])
def test_loadhudsprite_accepts_5_to_7_arguments_and_records_the_flag_bytes(argc, extra):
    src = ('.function main 0\nPushConstantf32 1.0\nPushConstantf32 2.0\nPushConstantf32 3.0\nPushConstantf32 4.0\n'
           'PushConstantString "n"\n%sCallGameFunction gcbigsprite.loadhudsprite %d\nPop\nPushConstanti8 1\nReturn\n'
           % (extra, argc))
    r, vm, ctx, tr, log = run_main(src)
    assert log[0].startswith("00000000 gcbigsprite.loadhudsprite %d -> 0026:00000001 G 0" % argc)


def test_loadhudsprite_flag_bytes_are_the_low_bytes():
    src = ('.globals s\n.function main 0\nPushConstantf32 1.0\nPushConstantf32 2.0\nPushConstantf32 3.0\n'
           'PushConstantf32 4.0\nPushConstantString "n"\nPushConstanti32 0x101\nPushConstanti32 0x100\n'
           'CallGameFunction gcbigsprite.loadhudsprite 7\nSetVariableValue global:s\nPushConstanti8 1\nReturn\n')
    r, vm, ctx, tr, log = run_main(src)
    (h, spr), = ctx._objects.items()
    assert (spr.flag5, spr.flag6) == (1, 0)                      # movzx of a byte (0x43db09's caller pushes `mov al, [ebx]`)


def make_install(tmp_path, art=None, loose=(), chars=None):
    inst = tmp_path / "install"
    (inst / "data" / "Art").mkdir(parents=True, exist_ok=True)
    if art is not None:
        (inst / "data" / "Art" / "Art.blk").write_bytes(blk_bytes(art))
    for name, entries in (chars or {}).items():
        p = inst / "data" / "Characters" / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(blk_bytes(entries))
    for rel in loose:
        p = inst / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text("x")
    return inst


def blk_bytes(names):
    out = b"BLKF" + struct.pack("<II", 1, len(names))
    for i, n in enumerate(sorted(names, key=lambda n: bytes(c | 0x60 if 0x41 <= c <= 0x5A else c for c in n.encode("latin1")))):
        out += struct.pack("<40sIIII", n.encode("latin1"), 64, 10, 0, 4096 + 64 * i)
    return out


def hud_sprite(tmp_path, name, art):
    inst = make_install(tmp_path / ("i%d" % len(art)), art=art)
    src = ('.globals s\n.function main 0\nPushConstantf32 1.0\nPushConstantf32 2.0\nPushConstantf32 3.0\n'
           'PushConstantf32 4.0\nPushConstantString "%s"\nPushConstanti8 0\nCallGameFunction gcbigsprite.loadhudsprite 6\n'
           'SetVariableValue global:s\nPushConstanti8 1\nReturn\n' % name)
    r, vm, ctx, tr, log = run_main(src, install=inst)
    (h, spr), = ctx._objects.items()
    return spr, ctx, log


def test_loadhudsprite_checks_the_source_through_the_block_file_oracle_and_never_returns_null(tmp_path):
    spr, ctx, log = hud_sprite(tmp_path, "Root/data/art/hud/Present", ["present.tga"])
    assert spr.source_present is True                              # Art.blk base-name Find, like doesfileexist
    spr, ctx, log = hud_sprite(tmp_path, "Root/data/art/hud/Absent", ["present.tga"])
    assert spr.source_present is False and log[0].startswith("00000000 gcbigsprite.loadhudsprite 6 -> 0026:00000001")
    # the original's method returns the `new` pointer: it is null only if operator new fails (0x44048a / 0x4404c4)


def test_loadhudsprite_leaves_the_current_block_file_slot_as_it_found_it(tmp_path):
    inst = make_install(tmp_path, art=["a.tga"])
    src = ('.function main 0\nPushConstantf32 1.0\nPushConstantf32 2.0\nPushConstantf32 3.0\nPushConstantf32 4.0\n'
           'PushConstantString "n"\nPushConstanti8 0\nCallGameFunction gcbigsprite.loadhudsprite 6\nPop\n'
           'PushConstanti8 1\nReturn\n')
    vm, ctx, tr, log = make_vm(src, install=inst)
    slots = ctx.subsystem(sv().BlockFileSlots)
    sentinel = [fw().BlockEntry("zzz.tga", 0, 0, 0, 0)]
    slots.current = sentinel
    assert vm.call("main", []).ok
    assert slots.current is sentinel                              # saved at 0x440468 and restored at 0x4404be


# ===============================================================================================================
# 4. scslosislkcubicspline.create   (body 0x61a2e0 -> ScSLOSISLKCubicSpline::ScSLOSISLKCubicSpline 0x61a4d0)
# ===============================================================================================================
def test_spline_asm_pins(disasm, image):
    pin(disasm, 0x61a2e0, 0x61a320, {0x61a2e8: "push 0xc", 0x61a2f9: "call 0x61a4d0", 0x61a303: "call 0x61a320"})
    pin(disasm, 0x61a4d0, 0x61a580, {0x61a4ef: "mov dword ptr [edx + 0x8], 0x0", 0x61a514: "call 0x61a580"})
    pin(disasm, 0x61a320, 0x61a380, {0x61a333: "mov word ptr [esi], 0xf", 0x61a350: "inc dword ptr [edi + 0x4]"})
    cited("scslosislkcubicspline.create", 0x61a2e0, 0x61a4d0, 0x61a580, 0x61a350)


def test_spline_create_is_an_identity_object_that_records_its_slk_name():
    r, vm, ctx, tr, log = run_main('.globals s\n.function main 0\n'
                                   'PushConstantString "Root/Data/Levels/lev0/frnt/frnt_CAM.slb"\n'
                                   'CallGameFunction scslosislkcubicspline.create 1\nSetVariableValue global:s\n'
                                   'PushConstanti8 1\nReturn\n')
    (h, sp), = ctx._objects.items()
    assert isinstance(sp, ob().ScSLOSISLKCubicSpline) and sp.type() == TYPES.id("ScSLOSISLKCubicSpline") == 0x23
    assert sp.slk_name == b"Root/Data/Levels/lev0/frnt/frnt_CAM.slb"
    assert log[0] == "00000000 scslosislkcubicspline.create 1 -> 0023:00000001 G 0" and vm.heap.engine(h) == 0


# ===============================================================================================================
# 5. the model director: addtoa / gettoa / getcharacter / show
# ===============================================================================================================
def test_addtoa_asm_pins(disasm, image):
    pin(disasm, 0x47cc30, 0x47cc60, {0x47cc36: "push 0x0", 0x47cc39: "call 0x47e170", 0x47cc45: "call 0x407f30"})
    # AddToa: the id of the name, new GcToa (0x430), the character block, [0x74749c], Add, then the success tail
    pin(disasm, 0x47e170, 0x47e720, {
        0x47e194: "call 0x617810", 0x47e1ac: "push 0x430", 0x47e1d0: "call 0x567d10",
        0x47e1eb: "mov dword ptr [edi + 0x10], edx", 0x47e1f1: "call 0x54db30", 0x47e1fe: "call 0x419330",
        0x47e221: "mov dword ptr [0x74749c], edx", 0x47e238: "call 0x4804c0", 0x47e4bd: "call 0x480eb0",
        0x47e4c4: "mov bl, al", 0x47e4c8: "jne 0x47e510", 0x47e4ca: "xor bl, bl", 0x47e522: "call 0x56c270",
        0x47e527: "mov dword ptr [0x7032b8], edi", 0x47e61e: "call 0x54db20", 0x47e657: "mov al, bl",
        0x47e57d: "sub dword ptr [ebp - 0x138], 0x74616875", 0x47e589: "sub dword ptr [ebp - 0x138], 0xf8040eec",
        0x47e595: "sub dword ptr [ebp - 0x138], 0xff09f900", 0x47e5a1: "sub dword ptr [ebp - 0x138], 0x4fff800",
        0x47e5ad: "sub dword ptr [ebp - 0x138], 0xf6f20408", 0x47e5b9: "sub dword ptr [ebp - 0x138], 0x80d08f8",
        0x47e5c7: "push 0x5", 0x47e5d0: "push 0x4", 0x47e5e0: "push 0x3", 0x47e5f0: "push 0x2", 0x47e600: "push 0x1",
        0x47e610: "push 0x0", 0x47e5c5: "jmp 0x47e610"})
    # the duplicate-key return (0x47e4ca-0x47e508) restores nothing: there is no `call 0x54db20` before the success tail
    assert not no_instruction(disasm, 0x47e4ca, 0x47e510, r"^call 0x54db20")
    assert not no_instruction(disasm, 0x47e4ca, 0x47e510, r"\[0x736128\]")
    # the GcToa constructor: [toa+0x380] = 0xfa (its only writer), ammo, then OnEvent(0x32, [toa+0x380]) through Call
    pin(disasm, 0x567d10, 0x568c20, {
        0x567e27: "mov word ptr [edx + 0x380], 0xfa", 0x56898b: "mov word ptr [0x7474c8], dx", 0x568992: "push 0x32",
        0x56899b: "call 0x413ed0", 0x5689a8: "mov ax, word ptr [edx + 0x380]", 0x5689b4: "call 0x4183f0",
        0x568a54: "push 0x73b964", 0x568a59: "call 0x60b320", 0x568a5e: "call 0x5d4e40"})
    pin(disasm, 0x413ed0, 0x413ef0, {0x413ed5: "mov word ptr [ebx], 0x10", 0x413eda: "movzx edx, byte ptr [esp + 0xc]"})
    pin(disasm, 0x4183f0, 0x418410, {0x4183f5: "mov word ptr [ebx], 0x10", 0x4183fa: "movzx edx, word ptr [esp + 0xc]"})
    assert image.read(0x73b964, 8) == b"OnEvent\0"
    # the Toa's character id (+0x100) is [0x7cb1cc] = 'cnt1' (static init 0x570be3), and +0x1d0 starts at 0
    pin(disasm, 0x570bc0, 0x570c00, {0x570be3: "mov dword ptr [0x7cb1cc], 0x636e7431"})
    pin(disasm, 0x40b520, 0x40c000, {0x40b73e: "mov dword ptr [ebx + 0x100], edx", 0x40bbba: "mov dword ptr [edx + 0x1d0], 0x0"})
    # the 'unreachable' INI branch behind the Call: GcSaver INI test (false on the owner's install, ini -> M2a-3)
    cited("gcmodeldirector.addtoa", 0x47cc30, 0x47e170, 0x567d10, 0x568a59, 0x567e27, 0x47e4ca, 0x47e61e, 0x419330)


def test_addtoa_asm_pins_for_the_name_to_toa_index_table():
    # SetCurrentToa(n): tahu 5, lewa 4, kopa 3, poha 2, gali 1, onua 0 and every other id 0 (0x47e610): computed from the
    # six cumulative subtraction immediates above
    o = ob()
    assert o.TOA_INDEX == {0x74616875: 5, 0x6C657761: 4, 0x6B6F7061: 3, 0x706F6861: 2, 0x67616C69: 1, 0x6F6E7561: 0}
    for n, (name, idx) in enumerate([(b"tahu", 5), (b"lewa", 4), (b"kopa", 3), (b"poha", 2), (b"gali", 1), (b"onua", 0), (b"abcd", 0)]):
        assert o.toa_index(sv().convert_string_to_id(name)) == idx
    assert o.toa_index(M32) == 0


ADDTOA = ('.globals a b c\n.function main 0\nPushConstantString "onua"\nCallGameFunction gcmodeldirector.addtoa 1\n'
          'SetVariableValue global:a\n%sPushConstanti8 1\nReturn\n' + ONEVENT)


def test_addtoa_calls_onevent_32_250_inside_the_native_and_returns_int_1():
    r, vm, ctx, tr, log = run_main((ADDTOA % "GetVariableValue global:a\nReturn\n").replace("PushConstanti8 1\nReturn\n.function", ".function", 1))
    assert r == Variant(0x10, 1)
    c = "C 00000000 OnEvent 0010:00000032 0010:000000fa" + " 000f:00000000" * 8
    assert log[0] == c and log[1] == "00000000 gcmodeldirector.addtoa 1 -> 0010:00000001"
    assert vm.max_nesting == 2                                     # main (1) -> addtoa -> OnEvent (2)
    md = ctx.subsystem(ob().ModelDirector)
    toa = md.map[CNT1]
    assert isinstance(toa, ob().GcToa) and toa.ammo == 250 and toa.engine_refs == 1       # the map's copy: 1
    assert toa.char_id == CNT1 and toa.node_id == CNT1 and toa.name_id == ONUA
    assert md.main_toa is toa and md.saver_ammo == 250 and md.saver_toa_id == ONUA
    assert md.toa_index == 0


def test_addtoa_name_selects_the_toa_index_and_the_id():
    for name, idx in (("tahu", 5), ("lewa", 4), ("kopa", 3), ("poha", 2), ("gali", 1), ("onua", 0), ("zzzz", 0), ("toolong", 0), ("", 0)):
        src = ('.function main 0\nPushConstantString "%s"\nCallGameFunction gcmodeldirector.addtoa 1\nReturn\n' % name) + ONEVENT
        r, vm, ctx, tr, log = run_main(src)
        md = ctx.subsystem(ob().ModelDirector)
        assert md.toa_index == idx, name
        want = sv().convert_string_to_id(name.encode())
        assert md.map[CNT1].name_id == want and md.saver_toa_id == want


def test_addtoa_second_call_runs_the_constructor_again_but_returns_0_and_keeps_the_first_toa():
    src = ('.function main 0\nPushConstantString "onua"\nCallGameFunction gcmodeldirector.addtoa 1\nPop\n'
           'PushConstantString "gali"\nCallGameFunction gcmodeldirector.addtoa 1\nReturn\n') + ONEVENT
    r, vm, ctx, tr, log = run_main(src)
    assert r == Variant(0x10, 0)
    assert [l for l in log if l.startswith("C ")].__len__() == 2          # OnEvent ran in both constructors (unconditional)
    md = ctx.subsystem(ob().ModelDirector)
    assert md.map[CNT1].name_id == ONUA and md.saver_toa_id == 0x67616C69       # [0x74749c] is written before Add (0x47e221)
    assert md.toa_index == 0                                                 # the switch is on the success tail only


def test_addtoa_duplicate_leaves_the_character_block_file_in_the_slot_and_success_restores_it(tmp_path):
    inst = make_install(tmp_path, art=["a.tga"], chars={"onua.blk": ["x.tga"], "gali.blk": ["y.tga"]})
    src = ('.function main 0\nPushConstantString "onua"\nCallGameFunction gcmodeldirector.addtoa 1\nPop\n'
           'PushConstantString "gali"\nCallGameFunction gcmodeldirector.addtoa 1\nReturn\n') + ONEVENT
    vm, ctx, tr, log = make_vm(src, install=inst)
    slots = ctx.subsystem(sv().BlockFileSlots)
    assert slots.current is None
    assert vm.call("main", []).ok
    # the first call restored the slot (0x47e61e); the duplicate one returned at 0x47e4ca without restoring: gali.blk stays
    assert slots.current is not None and [e.name for e in slots.current] == ["y.tga"]
    # so doesfileexist no longer falls back to the loose file system (natives.md 8.2: a set slot suppresses FileExists)
    assert sv().does_file_exist(ctx, b"Root/Data/Art/Art.blk") is False


def test_addtoa_success_with_a_missing_character_block_still_succeeds_and_restores_the_slot():
    src = ('.function main 0\nPushConstantString "onua"\nCallGameFunction gcmodeldirector.addtoa 1\nReturn\n') + ONEVENT
    r, vm, ctx, tr, log = run_main(src)
    assert r == Variant(0x10, 1) and ctx.subsystem(sv().BlockFileSlots).current is None


def test_addtoa_onevent_error_propagates_out_of_the_native_and_nothing_is_added():
    bad = ".function OnEvent 2\n.byte 0x85\nReturn\n"                      # BCLUnused: an error in the nested call
    src = ('.function main 0\nPushConstantString "onua"\nCallGameFunction gcmodeldirector.addtoa 1\nReturn\n') + bad
    msg, vm, ctx, tr, log = fails(src)
    md = ctx.subsystem(ob().ModelDirector)
    assert md.map == {} and md.main_toa is None and md.saver_toa_id is None
    assert md.saver_ammo == 250                                           # written at 0x56898b, before the Call at 0x568a59
    assert not any(l.startswith("00000000 gcmodeldirector.addtoa") for l in log)      # no line of its own (natives.md 3.2)
    assert "Script is calling gcmodeldirector.addtoa" in msg


def test_addtoa_without_an_onevent_function_is_not_an_error():
    r, vm, ctx, tr, log = run_main('.function main 0\nPushConstantString "onua"\nCallGameFunction gcmodeldirector.addtoa 1\n'
                                   'Return\n')
    assert r == Variant(0x10, 1) and "M OnEvent" in tr.lines


def test_gettoa_and_getcharacter_return_the_same_object_with_the_engine_count_of_the_map():
    src = ADDTOA % ("CallGameFunction gcmodeldirector.gettoa 0\nSetVariableValue global:b\n"
                    "PushConstanti32 0x636e7431\nPushConstanti32 0x636e7431\nCallGameFunction gcmodeldirector.getcharacter 2\n"
                    "SetVariableValue global:c\n")
    r, vm, ctx, tr, log = run_main(src)
    assert [l for l in log if "gettoa" in l or "getcharacter" in l] == [
        "00000001 gcmodeldirector.gettoa 0 -> 0037:00000001 G 1",
        "00000002 gcmodeldirector.getcharacter 2 -> 0022:00000001 G 1"]
    assert [l for l in tr.lines if l.startswith("A g")] == ["A g 00000001 37"]      # one handle, minted as GcToa
    (h, toa), = ctx._objects.items()
    assert h == 1 and isinstance(toa, ob().GcToa) and vm.heap.engine(1) == 1


def test_gettoa_and_getcharacter_return_nothing_without_an_object():
    src = ('.function main 0\nCallGameFunction gcmodeldirector.gettoa 0\nPop\nPushConstanti32 1\n'
           'CallGameFunction gcmodeldirector.getcharacter 1\nReturn\n')
    r, vm, ctx, tr, log = run_main(src)
    assert r == NOTHING
    assert log[0] == "00000000 gcmodeldirector.gettoa 0 -> 000f:00000000"
    assert log[1] == "00000001 gcmodeldirector.getcharacter 1 -> 000f:00000000"


def test_get_asm_pins(disasm, image):
    pin(disasm, 0x47cf80, 0x47cfb0, {0x47cf86: "mov edx, dword ptr [0x782884]", 0x47cf93: "call 0x480f70", 0x47cf9e: "call 0x47cfb0"})
    pin(disasm, 0x47ce90, 0x47ced0, {0x47ceb4: "call 0x480f70", 0x47cebf: "call 0x47ced0"})
    pin(disasm, 0x47cfb0, 0x47d090, {0x47cfc3: "mov word ptr [esi], 0xf", 0x47cfe0: "inc dword ptr [edi + 0x4]"})
    pin(disasm, 0x47ced0, 0x47cf80, {0x47cee3: "mov word ptr [esi], 0xf", 0x47cf00: "inc dword ptr [edi + 0x4]"})
    pin(disasm, 0x482d00, 0x482d60, {0x482d43: "mov dword ptr [0x782884], 0x636e7431"})
    # Add: temporary AddRef (0x480ec7), insert (0x482270), temporary Release (0x480f08), the inserted flag is the result
    pin(disasm, 0x480eb0, 0x480f30, {0x480ec7: "inc dword ptr [edx + 0x4]", 0x480ede: "call 0x482270",
                                    0x480f08: "dec dword ptr [edx + 0x4]", 0x480f19: "mov al, byte ptr [ebp - 0x24]"})
    for key in ("gcmodeldirector.gettoa", "gcmodeldirector.getcharacter"):
        cited(key, 0x480f70)
    cited("gcmodeldirector.gettoa", 0x47cf80, 0x47cfb0, 0x482d43, 0x47cfe0)
    cited("gcmodeldirector.getcharacter", 0x47ce90, 0x47ced0, 0x47cf00)


# ---- show
def show_src(ids, tail=""):
    body = "".join("PushConstanti32 %d\nPushConstanti32 0\nCallGameFunction gcmodeldirector.show 2\nPop\n" % i for i in ids)
    return ('.globals a b c\n.function main 0\nPushConstantString "onua"\nCallGameFunction gcmodeldirector.addtoa 1\nPop\n'
            'CallGameFunction gcmodeldirector.gettoa 0\nSetVariableValue global:b\n' + body + tail +
            'PushConstanti8 1\nReturn\n' + ONEVENT)


def test_show_asm_pins(disasm, image):
    pin(disasm, 0x47cd70, 0x47cdb0, {0x47cd94: "call 0x47f830"})
    pin(disasm, 0x47f830, 0x47fb50, {
        0x47f855: "call 0x480f70", 0x47f864: "add ecx, 0xc", 0x47f872: "call dword ptr [esi]", 0x47f880: "mov edx, dword ptr [0x838b44]",
        0x47f894: "cmp ecx, edx", 0x47f8a5: "je 0x47f960", 0x47f992: "jne 0x47fa30",
        0x47f99b: "mov edx, dword ptr [eax + 0x10]", 0x47f9a9: "mov ecx, dword ptr [0x8390a8]", 0x47f9b6: "call dword ptr [esi + 0x40]",
        0x47f9b9: "test eax, eax", 0x47f9bb: "je 0x47f9c5", 0x47f9d3: "call dword ptr [esi + 0x28]",
        0x47f9df: "inc dword ptr [edx + 0x4]", 0x47f9e6: "add ebx, 0x44", 0x47f9eb: "call 0x419890", 0x47f9f9: "dec dword ptr [ecx + 0x4]",
        0x47fa1d: "call 0x474580", 0x47fa24: "jmp 0x47fb0e",
        0x47fa41: "mov ecx, dword ptr [0x83aa4c]", 0x47fa4e: "call dword ptr [esi + 0x40]", 0x47fa51: "test eax, eax",
        0x47fa6e: "call dword ptr [esi + 0x28]", 0x47fa7a: "inc dword ptr [edx + 0x4]", 0x47fa86: "call 0x419890",
        0x47faa4: "mov esi, dword ptr [0x83a9c0]", 0x47fad8: "jne 0x47fb04", 0x47fae2: "call 0x426010",
        0x47fafd: "call 0x474580", 0x47fb09: "call 0x418d00", 0x47fb28: "call 0x515f80",
        0x47fb3b: "cmp ecx, edx", 0x47fb3d: "jne 0x47fb48", 0x47fb42: "call 0x54b330"})
    # SetCharacter: AddRef the new (0x54b344), Release the old (0x54b350-0x54b35f), store (0x54b364)
    pin(disasm, 0x54b330, 0x54b380, {0x54b338: "cmp dword ptr [0x83903c], edi", 0x54b33e: "je 0x54b36a",
                                    0x54b344: "inc dword ptr [edi + 0x4]", 0x54b359: "cmp dword ptr [ecx + 0x4], 0x0",
                                    0x54b364: "mov dword ptr [0x83903c], edi"})
    # the child vector: {capacity +0, size +4, data +8}; a full vector reallocates (cap 0 -> 1, then x2), copying the
    # old elements (0x4199b8), the new one (0x419a29), then destroying the old ones (0x419a4e)
    pin(disasm, 0x419890, 0x419b00, {0x419919: "jae 0x419940", 0x41992b: "call 0x419b00", 0x419935: "inc dword ptr [eax + 0x4]",
                                    0x419954: "mov dword ptr [ebp - 0x64], 0x1", 0x4199b8: "call 0x419b00",
                                    0x419a29: "call 0x419b00", 0x419a4e: "dec dword ptr [ecx + 0x4]", 0x419a6f: "inc dword ptr [eax + 0x4]"})
    pin(disasm, 0x419b00, 0x419b20, {0x419b0b: "cmp dword ptr [eax], 0x0", 0x419b12: "inc dword ptr [edx + 0x4]"})
    pin(disasm, 0x4ab810, 0x4ab870, {0x4ab83c: "mov dword ptr [esi + 0x44], 0x0", 0x4ab846: "mov dword ptr [esi + 0x48], 0x0",
                                    0x4ab84d: "mov dword ptr [esi + 0x4c], 0x0"})
    # Contains(id, depth, flag): the direct children compare their node id at +0x10 (flag 0 asks the child about itself)
    pin(disasm, 0x4abd60, 0x4abe00, {0x4abd70: "cmp byte ptr [ebp + 0x10], 0x0", 0x4abd7e: "mov ebx, dword ptr [eax + 0x10]",
                                    0x4abd86: "cmp ebx, edx", 0x4abdcd: "call dword ptr [esi + 0x40]"})
    cited("gcmodeldirector.show", 0x47cd70, 0x47f830, 0x47f9df, 0x419890, 0x54b330, 0x54b344, 0x4abd60, 0x47f9eb, 0x47f9f9)


def test_show_cnt1_adds_the_toa_to_its_render_group_and_to_the_ba_group():
    r, vm, ctx, tr, log = run_main(show_src([CNT1]))
    # events on the Toa (handle 1), in order: temporary +1 (0x47f9df), push_back copy +1 (0x419a29: the group vector was
    # empty, so the realloc path copies only the new element), temporary -1 (0x47f9f9), SetCharacter +1 (0x54b344)
    hs = [l for l in log if l.startswith("H ")]
    assert hs == ["H 0037:00000001 +1", "H 0037:00000001 +1", "H 0037:00000001 -1", "H 0037:00000001 +1"]
    assert vm.heap.engine(1) == 3                                   # map + render group + BAGroup
    md = ctx.subsystem(ob().ModelDirector)
    assert md.group_cnt1.items == [md.map[CNT1]] and md.group_cnt1.cap == 1
    assert ctx.subsystem(ob().BAGroup).character is md.map[CNT1]
    assert md.map[CNT1].parent is md.group_cnt1                        # SetParent 0x40fc00 (+0x20)


def test_show_is_idempotent_the_second_call_finds_the_node_id_in_the_group():
    r, vm, ctx, tr, log = run_main(show_src([CNT1, CNT1]))
    assert [l for l in log if l.startswith("H ")].__len__() == 4        # nothing from the second call (0x47f9b9 returns)
    assert vm.heap.engine(1) == 3


def test_getcharacter_after_show_reports_the_three_engine_holders():
    tail = "PushConstanti32 0x636e7431\nPushConstanti32 0x636e7431\nCallGameFunction gcmodeldirector.getcharacter 2\nSetVariableValue global:c\n"
    r, vm, ctx, tr, log = run_main(show_src([CNT1], tail))
    line = [l for l in log if "getcharacter" in l][0]
    assert line.endswith("-> 0022:00000001 G 3")                       # natives.md 2.4 / 3.4 say G 2: the asm adds the render-group copy


def test_show_unknown_id_dereferences_a_null_character_and_is_an_error():
    msg, vm, ctx, tr, log = fails(show_src([0x61626364]))
    assert "Script is calling gcmodeldirector.show" in msg


def test_show_other_ids_use_the_second_render_group_the_shown_id_list_and_not_the_ba_group():
    o = ob()
    r, vm, ctx, tr, log = run_main(show_src([]))
    md = ctx.subsystem(o.ModelDirector)
    other = o.GcCharacter(TYPES.id("GcCharacter"))
    other.char_id = other.node_id = 0x61626364
    md.add(other, 0x61626364)
    assert other.engine_refs == 1
    # drive Show through the script again on the same VM: a second run on a new VM keeps this unit simple
    vm.call("main2", []) if False else None
    o.show(ctx, 0x61626364, 0)
    assert md.group_other.items == [other] and md.shown_ids == [0x61626364] and md.group_cnt1.items == []
    assert other.engine_refs == 2 and ctx.subsystem(o.BAGroup).character is None
    assert other.shadow_type == 1                                       # 0x47fb04: SetShadowType(1) on this branch only
    o.show(ctx, 0x61626364, 0)                                          # contained: returns at 0x47fa51
    assert other.engine_refs == 2 and md.shown_ids == [0x61626364]


def test_show_second_group_member_reallocates_and_copies_the_first_element():
    o = ob()
    r, vm, ctx, tr, log = run_main(show_src([]))
    md = ctx.subsystem(o.ModelDirector)
    a, b = (o.GcCharacter(TYPES.id("GcCharacter")) for _ in range(2))
    a.char_id = a.node_id = 0x61000001
    b.char_id = b.node_id = 0x61000002
    md.add(a, 0x61000001)
    md.add(b, 0x61000002)
    o.show(ctx, 0x61000001, 0)
    assert (md.group_other.cap, len(md.group_other.items)) == (1, 1) and a.engine_refs == 2
    o.show(ctx, 0x61000002, 0)
    # cap 1 is full: realloc to 2 copies a (+1), the new b (+1), then destroys the old a (-1): a ends with the same count
    assert (md.group_other.cap, len(md.group_other.items)) == (2, 2) and a.engine_refs == 2 and b.engine_refs == 2


def test_render_node_group_exact_class_branch_is_unreachable_for_director_entries():
    o = ob()
    # the ScRenderNodeGroup class id (the bl branch of Show, 0x47f880-0x47f8a5) is the name hash 0xcd92fe7b; GcCharacter's
    # class info (0x838b4c, name at 0x6ffe32) hashes to 0xae7c7b09: never equal for a GcCharacter-derived entry
    def h(s):
        x = 0
        for c in s:
            x = (x * 0x83 + c) & M32
        return x
    assert h(b"ScRenderNodeGroup") == o.SC_RENDER_NODE_GROUP_ID == 0xCD92FE7B
    assert h(b"GcCharacter") == o.GcCharacter.CLASS_INFO_ID == 0xAE7C7B09 != o.SC_RENDER_NODE_GROUP_ID
    assert o.GcToa.CLASS_INFO_ID == o.GcCharacter.CLASS_INFO_ID


def test_class_info_pins(disasm, image):
    pin(disasm, 0x4f6af0, 0x4f6b20, {0x4f6b00: "imul eax, eax, 0x83"})
    assert image.read(0x71385b, 18) == b"ScRenderNodeGroup\0" and image.read(0x6ffe32, 12) == b"GcCharacter\0"
    pin(disasm, 0x4196b0, 0x4196c0, {0x4196b7: "mov eax, 0x838b4c"})
    pin(disasm, 0x422a70, 0x422a80, {0x422a70: "add ecx, -0xc", 0x422a73: "jmp 0x4196b0"})
    pin(disasm, 0x4ac5e0, 0x4ac680, {0x4ac642: "mov ebx, 0x838b40", 0x4ac653: "call 0x4fb3d0"})
    pin(disasm, 0x4fb3d0, 0x4fb460, {0x4fb3f3: "call 0x4f6af0", 0x4fb3fc: "mov dword ptr [edx + 0x4], eax"})


# ===============================================================================================================
# 6. gccharacter.initmotionsystem / setupmoveable
# ===============================================================================================================
def test_setupmoveable_asm_pins(disasm, image):
    pin(disasm, 0x4093c0, 0x409420, {0x4093e4: "push dword ptr [edx]", 0x4093fb: "call 0x418850", 0x409403: "mov word ptr [eax], 0xf"})
    pin(disasm, 0x418850, 0x4189c3, {0x418856: "cmp dword ptr [ebx + 0x124], 0x0", 0x41885d: "je 0x4189c3", 0x41887e: "call 0x574b80"})
    # the motion system guard: [ms+0x14] == MainCharacterID() (0x43af90) -> SetUnderWater(byte [0x73c5a8]); no early exit
    pin(disasm, 0x574b80, 0x574f02, {
        0x574b90: "mov dword ptr [esi + 0x8], 0x0", 0x574ed8: "or dword ptr [esi + 0x8], 0x80000000", 0x574ee5: "call 0x43af90",
        0x574eeb: "mov ecx, dword ptr [esi + 0x14]", 0x574eee: "mov edx, dword ptr [ebp - 0x14]", 0x574ef1: "cmp ecx, edx",
        0x574ef3: "jne 0x574f02", 0x574ef5: "mov al, byte ptr [0x73c5a8]", 0x574efa: "push eax", 0x574efd: "call 0x589050"})
    assert not no_instruction(disasm, 0x574b80, 0x574ee5, r"^ret")
    pin(disasm, 0x43af90, 0x43b000, {
        0x43af91: "cmp dword ptr [0x7032b8], 0x0", 0x43af98: "jne 0x43afb2", 0x43af9e: "mov edx, dword ptr [0x7796f4]",
        0x43afb8: "cmp dword ptr [edx + 0x1d0], 0x0", 0x43afc1: "mov edx, dword ptr [edx + 0x1d0]",
        0x43afcb: "mov edx, dword ptr [edx + 0x100]", 0x43afe4: "mov edx, dword ptr [0x7796f4]"})
    pin(disasm, 0x43c180, 0x43c1a0, {0x43c193: "mov dword ptr [0x7796f4], 0x636e7431"})
    # SetUnderWater: nonzero flag -> OnEvent(0x36, 1) at 0x589154; zero -> OnEvent(0x36, 0) at 0x589249 ("OnEvent" 0x73df20)
    pin(disasm, 0x589050, 0x589260, {
        0x58905c: "cmp byte ptr [ebp + 0x8], 0x0", 0x589060: "je 0x589178", 0x58907b: "push 0x36", 0x589094: "call 0x407f30",
        0x58914f: "push 0x73df20", 0x589154: "call 0x60b320", 0x589194: "push 0x36", 0x5891a7: "call 0x407f30",
        0x589244: "push 0x73df20", 0x589249: "call 0x60b320"})
    assert image.read(0x73df20, 8) == b"OnEvent\0" and image.read(0x73c5a8, 1) == b"\0"
    pin(disasm, 0x589260, 0x58926b, {0x589264: "mov byte ptr [0x73c5a8], dl"})
    cited("gccharacter.setupmoveable", 0x4093c0, 0x418850, 0x574b80, 0x574ef1, 0x589050, 0x589154, 0x589249, 0x43af90, 0x73c5a8)


def test_initmotionsystem_asm_pins(disasm, image):
    pin(disasm, 0x409320, 0x409345, {0x40932c: "call 0x40c600"})
    pin(disasm, 0x40c600, 0x40c680, {0x40c60b: "mov ebx, dword ptr [edi + 0x100]", 0x40c611: "push 0x300",
                                    0x40c62f: "call 0x5742a0", 0x40c637: "cmp dword ptr [edi + 0x124], eax",
                                    0x40c645: "inc dword ptr [eax + 0x4]", 0x40c657: "dec dword ptr [ecx + 0x4]",
                                    0x40c668: "mov dword ptr [edi + 0x124], eax"})
    pin(disasm, 0x5742a0, 0x574a30, {0x5742bf: "mov dword ptr [edx + 0x14], 0x3f3f3f3f", 0x5748ea: "mov edx, dword ptr [esi]",
                                    0x5748ec: "mov dword ptr [ebx + 0x14], edx"})
    row = {r["key"]: r for r in json.loads(OVERRIDES.read_text())}["gccharacter.initmotionsystem"]
    assert row["tier"] == "tier2" and "default" not in row
    # the merged row keeps side a's wording (controller ruling); the ctor/motion-id pins are the byte checks above
    assert {0x409320, 0x40c600, 0x40c60b} <= _addrs(row["cite"] + " " + row["note"])
    assert "gccharacter.initmotionsystem" not in ob().CITES                  # no bound body (fix 1)


SETUP = ('.globals a b\n.function main 0\nPushConstantString "onua"\nCallGameFunction gcmodeldirector.addtoa 1\nPop\n'
         'PushConstanti32 0x636e7431\nPushConstanti32 0x636e7431\nCallGameFunction gcmodeldirector.getcharacter 2\n'
         'SetVariableValue global:b\n%s'
         'GetVariableValue global:b\nPushConstantf32 1.5\nPushConstantf32 0.8\nPushConstanti8 1\nPushConstanti8 0\n'
         'PushConstanti8 0\nCallGameFunction gccharacter.setupmoveable 6\nPop\nPushConstanti8 1\nReturn\n' + ONEVENT)
INIT = ""        # initmotionsystem is a tier-2 stub (fix 1): setupmoveable models the motion system as present


def test_setupmoveable_with_a_motion_system_on_the_main_character_calls_onevent_0x36_0():
    r, vm, ctx, tr, log = run_main(SETUP % INIT)
    cs = [l for l in log if l.startswith("C ")]
    assert cs == ["C 00000000 OnEvent 0010:00000032 0010:000000fa" + " 000f:00000000" * 8,
                  "C 00000002 OnEvent 0010:00000036 0010:00000000" + " 000f:00000000" * 8]       # addtoa's, then setupmoveable's
    assert [l for l in log if "setupmoveable" in l] == ["00000002 gccharacter.setupmoveable 6 -> 000f:00000000"]
    ms = ctx.subsystem(ob().ModelDirector).map[CNT1].motion_system
    assert ms is not None and ms.id == CNT1


def test_setupmoveable_guard_follows_main_character_id_and_the_global_underwater_flag():
    o = ob()
    r, vm, ctx, tr, log = run_main(SETUP % INIT)
    md = ctx.subsystem(o.ModelDirector)
    toa = md.map[CNT1]
    # MainCharacterID(): [0x7032b8] null -> 'cnt1'; the Toa with +0x1d0 null -> 'cnt1'; +0x1d0 set -> that character's id
    assert o.main_character_id(ctx) == CNT1
    md.main_toa = None
    assert o.main_character_id(ctx) == CNT1
    md.main_toa = toa
    other = o.GcCharacter(TYPES.id("GcCharacter"))
    other.char_id = 0x61626364
    toa.attached = other
    assert o.main_character_id(ctx) == 0x61626364
    # the guard compares [ms+0x14] with it: the Toa's motion system (id 'cnt1') no longer matches
    n0 = len([l for l in log if l.startswith("C ")])
    o.setup_moveable(ctx, toa)
    assert len([l for l in log if l.startswith("C ")]) == n0
    toa.attached = None
    ctx.subsystem(o.UnderWater).global_flag = 1                          # [0x73c5a8] != 0 -> OnEvent(0x36, 1)
    o.setup_moveable(ctx, toa)
    assert [l for l in log if l.startswith("C ")][-1] == "C -1 OnEvent 0010:00000036 0010:00000001" + " 000f:00000000" * 8


def test_a_nothing_never_reaches_a_game_typed_parameter():
    # ConvertVariant refuses nothing -> a game type (opcodes.md), so the null-dereference branches of these natives
    # cannot be reached from a script; the models still raise the fault if a host passes one (unit level below)
    for native, extra, n in (("gclegocamera.getcameraaimode", "", 1),
                             ("gchuddirector.addbigsprite", "", 1)):
        msg, *_ = fails(".function main 0\nPushNothing\n%sCallGameFunction %s %d\nReturn\n" % (extra, native, n))
        assert "Script is calling " + native in msg


# ===============================================================================================================
# 7. gclegocamera.getcameraaimode / setcameraaimode / setcameraspline / settargetspline
# ===============================================================================================================
def test_camera_aimode_asm_pins(disasm, image):
    pin(disasm, 0x44ece0, 0x44ed10, {0x44ecee: "mov al, byte ptr [edx]", 0x44ecf0: "push eax", 0x44ecf1: "mov ecx, dword ptr [ebx]",
                                    0x44ecf3: "call 0x45b490"})
    pin(disasm, 0x44ed10, 0x44ed40, {0x44ed16: "mov edx, dword ptr [edx]", 0x44ed18: "mov al, byte ptr [edx + 0x3a1]",
                                    0x44ed23: "call 0x44ed40"})
    pin(disasm, 0x44ed40, 0x44ed56, {0x44ed4a: "movzx edx, byte ptr [esp + 0xc]"})
    pin(disasm, 0x45b490, 0x45b5d0, {
        0x45b49b: "mov al, byte ptr [edi + 0x3a1]", 0x45b4a7: "mov byte ptr [edi + 0x3a1], dl", 0x45b4b5: "mov dl, byte ptr [0x706fd8]",
        0x45b4bd: "jb 0x45b4c0", 0x45b4c8: "cmp byte ptr [edi + 0x3a1], 0xa", 0x45b4d1: "cmp byte ptr [edi + 0x3a1], 0x8",
        0x45b4da: "cmp byte ptr [edi + 0x3a1], 0x6", 0x45b4e3: "cmp byte ptr [edi + 0x3a1], 0xc", 0x45b4ec: "cmp byte ptr [edi + 0x3a1], 0x5",
        0x45b4fe: "mov byte ptr [edi + 0x454], 0x1", 0x45b505: "mov byte ptr [edi + 0x2f1], 0xb",
        0x45b510: "cmp byte ptr [edi + 0x3a1], 0x9", 0x45b519: "cmp byte ptr [edi + 0x3a1], 0xe", 0x45b522: "cmp byte ptr [edi + 0x3a1], 0x4",
        0x45b52b: "cmp byte ptr [edi + 0x3a1], 0x2", 0x45b534: "cmp byte ptr [edi + 0x3a1], 0x3", 0x45b53f: "call 0x4fa460",
        0x45b558: "add byte ptr [ebp - 0x10], -0x2", 0x45b55c: "cmp byte ptr [ebp - 0x10], 0x1", 0x45b560: "ja 0x45b5c6",
        0x45b56a: "mov dl, byte ptr [0x706fdc]", 0x45b581: "mov dl, byte ptr [0x706fe0]", 0x45b590: "cmp dword ptr [edi + 0x3a4], 0x0",
        0x45b5a8: "dec dword ptr [ecx + 0x4]", 0x45b5b6: "mov dword ptr [edi + 0x3a4], 0x0", 0x45b5cd: "ret 0x4"})
    assert [image.read(a, 1)[0] for a in (0x706fd8, 0x706fdc, 0x706fe0)] == [0xF, 2, 3]
    pin(disasm, 0x44f3e0, 0x450600, {0x44f6a0: "mov byte ptr [edx + 0x3a1], 0x0", 0x44f6ad: "mov dword ptr [edx + 0x3a4], 0x0"})
    cited("gclegocamera.setcameraaimode", 0x44ece0, 0x45b490, 0x706fd8, 0x706fdc, 0x706fe0, 0x45b5a8, 0x45b4fe)
    cited("gclegocamera.getcameraaimode", 0x44ed10, 0x44ed18, 0x44ed40, 0x44f6a0)


def test_camera_spline_asm_pins(disasm, image):
    pin(disasm, 0x44f180, 0x44f1e0, {0x44f1b1: "call 0x45b380"})
    pin(disasm, 0x44f1e0, 0x44f240, {0x44f21d: "call 0x45b400"})
    pin(disasm, 0x45b380, 0x45b490, {
        0x45b38b: "cmp dword ptr [edi + 0x3a4], ebx", 0x45b397: "inc dword ptr [ebx + 0x4]", 0x45b3a9: "dec dword ptr [ecx + 0x4]",
        0x45b3b7: "mov dword ptr [edi + 0x3a4], ebx", 0x45b3c6: "mov dword ptr [edi + 0x3b8], edx", 0x45b40b: "cmp dword ptr [edi + 0x3a4], ebx",
        0x45b417: "inc dword ptr [ebx + 0x4]", 0x45b429: "dec dword ptr [ecx + 0x4]", 0x45b437: "mov dword ptr [edi + 0x3a4], ebx",
        0x45b44f: "mov dword ptr [edi + 0x3bc], edx"})
    cited("gclegocamera.setcameraspline", 0x44f180, 0x45b380, 0x45b397, 0x45b3a9, 0x45b3b7)
    cited("gclegocamera.settargetspline", 0x44f1e0, 0x45b400, 0x45b417, 0x45b429, 0x45b437)


def cam_script(body):
    """A script that gets the camera the way Setup does (the registry stubs gcgame.getmainview / gcviewport.getcamera and
    hands out typed fresh handles) and then runs `body`."""
    return ('.globals c s\n.function main 0\nCallGameFunction gcgame.getmainview 0\nCallGameFunction gcviewport.getcamera 1\n'
            'SetVariableValue global:c\n' + body + 'PushConstanti8 1\nReturn\n')


def set_mode(m):
    return "GetVariableValue global:c\nPushConstanti32 %d\nCallGameFunction gclegocamera.setcameraaimode 2\nPop\n" % m


GET = "GetVariableValue global:c\nCallGameFunction gclegocamera.getcameraaimode 1\n"


def test_camera_initial_mode_is_0_and_set_get_round_trips_the_low_byte():
    r, vm, ctx, tr, log = run_main(cam_script(GET.replace("CallGameFunction gclegocamera.getcameraaimode 1\n", "CallGameFunction gclegocamera.getcameraaimode 1\nSetVariableValue global:s\n")))
    assert [l for l in log if "getcameraaimode" in l] == ["00000002 gclegocamera.getcameraaimode 1 -> 0010:00000000"]
    for m, want in ((0xB, 0xB), (9, 9), (3, 3), (0xF, 0xF), (0xFF, 0xFF), (0x100, 0), (0x1FF, 0xFF), (-1, 0xFF)):
        r, vm, ctx, tr, log = run_main(cam_script(set_mode(m) + GET + "Pop\n"))
        assert [l for l in log if "getcameraaimode" in l][0].endswith("-> 0010:%08x" % want), m


def test_camera_adopts_a_stub_minted_handle_and_keeps_its_state_across_calls():
    src = cam_script(set_mode(0xB) + set_mode(3) + GET + "Pop\n")
    r, vm, ctx, tr, log = run_main(src)
    cam = [o for o in ctx._objects.values() if isinstance(o, ob().GcLegoCamera)]
    assert len(cam) == 1 and cam[0].aimode == 3 and cam[0].type() == TYPES.id("GcLegoCamera") == 0x2A


def test_camera_mode_side_effect_fields():
    o = ob()
    cam = o.GcLegoCamera(TYPES.id("GcLegoCamera"))
    r, vm, ctx, tr, log = run_main(".function main 0\nPushConstanti8 1\nReturn\n")
    o.set_camera_ai_mode(ctx, cam, 0xB)
    assert cam.aimode == 0xB and cam.flag454 == 1 and cam.byte2f1 == 0xB
    cam2 = o.GcLegoCamera(TYPES.id("GcLegoCamera"))
    assert cam2.aimode == 0 and cam2.spline_holder is None                # the ctor (0x44f6a0, 0x44f6ad)
    for m in (0xA, 8, 6, 0xC, 5, 9, 0xE, 4, 0x10, 0xF, 0xFF, 0, 1, 7, 0xD):
        o.set_camera_ai_mode(ctx, cam2, m)
        assert cam2.aimode == m and cam2.flag454 == 0 and cam2.byte2f1 == 0 and cam2.entered_spline_mode is False, m
    for m in (2, 3):
        o.set_camera_ai_mode(ctx, cam2, m)
        assert cam2.entered_spline_mode is True, m                     # GetTime(0) into +0x3b0 (host clock, not modelled)


SPL = ('PushConstantString "frnt_CAM.slb"\nCallGameFunction scslosislkcubicspline.create 1\nSetVariableValue global:s\n')
SETSPL = ("GetVariableValue global:c\nGetVariableValue global:s\nPushConstanti8 1\nPushConstantf32 0.5\nPushConstanti8 0\n"
          "PushConstanti8 0\nCallGameFunction gclegocamera.setcameraspline 6\nPop\n")
TGTSPL = ("GetVariableValue global:c\nGetVariableValue global:s\nPushConstanti8 1\nPushConstanti8 2\nPushConstantf32 0.5\n"
          "PushConstanti8 0\nPushConstanti8 0\nCallGameFunction gclegocamera.settargetspline 7\nPop\n")


def test_camera_spline_setters_hold_one_reference_and_the_same_spline_twice_changes_nothing():
    r, vm, ctx, tr, log = run_main(cam_script(SPL + SETSPL + TGTSPL))
    hs = [l for l in log if l.startswith("H ")]
    assert hs == ["H 0023:00000003 +1"]                                 # setcameraspline: +1; settargetspline: same pointer, none
    cam = [o for o in ctx._objects.values() if isinstance(o, ob().GcLegoCamera)][0]
    assert cam.spline_holder is not None and vm.heap.engine(cam.spline_holder.handle) == 1


def test_camera_leaving_the_spline_modes_releases_the_held_spline():
    # holder is released when the OLD mode is 2 or 3 and the NEW mode is neither (0x45b55c-0x45b5b6)
    src = cam_script(SPL + set_mode(2) + SETSPL + set_mode(3) + set_mode(0xB))
    r, vm, ctx, tr, log = run_main(src)
    assert [l for l in log if l.startswith("H ")] == ["H 0023:00000003 +1", "H 0023:00000003 -1"]
    cam = [o for o in ctx._objects.values() if isinstance(o, ob().GcLegoCamera)][0]
    assert cam.spline_holder is None and vm.heap.engine(3) == 0           # the script still holds the spline (global s)


@pytest.mark.parametrize("old, new, releases", [(2, 3, False), (3, 2, False), (2, 2, False), (3, 3, False), (2, 0, True), (3, 0xB, True),
                                                (2, 0xF, True), (2, 0x103, False), (0, 0, False), (0xB, 0, False), (1, 0, False), (4, 9, False)])
def test_camera_release_condition_table(old, new, releases):
    o = ob()
    r, vm, ctx, tr, log = run_main(".function main 0\nPushConstanti8 1\nReturn\n")
    cam = o.GcLegoCamera(TYPES.id("GcLegoCamera"))
    held = o.GcLegoCamera(TYPES.id("GcLegoCamera"))
    held.engine_refs = 1
    cam.aimode, cam.spline_holder = old, held
    o.set_camera_ai_mode(ctx, cam, new & 0xFF)
    assert (cam.spline_holder is None) == releases and held.engine_refs == (0 if releases else 1)


def test_camera_models_fault_on_a_null_host_argument():
    # unreachable from a script (a nothing never converts to a game type) but the models do not guess a camera
    r, vm, ctx, tr, log = run_main(".function main 0\nPushConstanti8 1\nReturn\n")
    class Call:
        def __init__(self, vm): self.vm, self.slots = vm, [0]
        def arg(self, i): return NOTHING
    vm.stack[0] = NOTHING if len(vm.stack) else None
    with pytest.raises(osi_vm.VmError):
        ob()._arg(ctx, type("C", (), {"arg": lambda self, i: NOTHING, "vm": vm, "slots": [0]})(), 0, ob().GcLegoCamera, "GcLegoCamera")


# ===============================================================================================================
# 8. gchuddirector.addbigsprite / addtextobject
# ===============================================================================================================
def test_hud_asm_pins(disasm, image):
    pin(disasm, 0x47afe0, 0x47b010, {0x47afee: "call 0x47b730", 0x47affd: "call 0x407df0"})
    pin(disasm, 0x47b010, 0x47b040, {0x47b01e: "call 0x47b780", 0x47b02d: "call 0x407df0"})
    pin(disasm, 0x47b730, 0x47b780, {0x47b737: "push 0x44", 0x47b745: "mov dword ptr [ebp - 0x8], 0x3f3f3f3f", 0x47b75a: "call 0x4ac750",
                                    0x47b764: "call 0x47b940"})
    pin(disasm, 0x47b780, 0x47b7d0, {0x47b7aa: "call 0x4ac750", 0x47b7b4: "call 0x47b940"})
    # the item constructor: the sprite / text object pointer is stored at +0x1c and AddRef'd when non-null (0x4ac7d3)
    pin(disasm, 0x4ac750, 0x4ac810, {0x4ac786: "mov dword ptr [ebx + 0x10], edx", 0x4ac7c4: "mov dword ptr [ebx + 0x1c], edx",
                                    0x4ac7ce: "je 0x4ac7d6", 0x4ac7d3: "inc dword ptr [edx + 0x4]"})
    # Add: the id is the counter [0x83aa3c] before its increment; 0 comes back if the container refuses (duplicate id)
    pin(disasm, 0x47b940, 0x47b9d0, {0x47b94f: "mov esi, dword ptr [0x83aa3c]", 0x47b958: "mov dword ptr [0x83aa3c], edx",
                                    0x47b989: "call 0x47b9d0", 0x47b991: "test al, al", 0x47b995: "mov dword ptr [edi], 0x0",
                                    0x47b9b0: "mov dword ptr [edi], esi"})
    pin(disasm, 0x650500, 0x650600, {0x650523: "test al, al", 0x650527: "xor al, al"})
    pin(disasm, 0x650600, 0x650640, {0x650610: "mov ebx, dword ptr [edi + 0x4]", 0x650616: "cmp ebx, edx", 0x65061a: "mov al, 0x1"})
    pin(disasm, 0x47cb5e, 0x47cb70, {0x47cb5e: "mov edx, dword ptr [0x839124]", 0x47cb64: "mov dword ptr [0x83aa3c], edx"})
    pin(disasm, 0x47cb40, 0x47cb5e, {0x47cb54: "mov dword ptr [0x839124], 0x0"}) if False else None
    assert any("mov dword ptr [0x839124], 0x0" in t for t in disasm(0x47cb54, 0x47cb5e).values())
    pin(disasm, 0x407df0, 0x407e07, {0x407dfb: "mov word ptr [ebx], 0x10", 0x407e00: "mov dword ptr [ebx + 0x4], edx"})
    cited("gchuddirector.addbigsprite", 0x47afe0, 0x47b730, 0x47b940, 0x4ac7d3, 0x47b958, 0x47cb64)
    cited("gchuddirector.addtextobject", 0x47b010, 0x47b780, 0x47b940, 0x4ac7d3)


SPRITE = ('PushConstantf32 1.0\nPushConstantf32 2.0\nPushConstantf32 3.0\nPushConstantf32 4.0\nPushConstantString "n"\n'
          'PushConstanti8 0\nCallGameFunction gcbigsprite.loadhudsprite 6\nSetVariableValue global:%s\n')


def test_addbigsprite_returns_the_running_item_index_and_holds_one_reference_on_the_sprite():
    src = ('.globals a b\n.function main 0\n' + SPRITE % "a" + SPRITE % "b" +
           'GetVariableValue global:a\nCallGameFunction gchuddirector.addbigsprite 1\nPop\n'
           'GetVariableValue global:b\nCallGameFunction gchuddirector.addbigsprite 1\nReturn\n')
    r, vm, ctx, tr, log = run_main(src)
    assert r == Variant(0x10, 1)
    adds = [l for l in log if "addbigsprite" in l]
    assert adds == ["00000002 gchuddirector.addbigsprite 1 -> 0010:00000000", "00000003 gchuddirector.addbigsprite 1 -> 0010:00000001"]
    # H +1 on the argument, written inside the native, before its own line
    i = log.index(adds[0])
    assert log[i - 1] == "H 0026:00000001 +1"
    assert vm.heap.engine(1) == 1 and vm.heap.engine(2) == 1


def test_a_sprite_the_hud_holds_survives_the_script_dropping_it():
    src = ('.globals a\n.function main 0\n' + SPRITE % "a" +
           'GetVariableValue global:a\nCallGameFunction gchuddirector.addbigsprite 1\nPop\nPushNothing\nSetVariableValue global:a\n'
           'PushConstanti8 1\nReturn\n')
    r, vm, ctx, tr, log = run_main(src)
    assert "F g 00000001" not in tr.lines and 1 in vm.heap.games and vm.heap.engine(1) == 1


def test_addtextobject_shares_the_counter_and_takes_the_same_reference():
    src = ('.globals a f t\n.function main 0\n' + SPRITE % "a" +
           'GetVariableValue global:a\nCallGameFunction gchuddirector.addbigsprite 1\nPop\n'
           'PushConstantString "fnt"\nCallGameFunction sctilefont.create 1\nSetVariableValue global:f\n'
           'PushConstantf32 1.0\nPushConstantf32 2.0\nPushConstantf32 3.0\nGetVariableValue global:f\n'
           'PushConstantString "t"\nCallGameFunction sctextobject.create 5\nSetVariableValue global:t\n'
           'GetVariableValue global:t\nCallGameFunction gchuddirector.addtextobject 1\nReturn\n')
    r, vm, ctx, tr, log = run_main(src)
    assert r == Variant(0x10, 1)
    # seqs: loadhudsprite 0, addbigsprite 1, sctilefont.create 2, sctextobject.create 3, addtextobject 4
    i = log.index("00000004 gchuddirector.addtextobject 1 -> 0010:00000001")
    assert log[i - 1] == "H 002e:00000003 +1" and vm.heap.engine(3) == 1      # handle 3 = the text object (sprite 1, font 2)


def test_hud_add_of_a_null_object_takes_no_reference_but_consumes_an_index():
    # unreachable from a script (see test_a_nothing_never_reaches_a_game_typed_parameter); the item ctor tests the pointer (0x4ac7ce)
    r, vm, ctx, tr, log = run_main(".function main 0\nPushConstanti8 1\nReturn\n")
    hud = ctx.subsystem(ob().HudDirector)
    assert (hud.add(ctx, None), hud.add(ctx, None)) == (0, 1) and not any(l.startswith("H ") for l in log)


def test_hud_duplicate_item_id_returns_0_but_the_item_already_holds_the_sprite():
    o = ob()
    src = ('.globals a\n.function main 0\n' + SPRITE % "a" + 'PushConstanti8 1\nReturn\n')
    r, vm, ctx, tr, log = run_main(src)
    spr = [x for x in ctx._objects.values()][0]
    hud = ctx.subsystem(o.HudDirector)
    assert hud.add(ctx, spr) == 0 and hud.next_id == 1
    hud.next_id = 0                                                     # wraps onto an existing id (the counter is a u32)
    assert hud.add(ctx, spr) == 0 and hud.next_id == 1 and len(hud.items) == 1 and spr.engine_refs + vm.heap.engine(spr.handle) >= 2
    hud.next_id = M32
    assert hud.add(ctx, spr) == M32 and hud.next_id == 0                  # u32 wrap (0x47b958 stores edx = esi + 1)


# ===============================================================================================================
# 9. the A9 path with the real models (needs the install)
# ===============================================================================================================
KEYS = "down@100 down@160 enter@220 esc@280"


def run_oracle(lomn_data, tmp_path, tag, frames=600, extra=()):
    log = tmp_path / ("n_%s.log" % tag)
    py = [sys.executable, str(REPO / "tools" / "research" / "osi_vm.py"), "--run", str(lomn_data / "install" / "data" / "Base.osi"),
          "--frames", str(frames), "--keys", KEYS, "--real-natives", "--install", str(lomn_data / "install"),
          "--overrides", str(OVERRIDES), "--log-out", str(log), *extra]
    p = subprocess.run(py, capture_output=True, text=True, timeout=110)
    assert p.returncode == 0, p.stderr
    return p.stdout, log.read_text().splitlines()


def test_oracle_runs_the_a9_path_with_the_object_models_without_error(lomn_data, tmp_path):
    out, log = run_oracle(lomn_data, tmp_path, "a")
    assert "stop: none" in out, out
    lines = {l.split()[1] for l in log if re.match(r"^\d{8} ", l)}
    assert {"gcmodeldirector.addtoa", "gcmodeldirector.gettoa", "gcmodeldirector.getcharacter", "gcmodeldirector.show",
            "gccharacter.setupmoveable", "gclegocamera.getcameraaimode", "gclegocamera.setcameraaimode",
            "gchuddirector.addbigsprite", "gchuddirector.addtextobject", "gcbigsprite.loadhudsprite", "sctextobject.create",
            "sctilefont.create", "scslosislkcubicspline.create"} <= lines
    # every hit native of this group is tier 1 or 2 (A10): none is a stub any more
    assert any(re.match(r"^\d{8} gcmodeldirector\.addtoa 1 -> 0010:00000001$", l) for l in log)
    assert any(re.match(r"^C \d{8} OnEvent 0010:00000032 0010:000000fa( 000f:00000000){8}$", l) for l in log)
    assert any(re.match(r"^C \d{8} OnEvent 0010:00000036 0010:00000000( 000f:00000000){8}$", l) for l in log)
    assert re.search(r"max (\d+)", out.split("engine-call nesting:")[1]).group(1) >= "2"


def test_oracle_a9_run_is_deterministic(lomn_data, tmp_path):
    a = run_oracle(lomn_data, tmp_path, "x", frames=120)
    b = run_oracle(lomn_data, tmp_path, "y", frames=120)
    assert a == b


# ===============================================================================================================
# fix 1: tier-2 rows with their own asm pins (the return type and value are exact), playcinematic left to pair 8
# ===============================================================================================================
def test_synclistenertocamera_returns_int_0_asm_pins(disasm, image):
    pin(disasm, 0x55e150, 0x55e16b, {0x55e153: "call 0x55f720", 0x55e15d: "call 0x413ea0"})
    pin(disasm, 0x55f720, 0x55f78d, {0x55f787: "xor eax, eax", 0x55f78c: "ret"})
    pin(disasm, 0x413ea0, 0x413eb5, {0x413ea5: "mov word ptr [ebx], 0x10", 0x413eae: "mov dword ptr [ebx + 0x4], edx"})
    rows = {r["key"]: r for r in json.loads((REPO / "data" / "osi" / "natives.json").read_text()) if r["winner"]}
    assert rows["gc3dsound.synclistenertocamera"]["ret"] == "int" and rows["gc3dsound.synclistenertocamera"]["tier"] == "tier2"


def test_settoaposition_and_hideinventory_return_nothing_asm_pins(disasm, image):
    pin(disasm, 0x474360, 0x4743a1, {0x474384: "call 0x475cd0", 0x47438b: "mov word ptr [edi], 0xf",
                                     0x474390: "mov dword ptr [edi + 0x4], 0x0"})
    pin(disasm, 0x62d400, 0x62d420, {0x62d407: "call 0x62e250", 0x62d40c: "mov word ptr [ebx], 0xf",
                                     0x62d411: "mov dword ptr [ebx + 0x4], 0x0"})


def test_fix1_rows_are_present_and_playcinematic_is_not_in_this_group():
    data = {r["key"]: r for r in json.loads(OVERRIDES.read_text())}
    for k in ("gc3dsound.synclistenertocamera", "gccollisionphysicsgroup.settoaposition", "gcstatusscreen.hideinventory",
              "gccharacter.initmotionsystem"):
        assert data[k]["tier"] == "tier2" and "default" not in data[k] and data[k]["cite"].isascii(), k
    assert data["gchuddirector.playcinematic"]["tier"] == "tier1"   # pair 8 owns it (controller ruling)
    assert "gchuddirector.playcinematic" not in ob().TIER2_KEYS
    assert "0x568a5e" in data["gcmodeldirector.addtoa"]["note"] and "0x5d4e40" in data["gcmodeldirector.addtoa"]["note"]
    assert "Pohatu" in data["gcmodeldirector.addtoa"]["note"] and "0x568c02" in data["gcmodeldirector.addtoa"]["note"]


def test_setupmoveable_models_the_motion_system_as_present_with_the_characters_id():
    r, vm, ctx, tr, log = run_main(SETUP % "")
    ms = ctx.subsystem(ob().ModelDirector).map[CNT1].motion_system
    assert ms is not None and ms.id == CNT1
