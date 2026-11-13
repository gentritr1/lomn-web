#!/usr/bin/env python3
"""M2a-3 T11b: the area-character differential (x1-x5 of M2a-2 T12 plus the hive/slot lookups), Python oracle vs the C++ engine.

  area_diff.py --cpp /path/to/lomn_view [--keep] [--frames N]

Builds a hard-linked scratch copy of $LOMN_DATA/install (cp -Rl; the real install is read-only and never written: the scratch
Base.osi is unlinked, never overwritten, after a realpath check), assembles one synthetic Base.osi per sequence (no game script,
nothing committed), runs `osi_vm.py --run ... --real-natives` and `lomn_view --script 1 --natives real` on it, and requires
natives.log and the digest's D lines to be identical. Exit 0 = every sequence identical; 1 = a difference; 2 = usage/setup.
Every sequence loads the install's real `_char.slb` records (the area_source seam of the Python oracle, areachars.py)."""
from __future__ import annotations

import argparse
import difflib
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "osi"))
import osiasm  # noqa: E402

LOAD = ('PushConstantString "%s"\nCallGameFunction gcareadirector.setcurrentlevel 1\nPop\n'
        'PushConstantString "%s"\nCallGameFunction gcareadirector.setcurrentarea 1\nPop\n')
AREA = 'PushConstantString "%s"\nCallGameFunction gcareadirector.setcurrentarea 1\nPop\n'
LEVEL = 'PushConstantString "%s"\nCallGameFunction gcareadirector.setcurrentlevel 1\nPop\n'
EXISTS = 'PushConstantString "%s"\nCallGameFunction scslosifileio.doesfileexist 1\nPop\n'
VLGR = 0x766C6772
SEQUENCES = {
    "x1": LOAD % ("lev1", "vllg"),                                           # load only
    "x2": LOAD % ("lev1", "vllg") + AREA % "vllg",
    "x3": LOAD % ("lev1", "vllg") + LEVEL % "lev2",                          # the removal: OnEvent(0x2b, 'vlgr')
    "x4": LOAD % ("lev1", "vllg") + "PushConstanti32 %d\nCallGameFunction gcmodeldirector.getcharacter 1\nPop\n" % VLGR
          + LEVEL % "lev2",                                                  # a script-held handle, then the removal
    "x5": LOAD % ("lev1", "vllg") + EXISTS % "Root/Data/Levels/lev1/vllg_hive.slb"
          + EXISTS % "Root/Data/Levels/lev1/vllg_char.slb" + EXISTS % "Root/Data/Levels/lev1/atrm_char.slb",   # slot-relative lookups
    # beyond x1-x5: other levels' records (mixed ids), two areas in one level, removal of both
    "y1": LOAD % ("lev3", "mtup") + AREA % "vllg" + LEVEL % "lev4",
    "y2": LOAD % ("lev2", "ttun") + LEVEL % "lev1" + AREA % "scrp" + EXISTS % "Root/Data/Levels/lev1/scrp_hive.slb",
}
OSI_TAIL = (".function OnEvent 10\nPushConstanti8 1\nReturn\n.function input 5\nPushNothing\nReturn\n"
            ".function process 3\nPushConstanti8 1\nReturn\n")


def asm_for(body: str) -> str:
    return ".function setup 0\n" + body + "PushNothing\nReturn\n" + OSI_TAIL


def inside(path: Path, root: Path) -> bool:
    return os.path.realpath(path).startswith(os.path.realpath(root) + os.sep)


def make_scratch(scratch: Path, data: Path) -> Path:
    inst = scratch / "install"
    inst.mkdir()
    subprocess.run(["cp", "-Rl", str(data / "install") + "/.", str(inst)], check=True)
    if inst.is_symlink():
        sys.exit("scratch install is a symlink")
    return inst


def put_base(inst: Path, scratch: Path, osi: bytes) -> Path:
    base = inst / "data" / "Base.osi"
    if not inside(base.parent, scratch):
        sys.exit("refusing to touch %s: outside the scratch" % base)
    if base.exists() or base.is_symlink():
        os.unlink(base)                                      # the scratch link only: never write through a hard link
    base.write_bytes(osi)
    return base


def d_lines(path: Path) -> list[str]:
    return [l for l in path.read_text().splitlines() if l.startswith("D ")]


def run_pair(name, body, inst, scratch, cpp, frames, py):
    osi, _ = osiasm.assemble(asm_for(body))
    base = put_base(inst, scratch, osi)
    out = scratch / name
    out.mkdir()
    p = subprocess.run([py, str(REPO / "tools/research/osi_vm.py"), "--run", str(base), "--real-natives", "--install", str(inst),
                        "--frames", str(frames), "--overrides", str(REPO / "data/osi/native_overrides.json"),
                        "--log-out", str(out / "p.log"), "--digest", str(out / "p.dig")], capture_output=True, text=True, timeout=110)
    c = subprocess.run([str(cpp), "--install", str(inst), "--script", "1", "--frames", str(frames), "--natives", "real",
                        "--natives-log", str(out / "c.log"), "--digest", str(out / "c.dig")], capture_output=True, text=True, timeout=110)
    if p.returncode != 0 or c.returncode != 0:
        return False, "rc python %d, cpp %d\n%s%s" % (p.returncode, c.returncode, p.stdout[-600:] + p.stderr[-600:], c.stderr[-600:])
    pl, cl = (out / "p.log").read_text().splitlines(), (out / "c.log").read_text().splitlines()
    pd, cd = d_lines(out / "p.dig"), d_lines(out / "c.dig")
    msg = ["natives.log: %d lines each, %s" % (len(pl), "identical" if pl == cl else "DIFFERENT (python %d, cpp %d)" % (len(pl), len(cl))),
           "D lines: %d each, %s" % (len(pd), "identical" if pd == cd else "DIFFERENT (python %d, cpp %d)" % (len(pd), len(cd)))]
    if pl != cl:
        msg += list(difflib.unified_diff(pl, cl, "python", "cpp", lineterm="", n=1))[:40]
    if pd != cd:
        msg += list(difflib.unified_diff(pd, cd, "python", "cpp", lineterm="", n=1))[:40]
    n_c = sum(1 for l in pl if l.startswith("C "))
    msg.append("C records (python): %d%s" % (n_c, "".join("  " + l for l in pl if l.startswith("C ") and "0010:0000002b" in l)[:200]))
    return pl == cl and pd == cd and len(pl) > 0 and len(pd) > 0, "\n".join(msg)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cpp", required=True, help="the C++ lomn_view binary (read-only)")
    ap.add_argument("--frames", type=int, default=1)
    ap.add_argument("--keep", action="store_true", help="keep the scratch directory and print it")
    ap.add_argument("--only", help="comma list of sequences")
    a = ap.parse_args(argv)
    data = os.environ.get("LOMN_DATA")
    if not data or not (Path(data) / "install").is_dir() or not os.access(a.cpp, os.X_OK):
        print("need LOMN_DATA (with install/) and an executable --cpp", file=sys.stderr)
        return 2
    scratch = Path(tempfile.mkdtemp(prefix="area_diff_"))
    rc = 0
    try:
        inst = make_scratch(scratch, Path(data))
        names = a.only.split(",") if a.only else list(SEQUENCES)
        for n in names:
            ok, msg = run_pair(n, SEQUENCES[n], inst, scratch, a.cpp, a.frames, sys.executable)
            print("%s: %s\n  %s" % (n, "IDENTICAL" if ok else "DIFFERENT", msg.replace("\n", "\n  ")))
            rc |= 0 if ok else 1
    finally:
        if a.keep:
            print("scratch kept: %s" % scratch)
        elif inside(scratch, Path(tempfile.gettempdir())):
            shutil.rmtree(scratch)                          # hard links: removes the scratch names only
    return rc


if __name__ == "__main__":
    sys.exit(main())
