"""Group per-function decompiled C into one file per ORIGINAL source file.

Inputs: export dir (funcs/<addr>.c, functions.jsonl) + modules.json (from nb11: obj name, .text ranges, source files).
A function belongs to the module whose .text range contains its entry address.
Path: first .cpp/.c in the module's source_files, "C:\\Projects\\lego\\" prefix stripped -> e.g. Character/Bosses/GfMudBoss.cpp;
modules without source info -> _unplaced/<ObjBase>.cpp; addresses outside every module -> _unplaced/_no_module.cpp.
Writes corpus/<path> (functions sorted by address) and function_map.json {addr: path}.
"""
from __future__ import annotations

import bisect
import json
import re
import sys
from pathlib import Path

PREFIX = re.compile(r"^[A-Za-z]:\\Projects\\lego\\", re.I)


def module_path(mod: dict) -> str:
    for src in mod["source_files"]:
        if src.lower().endswith((".cpp", ".c")):
            return PREFIX.sub("", src).replace("\\", "/")
    base = re.split(r"[\\/]", mod["obj_name"])[-1].rsplit(".", 1)[0]
    return f"_unplaced/{re.sub(r'[^A-Za-z0-9_.-]', '_', base)}.cpp"


class RangeIndex:
    def __init__(self, modules: list[dict]):
        spans = sorted((va, va + size, module_path(m)) for m in modules for va, size in m["text_ranges"])
        self.starts = [s[0] for s in spans]
        self.spans = spans

    def lookup(self, addr: int) -> str:
        i = bisect.bisect_right(self.starts, addr) - 1
        if i >= 0 and self.spans[i][0] <= addr < self.spans[i][1]:
            return self.spans[i][2]
        return "_unplaced/_no_module.cpp"


def build(export_dir: Path, modules: list[dict], out_dir: Path) -> dict[str, str]:
    idx = RangeIndex(modules)
    funcs = [json.loads(l) for l in (export_dir / "functions.jsonl").read_text().splitlines() if l]
    groups: dict[str, list[tuple[int, str]]] = {}
    fmap = {}
    for f in funcs:
        a = int(f["addr"], 16)
        path = idx.lookup(a)
        fmap[f["addr"]] = path
        text = (export_dir / "funcs" / f"{a:08x}.c").read_text()
        groups.setdefault(path, []).append((a, text))
    for path, items in groups.items():
        dst = out_dir / "corpus" / path
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_text(f"// Decompiled from LEGOBionicle.exe; original file: {path}\n\n" +
                       "\n".join(t for _, t in sorted(items)))
    (out_dir / "function_map.json").write_text(json.dumps(dict(sorted(fmap.items())), indent=0))
    return fmap


if __name__ == "__main__":
    export_dir, modules_json, out_dir = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    fmap = build(export_dir, json.loads(modules_json.read_text()), out_dir)
    placed = sum(1 for p in fmap.values() if not p.startswith("_unplaced/"))
    print(f"functions {len(fmap)}; in known source paths {placed}; files {len(set(fmap.values()))}")
