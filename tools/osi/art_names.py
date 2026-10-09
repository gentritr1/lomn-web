"""M2a-2 T7a: the entry names of a SAGE blockfile, one per line, read by the independent table reader tools/research/blk.py
(not by the engine's sage::BlockFile). ctest `osi_services_art_names` feeds the list to lomn_tests, which checks that
scslosifileio.doesfileexist finds every Art.blk entry (the falsifier of the doesfileexist binding: `os.walk` is not an
oracle for it, natives.md 8.2). Usage: art_names.py <Art.blk>"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "research"))
import blk  # noqa: E402


def names(path):
    magic, _version, entries = blk.read(path)
    if magic != b"BLKF":
        raise SystemExit("%s: not a BLKF container" % path)
    return [e[0] for e in entries]


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    sys.stdout.write("".join(n + "\n" for n in names(sys.argv[1])))
