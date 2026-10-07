"""texres.py extended with the SrBlockFileExists order (oracle for A2).

For every texture name in every .x of an area blk (found by scanning the raw bytes for TextureFilename data
objects, exactly like texres.py), resolve it the way ScDrawableTexture::Load + the blockfile chain do:
  * full request "Root/Data/Levels/<lev>/textures/<name>"; if its last '.' starts ".dds"/".tga" (any case) and
    Exists(full) -> that name, else the first of <full>.dds, <full>.tga for which Exists() is true;
  * Exists order: Art.blk, area blk, level blk.  Load order: area blk, level blk, Art.blk.
Lookup is by basename, case-folded A-Z only (ScBlock::StrCmp). The name a table entry is matched by is the C string
starting at the entry -- for a 40-byte name without NUL it runs on into the size fields, as StrCmp reads it.
Usage: python3 texres_ext.py lev1/vllg lev2/ghut ...   (prints JSON)
"""
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import slb  # noqa: E402  (provides the data root D and raw entry access)


def fold(b: bytes) -> bytes:
    return bytes(c | 0x20 if 0x41 <= c <= 0x5a else c for c in b)


def table(rel):
    """[(display name, matched name bytes, offset, size)] in file order."""
    d = open(slb.D + rel, 'rb').read()
    n = struct.unpack_from('<I', d, 8)[0]
    end = 12 + 56 * n
    out = []
    for i in range(n):
        o = 12 + 56 * i
        z = d.find(b'\0', o, end)
        matched = d[o:(z if z >= 0 else end)]
        display = d[o:o + 40].split(b'\0')[0]
        bs, fs, fl, off = struct.unpack_from('<IIII', d, o + 40)
        out.append((display.decode('latin1'), matched, off, fs))
    return d, out


def index(entries):
    keys = [fold(m) for _, m, _, _ in entries]
    assert all(keys[i] < keys[i + 1] for i in range(len(keys) - 1)), 'table not sorted: Find would differ'
    return {k: i for i, k in enumerate(keys)}


def texnames(b):
    out = []
    for m in re.finditer(rb'TextureFilename', b):
        j = m.end()
        if b[j:j + 2] == b'\x0a\x00' and b[j + 2:j + 4] == b'\x02\x00':
            n = struct.unpack_from('<I', b, j + 4)[0]
            out.append(b[j + 8:j + 8 + n].decode('latin1'))
    return out


def basename(s):
    return re.split(r'[\\/]', s)[-1]


def resolve_area(lev, area):
    blks = {'current': f'levels/{lev}/{area}.blk', 'second': f'levels/{lev}/{lev}.blk', 'art': 'Art/Art.blk'}
    file_label = {'current': f'{area}.blk', 'second': f'{lev}.blk', 'art': 'Art.blk'}
    idx = {}
    area_bytes = None
    area_entries = None
    for k, rel in blks.items():
        d, ents = table(rel)
        idx[k] = index(ents)
        if k == 'current':
            area_bytes, area_entries = d, ents
    names = set()
    for disp, _, off, fs in area_entries:
        if disp.lower().endswith('.x'):
            names |= set(texnames(area_bytes[off:off + fs]))

    def find(src, req):
        return idx[src].get(fold(basename(req).encode('latin1')))

    def first(order, req):
        for s in order:
            i = find(s, req)
            if i is not None:
                return f'{s}:{file_label[s]}:{i}'
        return ''

    exists_order, load_order = ('art', 'current', 'second'), ('current', 'second', 'art')
    res = {}
    for t in sorted(names):
        full = f'Root/Data/Levels/{lev}/textures/{t}'
        chosen = ''
        dot = full.rfind('.')
        if dot >= 0 and full[dot:].lower() in ('.dds', '.tga') and first(exists_order, full):
            chosen = full
        if not chosen:
            for ext in ('.dds', '.tga'):
                if first(exists_order, full + ext):
                    chosen = full + ext
                    break
        res[t] = {'chosen': basename(chosen), 'load': first(load_order, chosen) if chosen else '',
                  'exists': first(exists_order, chosen) if chosen else ''}
    return res


if __name__ == '__main__':
    print(json.dumps({a: resolve_area(*a.split('/')) for a in sys.argv[1:]}, indent=1))
