"""M2a-2 T2a: the host-OnEvent injection probe. Does an engine -> script `OnEvent` from the Toa's process path change
what the script does on the front-end?

  event_probe.py --osi BASE.OSI [--frames 600] [--at 0,1,10,100,599] [--keys KEYSPEC] [--deadline SECONDS]
                 [--natives-impl stub|real]

Why (task-2a report, Decision 8): the Toa is a registered process (GcToa ctor, AddProcess 0x568262) and its
PreProcess runs from the GcBAGroup process (0x54acca), every frame on the front-end, gated only by state Setup sets.
Below them sit the 42 `OnEvent` call sites in SITES, each guarded by physics or animation state that the asm alone
cannot decide. Instead of modelling the Toa, this probe measures the effect of each event on the script.

Method:
  1. A baseline run, as osi_vm.run_base: Setup, then `frames` frames of [key Input calls, Process passes with the
     ScProcessManager parity and re-run rules]. Stub natives (D11) and the Beta 1.5.0 mod rows unless --natives-impl
     real (Task 4's models; Task 10 re-runs the probe that way).
  2. Every distinct event tuple of SITES (see `expand`), injected as `vm.call("OnEvent", [id, args..., nothing...])`
     at each frame of --at, after that frame's key calls and before its Process passes. The original's order between
     the Toa / BA-group processes and ScSLOSIProcess is the process map's key order (ScProcessManager::Process
     0x4a9170), which is not resolved here; before Process is the earlier, stronger choice. INFERRED.
  3. The run after the injection is compared with the baseline's: the executed-PC sequence and the native calls
     (key and argument values; heap handles compared by type only), the injected call's own instructions excluded.
     SAME: identical. CHANGED: different. ERROR: the injected call, or any later call, raised (a host-originated
     OnEvent error ends the original game: opcodes.md "After an error").

Output (stdout), one line per injection, then a summary on stderr:
  SAME|CHANGED|ERROR <id> (<args>) <frame> <delta instructions, %+d> <first divergence>
`<first divergence>` is `-`, `frame N` (the first frame whose PC or native sequence differs), or for ERROR
`error "<message>" at pc %08x op %02x` (where the run stopped).
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
from osi_vm import Variant, NOTHING, T_INT, T_FLOAT  # noqa: E402

DATA = REPO / "data" / "osi"
A9_KEYS = "down@100 down@160 enter@220 esc@280"
CNT1 = 0x636E7431                   # 'cnt1': the Toa's model-director key [0x782884] (0x482d43), MainCharacterID default


class Site(NamedTuple):
    """One `ScOSIVirtualMachine::Call(OnEvent, ...)` site under GcToa::Process / GcToa::PreProcess. `args` are a1.. (a0
    is `event`, CreateVariant 0x413ed0 with an immediate). Templates: ("i", v) a constant int; ("f", v) a constant
    float; ("id?",) an identifier computed at run time; ("i?",) an int computed at run time; ("f?",) a computed float.
    The remaining arguments up to 10 are nothing ({0x0f, 0} stored literally before each call)."""
    function: str
    call: int
    event: int
    args: tuple


F10K = ("f", 10000.0)               # the float at 0x73caa0 / 0x73d5f4 / 0x73db90 / 0x73dc30 / 0x73dc48 / 0x73de5c
HELL, CRSH, IMPC, WATR = 0x68656C6C, 0x63727368, 0x696D7063, 0x77617472   # identifier immediates ('hell' ...)
ID, I, FL = ("id?",), ("i?",), ("f?",)

# EXECUTED: objdump scan of every Call site in the closure of GcToa::Process / GcToa::PreProcess (task-2a scratch
# `args.py`), each argument traced to its CreateVariant overload and value source.
SITES = [
    Site("GcCharacter::Process", 0x41346D, 0x23, (ID, ("i", 1))),
    Site("GcCharacter::Process", 0x4135D8, 0x23, (ID, ("i", 0))),
    Site("GcCharacter::Process", 0x413836, 0x0D, (ID, I)),
    Site("GcCharacterMoveInterpreter::CheckForNewAnim", 0x5094FD, 0x0B, (ID, I)),
    Site("GcCharacterMoveInterpreter::ProcessAnimationFlags", 0x5097C1, 0x0C, (ID,)),
    Site("GcToa::Process", 0x56959F, 0x27, (ID,)),
    Site("GcMotionSystem::ProcessSlideTimer", 0x575743, 0x22, (ID,)),
    Site("GcMotionSystem::WorldDoubleCheck", 0x5760C8, 0x11, (ID, FL, FL, FL, FL, I)),
    Site("GcMotionSystem::PostProcess", 0x57675F, 0x2E, ()),
    Site("GcMotionSystem::PostProcess", 0x576862, 0x28, (F10K, ("i", HELL))),
    Site("GcMotionSystem::PostProcess", 0x57692D, 0x1F, (ID,)),
    Site("GcMotionSystem::ProcessConstantForwardVelocity", 0x577E0B, 0x36, (("i", 1),)),
    Site("GcMotionSystem::ProcessConstantForwardVelocity", 0x577F9F, 0x36, (("i", 0),)),
    Site("GcMotionSystem::ProcessGravity", 0x578F94, 0x36, (("i", 0),)),
    Site("GcMotionSystem::ProcessPhysics", 0x57CD00, 0x36, (("i", 1),)),
    Site("GcMotionSystem::ProcessPhysics", 0x57D011, 0x36, (("i", 0),)),
    Site("GcMotionSystem::CastWaterRay", 0x57D8AF, 0x36, (("i", 0),)),
    Site("GcMotionSystem::CastWaterRay", 0x57D9E3, 0x36, (("i", 1),)),
    Site("GcMotionSystem::CastWaterRay", 0x57DC17, 0x36, (("i", 0),)),
    Site("GcMotionSystem::CastWaterRay", 0x57DD6B, 0x36, (("i", 1),)),
    Site("GcMotionSystem::CastWaterRay", 0x57E051, 0x36, (("i", 1),)),
    Site("GcMotionSystem::CastWaterRay", 0x57E33C, 0x36, (("i", 0),)),
    Site("GcMotionSystem::CastWaterRay", 0x57E75B, 0x0F, (("i", WATR),)),
    Site("GcMotionSystem::CastWaterRay", 0x57E9D5, 0x2A, (FL, FL, FL)),
    Site("GcMotionSystem::SwimHitSurface", 0x57EEF6, 0x36, (("i", 0),)),
    Site("GcMotionSystem::SwimHitSurface", 0x57F088, 0x36, (("i", 0),)),
    Site("GcMotionSystem::DoObjectFall", 0x5804D0, 0x28, (F10K, ("i", CRSH))),
    Site("GcMotionSystem::DoObjectFall", 0x5810C4, 0x2D, (FL,)),
    Site("GcMotionSystem::DoObjectFall", 0x58186A, 0x11, (ID, FL, FL, FL, FL, I)),
    Site("GcMotionSystem::DoObjectFall", 0x581C04, 0x0F, (ID,)),
    Site("GcMotionSystem::DoObjectRoll", 0x584631, 0x11, (ID, FL, FL, FL, FL, I)),
    Site("GcMotionSystem::DoObjectRoll", 0x584AD3, 0x0F, (ID,)),
    Site("GcMotionSystem::NudgeTowardTarget", 0x585058, 0x10, (ID,)),
    Site("GcMotionSystem::DoCharacterSwim", 0x5859BD, 0x36, (("i", 0),)),
    Site("GcMotionSystem::DoCharacterSwim", 0x585C34, 0x36, (("i", 1),)),
    Site("GcMotionSystem::DoCharacterSwim", 0x585FA7, 0x2F, (("i", 0),)),
    Site("GcMotionSystem::DoCharacterSwim", 0x586116, 0x2F, (I,)),
    Site("GcMotionSystem::CheckCrushed1", 0x586408, 0x28, (F10K, ("i", CRSH))),
    Site("GcMotionSystem::CheckWallCloseness", 0x587240, 0x28, (("i", 2), ("i", IMPC))),
    Site("GcMotionSystem::CheckWallCloseness", 0x58738B, 0x28, (F10K, ("i", IMPC))),
    Site("GcMotionSystem::CheckWallCloseness", 0x587628, 0x28, (F10K, ("i", CRSH))),
    Site("GcMotionSystem::HellCheck", 0x589F17, 0x28, (F10K, ("i", HELL))),
]

REPS = {"id?": [CNT1, 0, 1], "i?": [0, 1], "f?": [("f", 0.0), ("f", 1.0)]}


def expand(site: Site) -> list:
    """The event tuples one site stands for: one vector per representative index k (the k-th value of every computed
    argument's list, the last value when a list is shorter); a site with no computed argument gives one tuple."""
    computed = [a[0] for a in site.args if a[0].endswith("?")]
    n = max((len(REPS[c]) for c in computed), default=1)
    out = []
    for k in range(n):
        vals = []
        for a in site.args:
            if a[0].endswith("?"):
                lst = REPS[a[0]]
                vals.append(lst[min(k, len(lst) - 1)])
            elif a[0] == "i":
                vals.append(a[1])
            else:
                vals.append(("f", a[1]))
        t = (site.event, tuple(vals))
        if t not in out:
            out.append(t)
    return out


def all_tuples() -> list:
    out = []
    for s in SITES:
        for t in expand(s):
            if t not in out:
                out.append(t)
    return out


def _variant(v) -> Variant:
    if isinstance(v, tuple) and v[0] == "f":
        return Variant(T_FLOAT, osi_vm.f32_bits(v[1]))
    return Variant(T_INT, v & 0xFFFFFFFF)


def fmt_args(args) -> str:
    def one(v):
        if isinstance(v, tuple):
            return repr(float(v[1]))
        return "0x%x" % v if v > 9 else str(v)
    return "(" + ",".join(one(v) for v in args) + ")"


class Recorder:
    """vm.pcs_seen stand-in: appends each executed pc to the current frame's list unless an injection is running."""

    def __init__(self, deadline):
        self.frames: list[list] = [[]]
        self.injecting = False
        self.injected = 0
        self.deadline = deadline
        self.n = 0

    def add(self, pc):
        self.n += 1
        if self.deadline is not None and not self.n & 4095 and time.monotonic() > self.deadline:
            raise Deadline()
        if self.injecting:
            self.injected += 1
        else:
            self.frames[-1].append(pc)


class Deadline(BaseException):
    pass


class ProbeRegistry(osi_vm.NativeRegistry):
    """Records (key, argument values) per frame; heap handles are compared by type only, since an injected call may
    allocate and shift the handle counters (D7) without changing the path."""

    rec: Optional[Recorder] = None

    def invoke(self, call):
        if self.rec is not None and not self.rec.injecting:
            vals = []
            for i in range(len(call.slots)):
                v = call.arg(i)
                vals.append((v.type,) if osi_vm.is_game(v.type) or v.type in (0x01, 0x13, 0x16) else (v.type, v.value))
            self.rec.frames[-1].append((call.entry.key, tuple(vals)))
        return super().invoke(call)


class Outcome(NamedTuple):
    frames: list            # per frame (index 0 = Setup), the executed pcs and native records
    stop: str
    injected: int
    inject_error: str


def run(script, natives, mods, types, frames, keys, deadline, inject=None, natives_impl="stub") -> Outcome:
    """inject = (frame, event, args) or None."""
    tt = osi_vm.TypeTable.load(types)
    if natives_impl == "real":
        raise SystemExit("event_probe.py: --natives-impl real needs tools/research/osi_natives (M2a-2 Task 4)")
    reg = ProbeRegistry.load(natives, mods, tt)
    rec = Recorder(time.monotonic() + deadline if deadline else None)
    reg.rec = rec
    heap = osi_vm.Heap(None)
    vm = osi_vm.Vm(script, reg, heap, None)
    vm.pcs_seen = rec
    kc = osi_vm.key_calls(keys) if keys else {}
    stop, inject_error = "none", ""
    dt = struct.unpack("<d", struct.pack("<Q", osi_vm.DT_BITS_DOUBLE))[0]
    t = 0.0
    pm_flag, proc_byte = 0, 1

    def where(e):
        return 'error "%s" at pc %08x op %02x' % (e.message, e.pc, e.op)

    try:
        r = vm.call("Setup", [])
        if not r.ok:
            return Outcome(rec.frames, "Setup " + where(vm.last_error), 0, "")
        for k in range(frames):
            rec.frames.append([])
            t = float(osi_vm.round_p(Fraction(t) + Fraction(dt), vm.fpu_pc))
            tb, db = osi_vm.f32_bits(t), osi_vm.f32_bits(dt)
            err = None
            for name, args in kc.get(k, []):
                r = vm.call(name, args)
                if not r.ok:
                    err = vm.last_error
                    break
            if err is None and inject is not None and inject[0] == k:
                ev, args = inject[1], inject[2]
                vs = [Variant(T_INT, ev)] + [_variant(a) for a in args]
                vs += [NOTHING] * (10 - len(vs))
                rec.injecting = True
                try:
                    r = vm.call("OnEvent", vs)
                finally:
                    rec.injecting = False
                if not r.ok:
                    inject_error = where(vm.last_error)
                    return Outcome(rec.frames, "frame %d injected OnEvent %s" % (k, inject_error), rec.injected,
                                   inject_error)
            pm_flag ^= 1
            passes = 0
            while err is None and proc_byte == pm_flag:
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
                stop = "frame %d %s" % (k, where(err))
                break
    except Deadline:
        stop = "deadline"
    return Outcome(rec.frames, stop, rec.injected, inject_error)


class Result(NamedTuple):
    verdict: str
    event: int
    args: tuple
    frame: int
    delta: int
    divergence: str
    injected: int


def _insns(frames) -> int:
    return sum(1 for fr in frames for x in fr if isinstance(x, int))


def compare(base: Outcome, inj: Outcome, event, args, frame) -> Result:
    if inj.inject_error or inj.stop != "none":
        # delta over the frames the stopped run completed (its last, interrupted frame excluded)
        done = len(inj.frames) - 1
        delta = _insns(inj.frames[:done]) - _insns(base.frames[:done])
        return Result("ERROR", event, args, frame, delta, inj.stop if not inj.inject_error else inj.inject_error,
                      inj.injected)
    delta = _insns(inj.frames) - _insns(base.frames)
    div = "-"
    for i in range(max(len(base.frames), len(inj.frames))):
        a = base.frames[i] if i < len(base.frames) else None
        b = inj.frames[i] if i < len(inj.frames) else None
        if a != b:
            div = "setup" if i == 0 else "frame %d" % (i - 1)
            break
    return Result("SAME" if div == "-" else "CHANGED", event, args, frame, delta, div, inj.injected)


def probe(script, natives, mods, types, frames=600, inject_frames=(0, 1, 10, 100, 599), tuples=None,
          keys=A9_KEYS, deadline=60.0, natives_impl="stub") -> list:
    base = run(script, natives, mods, types, frames, keys, deadline, None, natives_impl)
    if base.stop != "none":
        raise RuntimeError("the baseline run does not finish cleanly: %s" % base.stop)
    tuples = all_tuples() if tuples is None else tuples
    out = []
    for ev, args in tuples:
        for f in inject_frames:
            r = run(script, natives, mods, types, frames, keys, deadline, (f, ev, args), natives_impl)
            out.append(compare(base, r, ev, args, f))
    return out


def format_line(r: Result) -> str:
    return "%s 0x%02x %s %d %+d %s" % (r.verdict, r.event, fmt_args(r.args), r.frame, r.delta, r.divergence)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--osi", required=True)
    ap.add_argument("--frames", type=int, default=600)
    ap.add_argument("--at", default="0,1,10,100,599")
    ap.add_argument("--keys", default=A9_KEYS)
    ap.add_argument("--deadline", type=float, default=60.0, help="seconds per run")
    ap.add_argument("--natives-impl", choices=("stub", "real"), default="stub")
    ap.add_argument("--natives", default=str(DATA / "natives.json"))
    ap.add_argument("--mods", default=str(DATA / "natives_beta150.json"))
    ap.add_argument("--types", default=str(DATA / "types.json"))
    a = ap.parse_args(argv)
    t0 = time.monotonic()
    res = probe(Path(a.osi).read_bytes(), a.natives, a.mods, a.types, a.frames,
                [int(x) for x in a.at.split(",")], None, a.keys, a.deadline, a.natives_impl)
    for r in res:
        print(format_line(r))
    counts = {}
    for r in res:
        counts[r.verdict] = counts.get(r.verdict, 0) + 1
    print("event_probe: %d sites, %d tuples, %d injections, %s, frames %d, %.1f s" % (
        len(SITES), len(all_tuples()), len(res), ", ".join("%s %d" % (k, counts[k]) for k in sorted(counts)),
        a.frames, time.monotonic() - t0), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
