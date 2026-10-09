"""M2a-2 T2a-pc: the PC at every error raise of every OSI opcode handler, by a control-flow pass over the asm.

The canonical exe is read AS DATA (objdump on its .text; it is never run). For each of the 97 distinct handlers of the
`ScOSIVirtualMachine` handler table (tools/osi/opcodes.py), the pass walks every path from the handler entry with a
small abstract interpreter and records, at each error raise, the VM PC relative to the instruction's start (+0, +1,
...). Output: docs/osi/opcodes.md "Error PC (the PC at the throw), per handler", which conformance rule 15 cites.

What the interpreter tracks (everything else is "unknown"):
- `this` (the VM, `ecx` at entry) and pointers `this + k`, through registers and `[ebp +/- n]` frame slots;
- the VM vtable (`mov r, [this]`); the PC value read from `[this + 0x1f58]` plus a constant; immediates, absolute
  globals (compared only for identity) and byte flags in frame slots, joined to unknown where two paths disagree;
- the PC itself as a delta from the instruction start. The handlers write it in exactly four forms:
  `inc [this+0x1f58]`, `add [this+0x1f58], imm`, `add [this+0x1f58], reg` (reg = an operand read from the code: the
  delta becomes `+d+operand`) and `mov [this+0x1f58], reg` (reg = PC + k gives a delta, anything else an absolute
  target `set`). Any other write to that address, or a write at displacement 0x1f58 through an untracked base, raises
  UnmodelledPcWrite: the pass refuses to guess.

Raises:
- `Error` (vtable +0xc, ScOSIVirtualMachine::Error 0x60be80) and the direct throwers SrOSIError 0x5faee0,
  SrThrow 0x4d91f0 and ___throw 0x657260 end the path (they never return). The message is the nearest
  `push imm32` of a C string before the call (the format string; for SrThrow the error object).
- A call to a VM method (a vtable slot, or one of VM_NONVIRTUAL) is followed inline, carrying the PC state, so its
  raises are reported with the call chain (`via`).
- A direct call to any other function is summarised: the function and its direct callees are scanned (recursive
  descent) for the throwers above; if one is reached the call is a `callee` raise site (the path also continues), or
  an `alloc` site when every throw it reaches is an allocation failure (ALLOC_THROWS, std::length_error).
- An indirect call that is not a vtable call must be listed in INDIRECT (natives, landing-pad cleanups, the dead
  game-variable accessors), classified by reading the asm; an unlisted one stops the pass.
- Exception tables (tools/osi/exc.py): a raise at a call site that has a catch record (kind 0x10, !ScCatchMessage! or
  catch-all) is `caught`; the walk continues at the landing pad with the PC unchanged, registers unknown, the frame
  slots kept and the state tagged with the caught raise (`origin`).

Conditions are not evaluated, except a `cmp byte [ebp-n], imm` on a known byte flag (CodeWarrior's try retry and
rethrow flags, only ever written 0) and a `cmp` of two loads of the same global (the kNoException test after a try)
followed by `test r, r` on a known constant. Every other branch is taken both ways, so a raise only an infeasible
path reaches could be listed. Returns reached only after a catch are kept apart (`caught_exits`).

Usage:
  error_pc.py [--exe EXE]                        one line per raise site, per-handler exits and write counts
  error_pc.py --markdown                         the generated block for docs/osi/opcodes.md
  error_pc.py --write DOC / --check DOC          replace / verify that block in DOC (exit 1 when stale)
The exe defaults to $LOMN_DATA/reference/LEGOBionicle.exe.
"""
from __future__ import annotations

import argparse
import importlib.util
import os
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
PC_OFF = 0x1F58
VTABLE = 0x7514E4
ERROR_SLOT = 0xC
VTABLE_SIZE = 0x19C          # 103 slots: the dword at 0x7514e4 + 0x19c is no code address
# ScOSIVirtualMachine methods that are not in the vtable (symbols, gen/decomp/functions.jsonl): Call, GetScript,
# CallWithReturn, ConvertStringIDToDynamicString, ConvertVariant, ConstructObject
VM_NONVIRTUAL = {0x60B320, 0x60B440, 0x60B450, 0x60B880, 0x60B9D0, 0x60BE00}
THROWERS = {0x60BE80: "Error", 0x5FAEE0: "SrOSIError", 0x4D91F0: "SrThrow", 0x657260: "___throw"}
NAMES = {   # names for the report only (from the symbols in gen/decomp/functions.jsonl, M2a-0)
    0x60B9D0: "ConvertVariant", 0x60B880: "ConvertStringIDToDynamicString", 0x60BE00: "ConstructObject",
    0x4F5C80: "ScFixedString::append", 0x601F10: "ScOSIScript::GetStringTableToken",
    0x5FE880: "ScOSIScript::GetMemberTableToken", 0x6086A0: "ScOSIObject::GetProperty",
    0x608600: "ScOSIObject::FindMethod", 0x609BA0: "ScOSIScript::FindMemberID", 0x5FAC40: "ScOSISystem::GetTypeToken",
    0x5FAAE0: "ScOSISystem::GetToken", 0x609D90: "ScOSIScript::GetGlobalVariable", 0x60A930: "ScOSIVariant::Dispose",
    0x60AA20: "ScOSIVariant::Duplicate", 0x5FD0C0: "ScOSIStack::Push", 0x5FD260: "ScOSIStack::Pop",
    0x601EC0: "ScOSIStack::PushInto", 0x607C00: "ScOSIStack::PushIntoFrame", 0x600BA0: "ScOSIStack::Insert",
    0x4E46C0: "operator new", 0x4E4710: "operator delete", 0x657980: "_pow", 0x4F6130: "ScFixedString::Format",
    0x5FA750: "ScOSISystem::ScOSISystem", 0x60B5E0: "SetPCAndArgs", 0x60B450: "CallWithReturn",
    0x600CF0: "BCLReturn", 0x600E70: "BCLBranchAlways", 0x60B8D0: "ScFixedStringTemplate<255> ctor",
    0x46DE40: "ScFixedStringTemplate<255> ctor", 0x4F5C00: "ScFixedString::operator=",
    0x4F5B00: "ScFixedString::operator=", 0x401850: "ScFixedString::Init",
}
# Allocation failures, not modelled by the port: operator new 0x4e46c0 and operator new[] (in `_U`) throw error object
# 0x83aa0c by SrThrow; the std containers throw std::length_error (`!exception!!logic_error!!length_error!!`).
ALLOC_THROWS = {0x4E46E1, 0x4E4761}
ERROR_OBJECTS = {0x83AA20: "kLenTooLong", 0x83913C: "kOutOfRange", 0x83AA0C: "allocation failure"}


def is_alloc_throw(t) -> bool:
    return t[0] in ALLOC_THROWS or "length_error" in t[2]


# Every indirect (non-vtable) call reached from a handler, classified by reading the asm (2026-10-09). The pass refuses
# an indirect call that is not listed here.
_DEAD_ACCESSOR = "registered game-variable accessor (dead: the RegisteredVariable map is never populated)"
_CLEANUP = "landing-pad cleanup of the caught exception object (`push -1`, call through a frame slot)"
INDIRECT = {
    0x5FE793: ("accessor", _DEAD_ACCESSOR + ", +0xc"), 0x5FE7BD: ("accessor", _DEAD_ACCESSOR + ", +0x10"),
    0x5FEB1C: ("accessor", _DEAD_ACCESSOR + ", +0xc"), 0x5FF105: ("accessor", _DEAD_ACCESSOR + ", +0xc"),
    0x5FF665: ("accessor", _DEAD_ACCESSOR + ", +0xc"), 0x5FF69B: ("accessor", _DEAD_ACCESSOR + ", +0x10"),
    0x601AF5: ("native", "the native (RegisteredFunction +0x8)"),
    0x602938: ("native", "the native (RegisteredFunction +0x8)"),
    0x602EFD: ("native", "the native (RegisteredFunction +0x8, from the self-patched pointer)"),
    0x601B79: ("cleanup", _CLEANUP), 0x601CFC: ("cleanup", _CLEANUP), 0x602689: ("cleanup", _CLEANUP),
    0x602A5E: ("cleanup", _CLEANUP), 0x603009: ("cleanup", _CLEANUP),
}
STACK_OPS = {0x5FD260: "pop", 0x5FD0C0: "push", 0x601EC0: "push", 0x607C00: "push", 0x600BA0: "push"}
for _a, _n in THROWERS.items():
    NAMES.setdefault(_a, _n)

REG32 = ("eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp")
SUBREG = {"al": "eax", "ah": "eax", "ax": "eax", "bl": "ebx", "bh": "ebx", "bx": "ebx", "cl": "ecx", "ch": "ecx",
          "cx": "ecx", "dl": "edx", "dh": "edx", "dx": "edx", "si": "esi", "di": "edi", "bp": "ebp", "sp": "esp"}
JCC = re.compile(r"^j(?!mp)[a-z]+$")
STRING_OPS = {p + w for p in ("movs", "stos", "lods", "scas", "cmps") for w in "bwd"}
READ_ONLY = {"cmp", "test", "push", "bt", "nop"}
FPU_STORES = {"fst", "fstp", "fist", "fistp", "fisttp", "fnstcw", "fstcw", "fnstsw", "fstsw", "fbstp", "fnsave",
              "fnstenv"}


class UnmodelledPcWrite(Exception):
    pass


def _load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# ------------------------------------------------------------------------------------------------ the instructions

@dataclass(frozen=True)
class Insn:
    addr: int
    size: int
    mnem: str
    ops: tuple

    @property
    def next(self) -> int:
        return self.addr + self.size


_LINE = re.compile(r"^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} ?)+)\s*(?:\t(.*))?$")


def _split_ops(s: str) -> tuple:
    s = re.sub(r"\s*<[^>]*>", "", s).strip()
    if not s:
        return ()
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch == "[":
            depth += 1
        elif ch == "]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    out.append(cur.strip())
    return tuple(out)


def parse_listing(text: str) -> dict:
    insns, last = {}, None
    for line in text.splitlines():
        m = _LINE.match(line)
        if not m:
            continue
        addr, nbytes, rest = int(m.group(1), 16), len(m.group(2).split()), m.group(3)
        if rest is None:                      # continuation line of a long instruction
            if last is not None and last.next == addr:
                insns[last.addr] = last = Insn(last.addr, last.size + nbytes, last.mnem, last.ops)
            continue
        mnem, _, ops = rest.partition("\t")
        last = Insn(addr, nbytes, mnem.strip(), _split_ops(ops))
        insns[addr] = last
    return insns


class Disasm:
    """addr -> Insn. `from_exe` disassembles .text once; an address the linear sweep did not decode (a desync after
    padding) is decoded on demand from that address."""

    def __init__(self, insns: dict, exe: Path | None = None):
        self.insns, self.exe = insns, exe

    @classmethod
    def from_listing(cls, text: str) -> "Disasm":
        return cls(parse_listing(text))

    @classmethod
    def from_exe(cls, exe: Path) -> "Disasm":
        out = subprocess.run(["objdump", "-d", "--x86-asm-syntax=intel", "-j", ".text", str(exe)],
                             capture_output=True, text=True, timeout=110, check=True).stdout
        return cls(parse_listing(out), exe)

    def at(self, addr: int) -> Insn:
        if addr not in self.insns and self.exe is not None:
            out = subprocess.run(["objdump", "-d", "--x86-asm-syntax=intel", "--start-address=%#x" % addr,
                                  "--stop-address=%#x" % (addr + 0x80), str(self.exe)],
                                 capture_output=True, text=True, timeout=60, check=True).stdout
            got = parse_listing(out)
            # keep the run that starts at addr, up to the first transfer of control
            a = addr
            while a in got:
                self.insns.setdefault(a, got[a])
                if got[a].mnem in ("ret", "jmp"):
                    break
                a = got[a].next
        if addr not in self.insns:
            raise KeyError("no instruction decoded at %#x" % addr)
        return self.insns[addr]

    def prev(self, addr: int, limit: int = 16):
        """Instructions before addr, nearest first (linear order of the listing)."""
        a, n = addr, 0
        while n < limit:
            for k in range(1, 16):
                cand = self.insns.get(a - k)
                if cand is not None and cand.next == a:
                    yield cand
                    a = cand.addr
                    break
            else:
                return
            n += 1


# --------------------------------------------------------------------------------------------- operand evaluation

_MEM = re.compile(r"^(?:(byte|word|dword|qword|tbyte|xmmword) ptr )?(?:[a-z]s:)?\[(.*)\]$")


def _imm(s: str):
    try:
        return int(s, 0)
    except ValueError:
        return None


def parse_mem(op: str):
    """'dword ptr [edi + 4*ecx + 0x1f58]' -> (size, base, index, disp); None if not a memory operand."""
    m = _MEM.match(op)
    if not m:
        return None
    size, expr = m.group(1), m.group(2).replace(" ", "")
    base = index = None
    disp = 0
    for sign, term in re.findall(r"([+-]?)([^+-]+)", expr):
        neg = sign == "-"
        if term in REG32:
            base = term
        elif "*" in term:
            index = term
        else:
            v = int(term, 0)
            disp += -v if neg else v
    return size, base, index, disp


@dataclass
class State:
    regs: dict
    slots: dict
    pc: object                # int delta, or a str label
    pushes: list = field(default_factory=list)
    cmp: object = None        # (known, equal) of the last compare when the pass can evaluate it, or None
    origin: object = None     # on a path through a landing pad: the address of the raise that was caught

    def key(self):
        return (self.pc, tuple(sorted(self.regs.items())), tuple(sorted(self.slots.items())), self.cmp, self.origin)

    def copy(self) -> "State":
        return State(dict(self.regs), dict(self.slots), self.pc, list(self.pushes), self.cmp, self.origin)

    def core(self):
        """What a path is never merged over: the PC, the catch origin and where `this`, its vtable and PC values
        live. Constants (immediates, globals, byte flags) are joined to unknown when two paths disagree."""
        keep = ("this", "vt", "pcv")
        return (self.pc, self.origin,
                tuple(sorted((r, v) for r, v in self.regs.items() if v and v[0] in keep)),
                tuple(sorted((d, v) for d, v in self.slots.items() if v and v[0] in keep)))

    def join(self, other: "State") -> "State":
        regs = {r: v for r, v in self.regs.items() if other.regs.get(r) == v}
        slots = {d: v for d, v in self.slots.items() if other.slots.get(d) == v}
        return State(regs, slots, self.pc, [], self.cmp if self.cmp == other.cmp else None, self.origin)


def pc_label(pc) -> str:
    return "+%d" % pc if isinstance(pc, int) else pc


@dataclass
class Raise:
    raise_addr: int           # the raising instruction (in the handler or an inlined callee)
    via: tuple                # call instructions from the handler down to the raising function
    kind: str                 # Error | SrOSIError | SrThrow | ___throw | callee | indirect
    message: str
    pcs: set = field(default_factory=set)
    caught: int | None = None  # landing pad that catches it, if any
    throws: list = field(default_factory=list)      # kind 'callee'/'alloc': [(throw call, thrower, message, chain)]
    by_origin: dict = field(default_factory=dict)   # pc label -> {None (direct path) or the caught raise's address}

    @property
    def addr(self) -> int:
        return self.raise_addr


@dataclass
class HandlerResult:
    addr: int
    ops: list
    sites: list
    exits: set                # PC labels at a normal return (paths that did not go through a landing pad)
    caught_exits: set = field(default_factory=set)   # returns reached only after a catch (see the module doc)
    stack_ops: dict = field(default_factory=dict)    # 'pop' / 'push' -> PC labels at ScOSIStack calls (direct paths)
    writes: dict = field(default_factory=dict)       # PC-write instruction -> PC labels right after it
    call_targets: dict = field(default_factory=dict)  # inlined call instruction -> VM method address


class Analyzer:
    """vtable: {byte offset: target address, or 'Error'}; strings: .get(addr) -> str or None;
    exc_sites: {function start: {call address: landing pad}}; callee_throws(addr) -> list of reasons."""

    def __init__(self, dis: Disasm, vtable: dict, strings, exc_sites: dict, callee_throws=None, vm_methods=(),
                 indirect=None):
        self.dis, self.vtable, self.strings, self.exc_sites = dis, vtable, strings, exc_sites
        self.indirect = indirect
        self.vm_methods = set(vm_methods) | {t for t in vtable.values() if isinstance(t, int)}
        self.callee_throws = callee_throws or (lambda addr: [])
        self.raises: dict = {}
        self.inline_memo: dict = {}
        self.pc_writes: dict = {}      # instruction address -> form, over every function walked

    # -- helpers
    def _val(self, st: State, op: str):
        if op in REG32:
            return st.regs.get(op)
        v = _imm(op)
        if v is not None:
            return ("c", v & 0xFFFFFFFF)
        m = parse_mem(op)
        if m is None:
            return None
        size, base, index, disp = m
        if base is None and index is None and size in (None, "dword"):
            return ("g", disp)                  # an absolute global, compared only for identity
        if base == "ebp" and index is None and size in (None, "dword"):
            return st.slots.get(disp)
        bv = st.regs.get(base) if base else None
        if bv and bv[0] == "this" and index is None and size in (None, "dword"):
            off = bv[1] + disp
            if off == 0:
                return ("vt",)
            if off == PC_OFF:
                return ("pcv", st.pc) if isinstance(st.pc, int) else None
        return None

    def _set(self, st: State, op: str, v):
        if op in REG32:
            st.regs[op] = v
            return
        if op in SUBREG:
            st.regs[SUBREG[op]] = None
            return
        m = parse_mem(op)
        if m is None:
            return
        size, base, index, disp = m
        if base == "ebp" and index is None:
            if size == "byte" and v is not None and v[0] == "c":
                for k in range(-3, 4):
                    st.slots.pop(disp + k, None)
                st.slots[disp] = ("cb", v[1] & 0xFF)   # a byte flag (the try/catch retry and rethrow flags)
                return
            if size in (None, "dword") and v is not None and v[0] != "c":
                st.slots[disp] = v          # constants are not kept in the frame (flag slots would multiply paths)
            elif size in (None, "dword"):
                st.slots.pop(disp, None)
            else:
                for k in range(-3, 4):
                    st.slots.pop(disp + k, None)
            return
        if self._is_pc(st, base, index, disp):
            raise UnmodelledPcWrite("store of a non-modelled form to the PC")

    def _is_pc(self, st: State, base, index, disp) -> bool:
        if disp == PC_OFF and (base is None or st.regs.get(base) is None or index is not None):
            if base not in (None, "ebp", "esp"):
                return True           # displacement 0x1f58 through an untracked base: refuse to guess
        bv = st.regs.get(base) if base else None
        return bool(bv and bv[0] == "this" and index is None and bv[1] + disp == PC_OFF)

    def _message(self, addr: int, kind: str = "Error") -> str:
        return find_message(self.dis, self.strings, addr, kind)

    def _record(self, kind, raise_addr, via, message, st):
        key = (raise_addr, via)
        r = self.raises.get(key)
        if r is None:
            r = self.raises[key] = Raise(raise_addr, via, kind, message)
        r.pcs.add(pc_label(st.pc))
        r.by_origin.setdefault(pc_label(st.pc), set()).add(st.origin)
        return r

    # -- the walk
    def handler(self, entry: int) -> HandlerResult:
        self.raises, self.stack, self.hwrites, self.targets = {}, {}, {}, {}
        st = State({"ecx": ("this", 0)}, {}, 0)
        exits, _ = self._walk(entry, st, ())
        return HandlerResult(entry, [], sorted(self.raises.values(), key=lambda r: (r.via, r.raise_addr)),
                             {pc_label(p) for p, o in exits if o is None},
                             {pc_label(p) for p, o in exits if o is not None},
                             {k: set(v) for k, v in sorted(self.stack.items())},
                             {a: set(v) for a, v in sorted(self.hwrites.items())}, dict(self.targets))

    def _walk(self, entry: int, st0: State, via: tuple):
        """Explore one function. Returns (exit PC states, escaping raises [(Raise, pc)])."""
        catches = self.exc_sites.get(entry, {})
        seen, exits, escaping = {}, set(), []
        work = [(entry, st0)]
        visits = 0
        while work:
            addr, st = work.pop()
            k = (addr, st.core())
            if k in seen:
                merged = seen[k].join(st)
                if merged.key() == seen[k].key():
                    continue
                merged.pushes = list(st.pushes)
                st = merged
            seen[k] = st.copy()
            visits += 1
            if visits > 50000:
                raise RuntimeError("path explosion in %#x" % entry)
            ins = self.dis.at(addr)

            def raised(r, pc, at=ins.addr, st=st):
                pad = catches.get(at)
                if pad is not None:
                    r.caught = pad
                    origin = st.origin if st.origin is not None else r.raise_addr
                    work.append((pad, State({}, dict(st.slots), pc, origin=origin)))
                else:
                    escaping.append((r, pc))

            m, ops = ins.mnem, ins.ops
            if m == "ret":
                exits.add((st.pc, st.origin))
                continue
            if m == "jmp":
                tgt = _imm(ops[0])
                if tgt is not None:
                    work.append((tgt, st))
                else:
                    for t in self._jump_table(ins):
                        work.append((t, st.copy()))
                continue
            if JCC.match(m):
                known = st.cmp
                st.cmp = None
                if known is not None and m in ("je", "jne"):
                    taken = known[1] if m == "je" else not known[1]
                    work.append((_imm(ops[0]) if taken else ins.next, st))
                    continue
                work.append((_imm(ops[0]), st.copy()))
                work.append((ins.next, st))
                continue
            if m == "call":
                nxt = self._call(ins, st, via, raised)
                if nxt is None:
                    continue
                for s in nxt:
                    work.append((ins.next, s))
                continue
            self._step(ins, st)
            work.append((ins.next, st))
        return exits, escaping

    def _call(self, ins: Insn, st: State, via: tuple, raised):
        """Returns the list of states after the call, or None when the call never returns."""
        op = ins.ops[0]
        tgt = _imm(op)
        pushes = st.pushes
        after = st.copy()
        after.pushes = []
        for r in ("eax", "ecx", "edx"):
            after.regs[r] = None
        if tgt is not None:
            if tgt in STACK_OPS and st.origin is None:
                self.stack.setdefault(STACK_OPS[tgt], set()).add(pc_label(st.pc))
            if tgt in THROWERS:
                r = self._record(THROWERS[tgt], ins.addr, via, self._message(ins.addr, THROWERS[tgt]), st)
                raised(r, st.pc)
                return None
            if tgt in self.vm_methods:
                if st.regs.get("ecx") != ("this", 0):
                    raise UnmodelledPcWrite("VM method %#x called at %#x without `this` in ecx" % (tgt, ins.addr))
                return self._inline(tgt, ins, st, after, via, raised)
            throws = self.callee_throws(tgt)
            if throws:
                real = [t for t in throws if not is_alloc_throw(t)]
                kind = "callee" if real else "alloc"
                msg = "; ".join("%s at %#x" % (t[2], t[0]) for t in (real or throws))
                r = self._record(kind, ins.addr, via, "%s: %s" % (NAMES.get(tgt, "%#x" % tgt), msg), st)
                r.throws = throws
                raised(r, st.pc)
            return [after]
        m = parse_mem(op)
        if m is not None:
            size, base, index, disp = m
            if base and st.regs.get(base) == ("vt",) and index is None:
                t = self.vtable.get(disp)
                if t == "Error":
                    if not pushes or pushes[-1] != ("this", 0):
                        raise UnmodelledPcWrite("Error call at %#x without `this` pushed" % ins.addr)
                    r = self._record("Error", ins.addr, via, self._message(ins.addr), st)
                    raised(r, st.pc)
                    return None
                if t is None:
                    raise KeyError("vtable offset %#x at %#x not known" % (disp, ins.addr))
                return self._inline(t, ins, st, after, via, raised)
        if self.indirect is not None and ins.addr not in self.indirect:
            raise KeyError("unclassified indirect call at %#x" % ins.addr)
        kind, desc = (self.indirect or {}).get(ins.addr, ("indirect", ""))
        r = self._record(kind, ins.addr, via, desc, st)
        raised(r, st.pc)
        return [after]

    def _inline(self, tgt, ins, st, after, via, raised):
        self.targets[ins.addr] = tgt
        key = (tgt, st.pc, via + (ins.addr,))
        if key not in self.inline_memo:
            if len(via) > 6:
                raise RuntimeError("inline depth at %#x" % ins.addr)
            entry = State({"ecx": ("this", 0)}, {}, st.pc)
            self.inline_memo[key] = self._walk(tgt, entry, via + (ins.addr,))
        exits, escaping = self.inline_memo[key]
        for r, pc in escaping:
            raised(r, pc)
        outs = []
        for pc in sorted({p for p, o in exits}, key=str):
            s = after.copy()
            s.pc = pc
            outs.append(s)
        return outs

    def _jump_table(self, ins: Insn):
        m = parse_mem(ins.ops[0])
        if m is None or m[2] is None or m[1] is not None:
            raise RuntimeError("indirect jump at %#x not modelled" % ins.addr)
        idx = m[2].split("*")[1]
        for p in self.dis.prev(ins.addr, 8):
            if p.mnem == "cmp" and p.ops[0] == idx and _imm(p.ops[1]) is not None:
                n = _imm(p.ops[1]) + 1
                return sorted({self.strings.u32(m[3] + 4 * i) for i in range(n)})
        raise RuntimeError("jump table bound not found at %#x" % ins.addr)

    def _step(self, ins: Insn, st: State):
        m, ops = ins.mnem, ins.ops
        st.cmp = None
        if m == "cmp" and len(ops) == 2 and ops[0] in REG32 and ops[1] in REG32:
            a, b = st.regs.get(ops[0]), st.regs.get(ops[1])
            if a and b and a[0] == "g" and b[0] == "g":
                st.cmp = (True, a[1] == b[1])      # two loads of the same constant global (kNoException)
            return
        if m == "test" and len(ops) == 2 and ops[0] == ops[1]:
            a = st.regs.get(SUBREG.get(ops[0], ops[0]))
            if a and a[0] == "c":
                st.cmp = (True, a[1] == 0)
            return
        if m == "cmp" and len(ops) == 2 and _imm(ops[1]) is not None:
            mm = parse_mem(ops[0])
            if mm and mm[0] == "byte" and mm[1] == "ebp" and mm[2] is None:
                v = st.slots.get(mm[3])
                if v is not None and v[0] == "cb":
                    st.cmp = (True, v[1] == (_imm(ops[1]) & 0xFF))
            return
        # PC writes: the four modelled forms, everything else refused
        if ops and m not in READ_ONLY and not (m.startswith("f") and m not in FPU_STORES):
            mm = parse_mem(ops[0])
            if mm is not None and self._is_pc(st, mm[1], mm[2], mm[3]):
                self.pc_writes[ins.addr] = m if m == "inc" else "%s %s" % (m, "imm" if _imm(ops[1]) is not None else "reg")
                if m == "inc" and isinstance(st.pc, int):
                    st.pc += 1
                elif m == "add" and _imm(ops[1]) is not None and isinstance(st.pc, int):
                    st.pc += _imm(ops[1])
                elif m == "add" and ops[1] in REG32:
                    v = st.regs.get(ops[1])
                    if v and v[0] == "c" and isinstance(st.pc, int):
                        st.pc += v[1]
                    else:
                        st.pc = "%s+operand(%#x)" % (pc_label(st.pc), ins.addr)
                elif m == "mov" and ops[1] in REG32:
                    v = st.regs.get(ops[1])
                    st.pc = v[1] if v and v[0] == "pcv" and isinstance(v[1], int) else "set(%#x)" % ins.addr
                else:
                    raise UnmodelledPcWrite("%#x: %s %s" % (ins.addr, m, ", ".join(ops)))
                if st.origin is None:
                    self.hwrites.setdefault(ins.addr, set()).add(pc_label(st.pc))
                return
        if m == "push":
            st.pushes.append(self._val(st, ops[0]))
            return
        if m == "pop":
            v = st.pushes.pop() if st.pushes else None
            self._set(st, ops[0], v)
            return
        if m == "mov":
            self._set(st, ops[0], self._val(st, ops[1]))
            return
        if m == "lea":
            mm = parse_mem(ops[1])
            v = None
            if mm and mm[1] and mm[2] is None:
                bv = st.regs.get(mm[1])
                if bv and bv[0] == "this":
                    v = ("this", bv[1] + mm[3])
            self._set(st, ops[0], v)
            return
        if m in ("add", "sub") and ops[0] in REG32:
            a, b = st.regs.get(ops[0]), self._val(st, ops[1])
            sign = 1 if m == "add" else -1
            v = None
            # arithmetic on plain constants is dropped (a loop counter would never converge)
            if a and b and b[0] == "c" and a[0] in ("this", "pcv") and isinstance(a[-1], int):
                v = (a[0], a[1] + sign * _signed(b[1]))
            elif m == "add" and a and b and a[0] == "c" and b[0] in ("this", "pcv"):
                v = (b[0], b[1] + _signed(a[1]))
            st.regs[ops[0]] = v
            return
        if m in ("inc", "dec") and ops[0] in REG32:
            a = st.regs.get(ops[0])
            st.regs[ops[0]] = (a[0], a[1] + (1 if m == "inc" else -1)) if a and a[0] in ("this", "pcv") \
                and isinstance(a[1], int) else None
            return
        if m == "xor" and len(ops) == 2 and ops[0] == ops[1] and ops[0] in REG32:
            st.regs[ops[0]] = ("c", 0)
            return
        if m == "xchg":
            a, b = self._val(st, ops[0]), self._val(st, ops[1])
            self._set(st, ops[0], b)
            self._set(st, ops[1], a)
            return
        # implicit register writes
        if m in ("cdq", "cwd"):
            st.regs["edx"] = None
        elif m in ("fnstsw", "fstsw", "lahf", "cwde", "cbw", "cpuid", "rdtsc"):
            st.regs["eax"] = None
            if m in ("cpuid", "rdtsc"):
                st.regs["edx"] = None
        elif m in ("div", "idiv", "mul") or (m == "imul" and len(ops) == 1):
            st.regs["eax"] = st.regs["edx"] = None
        elif m.split()[0] in STRING_OPS or m.startswith("rep"):
            if st.regs.get("edi") and st.regs["edi"][0] == "this" and ("stos" in m or "movs" in m):
                raise UnmodelledPcWrite("%#x: string store through `this`" % ins.addr)
            for r in ("esi", "edi", "ecx", "eax"):
                st.regs[r] = None
        if not ops or m in READ_ONLY:
            return
        if m.startswith("f") and m not in FPU_STORES:
            return
        self._set(st, ops[0], None)


def _signed(v: int) -> int:
    return v - (1 << 32) if v & 0x80000000 else v


# ------------------------------------------------------------------------------------------ the exe-backed pieces

class ExeStrings:
    def __init__(self, img):
        self.img = img

    def u32(self, va: int) -> int:
        return self.img.u32(va)

    def get(self, va: int):
        if not self.img.has(va, 4):
            return None
        s = self.img.cstr(va, 160)
        if len(s) >= 4 and all(32 <= ord(c) < 127 for c in s):
            return s
        return None


def find_message(dis: Disasm, strings, addr: int, kind: str = "Error") -> str:
    """The format string of an Error / SrOSIError (the nearest `push imm32` of a C string before the call), or for
    SrThrow the error object (its first pushed argument, the nearest `push imm32`)."""
    for ins in dis.prev(addr, 24):
        if ins.mnem == "push" and ins.ops and _imm(ins.ops[0]) is not None:
            v = _imm(ins.ops[0]) & 0xFFFFFFFF
            if kind == "SrThrow":
                return "SrThrow(%#x)" % v
            s = strings.get(v)
            if s:
                return s
    return ""


def callee_summary(dis: Disasm, strings=None, limit: int = 4000):
    """callee_throws(addr) -> [reason, ...]: the throwers reached from addr through direct calls only (recursive
    descent over each function's branches). An unresolved indirect call is not followed."""
    memo: dict = {}

    def scan(fn: int) -> set:
        out, seen, work = set(), set(), [fn]
        while work and len(seen) < limit:
            a = work.pop()
            if a in seen:
                continue
            seen.add(a)
            try:
                ins = dis.at(a)
            except KeyError:
                continue
            if ins.mnem == "ret":
                continue
            if ins.mnem == "jmp":
                t = _imm(ins.ops[0]) if ins.ops else None
                if t is not None:
                    work.append(t)
                continue
            if JCC.match(ins.mnem):
                work.append(_imm(ins.ops[0]))
            if ins.mnem == "call":
                t = _imm(ins.ops[0])
                if t is not None:
                    if t in THROWERS:
                        out.add((fn, ins.addr, t))
                    else:
                        out.add(("call", t))
            work.append(ins.next)
        return out

    def throws(fn: int, stack=()) -> list:
        """[(throw call address, thrower, message, chain of functions)], one per distinct throw call."""
        if fn in memo:
            return memo[fn]
        if fn in stack:
            return []
        found = {}
        for item in sorted(scan(fn), key=str):
            if item[0] == "call":
                for at, t, msg, chain in throws(item[1], stack + (fn,)):
                    found.setdefault(at, (at, t, msg, (fn,) + chain))
            else:
                f, at, t = item
                msg = find_message(dis, strings, at, THROWERS[t]) if strings is not None else ""
                found.setdefault(at, (at, THROWERS[t], msg, (fn,)))
        memo[fn] = sorted(found.values())
        return memo[fn]

    return lambda fn: throws(fn)


@dataclass
class Result:
    handlers: dict            # handler addr -> HandlerResult
    by_op: dict               # op -> HandlerResult
    pc_writes: dict = field(default_factory=dict)   # instruction address -> write form


def analyze(exe: Path) -> Result:
    opcodes = _load("osi_opcodes", REPO / "tools" / "osi" / "opcodes.py")
    excmod = _load("osi_exc", REPO / "tools" / "osi" / "exc.py")
    data = Path(exe).read_bytes()
    table = opcodes.handler_table(data)
    img = excmod.Image.from_pe(data)
    dis = Disasm.from_exe(Path(exe))
    vtable = {}
    for off in range(0, VTABLE_SIZE, 4):
        vtable[off] = img.u32(VTABLE + off)
    vtable[ERROR_SLOT] = "Error"
    index = excmod.load_index(img)

    class ExcSites(dict):
        def get(self, fn, default=None):
            if fn not in self:
                d = excmod.decode_function(img, fn, index)
                sites = {}
                for s in d.get("sites", []):
                    for rec in s["records"]:
                        if rec.get("kind") == 0x10 and rec.get("type") in ("!ScCatchMessage!", ""):
                            sites[s["call"]] = rec["pad"]
                            break
                self[fn] = sites
            return dict.get(self, fn, default)

    strings = ExeStrings(img)
    an = Analyzer(dis, vtable, strings, ExcSites(), callee_summary(dis, strings), VM_NONVIRTUAL, INDIRECT)
    handlers, by_op = {}, {}
    for t in table:
        h = t["handler"]
        if h not in handlers:
            handlers[h] = an.handler(h)
        handlers[h].ops.append(t["op"])
        by_op[t["op"]] = handlers[h]
    return Result(handlers, by_op, dict(an.pc_writes))


# ------------------------------------------------------------------------------------------------------- output

def _ops_text(ops: list) -> str:
    if len(ops) > 4:
        return "%d ops (%s, ...)" % (len(ops), ", ".join("0x%02x" % o for o in ops[:3]))
    return ", ".join("0x%02x" % o for o in ops)


def origin_text(s: Raise) -> str:
    """'+6' on direct paths only; '+6 (after caught 0x6017ac, ...)' for a raise reached through a landing pad."""
    parts = []
    for pc in sorted(s.by_origin):
        o = s.by_origin[pc]
        txt = pc
        caught = sorted(x for x in o if x is not None)
        if caught:
            txt += " (after caught %s%s)" % (", ".join("%#x" % x for x in caught),
                                            "; also a direct path" if None in o else "")
        parts.append(txt)
    return " / ".join(parts)


def lines(res: Result) -> list:
    out = []
    for h in sorted(res.handlers.values(), key=lambda h: h.ops[0]):
        for s in h.sites:
            via = " via " + ">".join("%#x" % v for v in s.via) if s.via else ""
            out.append("%s handler %#x site %#x%s kind %s pc %s%s msg %r" % (
                _ops_text(h.ops), h.addr, s.raise_addr, via, s.kind, origin_text(s),
                " caught %#x" % s.caught if s.caught else "", s.message))
        out.append("%s handler %#x exits %s%s stack %s" % (
            _ops_text(h.ops), h.addr, "/".join(sorted(h.exits)) or "none",
            " (after a catch: %s)" % "/".join(sorted(h.caught_exits)) if h.caught_exits else "",
            " ".join("%s %s" % (k, "/".join(sorted(v))) for k, v in h.stack_ops.items()) or "none"))
    n_sites = sum(len(h.sites) for h in res.handlers.values())
    forms = {}
    for f in res.pc_writes.values():
        forms[f] = forms.get(f, 0) + 1
    out.append("pc writes %s" % " ".join("%s=%d" % (k.replace(" ", "_"), v) for k, v in sorted(forms.items())))
    out.append("handlers %d sites %d" % (len(res.handlers), n_sites))
    return out


# ------------------------------------------------------------------------------------------------ the doc tables

BEGIN = "<!-- BEGIN error-pc (generated: tools/osi/error_pc.py --markdown) -->"
END = "<!-- END error-pc -->"
TABLE_KINDS = ("Error", "SrOSIError", "SrThrow", "___throw", "callee", "native", "accessor")


def _ops_cell(ops: list) -> str:
    if len(ops) > 4:
        return "%d unused ops (0x%02x, 0x%02x, …, 0x%02x)" % (len(ops), ops[0], ops[1], ops[-1])
    return ", ".join("0x%02x" % o for o in ops)


def _label_text(x: str) -> str:
    m = re.match(r"set\((0x[0-9a-f]+)\)$", x)
    if m:
        return "jump target (mov at %s)" % m.group(1)
    m = re.match(r"(\+\d+)\+operand\((0x[0-9a-f]+)\)$", x)
    if m:
        return "%s + s16 operand (add at %s)" % m.groups()
    return x


def _labels(labels) -> str:
    def key(x):
        m = re.match(r"\+(\d+)$", x)
        return (0, int(m.group(1))) if m else (1, x)
    return " / ".join(_label_text(x) for x in sorted(labels, key=key)) or "—"


def _direct(r: Raise) -> set:
    return {pc for pc, o in r.by_origin.items() if None in o}


def _caught_note(h: HandlerResult, r: Raise) -> str:
    if not r.caught:
        return ""
    then = sorted(x.raise_addr for x in h.sites if x is not r and x.kind in ("Error", "___throw")
                  and any(r.raise_addr in o for o in x.by_origin.values()))
    if not then:   # caught on a path that already went through a pad: the handler's escaping post-catch raise
        then = sorted(x.raise_addr for x in h.sites if x.kind == "Error" and not x.caught
                      and None not in set().union(*x.by_origin.values()))
    return "; caught (pad %#x)%s" % (r.caught, ", then raised again at %s with this PC" %
                                     " → ".join("%#x" % t for t in then) if then else "")


def _capacity_only(r: Raise) -> bool:
    real = [t for t in r.throws if not is_alloc_throw(t)]
    return r.kind == "callee" and bool(real) and all("0x83913c" in t[2] for t in real)


def _callee_text(r: Raise) -> str:
    real = [t for t in r.throws if not is_alloc_throw(t)]
    parts = []
    for at, thrower, m, chain in real:
        obj = re.match(r"SrThrow\((0x[0-9a-f]+)\)", m)
        if obj:
            parts.append("%s (non-OSI ScError, SrThrow at %#x)" % (ERROR_OBJECTS.get(int(obj.group(1), 16),
                                                                                  obj.group(1)), at))
        else:
            parts.append('"%s" (%s at %#x)' % (m, thrower, at))
    return "%s → %s" % (r.message.partition(": ")[0], "; ".join(parts))


def _row_message(h: HandlerResult, r: Raise) -> str:
    if r.kind in ("Error", "SrOSIError"):
        msg = '"%s" (%s)' % (r.message.replace("|", "\\|"), r.kind)
    elif r.kind == "callee":
        msg = _callee_text(r)
    elif r.kind == "___throw":
        msg = "re-throw of the native's error from the inner catch (pad 0x601b10)"
    else:
        msg = r.message
    return msg + _caught_note(h, r)


def _row_pc(h: HandlerResult, r: Raise, raises_by_addr: dict) -> str:
    if r.kind == "Error" and (r.message.startswith("Script is calling") or "directly" in r.message):
        real, alloc = set(), set()
        for pc, origins in r.by_origin.items():
            for o in origins:
                src = raises_by_addr.get(o)
                (alloc if src is not None and src.kind == "alloc" else real).add(pc)
        txt = "%s: the PC of the caught error" % _labels(real)
        only_alloc = alloc - real
        if only_alloc:
            txt += " (%s only after an allocation failure)" % _labels(only_alloc)
        return txt + "; after a native whose nested engine call raised, that inner error's PC (D16; INFERRED)"
    txt = _labels(_direct(r) or r.pcs)
    if r.kind == "native":
        txt += "; INFERRED: when the native's nested engine call raised, that inner error's PC (D16)"
    if _capacity_only(r):
        txt += "; INFERRED reachable only with a string the 255-byte buffer cannot hold"
    if r.kind == "accessor":
        txt += "; dead code (\"The game-variable map\")"
    return txt


def _site_rows(h: HandlerResult) -> list:
    """(error site cell, message cell, PC cell) per row; kOutOfRange string-capacity throws reached through one
    handler-level call are merged into one row."""
    raises_by_addr = {r.raise_addr: r for r in h.sites}
    rows, groups = [], {}
    for r in h.sites:
        if r.kind not in TABLE_KINDS:
            continue
        if _capacity_only(r):
            top = r.via[0] if r.via else r.raise_addr
            k = (top, frozenset(_direct(r) or r.pcs), r.caught)
            if k not in groups:
                groups[k] = []
                rows.append(("group", k))
            groups[k].append(r)
            continue
        where = "%#x" % r.raise_addr if not r.via else "%#x (in an inlined VM method, called at %s)" % (
            r.raise_addr, " < ".join("%#x" % v for v in reversed(r.via)))
        rows.append(("row", (where, _row_message(h, r), _row_pc(h, r, raises_by_addr))))
    out = []
    for kind, v in rows:
        if kind == "row":
            out.append(v)
            continue
        members = groups[v]
        first = members[0]
        throws = sorted({t[0] for m in members for t in m.throws if not is_alloc_throw(t)})
        inner = sorted({m.raise_addr for m in members})
        if first.via:
            where = "call at %#x (%s); inner calls %s" % (first.via[0], NAMES.get(_target_of(h, first), "VM method"),
                                                        ", ".join("%#x" % a for a in inner))
        else:
            where = "%#x (%s)" % (first.raise_addr, first.message.partition(": ")[0])
        msg = "kOutOfRange (non-OSI ScError): SrThrow at %s" % ", ".join("%#x" % t for t in throws)
        msg += _caught_note(h, first)
        out.append((where, msg, _row_pc(h, first, raises_by_addr)))
    return out


def _target_of(h: HandlerResult, r: Raise):
    return h.call_targets.get(r.via[0]) if r.via else None


def markdown(res: Result) -> str:
    out = [BEGIN, ""]
    forms = {}
    for f in res.pc_writes.values():
        forms[f] = forms.get(f, 0) + 1
    n_alloc = n_cleanup = 0
    per_site = []
    summary = ["| op | handler | PC writes (instruction → PC after it) | PC at its errors | PC at its ScOSIStack calls "
               "| normal exit | allocation-failure sites |", "|---|---|---|---|---|---|---|"]
    for h in sorted(res.handlers.values(), key=lambda h: h.ops[0]):
        n_alloc += sum(1 for r in h.sites if r.kind == "alloc")
        n_cleanup += sum(1 for r in h.sites if r.kind == "cleanup")
        writes = ", ".join("%#x → %s" % (a, _labels(v)) for a, v in sorted(h.writes.items())) or "none"
        errs = set()
        for r in h.sites:
            if r.kind in TABLE_KINDS:
                errs |= _direct(r)
        stack = "; ".join("%s %s" % (k, _labels(v)) for k, v in sorted(h.stack_ops.items())) or "—"
        summary.append("| %s | %#x | %s | %s | %s | %s | %d |" % (
            _ops_cell(h.ops), h.addr, writes, _labels(errs) if errs else "no error", stack,
            _labels(h.exits) if h.exits else "never returns", sum(1 for r in h.sites if r.kind == "alloc")))
        for where, msg, pc in _site_rows(h):
            per_site.append("| %s | %#x | %s | %s | %s |" % (_ops_cell(h.ops), h.addr, where, msg, pc))
    total = sum(len(h.sites) for h in res.handlers.values())
    out.append("Generated by `tools/osi/error_pc.py --markdown` from the canonical exe: %d handlers, %d raise sites "
               "(%d allocation failures and %d landing-pad cleanup calls are counted, not listed), %d rows below. "
               "PC writes reached from the handlers: %s." % (
                   len(res.handlers), total, n_alloc, n_cleanup, len(per_site),
                   ", ".join("%s %d" % (k, v) for k, v in sorted(forms.items()))))
    out.append("")
    out += summary
    out.append("")
    out.append("| opcode | handler addr | error site addr | message | PC at throw |")
    out.append("|---|---|---|---|---|")
    out += per_site
    out.append("")
    out.append(END)
    return "\n".join(out) + "\n"


def replace_block(doc: str, block: str) -> str:
    if BEGIN not in doc or END not in doc:
        raise ValueError("the document has no error-pc markers")
    a = doc.index(BEGIN)
    b = doc.index(END) + len(END) + 1
    return doc[:a] + block + doc[b:]


def default_exe() -> Path:
    d = os.environ.get("LOMN_DATA")
    if not d:
        raise SystemExit("error_pc.py: pass --exe or set LOMN_DATA")
    return Path(d) / "reference" / "LEGOBionicle.exe"


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--exe", type=Path)
    ap.add_argument("--markdown", action="store_true", help="print the generated block for docs/osi/opcodes.md")
    ap.add_argument("--write", type=Path, help="replace the generated block in this document")
    ap.add_argument("--check", type=Path, help="exit 1 if this document's generated block is stale")
    a = ap.parse_args(argv)
    res = analyze(a.exe or default_exe())
    if a.markdown:
        sys.stdout.write(markdown(res))
    elif a.write or a.check:
        path = a.write or a.check
        doc = path.read_text()
        new = replace_block(doc, markdown(res))
        if a.check:
            if new != doc:
                print("error_pc.py: %s is stale (run --write)" % path)
                return 1
            return 0
        path.write_text(new)
    else:
        print("\n".join(lines(res)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
