"""Decomp metrics -> metrics.json. Counts come from the export + nb11 type file + corpus text, never typed by hand.

Readability counters (over the whole corpus): member accesses, raw-offset derefs, unnamed params/locals,
x87 NAN( noise remaining, in_stack/in_EAX artefacts, x87 rewrites applied.
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

PATTERNS = {
    "member_access": r"(?:->|\.)m[A-Z]\w*",
    "raw_offset_deref": r"\*\([\w ]+\*\)\(\(int\)",
    "unnamed_param": r"\bparam_\d+\b",
    "unnamed_local": r"\blocal_[0-9a-f]+\b",
    "x87_nan_noise": r"NAN\(",
    "in_stack_artifact": r"\bin_(?:stack_\w+|E[A-Z]X)\b",
    "x87_rewrites": r"/\*x87\*/",
}


def compute(gen: Path) -> dict:
    funcs = [json.loads(l) for l in (gen / "functions.jsonl").read_text().splitlines() if l]
    types = (gen / "types.txt").read_text().splitlines()
    corpus = "\n".join(p.read_text() for p in sorted((gen / "corpus").rglob("*.cpp")))
    m = {
        "functions": len(funcs),
        "functions_named": sum(1 for f in funcs if not f["name"].split("::")[-1].startswith("FUN_")),
        "decompiled_ok": sum(1 for f in funcs if f["ok"]),
        "structs": sum(1 for l in types if l.startswith("S ")),
        "members": sum(1 for l in types if l.startswith("M ") and not l.endswith(" vftable")),
        "procs_with_locals": sum(1 for l in types if l.startswith("F ")),
        "named_vars": sum(1 for l in types if l.startswith("V ")),
    }
    m.update({k: len(re.findall(p, corpus)) for k, p in PATTERNS.items()})
    return m


if __name__ == "__main__":
    gen = Path(sys.argv[1])
    m = compute(gen)
    (gen / "metrics.json").write_text(json.dumps(m, indent=1, sort_keys=True) + "\n")
    print(json.dumps(m, indent=1, sort_keys=True))
