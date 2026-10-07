"""Data census of a player install ($LOMN_DATA/install): sizes that drive memory sizing and quota checks.

Writes census.json + census.md. Area = levels/<lev>/<area>.blk; an area's load also needs its level's
common blockfile levels/<lev>/<lev>.blk, so area_bytes = area blk + level common blk.
Boot set = data/Main.blk + data/Base.osi + data/Languages/lang_eng.blk (+ data/Art/Art.blk when present).
All sizes are on-disk (blockfiles may be compressed; unpacked sizes arrive with the M1 BLK reader).
"""
from __future__ import annotations

import collections
import json
import sys
from pathlib import Path

BOOT_SET = ["data/Main.blk", "data/Base.osi", "data/Languages/lang_eng.blk", "data/Art/Art.blk"]


def census(install: Path) -> dict:
    files = [p for p in install.rglob("*")
             if p.is_file() and not any(part.startswith(".") for part in p.relative_to(install).parts)]
    by_ext = collections.defaultdict(lambda: {"count": 0, "bytes": 0})
    for p in files:
        e = by_ext[p.suffix.lower() or "(none)"]
        e["count"] += 1
        e["bytes"] += p.stat().st_size
    boot = {rel: (install / rel).stat().st_size for rel in BOOT_SET if (install / rel).is_file()}
    areas = []
    levels_dir = install / "data" / "levels"
    for lev in sorted(d for d in levels_dir.iterdir() if d.is_dir()) if levels_dir.is_dir() else []:
        common = lev / f"{lev.name}.blk"
        common_bytes = common.stat().st_size if common.is_file() else 0
        for blk in sorted(lev.glob("*.blk")):
            if blk.name.lower() == f"{lev.name}.blk".lower():
                continue
            areas.append({"level": lev.name, "area": blk.stem, "area_bytes": blk.stat().st_size,
                          "with_common_bytes": blk.stat().st_size + common_bytes})
    largest = max(areas, key=lambda a: a["with_common_bytes"]) if areas else None
    return {
        "total_files": len(files),
        "total_bytes": sum(p.stat().st_size for p in files),
        "by_ext": dict(sorted(by_ext.items(), key=lambda kv: -kv[1]["bytes"])),
        "boot_set": boot,
        "boot_set_bytes": sum(boot.values()),
        "areas": areas,
        "largest_area": largest,
    }


def render_md(c: dict) -> str:
    mb = lambda b: f"{b / 1e6:.1f} MB"
    out = ["# Data census (player install, on-disk sizes)", "",
           f"Total: {c['total_files']} files, {mb(c['total_bytes'])}", "",
           "| Extension | Files | Size |", "|---|---|---|"]
    out += [f"| {k} | {v['count']} | {mb(v['bytes'])} |" for k, v in c["by_ext"].items()]
    out += ["", f"Boot set: {mb(c['boot_set_bytes'])}", ""]
    out += [f"- `{k}` {mb(v)}" for k, v in c["boot_set"].items()]
    if c["largest_area"]:
        la = c["largest_area"]
        out += ["", f"Areas: {len(c['areas'])}; largest: `{la['level']}/{la['area']}` "
                    f"{mb(la['with_common_bytes'])} incl. level common blockfile"]
    return "\n".join(out) + "\n"


if __name__ == "__main__":
    install, out_dir = Path(sys.argv[1]), Path(sys.argv[2])
    out_dir.mkdir(parents=True, exist_ok=True)
    c = census(install)
    (out_dir / "census.json").write_text(json.dumps(c, indent=1))
    (out_dir / "census.md").write_text(render_md(c))
    print(render_md(c))
