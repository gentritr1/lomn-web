"""M2a-3 T4b: the host sources of the Python oracle: the camera process (spline follow, the OnEvent(6) guard), the mouse
driver's calls, the frame order, `C -1` records and the `Z` snapshot. Written from docs/osi/hud.md (§1 camera, §2 frame order,
§5 and §11 input, §9 `Z`) and docs/osi/trace.md (§2, §6 "Version 3"); the exe was read as data (objdump) only where a
citation below says so. Never from engine/.

What is modelled (everything INFERRED is marked at its place):
- `Spline` / `SplineFile`: ScCubicSpline<float,float>::Accelerate 0x4435f0 (Numerical Recipes `spline()`, natural ends) and
  Interpolate 0x443a70 (`splint()`), every x87 operation rounded to the precision control (24 or 53 bits) and every f32 store
  (y2, u, the parameter, the result) rounded to f32 (hud.md §1.5 "Pins", reproduced to the bit by tests/test_osi_host_sources.py).
- The camera natives' state (`CamState`): index bytes, duration, start time, flags, eye and target, written by the natives
  exactly as SetCameraSpline 0x45b380 / SetTargetSpline 0x45b400 / SetCameraAIMode 0x45b490 write them (the reference part stays
  in osi_natives.objects, which these wrappers call first).
- `camera_step`: GcLegoCamera::Process 0x451990, the spline-following modes 2 and 3 and their `OnEvent(6)` guards (§1.3).
- `FrameHost`: the order of one frame (§2.4: VK triggers, key mapper, mouse driver, the script's Process passes, the camera),
  the `K` line, `C -1` for the mouse and the camera (trace.md §6), the end-of-frame `Z` block (§9).
- `scslosimath.abs`, `gclegocamera.setcampos` / `settargpos` bodies (the camera eye and target are `Z` lines).

Not modelled: the camera's other modes (no OnEvent(6), no spline), `addyaw` (it rotates only +0x174, which no `Z` line shows,
hud.md §1.7), the HUD lines of `Z` (Task 6b), the spline's first-derivative table (GetFirstDerivative is not on any path)."""
from __future__ import annotations

import struct
from fractions import Fraction
from pathlib import Path
from typing import NamedTuple, Optional

import osi_vm
from osi_vm import NOTHING, T_FLOAT, T_INT, Variant, VmError, exact_round_f32, f32_bits
from expect_calc import round_p

from . import framework
from .framework import register_group

TIER1_KEYS = ["scslosimath.abs", "gclegocamera.setcampos", "gclegocamera.settargpos"]

CITES = {
    "scslosimath.abs":
        "// body 0x618290, method 0x657d46 <fabs>; asm pin: 0x61829b fld dword [arg], 0x6182a3 fstp qword (the double goes "
        "to the CRT fabs), 0x6182af fstp dword (the f32 store), 0x6182c5 CreateVariant 0x4077f0 (float)",
    "gclegocamera.setcampos":
        "// body 0x44e8a0, method 0x405e50 <GcLegoCamera::SetCameraPosition>; asm pin: 0x44e8be-0x44e8c6 the three float "
        "arguments, 0x44e8ce-0x44e8d8 fstp dword (f32 stores), 0x44e8e3 call SetCameraPosition, 0x44e8e8 returns nothing {0xf,0}",
    "gclegocamera.settargpos":
        "// body 0x44e900, method 0x405f00 <GcLegoCamera::SetTargetPosition>; asm pin: the same shape as setcampos, 0x44e943 "
        "call SetTargetPosition",
    # the behaviours that are not natives (no body): hud.md section, then the addresses
    "mouse.driver":
        "// method 0x55c160 <GcMouseDriver::Process>; asm pin: 0x55c172 device [0x73a0cc] null -> no event, 0x55c753 "
        "OnEvent(26, x, y) through Call 0x60b320 with the name at 0x73a11c, then the button events 23 / 24 / 25 in order 1..4",
    "frame.order":
        "// method 0x4a9170 <ScProcessManager::Process>; asm pin: AddProcess 0x4a8f50 key = the priority byte (0x4a8f60), "
        "priorities VK triggers 0 (0x43949b), mouse driver 1 (0x439551), key mapper 1 (0x4395ae), script 2 (0x4392ef), "
        "viewport camera node 3 (0x447f4a); ascending priority, the latest added first within one",
    "camera.process":
        "// method 0x451990 <GcLegoCamera::Process>; asm pin: 0x452a26 mode 3, 0x452a33 forward flag, 0x452a40 elapsed = time - "
        "[cam+0x3b0], 0x452a55 clamp to [cam+0x3a8], 0x452a74 param = f32(elapsed / duration), 0x452bfa fire when elapsed >= "
        "duration, 0x452c0b start = time, 0x452d24 OnEvent(6, camera, 0)",
    "camera.setcameraspline":
        "// method 0x45b380 <GcLegoCamera::SetCameraSpline>; asm pin: 0x45b3c6 index (dword) to +0x3b8, 0x45b3cf duration "
        "(double of the f32) to +0x3a8, 0x45b3d5 SrGetSeconds(0) to +0x3b0, 0x45b3e6 / 0x45b3ef flag bytes to +0x460 / +0x461",
    "camera.settargetspline":
        "// method 0x45b400 <GcLegoCamera::SetTargetSpline>; asm pin: 0x45b443 the first index to +0x3b8, 0x45b44c the second "
        "to +0x3bc, 0x45b455 duration to +0x3a8, 0x45b45e SrGetSeconds(0) to +0x3b0, 0x45b46f / 0x45b478 flag bytes",
    "spline.accelerate":
        "// method 0x4435f0 <ScCubicSpline<float,float>::Accelerate>; asm pin: 0x7041ec 0.0, 0x7041f0 2.0, 0x7041f4 1.0, "
        "0x7041f8 6.0, 0x7041fc 0.0 (the natural ends); AfterConstruction 0x4435b0 runs it per record",
    "spline.interpolate":
        "// method 0x443a70 <ScCubicSpline<float,float>::Interpolate>; asm pin: binary search lo = 0, hi = n - 1, no clamp, "
        "0x704204 1/6 (f32 0x3e2aaaab); wrapper 0x61a6e0 takes the record by the low byte of the index (0x61a6e8)",
}

M32 = 0xFFFFFFFF
SIXTH = Fraction(struct.unpack("<f", struct.pack("<I", 0x3E2AAAAB))[0])     # [0x704204]
LOOKAHEAD = Fraction(0.01)                                                   # [0x706728] (fld qword), hud.md §1.3


def f32v(bits: int) -> Fraction:
    return Fraction(struct.unpack("<f", struct.pack("<I", bits & M32))[0])


def _f32_store(x: Fraction) -> Fraction:
    """An f32 store (`fstp dword`) of an exact value: rounded once to f32."""
    return f32v(osi_vm._f32_bits_fraction(x))


# ---------------------------------------------------------------------------------------------------------------
# The spline (hud.md §1.5)
# ---------------------------------------------------------------------------------------------------------------
class Spline:
    """One component spline: knots (t, v) as f32 values. `accelerate(pc)` builds y2 (hud.md §1.5: NR spline(), natural
    ends); `interpolate(t_bits, pc)` is NR splint() with no clamp (the end segment extrapolates). Operations inside one
    expression are rounded to `pc` bits (24, 53 or 64); values stored to memory (y2, u, the result) are rounded to f32."""

    def __init__(self, knots: list[int], values: list[int]):
        self.t = [f32v(b) for b in knots]
        self.v = [f32v(b) for b in values]
        self.y2: Optional[list[Fraction]] = None

    def accelerate(self, pc: int) -> list[Fraction]:
        r = lambda x: round_p(x, pc)                                                  # noqa: E731
        t, v, n = self.t, self.v, len(self.t)
        y2, u = [Fraction(0)] * n, [Fraction(0)] * n                                   # y2[0] = u[0] = 0 (0x7041ec)
        for i in range(1, n - 1):
            sig = r(r(t[i] - t[i - 1]) / r(t[i + 1] - t[i - 1]))
            p = r(r(sig * y2[i - 1]) + 2)                                              # 2.0 (0x7041f0)
            y2[i] = _f32_store(r(r(sig - 1) / p))                                      # 1.0 (0x7041f4)
            a = r(r(v[i + 1] - v[i]) / r(t[i + 1] - t[i]))
            b = r(r(v[i] - v[i - 1]) / r(t[i] - t[i - 1]))
            d = r(a - b)
            num = r(r(r(6 * d) / r(t[i + 1] - t[i - 1])) - r(sig * u[i - 1]))          # 6.0 (0x7041f8)
            u[i] = _f32_store(r(num / p))
        y2[n - 1] = Fraction(0)                                                        # 0x7041fc
        for k in range(n - 2, -1, -1):
            y2[k] = _f32_store(r(r(y2[k] * y2[k + 1]) + u[k]))
        self.y2 = y2
        return y2

    def interpolate(self, t_bits: int, pc: int) -> int:
        """The f32 bits of the spline at t (an f32), rounded as the x87 does at precision control `pc`."""
        r = lambda x: round_p(x, pc)                                                  # noqa: E731
        if self.y2 is None:
            self.accelerate(pc)
        tk, vk, y2 = self.t, self.v, self.y2
        t = f32v(t_bits)
        lo, hi = 0, len(tk) - 1
        while hi - lo > 1:                                                             # 0x443a70: knot_t > t -> hi = mid
            mid = (hi + lo) >> 1
            if tk[mid] > t:
                hi = mid
            else:
                lo = mid
        h = r(tk[hi] - tk[lo])
        A = r(r(tk[hi] - t) / h)
        B = r(r(t - tk[lo]) / h)
        a3 = r(r(r(A * A) * A) - A)
        b3 = r(r(r(B * B) * B) - B)
        t1 = r(r(A * vk[lo]) + r(B * vk[hi]))
        t2 = r(r(a3 * y2[lo]) + r(b3 * y2[hi]))
        t3 = r(r(t2 * r(h * h)) * SIXTH)
        return osi_vm.exact_round_f32(t1 + t3, pc)


class SplineFile:
    """A `.slb` spline file: u32 count, u32 offset of the records; a record is {u32 knots, u32 offset of (f32 t, f32 v) x
    knots, u32 0} (hud.md §1.5 SLK loader; the offsets are file offsets before relocation). `pc` is the precision control
    at construction time: AfterConstruction 0x4435b0 runs Accelerate for every record then."""

    def __init__(self, data: bytes, pc: int = 24):
        self.pc = pc
        if len(data) < 8:
            raise VmError("Undefined behaviour: spline file too short")
        count, recs = struct.unpack_from("<II", data, 0)
        self.count = count & 0xFF            # the AfterConstruction counter is a byte (0x4435b9): only the low byte of the count
        self.splines: list[Spline] = []
        for i in range(count):
            k, off, _ = struct.unpack_from("<III", data, recs + 12 * i)
            kv = struct.unpack_from("<%dI" % (2 * k), data, off)
            self.splines.append(Spline(list(kv[0::2]), list(kv[1::2])))

    @classmethod
    def from_splines(cls, splines: list[Spline], pc: int = 24) -> "SplineFile":
        f = cls.__new__(cls)
        f.pc, f.count, f.splines = pc, len(splines) & 0xFF, splines
        return f

    def interpolate(self, index: int, t_bits: int, pc: int) -> int:
        i = index & 0xFF                                       # the wrapper takes the low byte (0x61a6e8)
        if i >= len(self.splines):
            raise VmError("Undefined behaviour: spline index %d is outside the file (%d splines)" % (i, len(self.splines)))
        sp = self.splines[i]
        if sp.y2 is None:
            sp.accelerate(self.pc)                             # tabulated once, at construction
        return sp.interpolate(t_bits, pc)


# ---------------------------------------------------------------------------------------------------------------
# Camera state (hud.md §1.2 field table) and the host subsystem
# ---------------------------------------------------------------------------------------------------------------
class CamState:
    """The fields the camera natives write and the `Z camera.*` lines print. None = no native has written it (`-`)."""

    def __init__(self):
        self.idx: Optional[int] = None          # +0x3b8 (dword)
        self.tidx: Optional[int] = None         # +0x3bc
        self.duration: Optional[float] = None   # +0x3a8 (double of an f32)
        self.start: Optional[float] = None      # +0x3b0 (double)
        self.fwd: Optional[int] = None          # +0x460 (byte)
        self.look: Optional[int] = None         # +0x461 (byte)
        self.eye: Optional[tuple] = None        # f32 bits x, y, z (+0x150)
        self.target: Optional[tuple] = None     # +0x15c


class HostSources:
    """The per-context host state (one per HostContext, built lazily): the pass time `[pm+8]` that SrGetSeconds(0) returns
    (0x4fa497), the camera states, the loaded spline files."""

    def __init__(self, ctx):
        self.ctx = ctx
        self.pass_time = 0.0
        self.cams: dict[int, CamState] = {}
        self.camera = None                      # an explicit camera object (tests); else the main view's (EngineObjects)
        self.files: dict[int, SplineFile] = {}  # id(spline object) -> its file
        self.preloaded: dict[bytes, SplineFile] = {}   # a name -> file seam for tests (no install)
        self.pcs: dict[int, int] = {}           # id(spline object) -> fpu_pc at construction

    def state(self, cam) -> CamState:
        s = self.cams.get(id(cam))
        if s is None:
            s = self.cams[id(cam)] = CamState()
        return s

    def camera_obj(self):
        if self.camera is not None:
            return self.camera
        from . import directors, objects
        cams = self.ctx.subsystem(directors.EngineObjects).cameras
        return next((c for c in cams.values() if isinstance(c, objects.GcLegoCamera)), None)

    def spline_file(self, spline) -> Optional[SplineFile]:
        """The data of a script-created ScSLOSISLKCubicSpline: `Root/Data/Levels/<l>/<a>/<a>_CAM.slb` is the entry
        `<a>_CAM.slb` of `Root/Data/Levels/<l>/<a>.blk` (natives.md §8; the path-to-block mapping is INFERRED there)."""
        f = self.files.get(id(spline))
        if f is not None:
            return f
        name = bytes(spline.slk_name)
        f = self.preloaded.get(name)
        if f is None:
            vfs = self.ctx.vfs
            if vfs is None:
                return None                     # a host with no install (the unit tests): the camera has no data to follow
            path = name.decode("latin-1").replace("\\", "/")
            blk, _, entry = path.rpartition("/")
            blk_path = blk + ".blk"
            ent = framework.BlockFiles(vfs.root, vfs).find(blk_path, entry)
            data = vfs.read(blk_path)
            if ent is None or data is None:
                raise VmError("Undefined behaviour: the spline file %r is not in the block file" % name)
            f = SplineFile(data[ent.offset:ent.offset + ent.size], self.pcs.get(id(spline), 24))
        self.files[id(spline)] = f
        return f


# ---------------------------------------------------------------------------------------------------------------
# The natives (wrappers over osi_natives.objects, plus the bodies of abs / setcampos / settargpos)
# ---------------------------------------------------------------------------------------------------------------
def _cam(ctx, call, i=0):
    from . import objects
    return objects._arg(ctx, call, i, objects.GcLegoCamera, "GcLegoCamera")


def _f(call, i) -> int:
    return call.arg(i).value & M32


def _wrap(reg, ctx, key, post):
    k = reg._key(*key.split(".", 1))
    orig = reg.entries[k].fn

    def fn(call):
        r = orig(call) if orig is not None else None
        post(call)
        return r
    reg.bind(key, fn)


def bind(reg, ctx) -> None:
    hs = ctx.subsystem(HostSources)

    def abs_(call):
        b = _f(call, 0) & 0x7FFFFFFF
        return Variant(T_FLOAT, osi_vm.NAN_BITS if (b & 0x7F800000) == 0x7F800000 and b & 0x7FFFFF else b)

    def setcampos(call):                                                     # 0x44e8a0: f32 stores, then SetCameraPosition
        hs.state(_cam(ctx, call)).eye = (_f(call, 1), _f(call, 2), _f(call, 3))
        return NOTHING

    def settargpos(call):
        hs.state(_cam(ctx, call)).target = (_f(call, 1), _f(call, 2), _f(call, 3))
        return NOTHING

    def getcamera(call):                  # directors' getter, but the camera is the objects group's class (it has the AI mode)
        from . import directors, objects
        vp = ctx.object_arg(call, 0)
        if vp is None:
            raise VmError("Undefined behaviour: GcViewPort::GetCamera on a null view (0x44809e reads [0+0x30])")
        cam_type = reg.types.id("GcLegoCamera")
        def make():
            c = objects.GcLegoCamera(cam_type)
            c.engine_refs = 1                                                      # the camera node holds it (natives.md §2.4)
            return c
        cam = ctx.subsystem(directors.EngineObjects).camera_of(vp, make)
        return ctx.create_variant(cam, cam_type)                                   # CreateVariant 0x447500

    reg.bind("gcviewport.getcamera", getcamera)
    reg.bind("scslosimath.abs", abs_)
    reg.bind("gclegocamera.setcampos", setcampos)
    reg.bind("gclegocamera.settargpos", settargpos)

    orig_create = reg.entries[reg._key("scslosislkcubicspline", "create")].fn

    def create(call):                                                        # AfterConstruction 0x4435b0 runs at this precision
        v = orig_create(call)
        obj = ctx._objects.get(v.value)
        if obj is not None:
            hs.pcs[id(obj)] = call.vm.fpu_pc
        return v
    if orig_create is not None:
        reg.bind("scslosislkcubicspline.create", create)

    def post_aimode(call):                                                   # 0x45b52b: modes 2 and 3 stamp the time
        cam = _cam(ctx, call)
        if cam.aimode in (2, 3):
            hs.state(cam).start = hs.pass_time

    def post_cameraspline(call):                                             # 0x45b3c6-0x45b3ef
        st = hs.state(_cam(ctx, call))
        st.idx = call.arg(2).value & M32
        st.duration = float(f32v(_f(call, 3)))
        st.start = hs.pass_time
        st.fwd, st.look = call.arg(4).value & 0xFF, call.arg(5).value & 0xFF

    def post_targetspline(call):                                             # 0x45b443-0x45b478
        st = hs.state(_cam(ctx, call))
        st.idx = call.arg(2).value & M32
        st.tidx = call.arg(3).value & M32
        st.duration = float(f32v(_f(call, 4)))
        st.start = hs.pass_time
        st.fwd, st.look = call.arg(5).value & 0xFF, call.arg(6).value & 0xFF

    _wrap(reg, ctx, "gclegocamera.setcameraaimode", post_aimode)
    _wrap(reg, ctx, "gclegocamera.setcameraspline", post_cameraspline)
    _wrap(reg, ctx, "gclegocamera.settargetspline", post_targetspline)


register_group("host_sources", bind)


# ---------------------------------------------------------------------------------------------------------------
# The camera process (hud.md §1.3)
# ---------------------------------------------------------------------------------------------------------------
class CameraStep(NamedTuple):
    fire: bool           # elapsed reached the duration: send OnEvent(6, camera, 0)


def camera_step(hs: HostSources, cam, time: float, pc: int) -> CameraStep:
    """One GcLegoCamera::Process 0x451990 at pass time `time` (a double), for a camera in AI mode 2 or 3: writes the eye
    (and the target) and reports whether the OnEvent(6) guard fires (the caller resets nothing: the start time is reset
    here, 0x452c0b). Any other mode does nothing (modes 0, 1, 8, ... have no spline and no OnEvent(6), hud.md §1.3-§1.4).
    INFERRED (not read to the bit): mode 2 writes the eye like mode 3 and its look-ahead target uses the camera splines."""
    mode = cam.aimode
    st = hs.state(cam)
    if mode not in (2, 3) or cam.spline_holder is None or st.duration is None:
        return CameraStep(False)
    r = lambda x: round_p(x, pc)                                                     # noqa: E731
    dur = st.duration
    if dur == 0.0:
        raise VmError("Undefined behaviour: the camera spline duration is 0 (a division by zero in the original)")
    spl = hs.spline_file(cam.spline_holder)
    if spl is None:
        return CameraStep(False)
    elapsed = float(r(Fraction(time) - Fraction(st.start)))                          # 0x452a40: fsub at pc, stored as a double
    if elapsed > dur:                                                                # 0x452a55: clamp
        elapsed = dur
    forward = bool(st.fwd)
    if not forward:                                                                  # reverse: rem = duration - elapsed
        elapsed = float(r(Fraction(dur) - Fraction(elapsed)))
    fe, fd = Fraction(elapsed), Fraction(dur)
    param = exact_round_f32(r(fe / fd), pc)                                          # 0x452a74: f32(elapsed / duration)
    i = st.idx
    eye = (spl.interpolate(i, param, pc), spl.interpolate(i + 1, param, pc), spl.interpolate(i + 2, param, pc))
    st.eye = eye                                                                     # SetCameraPosition (0x452b41)
    if mode == 3:
        j = st.tidx
        st.target = (spl.interpolate(j, param, pc), spl.interpolate(j + 1, param, pc), spl.interpolate(j + 2, param, pc))
    elif st.look:                                                                    # mode 2 with look-ahead: camera splines at param2
        p2 = exact_round_f32(r(r(fe + LOOKAHEAD) / fd) if forward else r(r(fe - LOOKAHEAD) / fd), pc)
        st.target = (spl.interpolate(i, p2, pc), spl.interpolate(i + 1, p2, pc), spl.interpolate(i + 2, p2, pc))
    fire = elapsed >= dur if forward else elapsed <= 0.0                             # 0x452bfa / reverse: rem <= 0.0
    if fire:
        st.start = time                                                              # 0x452c0b
    return CameraStep(fire)


# ---------------------------------------------------------------------------------------------------------------
# The `Z` snapshot (hud.md §9)
# ---------------------------------------------------------------------------------------------------------------
def _hexf(b: int) -> str:
    return "%08x" % (b & M32)


def _dbits(x: float) -> str:
    return "%016x" % struct.unpack("<Q", struct.pack("<d", x))[0]


def z_block(vm, ctx, camera_model: bool) -> list[str]:
    """The lines of one frame's `Z` block after its `Z %6d` header, sorted bytewise as whole lines (hud.md §9.2). The
    subsystems of this task: `ref` (every live game handle), `camera` (only when the host models a camera) and `cs.playing`
    (only once the cinematics state exists). The HUD lines are Task 6b's."""
    lines = []
    for h in sorted(vm.heap.games):
        lines.append("Z ref %04x:%08x %d" % (vm.heap.game_type(h) & 0xFFFF, h, vm.heap.engine(h)))
    if ctx is not None:
        from . import directors
        cin = ctx._subsystems.get(directors.Cinematics)
        if cin is not None:
            lines.append("Z cs.playing %d" % (1 if cin.playing else 0))
        cam = ctx.subsystem(HostSources).camera_obj() if camera_model else None
        if cam is not None:
            st = ctx.subsystem(HostSources).state(cam)
            lines.append("Z camera.mode %02x" % (cam.aimode & 0xFF))
            sp = cam.spline_holder
            lines.append("Z camera.spline " + ("-" if sp is None or not sp.handle else
                                               "%04x:%08x" % (vm.heap.game_type(sp.handle) & 0xFFFF, sp.handle)))
            idx = [("-" if v is None else "%02x" % (v & 0xFF)) for v in (st.idx, st.tidx)]
            lines.append("Z camera.index " + ("-" if idx == ["-", "-"] else " ".join(idx)))
            lines.append("Z camera.duration " + ("-" if st.duration is None else _dbits(st.duration)))
            lines.append("Z camera.start " + ("-" if st.start is None else _dbits(st.start)))
            fl = [("-" if v is None else "%02x" % v) for v in (st.fwd, st.look)]
            lines.append("Z camera.flags " + ("-" if fl == ["-", "-"] else " ".join(fl)))
            lines.append("Z camera.eye " + ("-" if st.eye is None else " ".join(_hexf(b) for b in st.eye)))
            lines.append("Z camera.target " + ("-" if st.target is None else " ".join(_hexf(b) for b in st.target)))
    return sorted(lines, key=lambda s: s.encode("ascii"))


# ---------------------------------------------------------------------------------------------------------------
# The frame (hud.md §2.4, trace.md §6 "Version 3", conformance.md §6 rule 23)
# ---------------------------------------------------------------------------------------------------------------
DT = struct.unpack("<d", struct.pack("<Q", 0x3F7B4E81C0000000))[0]       # the double at 0x712d68 (D3)
POSITIONS = ("vk", "keymapper", "mouse", "camera")


class FrameHost:
    """Runs frames in the original's order. `events[(frame, position)]` = [(name, args)] are the host calls of the VK
    triggers, the key mapper and the mouse driver (and, when `camera_model` is off, the declared camera events); with
    `camera_model` the camera is stepped after Process and sends its own OnEvent(6). Mouse and camera calls write
    `C -1` (live) or are checked against the log (replay); VK and key-mapper calls write nothing (trace.md §6)."""

    def __init__(self, vm, trace, ctx=None, events=None, camera_model=False, write_z=True):
        self.vm, self.trace, self.ctx = vm, trace, ctx
        self.events = events or {}
        self.camera_model = camera_model
        self.write_z = write_z
        self.time = 0.0
        self.pm_flag, self.proc_byte = 0, 1      # registered so that it runs on the first frame (the shipped script runs every frame)
        self.passes = 0
        self.z: dict[int, list[str]] = {}        # frame -> the Z block (after the header), for expectations
        self.error: Optional[VmError] = None

    # --- the calls
    def _call(self, name, args, logged):
        vm = self.vm
        if logged:
            if vm.natives.replay is not None:
                vm.natives.replay_host_c(vm.c_line(-1, name, args))
            else:
                vm.log_c(-1, name, args)
        r = vm.call(name, args)
        if not r.ok:
            self.error = vm.last_error
        return r.ok

    def _events(self, k, position, logged):
        for name, args in self.events.get((k, position), []):
            if not self._call(name, [self._materialise(a) for a in args], logged):
                return False
        return True

    def _materialise(self, a):
        return a(self.vm) if callable(a) else a

    # --- one frame; returns False when a call raised (self.error is set)
    def frame(self, k: int) -> bool:
        vm, trace = self.vm, self.trace
        self.passes = 0
        self.time = float(round_p(Fraction(self.time) + Fraction(DT), vm.fpu_pc))   # fadd qword at the x87 setting
        tb, db = f32_bits(self.time), f32_bits(DT)
        if self.ctx is not None:
            self.ctx.subsystem(HostSources).pass_time = self.time
        if trace is not None:
            trace.line("K %6d %08x %08x %08x" % (k, tb, db, k))
        vm.begin_frame(k)
        vm.pcs_seen = set() if vm.pcs_seen is not None else None
        if not (self._events(k, "vk", False) and self._events(k, "keymapper", False) and self._events(k, "mouse", True)):
            return False
        # PM+0x40 parity (0x4a92fd-0x4a930a): the first pass runs the process only if its byte at +8 equals the flag; a pass
        # that returns true writes +8 = NOT flag, re-runs 0x4a94a6-0x4a94bb; a process still false after the 10th re-run keeps
        # +8 == this frame's flag, so it is skipped on the next frame and runs again the frame after.
        self.pm_flag ^= 1
        eligible = self.proc_byte == self.pm_flag
        while eligible:
            # ScSLOSIProcess::Process (0x43b33f-0x43b35f) returns false only for an int 0 result; ScProcessManager::Process
            # re-runs a false process with the same arguments at most 10 more times (0x4a957d cmp 0xa).
            r = vm.call("Process", [Variant(T_FLOAT, tb), Variant(T_FLOAT, db), Variant(T_INT, k)])
            self.passes += 1
            if not r.ok:
                self.error = vm.last_error
                return False
            if not (r.result.type == T_INT and r.result.value == 0):
                self.proc_byte = self.pm_flag ^ 1
                break
            if self.passes >= 11:
                break
        if not self._camera(k):
            return False
        self._z(k)
        return True

    def _camera(self, k) -> bool:
        vm, ctx = self.vm, self.ctx
        if not self.camera_model:
            return self._events(k, "camera", True)
        if vm.natives.replay is not None:           # a replay runs no native: the camera's state is not there; the log says
            return self._camera_from_log()          # whether the call happens here (the call is then asserted line by line)
        hs = ctx.subsystem(HostSources)
        cam = hs.camera_obj()
        try:
            if cam is None or camera_step(hs, cam, self.time, vm.fpu_pc).fire is False:
                return True
        except VmError as e:                         # an Undefined behaviour of the model ends the run like a script error
            self.error = e
            return False
        return self._call("OnEvent", [Variant(T_INT, 6), ctx.create_variant(cam, cam.type_id), Variant(T_INT, 0)], True)

    def _camera_from_log(self) -> bool:
        reg = self.vm.natives
        rec = reg._peek()
        if not (rec and rec["kind"] == "C" and rec["seq"] == -1 and rec["name"] == "OnEvent" and rec["args"]
                and rec["args"][0][0] == Variant(T_INT, 6)):
            return True
        args = [reg._materialise(self.vm.heap, v, text, g, -1) for v, text, g in rec["args"]]
        return self._call("OnEvent", args, True)

    def _z(self, k: int) -> None:
        lines = z_block(self.vm, self.ctx, self.camera_model)
        self.z[k] = lines
        if self.write_z and self.trace is not None and self.vm.natives.replay is None:
            self.trace.line("Z %6d" % k)
            for ln in lines:
                self.trace.line(ln)
