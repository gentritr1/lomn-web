"""M1 seed set: functions reachable (direct calls) from the regex roots in m1_seeds.txt, plus every function
that calls a COM vtable method (vtable_uses.jsonl), i.e. the render-path call sites the D3D8 shim must serve.

Tags written to seeds.json:
  m1-required   reachable by direct calls from a root or a render call site
  m1-candidate  other methods of classes that contain an m1-required function (virtual dispatch is invisible
                to the direct call graph, so these are reviewed, not assumed)
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path


def load_jsonl(p: Path) -> list[dict]:
    return [json.loads(l) for l in p.read_text().splitlines() if l]


def compute(funcs: list[dict], calls: list[dict], vt_uses: list[dict], seed_patterns: list[str]) -> dict:
    name = {f["addr"]: f["name"] for f in funcs}
    edges = {c["from"]: c["to"] for c in calls}
    pats = [re.compile(p) for p in seed_patterns]
    roots = {a for a, n in name.items() if any(p.search(n) for p in pats)}
    render_sites = {u["addr"] for u in vt_uses}
    required, stack = set(), sorted(roots | render_sites)
    while stack:
        a = stack.pop()
        if a in required:
            continue
        required.add(a)
        stack.extend(t for t in edges.get(a, []) if t not in required)
    classes = {name[a].rsplit("::", 1)[0] for a in required if "::" in name.get(a, "")}
    candidate = {a for a, n in name.items() if "::" in n and n.rsplit("::", 1)[0] in classes} - required
    return {"roots": sorted(roots), "render_sites": sorted(render_sites),
            "m1-required": sorted(required), "m1-candidate": sorted(candidate)}


if __name__ == "__main__":
    export_dir, seeds_file, out = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    pats = [l.strip() for l in seeds_file.read_text().splitlines() if l.strip() and not l.startswith("#")]
    r = compute(load_jsonl(export_dir / "functions.jsonl"), load_jsonl(export_dir / "calls.jsonl"),
                load_jsonl(export_dir / "vtable_uses.jsonl"), pats)
    out.write_text(json.dumps(r, indent=0))
    print({k: len(v) for k, v in r.items()})
