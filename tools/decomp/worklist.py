"""Per-function work queue.

Generated view (gen/decomp/worklist.json): every function with addr, name, original source path, tags.
Committed state (worklist/status.json): only what humans/agents change: {addr: {status, port_file, test}}.
Status order: decompiled -> cleaned -> ported -> tested.

  worklist.py build <gen_dir> <status.json>              merge -> gen_dir/worklist.json
  worklist.py set <status.json> <addr> <status> [--port-file P] [--test NAME]
  worklist.py check <gen_dir> <status.json> <repo_root> [--ctest-dir build/native]
       ported => port_file exists; tested => port_file exists AND `ctest -R ^NAME$` passes.
  worklist.py dashboard <gen_dir> <status.json> <out.html>
"""
from __future__ import annotations

import argparse
import collections
import html
import json
import re
import subprocess
import sys
from pathlib import Path

STATUSES = ["decompiled", "cleaned", "ported", "tested"]


def load_status(p: Path) -> dict:
    return json.loads(p.read_text()) if p.exists() else {}


def build(gen: Path, status: dict) -> list[dict]:
    funcs = [json.loads(l) for l in (gen / "functions.jsonl").read_text().splitlines() if l]
    fmap = json.loads((gen / "function_map.json").read_text())
    seeds = json.loads((gen / "seeds.json").read_text()) if (gen / "seeds.json").exists() else {}
    tag_of = collections.defaultdict(list)
    for tag in ("m1-required", "m1-candidate"):
        for a in seeds.get(tag, []):
            tag_of[a].append(tag)
    rows = []
    for f in funcs:
        st = status.get(f["addr"], {})
        rows.append({"addr": f["addr"], "name": f["name"], "source": fmap.get(f["addr"]), "tags": tag_of.get(f["addr"], []),
                     "status": st.get("status", "decompiled"), "port_file": st.get("port_file"), "test": st.get("test")})
    return rows


def check(rows: list[dict], repo: Path, ctest_dir: Path | None) -> list[str]:
    errors = []
    for r in rows:
        if r["status"] not in STATUSES:
            errors.append(f"{r['addr']} {r['name']}: unknown status {r['status']}")
            continue
        if STATUSES.index(r["status"]) >= STATUSES.index("ported"):
            if not r["port_file"] or not (repo / r["port_file"]).is_file():
                errors.append(f"{r['addr']} {r['name']}: status {r['status']} but port_file missing: {r['port_file']}")
        if r["status"] == "tested":
            if not r["test"] or ctest_dir is None:
                errors.append(f"{r['addr']} {r['name']}: tested without a test name or ctest dir")
                continue
            try:
                res = subprocess.run(["ctest", "--test-dir", str(ctest_dir), "-R", f"^{re.escape(r['test'])}$",
                                      "--no-tests=error"], capture_output=True, text=True, timeout=600)
            except FileNotFoundError:
                errors.append(f"{r['addr']} {r['name']}: ctest not found on PATH, cannot verify test {r['test']}")
                continue
            if res.returncode != 0:
                errors.append(f"{r['addr']} {r['name']}: test {r['test']} failed or missing")
    return errors


def dashboard(rows: list[dict]) -> str:
    by_status = collections.Counter(r["status"] for r in rows)
    by_tag = collections.Counter(t for r in rows for t in r["tags"])
    m1 = [r for r in rows if "m1-required" in r["tags"]]
    m1_status = collections.Counter(r["status"] for r in m1)
    files = collections.defaultdict(collections.Counter)
    for r in rows:
        files[r["source"]][r["status"]] += 1
    def row(cells):
        return "<tr>" + "".join(f"<td>{html.escape(str(c))}</td>" for c in cells) + "</tr>"
    parts = ["<!doctype html><meta charset=utf-8><title>LOMN decomp progress</title>",
             "<style>body{font:14px system-ui;margin:24px;background:#fff;color:#111}table{border-collapse:collapse}"
             "td,th{border:1px solid #ccc;padding:3px 8px;text-align:left}@media (prefers-color-scheme:dark)"
             "{body{background:#111;color:#eee}td,th{border-color:#444}}</style>",
             f"<h1>LOMN decomp progress</h1><p>{len(rows)} functions</p><h2>All functions by status</h2><table>",
             *(row([s, by_status.get(s, 0)]) for s in STATUSES), "</table>",
             f"<h2>M1-required ({len(m1)}) by status</h2><table>",
             *(row([s, m1_status.get(s, 0)]) for s in STATUSES), "</table><h2>Tags</h2><table>",
             *(row([t, n]) for t, n in sorted(by_tag.items())), "</table>",
             "<h2>By original source file</h2><table><tr><th>file</th>" + "".join(f"<th>{s}</th>" for s in STATUSES) + "</tr>",
             *(row([f] + [c.get(s, 0) for s in STATUSES]) for f, c in sorted(files.items(), key=lambda kv: str(kv[0]))),
             "</table>"]
    return "\n".join(parts) + "\n"


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build"); b.add_argument("gen", type=Path); b.add_argument("status", type=Path)
    s = sub.add_parser("set"); s.add_argument("status", type=Path); s.add_argument("addr"); s.add_argument("new_status", choices=STATUSES)
    s.add_argument("--port-file"); s.add_argument("--test")
    c = sub.add_parser("check"); c.add_argument("gen", type=Path); c.add_argument("status", type=Path); c.add_argument("repo", type=Path)
    c.add_argument("--ctest-dir", type=Path)
    d = sub.add_parser("dashboard"); d.add_argument("gen", type=Path); d.add_argument("status", type=Path); d.add_argument("out", type=Path)
    a = ap.parse_args(argv)
    if a.cmd == "set":
        st = load_status(a.status)
        entry = dict(st.get(a.addr, {}))
        entry.update({k: v for k, v in {"status": a.new_status, "port_file": a.port_file, "test": a.test}.items() if v})
        st[a.addr] = entry
        a.status.parent.mkdir(parents=True, exist_ok=True)
        a.status.write_text(json.dumps(dict(sorted(st.items())), indent=1) + "\n")
        return 0
    rows = build(a.gen, load_status(a.status))
    if a.cmd == "build":
        (a.gen / "worklist.json").write_text(json.dumps(rows, indent=0))
        print(f"worklist: {len(rows)} functions")
        return 0
    if a.cmd == "check":
        errs = check(rows, a.repo, a.ctest_dir)
        for e in errs:
            print("WORKLIST:", e, file=sys.stderr)
        print(f"worklist check: {len(rows)} functions, {len(errs)} errors")
        return 1 if errs else 0
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_text(dashboard(rows))
    print("wrote", a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
