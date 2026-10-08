"""seed_addrs: .CRT initializers + RegisterFunction fn pointers, read from the exe bytes (data only, nothing executed)."""
import json
import struct

import pytest

import seed_addrs

BASE = 0x400000
TEXT_VA = BASE + 0x1000
CRT_VA = BASE + 0x2000
FAKE_REGF = TEXT_VA + 0x180
FAKE_REGT = TEXT_VA + 0x1A0
CRT_SLACK_PTR = TEXT_VA + 0x80  # in .CRT raw bytes past its vsz


def push(v):
    return b"\x68" + struct.pack("<I", v)


def call(at_va, target):
    return b"\xE8" + struct.pack("<i", target - (at_va + 5))


PROLOGUE = b"\x55\x89\xE5"  # push ebp; mov ebp, esp


def synthetic_pe(prologue=True):
    """Minimal PE32: .text (RVA 0x1000, 0x200 bytes) and .CRT (RVA 0x2000, vsz 3 dwords, raw 0x200 bytes with a .text
    pointer in the slack past vsz). Returns (bytes, expected).
    The registrations sit in one function with an aligned frame prologue at 0x401010 (prologue=False: int3 instead),
    preceded in the walk-back by an unaligned decoy prologue that must be skipped."""
    code = bytearray()

    def emit(b):
        code.extend(b)

    def here():
        return TEXT_VA + len(code)

    emit(b"\xCC" * 0x10)
    enclosing = here()                # 0x401010, 16-byte aligned
    emit(PROLOGUE if prologue else b"\xCC" * 3)
    emit(b"\x90")
    decoy = here()                    # 0x401014: not 16-byte aligned
    emit(PROLOGUE)
    # site 1: pushes text, non-text, text, non-text, non-text -> 2 .text candidates, the last one is the fn pointer
    emit(push(0x401100))
    emit(push(CRT_VA))                # .CRT address: not .text
    emit(push(0x401120))
    emit(push(0x00403000))            # outside every section
    emit(push(0x00403004))
    site1 = here()
    emit(call(site1, FAKE_REGF))
    # site 2: one .text candidate; site 1's pushes are behind the previous registration site and must not count.
    # The fn pointer push is the instruction immediately before the E8 (it ends exactly at the site).
    emit(b"\x8B\xC8")                 # mov ecx, eax (filler)
    emit(push(0x00403008))
    emit(push(0x401140))
    site2 = here()
    emit(call(site2, FAKE_REGF))
    # a RegisterType call and a decoy call elsewhere in .text
    tsite = here()
    emit(call(tsite, FAKE_REGT))
    emit(call(here(), 0x401160))
    emit(b"\xC3")
    text = bytes(code).ljust(0x200, b"\xCC")
    text = text[:0x180] + b"\xC3" + text[0x181:0x1A0] + b"\xC3" + text[0x1A1:]
    # .CRT: vsz 0xC (3 dwords); a .text pointer in the raw padding past vsz is not mapped and must be ignored
    crt = struct.pack("<IIII", 0x401000, 0x00000000, 0x401060, CRT_SLACK_PTR).ljust(0x200, b"\0")

    hdr = bytearray(0x400)
    hdr[0:2] = b"MZ"
    pe = 0x80
    struct.pack_into("<I", hdr, 0x3C, pe)
    hdr[pe:pe + 4] = b"PE\0\0"
    struct.pack_into("<HH", hdr, pe + 4, 0x14C, 2)          # machine, NumberOfSections
    struct.pack_into("<H", hdr, pe + 20, 0xE0)               # SizeOfOptionalHeader
    opt = pe + 24
    struct.pack_into("<H", hdr, opt, 0x10B)
    struct.pack_into("<I", hdr, opt + 28, BASE)              # ImageBase
    sec = opt + 0xE0
    for i, (name, rva, size, raw) in enumerate(((b".text", 0x1000, 0x200, 0x400), (b".CRT", 0x2000, 0xC, 0x600))):
        o = sec + 40 * i
        hdr[o:o + 8] = name.ljust(8, b"\0")
        struct.pack_into("<IIII", hdr, o + 8, size, rva, 0x200, raw)
    exe = bytes(hdr) + text + crt
    assert enclosing % 16 == 0 and decoy % 16 != 0
    expected = dict(site1=site1, site2=site2, tsite=tsite, enclosing=enclosing)
    return exe, expected


def test_synthetic_pe():
    exe, e = synthetic_pe()
    r = seed_addrs.compute(exe, register_function=FAKE_REGF, register_type=FAKE_REGT)
    assert r["crt"] == [0x401000, 0x401060]
    assert r["reg_sites"] == [
        {"site": e["site1"], "fnptr": 0x401120, "candidates": 2, "enclosing": 0x401010},
        {"site": e["site2"], "fnptr": 0x401140, "candidates": 1, "enclosing": 0x401010},
    ]
    assert r["type_sites"] == [e["tsite"]]
    assert r["seeds"] == [0x401000, 0x401010, 0x401060, 0x401120, 0x401140]


def test_synthetic_pe_without_aligned_prologue_raises():
    exe, _ = synthetic_pe(prologue=False)  # only the unaligned decoy remains
    with pytest.raises(ValueError, match="no 16-byte aligned frame prologue"):
        seed_addrs.compute(exe, register_function=FAKE_REGF, register_type=FAKE_REGT)


def test_default_constants_cite_symbols():
    # gen/decomp/symbols.txt: 0x5fab30 ?RegisterFunction@ScOSISystem@@..., 0x5fae00 ?RegisterType@ScOSISystem@@...
    assert seed_addrs.REGISTER_FUNCTION == 0x5FAB30
    assert seed_addrs.REGISTER_TYPE == 0x5FAE00


def test_refuses_non_canonical_exe(tmp_path):
    exe, _ = synthetic_pe()
    p = tmp_path / "LEGOBionicle.exe"
    p.write_bytes(exe)
    with pytest.raises(SystemExit, match="not the canonical exe"):
        seed_addrs.main([str(p), str(tmp_path / "out")])
    assert not (tmp_path / "out").exists()


def test_refuses_before_parsing(tmp_path):
    # Non-PE bytes: the sha check must come first, so this is the refusal, not a parse error.
    p = tmp_path / "garbage.exe"
    p.write_bytes(b"garbage")
    with pytest.raises(SystemExit, match="not the canonical exe"):
        seed_addrs.main([str(p), str(tmp_path / "out")])
    assert not (tmp_path / "out").exists()


@pytest.fixture(scope="module")
def real(canonical_exe):
    return seed_addrs.compute(canonical_exe.read_bytes())


def test_real_exe_counts(real):
    assert len(real["crt"]) == 391
    assert len(real["reg_sites"]) == 1056
    assert len(real["type_sites"]) == 445
    fnptrs = [s["fnptr"] for s in real["reg_sites"]]
    assert len(set(fnptrs)) == 1056
    enclosing = {s["enclosing"] for s in real["reg_sites"]}
    assert real["seeds"] == sorted(set(real["crt"]) | set(fnptrs) | enclosing)
    assert len(real["seeds"]) == 1448  # 1447 before the enclosing seeds (M2a-0 T2): only 0x61d6a0 is new


def test_real_exe_enclosing(real):
    """Registrations sit in 70 functions, one contiguous run of sites each; 69 are .CRT initializers and the 70th,
    0x61d6a0 (ScSLOSIDebug::Print/Assert), is an ScInitFunc callback that only the enclosing seed makes a function."""
    enc = [s["enclosing"] for s in real["reg_sites"]]
    runs = [enc[0]] + [b for a, b in zip(enc, enc[1:]) if a != b]
    assert len(set(enc)) == 70 and len(runs) == 70
    assert set(enc) - set(real["crt"]) == {0x61D6A0}
    assert max(s["site"] - s["enclosing"] for s in real["reg_sites"]) == 28916  # in 0x41b640 (29,050 B)


def test_real_exe_first_site_in_initializer(real):
    # The first registration of an initializer also sees the earlier ScInitFunc pushes (several .text candidates within
    # the 256-byte walk-back window); the fn pointer is the last .text push before the call. Later registrations see one.
    assert real["reg_sites"][0] == {"site": 0x402FEB, "fnptr": 0x402030, "candidates": 5, "enclosing": 0x402890}
    assert real["reg_sites"][1] == {"site": 0x4030CE, "fnptr": 0x402110, "candidates": 1, "enclosing": 0x402890}
    assert sum(1 for s in real["reg_sites"] if s["candidates"] == 1) == 988


def test_real_exe_seeds_are_corpus_functions(real, gen_decomp):
    """After the M2a-0 seeded decomp, every registered fn pointer and every .CRT initializer is a function start."""
    starts = {int(json.loads(line)["addr"], 16)
              for line in (gen_decomp / "functions.jsonl").read_text().splitlines() if line}
    fnptrs = {s["fnptr"] for s in real["reg_sites"]}
    # 79 in the pre-M2a-0 corpus
    assert len(fnptrs & starts) == 1056
    assert len(set(real["crt"]) & starts) == 391


def test_cli_writes_outputs(canonical_exe, real, tmp_path):
    seed_addrs.main([str(canonical_exe), str(tmp_path)])
    lines = (tmp_path / "seed_addrs.txt").read_text().splitlines()
    assert lines == ["0x%08x" % a for a in real["seeds"]]
    sites = json.loads((tmp_path / "reg_sites.json").read_text())
    assert len(sites) == 1056
