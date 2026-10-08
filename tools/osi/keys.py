"""M2a-1 T4: KEYSPEC -> the per-frame `Input` engine calls of the A6 run (consult §4.1, probe 3).

A KEYSPEC is space-separated `name@frame` taps, e.g. `down@100 down@160 enter@220 esc@280`. A tap is two frames:
state 2 on frame f, state 1 on frame f+1 (the action state machine; a held key would send 4).
  down, enter  the analog-trigger site S3: Input(C1, VK, state, 0.0, 0.0), VK 0x28 / 0x0d (5 arguments, `input` declares 5)
  esc          the key-mapper site S4: Input(C1, 251, state) (3 arguments; the other 7 of the 10 arrive as nothing)
C1 = 0x636e7431 ('cnt1'). Calls of one frame keep the KEYSPEC order; they run after the frame's K record and before
Process. `lomn_osi_run --print-keys --keys SPEC` prints the same listing (ctest osi_a6_digest compares the two).

Usage: keys.py "SPEC"   prints one line per call: `<frame> Input <v> ...` (variants as trace.md `%04x:%08x`)."""
from __future__ import annotations

import sys

C1 = 0x636E7431
VK = {"down": 0x28, "enter": 0x0D}


def expand(spec: str) -> list[tuple[int, list[tuple[int, int]]]]:
    """[(frame, [(type, value), ...]), ...] in frame order (stable within a frame)."""
    out = []
    for tok in spec.split():
        name, _, f = tok.partition("@")
        if not f.lstrip("-").isdigit():
            raise ValueError("bad key token %r" % tok)
        for state, frame in ((2, int(f)), (1, int(f) + 1)):
            if name in VK:
                args = [(0x10, C1), (0x10, VK[name]), (0x10, state), (0x12, 0), (0x12, 0)]
            elif name == "esc":
                args = [(0x10, C1), (0x10, 251), (0x10, state)]
            else:
                raise ValueError("unknown key %r" % name)
            out.append((frame, args))
    return sorted(out, key=lambda c: c[0])


def main(argv: list[str]) -> int:
    if len(argv) != 1:
        print(__doc__)
        return 2
    for frame, args in expand(argv[0]):
        print("%d Input %s" % (frame, " ".join("%04x:%08x" % a for a in args)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
