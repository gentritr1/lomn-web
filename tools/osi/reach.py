"""M2a-2 T1: static reachability walks, over the decompiled corpus and over Base.osi. Nothing is run.

  reach.py --vm-reentry                natives whose body reaches ScOSIVirtualMachine::Call 0x60b320 / CallWithReturn 0x60b450
  reach.py --argc-violations           the 20 Base.osi 0x82 sites whose argc is outside the registered [min, max]
  reach.py --0x19                      the 0x19 GetMemberFunctionFromString sites and where their name string comes from
  options: --corpus DIR (default gen/decomp)  --osi BASE.OSI (default $LOMN_DATA/install/data/Base.osi)
           --natives data/osi/natives.json

--vm-reentry: a reverse breadth-first search over gen/decomp/calls.jsonl from the two targets (direct calls only; an edge
to an address that is not in functions.jsonl is skipped). A native reaches the targets when its registry `body` is on
the search; `depth` is the number of calls on the shortest chain and `chain` names it. The last line is the summary,
including which of them are on the stub path (hit by the typed-stub oracle run of Setup + 30 frames; needs Base.osi).
Skips with `SKIPPED (run make decomp first)` when the corpus is absent.

The script walk (--argc-violations, --0x19), the reachability rule:
  - A subroutine (a global function, or a class method body; bodies shared by several classes are one subroutine) is
    reachable from the roots setup, process, input and onevent (the engine entry names).
  - A 0x50/0x51 target is reachable if its enclosing subroutine is. A 0x50 target is the file offset pushed by a
    0x40/0x41/0x42 immediate; here ANY such immediate equal to a global-function offset counts (address-taken: a
    superset of "the push in front of a 0x50").
  - A 0x51 target is a method: it is reachable if any class reachable by name has a method with that symbol. A class is
    reachable (constructed) when a reachable subroutine executes 0x13 CreateObject for it. A symbol is taken by a reachable
    0x12 or 0x17 operand, or by a 0x19 whose name comes from a literal.
  - A 0x19 (or 0x83) name that comes from a 0x11/0x16 literal, here: the instruction immediately before it is a 0x46
    PushConstantString, resolves to the function of that name. Any other source is UNRESOLVED.
For each argc site the verdict is REACHABLE/UNREACHABLE under the rule; `wild` is the verdict when an UNRESOLVED 0x19
in a reachable subroutine may name ANY method of a constructed class (the conservative closure).
Caveat printed with --argc-violations: classes can also be constructed by ScSLOSIFileIO::GetVariantFromBuffer
0x6160a0 (deserialising a saved object), which no walk of the script sees.
"""
from __future__ import annotations

import argparse
import collections
import importlib.util
import json
import os
import re
import sys
from pathlib import Path
from typing import NamedTuple, Optional

REPO = Path(__file__).resolve().parents[2]
CALL, CALLRET = 0x60B320, 0x60B450
GETVARIANT = 0x6160A0
_ADDR = re.compile(r"0x[0-9a-fA-F]{1,8}")
ROOTS = ("setup", "process", "input", "onevent")
DATA = REPO / "data" / "osi"


def _osi_module():
    spec = importlib.util.spec_from_file_location("osi_research", REPO / "tools" / "research" / "osi.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# ------------------------------------------------------------------------------------------------ the corpus walk

class Corpus(NamedTuple):
    names: dict      # addr -> name
    calls: dict      # addr -> [callee addr], internal edges only


def load_corpus(path) -> Optional[Corpus]:
    path = Path(path)
    fp, cp = path / "functions.jsonl", path / "calls.jsonl"
    if not fp.exists() or not cp.exists():
        return None
    names = {}
    for ln in fp.read_text().splitlines():
        r = json.loads(ln)
        names[int(r["addr"], 16)] = r["name"]
    calls: dict[int, list[int]] = {}
    for ln in cp.read_text().splitlines():
        r = json.loads(ln)
        f = int(r["from"], 16)
        if f in names:
            # an external edge is `0xEXTERNAL:<n>` (an import) or an address outside functions.jsonl: both are skipped
            calls[f] = [t for t in (int(x, 16) for x in r["to"] if _ADDR.fullmatch(x)) if t in names]
    return Corpus(names, calls)


def direct_callers(c: Corpus, targets=(CALL, CALLRET)) -> set:
    ts = set(targets)
    return {f for f, to in c.calls.items() if ts & set(to)}


class Reentry(NamedTuple):
    key: str
    body: int
    depth: int
    chain: list


def vm_reentry(c: Corpus, rows, targets=(CALL, CALLRET)) -> list[Reentry]:
    """Natives (winner rows) whose body reaches a target, sorted by (depth, key). The chain is a shortest path; ties
    go to the lowest address so the output is stable."""
    rev: dict[int, list[int]] = collections.defaultdict(list)
    for f, to in c.calls.items():
        for t in to:
            rev[t].append(f)
    dist = {t: 0 for t in targets if t in c.names}
    queue = collections.deque(dist)
    while queue:
        x = queue.popleft()
        for f in rev.get(x, ()):
            if f not in dist:
                dist[f] = dist[x] + 1
                queue.append(f)
    out = []
    for r in rows:
        if not r.get("winner", True):
            continue
        body = int(r["body"], 16)
        if body not in dist or dist[body] == 0:
            continue
        chain, x = [body], body
        while dist[x] > 0:
            x = min(t for t in c.calls[x] if dist.get(t, -1) == dist[x] - 1)
            chain.append(x)
        out.append(Reentry(r["key"], body, dist[body], chain))
    return sorted(out, key=lambda r: (r.depth, r.key))


def format_reentry(r: Reentry, c: Corpus) -> str:
    return "reentry %s depth %d body 0x%08x chain %s" % (r.key, r.depth, r.body, " -> ".join(c.names[a] for a in r.chain))


def reentry_summary(res, rows, c: Corpus, on_path) -> str:
    n = sum(1 for r in rows if r.get("winner", True))
    head = "vm-reentry: %d of %d natives reach 0x60b320/0x60b450 (direct callers of the targets: %d); " % (
        len(res), n, len(direct_callers(c)))
    if on_path is None:
        return head + "on the stub path n/a (no Base.osi)"
    on = [r for r in res if r.key in on_path]
    return head + "on the stub path %d: %s" % (len(on), ", ".join("%s %d" % (r.key, r.depth) for r in on))


def stub_path_natives(osi_path, frames: int = 30) -> set:
    """The natives the typed-stub oracle run of Setup + `frames` frames calls (69 on the shipped Base.osi)."""
    sys.path[:0] = [str(REPO / "tools" / "osi")]
    import flip_probe
    run = flip_probe.run_once(Path(osi_path).read_bytes(), DATA / "natives.json", DATA / "natives_beta150.json",
                              DATA / "types.json", frames=frames, deadline=60)
    return set(run.natives)


# ------------------------------------------------------------------------------------------------ the script walk

class Argc(NamedTuple):
    key: str
    argc: int
    lo: int
    hi: int
    pc: int
    names: list
    verdict: str
    wild: str
    reason: str


class Site19(NamedTuple):
    pc: int
    names: list
    source: str
    resolved: Optional[str]
    verdict: str


class Walk:
    """The subroutines of a Base.osi image and the reachability closure over them."""

    def __init__(self, data: bytes):
        osi = _osi_module()
        o = osi.parse(data)
        self.o = o
        self._names: dict[int, list[str]] = collections.defaultdict(list)
        self.func_offs: dict[int, str] = {}
        self.methods: dict[tuple, set] = collections.defaultdict(set)         # (symbol, class) -> offsets
        self.class_methods: dict[int, list] = collections.defaultdict(list)    # class -> [(symbol, offset)]
        self.owners: dict[int, list] = collections.defaultdict(list)           # offset -> [(class, symbol)]
        for name, off, _ in o["functions"]:
            self._names[off].append(name.lower())
            self.func_offs[off] = name.lower()
        for ci, (cname, _, meths) in enumerate(o["classes"]):
            for sym, off in meths:
                self._names[off].append(cname.lower() + "." + o["symbols"][sym].lower())
                self.methods[(sym, ci)].add(off)
                self.class_methods[ci].append((sym, off))
                self.owners[off].append((ci, sym))
        self.code = {off: osi.disasm(data, off) for off in self._names}
        self.sym_by_name: dict[str, list[int]] = collections.defaultdict(list)
        for i, s in enumerate(o["symbols"]):
            self.sym_by_name[s.lower()].append(i)
        self.roots = [off for off, n in self.func_offs.items() if n in ROOTS]
        self.constructors: dict[int, list[int]] = collections.defaultdict(list)
        for off, ins in self.code.items():
            for _, op, a in ins:
                if op == 0x13:
                    self.constructors[a[0]].append(off)
        self.reach, self.classes, self.symbols, self.unresolved_reached = self._closure(wild=False)
        self.wild_reach = self._closure(wild=True)[0]

    @property
    def reachable(self) -> dict:
        return self.reach

    def names(self, off: int) -> list[str]:
        return self._names[off]

    def literal_name(self, ins, i):
        """The name a 0x19 at index i takes, if the instruction before it is a PushConstantString literal."""
        if i and ins[i - 1][1] == 0x46:
            strings = self.o["strings"]
            k = ins[i - 1][2][0]
            return strings[k] if k < len(strings) else None
        return None

    def _closure(self, wild: bool):
        reach: dict[int, tuple] = {}
        classes: dict[int, int] = {}
        symbols: dict[int, int] = {}
        unresolved: list[tuple] = []
        queue = collections.deque()
        state = {"wild": False}

        def add(off, parent):
            if off not in reach:
                reach[off] = parent
                queue.append(off)

        def take(sym, parent):
            if sym not in symbols:
                symbols[sym] = parent
                for ci in classes:
                    for off in self.methods.get((sym, ci), ()):
                        add(off, parent)

        for r in self.roots:
            add(r, None)
        while queue:
            off = queue.popleft()
            ins = self.code[off]
            for i, (at, op, a) in enumerate(ins):
                if op == 0x13:
                    ci = a[0]
                    if ci not in classes:
                        classes[ci] = off
                        for sym, moff in self.class_methods.get(ci, ()):
                            if state["wild"] or sym in symbols:
                                add(moff, off)
                elif op in (0x12, 0x17):
                    take(a[0], off)
                elif op == 0x19:
                    name = self.literal_name(ins, i)
                    if name is not None:
                        for sym in self.sym_by_name.get(name.lower(), ()):
                            take(sym, off)
                    else:
                        unresolved.append((off, at))
                        if wild and not state["wild"]:
                            state["wild"] = True
                            for ci in list(classes):
                                for _, moff in self.class_methods.get(ci, ()):
                                    add(moff, off)
                elif op in (0x40, 0x41, 0x42) and a[0] in self.func_offs:
                    add(a[0], off)
        return reach, classes, symbols, unresolved

    # -------------------------------------------------------------------------------- reporting
    def chain(self, off: int) -> list[str]:
        out, seen = [], set()
        while off is not None and off not in seen:
            seen.add(off)
            out.append(self.names(off)[0])
            off = self.reach[off]
        return out[::-1]

    def why_unreachable(self, off: int) -> str:
        parts = []
        cl = self.o["classes"]
        if not self.owners.get(off):
            return "function %s is never called (no reachable 0x50 target or pushed offset)" % self.names(off)[0]
        for ci, sym in self.owners[off]:
            cname, symname = cl[ci][0].lower(), self.o["symbols"][sym].lower()
            if ci not in self.classes:
                cons = self.constructors.get(ci, [])
                if cons:
                    where = ", ".join(sorted({self.names(c)[0] for c in cons})[:3])
                    parts.append("class %s is never constructed in reachable code (0x13 only in unreachable %s)" % (
                        cname, where))
                else:
                    parts.append("class %s is never constructed in reachable code (no 0x13 for it anywhere)" % cname)
            else:
                parts.append("class %s is constructed, but symbol '%s' is never taken by a reachable 0x12/0x17/0x19"
                             % (cname, symname))
        return "; ".join(parts)

    def argc_sites(self, rows) -> list[Argc]:
        win = {r["key"]: r for r in rows if r.get("winner", True)}
        strings = self.o["strings"]
        out = []
        for off, ins in self.code.items():
            for at, op, a in ins:
                if op != 0x82:
                    continue
                r = win.get((strings[a[0]] + "." + strings[a[1]]).lower())
                if r is None or r["min"] <= a[2] <= r["max"]:
                    continue
                if off in self.reach:
                    verdict, reason = "REACHABLE", "reached: " + " > ".join(self.chain(off))
                else:
                    verdict, reason = "UNREACHABLE", self.why_unreachable(off)
                out.append(Argc(r["key"], a[2], r["min"], r["max"], at, self.names(off), verdict,
                                "REACHABLE" if off in self.wild_reach else "UNREACHABLE", reason))
        return sorted(out, key=lambda s: s.pc)

    def sites_0x19(self) -> list[Site19]:
        symbols = self.o["symbols"]
        out = []
        for off, ins in self.code.items():
            for i, (at, op, a) in enumerate(ins):
                if op != 0x19:
                    continue
                prev = ins[i - 1] if i else None
                lit = self.literal_name(ins, i)
                if lit is not None:
                    src, resolved = 'literal "%s"' % lit, lit
                elif prev is None:
                    src, resolved = "none", None
                elif prev[1] == 0x16:
                    src, resolved = "GetThisMemberValue %s" % symbols[prev[2][0]], None
                elif prev[1] == 0x11:
                    src, resolved = "GetMemberValue %s" % symbols[prev[2][0]], None
                elif prev[1] == 0x21:
                    idx = prev[2][0]
                    gl = self.o["globals"]
                    src = "GetVariableValue %s" % (("global:" + gl[idx & 0xFF]) if idx & 0x8000 and (idx & 0xFF) < len(gl)
                                                   else "frame %d" % idx)
                    resolved = None
                else:
                    src, resolved = "op 0x%02x" % prev[1], None
                out.append(Site19(at, self.names(off), src, resolved, "REACHABLE" if off in self.reach else "UNREACHABLE"))
        return sorted(out, key=lambda s: s.pc)


def format_argc(s: Argc) -> str:
    return "%s %s/%d pc 0x%08x in %s [registered %d..%d] wild %s: %s" % (
        s.verdict, s.key, s.argc, s.pc, ",".join(s.names), s.lo, s.hi, s.wild, s.reason)


def argc_summary(sites: list[Argc]) -> str:
    n_r = sum(1 for s in sites if s.verdict == "REACHABLE")
    n_u = len(sites) - n_r
    wild_more = sum(1 for s in sites if s.verdict == "UNREACHABLE" and s.wild == "REACHABLE")
    return ("argc-violations: %d sites, %d REACHABLE, %d UNREACHABLE (M2a-0 claim 'all statically unreachable': %s); "
            "with an UNRESOLVED 0x19 allowed to name any method: %d more REACHABLE" % (
                len(sites), n_r, n_u, "CONFIRMED" if n_r == 0 else "REFUTED", wild_more))


def format_0x19(s: Site19) -> str:
    tail = "=> RESOLVED %s" % s.resolved if s.resolved is not None else "=> UNRESOLVED"
    return "0x19 pc 0x%08x in %s %s source %s %s" % (s.pc, ",".join(s.names), s.verdict, s.source, tail)


def summary_0x19(sites: list[Site19], n83: int) -> str:
    un = sum(1 for s in sites if s.resolved is None)
    return "0x19: %d sites, %d RESOLVED, %d UNRESOLVED, %d in reachable code; 0x83 sites: %d" % (
        len(sites), len(sites) - un, un, sum(1 for s in sites if s.verdict == "REACHABLE"), n83)


# ------------------------------------------------------------------------------------------------ CLI

def default_osi() -> Optional[Path]:
    d = os.environ.get("LOMN_DATA")
    return Path(d) / "install" / "data" / "Base.osi" if d else None


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--vm-reentry", action="store_true")
    ap.add_argument("--argc-violations", action="store_true")
    ap.add_argument("--0x19", dest="site19", action="store_true")
    ap.add_argument("--corpus", default=str(REPO / "gen" / "decomp"))
    ap.add_argument("--osi")
    ap.add_argument("--natives", default=str(DATA / "natives.json"))
    a = ap.parse_args(argv)
    if not (a.vm_reentry or a.argc_violations or a.site19):
        ap.print_help()
        return 2
    rows = [r for r in json.loads(Path(a.natives).read_text()) if r["winner"]]
    osi_path = Path(a.osi) if a.osi else default_osi()
    have_osi = osi_path is not None and osi_path.exists()
    c = load_corpus(a.corpus)
    if a.vm_reentry:
        if c is None:
            print("SKIPPED (run make decomp first)")
        else:
            res = vm_reentry(c, rows)
            for r in res:
                print(format_reentry(r, c))
            print(reentry_summary(res, rows, c, stub_path_natives(osi_path) if have_osi else None))
    if a.argc_violations or a.site19:
        if not have_osi:
            print("SKIPPED (no LOMN_DATA)")
            return 0
        w = Walk(osi_path.read_bytes())
        if a.argc_violations:
            sites = w.argc_sites(rows)
            for s in sites:
                print(format_argc(s))
            print(argc_summary(sites))
            if c is not None:
                via = vm_reentry(c, rows, targets=(GETVARIANT,))
                print("caveat: ScSLOSIFileIO::GetVariantFromBuffer 0x6160a0 also constructs script objects "
                      "(ConstructObject 0x60be00); %d natives reach it: %s" % (
                          len(via), ", ".join(r.key for r in via) or "-"))
        if a.site19:
            sites = w.sites_0x19()
            for s in sites:
                print(format_0x19(s))
            n83 = sum(1 for ins in w.code.values() for _, op, _ in ins if op == 0x83)
            print(summary_0x19(sites, n83))
    return 0


if __name__ == "__main__":
    sys.exit(main())
