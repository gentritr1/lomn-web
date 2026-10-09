"""M2a-2 T5: the A10 taint pass of the Python oracle (docs/osi/natives.md §7: tiers and the taint sinks, Decision 7).

  osi_taint.py --osi B [--real-natives [--install D]] [--frames N] [--keys K] [--overrides F]

Runs Setup + N `Process` frames of the script B (with the key calls of K, the A6/A9 driver of osi_vm.py --run) in the
oracle, and traces which native results reach a sink. Output (stdout):
  taint: natives tainted %d, sinks %d (branch %d, gamevar %d, tier1-arg %d, process-result %d, name %d)
  sink <kind> pc %08x frame %6d <- <key>[,<key>…]
  native <key> verdict safe|UNSAFE
- "natives tainted" counts the distinct natives whose results carried a tag on the run (every native hit that is not
  tier1). One `native` line each, in first-hit order: UNSAFE when its key reaches any sink, else safe.
- A sink line is one site: the distinct (kind, pc). Its keys are the union over every time it fired with a tagged value,
  sorted; `frame` is the first time (-1 = Setup). "sinks" counts the lines; the five kind counts sum to it.
- stderr: `osi_taint: frames %d, instructions %d, stop <stop>, %.1f s`. The exit status is 1 when the run stopped on
  an error (the report of the part that ran is still printed), 2 on a usage error or when --real-natives binds nothing.
--overrides defaults to data/osi/native_overrides.json (natives.md §7.2; applied at load as osi_vm.py does).
--real-natives binds the registered native groups (tools/research/osi_natives); it fails closed when none is bound.

Semantics (the plan's Task 5). Every value has a tag, a set of native keys; a host value and a constant have none.
- Sources. The return of every native that is not tier1 (a D11 stub, a decided default, tier2, tier3, mod) carries
  {its key}; so do its by-reference writes and the arguments of the engine calls it makes. A `copy`-kind stub adds the
  tag of argument 0. A tier1 native (an override `tier1`, or a model bound by --real-natives) carries the union of its
  arguments' tags instead: its value is modelled, and a tagged argument is already a sink.
- Propagation. Stack copies (0x32-0x35); locals and frames (0x21-0x25); object members by reference (0x10-0x1a, the
  value read also carries the object reference's tag); arrays per element (0x90-0x96; a read also carries the index's
  and the array reference's tags; a store, insert or removal at a tagged index adds that tag to every element);
  globals (game variables 0x80/0x81 are no-ops in this build, opcodes.md); arithmetic, compares, logic, bitwise ops,
  colour channels, conversions and type tests (the union of the operands); string concatenation (0x70); script-call
  arguments (0x50/0x51 move them under the return marker) and returns (0x52); by-ref writes; copy-kind natives.
- Sinks (natives.md §7.3, Decision 7):
  1. `branch`: the condition of a 0x53. Addition: the target of a 0x50/0x51 (a tagged target decides which code runs);
  2. `gamevar`: the value a 0x81 writes; 0x81 pops nothing in this build, so the kind is counted and never fires;
  3. `tier1-arg`: any argument of a tier1 native;
  4. `process-result`: the result of an outermost `Process` engine call (the re-run rule reads it, spec §3.1);
  5. `name`: the 0x19 member name or 0x83 function name, when the string is tagged. Addition: the 0x18/0x1a member
     names too (the name selects the member, or the error).
- Implicit flows are not propagated: a branch on a tagged value is itself the sink.

The pass reads the VM only: a shadow of the stack (by absolute slot), the globals (by token), the members (by object
handle and property index) and the arrays (by handle) is kept beside it through the documented hook
`osi_vm.Vm.wrap_handlers`, an override of `Vm.call` (engine-call entries: the marker and the arguments) and a wrapper of
the registry's `invoke` (the native boundary). `tools/tests/test_osi_taint.py` checks that all 541 conformance programs
trace byte for byte the same with the pass attached.

The propagation corpus is tests/osi/taint (run_program): `<id>.asm` + `<id>.expect.json`
  {"id", "kind": propagation|sink|implicit|negative, "path", "entries": [entry names, called in order],
   "natives": [a conformance test native (osi_vm.test_native_entry), or a registry-like row with "ret" (a D11 stub);
               either may set "tier"; default "stub"],
   "expect": {"ok": true, "sinks": [{"kind", "pc": "@label", "keys": [...]}], "verdicts": {key: "safe"|"UNSAFE"}},
   "note"}
Sinks and verdicts are compared exactly; an unknown key fails the program (fail closed).
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
import time
from fractions import Fraction
from pathlib import Path
from typing import Optional

REPO = Path(__file__).resolve().parents[2]
for _p in (str(REPO / "tools" / "osi"), str(Path(__file__).resolve().parent)):
    if _p not in sys.path:
        sys.path.insert(0, _p)
import osi_vm  # noqa: E402
from osi_vm import (NOTHING, STACK_SLOTS, T_ARRAY, T_FLOAT, T_INT, T_OBJECT, Variant, cstr, get_token,  # noqa: E402
                    round_p, f32_bits)

DATA = REPO / "data" / "osi"
EMPTY: frozenset = frozenset()
SINK_KINDS = ("branch", "gamevar", "tier1-arg", "process-result", "name")
TIER1_TIERS = frozenset(["tier1", "implemented"])      # an override `tier1`; a model bound by --real-natives


class TaintError(Exception):
    """The shadow lost step with the VM (an internal error of the pass, never a script error): fail closed."""


class NothingBound(Exception):
    pass


# ---------------------------------------------------------------------------------------------------------------
# The report
# ---------------------------------------------------------------------------------------------------------------

class Report:
    def __init__(self):
        self.tagged: dict[str, None] = {}               # natives whose results carried a tag, in first-hit order
        self.sinks: dict[tuple[str, int], list] = {}    # (kind, pc) -> [first frame, set of keys], in first-fire order
        self.stop = "none"
        self.frames = 0
        self.instructions = 0

    def tag(self, key: str) -> None:
        self.tagged.setdefault(key, None)

    def sink(self, kind: str, pc: int, frame: int, taint) -> None:
        if not taint:
            return
        assert kind in SINK_KINDS, kind
        rec = self.sinks.get((kind, pc))
        if rec is None:
            self.sinks[(kind, pc)] = [frame, set(taint)]
        else:
            rec[1] |= taint

    def unsafe(self) -> set:
        out = set()
        for _frame, keys in self.sinks.values():
            out |= keys
        return out

    def kinds_of(self, key: str) -> list:
        return sorted({k for (k, _pc), (_f, keys) in self.sinks.items() if key in keys})

    def verdicts(self) -> dict:
        bad = self.unsafe()
        return {k: ("UNSAFE" if k in bad else "safe") for k in self.tagged}

    def lines(self) -> list[str]:
        per = {k: 0 for k in SINK_KINDS}
        for kind, _pc in self.sinks:
            per[kind] += 1
        out = ["taint: natives tainted %d, sinks %d (%s)" % (
            len(self.tagged), len(self.sinks), ", ".join("%s %d" % (k, per[k]) for k in SINK_KINDS))]
        for (kind, pc), (frame, keys) in self.sinks.items():
            out.append("sink %s pc %08x frame %6d <- %s" % (kind, pc, frame, ",".join(sorted(keys))))
        for key, verdict in self.verdicts().items():
            out.append("native %s verdict %s" % (key, verdict))
        return out


# ---------------------------------------------------------------------------------------------------------------
# The shadow
# ---------------------------------------------------------------------------------------------------------------

def _union(ts) -> frozenset:
    out = EMPTY
    for t in ts:
        if t:
            out = out | t
    return out


# op -> builder (Tracker._b_<name>). Ops not listed change no tag: 0x00, 0x02-0x06 (nop, debug, line), 0x24/0x25 and
# 0x7a/0x7b (in place: the tag stays), 0x30/0x31 (pops), 0x41 and 0x84 (always raise), 0x54, 0x80/0x81 (no-ops in this
# build), and the unused opcodes.
_BUILDERS = {}
for _op in (0x13, 0x14, 0x23, 0x40, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x90):
    _BUILDERS[_op] = "clean_push"
for _op in (0x60, 0x61, 0x62, 0x63, 0x64, 0x6A, 0x6B, 0x6D, 0x6E, 0x6F, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x77, 0x78,
            0xA0, 0xA1, 0xA2, 0xA3):
    _BUILDERS[_op] = "binary"
for _op in (0x12, 0x6C, 0x76, 0x92, 0xA4, 0xA5, 0xA6, 0xA7, 0xB0, 0xB1, 0xB2, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBF):
    _BUILDERS[_op] = "unary"
_BUILDERS.update({
    0x10: "set_member", 0x11: "get_member", 0x15: "set_this", 0x16: "get_this", 0x17: "get_this_fn",
    0x18: "get_member_str", 0x19: "get_member_fn_str", 0x1A: "set_member_str",
    0x21: "get_var", 0x22: "set_var", 0x32: "swap", 0x33: "pull", 0x34: "dupn", 0x35: "dup",
    0x50: "jump", 0x51: "jump", 0x52: "ret", 0x53: "branch", 0xFF: "halt",
    0x82: "native", 0x83: "native",
    0x91: "get_array", 0x93: "set_array", 0x94: "append_array", 0x95: "remove_array", 0x96: "insert_array",
})


class Tracker:
    """The shadow of one VM. ts[i] is the tag of stack slot i (by absolute slot: a pop leaves a stale tag above the top,
    and every push writes the tag of the slot it fills); gt the globals by token; objs[h][i] the property i of script
    object h; arrs[h][i] element i of array h (both built lazily at the first touch, from the live record)."""

    def __init__(self, vm, report: Report):
        self.vm = vm
        self.report = report
        self.ts: list = [EMPTY] * (STACK_SLOTS + 16)
        self.gt: dict[int, frozenset] = {}
        self.objs: dict[int, list] = {}
        self.arrs: dict[int, list] = {}
        self.out_stack: list = []       # the output tag of each native executing (innermost last)
        self.results: list = []         # the result tag of each native that returned, until its 0x82/0x83 pushes it
        self.exit = (EMPTY, None)       # (tag, pc) of the value the last engine call's Run ended with (0x52 / 0xff)
        self.frame = -1

    def sink(self, kind: str, pc: int, taint) -> None:
        self.report.sink(kind, pc, self.frame, taint)

    def wrap(self, op: int, h):
        name = _BUILDERS.get(op)
        return h if name is None else getattr(self, "_b_" + name)(op, h)

    # ---- helpers
    def _t(self, i: int) -> frozenset:
        return self.ts[i] if 0 <= i < len(self.ts) else EMPTY

    def _objt(self, v: Variant):
        """(shadow list, live record) of the script object v, or (None, None)."""
        if v.type != T_OBJECT:
            return None, None
        rec = self.vm.heap.objects.get(v.value)
        if rec is None:
            return None, None
        lst = self.objs.get(v.value)
        if lst is None:
            lst = self.objs[v.value] = [EMPTY] * len(rec[2])
        return lst, rec

    def _arrt(self, v: Variant):
        if v.type != T_ARRAY:
            return None
        rec = self.vm.heap.arrays.get(v.value)
        if rec is None:
            return None
        lst = self.arrs.get(v.value)
        if lst is None:
            lst = self.arrs[v.value] = [EMPTY] * len(rec[1])
        if len(lst) != len(rec[1]):
            raise TaintError("array %d: shadow %d elements, VM %d" % (v.value, len(lst), len(rec[1])))
        return lst

    def _check_arr(self, v: Variant, lst) -> None:
        rec = self.vm.heap.arrays.get(v.value)
        if rec is not None and lst is not None and len(lst) != len(rec[1]):
            raise TaintError("array %d: shadow %d elements, VM %d" % (v.value, len(lst), len(rec[1])))

    def _gtok(self, idx: int) -> Optional[int]:
        g = self.vm.script.globals
        i = idx & 0xFF
        return get_token(cstr(g[i])) if i < len(g) else None

    def _prop(self, rec, m: int) -> Optional[int]:
        return None if rec is None else self.vm._get_property_index(rec, m)

    def _member_index_by_name(self, rec, s: Variant) -> Optional[int]:
        """The property a 0x18/0x1a name selects, with FindMemberID's cache restored afterwards (the handler then does
        the very same lookup on the very same cache)."""
        if rec is None:
            return None
        sc = self.vm.script
        n = len(sc.member_cache)
        try:
            m = sc.find_member_id(self.vm._name_text(s))
        except Exception:                       # the handler raises the script error itself
            return None
        finally:
            del sc.member_cache[n:]
        return self.vm._get_property_index(rec, m)

    # ---- builders: generic
    def _b_clean_push(self, op, h):
        ts = self.ts

        def w(vm):
            d0 = vm.depth
            r = h(vm)
            for i in range(d0, vm.depth):
                ts[i] = EMPTY
            return r
        return w

    def _b_binary(self, op, h):
        ts = self.ts

        def w(vm):
            d0 = vm.depth
            t = (ts[d0 - 1] | ts[d0 - 2]) if d0 >= 2 else EMPTY
            r = h(vm)
            ts[vm.depth - 1] = t
            return r
        return w

    def _b_unary(self, op, h):
        ts = self.ts

        def w(vm):
            d0 = vm.depth
            t = ts[d0 - 1] if d0 >= 1 else EMPTY
            r = h(vm)
            ts[vm.depth - 1] = t
            return r
        return w

    # ---- members
    def _b_set_member(self, op, h):                                        # 0x10: [v, o] -> o.m = v
        ts = self.ts

        def w(vm):
            d0 = vm.depth
            o = vm.stack[d0 - 1] if d0 >= 1 else NOTHING
            lst, rec = self._objt(o)
            i = self._prop(rec, vm._u16(vm.pc + 1))
            tv = self._t(d0 - 2)
            r = h(vm)
            if i is not None:
                lst[i] = tv
            return r
        return w

    def _b_get_member(self, op, h):                                        # 0x11: [o] -> o.m
        ts = self.ts

        def w(vm):
            d0 = vm.depth
            o = vm.stack[d0 - 1] if d0 >= 1 else NOTHING
            lst, rec = self._objt(o)
            i = self._prop(rec, vm._u16(vm.pc + 1))
            t = self._t(d0 - 1) | (lst[i] if i is not None else EMPTY)
            r = h(vm)
            ts[vm.depth - 1] = t
            return r
        return w

    def _b_set_this(self, op, h):                                          # 0x15: [v] -> this.m = v
        def w(vm):
            d0, fr = vm.depth, vm.frame
            this = vm.stack[fr] if 0 <= fr < len(vm.stack) else NOTHING
            lst, rec = self._objt(this)
            i = self._prop(rec, vm._u16(vm.pc + 1))
            tv = self._t(d0 - 1)
            r = h(vm)
            if i is not None:
                lst[i] = tv
            return r
        return w

    def _b_get_this(self, op, h):                                          # 0x16: -> this.m
        ts = self.ts

        def w(vm):
            fr = vm.frame
            this = vm.stack[fr] if 0 <= fr < len(vm.stack) else NOTHING
            lst, rec = self._objt(this)
            i = self._prop(rec, vm._u16(vm.pc + 1))
            t = self._t(fr) | (lst[i] if i is not None else EMPTY)
            r = h(vm)
            ts[vm.depth - 1] = t
            return r
        return w

    def _b_get_this_fn(self, op, h):                                       # 0x17: -> this's method offset
        ts = self.ts

        def w(vm):
            t = self._t(vm.frame)
            r = h(vm)
            ts[vm.depth - 1] = t
            return r
        return w

    def _b_get_member_str(self, op, h):                                    # 0x18: [o, s] -> o.<s>
        ts = self.ts

        def w(vm):
            d0 = vm.depth
            ns, to = self._t(d0 - 1), self._t(d0 - 2)
            self.sink("name", vm.pc, ns)
            o = vm.stack[d0 - 2] if d0 >= 2 else NOTHING
            s = vm.stack[d0 - 1] if d0 >= 1 else NOTHING
            lst, rec = self._objt(o)
            i = self._member_index_by_name(rec, s)
            el = (lst[i] if i is not None else _union(lst)) if lst is not None else EMPTY
            t = ns | to | el
            r = h(vm)
            ts[vm.depth - 1] = t
            return r
        return w

    def _b_get_member_fn_str(self, op, h):                                 # 0x19: [o, s] -> o's method <s>
        ts = self.ts

        def w(vm):
            d0 = vm.depth
            ns = self._t(d0 - 1)
            self.sink("name", vm.pc, ns)
            t = ns | self._t(d0 - 2)
            r = h(vm)
            ts[vm.depth - 1] = t
            return r
        return w

    def _b_set_member_str(self, op, h):                                    # 0x1a: [v, o, s] -> o.<s> = v
        def w(vm):
            d0 = vm.depth
            self.sink("name", vm.pc, self._t(d0 - 1))
            o = vm.stack[d0 - 2] if d0 >= 2 else NOTHING
            s = vm.stack[d0 - 1] if d0 >= 1 else NOTHING
            lst, rec = self._objt(o)
            i = self._member_index_by_name(rec, s)
            tv = self._t(d0 - 3)
            r = h(vm)
            if lst is not None:
                if i is not None:
                    lst[i] = tv
                else:                                   # not reached: the handler raised on an unknown name
                    for j in range(len(lst)):
                        lst[j] = lst[j] | tv
            return r
        return w

    # ---- variables and the stack
    def _b_get_var(self, op, h):                                           # 0x21
        ts, gt = self.ts, self.gt

        def w(vm):
            idx = vm._u16(vm.pc + 1)
            t = gt.get(self._gtok(idx), EMPTY) if idx & 0x8000 else self._t(vm.frame + idx)
            r = h(vm)
            ts[vm.depth - 1] = t
            return r
        return w

    def _b_set_var(self, op, h):                                           # 0x22
        ts, gt = self.ts, self.gt

        def w(vm):
            idx = vm._u16(vm.pc + 1)
            t = self._t(vm.depth - 1)
            r = h(vm)
            if idx & 0x8000:
                gt[self._gtok(idx)] = t
            else:
                ts[vm.frame + idx] = t
            return r
        return w

    def _b_swap(self, op, h):                                              # 0x32
        ts = self.ts

        def w(vm):
            d0 = vm.depth
            a, b = self._t(d0 - 2), self._t(d0 - 1)
            r = h(vm)
            ts[d0 - 2], ts[d0 - 1] = b, a
            return r
        return w

    def _b_pull(self, op, h):                                              # 0x33 n: copy of slot depth - n
        ts = self.ts

        def w(vm):
            d0 = vm.depth
            t = self._t(d0 - vm._u8(vm.pc + 1))
            r = h(vm)
            ts[vm.depth - 1] = t
            return r
        return w

    def _b_dupn(self, op, h):                                              # 0x34 n: copies of the top n, in order
        ts = self.ts

        def w(vm):
            d0, n = vm.depth, vm._u8(vm.pc + 1)
            src = [self._t(d0 - n + j) for j in range(n)]
            r = h(vm)
            for j in range(n):
                ts[d0 + j] = src[j]
            return r
        return w

    def _b_dup(self, op, h):                                               # 0x35
        ts = self.ts

        def w(vm):
            t = self._t(vm.depth - 1)
            r = h(vm)
            ts[vm.depth - 1] = t
            return r
        return w

    # ---- control
    def _b_jump(self, op, h):                                              # 0x50/0x51 n: [a1..an, target]
        ts = self.ts

        def w(vm):
            d0, n = vm.depth, vm._u8(vm.pc + 1)
            self.sink("branch", vm.pc, self._t(d0 - 1))                 # addition: a tagged target decides the code
            d = d0 - 1
            base = d - n
            moved = ts[base:d] if base >= 0 else []
            r = h(vm)
            ts[base + 1:base + 1 + len(moved)] = moved                   # insert_below: the arguments move up one slot
            ts[base] = EMPTY                                              # the return marker
            return r
        return w

    def _b_ret(self, op, h):                                               # 0x52
        ts = self.ts

        def w(vm):
            pc = vm.pc
            t = self._t(vm.depth - 1)
            r = h(vm)
            ts[vm.depth - 1] = t
            if not r:                                                     # met the engine call's marker: Run ends
                ts[vm.depth - 2] = EMPTY
                self.exit = (t, pc)
            return r
        return w

    def _b_branch(self, op, h):                                            # 0x53: sink 1
        def w(vm):
            self.sink("branch", vm.pc, self._t(vm.depth - 1))
            return h(vm)
        return w

    def _b_halt(self, op, h):                                              # 0xff pushes nothing and ends Run
        ts = self.ts

        def w(vm):
            pc = vm.pc
            r = h(vm)
            ts[vm.depth - 1] = EMPTY
            self.exit = (EMPTY, pc)
            return r
        return w

    # ---- arrays
    def _b_get_array(self, op, h):                                         # 0x91: [a, i] -> a[i]
        ts = self.ts

        def w(vm):
            d0 = vm.depth
            a = vm.stack[d0 - 2] if d0 >= 2 else NOTHING
            i = vm.stack[d0 - 1] if d0 >= 1 else NOTHING
            lst = self._arrt(a)
            iv = i.value & 0xFFFFFFFF
            el = lst[iv] if lst is not None and iv < len(lst) else EMPTY
            t = self._t(d0 - 1) | self._t(d0 - 2) | el
            r = h(vm)
            ts[vm.depth - 1] = t
            return r
        return w

    def _arr_store(self, kind):
        def build(op, h):
            def w(vm):
                d0 = vm.depth
                if kind == "append":                                      # [a, v]
                    a, tv, ti, iv = (vm.stack[d0 - 2] if d0 >= 2 else NOTHING), self._t(d0 - 1), EMPTY, 0
                elif kind == "remove":                                    # [a, i]
                    a, tv, ti = (vm.stack[d0 - 2] if d0 >= 2 else NOTHING), EMPTY, self._t(d0 - 1)
                    iv = vm.stack[d0 - 1].value & 0xFFFFFFFF if d0 >= 1 else 0
                else:                                                     # set / insert: [a, i, v]
                    a, tv, ti = (vm.stack[d0 - 3] if d0 >= 3 else NOTHING), self._t(d0 - 1), self._t(d0 - 2)
                    iv = vm.stack[d0 - 2].value & 0xFFFFFFFF if d0 >= 2 else 0
                lst = self._arrt(a)
                r = h(vm)
                if lst is None:
                    return r
                if kind == "append":
                    lst.append(tv)
                elif kind == "set":
                    lst[iv] = tv | ti
                elif kind == "insert":
                    lst.insert(iv, tv | ti)
                else:
                    del lst[iv]
                if ti:                                  # a tagged index: which element moved is the native's choice
                    for j in range(len(lst)):
                        lst[j] = lst[j] | ti
                self._check_arr(a, lst)
                return r
            return w
        return build

    def _b_set_array(self, op, h):
        return self._arr_store("set")(op, h)

    def _b_append_array(self, op, h):
        return self._arr_store("append")(op, h)

    def _b_remove_array(self, op, h):
        return self._arr_store("remove")(op, h)

    def _b_insert_array(self, op, h):
        return self._arr_store("insert")(op, h)

    # ---- natives
    def _b_native(self, op, h):                                            # 0x82 / 0x83
        ts, results = self.ts, self.results

        def w(vm):
            if op == 0x83:
                self.sink("name", vm.pc, self._t(vm.depth - 1))          # the function name, popped first
            n0 = len(results)
            try:
                r = h(vm)
                ts[vm.depth - 1] = results[n0] if len(results) > n0 else EMPTY
                return r
            finally:
                del results[n0:]
        return w

    def invoke(self, call, orig):
        """The native boundary (the registry's invoke): the tier1-arg sink, the output tag, the by-ref writes."""
        e = call.entry
        ts = self.ts
        args = [ts[s] for s in call.slots]
        if e.tier in TIER1_TIERS:
            out = _union(args)
            self.sink("tier1-arg", call.site_pc, out)
        else:
            self.report.tag(e.key)
            out = frozenset([e.key])
            if e.ret == "copy" and e.default is None and e.fn is None and args:
                out = out | args[0]
        self.out_stack.append(out)
        try:
            r = orig(call)
        finally:
            self.out_stack.pop()
        for i, _v in call.writes:
            ts[call.slots[i]] = out
        self.results.append(out)
        return r


class TaintVm(osi_vm.Vm):
    """osi_vm.Vm with the taint pass attached (Tracker): the handlers wrapped through Vm.wrap_handlers, the registry's
    invoke wrapped on this VM's registry instance, and Vm.call overridden for engine-call entries."""

    def __init__(self, script, natives, heap, trace=None, *, fpu_pc: int = 24, report: Optional[Report] = None):
        super().__init__(script, natives, heap, trace, fpu_pc=fpu_pc)
        self.taint = Tracker(self, report if report is not None else Report())
        self.wrap_handlers(self.taint.wrap)
        orig = natives.invoke
        natives.invoke = lambda call: self.taint.invoke(call, orig)

    def call(self, name, args=()):
        """An engine call pushes its 0xff marker at the current depth and the argc arguments above it: host arguments
        are clean; those of a call made inside a native carry that native's output tag. The outermost Process result
        is sink 4."""
        t = self.taint
        tag = t.out_stack[-1] if t.out_stack else EMPTY
        d0 = self.depth
        ts = t.ts
        if 0 <= d0 < len(ts):
            ts[d0] = EMPTY
        for i in range(d0 + 1, min(d0 + 11, len(ts))):
            ts[i] = tag
        outer = self._nest == 0
        t.exit = (EMPTY, None)
        r = super().call(name, args)
        nb = name.encode("latin-1") if isinstance(name, str) else bytes(name)
        if outer and r.ok and nb.lower() == b"process":
            t.sink("process-result", t.exit[1], t.exit[0])
        return r


# ---------------------------------------------------------------------------------------------------------------
# The Base.osi driver (osi_vm.run_base's frame loop: ScProcessManager parity 0x4a92fd, the re-run rule 0x4a930d..0x4a957d)
# ---------------------------------------------------------------------------------------------------------------

def run_base(script: bytes, frames: int, keys: str = "", overrides=None, natives=osi_vm.DEFAULT_NATIVES,
             mods=osi_vm.DEFAULT_MODS, types=osi_vm.DEFAULT_TYPES, real_natives: bool = False, install=None) -> Report:
    tt = osi_vm.TypeTable.load(types)
    reg = osi_vm.NativeRegistry.load(natives, mods, tt)
    if overrides:
        osi_vm.apply_overrides(reg, overrides)
    heap = osi_vm.Heap(None)
    rep = Report()
    vm = TaintVm(script, reg, heap, None, report=rep)
    tr = vm.taint
    if real_natives:
        from osi_natives import framework
        vfs = framework.Vfs(install) if install else None
        ctx = framework.HostContext(vm, heap, vfs, framework.HostFlags(), None)
        framework.bind_natives(reg, ctx)
        if not any(e.tier == "implemented" for e in reg.entries.values()):
            raise NothingBound("--real-natives: nothing is bound (no native group is registered in osi_natives)")
    kc = osi_vm.key_calls(keys) if keys else {}
    dt = struct.unpack("<d", struct.pack("<Q", osi_vm.DT_BITS_DOUBLE))[0]
    t = 0.0
    pm_flag, proc_byte = 0, 1
    tr.frame = -1
    r = vm.call("Setup", [])
    if not r.ok:
        e = vm.last_error
        rep.stop = 'error "%s" in Setup at pc %08x op %02x' % (e.message, e.pc, e.op)
    else:
        for k in range(frames):
            tr.frame = k
            t = float(round_p(Fraction(t) + Fraction(dt), vm.fpu_pc))
            tb, db = f32_bits(t), f32_bits(dt)
            vm.begin_frame(k)
            err = None
            for name, args in kc.get(k, []):
                r = vm.call(name, args)
                if not r.ok:
                    err = vm.last_error
                    break
            pm_flag ^= 1
            passes = 0
            eligible = proc_byte == pm_flag
            while err is None and eligible:
                r = vm.call("Process", [Variant(T_FLOAT, tb), Variant(T_FLOAT, db), Variant(T_INT, k)])
                passes += 1
                if not r.ok:
                    err = vm.last_error
                    break
                if not (r.result.type == T_INT and r.result.value == 0):
                    proc_byte = pm_flag ^ 1
                    break
                if passes >= 11:
                    break
            rep.frames = k + 1
            if err is not None:
                rep.stop = 'error "%s" at frame %d pc %08x op %02x' % (err.message, k, err.pc, err.op)
                break
    rep.instructions = vm.instructions
    return rep


def flip_violations(flip_results, rep: Report, tier1: set, must_game_returning) -> list[str]:
    """Review focus 5 (C2): a flip-probe CHANGED native must be UNSAFE; a CHANGED-NULL native must be tier1 in the merged
    overrides or a "must (game-returning)" native of Tasks 7-9 (its only probed alternative, nothing, is refused by the
    next native's argument conversion, so the probe cannot say what the real object changes)."""
    bad = []
    unsafe = rep.unsafe()
    for r in flip_results:
        if r.verdict == "CHANGED" and r.key not in unsafe:
            bad.append("CHANGED %s: no sink (verdict %s)" % (r.key, "safe" if r.key in rep.tagged else "not tagged"))
        elif r.verdict == "CHANGED-NULL" and r.key not in tier1 and r.key not in must_game_returning:
            bad.append("CHANGED-NULL %s: not tier1 and not a must (game-returning) native" % r.key)
    return bad


# ---------------------------------------------------------------------------------------------------------------
# The propagation corpus (tests/osi/taint)
# ---------------------------------------------------------------------------------------------------------------

_SPEC_KEYS = {"id", "kind", "path", "entries", "natives", "expect", "note"}
_EXPECT_KEYS = {"ok", "sinks", "verdicts"}
_ROW_KEYS = {"cls", "name", "min", "max", "types", "ret", "ret_class", "default", "tier"}


def _corpus_entry(spec: dict, tt, reg) -> osi_vm.NativeEntry:
    if "ret" in spec:                                   # a registry-like row: the D11 stub of its ret column
        extra = set(spec) - _ROW_KEYS
        if extra:
            raise ValueError("unknown native key(s) %s" % sorted(extra))
        e = reg.entry_from_row(dict(spec, types=list(spec.get("types", []))))
    else:
        e = osi_vm.test_native_entry({k: v for k, v in spec.items() if k != "tier"}, tt, reg)
    return e._replace(tier=spec.get("tier", "stub"))


def run_program(asm_path) -> list[str]:
    """One corpus program; the list of failures (empty = pass)."""
    import osiasm
    asm_path = Path(asm_path)
    spec = json.loads(asm_path.with_name(asm_path.name[:-4] + ".expect.json").read_text())
    fails = ["unknown key %s" % k for k in sorted(set(spec) - _SPEC_KEYS)]
    fails += ["unknown key expect.%s" % k for k in sorted(set(spec.get("expect", {})) - _EXPECT_KEYS)]
    if fails:
        return fails
    image, info = osiasm.assemble(asm_path.read_text(encoding="latin-1"))
    labels = info["labels"]
    tt = osi_vm.TypeTable.load(osi_vm.DEFAULT_TYPES)
    reg = osi_vm.NativeRegistry(tt)
    for n in spec.get("natives", []):
        reg.add(_corpus_entry(n, tt, reg))
    rep = Report()
    vm = TaintVm(image, reg, osi_vm.Heap(None), None, report=rep)
    ex = spec["expect"]
    ok = True
    for entry in spec.get("entries", ["main"]):
        r = vm.call(entry, [])
        if not r.ok:
            ok = False
            fails.append('entry %s: error "%s" at %08x' % (entry, vm.last_error.message, vm.last_error.pc))
            break
    if ok is not ex.get("ok", True):
        fails.append("ok: expected %r, got %r" % (ex.get("ok", True), ok))
    want = set()
    for s in ex.get("sinks", []):
        pc = osi_vm._pc_spec(s["pc"], labels)
        want.add((s["kind"], pc, tuple(sorted(s["keys"]))))
    got = {(kind, pc, tuple(sorted(keys))) for (kind, pc), (_f, keys) in rep.sinks.items()}
    for kind, pc, keys in sorted(want - got, key=str):
        fails.append("sink missing: %s pc %s <- %s" % (kind, "?" if pc is None else "%08x" % pc, ",".join(keys)))
    for kind, pc, keys in sorted(got - want, key=str):
        fails.append("sink unexpected: %s pc %08x <- %s" % (kind, pc, ",".join(keys)))
    if rep.verdicts() != ex.get("verdicts", {}):
        fails.append("verdicts: expected %s, got %s" % (ex.get("verdicts", {}), rep.verdicts()))
    return fails


# ---------------------------------------------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------------------------------------------

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--osi", required=True)
    ap.add_argument("--frames", type=int, default=600)
    ap.add_argument("--keys", default="")
    ap.add_argument("--overrides", help="default: data/osi/native_overrides.json")
    ap.add_argument("--real-natives", action="store_true")
    ap.add_argument("--install", help="the install directory the native models read (with --real-natives)")
    ap.add_argument("--natives", default=str(osi_vm.DEFAULT_NATIVES))
    ap.add_argument("--mods", default=str(osi_vm.DEFAULT_MODS))
    ap.add_argument("--types", default=str(osi_vm.DEFAULT_TYPES))
    a = ap.parse_args(argv)
    overrides = a.overrides
    if overrides is None and (DATA / "native_overrides.json").exists():
        overrides = str(DATA / "native_overrides.json")
    t0 = time.monotonic()
    try:
        rep = run_base(Path(a.osi).read_bytes(), a.frames, a.keys, overrides, a.natives, a.mods, a.types,
                       a.real_natives, a.install)
    except NothingBound as e:
        print("osi_taint.py: %s" % e, file=sys.stderr)
        return 2
    print("\n".join(rep.lines()))
    print("osi_taint: frames %d, instructions %d, stop %s, %.1f s" % (rep.frames, rep.instructions, rep.stop,
                                                                     time.monotonic() - t0), file=sys.stderr)
    return 0 if rep.stop == "none" else 1


if __name__ == "__main__":
    sys.exit(main())
