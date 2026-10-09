"""M2a-2 T1: tools/osi/exc.py, the `.exc` exception-table decoder (docs/osi/opcodes.md "Exception tables").

Two kinds of test. The synthetic ones build a tiny image byte by byte from the document's record layouts and run
anywhere. The pinned ones read the canonical exe as data and skip with `SKIPPED (no LOMN_DATA)` without the game files.
The pinned catch pads were read from the exe with `exc.py --func`, and each is cross-checked against the document's
own numbers (opcodes.md 0x82 row: 0x601c94 / 0x601cb3 / 0x601cd3)."""
import importlib.util
import struct
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]


def _load():
    spec = importlib.util.spec_from_file_location("osi_exc", REPO / "tools" / "osi" / "exc.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


exc = _load()

INDEX = 0x500
STR = 0x2000


def catch(type_ptr, pad, frame, end=False):
    return struct.pack("<HIIi", 0x10 | (0x8000 if end else 0), type_ptr, pad, frame)


def dtor(fn, frame, end=False):
    return struct.pack("<HiI", 0x01 | (0x8000 if end else 0), frame, fn)


def cleanup_0a(end=False):
    return struct.pack("<H", 0x0A | (0x8000 if end else 0)) + b"\xaa" * 8


def dtor_0c(fn, frame, end=False):
    return struct.pack("<HiII", 0x0C | (0x8000 if end else 0), frame, fn, 0)


def spec_13(types, unexpected, frame, end=False):
    return struct.pack("<HHIi", 0x13 | (0x8000 if end else 0), len(types), unexpected, frame) + b"".join(
        struct.pack("<I", t) for t in types)


def full_info(table, sites, flags=0xC8):
    return struct.pack("<BIH", flags, table, len(sites)) + b"".join(struct.pack("<II", c, p) for c, p in sites)


def synth():
    """Functions 0x400100 (full, 2 sites), 0x400200 (flags 0x09: 4 bytes, followed by bytes that LOOK like sites),
    0x400300 (flags 0x00, null pointer: 4 bytes although bit 0 is clear), 0x400400 (a 0x0b record in the middle),
    0x400500 (every known size in one chain, the catch last)."""
    mem = bytearray(0x3000)
    cursor = [0x1000]

    def put(b):
        a = cursor[0]
        mem[a:a + len(b)] = b
        cursor[0] += len(b)
        return a

    msg = STR
    mem[msg:msg + 17] = b"!ScCatchMessage!\0"
    # function 0x400100
    chain_a = put(catch(msg, 0x400180, -16) + catch(0, 0x4001A0, -16) + dtor(0x500000, -32, end=True))
    chain_b = put(dtor(0x500010, -48, end=True))
    info_a = put(full_info(0x1000, [(0x400110, chain_a), (0x400120, chain_b)]))
    # function 0x400200: short, followed by what a decoder that ignores the flag would read as a site table
    info_b = put(b"\x09\0\0\0" + struct.pack("<HII", 1, 0x400210, chain_a))
    # function 0x400300: bit 0 clear, null table pointer -> 4 bytes
    info_c = put(b"\x00\0\0\0" + struct.pack("<HII", 1, 0x400310, chain_a))
    # function 0x400400: a catch, then an unknown kind 0x0b (size unknown), then a catch that must NOT be reported
    chain_d = put(catch(msg, 0x400480, -8) + struct.pack("<H", 0x0B) + b"\x00" * 12 + catch(msg, 0x4004F0, -8, True))
    info_d = put(full_info(0x1000, [(0x400410, chain_d)]))
    # function 0x400500: 0x0a (10) + 0x0c (14) + 0x13 with 2 types (20) + 0x11 (6) + catch(...) with the end bit
    chain_e = put(cleanup_0a() + dtor_0c(0x500020, -4) + spec_13([msg, 0], 0x400590, -12)
                  + struct.pack("<Hi", 0x11, -12) + catch(0, 0x4005A0, -12, True))
    info_e = put(full_info(0x1000, [(0x400510, chain_e)]))
    idx = [(0x400100, info_a), (0x400200, info_b), (0x400300, info_c), (0x400400, info_d), (0x400500, info_e)]
    body = b"".join(struct.pack("<II", f, i) for f, i in idx) + struct.pack("<I", 0xFFFFFFFF)
    mem[INDEX:INDEX + len(body)] = body
    return exc.Image([(0, bytes(mem))])


@pytest.fixture(scope="module")
def img():
    return synth()


def test_index_stops_at_terminator(img):
    idx = exc.load_index(img, INDEX)
    assert sorted(idx) == [0x400100, 0x400200, 0x400300, 0x400400, 0x400500]


def test_full_record_sites_and_catches(img):
    d = exc.decode_function(img, 0x400100, exc.load_index(img, INDEX))
    assert d["short"] is False and d["flags"] == 0xC8 and len(d["sites"]) == 2
    assert [s["call"] for s in d["sites"]] == [0x400110, 0x400120]
    assert [r["pad"] for r in d["catches"]] == [0x400180, 0x4001A0]
    assert d["catches"][0]["type"] == "!ScCatchMessage!" and d["catches"][1]["type"] == ""
    assert d["truncated"] == 0


def test_bit0_set_is_a_four_byte_record_with_no_call_sites(img):
    """The document's rule: a decoder that ignores bit 0 would read the 0x400210 site and its chain."""
    d = exc.decode_function(img, 0x400200, exc.load_index(img, INDEX))
    assert d["short"] is True and d["sites"] == [] and d["catches"] == []


def test_null_table_pointer_is_also_four_bytes_although_bit0_is_clear(img):
    """361 entries of the canonical exe look like this (flags 0x00/0x08/0x48/...; bytes 1..3 zero)."""
    d = exc.decode_function(img, 0x400300, exc.load_index(img, INDEX))
    assert d["short"] is True and d["sites"] == [] and d["catches"] == []


def test_unknown_kind_truncates_the_chain_and_is_reported(img):
    d = exc.decode_function(img, 0x400400, exc.load_index(img, INDEX))
    assert [r["pad"] for r in d["catches"]] == [0x400480]          # the catch behind the 0x0b is not reached
    assert d["truncated"] == 1
    lines = exc.format_function(d)
    assert any("unknown kind 0x0b" in ln for ln in lines) and lines[-1].endswith("truncated 1")


def test_record_sizes_of_every_known_kind(img):
    """0x0a = 10, 0x0c = 14, 0x13 = 12 + 4 count, 0x11 = 6: the catch behind all of them is found at the right place."""
    d = exc.decode_function(img, 0x400500, exc.load_index(img, INDEX))
    recs = d["sites"][0]["records"]
    assert [r["kind"] for r in recs] == [0x0A, 0x0C, 0x13, 0x11, 0x10]
    assert [r["size"] for r in recs] == [10, 14, 20, 6, 14]
    assert recs[2]["types"] == ["!ScCatchMessage!", ""]
    assert recs[-1]["end"] is True
    assert [r["pad"] for r in d["catches"]] == [0x4005A0]


def test_missing_function(img):
    d = exc.decode_function(img, 0x999999, exc.load_index(img, INDEX))
    assert exc.format_function(d) == ["func 0x00999999 no .exc entry"]


def test_output_is_one_line_per_record(img):
    lines = exc.format_function(exc.decode_function(img, 0x400100, exc.load_index(img, INDEX)))
    assert lines[0] == "func 0x00400100 info 0x%08x flags 0xc8 table 0x00001000 sites 2" % exc.decode_function(
        img, 0x400100, exc.load_index(img, INDEX))["info"]
    assert sum(1 for ln in lines if ln.startswith("site ")) == 2
    assert sum(1 for ln in lines if ln.startswith("  rec ")) == 4          # 3 + 1 records over the two chains
    assert lines[-1] == "catches 2 pads 0x00400180 0x004001a0"


# ----------------------------------------------------------------------------------------------- the canonical exe

@pytest.fixture(scope="module")
def real(canonical_exe):
    image = exc.Image.from_file(canonical_exe)
    return image, exc.load_index(image)


def pads(real, func):
    image, idx = real
    return sorted({r["pad"] for r in exc.decode_function(image, func, idx)["catches"]})


def test_real_index_size(real):
    """opcodes.md: 7,002 index entries have an info record; 4,605 are 4 bytes by bit 0."""
    image, idx = real
    assert len(idx) == 7002
    assert sum(1 for i in idx.values() if image.read(i, 1)[0] & 1) == 4605


def test_real_short_records_are_exactly_those_with_zero_bytes_1_to_3(real):
    """The rule the decoder uses (bit 0 set, or bytes 1..3 zero) is the same set as 'bytes 1..3 zero': 4,605 + 361."""
    image, idx = real
    short = [f for f, i in idx.items() if image.read(i + 1, 3) == b"\0\0\0"]
    assert len(short) == 4966
    assert all(exc.decode_function(image, f, idx)["short"] for f in short[:200])
    assert not any(exc.decode_function(image, f, idx)["short"] for f in list(set(idx) - set(short))[:200])


def test_real_0x6015a0_is_the_call_game_function_handler(real):
    """BCLCallGameFunction: function info 0x6dafe4; the outer catch pads 0x601c94 / 0x601cb3 / 0x601cd3 (opcodes.md 0x82
    row) plus the native call's own inner pads 0x601b10 / 0x601b30 / 0x601b50."""
    image, idx = real
    d = exc.decode_function(image, 0x6015A0, idx)
    assert d["info"] == 0x6DAFE4 and d["short"] is False and d["truncated"] == 0
    got = sorted({r["pad"] for r in d["catches"]})
    assert got == [0x601B10, 0x601B30, 0x601B50, 0x601C94, 0x601CB3, 0x601CD3]
    types = {r["pad"]: r["type"] for r in d["catches"]}
    assert types[0x601C94] == types[0x601B10] == "!ScCatchMessage!"
    assert types[0x601CB3] == "!exception!"
    assert types[0x601CD3] == ""                                           # catch (...)


def test_real_setupscripting_catches(real):
    assert pads(real, 0x4390D0) == [0x439324, 0x439343, 0x439363]


def test_real_winmain_catches(real):
    assert pads(real, 0x401020) == [0x401080, 0x401093, 0x4010B0]          # _WinMain@16


def test_real_run_has_no_catches(real):
    """`Run` 0x60b850 has a 4-byte record (flags 0x09): nothing is caught or destroyed."""
    image, idx = real
    d = exc.decode_function(image, 0x60B850, idx)
    assert d["short"] is True and d["flags"] == 0x09 and d["catches"] == []


def test_cli_func_prints_the_three_pads(canonical_exe):
    r = subprocess.run([sys.executable, str(REPO / "tools" / "osi" / "exc.py"), "--exe", str(canonical_exe),
                        "--func", "0x6015a0"], capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stderr
    last = r.stdout.splitlines()[-1]
    assert last == ("catches 6 pads 0x00601b10 0x00601b30 0x00601b50 0x00601c94 0x00601cb3 0x00601cd3")
    assert r.stdout.startswith("func 0x006015a0 info 0x006dafe4 flags 0xc8")


def test_cli_chain_several_functions(canonical_exe):
    r = subprocess.run([sys.executable, str(REPO / "tools" / "osi" / "exc.py"), "--exe", str(canonical_exe),
                        "--chain", "0x4390d0", "0x401020"], capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stderr
    assert [ln for ln in r.stdout.splitlines() if ln.startswith("catches")] == [
        "catches 3 pads 0x00439324 0x00439343 0x00439363", "catches 3 pads 0x00401080 0x00401093 0x004010b0"]
