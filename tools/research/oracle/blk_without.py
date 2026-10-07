"""Copy a SAGE blockfile ("BLKF", version 1, 56-byte entries {name[40], size, size, flags, offset}) without the named
entries (case-insensitive), payloads re-packed in entry order. Test plumbing for the A9 oracle (M1a-3 Task 8A E2b): the
synthetic state area without its POS table exercises make_camera's bounds-centre eye fallback.

  blk_without.py IN.blk OUT.blk NAME [NAME ...]
"""
import os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import blk  # noqa: E402


def main():
    src, dst, drop = sys.argv[1], sys.argv[2], {n.lower() for n in sys.argv[3:]}
    magic, ver, ents = blk.read(src)
    data = open(src, 'rb').read()
    keep = [e for e in ents if e[0].lower() not in drop]
    if len(keep) == len(ents):
        sys.exit('blk_without: none of %s in %s' % (sorted(drop), src))
    out = bytearray(struct.pack('<4sII', magic, ver, len(keep)))
    off = 12 + 56 * len(keep)
    for name, bs, fs, fl, o in keep:
        out += struct.pack('<40sIIII', name.encode('latin1'), bs, fs, fl, off)
        off += fs
    for name, bs, fs, fl, o in keep:
        out += data[o:o + fs]
    open(dst, 'wb').write(out)


if __name__ == '__main__':
    main()
