"""M2a-1 T0: the conformance-corpus coverage report (docs/osi/conformance.md "Coverage"). It reads ONLY the
`.expect.json` metadata of the programs under DIR (never the .asm, never a VM) and the targets from the tables:

  handlers ≥2      named handlers (docs/osi/opcodes.md, 96) cited in "rows" by at least 2 programs;
  row×message      (row, message) pairs from each named row's errors cell; a program covers a pair when it cites the row
                   and its expect_error.message matches the message (the %02.2X / %s / %d / %u formats match any text);
                   abbreviated cells ("… '=' (rhs) …") are expanded from the row's own lhs message;
  falsifiers       (1) each line of opcodes.md's "Conformance falsifiers for M2a-1" table ("0x73#2" = the second
                   0x73 line; the two D8' lines are "0x70#2" and "0x70#3"), (2) the two guards under it ("guard:0xb1",
                   "guard:0xb2"), (3) every named row whose notes cell states a falsifier ("0x75#notes"); a program
                   covers one by naming it in "falsifier_id" (a string or a list of strings);
  argc violations  docs/osi/types.md "Argument-count violations" rows expanded per site ("key/argc#i", i = 1..sites);
                   a program covers one by naming it in "argc_violation";
  programs         the number of .expect.json files.
--list (M2a-2 T2b) adds one line per program, sorted by id: `program <id> active`, or `program <id> pending <task>`
from the program's `pending` key (docs/osi/conformance.md §1: the runners skip it until that task lands).

Usage: conformance_report.py DIR [--opcodes docs/osi/opcodes.md] [--types docs/osi/types.md] [--group a|b] [--list]
Exit status 0 always (the report is a measurement; acceptance is read from its numbers)."""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
GROUPS = {   # consult §1.4 / plan Tasks 4-5: the families of each corpus half
    "a": [(0x00, 0x06), (0x10, 0x17), (0x18, 0x1A), (0x21, 0x25), (0x50, 0x54), (0x70, 0x75), (0xA0, 0xA7),
          (0xB8, 0xBD), (0xBF, 0xBF)],
    "b": [(0x30, 0x35), (0x40, 0x49), (0x60, 0x64), (0x6A, 0x6F), (0x76, 0x78), (0x7A, 0x7B), (0x80, 0x81),
          (0x82, 0x84), (0x90, 0x96), (0xB0, 0xB2), (0xFF, 0xFF)],
}
SECTION_REFS = {"The game-variable map"}     # quoted section titles inside errors cells, not messages


def table_rows(opcodes_md: Path) -> dict[int, dict]:
    rows = {}
    for line in opcodes_md.read_text().splitlines():
        if not re.match(r"^\| 0x[0-9a-f]{2} \| BCL", line) or "BCLUnused" in line:
            continue
        c = line.split(" | ")
        rows[int(c[0][2:], 16)] = {"handler": c[1], "errors": c[8], "notes": c[9] if len(c) > 9 else ""}
    return rows


def _expand(msgs: list[str]) -> list[str]:
    out, last_lhs = [], None
    for m in msgs:
        if m in SECTION_REFS:
            continue
        if m.startswith("… ") and m.endswith(" …"):
            if last_lhs is None:
                raise ValueError("abbreviated message %r with no lhs form before it" % m)
            full = last_lhs.replace("(lhs)", "(rhs)")
            if m[2:-2] not in full:
                raise ValueError("cannot expand %r from %r" % (m, last_lhs))
            out.append(full)
            continue
        if m.endswith(" …"):
            m = m[:-2] + " (type:%02.2X)"
        if "(lhs)" in m:
            last_lhs = m
        out.append(m)
    return list(dict.fromkeys(out))


def message_pairs(rows: dict[int, dict]) -> list[tuple[int, str]]:
    pairs = []
    for op, r in sorted(rows.items()):
        if r["errors"].startswith("none"):
            continue
        msgs = [m.replace('\\"', '"').replace("\\|", "|") for m in re.findall(r'"((?:[^"\\]|\\.)+)"', r["errors"])]
        pairs += [(op, m) for m in _expand(msgs)]
    return pairs


def falsifier_ids(opcodes_md: Path) -> list[str]:
    text = opcodes_md.read_text()
    sec = text.split("## Conformance falsifiers for M2a-1", 1)[1].split("\n## ", 1)[0]
    ids, seen = [], {}
    for op in re.findall(r"^\| (0x[0-9a-f]{2}) \| `", sec, re.M):
        seen[op] = seen.get(op, 0) + 1
        ids.append("%s#%d" % (op, seen[op]))
    guards = ["guard:%s" % g for g in re.findall(r"Guards kept for them: (0x[0-9a-f]{2}) .*?; (0x[0-9a-f]{2})", sec, re.S)[0]]
    notes = ["%#04x#notes" % op for op, r in sorted(table_rows(opcodes_md).items()) if "Falsifier" in r["notes"]]
    return ids + guards + notes


def argc_targets(types_md: Path) -> list[str]:
    sec = types_md.read_text().split("## Argument-count violations in `Base.osi`", 1)[1].split("\n## ", 1)[0]
    out = []
    for key, argc, sites in re.findall(r"^\| `([^`]+)` \| (\d+) \| \d+\.\.\d+ \| (\d+) \|", sec, re.M):
        out += ["%s/%s#%d" % (key, argc, i) for i in range(1, int(sites) + 1)]
    return out


def _matches(template: str, got: str) -> bool:
    pat = re.escape(template)
    for f in (re.escape("%02.2X"), re.escape("%s"), re.escape("%d"), re.escape("%u")):
        pat = pat.replace(f, ".*")
    return re.fullmatch(pat, got, re.S) is not None


def in_group(op: int, group: str | None) -> bool:
    return group is None or any(lo <= op <= hi for lo, hi in GROUPS[group])


def report(d: Path, opcodes_md: Path, types_md: Path, group: str | None = None) -> dict:
    rows = table_rows(opcodes_md)
    progs = [json.loads(p.read_text()) for p in sorted(Path(d).rglob("*.expect.json"))]
    cites: dict[int, int] = {}
    for p in progs:
        for r in set(p.get("rows", [])):
            cites[int(r, 16)] = cites.get(int(r, 16), 0) + 1
    handlers = [op for op in rows if in_group(op, group)]
    pairs = [pr for pr in message_pairs(rows) if in_group(pr[0], group)]
    covered = {pr for pr in pairs for p in progs
               if "expect_error" in p and "%#04x" % pr[0] in p.get("rows", [])
               and _matches(pr[1], p["expect_error"].get("message", ""))}
    fals = [f for f in falsifier_ids(opcodes_md) if in_group(int(f.split("#")[0].split(":")[-1], 16), group)]
    named = set()
    for p in progs:
        v = p.get("falsifier_id")
        named |= set(v) if isinstance(v, list) else {v}
    fal_cov = named & set(fals)
    argc = argc_targets(types_md) if group in (None, "b") else []
    argc_cov = {p.get("argc_violation") for p in progs} & set(argc)
    return {"handlers": (sum(1 for op in handlers if cites.get(op, 0) >= 2), len(handlers)),
            "pairs": (len(covered), len(pairs)), "falsifiers": (len(fal_cov), len(fals)),
            "argc": (len(argc_cov), len(argc)), "programs": len(progs),
            "missing_handlers": ["%#04x" % op for op in handlers if cites.get(op, 0) < 2],
            "missing_pairs": sorted(set(pairs) - covered), "missing_falsifiers": sorted(set(fals) - fal_cov),
            "missing_argc": sorted(set(argc) - argc_cov)}


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("dir")
    ap.add_argument("--opcodes", default=str(REPO / "docs" / "osi" / "opcodes.md"))
    ap.add_argument("--types", default=str(REPO / "docs" / "osi" / "types.md"))
    ap.add_argument("--group", choices=sorted(GROUPS))
    ap.add_argument("--missing", action="store_true", help="also list what is not covered")
    ap.add_argument("--list", action="store_true", help="also list every program with its status (active / pending)")
    a = ap.parse_args(argv)
    r = report(Path(a.dir), Path(a.opcodes), Path(a.types), a.group)
    print("handlers ≥2: %d/%d, row×message pairs: %d/%d, falsifiers: %d/%d, argc violations: %d/%d, programs: %d"
          % (r["handlers"] + r["pairs"] + r["falsifiers"] + r["argc"] + (r["programs"],)))
    if a.missing:
        for k in ("missing_handlers", "missing_pairs", "missing_falsifiers", "missing_argc"):
            for x in r[k]:
                print("%s: %s" % (k[8:], x))
    if a.list:
        for p in sorted((json.loads(f.read_text()) for f in Path(a.dir).rglob("*.expect.json")),
                        key=lambda p: p.get("id", "")):
            pend = p.get("pending")
            print("program %s %s" % (p.get("id"), ("pending " + pend) if pend else "active"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
