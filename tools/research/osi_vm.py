"""M2a-1 T2: the Python oracle of ScOSIVirtualMachine (the LOMN script VM), written from docs/osi/opcodes.md,
docs/osi/types.md, docs/osi/trace.md, docs/osi/conformance.md and the cited asm of the canonical exe (read as data with
objdump). It is NEVER written from engine/ (spec §5: two independent interpreters).

Standard library only (struct, fractions, json, hashlib) plus the two T0 helpers it is allowed to import:
`tools/osi/expect_calc.round_p` / `f32_bits` (exact rational -> p-bit -> f32 rounding, D8') and `tools/osi/osiasm`
(the conformance assembler, used by --conform and the unit tests).

Readings settled from the asm where a table cell was unclear (each one is listed in the T2 hand-back):
- 0x02/0x03 advance PC by 1 before raising (0x6038a6); an E line still prints the instruction's own address.
- ConvertVariant on a stack slot (0x82/0x83/0x84): the 0x11 -> int/float in-place dynamic string is then disposed by
  PushInto (0x601edc) before the converted value is stored (0x601ef2): the slot ends as the converted value, not 0x16.
- 0x60: 0x11 operands are converted to 0x16 unconditionally (0x605350-0x6053a9), so `1 = "1"` reports (type:16).
- Globals and native keys are ScOSISystem::GetToken hashes (0x5faae0: h = h*0x83 + upper(c), a-z folded), FindFunction
  is __strnicmp (A-Z folded, entry-length prefix), FindMemberID (0x609ba0) lower-cases the query and strcmp's the raw
  symbol text, with a per-script 50-entry cache whose miss entry is {last symbol text, 0xffff}.
- Every handler pops its operands before its type checks except where the row says otherwise (0x10/0x1a pop v after
  the object checks, 0x15 checks before popping); multi-operand disposes follow the asm order (a then b; o then s).

CLI:
  osi_vm.py --conform DIR [--trace-dir D]     every <id>.asm/<id>.expect.json under DIR; "conformance: N/N pass, P pending"
                                              (a program with "pending": "<task>" prints PENDING and is not run)
  osi_vm.py --run BASE.osi --natives N --mods M --types T --frames 600 --keys KEYSPEC [--replay natives.log]
            [--trace out.trace] [--digest out.digest] [--log-out natives.log] [--real-natives [--install DIR]]
            [--overrides native_overrides.json]

M2a-2 T4 (the native framework, docs/osi/natives.md): game objects with engine references (D13; Heap.new_game /
add_ref / release, the free hook, the handle wrap guard), natives.log v2 with its replay rules (D14: G H C X K records,
one cursor, G checked, H applied, C performed, X decides a swallow; v1 logs still replay), the swallowed nested error
(D16: Vm.swallow_error, reset_after_error to the outermost call's entry), the nesting high-water (D15: max_nesting), the
255-byte name bound of 0x18/0x19/0x1a/0x83 (natives.md §8.3; string-table entries are read as C strings at the entry,
as the original's pointer table does), the error PCs of the opcodes.md table (rule 15) and the runner keys of group n
(pc_after, then, swallowed, expect.top, returns.engine_refs/object, calls.depth, swallow). The host side natives use is
tools/research/osi_natives/framework.py.

M2a-2 T5 adds one hook, `Vm.wrap_handlers` (the A10 taint pass, tools/research/osi_taint.py); unused, it changes nothing.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import struct
import sys
from fractions import Fraction
from pathlib import Path
from typing import NamedTuple, Optional

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "osi"))
if str(Path(__file__).resolve().parent) not in sys.path:   # the osi_natives package sits next to this file
    sys.path.insert(0, str(Path(__file__).resolve().parent))
if __name__ == "__main__":                 # one module object: osi_natives.framework imports `osi_vm`
    sys.modules.setdefault("osi_vm", sys.modules[__name__])
from expect_calc import f32_bits as _f32_bits_fraction, round_p  # noqa: E402  (allowed helpers)

# ---------------------------------------------------------------------------------------------------------------
# Variants and constants
# ---------------------------------------------------------------------------------------------------------------

T_NONE, T_OBJECT, T_RET, T_ARRAY, T_NOTHING, T_INT = 0x00, 0x01, 0x02, 0x05, 0x0F, 0x10
T_SID, T_FLOAT, T_STRTAB, T_COL16, T_COLOUR, T_STRING, T_FRAME, T_BYREF = 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0xFF, 0x200
NAN_BITS = 0xFFC00000          # D9
STACK_SLOTS = 1000             # VM ctor 0x60b257 (push 0x3e8)
M32 = 0xFFFFFFFF


class Variant(NamedTuple):
    type: int
    value: int


NOTHING = Variant(T_NOTHING, 0)


def is_game(t: int) -> bool:
    return 0x20 <= t <= 0xFE


def s32(v: int) -> int:
    v &= M32
    return v - (1 << 32) if v & 0x80000000 else v


def bits_to_f(b: int) -> float:
    return struct.unpack("<f", struct.pack("<I", b & M32))[0]


def f32(x: float) -> float:
    """x (a double) rounded once to f32."""
    return bits_to_f(f32_bits(x))


def f32_bits(x: float) -> int:
    """The binary32 pattern of the double x rounded once (ties to even, overflow to inf, NaN -> D9)."""
    if math.isnan(x):
        return NAN_BITS
    if math.isinf(x):
        return 0x7F800000 if x > 0 else 0xFF800000
    if x == 0:
        return 0x80000000 if math.copysign(1.0, x) < 0 else 0
    return _f32_bits_fraction(Fraction(x))


def exact_round_f32(fr: Fraction, pc: int = 24) -> int:
    """D8': round_f32(round_p(exact)); returns bits. Exact zero must be signed by the caller."""
    return _f32_bits_fraction(round_p(fr, pc))


def fmt_v(v: Optional[Variant]) -> str:
    if v is None:
        return "----:--------"
    return "%04x:%08x" % (v.type & 0xFFFF, v.value & M32)


def name_token(b: bytes) -> str:
    out = []
    for c in b:
        if c == 0x5C:
            out.append("\\\\")
        elif 0x21 <= c <= 0x7E:
            out.append(chr(c))
        else:
            out.append("\\x%02x" % c)
    return "".join(out)


def msg_escape(b: bytes) -> str:
    out = []
    for c in b:
        if c == 0x22:
            out.append('\\"')
        elif c == 0x5C:
            out.append("\\\\")
        elif 0x20 <= c <= 0x7E:
            out.append(chr(c))
        else:
            out.append("\\x%02x" % c)
    return "".join(out)


def get_token(text: bytes) -> int:
    """ScOSISystem::GetToken (0x5faae0): h = h*0x83 + c, a-z folded to A-Z, up to the first NUL, 32-bit."""
    h = 0
    for c in text:
        if c == 0:
            break
        if 0x61 <= c <= 0x7A:
            c -= 0x20
        h = (h * 0x83 + c) & M32
    return h


def cstr(b: bytes) -> bytes:
    i = b.find(b"\0")
    return b if i < 0 else b[:i]


def _lower_az(b: bytes) -> bytes:
    return bytes(c + 0x20 if 0x41 <= c <= 0x5A else c for c in b)


# ---------------------------------------------------------------------------------------------------------------
# Errors
# ---------------------------------------------------------------------------------------------------------------

class VmError(Exception):
    """A script error (kOSIError, or a non-OSI ScError such as kLenTooLong) with the state at the throw (D10)."""

    def __init__(self, message, pc: int = 0, op: int = 0, depth: int = 0):
        if isinstance(message, str):
            message = message.encode("latin-1")
        super().__init__(message.decode("latin-1"))
        self.message_bytes = message
        self.message = message.decode("latin-1")
        self.pc, self.op, self.depth = pc, op, depth
        self.top: Optional[Variant] = None


class UndefinedBehaviour(Exception):
    """Raised by table helpers outside the VM; the VM re-raises it as a VmError at the current instruction."""


class ReplayDesync(Exception):
    """A replay-log mismatch (trace.md §6). Not an OSI error: it is never caught by 0x82's catch(...)."""


# ---------------------------------------------------------------------------------------------------------------
# Trace sinks
# ---------------------------------------------------------------------------------------------------------------

class TraceSink:
    def line(self, s: str) -> None:  # pragma: no cover - interface
        raise NotImplementedError

    def close(self) -> None:
        pass


class ListTrace(TraceSink):
    def __init__(self):
        self.lines: list[str] = []

    def line(self, s: str) -> None:
        self.lines.append(s)


class FileTrace(TraceSink):
    def __init__(self, path):
        self.f = open(path, "w", encoding="ascii", newline="\n")

    def line(self, s: str) -> None:
        self.f.write(s + "\n")

    def close(self) -> None:
        self.f.close()


FNV_OFF, FNV_PRIME = 0xCBF29CE484222325, 0x100000001B3


def fnv1a64(data: bytes, h: int = FNV_OFF) -> int:
    for c in data:
        h = ((h ^ c) * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return h


class DigestTrace(TraceSink):
    """trace.md §2 digest mode: the header verbatim, one `D` line per `K` block, then `T`. Lines before the first `K`
    (Setup) form a block numbered -1 (our convention; trace.md does not name that block)."""

    def __init__(self, path):
        self.f = open(path, "w", encoding="ascii", newline="\n")
        self.total, self.whole = 0, FNV_OFF
        self.block_frame, self.block_lines, self.block_h = None, 0, FNV_OFF
        self.header_done = False

    def _flush(self):
        if self.block_frame is not None and self.block_lines:
            self.f.write("D %6d %8d %016x\n" % (self.block_frame, self.block_lines, self.block_h))
        self.block_lines, self.block_h = 0, FNV_OFF

    def line(self, s: str) -> None:
        data = (s + "\n").encode("ascii")
        self.total += 1
        self.whole = fnv1a64(data, self.whole)
        if not self.header_done and s.startswith("# "):
            self.header_done = True
            self.f.write(s + "\n")
            self.block_frame = -1
            return
        if s.startswith("K "):
            self._flush()
            self.block_frame = int(s.split()[1])
        self.block_lines += 1
        self.block_h = fnv1a64(data, self.block_h)

    def close(self) -> None:
        self._flush()
        self.f.write("T %d %016x\n" % (self.total, self.whole))
        self.f.close()


# ---------------------------------------------------------------------------------------------------------------
# Heap (D7: per-kind monotonic handles from 1, never reused; explicit refcounts; A/F trace events)
# ---------------------------------------------------------------------------------------------------------------

HANDLE_LIMIT = 0xFFFFFFFF      # D7: a per-kind u32 counter; reaching it is an error, never a wrap (parked N4)


class Heap:
    KINDS = ("o", "a", "s", "g")

    def __init__(self, trace: Optional[TraceSink] = None):
        self.trace = trace
        self.objects: dict[int, list] = {}   # id -> [refcount, class_id, props(list[Variant]), class_syms]
        self.arrays: dict[int, list] = {}    # id -> [refcount, elements(list[Variant])]
        self.strings: dict[int, bytearray] = {}
        # id -> [total, minting type, engine]: total = engine references + variant references (D13, natives.md §2.4)
        self.games: dict[int, list] = {}
        self.next = {k: 1 for k in self.KINDS}
        self.allocs = {k: 0 for k in self.KINDS}
        self.free_hook = None                # called with the handle after a game object's `F g` (the host delete)

    def _t(self, s):
        if self.trace is not None:
            self.trace.line(s)

    def _new(self, kind):
        h = self.next[kind]
        if h >= HANDLE_LIMIT:
            raise VmError("Undefined: handle space exhausted")
        self.next[kind] += 1
        self.allocs[kind] += 1
        return h

    def live(self) -> dict:
        return {"o": len(self.objects), "a": len(self.arrays), "s": len(self.strings), "g": len(self.games)}

    # --- allocation
    def new_object(self, class_id: int, nprops: int, class_syms=()) -> int:
        h = self._new("o")
        self.objects[h] = [1, class_id, [NOTHING] * nprops, list(class_syms)]
        self._t("A o %08x %04x" % (h, class_id & 0xFFFF))
        return h

    def new_array(self, n: int = 0) -> int:
        h = self._new("a")
        self.arrays[h] = [1, [NOTHING] * n]
        self._t("A a %08x %08x" % (h, n))
        return h

    def new_string(self, text: bytes) -> int:
        h = self._new("s")
        self.strings[h] = bytearray(text)
        self._t("A s %08x %3d" % (h, len(text)))
        return h

    def new_game(self, type_id: int, engine_refs: int = 0) -> int:
        """Mint a game handle (A g) for an object the engine holds `engine_refs` times, returned in one variant:
        total = engine_refs + 1 (D13). engine_refs 0 is M2a-1's new_game(type)."""
        h = self._new("g")
        self.games[h] = [engine_refs + 1, type_id, engine_refs]
        self._t("A g %08x %02x" % (h, type_id))
        return h

    # --- engine-side references on a game object (D13; natives.md §1.2 Reference counts, §3.1 H)
    def _game(self, h: int) -> list:
        try:
            return self.games[h]
        except KeyError:
            raise VmError("Undefined behaviour: game handle %08x is not live" % h) from None

    def engine(self, h: int) -> int:
        return self._game(h)[2]

    def game_type(self, h: int) -> int:
        """The game type under which the handle was minted (its A g line)."""
        return self._game(h)[1]

    def add_ref(self, h: int) -> None:
        r = self._game(h)
        r[0] += 1
        r[2] += 1

    def release(self, h: int) -> None:
        r = self._game(h)
        r[0] -= 1
        r[2] -= 1
        if r[0] <= 0:
            self._free_game(h)

    def _free_game(self, h: int) -> None:
        del self.games[h]
        self._t("F g %08x" % h)                       # Dispose 0x60a94c dec, Free 0x60a957 -> the deleting dtor
        if self.free_hook is not None:
            self.free_hook(h)

    def text(self, h: int) -> bytes:
        try:
            return bytes(self.strings[h])
        except KeyError:
            raise VmError("Undefined behaviour: string handle %d is not live" % h) from None

    # --- Duplicate (0x60aa20) / Dispose (0x60a930)
    def duplicate(self, v: Variant) -> Variant:
        t = v.type
        if t == T_OBJECT:
            self._rec(self.objects, v)[0] += 1
        elif t == T_ARRAY:
            self._rec(self.arrays, v)[0] += 1
        elif is_game(t):
            self._rec(self.games, v)[0] += 1
        elif t == T_STRING:
            return Variant(T_STRING, self.new_string(self.text(v.value)))
        return v

    @staticmethod
    def _rec(table, v):
        try:
            return table[v.value]
        except KeyError:
            raise VmError("Undefined behaviour: handle %04x:%08x is not live" % (v.type, v.value)) from None

    def dispose(self, v: Variant) -> None:
        t = v.type
        if t == T_OBJECT:
            r = self._rec(self.objects, v)
            r[0] -= 1
            if r[0] <= 0:
                del self.objects[v.value]
                self._t("F o %08x" % v.value)
                props = r[2]
                for i in range(len(props) - 1, -1, -1):      # dtor 0x6085cd-0x6085e3: back to front
                    self.dispose(props[i])
        elif t == T_ARRAY:
            r = self._rec(self.arrays, v)
            r[0] -= 1
            if r[0] <= 0:
                del self.arrays[v.value]
                self._t("F a %08x" % v.value)
                el = r[1]
                while el:                                     # dtor 0x5fc7a0-0x5fc7bd: Dispose(back), pop_back
                    self.dispose(el[-1])
                    el.pop()
        elif is_game(t):
            r = self._rec(self.games, v)
            r[0] -= 1
            if r[0] <= 0:
                self._free_game(v.value)
        elif t == T_STRING:
            if v.value not in self.strings:
                raise VmError("Undefined behaviour: string handle %d is not live" % v.value)
            del self.strings[v.value]
            self._t("F s %08x" % v.value)


# ---------------------------------------------------------------------------------------------------------------
# Types and natives
# ---------------------------------------------------------------------------------------------------------------

BUILTIN_TYPE_NAMES = {"nothing": 0x0F, "int": 0x10, "string-id": 0x11, "float": 0x12, "colour16": 0x14,
                      "colour": 0x15, "string": 0x16, "byref": 0x200, "unknown:0x0": 0x00}


class TypeTable:
    def __init__(self, by_name: dict, by_id: dict):
        self.by_name, self.by_id = by_name, by_id

    @classmethod
    def load(cls, path) -> "TypeTable":
        d = json.loads(Path(path).read_text())
        by_name, by_id = dict(BUILTIN_TYPE_NAMES), {}
        for t in d["types"]:
            by_name[t["name"]] = t["id"]
            if t["name"].startswith("type:"):
                by_name[t["name"][5:]] = t["id"]
            by_id.setdefault(t["id"], t["name"])
        return cls(by_name, by_id)

    def id(self, name) -> int:
        if name is None:
            return T_NOTHING
        if name in self.by_name:
            return self.by_name[name]
        if name.startswith("type:") and name[5:] in self.by_name:
            return self.by_name[name[5:]]
        raise KeyError("unknown type name %r" % name)

    def name(self, type_id: int) -> str:
        return self.by_id.get(type_id, "0x%x" % type_id)


class NativeEntry(NamedTuple):
    cls: str
    name: str
    key: str
    min: int
    max: int
    types: tuple
    ret: str
    ret_type: int
    default: Optional[Variant]
    fn: object
    tier: str
    sets_fpu_pc64: bool = False


class NativeCall:
    """What a native sees: the VM, its registry row, the call-site pc and exactly argc stack-slot indices (deepest
    first). A by-reference (0x200) parameter is the slot itself (0x60198c-0x601abc: the native gets the slot address)."""

    def __init__(self, vm, entry, site_pc, slots, seq: int = -1):
        self.vm, self.entry, self.site_pc, self.slots = vm, entry, site_pc, slots
        self.seq = seq                                  # the native sequence number, assigned at entry (natives.md §1.2)
        self.writes: list[tuple[int, Variant]] = []

    def arg(self, i: int) -> Variant:
        return self.vm.stack[self.slots[i]]

    def write(self, i: int, v: Variant) -> None:
        """Store into the argument's slot (by-reference write; the later Pop(true) disposes it)."""
        self.vm.stack[self.slots[i]] = v
        self.writes.append((i, v))


class NativeRegistry:
    def __init__(self, types: TypeTable):
        self.types = types
        self.entries: dict[tuple[int, int], NativeEntry] = {}
        self.replay: Optional[list] = None
        self.replay_pos = 0                       # the one cursor of the replay (natives.md §3.3)
        self.replay_v2 = False
        self.return_log: Optional[list] = None   # when a list: natives.log lines are appended (trace.md §6)

    @staticmethod
    def _key(cls_name, fn_name) -> tuple[int, int]:
        c = cls_name.encode("latin-1") if isinstance(cls_name, str) else cls_name
        f = fn_name.encode("latin-1") if isinstance(fn_name, str) else fn_name
        return get_token(c), get_token(f)

    @classmethod
    def load(cls, natives_json, mods_json, types: TypeTable) -> "NativeRegistry":
        reg = cls(types)
        for path in (natives_json, mods_json):
            if path is None:
                continue
            for row in json.loads(Path(path).read_text()):
                if not row.get("winner", True):
                    continue
                reg.add(reg.entry_from_row(row))
        return reg

    def entry_from_row(self, row, fn=None) -> NativeEntry:
        ret = row.get("ret") or "none"
        ret_type = 0
        if ret == "game":
            ret_type = self.types.id(row["ret_class"])
        default = row.get("default")
        dv = parse_variant_spec(default) if isinstance(default, dict) else None
        tys = tuple(self.types.id(t) for t in row["types"])
        tier = row.get("tier", "stub")
        return NativeEntry(row["cls"], row["name"], row.get("key", (row["cls"] + "." + row["name"]).lower()),
                           row["min"], row["max"], tys, ret, ret_type, dv, fn, tier,
                           bool(row.get("sets_fpu_pc64", False)))

    def find(self, cls_name, fn_name) -> Optional[NativeEntry]:
        return self.entries.get(self._key(cls_name, fn_name))

    def add(self, entry: NativeEntry) -> None:
        k = self._key(entry.cls, entry.name)
        if k not in self.entries:          # insert_one 0x5fb290: unique-key insert, the first registration wins
            self.entries[k] = entry

    def bind(self, key: str, fn) -> None:
        c, f = key.split(".", 1)
        k = self._key(c, f)
        if k not in self.entries:
            raise LookupError("unknown native key %r" % key)
        self.entries[k] = self.entries[k]._replace(fn=fn, tier="implemented")

    # --- D11 typed stubs
    def stub_return(self, entry: NativeEntry, heap: Heap, call: NativeCall) -> Variant:
        if entry.default is not None:
            return entry.default
        r = entry.ret
        if r == "int":
            return Variant(T_INT, 0)
        if r == "float":
            return Variant(T_FLOAT, 0)
        if r == "string":
            return Variant(T_STRING, heap.new_string(b""))
        if r == "colour":
            return Variant(T_COLOUR, 0)
        if r == "game":
            return Variant(entry.ret_type, heap.new_game(entry.ret_type))
        if r == "copy":
            return heap.duplicate(call.arg(0)) if call.slots else NOTHING
        return NOTHING

    # --- the replay log (trace.md §6; v2 = natives.md §3, D14)
    def replay_from(self, log_path) -> None:
        recs = []
        for ln in Path(log_path).read_text(encoding="ascii").splitlines():
            if ln.strip():
                recs.append(parse_log_record(ln))
        self.replay, self.replay_pos = recs, 0
        # a v1 log has native lines only and no G (natives.md §3.3: it still replays; a missing G is 0, nothing checked)
        self.replay_v2 = any(r["kind"] != "n" or r["ret_g"] is not None or any(w[3] is not None for w in r["writes"])
                             for r in recs)

    @property
    def replay_seq(self) -> int:                    # M2a-1 name, kept for the probes
        return self.replay_pos

    def invoke(self, call: NativeCall) -> Variant:
        vm, entry = call.vm, call.entry
        if self.replay is not None:
            return self._replay_one(call)
        if entry.fn is not None:
            r = entry.fn(call)
        else:
            r = self.stub_return(entry, vm.heap, call)
        return r if r is not None else NOTHING

    def replay_finish(self) -> None:
        """At the end of a replay run the whole log must have been consumed: trailing records (a final H, a native or
        a K the replay never reached) are a desync."""
        if self.replay is not None and self.replay_pos < len(self.replay):
            rest = len(self.replay) - self.replay_pos
            raise ReplayDesync("replay desync at the end of the run: %d records not consumed, first %s"
                               % (rest, self.replay[self.replay_pos]["raw"]))

    def _peek(self):
        return self.replay[self.replay_pos] if self.replay_pos < len(self.replay) else None

    def replay_frame(self, frame: int) -> None:
        """The host's frame start: a v2 log has `K %6d` here, before anything else of the frame (natives.md §3.2)."""
        if not self.replay_v2:
            return
        rec = self._peek()
        if rec is None or rec["kind"] != "K" or rec["frame"] != frame:
            raise ReplayDesync("replay desync at frame %d: expected K %6d, got %s"
                               % (frame, frame, rec["raw"] if rec else "end of log"))
        self.replay_pos += 1

    def _replay_one(self, call: NativeCall) -> Variant:
        """One cursor in the replay's own execution order (natives.md §3.3, I6). Inside replayed native N: H records are
        applied, C N calls are performed (their natives consume their own records), X N decides a swallow; then N's own
        line. An H must carry the handle's minting type and is +1 or -1; a -1 that would take the engine count below 0
        (an early free of a variant-held object) is a desync. `C -1` (a host call) met here is not N's: a desync.
        Limit: the format cannot tell a host-level H written just before N's entry from one inside N, so every H met
        inside N is applied as N's (no host H exists in M2a-2, natives.md §3.1)."""
        vm, heap = call.vm, call.vm.heap
        seq = call.seq
        got = "%s.%s/%d" % (call.cls_text.decode("latin-1"), call.fn_text.decode("latin-1"), len(call.slots))
        while True:
            rec = self._peek()
            if rec is None:
                raise ReplayDesync("replay desync at seq %d: expected end of log, got %s" % (seq, got))
            kind = rec["kind"]
            if kind == "n":
                if self.replay_v2 and rec["seq"] != seq:
                    raise ReplayDesync("replay desync at seq %d: the log has seq %d (%s/%d) here, got %s"
                                       % (seq, rec["seq"], rec["name"], rec["argc"], got))
                exp = "%s/%d" % (rec["name"], rec["argc"])
                if exp.lower() != got.lower():
                    raise ReplayDesync("replay desync at seq %d: expected %s, got %s" % (seq, exp, got))
                self.replay_pos += 1
                for i, v, text, g in rec["writes"]:
                    call.write(i, self._materialise(heap, v, text, g, seq))
                return self._materialise(heap, rec["ret"], rec["ret_text"], rec["ret_g"], seq)
            if kind == "H":
                self.replay_pos += 1
                h = rec["handle"]
                if h not in heap.games:
                    raise ReplayDesync("replay desync at seq %d: %s on a handle that is not live" % (seq, rec["raw"]))
                if rec["type"] != heap.game_type(h):
                    raise ReplayDesync("replay desync at seq %d: %s: handle %d was minted as type %04x"
                                       % (seq, rec["raw"], h, heap.game_type(h)))
                if rec["delta"] == 1:
                    heap.add_ref(h)
                elif rec["delta"] == -1:
                    if heap.engine(h) <= 0:
                        raise ReplayDesync("replay desync at seq %d: %s would take engine count %d below 0 (an early "
                                           "free)" % (seq, rec["raw"], heap.engine(h)))
                    heap.release(h)                           # 0 frees it: F g at this event
                else:
                    raise ReplayDesync("replay desync at seq %d: %s is not +1/-1" % (seq, rec["raw"]))
                continue
            if kind == "C":
                if rec["seq"] != seq:
                    raise ReplayDesync("replay desync at seq %d: %s (another native's call)" % (seq, rec["raw"]))
                self.replay_pos += 1
                args = [self._materialise(heap, v, text, g, seq) for v, text, g in rec["args"]]
                raised = None
                try:
                    vm.call(rec["name"], args)
                except VmError as e:
                    raised = e
                nxt = self._peek()
                swallowed = nxt is not None and nxt["kind"] == "X" and nxt["seq"] == seq
                if raised is not None:
                    if not swallowed:
                        raise raised                           # as in the original: out of native N
                    self.replay_pos += 1
                    vm.swallow_error(raised, None)             # D16 rules 1-2 at this point (review m-2)
                elif swallowed:
                    raise ReplayDesync("replay desync at seq %d: %s after a nested call that did not raise"
                                       % (seq, nxt["raw"]))
                continue
            raise ReplayDesync("replay desync at seq %d: expected %s, got %s" % (seq, got, rec["raw"]))

    def _materialise(self, heap: Heap, v: Variant, text, g, seq) -> Variant:
        """A logged value as a live variant. A game value (natives.md §3.3 Identity and G): a live handle gets one more
        variant reference and its engine count is CHECKED against G; otherwise it must be the heap's next handle and is
        minted with G engine references (the only place G sets a count; a v1 line has no G: 0)."""
        if v.type == T_STRING:
            return Variant(T_STRING, heap.new_string(text or b""))
        if is_game(v.type):
            if v.value in heap.games:
                if g is not None and heap.engine(v.value) != g:
                    raise ReplayDesync("G check at seq %d: handle %d, engine count %d, logged G %d"
                                       % (seq, v.value, heap.engine(v.value), g))
                return heap.duplicate(v)
            if v.value == heap.next["g"]:
                return Variant(v.type, heap.new_game(v.type, g or 0))
            raise ReplayDesync("replay desync at seq %d: handle %04x:%08x is neither the next g handle (%d) nor live"
                               % (seq, v.type, v.value, heap.next["g"]))
        if v.type in (T_OBJECT, T_ARRAY):
            table, kind = (heap.objects, "o") if v.type == T_OBJECT else (heap.arrays, "a")
            if v.value in table:
                return heap.duplicate(v)
            raise ReplayDesync("replay desync at seq %d: handle %04x:%08x is neither the next %s handle (%d) nor live"
                               % (seq, v.type, v.value, kind, heap.next[kind]))
        return v


def apply_overrides(reg: "NativeRegistry", path) -> None:
    """natives.md §7.2 at load time (--overrides F): on the winner row of each key, set `tier` and, when the override has
    one, replace `default`. `cite` and `note` stay in the file. An unknown key stops it (LookupError)."""
    rows = json.loads(Path(path).read_text())
    for row in rows:
        c, f = row["key"].split(".", 1)
        k = reg._key(c, f)
        if k not in reg.entries:
            raise LookupError("unknown native key %r in %s" % (row["key"], path))
        e = reg.entries[k]
        e = e._replace(tier=row["tier"])
        if "default" in row:
            e = e._replace(default=parse_variant_spec(row["default"]))
        reg.entries[k] = e


def _parse_value(tok: list, i: int):
    """`<v>[ S %d <hex>][ G %d]` at tok[i]: (variant, text or None, G or None, next index)."""
    v = _parse_v(tok[i])
    i += 1
    text = g = None
    if i < len(tok) and tok[i] == "S":
        n = int(tok[i + 1])
        text = bytes.fromhex(tok[i + 2]) if n else b""
        i += 3 if n else 2
    if i < len(tok) and tok[i] == "G":
        g = int(tok[i + 1])
        i += 2
    return v, text, g, i


def parse_log_record(ln: str) -> dict:
    """One natives.log line (trace.md §6, natives.md §3.1): a native line (kind "n"), or an H / C / X / K record."""
    tok = ln.split(" ")
    head = tok[0]
    if head == "H":
        t, h = tok[1].split(":")
        return {"kind": "H", "type": int(t, 16), "handle": int(h, 16), "delta": int(tok[2]), "raw": ln}
    if head == "C":
        rec = {"kind": "C", "seq": int(tok[1]), "name": _unescape_name(tok[2]), "args": [], "raw": ln}
        i = 3
        while i < len(tok):
            v, text, g, i = _parse_value(tok, i)
            rec["args"].append((v, text, g))
        return rec
    if head == "X":
        return {"kind": "X", "seq": int(tok[1]), "raw": ln}
    if head == "K":
        return {"kind": "K", "frame": int(ln[1:]), "raw": ln}
    rec = {"kind": "n", "seq": int(tok[0]), "name": _unescape_name(tok[1]), "argc": int(tok[2]), "raw": ln}
    assert tok[3] == "->", ln
    rec["ret"], rec["ret_text"], rec["ret_g"], i = _parse_value(tok, 4)
    rec["writes"] = []
    while i < len(tok):
        assert tok[i] == "W", ln
        idx = int(tok[i + 1])
        v, text, g, i = _parse_value(tok, i + 2)
        rec["writes"].append((idx, v, text, g))
    return rec


def parse_log_line(ln: str) -> dict:
    """M2a-1 name: a native line (`%08d <cls>.<fn> %d -> <r>[ S %d <hex>][ G %d][ W %d <v>…]…`)."""
    return parse_log_record(ln)


def _unescape_name(s: str) -> str:
    return re.sub(r"\\x([0-9a-f]{2})", lambda m: chr(int(m.group(1), 16)), s).replace("\\\\", "\\")


def _parse_v(s: str) -> Variant:
    t, v = s.split(":")
    return Variant(int(t, 16), int(v, 16))


def parse_variant_spec(d) -> Variant:
    """{"type": "0x10", "value": 7} / {"type": "0x12", "bits": "3f800000"} -> Variant (no heap objects)."""
    t = d["type"]
    t = int(t, 0) if isinstance(t, str) else int(t)
    if "bits" in d:
        val = int(str(d["bits"]), 16)
    else:
        val = d.get("value", 0)
        val = int(val, 0) if isinstance(val, str) else int(val)
    return Variant(t, val & M32)


# ---------------------------------------------------------------------------------------------------------------
# Script image
# ---------------------------------------------------------------------------------------------------------------

class Script:
    """The OSI 4.1 tables (layout as tools/osi/osiasm.py writes and the VM reads them)."""

    def __init__(self, data: bytes):
        self.data = bytes(data)
        d, p = self.data, 0

        def u8():
            nonlocal p
            p += 1
            return d[p - 1]

        def u16():
            nonlocal p
            p += 2
            return struct.unpack_from("<H", d, p - 2)[0]

        def u32():
            nonlocal p
            p += 4
            return struct.unpack_from("<I", d, p - 4)[0]

        def take(n):
            nonlocal p
            if p + n > len(d):
                raise ValueError("OSI tables run past the end of the file")
            p += n
            return d[p - n:p]

        def strtab(offsets=None):
            out = []
            for _ in range(u16()):
                if offsets is not None:
                    offsets.append(p)
                n = u8()
                out.append(take(n))
                take(1)
            return out

        if take(4) != b"OSI\0":
            raise ValueError("bad magic")
        self.version = (u16(), u16())
        # ScOSIScript::ScOSIScript 0x609570 keeps a pointer per entry, table[i] = table[i-1] + length byte + 2
        # (0x60964e-0x60966c), and never checks the terminator byte; the VM reads an entry as the C string at table[i]+1.
        self.string_offsets: list[int] = []
        self.symbol_offsets: list[int] = []
        self.strings = strtab(self.string_offsets)
        self.globals = strtab()
        self.functions = []                 # (name bytes, offset, argc)
        for _ in range(u16()):
            n = u8()
            name = take(n)
            self.functions.append((name, u32(), u16()))
        ncls = u16()
        self.classes = []                   # [name, props(list of symbol ids), methods(list of (sym, off))]
        for _ in range(ncls):
            props = [u16() for _ in range(u8())]
            meths = [(u16(), u32()) for _ in range(u8())]
            self.classes.append([None, props, meths])
        for c in self.classes:
            n = u8()
            c[0] = take(n)
            take(1)
        self.symbols = strtab(self.symbol_offsets)
        self.sources = strtab()
        self.code_start = p
        self.member_cache: list[tuple[bytes, int]] = []   # FindMemberID's per-script cache (0x609c16, +0x48)

    def _c_string_at(self, off: int) -> bytes:
        """The C string at file offset off: up to the first NUL of the FILE, so an entry whose terminator byte is not
        NUL runs on into the next entry (natives.md §8.3: strlen 0x6573c0 at 0x4f5c13 on table[i]+1). Past the end of
        the image is not specified (rule 15(d)); the read stops there."""
        end = self.data.find(b"\0", off)
        return self.data[off:] if end < 0 else self.data[off:end]

    def string_text(self, idx: int) -> bytes:
        """String-table entry `idx & 0xffff` as the VM reads it: the C string at entry+1 (see _c_string_at)."""
        idx &= 0xFFFF
        if idx >= len(self.strings):
            raise VmError("Undefined behaviour: string-table index %d out of range" % idx)
        return self._c_string_at(self.string_offsets[idx] + 1)

    def symbol_text(self, idx: int) -> bytes:
        """Symbol entry idx: the C string at entry+1 (FindMemberID walks the same length+2 table, 0x609c70-0x609c7e)."""
        idx &= 0xFFFF
        if idx >= len(self.symbols):
            raise VmError("Undefined behaviour: symbol index %d out of range" % idx)
        return self._c_string_at(self.symbol_offsets[idx] + 1)

    def find_function(self, name: bytes):
        """FindFunction (0x609ce0): first entry whose name __strnicmp-matches the query over the entry's length."""
        q = _lower_az(name)
        for ename, off, argc in self.functions:
            e = _lower_az(ename)
            ok = True
            for i in range(len(e)):
                qc = q[i] if i < len(q) else 0
                if qc != e[i]:
                    ok = False
                    break
                if qc == 0:
                    break
            if ok:
                return off, argc
        return None

    def find_member_id(self, text: bytes) -> int:
        """FindMemberID (0x609ba0): lower-case (A-Z) the query, check the cache, then strcmp each symbol text; a miss
        returns 0xffff and caches {the last symbol compared, 0xffff} (0x609ca7-0x609cc8), so a later lookup of that symbol
        also returns 0xffff (cache poisoning). The cache (+0x48, 8-byte entries) has no bound: entry 50 lands on its own
        count at +0x1d8, so the 51st insert is undefined behaviour (controller ruling M1, fix round 1). With no symbols a
        miss caches a null pointer that the next cache scan would strcmp: also undefined."""
        q = _lower_az(cstr(text))
        for t, i in self.member_cache:
            if t is None:
                raise UndefinedBehaviour("Undefined behaviour: FindMemberID cache holds a null symbol pointer")
            if t == q:
                return i
        found, last = 0xFFFF, None                    # esi = 0 (0x609c57): no symbols leaves a null pointer
        for i in range(len(self.symbols)):
            last = self.symbol_text(i)
            if last == q:
                found = i
                break
        if len(self.member_cache) >= 50:
            raise UndefinedBehaviour("Undefined behaviour: FindMemberID cache overflow (51st miss)")
        self.member_cache.append((last, found))
        return found


# ---------------------------------------------------------------------------------------------------------------
# Arithmetic (rows 0x60-0x75, ConvertVariant)
# ---------------------------------------------------------------------------------------------------------------

def _num(v: Variant) -> float:
    return float(s32(v.value)) if v.type == T_INT else bits_to_f(v.value)


def _exact(v: Variant) -> Fraction:
    return Fraction(s32(v.value)) if v.type == T_INT else Fraction(bits_to_f(v.value))


def float_op(op: str, a: Variant, b: Variant, pc: int) -> int:
    """An x87 + - * / on exact operands (fild for ints, fld dword for floats), rounded to `pc` bits with the extended
    exponent range, then by fstp dword (D8'). Returns the f32 bits; NaN -> 0xffc00000 (D9)."""
    x, y = _num(a), _num(b)
    if not (math.isfinite(x) and math.isfinite(y)):
        try:
            r = {"+": x + y, "-": x - y, "*": x * y}[op] if op != "/" else x / y
        except ZeroDivisionError:
            r = math.copysign(math.inf, x) * math.copysign(1.0, y)
        return f32_bits(r)
    ex, ey = _exact(a), _exact(b)
    r = {"+": lambda: ex + ey, "-": lambda: ex - ey, "*": lambda: ex * ey, "/": lambda: ex / ey}[op]()
    if r == 0:
        z = {"+": lambda: x + y, "-": lambda: x - y, "*": lambda: x * y, "/": lambda: x / y}[op]()
        return 0x80000000 if math.copysign(1.0, z) < 0 else 0
    return exact_round_f32(r, pc)


def int_to_float_bits(n: int) -> int:
    """fild (exact) then one fstp dword rounding (0x60bd53-0x60bd55)."""
    return 0 if n == 0 else _f32_bits_fraction(Fraction(s32(n)))


def float_to_int(bits: int) -> int:
    """fistp qword with the rounding field forced to chop (0x60bc45-0x60bc5a), low 32 bits kept."""
    x = bits_to_f(bits)
    if math.isnan(x) or abs(x) >= 2.0 ** 63:
        return 0                                   # the integer indefinite 0x8000000000000000: low dword 0
    return int(x) & M32


_WS = b" \t\n\v\f\r"
_ATOI = re.compile(rb"[+-]?[0-9]+")
_ATOF = re.compile(rb"[+-]?(?:[0-9]+\.?[0-9]*|\.[0-9]+)(?:[eE][+-]?[0-9]+)?")


def c_atoi(text: bytes) -> int:
    # _atoi -> _strtol(s, 0, 10) saturates out-of-range values (0x65cd95-0x65cdbb), ERANGE; it does not wrap.
    m = _ATOI.match(cstr(text).lstrip(_WS))
    return (max(-2**31, min(2**31 - 1, int(m.group(0)))) & M32) if m else 0


def _strtold_overflow(s: str) -> tuple[bool, bool, bool]:
    """___strtold's range test on the matched decimal text: (overflow, exponent negative, mantissa negative).
    - Up to 20 significant digits are stored; leading integer zeros are skipped (state 4, 0x65c2a0); an integer digit
      past the 20th adds 1 to the adjustment (0x65c355); a fraction digit subtracts 1 while fewer than 20 are stored,
      a leading fraction zero is not stored (0x65c3c0-0x65c3e0).
    - The exponent magnitude sets the flag as soon as it exceeds 0x7fff (0x65c539), before its sign is applied.
    - Trailing stored zeros are stripped, each adding 1 (0x65c5c1-0x65c5df); the ADJUSTED exponent (exponent +
      adjustment) must lie in [-32768, 32767] (0x65c5fa-0x65c613)."""
    m = re.fullmatch(r"([+-]?)([0-9]*)(?:\.([0-9]*))?(?:[eE]([+-]?)([0-9]+))?", s)
    mant_neg = m.group(1) == "-"
    stored, adj, flag = "", 0, False
    for c in m.group(2).lstrip("0"):
        if len(stored) < 20:
            stored += c
        else:
            adj += 1
    for c in m.group(3) or "":
        if len(stored) < 20:
            if c != "0" or stored:
                stored += c
            adj -= 1
    exp, exp_neg = 0, m.group(4) == "-"
    for c in m.group(5) or "":
        exp = (exp * 10 + int(c)) & M32
        if s32(exp) > 0x7FFF:
            flag = True
    exp = s32(-exp if exp_neg else exp)
    while stored.endswith("0"):
        stored = stored[:-1]
        adj += 1
    if not -32768 <= exp + adj <= 32767:
        flag = True
    return flag, exp_neg, mant_neg


def c_atof(text: bytes) -> tuple[float, bool]:
    """atof: (double value, whether ___dec2num ran, i.e. whether the precision switches to 64 (D8')).

    SHARED BLIND SPOT (A6 risk, fix round 1 M3): the original rounds the decimal text into an MSL extended value, stores
    it with `fstp qword` (0x65c672) and then `fstp dword` (0x60bd13); this oracle takes Python's correctly rounded
    double and rounds that to f32. The two can differ for decimal text that is not exactly representable; the corpus
    uses exact text only, and both implementations share the reading."""
    m = _ATOF.match(cstr(text).lstrip(_WS))
    if not m:
        return 0.0, True
    s = m.group(0).decode("ascii")
    overflow, exp_neg, mant_neg = _strtold_overflow(s)
    if overflow:                                   # ___strtold's overflow path skips ___dec2num: no precision switch
        if exp_neg:
            return 0.0, False                      # +0.0 (0x762538) whatever the mantissa sign (0x65c626-0x65c635)
        return (-math.inf if mant_neg else math.inf), False   # +-HUGE_VAL (0x7620dc), 0x65c640-0x65c656
    return float(s if not s.endswith(".") else s[:-1] or "0"), True


def c_pow(x: float, y: float) -> float:
    try:
        return math.pow(x, y)
    except OverflowError:
        neg = x < 0 and float(y).is_integer() and int(y) % 2 == 1
        return -math.inf if neg else math.inf
    except ValueError:
        if x == 0 and y < 0:
            neg = math.copysign(1.0, x) < 0 and float(y).is_integer() and int(y) % 2 == 1
            return -math.inf if neg else math.inf
        return math.nan


def to_text_f(bits: int) -> bytes:
    return b"%f" % bits_to_f(bits)


# ---------------------------------------------------------------------------------------------------------------
# The VM
# ---------------------------------------------------------------------------------------------------------------

class CallResult(NamedTuple):
    found: bool
    ok: bool
    result: Variant


class VmState(NamedTuple):
    pc: int
    depth: int
    frame: int
    context_depth: int
    refcount: int
    top: Optional[Variant]
    halted: bool


ERR = {
    "unreg_var": b"Reference to unregistered Game Variable",
    "non_member": b"Member property of non member variable",
    "non_object": b"Member property of non object variable",
    "no_prop": b'Reference to non-existant member property "%s"',
    "no_method": b"Call to undefined member method",
    "method_non_member": b"Member method of non member variable",
    "unreg_fn": b"Call to unregistered Game Function",
    "conv": b"Requested variant conversion unimplimented.  Recompile to debug for types",
    "calling": b"Script is calling %s.%s",
    "calling_direct": b"Script is calling a game function directly.",
    "direct_literal": b"Direct call without a memoised site",
    "overflow": b"Stack overflow (1000 slots)",
    "underflow": b"Stack underflow",
    "divzero": b"Divide by 0",
    "too_many_entry": b"Too many args required (max 10) to call requested entry point.",
    "nest_cap": b"Undefined: engine-call nesting limit (64)",            # port cap (D10), not original
}
MAX_ENGINE_NESTING = 64


class Vm:
    def __init__(self, script: bytes, natives: NativeRegistry, heap: Heap, trace: Optional[TraceSink] = None,
                 *, fpu_pc: int = 24):
        self.script = Script(script)
        self.image = self.script.data
        self.natives = natives
        self.heap = heap
        self.trace = trace
        if heap.trace is None:
            heap.trace = trace
        self.stack: list[Variant] = [NOTHING] * STACK_SLOTS
        self.depth = 0
        self.frame = 0
        self.pc = 0
        self.context_depth = 0
        self.refcount = 0
        self.fpu_pc = fpu_pc
        self.globals: dict[int, Variant] = {}
        self.global_names: dict[int, bytes] = {}
        self.memo: dict[int, NativeEntry] = {}
        self.instructions = 0
        self.native_calls = 0
        self.halted = False
        self.last_error: Optional[VmError] = None
        self._nest = 0
        self.max_nesting = 0                  # D15: the high-water of engine calls in flight (the outermost included)
        self._outer_marker = None
        self._outer_saved_pc = 0
        self._outer_entry = (0, 0, 0)         # (context_depth, frame, refcount) at the outermost call's entry (D16 r6)
        self._seq_stack: list[int] = []       # sequence numbers of the natives executing, innermost last
        self.swallowed: list[VmError] = []    # errors swallowed by a native (D16 rule 2), in order
        self.host = None                      # the HostContext of osi_natives.framework, when one is attached
        self._ipc, self._iop = 0, 0
        self._sha = hashlib.sha256(self.image).hexdigest()
        self.pcs_seen: Optional[set] = None
        H = [Vm._unused] * 256
        for op, name in _HANDLERS.items():
            H[op] = getattr(Vm, name)
        self._h = H

    # ------------------------------------------------------------------ tracing / errors / fpu
    def _t(self, s: str) -> None:
        if self.trace is not None:
            self.trace.line(s)

    def _err(self, msg, *args) -> VmError:
        if args:
            msg = msg % args
        e = VmError(msg, self._ipc, self._iop, self.depth)
        return e

    def _set_pc64(self, pc: int) -> None:
        if self.fpu_pc != 64:
            self.fpu_pc = 64
            self._t("P %08x 64" % pc)

    # ------------------------------------------------------------------ stack (our caps, D10)
    def push(self, v: Variant) -> None:
        if self.depth >= STACK_SLOTS:
            raise self._err(ERR["overflow"])
        self.stack[self.depth] = v
        self.depth += 1

    def pop(self) -> Variant:
        if self.depth <= 0:
            raise self._err(ERR["underflow"])
        self.depth -= 1
        return self.stack[self.depth]

    def pop_dispose(self) -> Variant:
        if self.depth <= 0:
            raise self._err(ERR["underflow"])
        v = self.stack[self.depth - 1]
        self.heap.dispose(v)                  # Pop(true) 0x5fd260: Dispose first, then the slot moves out
        self.depth -= 1
        return v

    def _slot(self, i: int) -> int:
        if i < 0:
            raise self._err(ERR["underflow"])
        if i >= STACK_SLOTS:
            raise self._err(ERR["overflow"])
        return i

    def top(self) -> Optional[Variant]:
        return self.stack[self.depth - 1] if self.depth else None

    def scan_frame(self) -> int:
        """framePtr = (first slot scanning down from the top whose type is 2 or 0xff) + 1."""
        i = self.depth - 1
        while i >= 0:
            if self.stack[i].type in (T_RET, T_FRAME):
                return i + 1
            i -= 1
        return 0

    def insert_below(self, n: int, marker: Variant) -> None:
        if self.depth >= STACK_SLOTS:
            raise self._err(ERR["overflow"])
        base = self.depth - n
        if base < 0:
            raise self._err(ERR["underflow"])
        self.stack[base + 1:self.depth + 1] = self.stack[base:self.depth]
        del self.stack[STACK_SLOTS:]
        self.stack[base] = marker
        self.depth += 1

    # ------------------------------------------------------------------ public API
    def snapshot(self) -> VmState:
        return VmState(self.pc, self.depth, self.frame, self.context_depth, self.refcount, self.top(), self.halted)

    def global_value(self, name) -> Variant:
        b = name.encode("latin-1") if isinstance(name, str) else name
        return self.globals.get(get_token(b), NOTHING)

    def convert(self, v: Variant, target: int) -> Variant:
        r, src = self._convert(v, target)
        return r

    def call(self, name, args=()) -> CallResult:
        """The CallWithReturn protocol (opcodes.md "Engine calls"), re-entrant; the result is disposed by the host
        right after the R line (trace.md: as the original's Call does)."""
        nb = name.encode("latin-1") if isinstance(name, str) else bytes(name)
        args = list(args)
        if len(args) > 10:
            raise ValueError("at most 10 engine arguments")
        args += [NOTHING] * (10 - len(args))
        if self._nest >= MAX_ENGINE_NESTING:                    # port cap (D10): never the outermost call
            raise self._err(ERR["nest_cap"])
        self._nest += 1
        if self._nest > self.max_nesting:
            self.max_nesting = self._nest
        outer = self._nest == 1
        if outer:
            self._outer_marker = self.depth
            self._outer_saved_pc = self.pc        # the value CallWithReturn's {0xff, PC} marker gets (0x60b484)
            self._outer_entry = (self.context_depth, self.frame, self.refcount)
        try:
            return self._call_with_return(nb, args)
        except VmError as e:
            if not outer:
                raise
            e.top = self.top()
            self.last_error = e
            self._t('E %08x %02x %4d "%s"' % (e.pc, e.op, e.depth, msg_escape(e.message_bytes)))
            self._set_pc64(e.pc)             # the landing pad's fninit (0x657068 / 0x657245), D8'
            return CallResult(True, False, NOTHING)
        finally:
            self._nest -= 1

    def reset_after_error(self) -> None:
        """Our host behaviour (D10; D16 rule 6): pop (with dispose) from the top down to and including the failed
        outermost call's own marker, the slot at the depth it recorded at its entry, THROUGH any inner 0xff markers a
        swallow left (n-swallow-reerror), even when the script itself already popped that marker. Then context_depth,
        frame_ptr, PC (the marker's value: 0x60b484, restored at 0x60b5a2) and the VM refcount go back to their values
        at that call's entry; leftovers below that depth, left by an earlier successful call, stay. fpu_pc stays 64."""
        m = self._outer_marker if self._outer_marker is not None else 0
        while self.depth > m:
            v = self.stack[self.depth - 1]
            self.depth -= 1
            self.heap.dispose(v)
        self.pc = self._outer_saved_pc
        self.context_depth, self.frame, self.refcount = self._outer_entry
        self.last_error = None

    # ------------------------------------------------------------------ hooks
    def wrap_handlers(self, wrap) -> None:
        """Hook (M2a-2 T5): replace each of this VM's 256 opcode handlers h by wrap(op, h), a callable taking the VM.
        The A10 taint pass (tools/research/osi_taint.py) uses it to keep a shadow of the stack, the globals, the members
        and the arrays beside the VM. No runner calls it, so nothing changes when it is unused. Call it before the first
        engine call (Run binds the table on entry)."""
        H = self._h
        for op in range(256):
            H[op] = wrap(op, H[op])

    # ------------------------------------------------------------------ the native framework's VM side (M2a-2)
    def native_seq(self) -> int:
        """The sequence number of the innermost native executing (-1 outside any native: the host)."""
        return self._seq_stack[-1] if self._seq_stack else -1

    def swallow_error(self, e: VmError, seq: Optional[int]) -> None:
        """D16 rules 1-2, at the swallow point of a native's catch: the VM stays exactly as the throw left it; the E line
        of the swallowed error is written here (pc/op of the failing instruction, depth and top at the throw), then
        `P <pc> 64` if fpu_pc was 24 (the catch pad's fninit 0x657245). No last_error, no reset, no engine call ends.
        natives.log gets `X <seq>` (natives.md §3.1) when a log is kept, and the reserved `X -1` for a swallow on a host
        route (seq -1: no native executing). seq None (the replay) writes nothing."""
        e.top = self.top()
        e.pc_after = self.pc
        self._t('E %08x %02x %4d "%s"' % (e.pc, e.op, e.depth, msg_escape(e.message_bytes)))
        self._set_pc64(e.pc)
        self.swallowed.append(e)
        if seq is not None:
            self.log_x(seq)

    # natives.log v2 (natives.md §3; trace.md §6): lines in event order, written only while a log is kept
    def _log(self, line: str) -> None:
        log = self.natives.return_log
        if log is not None:
            log.append(line)

    def log_value(self, v: Variant) -> str:
        """`<v>`, then ` S %d <hex>` after a 0x16 and ` G %d` after a game value (the engine's references, no variant)."""
        s = fmt_v(v)
        if v.type == T_STRING:
            t = self.heap.text(v.value)
            s += (" S %d %s" % (len(t), t.hex())) if t else " S 0"
        elif is_game(v.type) and v.value in self.heap.games:
            s += " G %d" % self.heap.engine(v.value)
        return s

    def log_h(self, h: int, delta: int) -> None:
        self._log("H %04x:%08x %+d" % (self.heap.game_type(h), h, delta))

    @staticmethod
    def _log_seq(seq: int) -> str:
        """`%08d`, or the literal `-1` of the reserved host records (`C -1 …`, `X -1`), never `%08d` of -1."""
        return "-1" if seq < 0 else "%08d" % seq

    def log_c(self, seq: int, name, args) -> None:
        nb = name.encode("latin-1") if isinstance(name, str) else bytes(name)
        args = list(args) + [NOTHING] * (10 - len(args))
        self._log("C %s %s %s" % (self._log_seq(seq), name_token(nb), " ".join(self.log_value(a) for a in args)))

    def log_x(self, seq: int) -> None:
        self._log("X %s" % self._log_seq(seq))

    def begin_frame(self, frame: int) -> None:
        """The host's frame start: `K %6d` in natives.log (live), or the replay's check of that record."""
        if self.natives.replay is not None:
            self.natives.replay_frame(frame)
        else:
            self._log("K %6d" % frame)

    # ------------------------------------------------------------------ CallWithReturn (0x60b450)
    def _call_with_return(self, nb: bytes, args: list) -> CallResult:
        self.refcount += 1
        found = self.script.find_function(nb)
        self.push(Variant(T_FRAME, self.pc))                         # 0x60b484
        self.frame = self.scan_frame()
        self.context_depth = (self.context_depth + 1) & 0xFFFF
        result = NOTHING
        if found is None:
            self._t("M " + name_token(nb))                          # Message, not Error; args neither pushed nor disposed
        else:
            off, argc = found
            self._t("C %s %d %s" % (name_token(nb), argc, " ".join(fmt_v(a) for a in args)))
            self.pc = off                                           # SetPCAndArgs 0x60b5e0
            if argc > 10:
                self._ipc, self._iop = off, self.image[off] if off < len(self.image) else 0
                raise self._err(ERR["too_many_entry"])
            for i in range(10):
                if i < argc:
                    self.push(args[i])
                else:
                    self.heap.dispose(args[i])
            self._run()
            result = self.pop()                                     # Pop(false), 0x60b4fc
        while True:                                                 # Pop(true) until the 0xff marker, 0x60b53e
            v = self.pop_dispose()
            if v.type == T_FRAME:
                break
        self.context_depth = (self.context_depth - 1) & 0xFFFF
        if self.context_depth != 0:
            self.frame = self.scan_frame()
        self.pc = v.value
        self.refcount -= 1
        if found is not None:
            self._t("R " + fmt_v(result))
            self.heap.dispose(result)
        return CallResult(found is not None, True, result)

    # ------------------------------------------------------------------ Run (0x60b850)
    def _run(self) -> None:
        image, H, trace = self.image, self._h, self.trace
        n = len(image)
        self.halted = False
        while True:
            pc = self.pc
            if not 0 <= pc < n:
                self._ipc, self._iop = pc, 0
                raise self._err("Undefined behaviour: PC %d outside the script image" % pc)
            op = image[pc]
            self._ipc, self._iop = pc, op
            if trace is not None:
                d = self.depth
                trace.line("I %08x %02x %4d %4d %s" % (pc, op, d, self.frame,
                                                       fmt_v(self.stack[d - 1]) if d else "----:--------"))
            self.instructions += 1
            if self.pcs_seen is not None:
                self.pcs_seen.add(pc)
            try:
                cont = H[op](self)
            except (IndexError, struct.error):
                raise self._err("Undefined behaviour: operand read past the end of the script image") from None
            if not cont:
                return

    # operand readers
    def _u8(self, at):
        return self.image[at]

    def _u16(self, at):
        return self.image[at] | (self.image[at + 1] << 8)

    def _s16(self, at):
        v = self._u16(at)
        return v - 0x10000 if v & 0x8000 else v

    def _u32(self, at):
        return struct.unpack_from("<I", self.image, at)[0]

    # ------------------------------------------------------------------ handlers: nop / line / debug / unused
    def _unused(self):
        raise self._err(b"Call of undefined instruction")             # BCLUnused 0x60be60, PC not advanced

    def op_nop(self):
        self.pc += 1
        return True

    def op_debug_on(self):
        self.pc += 1                                                  # 0x6038a6, then Error
        raise self._err(b"Debugger not enabled! but encountered DebugOn")

    def op_debug_off(self):
        self.pc += 1
        raise self._err(b"Debugger not enabled! but encountered DebugOff")

    def op_line(self):
        self.pc += 5
        return True

    # ------------------------------------------------------------------ members
    def _game_type_error(self):
        return self._err(ERR["unreg_var"])

    def _obj(self, v: Variant):
        if v.type != T_OBJECT:
            raise self._err("Undefined behaviour: member access on a non-object (type %02x)" % v.type)
        return self.heap._rec(self.heap.objects, v)

    def _get_property_index(self, rec, m: int) -> Optional[int]:
        """GetProperty 0x6086a0: the first i (counted in 8 bits) whose class property symbol is m."""
        syms = rec[3]
        for i, s in enumerate(syms[:255]):
            if s == (m & 0xFFFF):
                return i & 0xFF
        return None

    def _find_method(self, rec, m: int) -> int:
        cls = self._class(rec[1])
        for sym, off in cls[2]:
            if sym == (m & 0xFFFF):
                return off
        raise self._err(ERR["no_method"])

    def _class(self, c: int):
        if c >= len(self.script.classes):
            raise self._err("Undefined behaviour: class index %d out of range" % c)
        return self.script.classes[c]

    def op_set_member(self):                                          # 0x10
        m = self._u16(self.pc + 1)
        self.pc += 3
        o = self.pop()
        if is_game(o.type):
            raise self._game_type_error()
        if o.type != T_OBJECT:
            raise self._err(ERR["non_member"])
        v = self.pop()
        rec = self._obj(o)
        i = self._get_property_index(rec, m)
        if i is None:
            raise self._err(ERR["no_prop"] % self._symbol_text(m))
        self.heap.dispose(rec[2][i])
        rec[2][i] = v
        self.heap.dispose(o)
        return True

    def op_get_member(self):                                          # 0x11
        m = self._u16(self.pc + 1)
        self.pc += 3
        o = self.pop()
        if is_game(o.type):
            raise self._game_type_error()
        rec = self._obj(o)
        i = self._get_property_index(rec, m)
        if i is None:
            raise self._err("Undefined behaviour: 0x11 on an absent property (null dereference in the original)")
        self.push(self.heap.duplicate(rec[2][i]))
        self.heap.dispose(o)
        return True

    def op_get_member_fn(self):                                       # 0x12
        m = self._u16(self.pc + 1)
        self.pc += 3
        o = self.pop()
        rec = self._obj(o)
        off = self._find_method(rec, m)
        self.push(Variant(T_INT, off))
        self.heap.dispose(o)
        return True

    def op_create_object(self):                                       # 0x13
        c = self._u16(self.pc + 1)
        self.pc += 3
        cls = self._class(c)
        h = self.heap.new_object(c, len(cls[1]), cls[1])
        self.push(Variant(T_OBJECT, h))
        return True

    def op_arg_check(self):                                           # 0x14
        n = self._u8(self.pc + 1)
        self.pc += 2
        k = (self.depth - self.frame) & 0xFF
        if k > n:
            raise self._err(b"Too many arguments to method")
        while k < n:
            self.push(NOTHING)
            k += 1
        return True

    def _this(self):
        this = self.stack[self._slot(self.frame)]
        return this

    def op_set_this_member(self):                                     # 0x15
        m = self._u16(self.pc + 1)
        self.pc += 3
        this = self._this()
        if this.type != T_OBJECT:
            raise self._err(ERR["non_member"])
        v = self.pop()
        rec = self._obj(this)
        i = self._get_property_index(rec, m)
        if i is None:
            raise self._err(ERR["no_prop"] % self._symbol_text(m))
        self.heap.dispose(rec[2][i])
        rec[2][i] = v
        return True

    def op_get_this_member(self):                                     # 0x16
        m = self._u16(self.pc + 1)
        self.pc += 3
        this = self._this()
        if this.type != T_OBJECT:
            raise self._err(ERR["non_member"])
        rec = self._obj(this)
        i = self._get_property_index(rec, m)
        if i is None:
            raise self._err(ERR["no_prop"] % self._symbol_text(m))
        self.push(self.heap.duplicate(rec[2][i]))
        return True

    def op_get_this_member_fn(self):                                  # 0x17
        m = self._u16(self.pc + 1)
        self.pc += 3
        this = self._this()
        if this.type != T_OBJECT:
            raise self._err(ERR["method_non_member"])
        rec = self._obj(this)
        self.push(Variant(T_INT, self._find_method(rec, m)))
        return True

    def _find_member_id(self, text: bytes) -> int:
        try:
            return self.script.find_member_id(text)
        except UndefinedBehaviour as e:
            raise self._err(str(e)) from None

    def _string_text(self, idx: int) -> bytes:
        try:
            return self.script.string_text(idx)
        except VmError as e:                                          # a port error at this instruction (rule 15(c))
            raise self._err(e.message_bytes) from None

    def _symbol_text(self, idx: int) -> bytes:
        try:
            return self.script.symbol_text(idx)
        except VmError as e:
            raise self._err(e.message_bytes) from None

    def _name_text(self, s: Variant) -> bytes:
        return self._string_text(s.value) if s.type == T_SID else self.heap.text(s.value)

    def _name_copy(self, s: Variant) -> bytes:
        """The 0x18/0x19/0x1a/0x83 name copy into a local ScFixedString of capacity 0xff (natives.md §8.3):
        operator=(const char*) 0x4f5c00 for a 0x11 (the C string at the entry, strlen 0x6573c0 at 0x4f5c13) or
        operator=(const ScFixedString&) 0x4f5b00 for a 0x16. Both throw kOutOfRange when len > capacity (`seta` at
        0x4f5c27 / 0x4f5b16, SrThrow 0x4f5c38 / 0x4f5b27): 255 passes, 256 throws, at the handler's PC then."""
        text = self._name_text(s)
        if len(text) > 255:
            raise self._err(b"kOutOfRange")
        return text

    def op_get_member_str(self):                                      # 0x18
        self.pc += 1
        s = self.pop()
        o = self.pop()
        if s.type not in (T_SID, T_STRING):
            raise self._err(b"Invalid type for member access (must be a string)")
        text = self._name_copy(s)                                     # 0x5fef3b / 0x5fef69, before the object checks
        if is_game(o.type):
            raise self._game_type_error()
        if o.type != T_OBJECT:
            raise self._err(ERR["non_member"])
        rec = self._obj(o)
        i = self._get_property_index(rec, self._find_member_id(text))
        if i is None:
            raise self._err(ERR["no_prop"] % cstr(text))
        self.push(self.heap.duplicate(rec[2][i]))
        self.heap.dispose(o)
        self.heap.dispose(s)
        return True

    def op_get_member_fn_str(self):                                   # 0x19
        self.pc += 1
        s = self.pop()
        o = self.pop()
        if o.type != T_OBJECT:
            raise self._err(ERR["method_non_member"])
        if s.type not in (T_SID, T_STRING):
            raise self._err(b"Invalid method type for function call (must be a string)")
        text = self._name_copy(s)                                     # 0x5ff2e7 / 0x5ff313, after the object check
        rec = self._obj(o)
        off = self._find_method(rec, self._find_member_id(text))
        self.push(Variant(T_INT, off))
        self.heap.dispose(o)
        self.heap.dispose(s)
        return True

    def op_set_member_str(self):                                      # 0x1a
        self.pc += 1
        s = self.pop()
        o = self.pop()
        if s.type not in (T_SID, T_STRING):
            raise self._err(b"Invalid type for member set (must be a string)")
        text = self._name_copy(s)                                     # 0x5ff49b / 0x5ff4c9, before the object checks
        if is_game(o.type):
            raise self._game_type_error()
        if o.type != T_OBJECT:
            raise self._err(ERR["non_object"])
        v = self.pop()
        rec = self._obj(o)
        i = self._get_property_index(rec, self._find_member_id(text))
        if i is None:
            raise self._err(ERR["no_prop"] % cstr(text))
        self.heap.dispose(rec[2][i])
        rec[2][i] = v
        self.heap.dispose(o)
        self.heap.dispose(s)
        return True

    # ------------------------------------------------------------------ variables
    def _global_token(self, idx: int) -> int:
        i = idx & 0xFF
        if i >= len(self.script.globals):
            raise self._err("Undefined behaviour: global index %d past the global-name table" % i)
        name = cstr(self.script.globals[i])
        tok = get_token(name)
        if tok not in self.globals:                                   # 0x609e10 inserts {nothing}
            self.globals[tok] = NOTHING
            self.global_names[tok] = name
        return tok

    def op_get_var(self):                                             # 0x21
        idx = self._u16(self.pc + 1)
        self.pc += 3
        if idx & 0x8000:
            v = self.globals[self._global_token(idx)]
        else:
            v = self.stack[self._slot(self.frame + idx)]
        self.push(self.heap.duplicate(v))
        return True

    def op_set_var(self):                                             # 0x22
        idx = self._u16(self.pc + 1)
        self.pc += 3
        v = self.pop()
        if idx & 0x8000:
            tok = self._global_token(idx)
            self.heap.dispose(self.globals[tok])
            self.globals[tok] = v
        else:
            i = self._slot(self.frame + idx)
            self.heap.dispose(self.stack[i])                          # PushIntoFrame 0x607c00
            self.stack[i] = v
        return True

    def op_create_vars(self):                                         # 0x23
        n = self._u8(self.pc + 1)
        self.pc += 2
        for _ in range(n):
            self.push(NOTHING)
        return True

    def _incdec(self, delta, frame_msg):
        idx = self._u16(self.pc + 1)
        self.pc += 3
        if idx & 0x8000:
            tok = self._global_token(idx)
            v = self.globals[tok]
            if v.type != T_INT:
                raise self._err(b"increment of non-integral type")    # also for 0x25 (0x607deb, copy-paste text)
            self.globals[tok] = Variant(v.type, (v.value + delta) & M32)
        else:
            i = self._slot(self.frame + idx)
            v = self.stack[i]
            if v.type != T_INT:
                raise self._err(frame_msg)
            self.stack[i] = Variant(v.type, (v.value + delta) & M32)
        return True

    def op_inc_var(self):                                             # 0x24
        return self._incdec(1, b"increment of non-integral type")

    def op_dec_var(self):                                             # 0x25
        return self._incdec(-1, b"decrement of non-integral type")

    # ------------------------------------------------------------------ stack ops
    def op_pop(self):                                                 # 0x30
        self.pc += 1
        self.pop_dispose()
        return True

    def op_popn(self):                                                # 0x31
        n = self._u8(self.pc + 1)
        self.pc += 2
        for _ in range(n):
            self.pop_dispose()
        return True

    def op_swap(self):                                                # 0x32
        self.pc += 1
        x = self.pop()
        y = self.pop()
        self.push(x)
        self.push(y)
        return True

    def op_pull(self):                                                # 0x33
        n = self._u8(self.pc + 1)
        self.pc += 2
        if n == 0:
            raise self._err("Undefined behaviour: Pull 0 copies the slot above the top")
        self.push(self.heap.duplicate(self.stack[self._slot(self.depth - n)]))
        return True

    def op_dupn(self):                                                # 0x34
        n = self._u8(self.pc + 1)
        self.pc += 2
        for _ in range(n):
            self.push(self.heap.duplicate(self.stack[self._slot(self.depth - n)]))
        return True

    def op_dup(self):                                                 # 0x35
        self.pc += 1
        self.push(self.heap.duplicate(self.stack[self._slot(self.depth - 1)]))
        return True

    # ------------------------------------------------------------------ pushes
    def op_push_i32(self):
        v = self._u32(self.pc + 1)
        self.pc += 5
        self.push(Variant(T_INT, v))
        return True

    def op_push_i24(self):                                            # 0x41: always errors, PC not advanced
        raise self._err(b"unwritten instruction BCL_PushConstanti24 called")

    def op_push_i16(self):
        v = self._s16(self.pc + 1)
        self.pc += 3
        self.push(Variant(T_INT, v & M32))
        return True

    def op_push_i8(self):
        v = self._u8(self.pc + 1)
        self.pc += 2
        self.push(Variant(T_INT, (v - 0x100 if v & 0x80 else v) & M32))
        return True

    def op_push_f32(self):
        v = self._u32(self.pc + 1)
        self.pc += 5
        self.push(Variant(T_FLOAT, v))
        return True

    def op_push_0(self):
        self.pc += 1
        self.push(Variant(T_INT, 0))
        return True

    def op_push_str(self):
        v = self._u16(self.pc + 1)
        self.pc += 3
        self.push(Variant(T_SID, v))
        return True

    def op_push_nothing(self):
        self.pc += 1
        self.push(NOTHING)
        return True

    def op_push_c8888(self):
        v = self._u32(self.pc + 1)
        self.pc += 5
        self.push(Variant(T_COLOUR, v))
        return True

    def op_push_c5551(self):
        v = self._u16(self.pc + 1)
        self.pc += 3
        self.push(Variant(T_COL16, v))
        return True

    # ------------------------------------------------------------------ calls / branches
    def _jump(self, file_relative: bool):
        n = self._u8(self.pc + 1)
        self.pc += 2
        t = self.pop()
        if t.type != T_INT:
            raise self._err(b"Internal error: Jump to non-integral address")
        self.insert_below(n, Variant(T_RET, self.pc))
        self.frame = self.scan_frame()
        # 0x50: script-file base + t; 0x51: t is the code pointer from 0x12/0x17/0x19, carried as a file offset (D4)
        self.pc = t.value
        return True

    def op_jump_rel(self):                                            # 0x50
        return self._jump(True)

    def op_jump_abs(self):                                            # 0x51
        return self._jump(False)

    def op_return(self):                                              # 0x52
        r = self.pop()
        while True:
            m = self.pop_dispose()
            if m.type in (T_RET, T_FRAME):
                break
        if m.type == T_FRAME:
            self.push(m)
            self.push(r)
            return False
        self.push(r)
        self.pc = m.value
        self.frame = self.scan_frame()
        return True

    def op_branch_if_false(self):                                     # 0x53
        c = self.pop()
        if c.type == T_NOTHING or (c.type == T_INT and c.value == 0):
            self.pc = self.pc + 3 + self._s16(self.pc + 1)
        elif c.type == T_INT:
            self.pc += 3
        else:
            raise self._err(b"Results of comparison expression is not integral")
        return True

    def op_branch(self):                                              # 0x54
        self.pc = self.pc + 3 + self._s16(self.pc + 1)
        return True

    def op_halt(self):                                                # 0xff
        self.pc += 1
        self.push(NOTHING)
        self.halted = True
        return False

    # ------------------------------------------------------------------ compare
    def _cmp(self, sym: bytes, fn):
        self.pc += 1
        b = self.pop()
        a = self.pop()
        ta, tb = a.type, b.type
        if (ta == T_FLOAT or ta == T_INT) and (tb == T_FLOAT or tb == T_INT) and (ta == T_FLOAT or tb == T_FLOAT):
            self.push(Variant(T_INT, 1 if fn(_num(a), _num(b)) else 0))
            return True
        if ta != T_INT:
            raise self._err(b"Invalid operand to operator '" + sym + b"' (lhs) (type:%02X)" % ta)
        if tb != T_INT:
            raise self._err(b"Invalid operand to operator '" + sym + b"' (rhs) (type:%02X)" % tb)
        self.push(Variant(T_INT, 1 if fn(s32(a.value), s32(b.value)) else 0))
        return True

    def op_lt(self):
        return self._cmp(b"<", lambda x, y: x < y)

    def op_gt(self):
        return self._cmp(b">", lambda x, y: x > y)

    def op_le(self):
        return self._cmp(b"<=", lambda x, y: x <= y)

    def op_ge(self):
        return self._cmp(b">=", lambda x, y: x >= y)

    def op_eq(self):                                                  # 0x60
        self.pc += 1
        b = self.pop()
        a = self.pop()
        H = self.heap
        if a.type == T_NOTHING:
            if b.type == T_NOTHING:
                self.push(Variant(T_INT, 1))
            else:
                self.push(Variant(T_INT, 0))
                H.dispose(b)
            return True
        if b.type == T_NOTHING:
            self.push(Variant(T_INT, 0))
            H.dispose(a)
            return True
        if a.type == T_OBJECT and b.type == T_OBJECT:
            self.push(Variant(T_INT, 1 if a.value == b.value else 0))
            H.dispose(a)
            H.dispose(b)
            return True
        if is_game(a.type) and is_game(b.type):
            self.push(Variant(T_INT, 1 if (a.type == b.type and a.value == b.value) else 0))
            H.dispose(a)
            H.dispose(b)
            return True
        if a.type == T_SID:
            a = Variant(T_STRING, self._sid_to_dyn(a.value))
        if b.type == T_SID:
            b = Variant(T_STRING, self._sid_to_dyn(b.value))
        if a.type == T_STRING and b.type == T_STRING:
            x, y = H.text(a.value), H.text(b.value)
            eq = len(x) == len(y) and all(
                (cx - 0x20 if 0x61 <= cx <= 0x7A else cx) == (cy - 0x20 if 0x61 <= cy <= 0x7A else cy)
                for cx, cy in zip(x, y))
            self.push(Variant(T_INT, 1 if eq else 0))
            H.dispose(a)
            H.dispose(b)
            return True
        ta, tb = a.type, b.type
        if (ta in (T_FLOAT, T_INT)) and (tb in (T_FLOAT, T_INT)) and (ta == T_FLOAT or tb == T_FLOAT):
            self.push(Variant(T_INT, 1 if _num(a) == _num(b) else 0))
            return True
        if ta != T_INT:
            raise self._err(b"Invalid operand to operator '=' (lhs) (type:%02X)" % ta)
        if tb != T_INT:
            raise self._err(b"Invalid operand to operator '=' (rhs) (type:%02X)" % tb)
        self.push(Variant(T_INT, 1 if a.value == b.value else 0))
        return True

    # ------------------------------------------------------------------ logic / bitwise
    def _int2(self, sym: bytes):
        self.pc += 1
        b = self.pop()
        a = self.pop()
        if a.type != T_INT:
            raise self._err(b"Invalid operand to operator '" + sym + b"' (lhs) (type:%02X)" % a.type)
        if b.type != T_INT:
            raise self._err(b"Invalid operand to operator '" + sym + b"' (rhs) (type:%02X)" % b.type)
        return a, b

    def op_and(self):
        a, b = self._int2(b"and")
        self.push(Variant(T_INT, 1 if (a.value and b.value) else 0))
        return True

    def op_or(self):
        a, b = self._int2(b"or")
        self.push(Variant(T_INT, 1 if (a.value or b.value) else 0))
        return True

    def op_bit_and(self):
        a, b = self._int2(b"&")
        self.push(Variant(T_INT, a.value & b.value))
        return True

    def op_bit_or(self):
        a, b = self._int2(b"|")
        self.push(Variant(T_INT, a.value | b.value))
        return True

    def op_bit_xor(self):
        a, b = self._int2(b"@")
        self.push(Variant(T_INT, a.value ^ b.value))
        return True

    def op_shl(self):
        a, b = self._int2(b"<<")
        self.push(Variant(a.type, (a.value << (b.value & 31)) & M32))
        return True

    def op_shr(self):
        a, b = self._int2(b">>")
        self.push(Variant(a.type, (a.value & M32) >> (b.value & 31)))
        return True

    def _unary(self, sym: bytes, fn):
        self.pc += 1
        a = self.pop()
        if a.type == T_NOTHING:
            self.push(Variant(T_INT, 1))
            return True
        if a.type != T_INT:
            raise self._err(b"Invalid operand to operator '" + sym + b"' (type:%02X)" % a.type)
        self.push(Variant(T_INT, fn(a.value) & M32))
        return True

    def op_not(self):
        return self._unary(b"not", lambda v: 1 if v == 0 else 0)

    def op_bit_not(self):
        return self._unary(b"~", lambda v: ~v)

    def op_inc(self):                                                 # 0x7a: raw value + 1, no type check
        self.pc += 1
        i = self._slot(self.depth - 1)
        v = self.stack[i]
        self.stack[i] = Variant(v.type, (v.value + 1) & M32)
        return True

    def op_dec(self):                                                 # 0x7b
        self.pc += 1
        i = self._slot(self.depth - 1)
        v = self.stack[i]
        self.stack[i] = Variant(v.type, (v.value - 1) & M32)
        return True

    # ------------------------------------------------------------------ arithmetic
    def _sid_to_dyn(self, idx: int) -> int:
        """ConvertStringIDToDynamicString (0x60b880): a new 0x16 from entry idx (strlen); >= 255 chars throws
        kOutOfRange from the string ctor 0x60b8d0 (0x60b970), before the string exists."""
        text = self._string_text(idx)
        if len(text) >= 255:
            raise self._err(b"kOutOfRange")
        return self.heap.new_string(text)

    def _arith(self, op: str, lhs_msg: bytes, rhs_msg: bytes):
        b = self.pop()
        a = self.pop()
        ta, tb = a.type, b.type
        if ta == T_FLOAT or tb == T_FLOAT:
            if ta in (T_FLOAT, T_INT) and tb in (T_FLOAT, T_INT):
                if op == "/":
                    if tb == T_FLOAT:
                        if bits_to_f(b.value) == 0.0:                 # ordered equality: -0.0 errors, NaN does not
                            raise self._err(ERR["divzero"])
                    elif b.value == 0:
                        raise self._err(ERR["divzero"])
                self.push(Variant(T_FLOAT, float_op(op, a, b, self.fpu_pc)))
                return True
        if ta != T_INT:
            raise self._err(lhs_msg % ta)
        if tb != T_INT:
            raise self._err(rhs_msg % tb)
        x, y = s32(a.value), s32(b.value)
        if op == "+":
            r = x + y
        elif op == "-":
            r = x - y
        elif op == "*":
            r = x * y
        else:
            if y == 0:
                raise self._err(ERR["divzero"])
            if x == -2 ** 31 and y == -1:
                raise self._err("Undefined behaviour: INT_MIN / -1 raises #DE in the original")
            q = abs(x) // abs(y)
            r = q if (x < 0) == (y < 0) else -q
            self.push(Variant(T_INT, r & M32))
            return True
        self.push(Variant(a.type, r & M32))
        return True

    def op_add(self):                                                 # 0x70
        self.pc += 1
        b = self.pop()
        a = self.pop()
        if a.type == T_SID:
            a = Variant(T_STRING, self._sid_to_dyn(a.value))
        if b.type == T_SID:
            b = Variant(T_STRING, self._sid_to_dyn(b.value))
        if a.type == T_STRING:
            if b.type == T_STRING:
                ta, tb = self.heap.strings[a.value], self.heap.text(b.value)
                if len(ta) >= 0xFF - len(tb):                         # ScFixedString::append 0x4f5c80 (unsigned)
                    raise self._err(b"kLenTooLong")
                ta += tb
                self.push(a)
                self.heap.dispose(b)
                return True
            raise self._err(b"Invalid operand to string operator '+' (rhs) (type:%02X)" % b.type)
        if b.type == T_STRING:
            raise self._err(b"Invalid operand to string operator '+' (lhs) (type:%02X)" % a.type)
        self.push(a)
        self.push(b)
        return self._arith("+", b"Invalid operand to operator '+' (lhs) (type:%02X)",
                           b"Invalid operand to operator '+' (rhs) (type:%02X)")

    def op_sub(self):
        self.pc += 1
        return self._arith("-", b"Invalid operand to operator '-' (lhs) (type:%02X)",
                           b"Invalid operand to operator '-' (rhs) (type:%02X)")

    def op_mul(self):
        self.pc += 1
        return self._arith("*", b"Invalid left-hand-side operand to operator '*' (type:%02X)",
                           b"Invalid operand to operator '*' (rhs) (type:%02X)")

    def op_div(self):
        self.pc += 1
        return self._arith("/", b"Invalid operand to operator '/' (lhs) (type:%02X)",
                           b"Invalid operand to operator '/' (rhs) (type:%02X)")

    def op_mod(self):                                                 # 0x75
        a, b = self._int2(b"%")
        if b.value == 0:
            raise self._err(ERR["divzero"])
        self.push(Variant(a.type, (a.value & M32) % (b.value & M32)))
        return True

    def op_pow(self):                                                 # 0x74
        self.pc += 1
        b = self.pop()
        a = self.pop()
        ta, tb = a.type, b.type
        if ta == T_FLOAT and tb == T_FLOAT:
            self.push(Variant(T_FLOAT, f32_bits(c_pow(bits_to_f(a.value), bits_to_f(b.value)))))
            return True
        if ta == T_INT and tb == T_FLOAT:
            self.push(Variant(T_FLOAT, f32_bits(c_pow(float(s32(a.value)), bits_to_f(b.value)))))
            return True
        if ta == T_FLOAT and tb == T_INT:
            x = a.value
            n = b.value & M32
            seen = {}
            i = 0
            while i < n:                    # x <- (float)(int32 bits of x) * x  (0x604944-0x604955), b times
                if x in seen:               # a cycle: skip whole periods (the loop is deterministic)
                    period = i - seen[x]
                    i += ((n - i) // period) * period
                    seen = {}
                    if i >= n:
                        break
                seen[x] = i
                x = float_op("*", Variant(T_INT, x), Variant(T_FLOAT, x), self.fpu_pc)
                i += 1
            self.push(Variant(a.type, x))
            return True
        if ta != T_INT:
            raise self._err(b"Invalid operand to operator '^' (lhs) (type:%02X)" % ta)
        if tb != T_INT:
            raise self._err(b"Invalid operand to operator '^' (rhs) (type:%02X)" % tb)
        x, n, i = a.value & M32, b.value & M32, 0
        while i < n:                        # x <- x*x (wrapping imul), b times; squaring mod 2^32 reaches a fixed point
            y = (x * x) & M32
            i += 1
            if y == x:
                break
            x = y
        self.push(Variant(a.type, x))
        return True

    # ------------------------------------------------------------------ ConvertVariant (0x60b9d0)
    def _convert(self, v: Variant, target: int) -> tuple[Variant, Variant]:
        """Returns (result, the source variant afterwards): a 0x11 source of an int/float target is replaced in place
        by a dynamic string (0x60bbf9-0x60bc02, 0x60bcf9-0x60bd02)."""
        H = self.heap
        if v.type == target:
            return H.duplicate(v), v
        if target == T_INT:
            if v.type == T_SID:
                v = Variant(T_STRING, self._sid_to_dyn(v.value))
                return Variant(T_INT, c_atoi(H.text(v.value))), v
            if v.type == T_FLOAT:
                return Variant(T_INT, float_to_int(v.value)), v
            if v.type == T_STRING:
                return Variant(T_INT, c_atoi(H.text(v.value))), v
            return Variant(T_NONE, 0), v
        if target == T_FLOAT:
            if v.type == T_INT:
                return Variant(T_FLOAT, int_to_float_bits(v.value)), v
            if v.type in (T_SID, T_STRING):
                if v.type == T_SID:
                    v = Variant(T_STRING, self._sid_to_dyn(v.value))
                x, switch = c_atof(H.text(v.value))
                if switch:
                    self._set_pc64(self._ipc)
                return Variant(T_FLOAT, f32_bits(x)), v
            return Variant(T_NONE, 0), v
        if target == T_STRING:
            if v.type == T_INT:
                return Variant(T_STRING, H.new_string(b"%d" % s32(v.value))), v
            if v.type == T_SID:
                return Variant(T_STRING, self._sid_to_dyn(v.value)), v
            if v.type == T_FLOAT:
                return Variant(T_STRING, H.new_string(to_text_f(v.value))), v
            return Variant(T_NONE, 0), v
        raise self._err(ERR["conv"])

    def _convert_op(self, target):
        self.pc += 1
        a = self.pop()
        r, a2 = self._convert(a, target)
        self.heap.dispose(a2)
        self.push(r)
        return True

    def op_to_string(self):
        return self._convert_op(T_STRING)

    def op_to_float(self):
        return self._convert_op(T_FLOAT)

    def op_to_int(self):
        return self._convert_op(T_INT)

    # ------------------------------------------------------------------ type tests
    def _test(self, pred):
        self.pc += 1
        a = self.pop()
        self.push(Variant(T_INT, 1 if pred(a.type) else 0))
        self.heap.dispose(a)
        return True

    def op_is_int(self):
        return self._test(lambda t: t == T_INT)

    def op_is_float(self):
        return self._test(lambda t: t == T_FLOAT)

    def op_is_string(self):
        return self._test(lambda t: t in (T_STRING, T_SID))

    def op_is_object(self):
        return self._test(lambda t: t == T_OBJECT)

    def op_is_game(self):
        return self._test(is_game)

    def op_is_array(self):
        return self._test(lambda t: t == T_ARRAY)

    def op_class_id(self):                                            # 0xbf
        self.pc += 1
        o = self.pop()
        if o.type == T_OBJECT:
            self.push(Variant(T_INT, self._obj(o)[1] & 0xFFFF))
        elif is_game(o.type):
            self.push(Variant(T_INT, o.type + 0x10000))
        else:
            raise self._err(b"Cant get classID of non object")
        self.heap.dispose(o)
        return True

    # ------------------------------------------------------------------ arrays
    def _arr(self, a: Variant):
        return self.heap._rec(self.heap.arrays, a)[1]

    def op_create_array(self):
        self.pc += 1
        self.push(Variant(T_ARRAY, self.heap.new_array()))
        return True

    def op_get_array(self):                                           # 0x91
        self.pc += 1
        i = self.pop()
        a = self.pop()
        if a.type == T_STRTAB:
            raise self._err("Undefined behaviour: no 0x13 value can exist")
        if a.type != T_ARRAY:
            raise self._err(b"array reference on non-array")
        el = self._arr(a)
        if (i.value & M32) >= len(el):
            raise self._err(b"Reference beyond start/end of array")
        self.push(self.heap.duplicate(el[i.value]))
        self.heap.dispose(a)
        return True

    def op_count_array(self):                                         # 0x92
        self.pc += 1
        a = self.pop()
        if a.type != T_ARRAY:
            raise self._err(b"array reference on non-array")
        self.push(Variant(T_INT, len(self._arr(a))))
        self.heap.dispose(a)
        return True

    def op_set_array(self):                                           # 0x93
        self.pc += 1
        v = self.pop()
        i = self.pop()
        a = self.pop()
        if a.type == T_STRTAB:
            raise self._err(b"Assigning into string table is illegal")
        if a.type != T_ARRAY:
            raise self._err(b"array reference on non-array")
        el = self._arr(a)
        if (i.value & M32) >= len(el):
            raise self._err(b"Access beyond end of array")
        self.heap.dispose(el[i.value])
        el[i.value] = v
        self.heap.dispose(a)
        return True

    def op_append_array(self):                                        # 0x94
        self.pc += 1
        v = self.pop()
        a = self.pop()
        if a.type == T_STRTAB:
            raise self._err(b"Assigning into string table is illegal")
        if a.type != T_ARRAY:
            raise self._err(b"array reference on non-array")
        self._arr(a).append(v)
        self.heap.dispose(a)
        return True

    def op_remove_array(self):                                        # 0x95
        self.pc += 1
        i = self.pop()
        a = self.pop()
        if a.type == T_STRTAB:
            raise self._err(b"Removal from string table is illegal")
        if a.type != T_ARRAY:
            raise self._err(b"array reference on non-array")
        el = self._arr(a)
        if (i.value & M32) >= len(el):
            raise self._err(b"removal outside of array bounds")
        self.heap.dispose(el[i.value])
        del el[i.value]
        self.heap.dispose(a)
        return True

    def op_insert_array(self):                                        # 0x96
        self.pc += 1
        v = self.pop()
        i = self.pop()
        a = self.pop()
        if a.type == T_STRTAB:
            raise self._err(b"Assigning into string table is illegal")
        if a.type != T_ARRAY:
            raise self._err(b"array reference on non-array")
        el = self._arr(a)
        if (i.value & M32) >= len(el):
            raise self._err(b"insertion outside of array bounds")
        el.insert(i.value, v)
        self.heap.dispose(a)
        return True

    # ------------------------------------------------------------------ colour
    def _set_channel(self, name: bytes, shift: int):
        self.pc += 1
        v = self.pop()
        c = self.pop()
        if c.type != T_COLOUR:
            raise self._err(b"cant access " + name + b" value of anything but a color")
        if v.type != T_INT:
            raise self._err(b"cant set " + name + b" to a non-integral value")
        x = v.value & M32
        if x > 0xFF:
            x = 0xFF
        self.push(Variant(c.type, (c.value & ~(0xFF << shift) & M32) | (x << shift)))
        return True

    def _get_channel(self, name: bytes, shift: int):
        self.pc += 1
        c = self.pop()
        if c.type != T_COLOUR:
            raise self._err(b"cant access " + name + b" value of anything but a color")
        self.push(Variant(T_INT, (c.value >> shift) & 0xFF))
        return True

    def op_set_r(self):
        return self._set_channel(b"red", 24)

    def op_set_g(self):
        return self._set_channel(b"green", 16)

    def op_set_b(self):
        return self._set_channel(b"blue", 8)

    def op_set_a(self):
        return self._set_channel(b"alpha", 0)

    def op_get_r(self):
        return self._get_channel(b"red", 24)

    def op_get_g(self):
        return self._get_channel(b"green", 16)

    def op_get_b(self):
        return self._get_channel(b"blue", 8)

    def op_get_a(self):
        return self._get_channel(b"alpha", 0)

    # ------------------------------------------------------------------ game variables (no-ops in this build)
    def op_game_var(self):
        self.pc += 5
        return True

    # ------------------------------------------------------------------ native calls
    def _native_call(self, entry: NativeEntry, n: int, site: int, cls_text: bytes, fn_text: bytes) -> Variant:
        """Steps 3-7 of trace.md §3 (argument coercion, the native, S/N lines, the pops with dispose). Returns the
        result, which the caller pushes. Raises on any failure (the caller wraps it per its catch(...))."""
        base = self.depth - n
        if base < 0:
            raise self._err(ERR["underflow"])
        slots = list(range(base, self.depth))
        types = entry.types
        byref = []
        for i in range(n):
            t = types[i] if i < len(types) else T_NOTHING
            if t == T_BYREF:
                byref.append(True)
                continue
            byref.append(False)
            v = self.stack[slots[i]]
            if v.type != t:
                r, v2 = self._convert(v, t)
                self.heap.dispose(v2)                                 # PushInto 0x601edc
                self.stack[slots[i]] = r
        seq = self.native_calls                                       # assigned at native entry (natives.md §3.2)
        call = NativeCall(self, entry, site, slots, seq)
        call.cls_text, call.fn_text = cls_text, fn_text
        self.native_calls += 1
        self._seq_stack.append(seq)
        try:
            res = self.natives.invoke(call)
        finally:
            self._seq_stack.pop()
        if entry.sets_fpu_pc64:
            self._set_pc64(site)
        args = [self.stack[s] for s in slots]
        for i, a in enumerate(args):
            if not byref[i] and a.type == T_STRING:
                self._t_string(a.value)
        if res.type == T_STRING:
            self._t_string(res.value)
        parts = ["N %08x %s.%s %d" % (site, name_token(cls_text), name_token(fn_text), n)]
        for i, a in enumerate(args):
            parts.append("0200:%08x" % slots[i] if byref[i] else fmt_v(a))
        parts.append("-> " + fmt_v(res))
        self._t(" ".join(parts))
        if self.natives.return_log is not None:                      # N's own line, at its return (event order)
            self._log(self._log_line(seq, cls_text, fn_text, n, res, call.writes))
        for _ in range(n):                                            # n Pops WITH dispose, 0x601c5f
            self.pop_dispose()
        return res

    def _t_string(self, h: int) -> None:
        t = self.heap.text(h)
        self._t(("S %08x %3d %s" % (h, len(t), t.hex())) if t else ("S %08x %3d" % (h, 0)))

    def _log_line(self, seq, cls_text, fn_text, n, res, writes) -> str:
        out = "%08d %s.%s %d -> %s" % (seq, name_token(cls_text), name_token(fn_text), n, self.log_value(res))
        for i, v in writes:
            out += " W %d %s" % (i, self.log_value(v))
        return out

    def op_call_native(self):                                         # 0x82
        site = self.pc
        ci, fi, n = self._u16(site + 1), self._u16(site + 3), self._u8(site + 5)
        self.pc += 5                                                  # 5 incs 0x6015c6-0x60162c before the lookup
        C, F = self._string_text(ci), self._string_text(fi)           # a bad index is a port error at +5 (rule 15(c))
        memo = self.memo.get(site)
        if memo is None:
            entry = self.natives.find(C, F)
            if entry is None:
                raise self._err(ERR["unreg_fn"])                     # SrOSIError 0x601725, outside every try
        else:
            entry = memo
        self.pc += 1                                                  # the sixth inc, 0x601780
        saved = (self._ipc, self._iop)
        try:
            if memo is None:
                if n > entry.max:
                    raise self._err(b"Too many parameters passed to game function")
                if n < entry.min:
                    raise self._err(b"Too few parameters passed to game function")
            res = self._native_call(entry, n, site, C, F)
            self.push(res)                                            # 0x601c8d (inside the try)
        except VmError:
            self._ipc, self._iop = saved
            if memo is None:
                raise self._err(ERR["calling"] % (C, F)) from None   # Error at 0x601dd3
            raise self._err(ERR["calling_direct"]) from None         # the 0x84 path, Error at 0x60303c
        self.memo[site] = entry                                       # D4: memoise, never write the image
        return True

    def op_call_native_str(self):                                     # 0x83
        site = self.pc
        ci = self._u16(site + 1)
        self.pc += 3
        s = self.pop()                                                # 0x6021c9, without dispose
        if s.type not in (T_SID, T_STRING):
            raise self._err(b"Invalid member type for function call (must be a string)")   # 0x6022c3, +3
        F = cstr(self._name_copy(s))                                  # 0x60226a / 0x602299, before the try: +3
        C = self._string_text(ci)
        saved = (self._ipc, self._iop)
        try:
            entry = self.natives.find(C, F)
            n = self._u8(self.pc)
            self.pc += 1
            if entry is None:
                raise self._err(ERR["unreg_fn"])
            if n > entry.max:
                raise self._err(b"Too many parameters passed to game function")
            if n < entry.min:
                raise self._err(b"Too few parameters passed to game function")
            res = self._native_call(entry, n, site, C, F)
            self.push(res)
        except VmError:
            self._ipc, self._iop = saved
            raise self._err(ERR["calling"] % (C, F)) from None       # Error at 0x602b24
        self.heap.dispose(s)                                          # 0x602b91, after the push
        return True

    def op_call_native_direct(self):                                  # a literal 0x84 in the file (D10)
        raise self._err(ERR["direct_literal"])


_HANDLERS = {
    0x00: "op_nop", 0x02: "op_debug_on", 0x03: "op_debug_off", 0x04: "op_line", 0x05: "op_line", 0x06: "op_line",
    0x10: "op_set_member", 0x11: "op_get_member", 0x12: "op_get_member_fn", 0x13: "op_create_object",
    0x14: "op_arg_check", 0x15: "op_set_this_member", 0x16: "op_get_this_member", 0x17: "op_get_this_member_fn",
    0x18: "op_get_member_str", 0x19: "op_get_member_fn_str", 0x1A: "op_set_member_str",
    0x21: "op_get_var", 0x22: "op_set_var", 0x23: "op_create_vars", 0x24: "op_inc_var", 0x25: "op_dec_var",
    0x30: "op_pop", 0x31: "op_popn", 0x32: "op_swap", 0x33: "op_pull", 0x34: "op_dupn", 0x35: "op_dup",
    0x40: "op_push_i32", 0x41: "op_push_i24", 0x42: "op_push_i16", 0x43: "op_push_i8", 0x44: "op_push_f32",
    0x45: "op_push_0", 0x46: "op_push_str", 0x47: "op_push_nothing", 0x48: "op_push_c8888", 0x49: "op_push_c5551",
    0x50: "op_jump_rel", 0x51: "op_jump_abs", 0x52: "op_return", 0x53: "op_branch_if_false", 0x54: "op_branch",
    0x60: "op_eq", 0x61: "op_lt", 0x62: "op_gt", 0x63: "op_le", 0x64: "op_ge",
    0x6A: "op_and", 0x6B: "op_or", 0x6C: "op_not", 0x6D: "op_bit_and", 0x6E: "op_bit_or", 0x6F: "op_bit_xor",
    0x70: "op_add", 0x71: "op_sub", 0x72: "op_mul", 0x73: "op_div", 0x74: "op_pow", 0x75: "op_mod",
    0x76: "op_bit_not", 0x77: "op_shl", 0x78: "op_shr", 0x7A: "op_inc", 0x7B: "op_dec",
    0x80: "op_game_var", 0x81: "op_game_var", 0x82: "op_call_native", 0x83: "op_call_native_str",
    0x84: "op_call_native_direct",
    0x90: "op_create_array", 0x91: "op_get_array", 0x92: "op_count_array", 0x93: "op_set_array",
    0x94: "op_append_array", 0x95: "op_remove_array", 0x96: "op_insert_array",
    0xA0: "op_set_r", 0xA1: "op_set_g", 0xA2: "op_set_b", 0xA3: "op_set_a",
    0xA4: "op_get_r", 0xA5: "op_get_g", 0xA6: "op_get_b", 0xA7: "op_get_a",
    0xB0: "op_to_string", 0xB1: "op_to_float", 0xB2: "op_to_int",
    0xB8: "op_is_int", 0xB9: "op_is_float", 0xBA: "op_is_string", 0xBB: "op_is_object", 0xBC: "op_is_game",
    0xBD: "op_is_array", 0xBF: "op_class_id", 0xFF: "op_halt",
}


# ---------------------------------------------------------------------------------------------------------------
# Helpers shared by the runners
# ---------------------------------------------------------------------------------------------------------------

def sha256_file(path) -> str:
    return hashlib.sha256(Path(path).read_bytes()).hexdigest() if path and Path(path).exists() else hashlib.sha256(b"").hexdigest()


def header_line(script: bytes, natives_p, mods_p, types_p, extra: str = "") -> str:
    return "# lomn-osi-trace 1 script=%s natives=%s mods=%s types=%s fpu=pc24%s" % (
        hashlib.sha256(script).hexdigest(), sha256_file(natives_p), sha256_file(mods_p), sha256_file(types_p), extra)


DEFAULT_NATIVES = REPO / "data" / "osi" / "natives.json"
DEFAULT_MODS = REPO / "data" / "osi" / "natives_beta150.json"
DEFAULT_TYPES = REPO / "data" / "osi" / "types.json"


def host_variant(vm_or_heap, spec, types: TypeTable) -> Variant:
    """A conformance value spec as a host-owned variant: plain variants, or {"text"}/{"hex"} (a new 0x16 on the VM
    heap, conformance.md §6.2), or {"game": Class} (a new game handle)."""
    heap = vm_or_heap.heap if isinstance(vm_or_heap, Vm) else vm_or_heap
    if spec is None:
        return NOTHING
    if "text" in spec:
        return Variant(T_STRING, heap.new_string(spec["text"].encode("latin-1")))
    if "hex" in spec:
        return Variant(T_STRING, heap.new_string(bytes.fromhex(spec["hex"])))
    if "game" in spec:
        t = types.id(spec["game"])
        return Variant(t, heap.new_game(t))
    return parse_variant_spec(spec)


def test_native_entry(spec: dict, types: TypeTable, reg: NativeRegistry) -> NativeEntry:
    """A declarative test native (conformance.md §1, §6.1 and the native-framework fields; rules 17 and 18), built on
    the native framework (osi_natives.framework.HostContext), as a native model would be:
    - `calls` {entry, args, depth}: the nested call (`engine_call`), made only while context_depth < depth;
    - `swallow`: the nested call's error is swallowed (D16 rules 1-2), then the writes and the return as usual;
    - `writes`: by-reference writes into the argument slots the handler addressed before the call (D16 rule 3);
    - `returns` {game, engine_refs, object}: a host object the engine holds engine_refs times, through create_variant
      (the first return of a named object creates it; later returns while it is live give the same handle)."""
    tys = list(spec.get("types", []))
    mx = spec.get("max", len(tys))
    tys = [types.id(t) for t in tys] + [T_NOTHING] * (10 - len(tys))

    def fn(call: NativeCall) -> Variant:
        from osi_natives.framework import GameObject, HostContext
        vm = call.vm
        ctx = HostContext.of(vm)
        nested = spec.get("calls")
        if nested is not None and ("depth" not in nested or vm.context_depth < int(nested["depth"])):
            nargs = [host_variant(vm, a, types) for a in nested.get("args", [])]
            ctx.engine_call(nested["entry"], nargs, swallow=bool(spec.get("swallow", False)))
        for w in spec.get("writes", []):
            call.write(int(w["arg"]), host_variant(vm, w, types))
        ret = spec.get("returns")
        if ret is None:
            return NOTHING
        if "arg" in ret:
            return vm.heap.duplicate(call.arg(int(ret["arg"])))
        if "game" in ret:
            t = types.id(ret["game"])
            k = int(ret.get("engine_refs", 0))
            name = ret.get("object")
            obj = ctx.test_objects.get(name) if name is not None else None
            if obj is None or obj.deleted:
                obj = GameObject(t, k)
                if name is not None:
                    ctx.test_objects[name] = obj
            return ctx.create_variant(obj, t)
        return host_variant(vm, ret, types)

    return NativeEntry(spec["cls"], spec["name"], (spec["cls"] + "." + spec["name"]).lower(), spec.get("min", 0), mx,
                       tuple(tys), "test", 0, None, fn, "test", bool(spec.get("sets_fpu_pc64", False)))


# ---------------------------------------------------------------------------------------------------------------
# --conform (docs/osi/conformance.md)
# ---------------------------------------------------------------------------------------------------------------

def _spec_eq(vm: Vm, got: Optional[Variant], spec, text_of=None) -> Optional[str]:
    """None when equal, else a description."""
    if spec is None:
        return None if got is None else "expected empty, got %s" % fmt_v(got)
    if got is None:
        return "expected %r, got empty" % (spec,)
    if "text" in spec or "hex" in spec:
        want = spec["text"].encode("latin-1") if "text" in spec else bytes.fromhex(spec["hex"])
        if got.type != T_STRING:
            return "expected a string %r, got %s" % (want, fmt_v(got))
        have = text_of(got) if text_of else None
        return None if have == want else "expected text %r, got %r" % (want, have)
    if "game" in spec:
        t = vm.natives.types.id(spec["game"])
        return None if got.type == t else "expected game type %02x, got %s" % (t, fmt_v(got))
    want = parse_variant_spec(spec)
    if "bits" in spec or "value" in spec:
        return None if want == got else "expected %s, got %s" % (fmt_v(want), fmt_v(got))
    return None if want.type == got.type else "expected type %04x, got %s" % (want.type, fmt_v(got))


def run_program(asm_path: Path, trace_dir: Optional[Path] = None, *, natives_p=DEFAULT_NATIVES, mods_p=DEFAULT_MODS,
                types_p=DEFAULT_TYPES) -> list[str]:
    """Runs one corpus program; returns the list of FAIL reasons (empty = pass)."""
    import osiasm
    exp_path = asm_path.with_name(asm_path.name[:-4] + ".expect.json")
    spec = json.loads(exp_path.read_text())
    bad = uncompared_keys(spec)
    if bad:                                           # fail closed: a field nobody compares must not pass unexamined
        return ["uncompared key " + k for k in bad]
    shape = [k for k, want in (("then", dict),) if k in spec and not isinstance(spec[k], want)]
    shape += ["%s.swallowed" % e for e in ("expect", "expect_error")
              if isinstance(spec.get(e), dict) and "swallowed" in spec[e] and not isinstance(spec[e]["swallowed"], list)]
    if shape:
        return ["malformed key " + k for k in shape]
    try:
        image, info = osiasm.assemble(asm_path.read_text(encoding="latin-1"))
    except osiasm.AsmError as e:
        return ["assemble: %s" % e]
    types = TypeTable.load(types_p)
    if spec.get("registry", "none") == "natives.json":
        reg = NativeRegistry.load(natives_p, mods_p, types)
    else:
        reg = NativeRegistry(types)
    for n in spec.get("natives", []):
        reg.add(test_native_entry(n, types, reg))
    trace = None
    if trace_dir is not None:
        trace_dir.mkdir(parents=True, exist_ok=True)
        trace = FileTrace(trace_dir / (asm_path.name[:-4] + ".trace"))
        trace.line(header_line(image, natives_p, mods_p, types_p))
    heap = Heap(trace)
    vm = Vm(image, reg, heap, trace)
    fails = []
    try:
        args = [host_variant(vm, a, types) for a in spec.get("args", [])]
        texts = {}
        orig_dispose = heap.dispose

        def capture(v):                       # keep a result string's text for the "text" comparison
            if v.type == T_STRING and v.value in heap.strings:
                texts[v.value] = bytes(heap.strings[v.value])
            orig_dispose(v)

        heap.dispose = capture
        res = vm.call(spec.get("entry", "main"), args)
        heap.dispose = orig_dispose
        snap = vm.snapshot()
        labels = info["labels"]
        text_of_live = lambda v: heap.text(v.value) if v.value in heap.strings else None   # noqa: E731
        if "expect" in spec:
            ex = spec["expect"]
            if not res.ok:
                e = vm.last_error
                fails.append('ok: expected true, got error "%s" at %08x op %02x' % (e.message, e.pc, e.op))
            else:
                if ex.get("ok", True) is not True:
                    fails.append("ok: expected %r, got true" % ex.get("ok"))
                if "result" in ex:
                    d = _spec_eq(vm, res.result, ex["result"], lambda v: texts.get(v.value))
                    if d:
                        fails.append("result: " + d)
            if "top" in ex:                                    # after the host disposed the result (M2a-2 T2b)
                d = _spec_eq(vm, snap.top, ex["top"], text_of_live)
                if d:
                    fails.append("top: " + d)
            _cmp_state(vm, ex, snap, fails)
            _cmp_pc_after(ex, snap, labels, fails)
            _cmp_swallowed(vm, ex.get("swallowed", []), labels, fails)
        else:
            ex = spec["expect_error"]
            if res.ok:
                fails.append("expect_error: the call succeeded (result %s)" % fmt_v(res.result))
            else:
                e = vm.last_error
                if "message" in ex and e.message_bytes != ex["message"].encode("latin-1"):
                    fails.append("message: expected %r, got %r" % (ex["message"], e.message))
                if "pc" in ex:
                    want = _pc_spec(ex["pc"], labels)
                    if want != e.pc:
                        fails.append("pc: expected %s, got %08x" % (want if want is None else "%08x" % want, e.pc))
                if "op" in ex:
                    want = int(ex["op"], 0) if isinstance(ex["op"], str) else ex["op"]
                    if want != e.op:
                        fails.append("op: expected %02x, got %02x" % (want, e.op))
                if "depth" in ex and ex["depth"] != e.depth:
                    fails.append("depth: expected %d, got %d" % (ex["depth"], e.depth))
                if "top" in ex:
                    d = _spec_eq(vm, e.top, ex["top"], text_of_live)
                    if d:
                        fails.append("top: " + d)
                _cmp_state(vm, ex, snap, fails, error=True)
                _cmp_pc_after(ex, snap, labels, fails)
            _cmp_swallowed(vm, ex.get("swallowed", []), labels, fails)
            if not res.ok and "then" not in spec:
                # D10: snapshot taken; reset, then the VM must be usable again (our behaviour)
                vm.reset_after_error()
                if any(f[0].lower() == b"main2" for f in vm.script.functions):
                    r2 = vm.call("main2", [])
                    if not r2.ok:
                        fails.append("main2 after reset: %s" % vm.last_error.message)
        if "then" in spec:                                     # rule 19: a second engine call on the same VM
            _run_then(vm, spec["then"], res, types, labels, fails)
        sha_after = hashlib.sha256(vm.image).hexdigest()
        if sha_after != hashlib.sha256(image).hexdigest():
            fails.append("tripwire: the script image changed")
    except ReplayDesync as e:
        fails.append("replay: %s" % e)
    except VmError as e:
        fails.append("host error: %s" % e.message)
    except RecursionError as e:                                 # M1: like the C++ "Undefined: host exception: ..."
        fails.append("host error: Undefined: host exception: RecursionError: %s" % e)
    finally:
        if trace is not None:
            trace.close()
    return fails


def _pc_spec(want, labels) -> Optional[int]:
    """An absolute file offset: an integer, "0x…", a label "@L" or "@L+k" (k decimal; conformance.md §1)."""
    if isinstance(want, int):
        return want
    if isinstance(want, str) and want.startswith("@"):
        m = re.fullmatch(r"@([^+]+)(?:\+([0-9]+))?", want)
        if not m or m.group(1) not in labels:
            return None
        return labels[m.group(1)] + int(m.group(2) or 0)
    return int(want, 0)


def _cmp_pc_after(ex: dict, snap: VmState, labels, fails: list, what: str = "pc_after") -> None:
    """snapshot().pc, compared under conformance rule 15 (opcodes.md error-PC table)."""
    if "pc_after" in ex:
        want = _pc_spec(ex["pc_after"], labels)
        if want != snap.pc:
            fails.append("%s: expected %s, got %08x" % (what, "?" if want is None else "%08x" % want, snap.pc))


def _cmp_swallowed(vm: Vm, want: list, labels, fails: list) -> None:
    """The errors swallowed during the call, in order (D16 rule 2): exactly these, so [] (or no key) means none."""
    got = vm.swallowed
    if len(got) != len(want):
        fails.append("swallowed: expected %d, got %d (%s)" % (len(want), len(got), "; ".join(
            '"%s" at %08x' % (e.message, e.pc) for e in got)))
        return
    for i, (w, e) in enumerate(zip(want, got)):
        if "message" in w and e.message_bytes != w["message"].encode("latin-1"):
            fails.append("swallowed[%d].message: expected %r, got %r" % (i, w["message"], e.message))
        if "pc" in w and _pc_spec(w["pc"], labels) != e.pc:
            fails.append("swallowed[%d].pc: expected %s, got %08x" % (i, w["pc"], e.pc))
        if "op" in w:
            op = int(w["op"], 0) if isinstance(w["op"], str) else w["op"]
            if op != e.op:
                fails.append("swallowed[%d].op: expected %02x, got %02x" % (i, op, e.op))
        if "depth" in w and w["depth"] != e.depth:
            fails.append("swallowed[%d].depth: expected %d, got %d" % (i, w["depth"], e.depth))
        if "top" in w:
            d = _spec_eq(vm, e.top, w["top"], lambda v: vm.heap.text(v.value) if v.value in vm.heap.strings else None)
            if d:
                fails.append("swallowed[%d].top: %s" % (i, d))
        if "pc_after" in w and _pc_spec(w["pc_after"], labels) != e.pc_after:
            fails.append("swallowed[%d].pc_after: expected %s, got %08x" % (i, w["pc_after"], e.pc_after))


def _run_then(vm: Vm, th: dict, first: CallResult, types: TypeTable, labels, fails: list) -> None:
    """Conformance rule 19: after the first call's comparison, reset only if it raised (else the second call runs above
    whatever the first left, D16 rule 5); then call then.entry and compare the fields given, like `expect`."""
    if not first.ok:
        vm.reset_after_error()
    heap = vm.heap
    args = [host_variant(vm, a, types) for a in th.get("args", [])]
    texts = {}
    orig_dispose = heap.dispose

    def capture(v):
        if v.type == T_STRING and v.value in heap.strings:
            texts[v.value] = bytes(heap.strings[v.value])
        orig_dispose(v)
    heap.dispose = capture
    try:
        r = vm.call(th.get("entry", "main2"), args)
    finally:
        heap.dispose = orig_dispose
    snap = vm.snapshot()
    want_ok = th.get("ok", True)
    if r.ok is not want_ok:
        fails.append("then.ok: expected %r, got %r%s" % (want_ok, r.ok, (' ("%s")' % vm.last_error.message)
                                                         if not r.ok else ""))
    if "result" in th and r.ok:
        d = _spec_eq(vm, r.result, th["result"], lambda v: texts.get(v.value))
        if d:
            fails.append("then.result: " + d)
    if "depth" in th and th["depth"] != snap.depth:
        fails.append("then.depth: expected %d, got %d" % (th["depth"], snap.depth))
    if "top" in th:
        d = _spec_eq(vm, snap.top, th["top"], lambda v: heap.text(v.value) if v.value in heap.strings else None)
        if d:
            fails.append("then.top: " + d)
    if "context_depth" in th and th["context_depth"] != snap.context_depth:
        fails.append("then.context_depth: expected %d, got %d" % (th["context_depth"], snap.context_depth))
    _cmp_pc_after(th, snap, labels, fails, "then.pc_after")


def _cmp_state(vm: Vm, ex: dict, snap: VmState, fails: list, error: bool = False) -> None:
    if not error and "depth" in ex and ex["depth"] != snap.depth:
        fails.append("depth: expected %d, got %d" % (ex["depth"], snap.depth))
    if "context_depth" in ex and ex["context_depth"] != snap.context_depth:
        fails.append("context_depth: expected %d, got %d" % (ex["context_depth"], snap.context_depth))
    if "fpu_pc" in ex and ex["fpu_pc"] != vm.fpu_pc:
        fails.append("fpu_pc: expected %d, got %d" % (ex["fpu_pc"], vm.fpu_pc))
    for name, want in (ex.get("globals") or {}).items():
        d = _spec_eq(vm, vm.global_value(name), want, lambda v: vm.heap.text(v.value) if v.value in vm.heap.strings else None)
        if d:
            fails.append("globals.%s: %s" % (name, d))
    hp = ex.get("heap")
    if hp:
        live, allocs = vm.heap.live(), vm.heap.allocs
        for k, v in (hp.get("live") or {}).items():
            if live.get(k) != v:
                fails.append("heap.live.%s: expected %d, got %d" % (k, v, live.get(k)))
        for k, v in (hp.get("allocs") or {}).items():
            if allocs.get(k) != v:
                fails.append("heap.allocs.%s: expected %d, got %d" % (k, v, allocs.get(k)))


# The expectation keys this oracle reads (fail-closed, docs/osi/conformance.md amendment 18). A key path is the chain of object
# keys from the top of the .expect.json, `[*]` after a key whose value is an array of objects, and `*` for the name of a global
# under expect.globals / expect_error.globals. A non-pending program with a key path in neither list FAILS before it runs:
# "uncompared key <path>". The C++ runner (engine/tools/lomn_osi_conform.cpp) keeps its own list; both print as sorted
# `compared|ignored <path>` lines with `--list-keys`, and tools/tests/test_osi_conform_keys.py asserts they are the same set.
_VALUE_FORMS = ("text", "hex", "game", "type", "value", "bits")      # a value spec (host_variant / parse_variant_spec / _spec_eq)


def _value_keys(prefix: str) -> list[str]:
    return [prefix] + [prefix + "." + k for k in _VALUE_FORMS]


def _state_keys(prefix: str) -> list[str]:
    heap = [prefix + ".heap", prefix + ".heap.live", prefix + ".heap.allocs"]
    heap += ["%s.heap.%s.%s" % (prefix, part, kind) for part in ("live", "allocs") for kind in "oasg"]
    return [prefix, prefix + ".context_depth", prefix + ".fpu_pc", prefix + ".globals"] + _value_keys(prefix + ".globals.*") + heap


COMPARED_KEYS = frozenset(
    ["registry", "entry", "args", "natives", "expect", "expect_error"]
    + [k for k in _value_keys("args[*]")[1:]]
    + ["natives[*]." + k for k in ("cls", "name", "min", "max", "types", "sets_fpu_pc64", "calls", "writes", "returns")]
    + ["natives[*].calls.entry", "natives[*].calls.args"] + _value_keys("natives[*].calls.args[*]")[1:]
    + ["natives[*].writes[*].arg"] + _value_keys("natives[*].writes[*]")[1:]
    + ["natives[*].returns.arg"] + _value_keys("natives[*].returns")[1:]
    + ["expect.ok", "expect.depth"] + _value_keys("expect.result") + _state_keys("expect")[1:]
    + ["expect_error.message", "expect_error.pc", "expect_error.op", "expect_error.depth"] + _value_keys("expect_error.top")
    + _state_keys("expect_error")[1:]
    # M2a-2 T4: every key group n uses (tests/osi/conformance/n; conformance.md "Native-framework fields", rules 15-19).
    # Exactly the paths a program needs (rule 22), no unused value forms: 38 keys, 105 -> 143.
    + ["natives[*].swallow", "natives[*].calls.depth", "natives[*].returns.engine_refs", "natives[*].returns.object"]
    + ["expect.top", "expect.top.type", "expect.top.value", "expect.pc_after", "expect_error.pc_after"]
    + [p + ".swallowed" + k for p in ("expect", "expect_error")
       for k in ("", "[*].message", "[*].pc", "[*].op", "[*].depth", "[*].top", "[*].top.type", "[*].top.value")]
    + ["then", "then.entry", "then.args", "then.ok", "then.result", "then.result.type", "then.result.value", "then.depth",
       "then.top", "then.top.type", "then.top.value", "then.context_depth", "then.pc_after"])
# descriptive keys no runner compares (the coverage report reads some of them): listed so that nothing else passes unexamined
IGNORED_KEYS = frozenset(["id", "rows", "note", "alternative", "falsifier", "falsifier_id", "argc_violation", "pending"])


def _key_paths(node, path="", pattern=""):
    """(real path, pattern path) of every object key under `node`; arrays of objects add `[*]`, scalars add nothing."""
    if isinstance(node, list):
        for v in node:
            yield from _key_paths(v, path + "[*]", pattern + "[*]")
    elif isinstance(node, dict):
        for k, v in node.items():
            wild = "*" if pattern in ("expect.globals", "expect_error.globals") else k
            rp, pp = (path + "." + k) if path else k, (pattern + "." + wild) if pattern else wild
            yield rp, pp
            yield from _key_paths(v, rp, pp)


def uncompared_keys(spec) -> list[str]:
    """The key paths of an expectation that no runner compares and the ignored list does not allow."""
    return [rp for rp, pp in _key_paths(spec) if pp not in COMPARED_KEYS and pp not in IGNORED_KEYS]


def conform(directory, trace_dir=None) -> int:
    """Runs every program under DIR. A program whose expect.json has a non-empty string "pending" (the task id) is not run:
    it prints `PENDING <group>/<id> (<task>)` and is counted apart; it needs no .asm, writes no trace and never fails."""
    from conform_pending import pending_task
    d = Path(directory)
    passed, pending, total = 0, 0, 0
    for exp in sorted(d.rglob("*.expect.json")):
        asm = exp.with_name(exp.name[:-len(".expect.json")] + ".asm")
        task = pending_task(exp)
        if task is not None:
            pending += 1
            print("PENDING %s (%s)" % (asm.relative_to(d).with_suffix("").as_posix(), task))
            continue
        if not asm.exists():
            continue
        total += 1
        td = None
        if trace_dir is not None:
            td = Path(trace_dir) / asm.parent.relative_to(d)
        fails = run_program(asm, td)
        if fails:
            print("FAIL %s: %s" % (asm.relative_to(d), "; ".join(fails)))
        else:
            passed += 1
    print("conformance: %d/%d pass, %d pending" % (passed, total, pending))
    return 0 if passed == total and (total or pending) else 1


# ---------------------------------------------------------------------------------------------------------------
# --run (A6: Setup + frames + keys on Base.osi, typed stubs D11/D12 or the replay log)
# ---------------------------------------------------------------------------------------------------------------

C1 = 0x636E7431
DT_BITS_DOUBLE = 0x3F7B4E81C0000000      # the double at 0x712d68 (fake dt, D3)
KEY_VK = {"down": 0x28, "enter": 0x0D, "up": 0x26, "left": 0x25, "right": 0x27}


def key_calls(keyspec: str) -> dict:
    """KEYSPEC "down@100 down@160 enter@220 esc@280" -> {frame: [(name, args)]} (consult §4.1: a tap is state 2 on
    frame f and state 1 on f+1; Esc goes through the key mapper id 251 with 3 arguments)."""
    out: dict[int, list] = {}
    for tok in keyspec.replace(",", " ").split():
        k, f = tok.split("@")
        f = int(f)
        for df, state in ((0, 2), (1, 1)):
            if k == "esc":
                args = [Variant(T_INT, C1), Variant(T_INT, 251), Variant(T_INT, state)]
            else:
                args = [Variant(T_INT, C1), Variant(T_INT, KEY_VK[k]), Variant(T_INT, state),
                        Variant(T_FLOAT, 0), Variant(T_FLOAT, 0)]
            out.setdefault(f + df, []).append(("Input", args))
    return out


def run_base(a) -> int:
    script = Path(a.run).read_bytes()
    types = TypeTable.load(a.types)
    reg = NativeRegistry.load(a.natives, a.mods, types)
    if a.overrides:
        apply_overrides(reg, a.overrides)
    if a.replay:
        reg.replay_from(a.replay)
    else:
        reg.return_log = []
    if a.digest:
        trace = DigestTrace(a.digest)
    elif a.trace:
        trace = FileTrace(a.trace)
    else:
        trace = None
    extra = " keys=%s" % a.keys.replace(" ", ",") if a.keys else ""
    if trace is not None:
        trace.line(header_line(script, a.natives, a.mods, a.types, extra))
    heap = Heap(trace)
    vm = Vm(script, reg, heap, trace)
    if a.real_natives:                    # the models of the registered groups (Tasks 7b-9b); the rest stay stubs (D11)
        from osi_natives import framework
        vfs = framework.Vfs(a.install) if a.install else None
        ctx = framework.HostContext(vm, heap, vfs, framework.HostFlags(), reg.return_log)
        framework.bind_natives(reg, ctx)
    vm.pcs_seen = set()
    stop = "none"
    per_frame = []
    pass_counts: list[int] = []
    pm_flag, proc_byte = 0, 1     # registered so that it runs on the first frame (the shipped script runs every frame)
    keys = key_calls(a.keys) if a.keys else {}
    dt = struct.unpack("<d", struct.pack("<Q", DT_BITS_DOUBLE))[0]
    time = 0.0
    try:
        r = vm.call("Setup", [])
        per_frame.append(("setup", vm.instructions, vm.native_calls, len(vm.pcs_seen)))
        if not r.ok:
            stop = 'error "%s" in Setup at pc %08x op %02x' % (vm.last_error.message, vm.last_error.pc, vm.last_error.op)
        else:
            for k in range(a.frames):
                time = float(round_p(Fraction(time) + Fraction(dt), vm.fpu_pc))     # fadd qword at the x87 setting
                tb, db = f32_bits(time), f32_bits(dt)
                if trace is not None:
                    trace.line("K %6d %08x %08x %08x" % (k, tb, db, k))
                vm.begin_frame(k)                 # natives.log `K %6d` before anything else of the frame (or its replay check)
                i0, n0 = vm.instructions, vm.native_calls
                vm.pcs_seen = set()
                err = None
                for name, args in keys.get(k, []):
                    r = vm.call(name, args)
                    if not r.ok:
                        err = vm.last_error
                        break
                # PM+0x40 parity (0x4a92fd-0x4a930a, toggled every frame after the time update): the first pass runs the
                # process only if its byte at +8 equals the flag (0x4a937d; +9 'paused' is never set for it, 0x4a9383);
                # a pass that returns true writes +8 = NOT flag (0x4a93b7-0x4a93c1, re-runs 0x4a94a6-0x4a94bb). A process
                # still false after the 10th re-run keeps +8 == this frame's flag, so it is skipped on the next frame
                # (no C Process line) and runs again the frame after. The time, the K line, the key calls (separate
                # engine calls) and the counter PM+0x44 (0x4a95b6) are unconditional.
                pm_flag ^= 1
                passes = 0
                eligible = proc_byte == pm_flag
                while err is None and eligible:
                    # ScSLOSIProcess::Process (0x43b33f-0x43b35f) returns false only for an int 0 result (nothing and
                    # every other type -> true). ScProcessManager::Process re-runs the processes that returned false
                    # with the same time, dt and counter, at most 10 more times (0x4a930d counter 0; 0x4a955b inc;
                    # 0x4a957d cmp 0xa / jb); the counter PM+0x44 is incremented only at frame end (0x4a95b6).
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
                pass_counts.append(passes)
                per_frame.append((k, vm.instructions - i0, vm.native_calls - n0, len(vm.pcs_seen)))
                if err is not None:
                    stop = 'error "%s" at frame %d pc %08x op %02x' % (err.message, k, err.pc, err.op)
                    break
        if stop == "none":                        # only check log consumption on a normal completion
            reg.replay_finish()                   # a replay must consume the whole log (no-op without --replay)
    except ReplayDesync as e:
        stop = "replay desync: %s" % e
    finally:
        if trace is not None:
            trace.close()
    if reg.return_log is not None and a.log_out:
        Path(a.log_out).write_text("".join(l + "\n" for l in reg.return_log), encoding="ascii")
    frames = [p for p in per_frame if p[0] != "setup"]
    ins = sorted(p[1] for p in frames)
    p50 = ins[len(ins) // 2] if ins else 0
    p95 = ins[min(len(ins) - 1, int(len(ins) * 0.95))] if ins else 0
    setup = per_frame[0] if per_frame else ("setup", 0, 0, 0)
    print("setup: instructions %d, natives %d, distinct_pcs %d" % setup[1:])
    print("frames %d (of %d requested), instructions %d, natives %d, insns/frame p50 %d p95 %d (n=%d)"
          % (len(frames), a.frames, vm.instructions, vm.native_calls, p50, p95, len(ins)))
    if frames:
        print("distinct_pcs_by_frame (first 5, last): %s ... %s"
              % (" ".join(str(p[3]) for p in frames[:5]), frames[-1][3]))
    rerun = [(i, n) for i, n in enumerate(pass_counts) if n > 1]
    skipped = [i for i, n in enumerate(pass_counts) if n == 0]
    print("process passes: total %d over %d frames, max %d, frames with re-runs %d%s, frames skipped %d%s"
          % (sum(pass_counts), len(pass_counts), max(pass_counts, default=0), len(rerun),
             (" (first: %s)" % " ".join("%d:%d" % x for x in rerun[:5])) if rerun else "", len(skipped),
             (" (first: %s)" % " ".join(str(i) for i in skipped[:5])) if skipped else ""))
    print("engine-call nesting: max %d (cap %d)" % (vm.max_nesting, MAX_ENGINE_NESTING))
    print("stop: %s" % stop)
    print("script sha256 unchanged: %s" % (hashlib.sha256(vm.image).hexdigest() == hashlib.sha256(script).hexdigest()))
    return 1 if stop.startswith("replay desync") else 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="LOMN OSI VM oracle (M2a-1 T2)")
    ap.add_argument("--conform", metavar="DIR")
    ap.add_argument("--list-keys", action="store_true", help="print the compared and ignored expectation key paths")
    ap.add_argument("--trace-dir")
    ap.add_argument("--run", metavar="BASE_OSI")
    ap.add_argument("--natives", default=str(DEFAULT_NATIVES))
    ap.add_argument("--mods", default=str(DEFAULT_MODS))
    ap.add_argument("--types", default=str(DEFAULT_TYPES))
    ap.add_argument("--frames", type=int, default=600)
    ap.add_argument("--keys", default="")
    ap.add_argument("--replay")
    ap.add_argument("--trace")
    ap.add_argument("--digest")
    ap.add_argument("--log-out", help="write the natives.log (v2, natives.md §3) of this run")
    ap.add_argument("--real-natives", action="store_true",
                    help="bind the registered native models (osi_natives groups); the other rows stay typed stubs")
    ap.add_argument("--install", help="the install directory the native models read (Root/ of the VFS)")
    ap.add_argument("--overrides", help="apply a native_overrides.json (tier, default) at load (natives.md §7.2)")
    a = ap.parse_args(argv)
    if a.list_keys:
        for kind, keys in (("compared", COMPARED_KEYS), ("ignored", IGNORED_KEYS)):
            for k in sorted(keys):
                print("%s %s" % (kind, k))
        return 0
    if a.conform:
        return conform(a.conform, a.trace_dir)
    if a.run:
        return run_base(a)
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
