"""Independent DDS decoder (DXT1/DXT3/DXT5 + A8R8G8B8 -> RGBA8), written from the format definition, not from
the C++ code. Oracle for A5.

Format (DirectDraw Surface + S3TC/BC1-3 as Direct3D defines it):
  * 4x4 texel blocks, row-major, texel t = 4*y + x.
  * Colour block (8 bytes): two RGB565 endpoints c0, c1 (LE u16) and 16 2-bit indices (LE u32, texel t at bits 2t).
    An n-bit UNORM channel code k means the real value k / (2^n - 1).
    Palette: p0 = c0, p1 = c1; if c0 > c1 (or for DXT3/DXT5 colour blocks, always): p2 = (2 c0 + c1) / 3,
    p3 = (c0 + 2 c1) / 3; else p2 = (c0 + c1) / 2 and p3 = transparent black.
  * DXT3 alpha (8 bytes): 16 explicit 4-bit UNORM values, texel t at bits 4t of the LE u64.
  * DXT5 alpha (8 bytes): a0, a1 (8-bit UNORM) then 16 3-bit indices (LE 48-bit). If a0 > a1, code c in 2..7
    -> ((8-c) a0 + (c-1) a1) / 7; otherwise codes 2..5 -> ((6-c) a0 + (c-1) a1) / 5, code 6 -> 0, code 7 -> 1.
  * Real values are converted to 8 bits by round-to-nearest of value*255 (ties up), computed exactly here with
    Fractions.
"""
from fractions import Fraction
from functools import lru_cache
import math
import struct


def to8(v: Fraction) -> int:
    return math.floor(v * 255 + Fraction(1, 2))


def rgb565(c):
    return (Fraction((c >> 11) & 31, 31), Fraction((c >> 5) & 63, 63), Fraction(c & 31, 31))


@lru_cache(maxsize=None)
def colour_palette(c0, c1, four_only):
    e0, e1 = rgb565(c0), rgb565(c1)
    p = [tuple(to8(x) for x in e0) + (255,), tuple(to8(x) for x in e1) + (255,)]
    if four_only or c0 > c1:
        p.append(tuple(to8((2 * a + b) / 3) for a, b in zip(e0, e1)) + (255,))
        p.append(tuple(to8((a + 2 * b) / 3) for a, b in zip(e0, e1)) + (255,))
    else:
        p.append(tuple(to8((a + b) / 2) for a, b in zip(e0, e1)) + (255,))
        p.append((0, 0, 0, 0))
    return p


@lru_cache(maxsize=None)
def alpha_palette(a0, a1):
    f0, f1 = Fraction(a0, 255), Fraction(a1, 255)
    pal = {0: a0, 1: a1}
    if a0 > a1:
        for c in range(2, 8):
            pal[c] = to8(((8 - c) * f0 + (c - 1) * f1) / 7)
    else:
        for c in range(2, 6):
            pal[c] = to8(((6 - c) * f0 + (c - 1) * f1) / 5)
        pal[6], pal[7] = 0, 255
    return pal


def decode_block(fmt, blk):
    """16 RGBA tuples for one block."""
    cb = blk if fmt == 'DXT1' else blk[8:16]
    c0, c1, idx = struct.unpack('<HHI', cb)
    pal = colour_palette(c0, c1, fmt != 'DXT1')
    texels = [list(pal[(idx >> (2 * t)) & 3]) for t in range(16)]
    if fmt == 'DXT3':
        bits = int.from_bytes(blk[0:8], 'little')
        for t in range(16):
            texels[t][3] = ((bits >> (4 * t)) & 15) * 17   # k/15 * 255 is exact
    elif fmt == 'DXT5':
        apal = alpha_palette(blk[0], blk[1])
        bits = int.from_bytes(blk[2:8], 'little')
        for t in range(16):
            texels[t][3] = apal[(bits >> (3 * t)) & 7]
    return texels


def decode_dds(data: bytes):
    """Returns (width, height, format, rgba bytes) for the top mip level."""
    if data[:4] != b'DDS ':
        raise ValueError('not a DDS')
    height, width = struct.unpack_from('<II', data, 12)
    pf_flags, fourcc, bits, rm, gm, bm, am = struct.unpack_from('<I4sIIIII', data, 80)
    out = bytearray(width * height * 4)
    if pf_flags & 4:
        fmt = fourcc.decode('latin1')
        bsize = 8 if fmt == 'DXT1' else 16
        bw, bh = (width + 3) // 4, (height + 3) // 4
        for by in range(bh):
            for bx in range(bw):
                o = 128 + (by * bw + bx) * bsize
                texels = decode_block(fmt, data[o:o + bsize])
                for t, px in enumerate(texels):
                    x, y = bx * 4 + t % 4, by * 4 + t // 4
                    if x < width and y < height:
                        q = (y * width + x) * 4
                        out[q:q + 4] = bytes(px)
        return width, height, fmt, bytes(out)
    if bits == 32 and (rm, gm, bm, am) == (0xff0000, 0xff00, 0xff, 0xff000000):
        for i in range(width * height):
            v = struct.unpack_from('<I', data, 128 + 4 * i)[0]
            out[4 * i:4 * i + 4] = bytes(((v >> 16) & 255, (v >> 8) & 255, v & 255, v >> 24))
        return width, height, 'A8R8G8B8', bytes(out)
    raise ValueError('unsupported pixel format')
