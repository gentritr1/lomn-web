"""Independent .col / .fnt reader (tile fonts), written from probe 2's tables
(docs/knowledge/research/2026-10-08-m2a0-probe2-fonts.md) and docs/osi/hud.md 3.6 / 4.4. Oracle for engine/src/sage/colfont.cpp.

  .col  (BE): u32 version, count, flags (0x4 = hash mode), total, 0x14 raw bytes; then count x {hash, flags, offset, usize}.
        hash = h*0x83 + byte (u32, case-sensitive); flags 0x11 = LZSS, 0x01 = stored; csize = next.offset - offset (last: total - offset).
  .fnt  (LE, inside the .col as <Name>.fnt): 8 x i32 {rows, columns, numberOfCharacters, charWidth, charHeight, kerning, baseline,
        spaceWidth}, then n x 18 B {u16 ascii, i32 width, height, xOffset, yOffset}. Record i owns <Name><i//columns>_<i%columns>.tga.
Labels: everything here is EXECUTED against the install except the paths no shipped font reaches (2-byte codes, 0xFF escape, unsigned wraps), which are read from the asm (GetStrLen 0x628ae0, GetStrHeight 0x628bf0) and the terminator read (INFERRED 0) and the empty-string width (INFERRED).

CLI:  python tools/research/colfont.py table [--install DIR]   (prints the glyph table, see TABLE_FORMAT_LINE)
"""
from __future__ import annotations

import argparse
import os
import struct
import sys
from pathlib import Path

MAX_ENTRIES = 1 << 16
TABLE_FORMAT_LINE = ("# colfont-table v1 | font NAME dir=N glyphs=N rows=R cols=C cw=W ch=H kern=K base=B space=S maxw=X minw=Y tga=WxH"
                     " | glyph NAME IDX ASCII(hex4) W H XOFF YOFF | str NAME 'TEXT' len=L height=H"
                     " | fonts sorted by name (ASCII), glyphs in record order, ints decimal, one space between fields")
TABLE_STRINGS = ("New Game", "Options", "Quit", "Play Game")


M32 = 0xFFFFFFFF


def f32(x: float) -> float:
    """x87 `fstp dword`: store as binary32 (round-to-nearest-even)."""
    return struct.unpack("<f", struct.pack("<f", x))[0]


class ColError(Exception):
    pass


def hash_name(name: str | bytes) -> int:
    h = 0
    for b in (name.encode("latin-1") if isinstance(name, str) else name):
        h = (h * 0x83 + b) & 0xFFFFFFFF
    return h


def lzss_decode(data: bytes, usize: int) -> bytes:
    # 0x4d2955..0x4d2965 fills only 0..0xFED with 0x20; 0xFEE..0xFFF are uninitialised heap (value there INFERRED, modelled 0)
    ring = bytearray(b" " * 0xFEE + bytes(18))
    w = 0xFEE
    out = bytearray()
    i = 0
    while len(out) < usize and i < len(data):
        flags = data[i]
        i += 1
        for bit in range(8):
            if len(out) >= usize:
                break
            if flags >> bit & 1:
                if i >= len(data):
                    break
                c = data[i]
                i += 1
                out.append(c)
                ring[w] = c
                w = (w + 1) & 0xFFF
            else:
                if i + 1 >= len(data):
                    i = len(data)
                    break
                b0, b1 = data[i], data[i + 1]
                i += 2
                pos = b0 | (b1 & 0xF0) << 4
                for k in range((b1 & 0xF) + 3):
                    c = ring[(pos + k) & 0xFFF]
                    out.append(c)
                    ring[w] = c
                    w = (w + 1) & 0xFFF
    if len(out) < usize:
        raise ColError(f"lzss: short output {len(out)} < {usize}")
    return bytes(out[:usize])


class Entry:
    def __init__(self, hash_, flags, offset, usize, csize):
        self.hash, self.flags, self.offset, self.usize, self.csize = hash_, flags, offset, usize, csize


class Col:
    def __init__(self, data: bytes):
        if len(data) < 0x24:
            raise ColError("short header")
        self.data = data
        self.version, count, self.flags, self.total = struct.unpack_from(">4I", data, 0)
        if count > MAX_ENTRIES or 0x24 + 16 * count > len(data):
            raise ColError("bad count")
        if self.flags & 0xE != 4:
            raise ColError("not hash mode")
        raw = [struct.unpack_from(">4I", data, 0x24 + 16 * i) for i in range(count)]
        self.entries = []
        for i, (h, fl, off, us) in enumerate(raw):
            end = raw[i + 1][2] if i + 1 < count else self.total
            self.entries.append(Entry(h, fl, off, us, end - off))
        self.sorted_by_hash = all(self.entries[i].hash < self.entries[i + 1].hash for i in range(count - 1))

    def find(self, name: str):
        """GetInfo 0x4cfc20: lower-bound binary search over the directory (unsigned `ja` at 0x4cfc6d), so the lowest of equal hashes wins."""
        h, lo, hi = hash_name(name), -1, len(self.entries) - 1
        while hi - lo > 1:
            mid = (hi + lo) >> 1
            if h > self.entries[mid].hash:
                lo = mid
            else:
                hi = mid
        return self.entries[hi] if hi >= 0 and self.entries[hi].hash == h else None

    def entry_for(self, name: str) -> Entry:
        e = self.find(name)
        if e is None:
            raise ColError(f"no entry {name}")
        return e

    def read_entry(self, e: Entry) -> bytes:
        if e.csize < 0 or e.offset + e.csize > len(self.data):
            raise ColError("entry out of range")
        raw = self.data[e.offset:e.offset + e.csize]
        kind = e.flags & 0xF0
        if kind == 0x10:
            return lzss_decode(raw, e.usize)
        if kind == 0x00:
            return raw[:e.usize]
        raise ColError(f"unknown compression {e.flags:#x}")

    def read(self, name: str) -> bytes:
        return self.read_entry(self.entry_for(name))


class Fnt:
    def __init__(self, data: bytes):
        if len(data) < 32:
            raise ColError("short fnt")
        (self.rows, self.columns, self.n, self.char_w, self.char_h,
         self.kerning, self.baseline, self.space) = struct.unpack_from("<8i", data, 0)
        if self.n < 0 or self.n > MAX_ENTRIES or 32 + 18 * self.n > len(data):
            raise ColError("bad glyph count")
        self.raw_size = len(data)
        self.records = [struct.unpack_from("<Hiiii", data, 32 + 18 * i) for i in range(self.n)]  # (ascii, w, h, xoff, yoff)
        ws = [r[1] & M32 for r in self.records]                 # unsigned compares: 0x6277e0 jbe, 0x627810 jae
        self.max_width = max(ws, default=0)                     # init 0 (0x626d71)
        self.min_width = min(ws, default=M32)                   # init 0xffffffff (0x626d7e)
        self._first = {}
        for i, r in enumerate(self.records):
            self._first.setdefault(r[0], i)  # linear search, first match wins

    def codes(self, s):
        """Walk of GetStrLen 0x628ae0 / GetStrHeight 0x628bf0 (same walk). Lead 0xF0..0xFE: code = lead<<8 + next byte (0x628b20), also
        when the lead is last (the byte read is then the ScString terminator, INFERRED 0). 0xFF / '\\n' are tested on the byte at the
        CURRENT index, i.e. the trail byte after a lead. 0xFF skips itself and the next char (+ its trail if that is a lead): no code."""
        b = s.encode("latin-1") if isinstance(s, str) else bytes(s)
        at = lambda i: b[i] if i < len(b) else 0
        out, i = [], 0
        while i < len(b):
            c = at(i)
            if 0xF0 <= c <= 0xFE:
                i += 1
                c = (c << 8) + at(i)
            if at(i) == 0xFF:
                i += 1
                if 0xF0 <= at(i) <= 0xFE:
                    i += 1
            elif at(i) == 0x0A:
                break
            else:
                out.append(c)
            i += 1
        return out

    def _width(self, code, xs):
        xs = f32(xs)
        if code & 0xFFFF == 0x20:                                # GetWidth 0x6289a0: fild dword (signed)
            s = (self.space + self.kerning + 2**31) % 2**32 - 2**31
            return s * xs
        i = self._first.get(code & 0xFFFF)
        if i is None:                                            # fild qword of a zero-extended u32 (0x628a20 / 0x6289f9)
            return ((self.max_width + self.kerning) & M32) * xs
        return ((self.kerning + self.records[i][1]) & M32) * xs

    def get_width(self, c, xscale: float = 1.0):
        code = c if isinstance(c, int) else self.codes(c)[0]
        return self._width(code, xscale)

    def get_str_len(self, s, xscale: float = 1.0) -> int:
        """0x628ae0: the sum is stored as f32 after each add (0x628ba0); result = sum - kerning*xScale, x87 chop (or 0xc00 at
        0x628b76 / 0x628bc1) then fistp qword, low dword. int() truncates toward zero."""
        tot = 0.0                                                # [0x75822c] = 0.0
        for code in self.codes(s):
            tot = f32(self._width(code, xscale) + tot)
        return int(tot - self.kerning * f32(xscale))

    def has_unknown(self, s) -> bool:
        return any(c != 0x20 and c not in self._first for c in self.codes(s))

    def get_str_height(self, s, yscale: float = 1.0) -> int:
        """GetStrHeight 0x628bf0 (hud.md 4.9): a code with no record is skipped (ruling INFERRED: the original reads heap at
        [[this+0x58]-0x110]); a code WITH a record (space included, GetHeight(' ') = 0.0 at 0x628a67) still feeds below. Stores are f32.
        d = u32(baseline - yOffset); below = d if d > below and base >= yo unsigned (0x628d5b, 0x628d76 jb); cand = h - d if base > yo
        unsigned (0x628dae jbe) else h; above = cand if cand > above (strict). Result chopped (0x628ccd)."""
        ys, below, above = f32(yscale), 0.0, 0.0
        for code in self.codes(s):
            i = self._first.get(code & 0xFFFF)
            if i is None:
                continue
            _, _, h, _, yo = self.records[i]
            base, yo = self.baseline & M32, yo & M32
            d = (base - yo) & M32
            hh = 0.0 if code & 0xFFFF == 0x20 else f32((h & M32) * ys)
            if d > below and base >= yo:
                below = f32(d)
            cand = f32(hh - d) if base > yo else hh
            if cand > above:
                above = cand
        return int(above + below)                                # chop toward zero; both terms >= 0


class Tga:
    def __init__(self, data: bytes):
        if len(data) < 18:
            raise ColError("short tga")
        idlen, cmap, typ = data[0], data[1], data[2]
        self.w, self.h = struct.unpack_from("<HH", data, 12)
        self.bpp, self.descriptor = data[16], data[17]
        if typ != 2 or cmap != 0 or self.bpp != 32:
            raise ColError(f"unsupported tga type={typ} bpp={self.bpp}")
        n = self.w * self.h * 4
        o = 18 + idlen
        if o + n > len(data):
            raise ColError("tga pixels truncated")
        self.pixels = data[o:o + n]  # BGRA, rows in file order (bottom-up when descriptor bit 5 is clear)


class TileFont:
    def __init__(self, name: str, col: Col):
        self.name, self.col = name, col
        self.fnt = Fnt(col.read(name + ".fnt"))

    def glyph_name(self, i: int) -> str:
        return f"{self.name}{i // self.fnt.columns}_{i % self.fnt.columns}.tga"

    def glyph_dir_names(self):
        return [self.glyph_name(i) for i in range(self.fnt.n) if self.col.find(self.glyph_name(i))]

    def glyph_tga(self, i: int) -> Tga:
        return Tga(self.col.read(self.glyph_name(i)))

    def tga_entries(self) -> int:
        """Directory entries that decode as a char_w x char_h 32 bpp TGA (content-based, independent of the names)."""
        n = 0
        for e in self.col.entries:
            try:
                t = Tga(self.col.read_entry(e))
            except ColError:
                continue
            n += (t.w, t.h) == (self.fnt.char_w, self.fnt.char_h)
        return n

    def ink_extent(self, s):
        """(min, max) inclusive pixel columns with alpha >= 128, laying tiles out like Draw: tile left = pen - xOffset."""
        f, pen, lo, hi = self.fnt, 0, None, None
        for code in f.codes(s):
            i = f._first.get(code)
            if code == 0x20 or i is None:
                pen += int(f._width(code, 1.0))
                continue
            _, w, _, xo, _ = f.records[i]
            t = self.glyph_tga(i)
            for x in range(t.w):
                if any(t.pixels[(y * t.w + x) * 4 + 3] >= 128 for y in range(t.h)):
                    X = pen - xo + x
                    lo = X if lo is None else min(lo, X)
                    hi = X if hi is None else max(hi, X)
            pen += w + f.kerning
        return lo, hi


def load_font(path, name: str | None = None) -> TileFont:
    path = Path(path)
    return TileFont(name or path.stem, Col(path.read_bytes()))


def list_fonts(fontdir):
    return sorted((p.stem, p) for p in Path(fontdir).iterdir() if p.name.lower().endswith(".col"))


def info_charset(info: bytes):
    """The charset line (7th non-comment line) of a TiffSpliter .info: bytes, \\xHH escapes for >= 0xA0."""
    lines = [ln for ln in info.decode("latin-1").splitlines() if not ln.startswith("//")]
    line, out, i = lines[6], [], 0
    while i < len(line):
        if line.startswith("\\x", i) and i + 4 <= len(line):
            out.append(int(line[i + 2:i + 4], 16))
            i += 4
        else:
            out.append(ord(line[i]))
            i += 1
    return out


def table(fontdir) -> str:
    out = [TABLE_FORMAT_LINE]
    for name, path in list_fonts(fontdir):
        ft = load_font(path, name)
        f = ft.fnt
        t0 = ft.glyph_tga(0) if f.n else None
        sizes = {(t.w, t.h) for t in (ft.glyph_tga(i) for i in range(f.n))}
        tga = f"{t0.w}x{t0.h}" if len(sizes) == 1 else "mixed"
        out.append(f"font {name} dir={len(ft.col.entries)} glyphs={f.n} rows={f.rows} cols={f.columns} cw={f.char_w} ch={f.char_h}"
                   f" kern={f.kerning} base={f.baseline} space={f.space} maxw={f.max_width} minw={f.min_width} tga={tga}")
        for i, (a, w, h, xo, yo) in enumerate(f.records):
            out.append(f"glyph {name} {i} {a:04x} {w} {h} {xo} {yo}")
        for s in TABLE_STRINGS:
            hh = str(f.get_str_height(s))                          # §4.9: no-record codes skipped
            out.append(f"str {name} '{s}' len={f.get_str_len(s)} height={hh}")
    return "\n".join(out) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    t = sub.add_parser("table")
    t.add_argument("--install", default=os.path.join(os.environ.get("LOMN_DATA", ""), "install"))
    a = ap.parse_args(argv)
    sys.stdout.write(table(Path(a.install) / "data" / "Art" / "Fonts"))


if __name__ == "__main__":
    main()
