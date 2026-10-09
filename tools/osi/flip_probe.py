"""M2a-2 T1: the flip probe. Which natives' return values change what the script does?

  flip_probe.py --osi BASE.OSI --frames N [--natives-impl real|stub] [--deadline SECONDS]

Method (plan "Measured facts", the flip row):
  1. A baseline run of Setup + N `Process` frames with every native a typed stub (D11) and the Beta 1.5.0 mod rows
     (D12), in the Python oracle (tools/research/osi_vm.py). It records the executed-PC set, the instruction total and
     the distinct natives in first-hit order.
  2. For each distinct native, the same run with that one native's return replaced, at every call site, by one fixed
     alternative: int or none -> int 1; float -> 1.0; string -> "x"; colour -> colour 1; copy -> int 1; game -> nothing.
  3. CHANGED when the executed-PC set or the instruction total differs from the baseline, else SAME.
     CHANGED-NULL (C2) is a CHANGED verdict of a game-returning native whose alternative run ENDED IN A 0x82 that
     rejected the flipped nothing: its only tested alternative is nothing, which the next native's `type:<Class>`
     argument conversion refuses (Setup, or a frame, ends), so the probe cannot say what the real object would change.
     An error anywhere else (an arithmetic op on the nothing, say) stays CHANGED with the error in <stop>. Such a native must be real (tier 1) in
     M2a-2. A game-returning native whose nothing did change the path without an error (gctimer.create,
     gcgame.getmp3player on the real script) stays CHANGED: the path moved by value.
Each run has its own wall-clock deadline (default 20 s); a run that reaches it stops with `deadline`.

Output, one line per distinct native in first-hit order (stdout), and a summary on stderr:
  SAME|CHANGED|CHANGED-NULL <key> <delta instructions, %+d> <stop>
`<stop>` is `none`, `deadline`, or `error "<message>" in Setup at pc %08x op %02x` /
`error "<message>" at frame N pc %08x op %02x`.

--natives-impl real needs the Python native models (tools/research/osi_natives, M2a-2 Task 4) and exits with an
error until they exist; `stub` (default) is the typed-stub run above.
"""
from __future__ import annotations

import argparse
import struct
import sys
import time
from fractions import Fraction
from pathlib import Path
from typing import NamedTuple, Optional

REPO = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(REPO / "tools" / "research"), str(REPO / "tools" / "osi")]
import osi_vm  # noqa: E402
from osi_vm import Variant, NOTHING, T_INT, T_FLOAT, T_STRING, T_COLOUR  # noqa: E402

DATA = REPO / "data" / "osi"

# ret class -> the one alternative that is tried (None = nothing)
ALTERNATIVE = {
    "int": ("int", 1), "none": ("int", 1), "copy": ("int", 1), "float": ("float", 1.0), "string": ("string", "x"),
    "colour": ("colour", 1), "game": None,
}


class Deadline(BaseException):
    """Raised from inside the VM loop; a BaseException so that no handler in the oracle can swallow it."""


class PcSet(set):
    """The executed-PC set that also checks the clock (every 2,048 instructions)."""

    def __init__(self, deadline: Optional[float]):
        super().__init__()
        self.deadline, self.n = deadline, 0

    def add(self, pc):
        set.add(self, pc)
        self.n += 1
        if self.deadline is not None and not self.n & 2047 and time.monotonic() > self.deadline:
            raise Deadline()


class ProbeRegistry(osi_vm.NativeRegistry):
    """Records the natives in first-hit order and, for `flip_key`, answers with the alternative."""

    def __init__(self, types):
        super().__init__(types)
        self.hit: list[str] = []
        self.rets: dict[str, str] = {}
        self.flip_key: Optional[str] = None

    def invoke(self, call):
        e = call.entry
        if e.key not in self.rets:
            self.rets[e.key] = e.ret
            self.hit.append(e.key)
        if e.key == self.flip_key:
            alt = ALTERNATIVE[e.ret]
            heap = call.vm.heap
            if alt is None:
                return NOTHING
            kind, v = alt
            if kind == "int":
                return Variant(T_INT, v)
            if kind == "float":
                return Variant(T_FLOAT, osi_vm.f32_bits(v))
            if kind == "colour":
                return Variant(T_COLOUR, v)
            return Variant(T_STRING, heap.new_string(v.encode("latin-1")))
        return super().invoke(call)


class Run(NamedTuple):
    setup_instructions: int
    instructions: int
    pcs: frozenset
    natives: list
    rets: dict
    stop: str
    null_rejected: bool = False     # the run ended in a 0x82 whose type:<Class> argument was the flipped nothing


def _rejects_nothing(vm, reg, err) -> bool:
    """The error `err` was raised by a 0x82 CallGameFunction one of whose game-typed arguments is a nothing still on the
    stack (an error leaves the arguments in place, opcodes.md 0x82): the next native consuming the flipped value refused
    it. The baseline run is clean, so a nothing in a game-typed argument slot comes from the flipped return."""
    if err.op != 0x82:
        return False
    pc = err.pc
    entry = reg.find(vm.script.string_text(vm._u16(pc + 1)), vm.script.string_text(vm._u16(pc + 3)))
    if entry is None:
        return False
    n = vm._u8(pc + 5)
    base = vm.depth - n
    return any(osi_vm.is_game(entry.types[i]) and 0 <= base + i < len(vm.stack) and vm.stack[base + i].type == osi_vm.T_NOTHING
               for i in range(min(n, len(entry.types))))


def run_once(script: bytes, natives, mods, types, frames: int, deadline: float, flip_key: Optional[str] = None) -> Run:
    """Setup + `frames` Process frames, no keys: the frame loop of osi_vm.run_base (ScProcessManager parity 0x4a92fd,
    the re-run rule 0x4a930d..0x4a957d), copied here because run_base is a print-only driver. The test
    `test_flip_probe_real_driver_matches_run_base` compares the instruction totals of the two."""
    tt = osi_vm.TypeTable.load(types)
    reg = ProbeRegistry.load(natives, mods, tt)
    reg.flip_key = flip_key
    heap = osi_vm.Heap(None)
    vm = osi_vm.Vm(script, reg, heap, None)
    vm.pcs_seen = PcSet(time.monotonic() + deadline)
    stop = "none"
    rejected = False
    setup_insns = 0
    dt = struct.unpack("<d", struct.pack("<Q", osi_vm.DT_BITS_DOUBLE))[0]
    t = 0.0
    pm_flag, proc_byte = 0, 1
    try:
        r = vm.call("Setup", [])
        setup_insns = vm.instructions
        if not r.ok:
            stop = 'error "%s" in Setup at pc %08x op %02x' % (vm.last_error.message, vm.last_error.pc, vm.last_error.op)
            rejected = _rejects_nothing(vm, reg, vm.last_error)
        else:
            for k in range(frames):
                t = float(osi_vm.round_p(Fraction(t) + Fraction(dt), vm.fpu_pc))
                tb, db = osi_vm.f32_bits(t), osi_vm.f32_bits(dt)
                pm_flag ^= 1
                passes = 0
                err = None
                while proc_byte == pm_flag:
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
                if err is not None:
                    stop = 'error "%s" at frame %d pc %08x op %02x' % (err.message, k, err.pc, err.op)
                    rejected = _rejects_nothing(vm, reg, err)
                    break
    except Deadline:
        stop = "deadline"
    return Run(setup_insns, vm.instructions, frozenset(vm.pcs_seen), list(reg.hit), dict(reg.rets), stop, rejected)


class Result(NamedTuple):
    verdict: str
    key: str
    delta: int
    stop: str


def probe(script: bytes, natives, mods, types, frames: int, deadline: float = 20.0) -> list[Result]:
    base = run_once(script, natives, mods, types, frames, deadline)
    if base.stop != "none":
        raise RuntimeError("the baseline run does not finish cleanly: %s" % base.stop)
    out = []
    for key in base.natives:
        r = run_once(script, natives, mods, types, frames, deadline, flip_key=key)
        changed = r.pcs != base.pcs or r.instructions != base.instructions
        if not changed:
            verdict = "SAME"
        else:
            verdict = "CHANGED-NULL" if base.rets[key] == "game" and r.null_rejected else "CHANGED"
        out.append(Result(verdict, key, r.instructions - base.instructions, r.stop))
    return out


def format_line(r: Result) -> str:
    return "%s %s %+d %s" % (r.verdict, r.key, r.delta, r.stop)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--osi", required=True)
    ap.add_argument("--frames", type=int, required=True)
    ap.add_argument("--natives-impl", choices=("real", "stub"), default="stub")
    ap.add_argument("--deadline", type=float, default=20.0, help="seconds per run")
    ap.add_argument("--natives", default=str(DATA / "natives.json"))
    ap.add_argument("--mods", default=str(DATA / "natives_beta150.json"))
    ap.add_argument("--types", default=str(DATA / "types.json"))
    a = ap.parse_args(argv)
    if a.natives_impl == "real":
        raise SystemExit("flip_probe.py: --natives-impl real needs tools/research/osi_natives (M2a-2 Task 4); "
                         "it does not exist yet")
    script = Path(a.osi).read_bytes()
    t0 = time.monotonic()
    res = probe(script, a.natives, a.mods, a.types, a.frames, a.deadline)
    for r in res:
        print(format_line(r))
    counts = {}
    for r in res:
        counts[r.verdict] = counts.get(r.verdict, 0) + 1
    print("flip_probe: %d natives, %s, frames %d, %.1f s" % (
        len(res), ", ".join("%s %d" % (k, counts[k]) for k in sorted(counts)), a.frames, time.monotonic() - t0),
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
