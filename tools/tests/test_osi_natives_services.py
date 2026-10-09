"""M2a-2 T7b: the services-and-time native models of the Python oracle (tools/research/osi_natives/services.py), written
from docs/osi/natives.md (§1.2 the ABI, §1.3 the citation format, §2.4 engine references, §8.1-8.2, §9.3) and from the
asm of the canonical exe and of BetterSaver.dll, read as data with objdump (never run). Never from the C++ engine.

For every native: an asm-pin test (objdump of the cited range, ranged so that the decode starts at the function and is not
a linear sweep; data constants by address from the PE image) and a behaviour test (the native called from a script
through the 0x82 handler, so the argument conversion, the S/N lines and the natives.log are the real ones).

Pins need LOMN_DATA (they skip with `SKIPPED (no LOMN_DATA)`); the behaviour tests of the pure models do not.

Registers read in the pins below are the ones objdump prints (Intel syntax, `, ` between operands). Every address is the
one cited in services.CITES, in the natives.md §1.3 form; the numbers a test pins come from the commands named in the
commit body (objdump --start-address/--stop-address on the exe, a PE section read for the .data constants).
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


def sv():
    from osi_natives import services
    return services


def fw():
    from osi_natives import framework
    return framework


# ----------------------------------------------------------------------------------------------- harness
def make_vm(asm, install=None, deterministic=True, with_log=True):
    """The oracle VM with the real native registry and only this group bound (so the tests do not depend on 8b/9b)."""
    f, s = fw(), sv()
    image, _ = osiasm.assemble(asm)
    tr = osi_vm.ListTrace()
    heap = osi_vm.Heap(tr)
    reg = osi_vm.NativeRegistry.load(osi_vm.DEFAULT_NATIVES, osi_vm.DEFAULT_MODS, TYPES)
    vm = osi_vm.Vm(image, reg, heap, tr)
    log = [] if with_log else None
    vfs = f.Vfs(install) if install is not None else None
    ctx = f.HostContext(vm, heap, vfs, f.HostFlags(deterministic=deterministic), log)
    s.bind(reg, ctx)
    return vm, ctx, tr, log


def run_main(asm, **kw):
    vm, ctx, tr, log = make_vm(asm, **kw)
    r = vm.call("main", [])
    assert r.ok, vm.last_error and vm.last_error.message
    return r.result, vm, ctx, tr, log


def one_string_call(native, text):
    return run_main('.function main 0\nPushConstantString "%s"\nCallGameFunction %s 1\nReturn\n' % (text, native))


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
        # objdump annotates a branch target with its section ("call 0x54ddc0 <.text+0x14cdc0>"); the pin names the target
        if got is not None:
            got = re.sub(r" <[^>]*>$", "", got)
        if got != text:
            bad[a] = (text, got)
    assert not bad, "asm pin mismatch (address: (expected, decoded)): " + ", ".join(
        "%#x: %r vs %r" % (a, e, g) for a, (e, g) in sorted(bad.items()))


def calls_in(disasm, start, stop):
    return [int(re.search(r"call (0x[0-9a-f]+)", t).group(1), 16) for a, t in sorted(disasm(start, stop).items())
            if t.startswith("call 0x")]


def iat_names(dll):
    """{IAT slot VA: (dll name, function name)} of a PE32 image's import table, parsed as data."""
    d = Path(dll).read_bytes()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec, osz = struct.unpack_from("<H", d, pe + 6)[0], struct.unpack_from("<H", d, pe + 20)[0]
    base = struct.unpack_from("<I", d, pe + 24 + 28)[0]
    secs = [struct.unpack_from("<IIII", d, pe + 24 + osz + 40 * i + 8) for i in range(nsec)]    # vsize, rva, rawsize, rawptr

    def off(rva):
        for vs, va, rs, rp in secs:
            if va <= rva < va + max(vs, rs):
                return rp + rva - va
        raise ValueError(hex(rva))

    def cstr(o):
        return d[o:d.index(b"\0", o)]
    out = {}
    p = off(struct.unpack_from("<I", d, pe + 24 + 104)[0])
    while True:
        ot, _, _, nm, ft = struct.unpack_from("<IIIII", d, p)
        if nm == 0:
            break
        dll_name, q, a = cstr(off(nm)), off(ot or ft), ft
        while struct.unpack_from("<I", d, q)[0]:
            v = struct.unpack_from("<I", d, q)[0]
            out[base + a] = (dll_name, b"ord%d" % (v & 0xFFFF) if v & 0x80000000 else cstr(off(v) + 2))
            q, a = q + 4, a + 4
        p += 20
    return out


# ===============================================================================================================
# 0. registration, citations and the overrides rows
# ===============================================================================================================
TIER1_KEYS = ["gcgame.getresolutionheight", "gcgame.getresolutionwidth", "gctimer.create",
              "scprocessmanager.userealdeltatime", "scslosifileio.doesfileexist",
              "scslosifoundation.identifiertostring", "scslosifoundation.stringtoidentifier"]
TIER3_KEYS = ["scslosifileio.loadosiobject", "scslosifileio.loadosiobjectfrommemcard",
              "scslosistringfuncs.getstringtable"]


def test_group_registers_itself_and_binds_exactly_the_tier1_rows():
    s, f = sv(), fw()
    assert "services" in f._GROUPS
    reg = osi_vm.NativeRegistry.load(osi_vm.DEFAULT_NATIVES, osi_vm.DEFAULT_MODS, TYPES)
    vm = osi_vm.Vm(osiasm.assemble(".function main 0\nReturn\n")[0], reg, osi_vm.Heap(None), None)
    ctx = f.HostContext(vm, vm.heap, None)
    s.bind(reg, ctx)
    implemented = sorted(e.key for e in reg.entries.values() if e.tier == "implemented")
    assert implemented == TIER1_KEYS


def test_every_bound_key_has_a_citation_in_the_natives_md_1_3_form():
    s = sv()
    assert sorted(s.CITES) == TIER1_KEYS
    for key, cite in s.CITES.items():
        assert cite.isascii(), key                                  # review I1: the overrides are pure ASCII
        assert re.match(r"^// body 0x[0-9a-f]+, method 0x[0-9a-f]+ <[^>]+>( -> 0x[0-9a-f]+ <[^>]+>)*; asm pin: \S", cite), key
        assert re.search(r"0x[0-9a-f]{5,}", cite)
        assert not re.search(r"0x0[0-9a-f]|0X|0x[0-9a-f]*[A-F]", cite)    # shortest lowercase hex (natives.md §1.3, review m-5)


def _addrs(text):
    """Code addresses (5+ hex digits) in a cite, as numbers: 0x18 or 0x68 are shifts and offsets, not addresses."""
    return {int(m, 16) for m in re.findall(r"0x([0-9a-fA-F]{5,})", text)}


# Addresses this module cites that the merged file's cite for the same key (the C++ side's wording) does not carry. The
# test pins them exactly (a changed cite on either side shows up); they are reported, not forced into the file.
CITE_ADDRESSES_ONLY_IN_THE_MODULE = {
    "gcgame.getresolutionheight": {0x4077f0, 0x5d4a7c},
    "gcgame.getresolutionwidth": {0x4077f0, 0x5d4a62},
    "gctimer.create": {0x4458fc, 0x4c2440},
    "scprocessmanager.userealdeltatime": {0x4a9170},
    "scslosifileio.doesfileexist": {0x10001da0, 0x10001dbe, 0x54de11, 0x60fec0, 0x73612c},
    "scslosifoundation.identifiertostring": {0x6178dd, 0x6178e6, 0x6178f7, 0x61796b},
    "scslosifoundation.stringtoidentifier": set(),
}


def test_committed_overrides_rows_are_valid_sorted_ascii_and_agree_with_the_module():
    """The merged overrides file carries both sides' rows (the C++ side's wording). Checked per key: `tier` and `default`
    exactly, and that the code addresses the module cites also appear in the file's cite (as numbers); the exceptions are
    pinned above."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("osi_registry_t7b", REPO / "tools" / "osi" / "registry.py")
    reg = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(reg)
    rows = json.loads((REPO / "data" / "osi" / "natives.json").read_text())
    mods = json.loads((REPO / "data" / "osi" / "natives_beta150.json").read_text())
    assert OVERRIDES.read_bytes().isascii()                         # review I1 / the controller's pure-ASCII ruling
    data = json.loads(OVERRIDES.read_text())
    assert reg.check_overrides(data, rows, mods) == []
    by_key = {r["key"]: r for r in data}
    keys = [r["key"] for r in data]
    assert keys == sorted(keys) and len(set(keys)) == len(keys)
    nothing = {"type": "0x0f", "value": 0}
    for k in TIER1_KEYS:
        assert by_key[k]["tier"] == "tier1" and "default" not in by_key[k], k
        mine, theirs = _addrs(sv().CITES[k]), _addrs(by_key[k]["cite"])
        assert mine - theirs == CITE_ADDRESSES_ONLY_IN_THE_MODULE[k], (k, sorted(hex(a) for a in mine - theirs))
    assert by_key["debug.print"]["tier"] == "tier2" and "default" not in by_key["debug.print"]
    for k in TIER3_KEYS:
        assert by_key[k]["tier"] == "tier3" and by_key[k]["default"] == nothing, k
    # the bodies of the tier3 and tier2 rows are named in their cites
    for k, body in (("debug.print", 0x61d5e0), ("scslosifileio.loadosiobject", 0x615990),
                    ("scslosifileio.loadosiobjectfrommemcard", 0x615ab0), ("scslosistringfuncs.getstringtable", 0x61b7d0)):
        assert body in _addrs(by_key[k]["cite"]), k


def test_the_merged_tables_carry_the_tiers():
    rows = {r["key"]: r for r in json.loads((REPO / "data" / "osi" / "natives.json").read_text()) if r["winner"]}
    for k in TIER1_KEYS:
        assert rows[k]["tier"] == "tier1", k
    assert rows["debug.print"]["tier"] == "tier2"
    for k in TIER3_KEYS:
        assert rows[k]["tier"] == "tier3" and rows[k]["default"] == {"type": "0x0f", "value": 0}


# ===============================================================================================================
# 1. scslosifoundation.stringtoidentifier   (body 0x617700 -> 0x617810 -> ConvertStringToID 0x5fb060)
# ===============================================================================================================
def test_stringtoidentifier_asm_pins(disasm, image):
    # ConvertStringToID: length test, the -1 result, then the shift immediates (first character in the high byte)
    pin(disasm, 0x5fb060, 0x5fb0c9, {
        0x5fb06b: "mov eax, dword ptr [edx + 0x4]", 0x5fb06e: "cmp eax, 0x4", 0x5fb075: "mov dword ptr [edi], 0xffffffff",
        0x5fb089: "mov esi, dword ptr [edx]",
        0x5fb08d: "and ebx, 0xff", 0x5fb093: "shl ebx, 0x18",
        0x5fb098: "and ecx, 0xff00", 0x5fb09e: "shl ecx, 0x8",
        0x5fb0a3: "and edx, 0xff0000", 0x5fb0a9: "shr edx, 0x8",
        0x5fb0ac: "and esi, 0xff000000", 0x5fb0b2: "shr esi, 0x18",
        0x5fb0b5: "or ebx, ecx", 0x5fb0b7: "or ebx, edx", 0x5fb0b9: "or ebx, esi", 0x5fb0bb: "mov dword ptr [edi], ebx"})
    # the method 0x617810 builds a NUL-terminated copy (c_str, assign with npos) and discards the validity flag
    pin(disasm, 0x617810, 0x61789b, {
        0x617843: "mov eax, dword ptr [0x726a44]", 0x617849: "mov ecx, dword ptr [ebp + 0xc]",
        0x61784c: "call 0x40c720", 0x617855: "call 0x401850", 0x617868: "call 0x5fb060",
        0x61786f: "mov ebx, dword ptr [ebp - 0x11c]", 0x617882: "cmp dword ptr [0x755798], edx",
        0x617888: "sete al", 0x61788b: "mov dword ptr [esi], ebx"})
    # the body: calls the method at 0x61771d, builds the int variant (0x407df0: type word 0x10 at 0x407dfb)
    pin(disasm, 0x617700, 0x617744, {0x61771d: "call 0x617810", 0x61772c: "call 0x407df0"})
    pin(disasm, 0x407df0, 0x407e07, {0x407dfb: "mov word ptr [ebx], 0x10", 0x407e00: "mov dword ptr [ebx + 0x4], edx"})
    # c_str() is the data pointer; assign(ptr, npos) takes strlen and throws at len >= capacity (0xff)
    pin(disasm, 0x40c720, 0x40c725, {0x40c722: "mov eax, dword ptr [edx]"})
    pin(disasm, 0x401850, 0x4018e0, {
        0x401874: "cmp edi, dword ptr [0x726a44]", 0x401881: "call 0x6573c0", 0x401894: "cmp edi, dword ptr [esi + 0x8]",
        0x401897: "setae al", 0x4018a8: "call 0x4d91f0"})
    assert image.u32(0x726a44) == 0xFFFFFFFF                         # npos


def test_stringtoidentifier_behaviour_through_a_script():
    r, vm, ctx, tr, log = one_string_call("scslosifoundation.stringtoidentifier", "frnt")
    assert r == Variant(0x10, 0x66726E74)                          # the falsifier: "frnt" -> 0x66726e74
    assert log[0] == "00000000 scslosifoundation.stringtoidentifier 1 -> 0010:66726e74"
    for text in ("x", "", "abcde", "abc"):
        assert one_string_call("scslosifoundation.stringtoidentifier", text)[0] == Variant(0x10, 0xFFFFFFFF), text
    assert one_string_call("scslosifoundation.stringtoidentifier", "ABcd")[0] == Variant(0x10, 0x41426364)   # case kept


def test_stringtoidentifier_model_edges():
    s = sv()
    assert s.convert_string_to_id(b"frnt") == 0x66726E74
    assert s.convert_string_to_id(b"\xff\xfe\xfd\xfc") == 0xFFFEFDFC
    assert s.convert_string_to_id(b"") == s.convert_string_to_id(b"abc") == s.convert_string_to_id(b"abcde") == M32
    # the wrapper copies through c_str + assign(npos): the length is strlen, so an embedded NUL shortens the string
    assert s.c_string(b"ab\0d") == b"ab" and s.convert_string_to_id(s.c_string(b"ab\0d")) == M32
    assert s.convert_string_to_id(s.c_string(b"abc\0")) == M32
    # assign throws at len >= 0xff (0x401894 setae): 254 passes, 255 throws kOutOfRange
    assert s.c_string(b"a" * 254) == b"a" * 254
    with pytest.raises(osi_vm.VmError) as e:
        s.c_string(b"a" * 255)
    assert e.value.message == "kOutOfRange"


# ===============================================================================================================
# 2. scslosifoundation.identifiertostring   (body 0x6177a0 -> IdentifierToString 0x6178c0)
# ===============================================================================================================
def test_identifiertostring_asm_pins(disasm, image):
    # a plain byte swap of the id into 4 characters (no table, so no "unknown id" text exists), then strlen-truncated
    pin(disasm, 0x6178c0, 0x617994, {
        0x6178cd: "mov edx, dword ptr [edx]",
        0x6178d7: "and ecx, 0xff00", 0x6178dd: "shl ecx, 0x8", 0x6178e0: "and edx, 0xff", 0x6178e6: "shl edx, 0x18",
        0x6178f1: "and edx, 0xff000000", 0x6178f7: "shr edx, 0x18", 0x617902: "and edx, 0xff0000", 0x617908: "shr edx, 0x8",
        0x617938: "push 0x4", 0x617944: "call 0x401850",
        0x617971: "mov eax, dword ptr [ebp - 0x118]", 0x61796b: "mov eax, dword ptr [0x726a44]", 0x61797b: "call 0x401850"})
    assert calls_in(disasm, 0x6178c0, 0x617994) == [0x4018f0, 0x401850, 0x4018f0, 0x401850, 0x4f5ad0]
    # the body 0x6177a0 passes the id by address and builds a dynamic string (0x46de00: type word 0x16)
    pin(disasm, 0x6177a0, 0x6177ed, {0x6177c2: "call 0x6178c0", 0x6177d4: "call 0x46de00"})
    pin(disasm, 0x46de00, 0x46de3d, {0x46de08: "mov word ptr [esi], 0x16", 0x46de2c: "call 0x46de40"})
    assert image.u32(0x726a44) == 0xFFFFFFFF
    # HashString 0x6178a0 is another native's method: it hashes, it does not look an id up (the brief's table row)
    pin(disasm, 0x6178a0, 0x6178c0, {0x6178ab: "call 0x4f6af0"})


def test_identifiertostring_behaviour_and_round_trip():
    s = sv()
    f = lambda i: run_main(".function main 0\nPushConstanti32 %d\nCallGameFunction scslosifoundation.identifiertostring 1\n"
                           "Return\n" % i)
    r, vm, ctx, tr, log = f(0x66726E74)
    assert r.type == 0x16                                             # the engine-call result is disposed at the return:
    assert log[0] == "00000000 scslosifoundation.identifiertostring 1 -> 0016:00000001 S 4 66726e74"   # read it in the log
    assert f(0)[4][0].endswith("-> 0016:00000001 S 0") and f(0x66720073)[4][0].endswith("S 2 6672")
    assert s.identifier_to_string(0) == b"" and s.identifier_to_string(0x66720073) == b"fr"      # strlen stops at the first NUL
    assert s.identifier_to_string(0xFFFFFFFF) == b"\xff\xff\xff\xff"
    assert s.identifier_to_string(0x41424344) == b"ABCD" and s.identifier_to_string(0x00424344) == b""
    for text in (b"frnt", b"cnt1", b"onua", b"\x01\x02\x03\x04", b"\xffabc"):
        i = s.convert_string_to_id(text)
        assert s.identifier_to_string(i) == text
    # the script-level inverse: id -> string -> id
    r2, *_ = run_main('.function main 0\nPushConstantString "frnt"\nCallGameFunction scslosifoundation.stringtoidentifier 1\n'
                      'CallGameFunction scslosifoundation.identifiertostring 1\nReturn\n')
    assert r2.type == 0x16


def test_round_trip_over_every_four_character_string_in_base_osi(lomn_data):
    import osi
    s = sv()
    o = osi.parse((lomn_data / "install" / "data" / "Base.osi").read_bytes())
    strs = [x if isinstance(x, bytes) else x.encode("latin1") for x in o["strings"]]
    four = sorted({x for x in strs if len(x) == 4 and b"\0" not in x})
    assert len(four) > 50                                            # Setup passes many ids ('frnt', 'cnt1', ...)
    for t in four:
        assert s.identifier_to_string(s.convert_string_to_id(t)) == t
    assert s.convert_string_to_id(b"frnt") == 0x66726E74 and b"frnt" in four and b"cnt1" in four


# ===============================================================================================================
# 3. scslosifileio.doesfileexist   (body 0x615950 -> 0x615ef0 -> SrBlockFileExists 0x54ddc0)
# ===============================================================================================================
def blk_bytes(names):
    out = b"BLKF" + struct.pack("<II", 1, len(names))
    for i, n in enumerate(names):
        out += struct.pack("<40sIIII", n.encode("latin1"), 64, 10, 0, 4096 + 64 * i)
    return out


def sorted_ci(names):
    return sorted(names, key=lambda n: bytes(c | 0x60 if 0x41 <= c <= 0x5A else c for c in n.encode("latin1")))


def make_install(tmp_path, art=None, loose=()):
    inst = tmp_path / "install"
    (inst / "data" / "Art").mkdir(parents=True)
    if art is not None:
        (inst / "data" / "Art" / "Art.blk").write_bytes(blk_bytes(art))
    for rel in loose:
        p = inst / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text("x")
    return inst


def exists(inst, path, setup=None):
    asm = '.function main 0\nPushConstantString "%s"\nCallGameFunction scslosifileio.doesfileexist 1\nReturn\n' % path
    vm, ctx, tr, log = make_vm(asm, install=inst)
    if setup:
        setup(ctx)
    r = vm.call("main", [])
    assert r.ok, vm.last_error.message
    assert r.result.type == 0x10
    return r.result.value, log


def test_doesfileexist_asm_pins(disasm, image):
    pin(disasm, 0x615950, 0x61598c, {0x615969: "call 0x615ef0", 0x615974: "call 0x407f30"})
    pin(disasm, 0x615ef0, 0x615f03, {0x615ef6: "mov eax, dword ptr [edx]", 0x615ef9: "call 0x54ddc0"})
    # SrBlockFileExists: Art.blk (0x83b0b8) first, then the two slots, FileExists only when the first slot is null
    pin(disasm, 0x54ddc0, 0x54de4d, {
        0x54ddc7: "mov ecx, 0x83b0b8", 0x54ddcc: "call 0x60fff0", 0x54ddd1: "test al, al", 0x54ddd3: "je 0x54ddf0",
        0x54dddb: "call 0x60fec0", 0x54dde2: "jl 0x54ddf0", 0x54dde4: "mov al, 0x1",
        0x54ddf0: "cmp dword ptr [0x736128], 0x0", 0x54ddf7: "je 0x54de40",
        0x54de00: "call 0x60fec0", 0x54de07: "jl 0x54de11",
        0x54de11: "cmp dword ptr [0x73612c], 0x0", 0x54de18: "je 0x54de32", 0x54de21: "call 0x60fec0",
        0x54de32: "xor al, al", 0x54de41: "call 0x54de70"})
    # IsHeaderLoaded is "the entry table pointer is non-null"; the header is read at 0x610000 from the path at 0x703523
    pin(disasm, 0x60fff0, 0x60fffa, {0x60fff2: "cmp dword ptr [edx + 0x10], 0x0", 0x60fff6: "setne al"})
    assert image.read(0x703523, 22) == b"Root/Data/Art/Art.blk\0"
    pin(disasm, 0x43b030, 0x43b054, {0x43b033: "mov ecx, 0x83b0b8", 0x43b041: "push 0x703523", 0x43b04b: "call 0x610000"})
    # LoadArtBlockHeader runs in DoSetUp 0x436a10 (0x436a55), before it calls SetUpScripting 0x4390d0 (0x436bcd): the header is
    # loaded when Setup runs. The block file saved at 0x436a4e is restored at 0x436bae; the Art.blk object is not the slot.
    pin(disasm, 0x436a4e, 0x436a5a, {0x436a4e: "call 0x54db30", 0x436a55: "call 0x43b030"})
    pin(disasm, 0x436bae, 0x436bd7, {0x436bae: "call 0x54db20", 0x436bcd: "call 0x4390d0"})
    # Find: the base name after the last '\\' (0x5c) or '/' (0x2f), a binary search over 0x38-byte entries
    pin(disasm, 0x60fec0, 0x60ff6d, {
        0x60fecc: "mov edi, ecx", 0x60fece: "mov dword ptr [ebp - 0x14], 0x0", 0x60fed5: "mov eax, dword ptr [edi + 0x8]",
        0x60fedf: "jg 0x60fef0", 0x60fee1: "mov eax, 0xffffffff",
        0x60fef6: "cmp byte ptr [ebx + edx], 0x5c", 0x60fefc: "cmp byte ptr [ebx + edx], 0x2f",
        0x60ff04: "inc esi", 0x60ff14: "sar ebx", 0x60ff16: "imul edx, ebx, 0x38", 0x60ff19: "add edx, dword ptr [edi + 0x10]",
        0x60ff1e: "call 0x60fd30", 0x60ff29: "mov eax, ebx", 0x60ff39: "cmp dword ptr [ebp - 0x14], edx",
        0x60ff58: "call 0x60fd10", 0x60ff63: "mov dword ptr [ebp - 0x10], ebx", 0x60ff68: "mov dword ptr [ebp - 0x14], ebx"})
    # the comparators: 0x60fd30 is "equal", 0x60fd10 is "name < entry", both over 0x60fd70 (A-Z folded with `or 0x60`)
    pin(disasm, 0x60fd10, 0x60fd2b, {0x60fd1b: "call 0x60fd70", 0x60fd24: "setl al"})
    pin(disasm, 0x60fd30, 0x60fd4b, {0x60fd3b: "call 0x60fd70", 0x60fd44: "sete al"})
    pin(disasm, 0x60fd70, 0x60fe05, {
        0x60fdb3: "cmp byte ptr [edi], 0x41", 0x60fdb8: "cmp byte ptr [edi], 0x5a", 0x60fdbf: "or bl, 0x60",
        0x60fdc6: "cmp byte ptr [esi], 0x41", 0x60fdcb: "cmp byte ptr [esi], 0x5a", 0x60fdd2: "or dl, 0x60",
        0x60fde1: "mov eax, 0xffffffff", 0x60fdf0: "mov eax, 0x1"})
    # FileExists 0x54de70: Open(name, flags) inside a try, flags = [0x71edf8] | [0x71ee10]; errors give false
    pin(disasm, 0x54de70, 0x54ded7, {0x54deac: "mov edx, dword ptr [0x71edf8]", 0x54deb2: "or edx, dword ptr [0x71ee10]",
                                     0x54debd: "call 0x4daea0", 0x54decf: "call 0x4daf40"})
    assert image.u32(0x71edf8) | image.u32(0x71ee10) == 0x41
    pin(disasm, 0x54df2d, 0x54df76, {0x54df2d: "mov ebx, dword ptr [0x8391d0]", 0x54df52: "xor al, al"})
    # slots at Setup: .data zero (natives.md §8.2)
    assert image.u32(0x736128) == 0 and image.u32(0x73612c) == 0


def test_doesfileexist_hook_asm_pins(lomn_data, disasm):
    dll = lomn_data / "install" / "mods" / "BetterSaver.dll"
    if not dll.exists():
        pytest.skip("SKIPPED (no BetterSaver.dll)")
    import exc
    img = exc.Image.from_file(dll)
    pin(disasm, 0x10001da0, 0x10001e3c, {
        0x10001dbe: "call dword ptr [0x1000809c]", 0x10001dc9: "je 0x10001e23",
        0x10001df2: "call dword ptr [0x1000806c]", 0x10001dfb: "je 0x10001e12", 0x10001dfd: "test al, 0x10",
        0x10001e01: "mov al, 0x1", 0x10001e12: "xor al, al", 0x10001e24: "call dword ptr [0x1000a7b8]"}, exe=dll)
    assert img.read(0x1000833c, 6) == b"User/\0"                    # the strstr needle (case-sensitive)


def test_doesfileexist_main_path_is_zero_on_the_real_install_and_agrees_with_an_independent_table_reader(lomn_data):
    """The falsifier: tools/research/blk.py's reader (not os.walk) over Art.blk and Main.blk, and the file system."""
    import blk
    inst = lomn_data / "install"
    for rel in ("data/Art/Art.blk", "data/Main.blk"):
        _, _, ents = blk.read(str(inst / rel))
        assert "areaviewer.txt" not in {e[0].lower() for e in ents}, rel
    assert not list(inst.rglob("AreaViewer*"))
    v, log = exists(inst, "Root/Data/AreaViewer.txt")
    assert v == 0 and log[0] == "00000000 scslosifileio.doesfileexist 1 -> 0010:00000000"
    # a name that IS in Art.blk is found by the base name, whatever the directory in front of it
    _, _, ents = blk.read(str(inst / "data/Art/Art.blk"))
    name = ents[len(ents) // 3][0]
    assert exists(inst, "Root/Data/Art/" + name)[0] == 1
    assert exists(inst, "Root/Whatever/Dir/" + name.upper())[0] == 1             # base name, A-Z folded
    assert exists(inst, "Root/Data/Art/no_such_file_%s" % name)[0] == 0


def test_doesfileexist_block_find_matches_the_original_binary_search(tmp_path):
    s = sv()
    names = ["Alpha.txt", "beta.TGA", "Gamma.slb", "delta"]                     # not sorted: the search must not care
    srt = [type("E", (), {"name": n}) for n in sorted_ci(names)]
    for i, n in enumerate(sorted_ci(names)):
        assert s.block_find(srt, n.encode()) == i
        assert s.block_find(srt, ("Dir\\Sub/" + n.swapcase()).encode()) == i   # last separator, then A-Z folded compare
        assert s.block_find(srt, ("Dir/Sub\\" + n.swapcase()).encode()) == i   # a BACKSLASH last (0x60fef6: 0x5c as well as 0x2f)
        assert s.block_find(srt, ("Dir\\" + n).encode()) == i
    assert s.block_find(srt, b"zzz") == -1 and s.block_find(srt, b"") == -1 and s.block_find([], b"alpha.txt") == -1
    assert s.block_find(srt, b"alpha.tx") == -1 and s.block_find(srt, b"alpha.txtx") == -1    # no prefix matching
    # an unsorted table: the original's search finds "a" but not "b" in [b, a] (hand-traced from 0x60ff0e-0x60ff6b)
    bad = [type("E", (), {"name": n}) for n in ("b", "a")]
    assert s.block_find(bad, b"a") == 1 and s.block_find(bad, b"b") == -1
    assert s.compare_names(b"abc", b"ABD") == -1 and s.compare_names(b"ABC", b"abc") == 0
    assert s.compare_names(b"ab", b"abc") == -1 and s.compare_names(b"abc", b"ab") == 1
    assert s.compare_names(b"[", b"{") == -1 and s.compare_names(b"_", b"A") == -1   # only A-Z is folded (or 0x60)


def test_doesfileexist_routes(tmp_path):
    inst = make_install(tmp_path, art=sorted_ci(["Foo.tga", "bar.tga"]), loose=["data/loose.txt", "Loose2.bin"])
    assert exists(inst, "Root/Data/Art/foo.TGA")[0] == 1                         # 1. Art.blk header loaded and Find >= 0
    assert exists(inst, "Root/loose2.BIN")[0] == 1                               # 3. slot null: FileExists on a loose file
    assert exists(inst, "Root/Data/loose.txt")[0] == 1
    assert exists(inst, "Root/Data/missing.txt")[0] == 0
    assert exists(inst, "Root/Data")[0] == 0                                     # a directory is not a file
    assert exists(inst, "Elsewhere/Data/loose.txt")[0] == 0                      # no such volume: Open throws, caught -> false
    # 2. [0x736128] non-null: Find in it, then in [0x73612c]; FileExists is NOT consulted
    def slots(ctx):
        st = ctx.subsystem(sv().BlockFileSlots)
        st.current = [type("E", (), {"name": "inslot.dat"})]
        st.second = [type("E", (), {"name": "second.dat"})]
    assert exists(inst, "Root/x/inslot.dat", slots)[0] == 1
    assert exists(inst, "Root/x/second.dat", slots)[0] == 1
    assert exists(inst, "Root/Data/loose.txt", slots)[0] == 0                    # slot set -> no FileExists fallback
    assert exists(inst, "Root/Data/Art/foo.tga", slots)[0] == 1                  # Art.blk still first
    def only_current(ctx):
        ctx.subsystem(sv().BlockFileSlots).current = []
    assert exists(inst, "Root/Data/loose.txt", only_current)[0] == 0
    # Art.blk header not loaded (no Art.blk): straight to the slots / FileExists
    inst2 = make_install(tmp_path / "b", art=None, loose=["data/loose.txt"])
    assert exists(inst2, "Root/Data/loose.txt")[0] == 1 and exists(inst2, "Root/Data/Art/foo.tga")[0] == 0


def test_doesfileexist_hook_resolve_game_path_asm_pins(lomn_data, disasm):
    """The hook hands the path to LOMNHook::ResolveGamePath (import of ijl15.dll, 0x1000813c) with size 0x104 and flag 0, then
    GetFileAttributesA on the result. ResolveGamePath (ijl15.dll 0x10001570, read as data, never run) has three branches on
    the position of "Root/" and "User/" (strstr results compared with the path pointer): Root/ -> GetCurrentDirectory +
    path[4:]; User/ -> SHGetFolderPath(0x1a) + edition + path[4:]; NEITHER -> a MessageBoxA "OSI Script Security Abort"
    (0x10001632) and then FALL THROUGH to the same strcat_s (0x10001641) with the path unchanged, so the result is the path
    itself, relative to the current directory (= the Root/ mapping); '/' becomes '\\' (0x1000166a); the directory-creating
    tail (0x10001683) is skipped for flag 0."""
    mods = lomn_data / "install" / "mods" / "BetterSaver.dll"
    ijl = lomn_data / "install" / "ijl15.dll"
    if not mods.exists() or not ijl.exists():
        pytest.skip("SKIPPED (no BetterSaver.dll / ijl15.dll)")
    pin(disasm, 0x10001da0, 0x10001e3c, {
        0x10001dcb: "push 0x0", 0x10001dcd: "push 0x104", 0x10001de2: "call dword ptr [0x1000813c]"}, exe=mods)
    assert iat_names(mods)[0x1000813c] == (b"ijl15.dll", b"?ResolveGamePath@LOMNHook@@YAXPBDPADI_N@Z")
    import exc
    img = exc.Image.from_file(ijl)
    assert img.read(0x1001e5b4, 6) == b"Root/\0" and img.read(0x1001e5bc, 6) == b"User/\0"
    assert img.read(0x1001e624, 26) == b"OSI Script Security Abort\0"
    names = iat_names(ijl)
    for va, want in ((0x1001e128, b"strstr"), (0x1001e01c, b"GetCurrentDirectoryA"), (0x1001e0f0, b"SHGetFolderPathA"),
                     (0x1001e100, b"MessageBoxA"), (0x1001e1f0, b"strcat_s")):
        assert names[va][1] == want, hex(va)
    pin(disasm, 0x10001570, 0x100016ae, {
        0x1000159e: "mov byte ptr [eax], 0x0",                                  # out[0] = 0
        0x10001592: "push 0x1001e5b4", 0x100015a1: "call esi", 0x100015a3: "push 0x1001e5bc", 0x100015ab: "call esi",
        0x100015b2: "cmp edi, ebx", 0x100015b7: "jne 0x100015cc",                # strstr(path, "Root/") == path
        0x100015c1: "call dword ptr [0x1001e01c]", 0x100015c7: "add ebx, 0x4", 0x100015ca: "jmp 0x1000163e",
        0x100015cc: "cmp ecx, ebx", 0x100015ce: "jne 0x10001608",                # strstr(path, "User/") == path
        0x100015db: "push 0x1a", 0x100015df: "call dword ptr [0x1001e0f0]", 0x10001603: "add ebx, 0x4",
        0x10001606: "jmp 0x1000163e",
        0x10001608: "push ebx", 0x1000161a: "call 0x10001ab0",                    # neither: the message text from the path
        0x10001628: "push 0x10", 0x1000162a: "push 0x1001e624", 0x10001632: "call dword ptr [0x1001e100]",
        0x10001638: "mov esi, dword ptr [ebp - 0x198]",                          # ... and no jump: it falls through
        0x1000163e: "push ebx", 0x10001641: "call dword ptr [0x1001e1f0]",       # strcat_s(out, size, ebx): ebx = path
        0x10001665: "cmp bl, 0x2f", 0x1000166a: "mov byte ptr [esi + edx], 0x5c",
        0x1000167f: "cmp byte ptr [ebp + 0x14], 0x0", 0x10001683: "je 0x1000169d"}, exe=ijl)


def test_doesfileexist_hook_user_paths(tmp_path):
    inst = make_install(tmp_path, art=["foo.tga"],
                        loose=["User/data/player1.sav", "data/User_notes.txt", "Data/Saves/User/x.sav"])
    # 1. "Root/" at the start (branch Root): GetCurrentDirectory + path[4:], i.e. the install's own file; here the file exists
    assert exists(inst, "Root/User/data/player1.sav")[0] == 1
    assert exists(inst, "Root/User/data/missing.sav")[0] == 0
    # 2. "User/" at the start (branch User): the per-run User store, which the oracle host does not hold: false even when
    #    a file of that name sits in the install
    assert exists(inst, "User/data/player1.sav")[0] == 0
    # 3. NEITHER prefix (C-1): MessageBoxA, then the SAME strcat_s with the path unchanged, relative to the current
    #    directory = the Root/ mapping: exists("Root/" + path). The message box is not logged.
    assert exists(inst, "Data/Saves/User/x.sav")[0] == 1
    assert exists(inst, "Data/Saves/User/y.sav")[0] == 0
    assert exists(inst, "Whatever/User/data/player1.sav")[0] == 0              # install/Whatever/... does not exist
    assert exists(inst, "root/User/data/player1.sav")[0] == 0                   # lower-case root/: neither prefix, install/root/User/.. is absent
    # no "User/" at all (lower case "user/"): the original route (Art.blk, then FileExists)
    assert exists(inst, "Root/user/data/player1.sav")[0] == 1
    assert exists(inst, "Root/Data/User_notes.txt")[0] == 1
    # the step is a pure function of the strings
    s = sv()
    assert s.hook_route(b"Root/User/x") == ("root", "Root/User/x") and s.hook_route(b"User/x") == ("user", "User/x")
    assert s.hook_route(b"a/User/x") == ("neither", "Root/a/User/x") and s.hook_route(b"a/user/x") == ("original", "a/user/x")
    # embedded NUL: the C string ends there (the hook and SrBlockFileExists read a char*)
    assert s.has_user_needle(b"Root/Us\0er/User/x") is False and s.has_user_needle(b"a/User/x") is True
    # strstr finds the FIRST "Root/"/"User/" anywhere: "User/" is checked only at the start, so a later "Root/" is neither
    assert s.hook_route(b"a/Root/User/x")[0] == "neither"


def test_doesfileexist_without_a_vfs_is_false():
    r, *_ = run_main('.function main 0\nPushConstantString "Root/Data/x"\nCallGameFunction scslosifileio.doesfileexist 1\nReturn\n')
    assert r == Variant(0x10, 0)


# ===============================================================================================================
# 4. gctimer.create   (body 0x4454f0 -> GcTimer::GcTimer 0x445840, Start 0x445990, CreateVariant 0x445550)
# ===============================================================================================================
def test_gctimer_asm_pins(disasm, image):
    pin(disasm, 0x4454f0, 0x445548, {
        0x445502: "push 0x80", 0x445514: "mov edx, dword ptr [esi]", 0x44551c: "fld dword ptr [ebx]",
        0x445520: "fstp qword ptr [esp]", 0x445523: "mov al, byte ptr [edi]", 0x44552a: "call 0x445840",
        0x445536: "call 0x445550"})
    # the ctor: duration (double) at +0x68, the started flag at +0x78 cleared, then Start only when the BYTE is non-zero
    pin(disasm, 0x445840, 0x445920, {
        0x4458fc: "fstp qword ptr [edx + 0x68]", 0x445902: "mov byte ptr [edx + 0x78], 0x0",
        0x445906: "cmp byte ptr [ebp + 0xc], 0x0", 0x44590a: "je 0x445914", 0x44590f: "call 0x445990",
        0x4458d2: "call 0x517510"})
    # Start: registers once (flag +0x78), AddTrigger 0x4c2440, then flag := 1
    pin(disasm, 0x445990, 0x445a34, {
        0x4459c6: "cmp byte ptr [esi + 0x78], 0x0", 0x4459ca: "jne 0x4459f5", 0x4459e9: "call 0x4c2440",
        0x4459f1: "mov byte ptr [esi + 0x78], 0x1"})
    # AddTrigger takes one reference on the trigger it stores (natives.md §2.4: the map's copy); the timer's id is the key
    pin(disasm, 0x4c25d4, 0x4c25f8, {0x4c25da: "mov dword ptr [ebx], ecx", 0x4c25e2: "mov ecx, dword ptr [ebp + 0x8]",
                                     0x4c25e5: "inc dword ptr [ecx + 0x4]"})
    # CreateVariant(GcTimer*): AddRef at 0x445580, null -> {0x0f, 0} at 0x445563
    pin(disasm, 0x445550, 0x4455f2, {0x445563: "mov word ptr [esi], 0xf", 0x445580: "inc dword ptr [edi + 0x4]"})
    # the base ctor stores a zero count: no reference on `this` from the constructors (natives.md §2.4)
    pin(disasm, 0x4f2b80, 0x4f2ba0, {0x4f2b88: "mov dword ptr [eax + 0x4], 0x0"})


def make_timer(id_, start, dur, **kw):
    """Creates a timer and keeps it in a global (an engine call's result is disposed at its return), then returns
    (the host GcTimer, vm, ctx, tr, log, its handle)."""
    asm = (".globals t\n.function main 0\nPushConstanti32 %d\nPushConstanti32 %d\nPushConstantf32 %s\n"
           "CallGameFunction gctimer.create 3\nSetVariableValue global:t\nPushConstanti8 1\nReturn\n" % (id_, start, dur))
    vm, ctx, tr, log = make_vm(asm, **kw)
    assert vm.call("main", []).ok
    (h, obj), = ctx._objects.items()
    return obj, vm, ctx, tr, log, h


def test_gctimer_create_0_0_5_is_not_started_and_has_no_engine_reference():
    t, vm, ctx, tr, log, h = make_timer(0, 0, "5.0")
    s = sv()
    assert isinstance(t, s.GcTimer) and t.type() == TYPES.id("GcTimer") == 0x27
    assert (t.event_id, t.started, t.duration) == (0, False, 5.0)
    assert vm.heap.engine(h) == 0 and vm.heap.game_type(h) == 0x27   # G 0 (natives.md §2.4): constructed by the native
    assert "A g 00000001 27" in tr.lines
    assert log[0] == "00000000 gctimer.create 3 -> 0027:00000001 G 0"
    assert not any(l.startswith(("C ", "H ")) for l in log)         # nothing re-enters the VM, no engine reference change


def test_gctimer_create_started_registers_the_trigger_and_holds_one_reference():
    t, vm, ctx, tr, log, h = make_timer(7, 1, "2.5")
    assert (t.event_id, t.started, t.duration) == (7, True, 2.5)
    assert vm.heap.engine(h) == 1 and log[0].endswith("-> 0027:00000001 G 1")


def test_gctimer_started_flag_is_the_low_byte_of_the_second_argument():
    for arg, started in ((1, True), (255, True), (256, False), (0x100 + 5, True), (0, False)):
        t, *_ = make_timer(0, arg, "1.0")
        assert t.started is started, arg                             # mov al, byte [edi] at 0x445523; cmp byte at 0x445906


def test_gctimer_duration_is_the_float_widened_to_a_double_and_id_is_a_dword():
    t, *_ = make_timer(0x7FFFFFFF, 0, "0.1")
    assert t.event_id == 0x7FFFFFFF and t.duration == struct.unpack("<f", struct.pack("<f", 0.1))[0] != 0.1


def test_gctimer_lifetime_follows_the_engine_references():
    # unstarted: only the script holds it; dropping the variant frees it (F g) and runs the host delete
    r, vm, ctx, tr, log = run_main(".function main 0\nPushConstanti8 0\nPushConstanti8 0\nPushConstantf32 5.0\n"
                                   "CallGameFunction gctimer.create 3\nPop\nPushConstanti8 1\nReturn\n")
    assert tr.lines.count("F g 00000001") == 1 and ctx._objects == {}
    # started: the trigger map holds a reference, so dropping the script's variant keeps the timer
    r, vm, ctx, tr, log = run_main(".function main 0\nPushConstanti8 0\nPushConstanti8 1\nPushConstantf32 5.0\n"
                                   "CallGameFunction gctimer.create 3\nPop\nPushConstanti8 1\nReturn\n")
    assert "F g 00000001" not in tr.lines and len(ctx._objects) == 1


def test_gctimer_never_calls_onevent_in_900_frames():
    asm = (".function main 0\nPushConstanti8 0\nPushConstanti8 0\nPushConstantf32 5.0\nCallGameFunction gctimer.create 3\n"
           "Pop\nPushConstanti8 1\nReturn\n.function Process 3\nPushConstanti8 1\nReturn\n")
    vm, ctx, tr, log = make_vm(asm)
    assert vm.call("main", []).ok
    for k in range(900):
        assert vm.call("Process", [Variant(0x12, 0), Variant(0x12, 0), Variant(0x10, k)]).ok
    assert not any(l.startswith("C ") for l in log)
    assert not any(" OnEvent" in l for l in tr.lines)


# ===============================================================================================================
# 5. scprocessmanager.userealdeltatime   (body 0x4a8ac0 -> ScProcessManager::UseRealDeltaTime 0x4a90e0)
# ===============================================================================================================
def test_userealdeltatime_asm_pins(disasm, image):
    pin(disasm, 0x4a8ac0, 0x4a8ae7, {0x4a8ac7: "lea edx, [ebp + 0x10]", 0x4a8aca: "mov al, byte ptr [edx]",
                                     0x4a8acd: "call 0x4a90e0", 0x4a8ad3: "mov word ptr [ebx], 0xf"})
    pin(disasm, 0x4a90e0, 0x4a90f9, {0x4a90e4: "test dl, dl", 0x4a90e8: "cmp byte ptr [0x712d78], 0x1",
                                     0x4a90f2: "mov byte ptr [0x712d78], dl"})
    assert image.read(0x712d78, 1) == b"\0"                          # the original's default: the fake-time path
    # ScProcessManager::Process 0x4a9170: the time step (D3)
    pin(disasm, 0x4a9170, 0x4a9330, {
        0x4a9212: "fstp qword ptr [0x712d80]", 0x4a9218: "cmp dword ptr [0x712d88], 0xa", 0x4a9221: "fld qword ptr [0x712db0]",
        0x4a923c: "fcomp qword ptr [0x712db8]", 0x4a9249: "fld qword ptr [0x712dc0]", 0x4a9266: "fmul qword ptr [0x712dc8]",
        0x4a926c: "fld qword ptr [0x712dd0]", 0x4a9272: "fmul qword ptr [0x712d80]", 0x4a9278: "faddp st(1), st",
        0x4a927a: "fstp qword ptr [0x712d70]", 0x4a9280: "inc dword ptr [0x712d88]",
        0x4a928c: "fcomp qword ptr [0x712dd8]", 0x4a9299: "fld qword ptr [0x712de0]", 0x4a929f: "fstp qword ptr [0x712d70]",
        0x4a92a5: "cmp byte ptr [0x712d78], 0x0", 0x4a92c8: "fld qword ptr [0x712d68]", 0x4a92dd: "fadd qword ptr [ebp - 0xcc]"})
    d = lambda va: struct.unpack("<d", image.read(va, 8))[0]
    s = sv()
    assert d(0x712d68) == s.FAKE_DT == 0.006666666828095913          # the D3 constants, read from .data by address
    assert d(0x712db0) == d(0x712dc0) == s.REAL_DT_DEFAULT == 0.016666000708937645
    assert d(0x712db8) == s.REAL_GAP == 0.5
    assert d(0x712dc8) == s.REAL_W_NEW == 0.07000000029802322
    assert d(0x712dd0) == s.REAL_W_OLD == 0.9300000071525574
    assert d(0x712dd8) == d(0x712de0) == s.REAL_DT_MAX == 0.06599999964237213
    assert d(0x712d70) == 0.0 and image.u32(0x712d88) == 0           # smoothed dt and pass counter start at 0


def test_userealdeltatime_is_a_logged_no_op_when_deterministic():
    r, vm, ctx, tr, log = run_main(".function main 0\nPushConstanti8 1\nCallGameFunction scprocessmanager.userealdeltatime 1\n"
                                   "Return\n", deterministic=True)
    assert r == NOTHING
    assert log == ["00000000 scprocessmanager.userealdeltatime 1 -> 000f:00000000"]
    assert ctx.subsystem(sv().ProcessTime).use_real == 0              # the fake-time path stays on (D3)


def test_userealdeltatime_sets_the_flag_byte_when_not_deterministic():
    for arg, byte in ((1, 1), (0, 0), (2, 2), (256, 0), (0x1FF, 0xFF)):
        r, vm, ctx, tr, log = run_main(".function main 0\nPushConstanti32 %d\nCallGameFunction "
                                       "scprocessmanager.userealdeltatime 1\nReturn\n" % arg, deterministic=False)
        assert r == NOTHING and ctx.subsystem(sv().ProcessTime).use_real == byte, arg   # mov [0x712d78], dl


def test_real_time_smoothing_follows_the_0x4a9170_constants():
    s = sv()
    t = s.ProcessTime(None)
    t.use_real = 0
    assert t.step(now=10.0) == (s.FAKE_DT, "fake")                    # fake time: dt = 0.0066667 whatever the clock says
    t = s.ProcessTime(None)
    t.use_real = 1
    t.last = 1.0
    # gap > 0.5 s: dt := 0.016666 (the constant), the pass counter still increments
    assert t.step(now=1.6)[0] == s.REAL_DT_DEFAULT and t.count == 1
    # the boundary: diff == 0.5 exactly is NOT a gap (`test ah, 0x45; jne` at 0x4a9244 skips on "<= 0.5"), so it smooths
    t = s.ProcessTime(None)
    t.use_real, t.last = 1, 1.0
    assert t.step(now=1.5)[0] == pytest.approx(0.5 * 0.07000000029802322 + 0.9300000071525574 * 0.016666000708937645, abs=1e-15)
    assert t.step(now=1.5 + 0.5000001)[0] == s.REAL_DT_DEFAULT                   # just above: a gap
    # the first ten passes use the default as the previous smoothed value: 0.07 * diff + 0.93 * 0.016666
    t = s.ProcessTime(None)
    t.use_real = 1
    t.last = 1.0
    dt1 = t.step(now=1.02)[0]
    assert dt1 == pytest.approx(0.02 * 0.07000000029802322 + 0.9300000071525574 * 0.016666000708937645, rel=0, abs=1e-15)
    # from the 11th pass the previous smoothed value is used instead of the default
    t.count = 10
    t.smoothed = 0.05
    assert t.step(now=1.04)[0] == pytest.approx(0.02 * 0.07000000029802322 + 0.9300000071525574 * 0.05, abs=1e-15)
    # the clamp: dt > 0.066 becomes 0.066
    t = s.ProcessTime(None)
    t.use_real = 1
    t.last, t.smoothed, t.count = 0.0, 0.5, 10
    assert t.step(now=0.4)[0] == s.REAL_DT_MAX


# ===============================================================================================================
# 6. gcgame.getresolutionwidth / getresolutionheight   (bodies 0x4367b0 / 0x4367d0 -> 0x43ae50 / 0x43ae70)
# ===============================================================================================================
def test_resolution_asm_pins(disasm, image):
    # the screen object is [0x70fa64] (set by ScScreenManager::AddScreen, 0x49df87); width is +0x10, height +0x14 (floats)
    pin(disasm, 0x43ae50, 0x43ae6d, {
        0x43ae54: "mov edx, dword ptr [0x70fa64]", 0x43ae5a: "add edx, 0x10", 0x43ae5d: "fld dword ptr [edx]",
        0x43ae5f: "fstp st(2)", 0x43ae61: "fld dword ptr [edx + 0x4]", 0x43ae64: "fstp st(1)", 0x43ae66: "fld st(1)",
        0x43ae68: "fstp st(2)", 0x43ae6a: "fstp st(0)", 0x43ae6c: "ret"})
    pin(disasm, 0x43ae70, 0x43ae89, {
        0x43ae72: "mov edx, dword ptr [0x70fa64]", 0x43ae78: "add edx, 0x10", 0x43ae7b: "fld dword ptr [edx]",
        0x43ae7d: "fstp st(1)", 0x43ae7f: "fld dword ptr [edx + 0x4]", 0x43ae82: "fstp st(1)", 0x43ae84: "fld st(0)",
        0x43ae86: "fstp st(1)", 0x43ae88: "ret"})
    pin(disasm, 0x49df50, 0x49df91, {0x49df7b: "cmp dword ptr [0x70fa60], 0x1", 0x49df87: "mov dword ptr [0x70fa64], edx"})
    pin(disasm, 0x4367b0, 0x4367ce, {0x4367b3: "call 0x43ae50", 0x4367b9: "fstp dword ptr [esp]", 0x4367c0: "call 0x4077f0"})
    pin(disasm, 0x4367d0, 0x4367ee, {0x4367d3: "call 0x43ae70", 0x4367d9: "fstp dword ptr [esp]", 0x4367e0: "call 0x4077f0"})
    pin(disasm, 0x4077f0, 0x407815, {0x407800: "mov word ptr [ebx], 0x12", 0x40780b: "mov dword ptr [ebx + 0x4], edx"})
    # the cited default (INI -> M2a-3): LoadIni writes ResolutionW/H = "640"/"480" when the INI has none (0x5d4a5d-0x5d4a8b)
    pin(disasm, 0x5d4a5d, 0x5d4a95, {
        0x5d4a62: "push 0x7475a2", 0x5d4a67: "push 0x74755c", 0x5d4a6c: "push 0x747568", 0x5d4a71: "call dword ptr [0x771420]",
        0x5d4a7c: "push 0x7475a6", 0x5d4a81: "push 0x7475aa", 0x5d4a86: "push 0x747568", 0x5d4a8b: "call dword ptr [0x771420]"})
    assert image.read(0x74755c, 12) == b"ResolutionW\0" and image.read(0x7475aa, 12) == b"ResolutionH\0"
    assert image.read(0x7475a2, 4) == b"640\0" and image.read(0x7475a6, 4) == b"480\0"
    assert sv().DEFAULT_RESOLUTION == (640.0, 480.0)
    assert image.u32(0x70fa64) == 0                                  # .data: the screen is built at run time


def test_resolution_returns_floats_of_the_cited_default():
    for fn, want in (("getresolutionwidth", 640.0), ("getresolutionheight", 480.0)):
        r, vm, ctx, tr, log = run_main(".function main 0\nCallGameFunction gcgame.%s 0\nReturn\n" % fn)
        assert r == Variant(0x12, osi_vm.f32_bits(want)), fn
        assert log[0].startswith("00000000 gcgame.%s 0 -> 0012:" % fn)


def test_resolution_follows_the_screen_subsystem():
    s = sv()
    vm, ctx, tr, log = make_vm(".function main 0\nCallGameFunction gcgame.getresolutionwidth 0\nCallGameFunction "
                               "gcgame.getresolutionheight 0\nReturn\n")
    scr = ctx.subsystem(s.Screen)
    scr.width, scr.height = 1024.0, 768.0                            # M2a-3 will fill these from the INI
    r = vm.call("main", [])
    assert r.result == Variant(0x12, osi_vm.f32_bits(768.0))


# ===============================================================================================================
# 6b. the rows with no body in this group: debug.print (tier 2) and the three copy-kind natives (tier 3)
# ===============================================================================================================
def test_debug_print_and_copy_kind_asm_pins(disasm, image):
    # Debug.Print 0x61d5e0: a debug-enable test, "\n" appended to ITS argument copy, handed to the VM's slot 2; returns nothing
    pin(disasm, 0x61d5e0, 0x61d640, {0x61d5ef: "call 0x5d4f00", 0x61d601: "push 0x756948", 0x61d614: "call 0x4f5c80",
                                     0x61d639: "call dword ptr [esi + 0x8]"})
    pin(disasm, 0x61d63e, 0x61d646, {0x61d63e: "mov word ptr [edi], 0xf"})
    assert image.read(0x756948, 2) == b"\n\0"
    # the three copy-kind natives return through CreateVariant 0x6159e0, a bitwise copy of an 8-byte variant {type, value}
    pin(disasm, 0x6159e0, 0x6159fb, {0x6159ea: "mov dx, word ptr [ebx]", 0x6159ed: "mov word ptr [esi], dx",
                                     0x6159f0: "mov edx, dword ptr [ebx + 0x4]", 0x6159f3: "mov dword ptr [esi + 0x4], edx"})
    pin(disasm, 0x615990, 0x6159d9, {0x6159b1: "call 0x615f10", 0x6159c1: "call 0x6159e0"})
    # GetStringTable: SrBlockFileLoad, then 1 into the buffer's first dword, then {0x13, buffer}: nothing like a copy of arg 0
    pin(disasm, 0x61bbf0, 0x61bc47, {0x61bc1e: "call 0x54db60", 0x61bc29: "mov dword ptr [edx], 0x1",
                                     0x61bc32: "mov edx, 0x13", 0x61bc37: "mov word ptr [esi], dx"})
    pin(disasm, 0x615ab0, 0x615ab8, {0x615ab0: "push ebp"})


# ===============================================================================================================
# 7. the A9 path with the real models (needs the install)
# ===============================================================================================================
def test_oracle_runs_setup_with_the_services_models(tmp_path, lomn_data):
    py = [sys.executable, str(REPO / "tools" / "research" / "osi_vm.py"), "--run",
          str(lomn_data / "install" / "data" / "Base.osi"), "--frames", "2", "--real-natives", "--install",
          str(lomn_data / "install"), "--overrides", str(OVERRIDES), "--log-out", str(tmp_path / "n.log")]
    p = subprocess.run(py, capture_output=True, text=True, timeout=110)
    assert p.returncode == 0, p.stderr
    assert "stop: none" in p.stdout, p.stdout
    log = (tmp_path / "n.log").read_text().splitlines()
    mine = [l for l in log if re.search(r" (scslosifoundation|scslosifileio|gctimer|scprocessmanager|gcgame\.getresolution)", l)]
    names = {l.split()[1] for l in mine}
    assert {"scslosifoundation.stringtoidentifier", "gctimer.create", "scslosifileio.doesfileexist",
            "scprocessmanager.userealdeltatime", "gcgame.getresolutionwidth", "gcgame.getresolutionheight"} <= names
    assert any(re.match(r"^\d+ scslosifileio\.doesfileexist 1 -> 0010:00000000$", l) for l in mine)
    assert any(re.match(r"^\d+ gctimer\.create 3 -> 0027:[0-9a-f]{8} G 0$", l) for l in mine)
    assert any(re.match(r"^\d+ scslosifoundation\.stringtoidentifier 1 -> 0010:66726e74$", l) for l in mine)


def test_doesfileexist_dotdot_above_the_mount_root_is_zero(tmp_path):   # F7b item 6: both engines, decided behaviour
    inst = make_install(tmp_path, art=["foo.tga"], loose=["data/loose.txt", "User/data/p.sav"])
    assert exists(inst, "Root/Data/../Data/loose.txt")[0] == 1
    assert exists(inst, "Root/../Root/Data/loose.txt")[0] == 0                  # climbs above Root/ and returns
    assert exists(inst, "Root/Data/../../Root/Data/loose.txt")[0] == 0
    assert exists(inst, "Data/../../install/Data/loose.txt")[0] == 0           # the neither route (no User/): same rule
    assert exists(inst, "Root/../Root/User/data/p.sav")[0] == 0                # root route
    assert exists(inst, "User/../Root/Data/loose.txt")[0] == 0                 # user route is false anyway


def test_doesfileexist_hook_routes_by_has_user_needle(monkeypatch):   # review 7b n1: the helper is the one that decides
    s = sv()
    monkeypatch.setattr(s, "has_user_needle", lambda name: True)
    assert s.hook_route(b"a/b")[0] == "neither"                                # forced true: no "User/" in the name at all
    monkeypatch.setattr(s, "has_user_needle", lambda name: False)
    assert s.hook_route(b"Root/User/x") == ("original", "Root/User/x")
