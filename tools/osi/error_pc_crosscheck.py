"""M2a-2 T2b (T2a-pc review M5): cross-check the error-PC table against the Python oracle.

Every `expect_error` program under the given corpus directories is assembled (tools/osi/osiasm.py) and run through the
oracle (tools/research/osi_vm.py: `Vm.call`, then `snapshot()` before any reset). The measured delta is
`snapshot().pc - last_error.pc`. It is compared with docs/osi/opcodes.md "Error PC (the PC at the throw), per handler"
(the generated summary table) and conformance.md rule 15:

  table  an error the original raises: the row's "PC at its errors" column. Two-valued cells are resolved by message:
         0x82 "+5 / +6" is +5 for "Call to unregistered Game Function", else +6; 0x83 "+3 / +4" is +3 for "Invalid
         member type..." and kOutOfRange (the name copies), else +4.
  stack  the port's "Stack underflow" / "Stack overflow (1000 slots)": the "PC at its ScOSIStack calls" column, the pop
         value for an underflow and the push value for an overflow (rule 15(c)).
  rule   not a table value: the literal 0x84 (+0), SetPCAndArgs argc > 10 (the entry, +0), and any error raised inside
         a nested engine call (snapshot context_depth > 1: rule 15(b), the PC is in the inner function). Rule cases
         with a fixed delta are still compared; the nested ones are only counted.

The oracle is an independent reader written from the asm in M2a-1, so agreement is a cross-check, not a proof: the
table's own citations are the hand-read pins in tools/tests/test_osi_error_pc.py.

Usage: error_pc_crosscheck.py [DIR ...] [--opcodes docs/osi/opcodes.md] [-v]
       (default DIRs: tests/osi/conformance/a and b; group n is pending Tasks 3/4)
Prints one line per MISMATCH (every program with -v) and a summary line; exit 1 on any mismatch."""
from __future__ import annotations

import argparse
import importlib.util
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
UNUSED_ROW = "160 unused ops"
STACK_MESSAGES = {"Stack underflow": "pop", "Stack overflow (1000 slots)": "push"}
RULE_MESSAGES = {"Direct call without a memoised site": 0,                              # rule 15(c): literal 0x84
                 "Too many args required (max 10) to call requested entry point.": 0}  # rule 5: PC = the entry
SUMMARY_HEAD = "| op | handler | PC writes (instruction → PC after it) | PC at its errors | PC at its ScOSIStack calls "


def _load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


def load_table(opcodes_md: Path) -> dict[int, dict]:
    """{op: {"errors": cell, "stack": cell}} from the generated summary table (all 256 ops; the unused ops share a row)."""
    text = Path(opcodes_md).read_text()
    sec = text.split(SUMMARY_HEAD, 1)[1].split("\n\n", 1)[0]
    out, unused = {}, None
    for line in sec.splitlines()[2:]:
        c = [x.strip() for x in line.strip().strip("|").split(" | ")]
        if len(c) < 7:
            continue
        cell = {"errors": c[3], "stack": c[4]}
        if c[0].startswith(UNUSED_ROW):
            unused = cell
        else:
            out[int(c[0], 16)] = cell
    for op in range(256):
        out.setdefault(op, unused)
    return out


def _plus(text: str) -> list[int]:
    return [int(x) for x in re.findall(r"\+(\d+)", text)]


def expected(table: dict, op: int, message: str, nested: bool) -> tuple[str, int | None]:
    """(kind, delta): kind is table / stack / rule; delta None = not compared (a nested error, rule 15(b))."""
    if message in RULE_MESSAGES:
        return "rule", RULE_MESSAGES[message]
    if nested:
        return "rule", None
    row = table[op]
    if message in STACK_MESSAGES:
        part = [p for p in row["stack"].split(";") if p.strip().startswith(STACK_MESSAGES[message])]
        vals = _plus(part[0]) if part else []
        return "stack", (vals[-1] if vals else None)
    vals = _plus(row["errors"])
    if len(vals) == 1:
        return "table", vals[0]
    if op == 0x82:
        return "table", vals[0] if message == "Call to unregistered Game Function" else vals[1]
    if op == 0x83:
        first = message.startswith("Invalid member type") or message == "kOutOfRange"
        return "table", vals[0] if first else vals[1]
    raise ValueError("op %#04x: cannot resolve %r for %r" % (op, row["errors"], message))


def _oracle():
    sys.path.insert(0, str(REPO / "tools" / "osi"))          # osi_vm imports osiasm by name, as its own run_program does
    return _load("osi_vm_xc", REPO / "tools" / "research" / "osi_vm.py"), _load("osiasm", REPO / "tools" / "osi" / "osiasm.py")


def run_one(vmod, asm, asm_path: Path) -> dict:
    """Run one expect_error program in the oracle; return the error and the snapshot (never compared to the spec)."""
    spec = json.loads(asm_path.with_name(asm_path.name[:-4] + ".expect.json").read_text())
    image, _info = asm.assemble(asm_path.read_text(encoding="latin-1"))
    types = vmod.TypeTable.load(vmod.DEFAULT_TYPES)
    if spec.get("registry", "none") == "natives.json":
        reg = vmod.NativeRegistry.load(vmod.DEFAULT_NATIVES, vmod.DEFAULT_MODS, types)
    else:
        reg = vmod.NativeRegistry(types)
    for n in spec.get("natives", []):
        reg.add(vmod.test_native_entry(n, types, reg))
    heap = vmod.Heap(None)
    vm = vmod.Vm(image, reg, heap, None)
    args = [vmod.host_variant(vm, a, types) for a in spec.get("args", [])]
    res = vm.call(spec.get("entry", "main"), args)
    snap = vm.snapshot()
    if res.ok:
        return {"ok": True}
    e = vm.last_error
    return {"ok": False, "pc": e.pc, "op": e.op, "message": e.message, "snap_pc": snap.pc,
            "context_depth": snap.context_depth}


def crosscheck(dirs, opcodes_md: Path = REPO / "docs" / "osi" / "opcodes.md") -> list[dict]:
    table = load_table(opcodes_md)
    vmod, asm = _oracle()
    out = []
    for d in dirs:
        for exp in sorted(Path(d).glob("*.expect.json")):
            if "expect_error" not in json.loads(exp.read_text()):
                continue
            pid = exp.name[: -len(".expect.json")]
            got = run_one(vmod, asm, exp.with_name(pid + ".asm"))
            row = {"id": "%s/%s" % (Path(d).name, pid)}
            if got["ok"]:
                row.update(kind="table", verdict="MISMATCH", why="the oracle call succeeded")
                out.append(row)
                continue
            kind, want = expected(table, got["op"], got["message"], got["context_depth"] > 1)
            delta = got["snap_pc"] - got["pc"]
            verdict = "COUNTED" if want is None else ("MATCH" if delta == want else "MISMATCH")
            row.update(kind=kind, verdict=verdict, op=got["op"], delta=delta, want=want, message=got["message"])
            out.append(row)
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("dirs", nargs="*", type=Path)
    ap.add_argument("--opcodes", type=Path, default=REPO / "docs" / "osi" / "opcodes.md")
    ap.add_argument("-v", action="store_true", help="print every program, not only mismatches")
    a = ap.parse_args(argv)
    dirs = a.dirs or [REPO / "tests" / "osi" / "conformance" / g for g in ("a", "b")]
    res = crosscheck(dirs, a.opcodes)
    for r in res:
        if a.v or r["verdict"] == "MISMATCH":
            print("%-8s %-5s %s op %s delta %s want %s %s" % (
                r["verdict"], r["kind"], r["id"], "%#04x" % r["op"] if "op" in r else "-", r.get("delta", "-"),
                r.get("want", "-"), json.dumps(r.get("message", r.get("why", "")))))
    n = {}
    for r in res:
        n[(r["kind"], r["verdict"])] = n.get((r["kind"], r["verdict"]), 0) + 1
    bad = sum(v for (k, verdict), v in n.items() if verdict == "MISMATCH")
    print("error-pc crosscheck: %d error programs; table %d/%d, stack %d/%d, rule %d (%d compared, %d nested); "
          "mismatches %d" % (
              len(res), n.get(("table", "MATCH"), 0), sum(v for (k, _), v in n.items() if k == "table"),
              n.get(("stack", "MATCH"), 0), sum(v for (k, _), v in n.items() if k == "stack"),
              sum(v for (k, _), v in n.items() if k == "rule"), n.get(("rule", "MATCH"), 0),
              n.get(("rule", "COUNTED"), 0), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
