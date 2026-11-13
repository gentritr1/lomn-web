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
    assert f.get_str_len(b"A\xffB") == f.get_str_len(b"AB")  # 0xFF callback byte has no width
    assert f.get_str_len(b"AB", 2.0) == 34              # round((12+7)*2 - 2*2)
    assert f.get_str_len(b"") == -2                     # initial sum 0, minus kerning (formula as read; INFERRED edge)


def test_str_height_skips_unknown_and_space():
    # glyph: height 11, yOffset 1, baseline 12: below = 12-1 = 11, above = 11-(12-1) = 0 -> 11
    f = colfont.Fnt(_fnt(1, 1, 1, 16, 16, 0, 12, 8, [(65, 10, 11, 0, 1)]))
    assert f.get_str_height(b"A") == 11
    assert f.get_str_height(b"A A") == 11               # space skipped (original reads mChar[-1]: UB, probe 2 3.2)


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
