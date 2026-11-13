"""M2a-3 T7b: independent .col/.fnt reader (tools/research/colfont.py), written from probe 2's tables
(docs/knowledge/research/2026-10-08-m2a0-probe2-fonts.md) and docs/osi/hud.md 3.6 / 4.4 only.
The A11 values below are absolute (plan Task 7). Data-dependent tests skip without LOMN_DATA."""
import struct
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "research"))
import colfont  # noqa: E402

FONTS = ["BionicleFont", "BionicleFontHi", "BionicleFontSh", "BionicleFontshad", "Tech_Demo_Font01",
         "Tech_Demo_Font_Highlighted", "tech_font_01", "tech_font_01_hilight"]


# ---- data-free units ----
def test_hash_is_x83_u32():
    assert colfont.hash_name("") == 0
    assert colfont.hash_name("a") == 97
    assert colfont.hash_name("ab") == 97 * 0x83 + 98
    assert colfont.hash_name("a" * 8) == (sum(97 * 0x83 ** i for i in range(8))) & 0xFFFFFFFF  # wraps u32
    assert colfont.hash_name("A") != colfont.hash_name("a")  # case-sensitive


def test_lzss_ring_literals_and_backref():
    # flag 0b00000111 (LSB first): 3 literals "abc", then a ref. Ring starts 0x20-filled, cursor 0xfee: abc go to 0xfee..0xff0.
    # ref pos=0xfee len=3 -> b0=0xee, b1=(0xf<<4 | (3-3)) = 0xf0 -> copies "abc" again.
    data = bytes([0x07, ord("a"), ord("b"), ord("c"), 0xEE, 0xF0])
    assert colfont.lzss_decode(data, 6) == b"abcabc"
    # a back-reference into the 0x20-filled ring start: pos 0 len 4 -> four spaces
    assert colfont.lzss_decode(bytes([0x00, 0x00, 0x01]), 4) == b"    "


def test_lzss_ring_tail_not_spaces():
    # M6: decoder 0x4d2920 fills only 0..0xFED with 0x20 (0x4d2955..0x4d2965); 0xFEE..0xFFF are uninitialised heap (modelled 0, INFERRED)
    assert colfont.lzss_decode(bytes([0x00, 0xEE, 0xF0]), 3) == b"\0\0\0"      # ref pos 0xFEE len 3, nothing written yet
    assert colfont.lzss_decode(bytes([0x00, 0xED, 0xE0]), 3) == b"   "          # pos 0xFED is still a space


def _col(entries):
    """entries: [(name, data)] -> .col bytes, stored (flags 0x01), directory sorted nondecreasing by hash (stable)."""
    ents = sorted(entries, key=lambda e: colfont.hash_name(e[0]))
    off = 0x24 + 16 * len(ents)
    d, body = b"", b""
    for n, data in ents:
        d += struct.pack(">4I", colfont.hash_name(n), 1, off + len(body), len(data))
        body += data
    return struct.pack(">4I", 0, len(ents), 4, off + len(body)) + bytes(0x14) + d + body


def test_col_duplicate_hash_returns_lowest_entry():
    # M6: GetInfo 0x4cfc6b is a lower-bound search (ja -> lo = mid, else hi = mid): the LOWEST of equal hashes wins
    col = colfont.Col(_col([("x", b"AAAA"), ("x", b"BBBB"), ("y", b"CC"), ("w", b"D")]))
    assert [e.hash for e in col.entries] == sorted(e.hash for e in col.entries)
    assert col.read("x") == b"AAAA"
    assert col.read("y") == b"CC" and col.read("w") == b"D"
    assert col.find("nope") is None


def _fnt(rows, cols, n, cw, ch, kern, base, space, recs):
    b = struct.pack("<8i", rows, cols, n, cw, ch, kern, base, space)
    for a, w, h, xo, yo in recs:
        b += struct.pack("<Hiiii", a, w, h, xo, yo)
    return b


def test_fnt_parse_and_widths_synthetic():
    f = colfont.Fnt(_fnt(1, 3, 3, 16, 16, 2, 12, 8, [(65, 10, 11, 0, 1), (66, 5, 11, 0, 1), (65, 99, 1, 0, 0)]))
    assert (f.rows, f.columns, f.n, f.char_w, f.char_h, f.kerning, f.baseline, f.space) == (1, 3, 3, 16, 16, 2, 12, 8)
    assert (f.max_width, f.min_width) == (99, 5)
    assert f.get_width(b"A") == 12                      # first match wins (width 10, not 99), + kerning
    assert f.get_width(b" ") == 10                      # space + kerning
    assert f.get_width(b"Z") == f.max_width + 2         # unknown: maxWidth + kerning
    assert f.get_str_len(b"AB") == (12 + 7) - 2         # minus trailing kerning
    assert f.get_str_len(b"A\nBBBB") == 10              # stops at newline
    assert f.get_str_len(b"AB", 2.0) == 34              # round((12+7)*2 - 2*2)
    assert f.get_str_len(b"") == -2                     # initial sum 0, minus kerning (formula as read; INFERRED edge)


def _kab():
    return colfont.Fnt(_fnt(1, 3, 3, 16, 16, 2, 12, 8, [(65, 10, 11, 0, 1), (66, 5, 11, 0, 1), (0xF1, 4, 1, 0, 0)]))


def test_get_str_len_chops_f32_sum():
    # I1: x87 RC = chop (0x628b76/0x628bc1), fistp qword, low dword. f32 steps (0x628ba0): 8.4 -> 8.3999996, +4.9 -> 13.299999; minus 2*0.7
    assert _kab().get_str_len(b"AB", 0.7) == 11        # 11.899999 chops to 11; round() gave 12
    big = colfont.Fnt(_fnt(1, 2, 2, 16, 16, 0, 12, 8, [(65, 2**24, 1, 0, 0), (66, 1, 1, 0, 0)]))
    assert big.get_str_len(b"AB") == 2**24             # the running sum is binary32: 2^24 + 1 is not representable (0x628ba0)


def test_escape_skips_next_char_and_its_trail():
    # I2: 0xFF skips itself AND the next char (0x628b2c..0x628b5c, then idx++ at 0x628ba6); a lead next also skips its trail
    f = _kab()
    assert f.get_str_len(b"A\xffB") == 10 == f.get_str_len(b"A")          # (2+10) - 2: only A counts
    assert f.get_str_len(b"A\xff\xf1ZB") == (12 + 7) - 2                   # F1 and its trail Z skipped, B counts
    assert f.codes(b"A\xff") == [65] and f.codes(b"\xffAB") == [66]


def test_two_byte_code_is_lead_shl8_plus_trail():
    # I3: 0x628b0b..0x628b25: code = lead<<8 + next; the trail byte is at the CURRENT idx afterwards, so it is what 0xFF / 0x0A test
    f = _kab()
    assert f.codes(b"\xf1A") == [0xF141]
    assert f.codes(b"\xf1") == [0xF100]                  # lead last: the byte past the end (terminator, INFERRED 0) is the trail
    assert f.codes(b"\xf1\n") == [] and f.codes(b"\xf1\xffAB") == [66]
    assert f.get_str_len(b"\xf1") == (10 + 2) - 2         # 0xF100 is unknown (the 0xF1 record is code 0x00F1): maxWidth + kerning


def test_unsigned_widths_and_bounds():
    # M4: glyph width = u32(kerning + width) (0x6289f9/0x628a05), unknown = u32(maxWidth + kerning) (0x628a20), space stays i32 (0x6289b7)
    f = colfont.Fnt(_fnt(1, 1, 1, 16, 16, -4, 12, 8, [(65, 3, 11, 0, 1)]))
    assert f.get_width(b"A") == 4294967295.0
    assert f.get_width(b"Z") == (3 - 4) % 2**32        # u32(max 3 + kern -4)
    assert f.get_width(b" ") == 4.0                    # signed: 8 + -4
    g = colfont.Fnt(_fnt(1, 2, 2, 16, 16, 0, 12, -8, [(65, 5, 1, 0, 0), (66, -1, 1, 0, 0)]))
    assert g.get_width(b" ") == -8.0
    assert (g.max_width, g.min_width) == (0xFFFFFFFF, 5)   # unsigned compares, 0x6277e0 jbe / 0x627810 jae
    e = colfont.Fnt(_fnt(0, 0, 0, 16, 16, 0, 12, 8, []))
    assert (e.max_width, e.min_width) == (0, 0xFFFFFFFF)   # init 0x626d71 / 0x626d7e


def test_str_height_skips_unknown_and_space():
    # glyph: height 11, yOffset 1, baseline 12: below = 12-1 = 11, above = 11-(12-1) = 0 -> 11
    f = colfont.Fnt(_fnt(1, 1, 1, 16, 16, 0, 12, 8, [(65, 10, 11, 0, 1)]))
    assert f.get_str_height(b"A") == 11
    assert f.get_str_height(b"A A") == 11               # space skipped (hud.md §4.9 ruling)
    assert f.get_str_height(b"A", 1.6) == 17             # 6.6 + 11 = 17.6: truncated (0x628ccd chop), round gives 18


def test_str_height_unsigned_gates():
    # M4: baseline 4, glyph h 3, yOffset -2: d = u32(6) but base < yo unsigned (0x628d76 jb) so below stays 0; cand = h (0x628dae jbe)
    f = colfont.Fnt(_fnt(1, 1, 1, 16, 16, 0, 4, 8, [(65, 5, 3, 0, -2)]))
    assert f.get_str_height(b"A") == 3


def test_str_height_space_with_record_updates_below():
    # M5: GetHeight(' ') = 0.0 (0x628a67) but the record's yOffset still feeds below; only a code with NO record is skipped (hud.md 4.9)
    f = colfont.Fnt(_fnt(1, 2, 2, 16, 16, 0, 10, 8, [(0x20, 5, 9, 0, 2), (65, 5, 4, 0, 9)]))
    assert f.get_str_height(b" A") == 11               # below 10-2=8, above 4-(10-9)=3
    assert f.get_str_height(b"A") == 3 + 1             # without the space record: below 1


# ---- data-dependent ----
@pytest.fixture(scope="module")
def fontdir(lomn_data):
    p = lomn_data / "install" / "data" / "Art" / "Fonts"
    if not p.is_dir():
        pytest.skip("SKIPPED (no install fonts)")
    return p


@pytest.fixture(scope="module")
def fonts(fontdir):
    return {n: colfont.load_font(fontdir / (n + ".col"), n) for n in FONTS}


def test_all_eight_fonts_found(fontdir):
    assert [n for n, _ in colfont.list_fonts(fontdir)] == sorted(FONTS)


def test_structure_every_font(fonts, fontdir):
    for n, ft in fonts.items():
        col = ft.col
        assert col.total == (fontdir / (n + ".col")).stat().st_size, n
        assert col.sorted_by_hash, n
        assert ft.fnt.n == ft.fnt.rows * ft.fnt.columns, n
        assert ft.fnt.raw_size == 32 + 18 * ft.fnt.n == col.entry_for(n + ".fnt").usize, n
        # every record owns a directory entry <Name><row>_<col>.tga that decodes as a char_w x char_h 32bpp TGA
        for i in range(ft.fnt.n):
            t = ft.glyph_tga(i)
            assert (t.w, t.h, t.bpp) == (ft.fnt.char_w, ft.fnt.char_h, 32), (n, i)


def test_probe2_table_pins(fonts):
    # (dir entries, glyphs, tile, kerning, baseline, space) from probe 2 section 2 (all re-measured here, not copied code)
    want = {"BionicleFont": (153, 145, 16, 0, 12, 8), "BionicleFontHi": (86, 79, 32, 4, 12, 16),
            "BionicleFontSh": (87, 79, 32, 4, 12, 16), "BionicleFontshad": (87, 79, 32, 4, 12, 16),
            "Tech_Demo_Font01": (102, 46, 32, 8, 12, 16), "Tech_Demo_Font_Highlighted": (104, 46, 32, 8, 12, 16),
            "tech_font_01": (167, 79, 32, 8, 12, 16), "tech_font_01_hilight": (170, 79, 32, 8, 12, 16)}
    for n, (d, g, t, k, b, sp) in want.items():
        f = fonts[n].fnt
        got = (len(fonts[n].col.entries), f.n, f.char_w, f.kerning, f.baseline, f.space)
        assert got == (d, g, t, k, b, sp), (n, got)
        assert f.char_w == f.char_h
    assert (fonts["BionicleFont"].fnt.max_width, fonts["BionicleFont"].fnt.min_width) == (15, 3)
    assert (fonts["BionicleFontHi"].fnt.max_width, fonts["BionicleFontHi"].fnt.min_width) == (25, 2)


def test_a11_bionicle_font_counts_and_charset(fonts):
    ft = fonts["BionicleFont"]
    assert ft.fnt.n == 145 == len(ft.fnt.records) == len(ft.glyph_dir_names()) == ft.tga_entries()  # glyphs = .fnt records = directory TGAs
    asc = [r[0] for r in ft.fnt.records]
    assert max(asc) <= 0xFF
    assert asc[:10] == list(b"0123456789") and asc[-65:] == list(range(0xA0, 0xE1))
    assert set(range(ord("a"), ord("z") + 1)) <= set(asc) and set(range(ord("A"), ord("Z") + 1)) <= set(asc)
    # charset = the .fnt ascii column: the .info packed inside the same .col (tool input) lists the same charset in tile order
    assert colfont.info_charset(ft.col.read("BionicleFont.info")) == asc


def test_a11_str_len_absolute(fonts):
    b, h = fonts["BionicleFont"].fnt, fonts["BionicleFontHi"].fnt
    assert [b.get_str_len(s) for s in ("New Game", "Options", "Quit")] == [84, 57, 32]
    assert [h.get_str_len(s) for s in ("New Game", "Options", "Quit")] == [165, 113, 57]
    # probe 2 3.4 heights (non-space glyphs)
    assert [b.get_str_height(s) for s in ("New Game", "Options", "Quit")] == [10, 11, 11]
    assert [h.get_str_height(s) for s in ("New Game", "Options", "Quit")] == [15, 20, 15]
    # hand check from probe 2: Q13 + u9 + i3 + t7
    assert [b.get_width(c) for c in "Quit"] == [13, 9, 3, 7]


def test_menu_strings_play_game_options_quit(fonts):
    # F6 menu strings; 7a pins these same numbers. Values EXECUTED by this reader; cross-checked below by hand-summing glyph widths.
    for name, want in MENU_PINS.items():
        f = fonts[name].fnt
        got = [f.get_str_len(s) for s in ("Play Game", "Options", "Quit")]
        assert got == want, (name, got)
        for s, w in zip(("Play Game", "Options", "Quit"), want):
            tot = 0
            for ch in s:
                if ch == " ":
                    tot += f.space + f.kerning
                else:
                    tot += f.kerning + next(r[1] for r in f.records if r[0] == ord(ch))
            assert tot - f.kerning == w


def test_rendered_ink_extent_equals_str_len(fonts):
    # probe 2 headline 3: hard-ink (alpha >= 128) right edge of BionicleFont strings == GetStrLen
    ft = fonts["BionicleFont"]
    for s in ("New Game", "Options", "Quit"):
        lo, hi = ft.ink_extent(s)
        assert (lo, hi + 1) == (0, ft.fnt.get_str_len(s)), s   # hi is the last inked column; extent is half-open


def test_tga_descriptor0_has_real_alpha(fonts):
    t = fonts["BionicleFont"].glyph_tga(ord_index(fonts["BionicleFont"], "Q"))
    assert t.descriptor == 0x00 and t.bpp == 32
    alphas = {t.pixels[i + 3] for i in range(0, len(t.pixels), 4)}
    assert 0 in alphas and 255 in alphas               # a real mask, not constant (Pillow reads constant 255)


def ord_index(ft, ch):
    return next(i for i, r in enumerate(ft.fnt.records) if r[0] == ord(ch))


def test_cli_table_format_and_determinism(lomn_data):
    cmd = [sys.executable, str(REPO / "tools" / "research" / "colfont.py"), "table", "--install", str(lomn_data / "install")]
    a = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout
    b = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout
    assert a == b
    lines = a.splitlines()
    assert lines[0] == colfont.TABLE_FORMAT_LINE
    heads = [ln for ln in lines if ln.startswith("font ")]
    assert [h.split()[1] for h in heads] == sorted(FONTS)
    assert sum(1 for ln in lines if ln.startswith("glyph ")) == 145 + 79 * 3 + 46 * 2 + 79 * 2
    assert "str BionicleFont 'New Game' len=84 height=10" in lines
    assert any(ln.startswith("glyph BionicleFont 0 0030 ") for ln in lines)


MENU_PINS = {"BionicleFont": [82, 57, 32], "BionicleFontHi": [165, 113, 57]}
