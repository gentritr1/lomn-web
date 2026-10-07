"""Measures, over every textures.slb entry of every .blk in an install, what the C++ parse_spec_textures caps
(engine/include/lomn/sage/slb.h kMaxSpecTextures / kMaxSpecTextureName cite these values; M1a-3 Task 8B C1).
Independent of the C++ reader: BLK directory via blk.py, then the GcSpecTextureLoader::LoadTextures (0x0046f270) layout
read here: header {u32 n, u32 n, data-relative ptr records}, 20-byte records {ptr name, f32 trans, f32 u, f32 v,
u32 frames}; the game reads the C string at name + 1 (the first byte is a length the game ignores).
Usage: spec_textures_maxima.py INSTALL_DIR   (JSON to stdout)"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import blk  # noqa: E402


def entries(install):
    for root, _, files in sorted(os.walk(install)):
        for f in sorted(files):
            if not f.lower().endswith('.blk'):
                continue
            p = os.path.join(root, f)
            _, _, ents = blk.read(p)
            for name, _bs, fs, _fl, off in ents:
                if name.lower() == 'textures.slb':
                    with open(p, 'rb') as h:
                        h.seek(off)
                        yield os.path.relpath(p, install), h.read(fs)


def main():
    out = dict(tables=0, entry_bytes_max=0, records_max=0, records_total=0, name_len_max=0, where={})
    for rel, b in entries(sys.argv[1]):
        out['tables'] += 1
        n, n2, base = struct.unpack_from('<III', b, 0)
        out['entry_bytes_max'] = max(out['entry_bytes_max'], len(b))
        out['records_total'] += n
        if n > out['records_max']:
            out['records_max'], out['where']['records'] = n, rel
        for i in range(n):
            name_ptr = struct.unpack_from('<I', b, base + 20 * i)[0]
            end = b.index(b'\0', name_ptr + 1)
            ln = end - (name_ptr + 1)
            if ln > out['name_len_max']:
                out['name_len_max'], out['where']['name'] = ln, '%s #%d %s' % (rel, i, b[name_ptr + 1:end].decode('latin1'))
    print(json.dumps(out, indent=1))


if __name__ == '__main__':
    main()
