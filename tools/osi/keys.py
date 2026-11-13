"""M2a-1 T4: KEYSPEC -> the per-frame `Input` engine calls of the A6 run (consult §4.1, probe 3).

A KEYSPEC is space-separated `name@frame` taps, e.g. `down@100 down@160 enter@220 esc@280`. A tap is two frames:
state 2 on frame f, state 1 on frame f+1 (the action state machine; a held key would send 4).
  down, enter  the analog-trigger site S3: Input(C1, VK, state, 0.0, 0.0), VK 0x28 / 0x0d (5 arguments, `input` declares 5)
  esc          the key-mapper site S4: Input(C1, 251, state) (3 arguments; the other 7 of the 10 arrive as nothing)
C1 = 0x636e7431 ('cnt1'). Calls of one frame keep the KEYSPEC order; they run after the frame's K record and before
Process. `lomn_osi_run --print-keys --keys SPEC` prints the same listing (ctest osi_a6_digest compares the two).

MOUSESPEC (M2a-3, docs/osi/hud.md §11): space-separated tokens, each holding from its frame on:
  x,y@f        the cursor (client pixels at the game resolution, 0..65535 each); (0, 0) before the first one
  lmb+@f lmb-@f  button 1 down / up (rmb = 2, mmb = 3, xmb = 4)
A non-empty MOUSESPEC starts the mouse source: every frame OnEvent(26, x, y), then per button 1..4 OnEvent(23, b) on
its first frame down, 24 on every later frame down, 25 on its first frame up (hud.md §5.1, GcMouseDriver 0x55c160).

Usage: keys.py "SPEC"   prints one line per call: `<frame> Input <v> ...` (variants as trace.md `%04x:%08x`).
       keys.py --mouse "MOUSESPEC" N   the same for frames 0..N-1: `<frame> OnEvent <v> ...`."""
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


BUTTONS = {"lmb": 1, "rmb": 2, "mmb": 3, "xmb": 4}


def expand_mouse(spec: str, frames: int) -> list[tuple[int, list[tuple[int, int]]]]:
    """The mouse source's OnEvent calls for frames 0..frames-1, [(frame, [(type, value), ...]), ...]; [] if spec is empty."""
    pos, down, seen = {}, {b: {} for b in BUTTONS.values()}, set()
    for tok in spec.split():
        what, _, f = tok.partition("@")
        if not f.isdigit():
            raise ValueError("bad mouse token %r" % tok)
        f = int(f)
        if what[-1:] in "+-" and what[:-1] in BUTTONS:
            key, table, val = what[:-1], down[BUTTONS[what[:-1]]], what[-1] == "+"
        else:
            x, _, y = what.partition(",")
            if not (x.isdigit() and y.isdigit() and int(x) < 65536 and int(y) < 65536):
                raise ValueError("bad mouse token %r" % tok)
            key, table, val = "pos", pos, (int(x), int(y))
        if (key, f) in seen:
            raise ValueError("two %s tokens at frame %d" % (key, f))
        seen.add((key, f))
        table[f] = val
    if not seen:
        return []
    out, xy, state = [], (0, 0), {b: False for b in down}
    for f in range(frames):
        xy = pos.get(f, xy)
        out.append((f, [(0x10, 26), (0x10, xy[0]), (0x10, xy[1])]))
        for b in sorted(down):
            was, now = state[b], down[b].get(f, state[b])
            ev = 24 if was and now else 23 if now else 25 if was else None
            if ev:
                out.append((f, [(0x10, ev), (0x10, b)]))
            state[b] = now
    return out


def main(argv: list[str]) -> int:
    if len(argv) == 3 and argv[0] == "--mouse" and argv[2].isdigit():
        calls, name = expand_mouse(argv[1], int(argv[2])), "OnEvent"
    elif len(argv) == 1:
        calls, name = expand(argv[0]), "Input"
    else:
        print(__doc__)
        return 2
    for frame, args in calls:
        print("%d %s %s" % (frame, name, " ".join("%04x:%08x" % a for a in args)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
