"""M2a-3 T1: the menu probe. Runs `Base.osi` (Setup + N frames, the oracle's real natives) with host events injected from
several sources at once, and reports what the run reached. Built on event_probe.run (its frame loop is the only one).

  menu_probe.py --osi B --install I [--frames N] [--keys KEYSPEC] [--mouse x,y@frame]... [--event F:ID[:a1,a2..][:pre|post]]...
                [--natives-impl real|stub] [--deadline S] [--report coverage|loads|frames|watch:PC,..|calls:A-B]...

--event F:ID:a1,a2   OnEvent(ID, a1, ...) in frame F, after that frame's key calls. `pre` = before the frame's Process,
                     `post` = after it. Default: post for ID 6 (the camera's source, F19: the camera process runs after the
                     script's), pre for everything else (26/23/24/25 are mouse events). Arguments are ints (0x.. ok) or
                     floats (with a point). `F:6:0` is a STAND-IN for the camera's OnEvent(6, <camera handle>, 0): it passes no
                     handle (the front end's `onevent` reads arguments only for events 26 and 25).
                     A trailing `:quiet` leaves the injected call's own natives and pcs out of the run, as event_probe's
                     single-injection mode does (use it to see only what the injection changes in the script's own path).
IN-FRAME ORDER (keys, then the mouse's OnEvent(26), then the other `pre` events, then Process, then `post` events) is an
ASSUMPTION of this stand-in with no asm basis. Task 4 pins the order from Task 2a's process-map ruling, not from this probe.
--mouse x,y@frame    from that frame on (until the next --mouse), OnEvent(26, x, y) before every frame's Process, ahead of the
                     frame's `pre` events. A STAND-IN for Task 4's mouse driver; the full MOUSESPEC grammar is Task 3's.
Reports (repeatable; default coverage):
  coverage   coverage.py's grammar: the head line and one `native` row per distinct native (merged tier, first frame, calls)
  loads      the string arguments of the file-loading natives (LOAD_KEYS), with their frame (-1 = Setup)
  frames     natives per frame p50/p95/max with n against the expected frame count (Setup excluded)
  watch:PC,..  the frames in which each pc executes (decimal or 0x..; -1 = Setup)
  calls:A-B  every native call of frames A..B (key and argument values; heap handles by type only)
"""
from __future__ import annotations

import argparse
import re
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(REPO / "tools" / "osi"), str(REPO / "tools" / "research")]
import coverage  # noqa: E402
import event_probe as ep  # noqa: E402

DATA = REPO / "data" / "osi"


def parse_value(tok: str):
    return ("f", float(tok)) if "." in tok else int(tok, 0)


def parse_event(spec: str) -> ep.Injection:
    """`F:ID[:a1,a2,..][:pre|post]`."""
    parts = spec.split(":")
    when, record = None, True
    while parts[-1] in ("pre", "post", "quiet"):
        tok = parts.pop()
        if tok == "quiet":
            record = False
        else:
            when = tok
    if len(parts) < 2 or len(parts) > 3:
        raise ValueError("bad --event %r (F:ID[:a1,a2..][:pre|post])" % spec)
    frame, event = int(parts[0]), int(parts[1], 0)
    args = tuple(parse_value(a) for a in parts[2].split(",") if a) if len(parts) == 3 else ()
    return ep.Injection(frame, event, args, when or ("post" if event == 6 else "pre"), record)


def parse_mouse(spec: str) -> tuple:
    """`x,y@frame` -> (frame, x, y)."""
    xy, _, f = spec.partition("@")
    x, _, y = xy.partition(",")
    if not (x and y and f):
        raise ValueError("bad --mouse %r (x,y@frame)" % spec)
    return int(f), int(x), int(y)


def parse_range(spec: str) -> tuple:
    """`A` or `A-B` (A may be -1, Setup)."""
    m = re.fullmatch(r"(-?\d+)(?:-(\d+))?", spec)
    if not m:
        raise ValueError("bad frame range %r" % spec)
    return int(m.group(1)), int(m.group(2) or m.group(1))


def natives_of(out: ep.Outcome):
    """[(frame, key, vals)] in call order; frame -1 = Setup."""
    return [(k - 1, x[0], x[1]) for k, fr in enumerate(out.frames) for x in fr if isinstance(x, tuple)]


def coverage_rows(out: ep.Outcome, reg: dict) -> list:
    events = [(f, i, key) for i, (f, key, _) in enumerate(natives_of(out))]
    return coverage.report(events, 0, reg)


def per_frame(out: ep.Outcome) -> list:
    """Native calls per frame, Setup excluded."""
    return [sum(1 for x in fr if isinstance(x, tuple)) for fr in out.frames[1:]]


def percentile(xs: list, p: float) -> int:
    """Nearest rank."""
    s = sorted(xs)
    return s[max(0, -(-int(p * len(s)) // 100) - 1)] if s else 0


def frames_rows(out: ep.Outcome, expected: int) -> list:
    n = per_frame(out)
    return ["frames: n %d expected %d%s natives/frame p50 %d p95 %d max %d, calls %d (Setup %d)" % (
        len(n), expected, "" if len(n) == expected else " MISMATCH", percentile(n, 50), percentile(n, 95),
        max(n, default=0), sum(n), sum(1 for x in out.frames[0] if isinstance(x, tuple)))]


def watch_rows(out: ep.Outcome, pcs: list) -> list:
    rows = []
    for pc in pcs:
        fr = tuple(k - 1 for k, f in enumerate(out.frames) if pc in f)
        rows.append("watch %d: %d frame(s), first %s, frames %s" % (
            pc, len(fr), fr[0] if fr else "-", ep.fmt_frames(fr) if fr else "-"))
    return rows


def calls_rows(out: ep.Outcome, lo: int, hi: int) -> list:
    rows = []
    for f, key, vals in natives_of(out):
        if lo <= f <= hi:
            rows.append("call %d %s %s" % (f, key, ",".join("t%02x" % v[0] if len(v) == 1 else "t%02x:%s" % v
                                                            for v in vals)))
    return rows


def loads_rows(out: ep.Outcome) -> list:
    return ["load %d %s %s" % (f, key, " ".join(repr(a) if isinstance(a, str) else str(a) for a in args))
            for f, key, args in out.loads]


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--osi", required=True)
    ap.add_argument("--install")
    ap.add_argument("--frames", type=int, default=600)
    ap.add_argument("--keys", default="")
    ap.add_argument("--mouse", action="append", default=[],
                    help="x,y@frame. ASSUMED in-frame order (no asm basis): keys, mouse 26, other pre events, Process, post events")
    ap.add_argument("--event", action="append", default=[],
                    help="F:ID[:a1,a2..][:pre|post][:quiet]. In-frame order is an ASSUMPTION of this stand-in (see --mouse)")
    ap.add_argument("--natives-impl", choices=("stub", "real"), default="real")
    ap.add_argument("--deadline", type=float, default=110.0, help="seconds")
    ap.add_argument("--report", action="append")
    ap.add_argument("--natives", default=str(DATA / "natives.json"))
    ap.add_argument("--mods", default=str(DATA / "natives_beta150.json"))
    ap.add_argument("--types", default=str(DATA / "types.json"))
    ap.add_argument("--overrides", default=str(DATA / "native_overrides.json"))
    a = ap.parse_args(argv)
    t0 = time.monotonic()
    out = ep.run(Path(a.osi).read_bytes(), a.natives, a.mods, a.types, a.frames, a.keys, a.deadline,
                 natives_impl=a.natives_impl, install=a.install, injections=[parse_event(e) for e in a.event],
                 mouse=[parse_mouse(m) for m in a.mouse], want_loads=True)
    rows = []
    for rep in a.report or ["coverage"]:
        name, _, arg = rep.partition(":")
        if name == "coverage":
            ov = a.overrides if Path(a.overrides).exists() else None
            rows += coverage_rows(out, coverage.load_registry(a.natives, a.mods, ov))
        elif name == "loads":
            rows += loads_rows(out)
        elif name == "frames":
            rows += frames_rows(out, a.frames)
        elif name == "watch":
            rows += watch_rows(out, [int(x, 0) for x in arg.split(",")])
        elif name == "calls":
            lo, hi = parse_range(arg)
            rows += calls_rows(out, lo, hi)
        else:
            ap.error("unknown report %r" % rep)
    print("\n".join(rows))
    print("menu_probe: stop %s, %d frames, %.1f s" % (out.stop, len(out.frames) - 1, time.monotonic() - t0),
          file=sys.stderr)
    return 0 if out.stop == "none" else 1


if __name__ == "__main__":
    sys.exit(main())
