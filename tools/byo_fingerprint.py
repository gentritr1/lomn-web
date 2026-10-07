"""Independent oracle for the BYO page's build identification (web/app/fingerprint.js): the SHA-256 of
LEGOBionicle.exe and the blockfile-table fingerprint of an install folder.

Fingerprint (written from the format, not from the JS): for every data/**/*.blk under the install root, path relative
to the root with '/' separators and ASCII lower-case, sorted by that path; table = bytes [0, 12 + count * 56) where
count is the u32 LE at +8 after the "BLKF" magic; line = "<path> <file size> <sha256 hex of table>"; fingerprint =
sha256 hex of the lines joined by '\n' plus a final '\n'. Dot files are skipped, as the page does.

Usage: python3 tools/byo_fingerprint.py <install_dir>   -> JSON {exe_sha256, blk_tables_sha256, blk_files, table_bytes}
"""
from __future__ import annotations

import hashlib
import json
import os
import struct
import sys
from pathlib import Path


def lower_ascii(s: str) -> str:
    return "".join(chr(ord(c) + 32) if "A" <= c <= "Z" else c for c in s)


def fingerprint(root: Path) -> dict:
    blks = []
    exe = None
    for d, dirs, files in os.walk(root):
        dirs[:] = [x for x in dirs if not x.startswith(".")]
        for f in files:
            if f.startswith("."):
                continue
            p = Path(d) / f
            rel = p.relative_to(root).as_posix()
            key = lower_ascii(rel)
            if key == "legobionicle.exe":
                exe = p
            if key.startswith("data/") and key.endswith(".blk"):
                blks.append((key, p))
    blks.sort(key=lambda kp: kp[0].encode())
    lines = []
    table_bytes = 0
    for key, p in blks:
        size = p.stat().st_size
        with open(p, "rb") as h:
            hdr = h.read(12)
            if len(hdr) != 12 or hdr[:4] != b"BLKF":
                raise ValueError(f"{key}: not a blockfile")
            (count,) = struct.unpack("<I", hdr[8:12])
            n = 12 + count * 56
            if n > size:
                raise ValueError(f"{key}: table larger than the file")
            h.seek(0)
            table = h.read(n)
        table_bytes += n
        lines.append(f"{key} {size} {hashlib.sha256(table).hexdigest()}")
    fp = hashlib.sha256(("\n".join(lines) + "\n").encode()).hexdigest()
    return {
        "exe_sha256": hashlib.sha256(exe.read_bytes()).hexdigest() if exe else None,
        "blk_tables_sha256": fp,
        "blk_files": len(blks),
        "table_bytes": table_bytes,
    }


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    print(json.dumps(fingerprint(Path(sys.argv[1])), indent=1))
