"""M2a-1 T1: the build-time assembly step of the C++ conformance runner (lomn_osi_conform). Every `<id>.asm` under SRC is
assembled with tools/osi/osiasm.py into OUT/<same relative dir>/<id>.osi, and its label table is written next to it as
`<id>.labels` (one `name file_offset` line per label, sorted by name), so `"pc": "@L1"` in an expectation resolves without
the runner parsing assembly. The binaries live only in the build directory (never committed).

Usage: conform_assemble.py SRC OUT   (exit 1 if any program fails to assemble; prints the count)"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from osiasm import AsmError, assemble  # noqa: E402


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__)
        return 2
    src, out = Path(argv[0]), Path(argv[1])
    n, bad = 0, 0
    for asm in sorted(src.rglob("*.asm")):
        rel = asm.relative_to(src)
        dst = out / rel.parent
        dst.mkdir(parents=True, exist_ok=True)
        try:
            image, info = assemble(asm.read_text(encoding="latin-1"))
        except AsmError as e:
            print("%s: %s" % (asm, e), file=sys.stderr)
            bad += 1
            continue
        (dst / (asm.stem + ".osi")).write_bytes(image)
        (dst / (asm.stem + ".labels")).write_text("".join("%s %d\n" % (k, v) for k, v in sorted(info["labels"].items())))
        n += 1
    print("assembled %d program(s), %d failed" % (n, bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
