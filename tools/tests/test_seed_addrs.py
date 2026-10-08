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


def push(v):
    return b"\x68" + struct.pack("<I", v)


def call(at_va, target):
    return b"\xE8" + struct.pack("<i", target - (at_va + 5))


def synthetic_pe():
    """Minimal PE32: .text (RVA 0x1000, 0x200 bytes) and .CRT (RVA 0x2000, 3 dwords). Returns (bytes, expected)."""
    code = bytearray()

    def emit(b):
        code.extend(b)

    def here():
        return TEXT_VA + len(code)

    # site 1: pushes text, non-text, text, non-text, non-text -> 2 .text candidates, the last one is the fn pointer
    emit(push(0x401100))
    emit(push(CRT_VA))                # .CRT address: not .text
    emit(push(0x401120))
    emit(push(0x00403000))            # outside every section
    emit(push(0x00403004))
    site1 = here()
    emit(call(site1, FAKE_REGF))
    # site 2: one .text candidate; site 1's pushes are behind the previous registration site and must not count
    emit(b"\x8B\xC8")                 # mov ecx, eax (filler)
    emit(push(0x401140))
    emit(push(0x00403008))
    site2 = here()
    emit(call(site2, FAKE_REGF))
    # a RegisterType call and a decoy call elsewhere in .text
    tsite = here()
    emit(call(tsite, FAKE_REGT))
    emit(call(here(), 0x401160))
    emit(b"\xC3")
    text = bytes(code).ljust(0x200, b"\xCC")
    text = text[:0x180] + b"\xC3" + text[0x181:0x1A0] + b"\xC3" + text[0x1A1:]
    crt = struct.pack("<III", 0x401000, 0x00000000, 0x401060).ljust(0x200, b"\0")

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
    expected = dict(site1=site1, site2=site2, tsite=tsite)
    return exe, expected


def test_synthetic_pe():
    exe, e = synthetic_pe()
    r = seed_addrs.compute(exe, register_function=FAKE_REGF, register_type=FAKE_REGT)
    assert r["crt"] == [0x401000, 0x401060]
    assert r["reg_sites"] == [
        {"site": e["site1"], "fnptr": 0x401120, "candidates": 2},
        {"site": e["site2"], "fnptr": 0x401140, "candidates": 1},
    ]
    assert r["type_sites"] == [e["tsite"]]
    assert r["seeds"] == [0x401000, 0x401060, 0x401120, 0x401140]


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


@pytest.fixture(scope="module")
def real(canonical_exe):
    return seed_addrs.compute(canonical_exe.read_bytes())


def test_real_exe_counts(real):
    assert len(real["crt"]) == 391
    assert len(real["reg_sites"]) == 1056
    assert len(real["type_sites"]) == 445
    fnptrs = [s["fnptr"] for s in real["reg_sites"]]
    assert len(set(fnptrs)) == 1056
    assert real["seeds"] == sorted(set(real["crt"]) | set(fnptrs))
    assert len(real["seeds"]) == 1447


def test_real_exe_first_site_in_initializer(real):
    # The first registration of an initializer also sees the earlier ScInitFunc pushes (several .text candidates within
    # the 256-byte walk-back window); the fn pointer is the last .text push before the call. Later registrations see one.
    assert real["reg_sites"][0] == {"site": 0x402FEB, "fnptr": 0x402030, "candidates": 5}
    assert real["reg_sites"][1] == {"site": 0x4030CE, "fnptr": 0x402110, "candidates": 1}
    assert sum(1 for s in real["reg_sites"] if s["candidates"] == 1) == 988


def test_real_exe_corpus_fnptrs_are_seeds(real, gen_decomp):
    starts = {int(json.loads(line)["addr"], 16)
              for line in (gen_decomp / "functions.jsonl").read_text().splitlines() if line}
    in_corpus = {s["fnptr"] for s in real["reg_sites"]} & starts
    assert len(in_corpus) == 79
    assert in_corpus <= set(real["seeds"])


def test_cli_writes_outputs(canonical_exe, real, tmp_path):
    seed_addrs.main([str(canonical_exe), str(tmp_path)])
    lines = (tmp_path / "seed_addrs.txt").read_text().splitlines()
    assert lines == ["0x%08x" % a for a in real["seeds"]]
    sites = json.loads((tmp_path / "reg_sites.json").read_text())
    assert len(sites) == 1056
