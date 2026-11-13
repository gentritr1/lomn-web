"""M2a-3 T4b: the host sources of the Python oracle (tools/research/osi_natives/host_sources.py, the frame order and the
`C -1` / `Z` records in tools/research/osi_vm.py). Written from docs/osi/hud.md (§1 camera and spline pins, §2 frame order,
§5 and §11 input, §9 `Z`), docs/osi/trace.md (§2, §6 "Version 3") and docs/osi/conformance.md (host events, rules 22, 23);
the exe was read as data with objdump for the asm pins. Never from engine/.

Per behaviour: a citation test, an asm-pin test and a behaviour test. Pins need LOMN_DATA (they skip with `SKIPPED (no
LOMN_DATA)`); the behaviour tests run on a synthetic script and a synthetic spline file, so they need no install. The Base.osi
tests (the real handler, F_act, the replay) skip without it."""
import json
import os
import re
import shutil
import struct
import subprocess
import sys
from fractions import Fraction
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "osi"))
sys.path.insert(0, str(REPO / "tools" / "research"))

import osiasm  # noqa: E402
import osi_vm  # noqa: E402
import keys as osi_keys  # noqa: E402
from expect_calc import round_p  # noqa: E402
from osi_natives import framework  # noqa: E402
from osi_natives import host_sources as hsm  # noqa: E402
from osi_vm import Variant, T_INT, T_FLOAT  # noqa: E402

TYPES = osi_vm.TypeTable.load(REPO / "data" / "osi" / "types.json")
ORACLE = REPO / "tools" / "research" / "osi_vm.py"
H_DIR = REPO / "tests" / "osi" / "conformance" / "h"
CAM_TYPE = TYPES.id("GcLegoCamera")


def fb(bits):
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def tof(x):
    """The f32 nearest to the exact value x, as bits."""
    return osi_vm._f32_bits_fraction(Fraction(x))


# ===============================================================================================================
# 1. The spline (hud.md §1.5): Accelerate 0x4435f0 and Interpolate 0x443a70
# ===============================================================================================================
SYN_KNOTS = [(0, 0), (Fraction(1, 4), 1), (Fraction(1, 2), 0), (1, 2)]
# hud.md §1.5 "Pins" (EXECUTED there by spline.py): (t, f32 bits under pc 24, under pc 53)
SYN_PINS = [(Fraction(1, 8), 0x3F3A6F4E, 0x3F3A6F4E), (Fraction(3, 4), 0x3EB21642, 0x3EB21641)]
FRNT_PINS = [(48, "0.10", 0xC185B15D, 0xC185B15D), (48, "0.28", 0xC0D10E08, 0xC0D10E08), (48, "0.44", 0x413BA175, 0x413BA175),
             (48, "0.75", 0x425B120A, 0x425B120B), (1, "0.13", 0x417C70F8, 0x417C70F8), (1, "0.86", 0x417ADEB6, 0x417ADEB5)]


def syn_spline(last=2):
    ks, vs = zip(*(SYN_KNOTS[:3] + [(1, last)]))
    return hsm.Spline([tof(k) for k in ks], [tof(v) for v in vs])


def textbook(ks, vs, t):
    """A natural cubic spline in exact rationals from the published formulas (tridiagonal system for the second
    derivatives, Thomas algorithm; the cubic on the segment). Written without looking at the oracle or the decomp."""
    n = len(ks)
    h = [ks[i + 1] - ks[i] for i in range(n - 1)]
    a, b, c, d = [Fraction(0)] * n, [Fraction(1)] * n, [Fraction(0)] * n, [Fraction(0)] * n
    for i in range(1, n - 1):
        a[i], b[i], c[i] = h[i - 1], 2 * (h[i - 1] + h[i]), h[i]
        d[i] = 6 * ((vs[i + 1] - vs[i]) / h[i] - (vs[i] - vs[i - 1]) / h[i - 1])
    for i in range(1, n):                                              # forward elimination
        w = a[i] / b[i - 1]
        b[i] -= w * c[i - 1]
        d[i] -= w * d[i - 1]
    m = [Fraction(0)] * n
    m[n - 1] = d[n - 1] / b[n - 1]
    for i in range(n - 2, -1, -1):
        m[i] = (d[i] - c[i] * m[i + 1]) / b[i]
    i = max(j for j in range(n - 1) if ks[j] <= t) if t < ks[-1] else n - 2
    hh = h[i]
    return (m[i] * (ks[i + 1] - t) ** 3 / (6 * hh) + m[i + 1] * (t - ks[i]) ** 3 / (6 * hh)
            + (vs[i] / hh - m[i] * hh / 6) * (ks[i + 1] - t) + (vs[i + 1] / hh - m[i + 1] * hh / 6) * (t - ks[i]))


@pytest.mark.parametrize("pc, col", [(24, 1), (53, 2)])
def test_synthetic_spline_pins_to_the_f32_bit_under_both_precisions(pc, col):
    sp = syn_spline()
    sp.accelerate(pc)
    for pin in SYN_PINS:
        assert sp.interpolate(tof(pin[0]), pc) == pin[col], (pin[0], pc)


def test_synthetic_spline_with_the_last_knot_at_1_gives_the_other_pins():
    sp = syn_spline(last=1)                    # hud.md §1.5: with (1, 1) the values would be 3f38590b / bcb21640 (pc24)
    sp.accelerate(24)
    assert [sp.interpolate(tof(t), 24) for t in (Fraction(1, 8), Fraction(3, 4))] == [0x3F38590B, 0xBCB21640]


def test_the_synthetic_pins_tell_the_spline_families_apart():
    # on (0,0) (0.25,1) (0.5,0) (1,2): linear 0.5 / 1.0, Catmull-Rom 0.5625 / 0.9375 (hud.md §1.5); the natural cubic is far from both
    got = [fb(p[1]) for p in SYN_PINS]
    for lin, cr, g in ((0.5, 0.5625, got[0]), (1.0, 0.9375, got[1])):
        assert abs(g - lin) > 0.1 and abs(g - cr) > 0.1


def test_the_synthetic_spline_equals_a_textbook_natural_cubic_within_1e_5():
    ks = [Fraction(fb(tof(k))) for k, _ in SYN_KNOTS]
    vs = [Fraction(fb(tof(v))) for _, v in SYN_KNOTS]
    for t, p24, p53 in SYN_PINS:
        want = textbook(ks, vs, Fraction(fb(tof(t))))
        for bits in (p24, p53):
            assert abs((Fraction(fb(bits)) - want) / want) < Fraction(1, 100000)


def test_x87_rounding_is_applied_per_operation_and_f32_stores_are_applied_to_y2():
    # a wrong model (all in f32, or all exact) would not land on both pin columns at once: the two modes differ in the last bit
    sp = syn_spline()
    sp.accelerate(53)
    assert sp.interpolate(tof(Fraction(3, 4)), 53) == 0x3EB21641 != 0x3EB21642
    sp.y2 = None
    sp.accelerate(24)
    assert sp.interpolate(tof(Fraction(3, 4)), 24) == 0x3EB21642


def _frnt_cam(lomn_data):
    """FRNT_CAM.slb from the install, read here with struct (not through the oracle's loader): the splines as (t bits, v bits)."""
    sys.path.insert(0, str(REPO / "tools" / "research"))
    import slb
    os.environ.setdefault("LOMN_DATA", str(lomn_data))
    data = slb.entry("levels/lev0/frnt.blk", "FRNT_CAM.slb")
    count, recs = struct.unpack_from("<II", data, 0)
    out = []
    for i in range(count):
        k, off, _ = struct.unpack_from("<III", data, recs + 12 * i)
        kv = struct.unpack_from("<%dI" % (2 * k), data, off)
        out.append((list(kv[0::2]), list(kv[1::2])))
    return data, out


@pytest.mark.parametrize("pc, col", [(24, 2), (53, 3)])
def test_frnt_cam_pins_to_the_f32_bit_under_both_precisions(lomn_data, pc, col):
    data, splines = _frnt_cam(lomn_data)
    assert len(splines) == 60 and len(data) == 5012                       # hud.md §1.5 / F8
    for pin in FRNT_PINS:
        ks, vs = splines[pin[0]]
        sp = hsm.Spline(ks, vs)
        sp.accelerate(pc)
        assert sp.interpolate(tof(Fraction(pin[1])), pc) == pin[col], pin


def test_frnt_cam_pins_equal_a_textbook_natural_cubic_within_1e_5(lomn_data):
    _, splines = _frnt_cam(lomn_data)
    for idx, t, p24, p53 in FRNT_PINS:
        ks, vs = [Fraction(fb(b)) for b in splines[idx][0]], [Fraction(fb(b)) for b in splines[idx][1]]
        want = textbook(ks, vs, Fraction(fb(tof(Fraction(t)))))
        for bits in (p24, p53):
            assert abs((Fraction(fb(bits)) - want) / want) < Fraction(1, 100000), (idx, t)


def test_the_spline_file_loader_reads_frnt_cam_and_indexes_by_the_low_byte(lomn_data):
    data, splines = _frnt_cam(lomn_data)
    f = hsm.SplineFile(data, 24)
    assert f.count == 60 and len(f.splines) == 60
    t = tof(Fraction("0.44"))
    assert f.interpolate(48, t, 24) == f.interpolate(48 + 256, t, 24) == 0x413BA175     # the wrapper takes the low byte (0x61a6e8)
    with pytest.raises(osi_vm.VmError):
        f.interpolate(60, t, 24)


def test_the_pin_values_are_the_curved_ones(lomn_data):
    # spline 0 is nearly straight (hud.md F21), so it cannot tell families apart; 48 and 1 are curved
    _, splines = _frnt_cam(lomn_data)
    ks, vs = splines[48]
    assert len(ks) == 7 and fb(vs[0]) == pytest.approx(-20.76, abs=0.01) and fb(vs[-1]) == pytest.approx(89.81, abs=0.01)


# --- asm pins (the exe read as data)
@pytest.fixture(scope="session")
def image(canonical_exe):
    import exc
    return exc.Image.from_file(canonical_exe)


@pytest.fixture(scope="session")
def disasm(canonical_exe):
    if shutil.which("objdump") is None:
        pytest.skip("SKIPPED (no objdump)")
    cache = {}

    def run(start, stop):
        if (start, stop) not in cache:
            out = subprocess.run(["objdump", "-d", "-M", "intel", "--start-address=%#x" % start, "--stop-address=%#x" % stop,
                                  str(canonical_exe)], capture_output=True, text=True, check=True, timeout=60).stdout
            rows = {}
            for ln in out.splitlines():
                m = re.match(r"^\s*([0-9a-f]+):\s+[0-9a-f ]+\t(.*)$", ln)
                if m:
                    rows[int(m.group(1), 16)] = re.sub(r" <[^>]*>$", "", re.sub(r"\s+", " ", m.group(2).strip()))
            cache[(start, stop)] = rows
        return cache[(start, stop)]
    return run


def pin(disasm, start, stop, expect):
    rows = disasm(start, stop)
    bad = {a: (t, rows.get(a)) for a, t in expect.items() if rows.get(a) != t}
    assert not bad, "asm pin mismatch: " + ", ".join("%#x: %r vs %r" % (a, e, g) for a, (e, g) in sorted(bad.items()))


def cited(key, *addrs):
    have = {int(m, 16) for m in re.findall(r"0x([0-9a-f]{5,})", hsm.CITES[key])}
    missing = [hex(a) for a in addrs if a not in have]
    assert not missing, "%s: CITES does not name %s" % (key, missing)


def test_spline_asm_pins(disasm, image):
    # the constants by address (hud.md §1.5)
    assert [fb(struct.unpack("<I", image.read(a, 4))[0]) for a in (0x7041EC, 0x7041F0, 0x7041F4, 0x7041F8, 0x7041FC)] == [0.0, 2.0, 1.0, 6.0, 0.0]
    assert struct.unpack("<I", image.read(0x704204, 4))[0] == 0x3E2AAAAB                      # 1/6, the f32 the code multiplies by
    pin(disasm, 0x4435B0, 0x4435E6, {0x4435B9: "mov byte ptr [ebp - 0x8], 0x0", 0x4435CD: "call 0x4435f0",
                                    0x4435D2: "inc byte ptr [ebp - 0x8]", 0x4435DB: "cmp byte ptr [ebp - 0x8], dl"})   # the byte counter
    pin(disasm, 0x4435F0, 0x4438D4, {0x4436AD: "fld dword ptr [0x7041ec]", 0x44371A: "fadd dword ptr [0x7041f0]",
                                     0x443733: "fsub dword ptr [0x7041f4]", 0x443781: "fld dword ptr [0x7041f8]",
                                     0x4437BC: "fld dword ptr [0x7041fc]", 0x443826: "fld dword ptr [0x704200]", 0x4438CC: "ret"})
    pin(disasm, 0x443A70, 0x443B46, {0x443A8A: "jmp 0x443ab2", 0x443AA0: "fcomp st(2)", 0x443AA4: "test ah, 0x45",
                                     0x443AA7: "jne 0x443ab0", 0x443AA9: "mov edi, ebx", 0x443AB0: "mov ebp, ebx",
                                     0x443B30: "fmul dword ptr [0x704204]", 0x443B45: "ret 0x4"})
    cited("spline.accelerate", 0x4435F0, 0x4435B0, 0x7041EC, 0x7041F0, 0x7041F4, 0x7041F8, 0x7041FC)
    cited("spline.interpolate", 0x443A70, 0x61A6E0, 0x61A6E8, 0x704204)


# ===============================================================================================================
# 2. The camera process (hud.md §1.1-§1.4): asm pins and a synthetic run
# ===============================================================================================================
def test_camera_process_asm_pins(disasm, image):
    pin(disasm, 0x452A20, 0x452A90, {
        0x452A26: "cmp byte ptr [eax + 0x3a1], 0x3", 0x452A33: "cmp byte ptr [eax + 0x460], 0x0",
        0x452A40: "fld qword ptr [ebp + 0x8]", 0x452A43: "fsub qword ptr [eax + 0x3b0]", 0x452A55: "fcomp qword ptr [eax + 0x3a8]",
        0x452A5D: "test ah, 0x45", 0x452A68: "fld qword ptr [eax + 0x3a8]", 0x452A6E: "fstp qword ptr [ebp - 0xe74]",
        0x452A80: "fdiv qword ptr [eax + 0x3a8]", 0x452A86: "fstp dword ptr [ebp - 0x9b4]"})
    pin(disasm, 0x452BF8, 0x452C20, {0x452BFA: "fcomp qword ptr [eax + 0x3a8]", 0x452C02: "test ah, 0x5", 0x452C05: "jne 0x45a910",
                                     0x452C0B: "fld qword ptr [ebp + 0x8]", 0x452C14: "fstp qword ptr [eax + 0x3b0]", 0x452C1A: "push 0x6"})
    pin(disasm, 0x452D1F, 0x452D2C, {0x452D1F: "push 0x7072f8", 0x452D24: "call 0x60b320", 0x452D29: "jmp 0x45a910"})
    cited("camera.process", 0x451990, 0x452A26, 0x452A33, 0x452A40, 0x452A55, 0x452A74, 0x452BFA, 0x452C0B, 0x452D24)


def test_camera_setters_asm_pins(disasm):
    pin(disasm, 0x45B380, 0x45B490, {
        0x45B3C6: "mov dword ptr [edi + 0x3b8], edx", 0x45B3CF: "fstp qword ptr [edi + 0x3a8]", 0x45B3D5: "push 0x0",
        0x45B3D7: "call 0x4fa460", 0x45B3DD: "fstp qword ptr [edi + 0x3b0]", 0x45B3E6: "mov byte ptr [edi + 0x460], dl",
        0x45B3EF: "mov byte ptr [edi + 0x461], dl", 0x45B3FC: "ret 0x14",
        0x45B446: "mov dword ptr [edi + 0x3b8], edx", 0x45B44C: "mov edx, dword ptr [ebp + 0x10]",
        0x45B44F: "mov dword ptr [edi + 0x3bc], edx", 0x45B458: "fstp qword ptr [edi + 0x3a8]", 0x45B466: "fstp qword ptr [edi + 0x3b0]",
        0x45B46F: "mov byte ptr [edi + 0x460], dl", 0x45B478: "mov byte ptr [edi + 0x461], dl", 0x45B485: "ret 0x18"})
    cited("camera.setcameraspline", 0x45B380, 0x45B3C6, 0x45B3CF, 0x45B3D5, 0x45B3E6, 0x45B3EF)
    cited("camera.settargetspline", 0x45B400, 0x45B443, 0x45B44C, 0x45B455, 0x45B45E, 0x45B46F, 0x45B478)


def test_the_natives_bodies_asm_pins_and_citations(disasm, image):
    pin(disasm, 0x618290, 0x6182D0, {0x61829B: "fld dword ptr [edx]", 0x6182A3: "fstp qword ptr [esp]", 0x6182A8: "call 0x657d46",
                                     0x6182AF: "fstp dword ptr [ebp - 0x8]", 0x6182C5: "call 0x4077f0"})
    pin(disasm, 0x44E8A0, 0x44E950, {0x44E8CE: "fstp dword ptr [ebp - 0x18]", 0x44E8D3: "fstp dword ptr [ebp - 0x14]",
                                     0x44E8D8: "fstp dword ptr [ebp - 0x10]", 0x44E8E3: "call 0x405e50", 0x44E8E8: "mov word ptr [edi], 0xf",
                                     0x44E943: "call 0x405f00"})
    for key in hsm.TIER1_KEYS:
        assert re.match(r"^// body 0x[0-9a-f]+, method 0x[0-9a-f]+ <[^>]+>; asm pin: \S", hsm.CITES[key]), key
    cited("scslosimath.abs", 0x618290, 0x657D46, 0x4077F0)
    cited("gclegocamera.setcampos", 0x44E8A0, 0x405E50, 0x44E8E3)
    cited("gclegocamera.settargpos", 0x44E900, 0x405F00, 0x44E943)


def make(asm, pc=24, install=None):
    """The oracle VM with every registered native group bound (the real registry), a HostContext with a natives.log."""
    image_, _ = osiasm.assemble(asm)
    tr = osi_vm.ListTrace()
    heap = osi_vm.Heap(tr)
    reg = osi_vm.NativeRegistry.load(osi_vm.DEFAULT_NATIVES, osi_vm.DEFAULT_MODS, TYPES)
    vm = osi_vm.Vm(image_, reg, heap, tr, fpu_pc=pc)
    log = []
    ctx = framework.HostContext(vm, heap, framework.Vfs(install) if install else None, framework.HostFlags(), log)
    framework.bind_natives(reg, ctx)
    return vm, ctx, tr, log


def synthetic_file():
    """Six 2-knot splines (0, 0) (1, k + 1): the eye at parameter p is (p, 2p, 3p), the target (4p, 5p, 6p)."""
    return hsm.SplineFile.from_splines([hsm.Spline([tof(0), tof(1)], [tof(0), tof(k + 1)]) for k in range(6)])


SET = """    GetVariableValue global:cam
    PushConstanti8 {mode}
    CallGameFunction gclegocamera.setcameraaimode 2
    Pop
    GetVariableValue global:cam
    GetVariableValue global:spl
    PushConstanti8 0
    PushConstantf32 {dur}
    PushConstanti8 {fwd}
    PushConstanti8 {look}
    CallGameFunction gclegocamera.setcameraspline 6
    Pop
    GetVariableValue global:cam
    GetVariableValue global:spl
    PushConstanti8 0
    PushConstanti8 3
    PushConstantf32 {dur}
    PushConstanti8 {fwd}
    PushConstanti8 {look}
    CallGameFunction gclegocamera.settargetspline 7
    Pop
"""


def cam_script(handler_sets_mode=None, mode=3, dur="4.0", fwd=1, look=1, at=3, stamp_only=False):
    """Setup gets the camera and a spline; Process sets the AI mode and both splines in frame `at` and records the first frame
    that sees the handler's flag (`seen`); OnEvent leaves the mode alone (the stand-in) or sets it to `handler_sets_mode`."""
    handler = ""
    if handler_sets_mode is not None:
        handler = "    GetVariableValue global:cam\n    PushConstanti8 %d\n    CallGameFunction gclegocamera.setcameraaimode 2\n    Pop\n" % handler_sets_mode
    setup = SET.format(mode=mode, dur=dur, fwd=fwd, look=look)
    if stamp_only:
        setup = "    GetVariableValue global:cam\n    PushConstanti8 %d\n    CallGameFunction gclegocamera.setcameraaimode 2\n    Pop\n" % mode
    return """.globals cam spl fr ev seen
.function process 3 @proc
.function onevent 10 @evt
.function main 0
    CallGameFunction gcgame.getmainview 0
    CallGameFunction gcviewport.getcamera 1
    SetVariableValue global:cam
    PushConstantString "cam.slb"
    CallGameFunction scslosislkcubicspline.create 1
    SetVariableValue global:spl
    PushConstant0
    SetVariableValue global:ev
    PushConstant0
    SetVariableValue global:seen
    PushConstanti8 1
    Return
proc:
    GetVariableValue 2
    SetVariableValue global:fr
    GetVariableValue 2
    PushConstanti8 %d
    EqualTo
    CompareAndBranchIfFalse @noset
%s
noset:
    GetVariableValue global:ev
    CompareAndBranchIfFalse @done
    GetVariableValue global:seen
    PushConstant0
    EqualTo
    CompareAndBranchIfFalse @done
    GetVariableValue global:fr
    SetVariableValue global:seen
done:
    PushConstanti8 1
    Return
evt:
%s    PushConstanti8 1
    SetVariableValue global:ev
    PushConstanti8 1
    Return
""" % (at, setup, handler)


def run_cam(asm, frames, pc=24):
    vm, ctx, tr, log = make(asm, pc)
    ctx.subsystem(hsm.HostSources).preloaded[b"cam.slb"] = synthetic_file()
    assert vm.call("main", []).ok, vm.last_error and vm.last_error.message
    host = hsm.FrameHost(vm, tr, ctx, {}, camera_model=True)
    for k in range(frames):
        assert host.frame(k), vm.last_error and vm.last_error.message
    return vm, ctx, tr, log, host


def event_frames(log):
    out, f = [], None
    for ln in log:
        if ln.startswith("K "):
            f = int(ln[1:])
        elif ln.startswith("C -1 OnEvent 0010:00000006 "):
            out.append(f)
    return out


def k_bits(tr, frame):
    for ln in tr.lines:
        if ln.startswith("K %6d " % frame):
            return ln.split()[2]
    raise AssertionError("no K line for frame %d" % frame)


def test_the_time_accumulation_reproduces_the_k_bits_of_hud_md():
    vm, ctx, tr, log, host = run_cam(cam_script(), 606)
    # hud.md §1.6 / F9: frame 3 3cda740e, 602 4080a3d0, 603 4080da6d, 604 4081110a (the port's K lines, EXECUTED there)
    assert [k_bits(tr, f) for f in (3, 602, 603, 604)] == ["3cda740e", "4080a3d0", "4080da6d", "4081110a"]


def test_onevent6_is_not_sent_before_f_evt_and_is_sent_at_it_pc24_604_pc53_603():
    for pc, want in ((24, 604), (53, 603)):
        vm, ctx, tr, log, host = run_cam(cam_script(handler_sets_mode=0), want + 3, pc)
        assert event_frames(log) == [want], pc        # the wrong precision mode fails this pin: 604 vs 603 (margin 9.7e-8 s)


def test_f_act_is_f_evt_plus_1():
    vm, ctx, tr, log, host = run_cam(cam_script(handler_sets_mode=0), 610)
    # OnEvent sets ev in frame 604 (after Process); the script's Process first sees it in frame 605 (hud.md §1.6: F_act = F_evt + 1)
    assert event_frames(log) == [604] and vm.global_value("seen") == Variant(T_INT, 605)


def test_refire_with_a_stand_in_handler_that_leaves_the_mode_at_3():
    # (a) hud.md §1.4 "Stand-in case only": the start time is reset at the fire (0x452c0b), so the next fires are 1205 and 1805
    vm, ctx, tr, log, host = run_cam(cam_script(), 1810)
    assert event_frames(log) == [604, 1205, 1805]


def test_refire_with_a_handler_that_sets_the_mode_to_0_never_fires_again_through_2400():
    # (b-synthetic) the real handler's effect (it sets camera AI mode 0, which freezes the camera: 0x452346 -> 0x45ab60)
    vm, ctx, tr, log, host = run_cam(cam_script(handler_sets_mode=0), 2401)
    assert event_frames(log) == [604]


def test_the_camera_event_writes_c_minus_1_with_the_game_argument_and_no_h_line():
    vm, ctx, tr, log, host = run_cam(cam_script(), 606)
    ev = [ln for ln in log if ln.startswith("C -1 OnEvent")]
    assert len(ev) == 1
    assert re.fullmatch(r"C -1 OnEvent 0010:00000006 002a:00000002 G 1 0010:00000000( 000f:00000000){7}", ev[0]), ev[0]
    assert not [ln for ln in log if ln.startswith("H 002a:")]                  # the +1 of CreateVariant 0x447530 is a variant reference
    # the trace has the call as an ordinary C line, after the frame's K line
    assert [ln for ln in tr.lines if ln.startswith("C OnEvent")][-1].startswith("C OnEvent 10 0010:00000006 002a:00000002 ")


def test_z_camera_lines_after_the_setters_and_at_the_fire_frame():
    vm, ctx, tr, log, host = run_cam(cam_script(), 606)
    z3 = host.z[3]
    t3 = struct.unpack("<Q", struct.pack("<d", fb(int(k_bits(tr, 3), 16))))[0]     # the start time = frame 3's pass time, as a double
    assert [ln for ln in z3 if ln.startswith("Z camera.")] == [
        "Z camera.duration 4010000000000000", "Z camera.eye 00000000 00000000 00000000", "Z camera.flags 01 01",
        "Z camera.index 00 03", "Z camera.mode 03", "Z camera.spline 0023:00000003", "Z camera.start %016x" % t3,
        "Z camera.target 00000000 00000000 00000000"]
    z604 = host.z[604]
    t604 = struct.unpack("<Q", struct.pack("<d", fb(int(k_bits(tr, 604), 16))))[0]
    assert "Z camera.eye 3f800000 40000000 40400000" in z604           # param 1.0 (elapsed clamped to the duration): (1, 2, 3)
    assert "Z camera.target 40800000 40a00000 40c00000" in z604        # (4, 5, 6)
    assert "Z camera.start %016x" % t604 in z604                        # reset at the fire (0x452c0b)
    assert "Z ref 002a:00000002 1" in z604                             # the camera: the viewport node's reference only
    assert z604 == sorted(z604, key=lambda s: s.encode())


def test_the_eye_follows_f32_elapsed_over_duration_between_frames():
    vm, ctx, tr, log, host = run_cam(cam_script(), 120)
    t = lambda f: Fraction(fb(int(k_bits(tr, f), 16)))                              # noqa: E731
    elapsed = round_p(t(100) - t(3), 24)                                            # 0x452a40: fsub at pc 24, stored as a double
    p = osi_vm._f32_bits_fraction(round_p(elapsed / 4, 24))                         # 0x452a74: f32(elapsed / duration)
    eye = [ln for ln in host.z[100] if ln.startswith("Z camera.eye")][0].split()[2:]
    assert eye == ["%08x" % p, "%08x" % tof(2 * fb(p)), "%08x" % tof(3 * fb(p))]   # the splines k = 0, 1, 2 are linear: y = (k + 1) t


def test_reverse_mode_3_starts_at_the_end_and_fires_when_the_remainder_reaches_0():
    vm, ctx, tr, log, host = run_cam(cam_script(fwd=0, handler_sets_mode=0), 610)
    assert "Z camera.eye 3f800000 40000000 40400000" in host.z[3]       # rem = duration: param 1.0
    assert event_frames(log) == [604]
    assert "Z camera.eye 00000000 00000000 00000000" in host.z[604]     # rem 0: param 0.0


def test_mode_2_forward_sets_the_target_only_with_the_look_ahead_flag():
    # hud.md §1.3 (mode 2 forward): with +0x461 set the target is the camera splines at (elapsed + 0.01 [0x706728]) / duration;
    # without it the target is not set (here: never written, so `-`). At frame 3 elapsed is 0 and f32(0.01) / 4 is exact.
    vm, ctx, tr, log, host = run_cam(cam_script(mode=2, look=1, handler_sets_mode=0), 10)
    p2 = tof(Fraction(fb(tof(Fraction(0.01)))) / 4)
    assert p2 == 0x3B23D70A
    assert "Z camera.target %08x %08x %08x" % (p2, tof(2 * fb(p2)), tof(3 * fb(p2))) in host.z[3]
    vm, ctx, tr, log, host = run_cam(cam_script(mode=2, look=0), 10)
    assert "Z camera.target -" in host.z[3] and "Z camera.target -" in host.z[9]
    assert "Z camera.eye 00000000 00000000 00000000" in host.z[3]                 # the eye is written in mode 2 (INFERRED)


def test_a_camera_in_another_mode_does_nothing():
    for mode in (0, 1, 8, 0xB):
        vm, ctx, tr, log, host = run_cam(cam_script(mode=mode), 700)
        assert event_frames(log) == [], mode


def test_setcameraaimode_to_2_or_3_stamps_the_start_time_with_the_pass_time():
    vm, ctx, tr, log, host = run_cam(cam_script(stamp_only=True, mode=3, at=5), 8)
    t5 = struct.unpack("<Q", struct.pack("<d", fb(int(k_bits(tr, 5), 16))))[0]
    assert "Z camera.start %016x" % t5 in host.z[5] and "Z camera.start -" in host.z[4]      # 0x45b52b-0x45b545; unwritten before


def test_abs_setcampos_and_settargpos_bodies():
    asm = """.globals c r
.function main 0
    CallGameFunction gcgame.getmainview 0
    CallGameFunction gcviewport.getcamera 1
    SetVariableValue global:c
    PushConstantf32 -2.5
    CallGameFunction scslosimath.abs 1
    SetVariableValue global:r
    GetVariableValue global:c
    PushConstantf32 1.5
    PushConstantf32 -2.0
    PushConstantf32 3.0
    CallGameFunction gclegocamera.setcampos 4
    Pop
    GetVariableValue global:c
    PushConstantf32 7.0
    PushConstantf32 8.0
    PushConstantf32 -9.0
    CallGameFunction gclegocamera.settargpos 4
    Pop
    PushConstanti8 1
    Return
"""
    vm, ctx, tr, log = make(asm)
    assert vm.call("main", []).ok
    assert vm.global_value("r") == Variant(T_FLOAT, tof(2.5))
    block = hsm.z_block(vm, ctx, True)
    assert "Z camera.eye 3fc00000 c0000000 40400000" in block and "Z camera.target 40e00000 41000000 c1100000" in block


# ===============================================================================================================
# 3. The mouse driver and the frame order (hud.md §2.4, §5.1, §11; trace.md §6 "Version 3")
# ===============================================================================================================
def test_mouse_asm_pins(disasm):
    pin(disasm, 0x55C160, 0x55C180, {0x55C172: "cmp dword ptr [0x73a0cc], 0x0"})
    pin(disasm, 0x55C740, 0x55C760, {0x55C74E: "push 0x73a11c", 0x55C753: "call 0x60b320"})
    cited("mouse.driver", 0x55C160, 0x55C172, 0x55C753, 0x73A11C, 0x60B320)


def test_the_process_map_priorities_asm_pins(disasm):
    pin(disasm, 0x43949B, 0x4394A8, {0x43949B: "push 0x0", 0x4394A4: "call 0x4a8f50"})        # GcControllerProcess: VK triggers
    pin(disasm, 0x439551, 0x43955C, {0x439551: "push 0x1", 0x43955A: "call 0x4a8f50"})        # GcMouseDriver
    pin(disasm, 0x4395AE, 0x4395B9, {0x4395AE: "push 0x1", 0x4395B7: "call 0x4a8f50"})        # GcKeyMapper
    pin(disasm, 0x4392EF, 0x4392FA, {0x4392EF: "push 0x2", 0x4392F8: "call 0x4a8f50"})        # the script
    pin(disasm, 0x447F4A, 0x447F55, {0x447F4A: "push 0x3", 0x447F53: "call 0x4a8f50"})        # the viewport camera node
    pin(disasm, 0x4A8F50, 0x4A8F70, {0x4A8F60: "movzx edx, byte ptr [ebp + 0xc]"})            # the key is the priority byte
    cited("frame.order", 0x4A9170, 0x4A8F50, 0x4A8F60, 0x43949B, 0x439551, 0x4395AE, 0x4392EF, 0x447F4A)


ORDER = """.globals g
.function process 3 @proc
.function onevent 10 @ev
.function input 5 @inp
.function main 0
    PushConstant0
    SetVariableValue global:g
    PushConstanti8 1
    Return
ev:
    GetVariableValue global:g
    PushConstanti8 10
    Multiply
    GetVariableValue 0
    Add
    SetVariableValue global:g
    PushNothing
    Return
inp:
    GetVariableValue global:g
    PushConstanti8 10
    Multiply
    PushConstanti8 1
    Add
    SetVariableValue global:g
    PushNothing
    Return
proc:
    PushConstanti8 1
    Return
"""


def test_frame_order_vk_keymapper_mouse_process_camera_in_any_declared_order():
    vm, ctx, tr, log = make(ORDER)
    assert vm.call("main", []).ok
    ev = lambda name, *a: (name, [Variant(T_INT, x) for x in a])                      # noqa: E731
    events = {(0, "camera"): [ev("OnEvent", 6)], (0, "mouse"): [ev("OnEvent", 26)], (0, "keymapper"): [ev("Input", 1)],
              (0, "vk"): [ev("Input", 2)]}
    host = hsm.FrameHost(vm, tr, ctx, events, camera_model=False)
    assert host.frame(0)
    assert [ln.split()[1] for ln in tr.lines if ln.startswith("C ")] == ["main", "Input", "Input", "OnEvent", "Process", "OnEvent"]
    assert [ln.split()[3] for ln in tr.lines if ln.startswith("C Input")] == ["0010:00000002", "0010:00000001"]
    # `C -1` only for the mouse and the camera
    assert [ln.split()[3] for ln in log if ln.startswith("C -1")] == ["0010:0000001a", "0010:00000006"]
    assert log[0] == "K      0" and [ln[:8] for ln in log[1:]] == ["C -1 OnE", "C -1 OnE"]


def test_mouse_calls_equal_the_mousespec_expansion_line_for_line():
    spec = "450,330@1 lmb+@2 450,370@3 lmb-@4 rmb+@4"
    vm, ctx, tr, log = make(ORDER)
    assert vm.call("main", []).ok
    events = {}
    for f, args in osi_keys.expand_mouse(spec, 6):
        events.setdefault((f, "mouse"), []).append(("OnEvent", [Variant(*a) for a in args]))
    host = hsm.FrameHost(vm, tr, ctx, events, camera_model=False)
    for k in range(6):
        assert host.frame(k)
    got = [ln for ln in log if ln.startswith("C -1")]
    want = []
    for f, args in osi_keys.expand_mouse(spec, 6):
        want.append("C -1 OnEvent %s%s" % (" ".join("%04x:%08x" % a for a in args), " 000f:00000000" * (10 - len(args))))
    assert got == want and len(got) == 6 + 3 + 1 + 1                      # 26 each frame; 23, 24, 25 (lmb); 23 (rmb)


def tiny_osi(tmp_path):
    img, _ = osiasm.assemble(ORDER.replace(".function main 0", ".function setup 0"))
    p = tmp_path / "tiny.osi"
    p.write_bytes(img)
    return p


def run_oracle(*args):
    return subprocess.run([sys.executable, str(ORACLE), *map(str, args)], capture_output=True, text=True, timeout=300)


def test_the_run_cli_takes_a_mousespec_and_writes_c_minus_1_that_a_replay_asserts(tmp_path):
    osi = tiny_osi(tmp_path)
    spec = "450,330@1 lmb+@2 450,370@3 lmb-@4"
    live = run_oracle("--run", osi, "--frames", 6, "--mouse", spec, "--log-out", tmp_path / "n.log", "--digest", tmp_path / "live.dig")
    assert live.returncode == 0 and "stop: none" in live.stdout, live.stdout + live.stderr
    lines = (tmp_path / "n.log").read_text().splitlines()
    assert sum(ln.startswith("C -1 OnEvent 0010:0000001a ") for ln in lines) == 6
    # the replay runs the same MOUSESPEC itself and asserts each call against the log: the D lines are equal, and it writes no Z
    rep = run_oracle("--run", osi, "--frames", 6, "--mouse", spec, "--replay", tmp_path / "n.log", "--digest", tmp_path / "rep.dig")
    assert rep.returncode == 0, rep.stdout + rep.stderr
    live_d = [ln for ln in (tmp_path / "live.dig").read_text().splitlines() if ln.startswith("D")]
    assert live_d == [ln for ln in (tmp_path / "rep.dig").read_text().splitlines() if ln.startswith("D")]
    assert not [ln for ln in (tmp_path / "rep.dig").read_text().splitlines() if ln.startswith("Z")]
    assert [ln for ln in (tmp_path / "live.dig").read_text().splitlines() if ln.startswith("Z")]
    # another MOUSESPEC is a desync (the replay's own host disagrees with the record)
    bad = run_oracle("--run", osi, "--frames", 6, "--mouse", "451,330@1 lmb+@2 450,370@3 lmb-@4", "--replay", tmp_path / "n.log")
    assert bad.returncode == 1 and "replay desync at a host call" in bad.stdout, bad.stdout
    # no MOUSESPEC: no mouse source, so the replay would meet a record its host never makes
    none = run_oracle("--run", osi, "--frames", 6, "--replay", tmp_path / "n.log")
    assert none.returncode == 1 and "replay desync" in none.stdout


def test_no_mousespec_means_no_mouse_source_and_no_c_minus_1(tmp_path):
    osi = tiny_osi(tmp_path)
    r = run_oracle("--run", osi, "--frames", 4, "--log-out", tmp_path / "n.log")
    assert r.returncode == 0 and not [ln for ln in (tmp_path / "n.log").read_text().splitlines() if ln.startswith("C ")]


def test_a_bad_mousespec_is_refused():
    with pytest.raises(ValueError):
        osi_keys.expand_mouse("450,330@1 450,331@1", 3)                  # two position tokens at one frame (hud.md §11)


# --- the digest's Z block (trace.md §2)
def test_the_digest_keeps_z_apart_from_d_and_t_covers_every_line(tmp_path):
    dig = osi_vm.DigestTrace(tmp_path / "d")
    lines = ["# lomn-osi-trace 1 x", "K      0 00000000 00000000 00000000", "I 00000000 46    1    1 00ff:00000000",
             "Z      0", "Z ref 0027:00000001 1", "K      1 00000000 00000000 00000000", "Z      1"]
    for ln in lines:
        dig.line(ln)
    dig.close()
    out = (tmp_path / "d").read_text().splitlines()
    k0 = ("K      0 00000000 00000000 00000000\nI 00000000 46    1    1 00ff:00000000\n").encode()
    assert out[0] == lines[0]
    assert out[1] == "D      0        2 %016x" % osi_vm.fnv1a64(k0)                       # the D count and hash exclude the Z lines
    assert out[2] == "Z      0        2 %016x" % osi_vm.fnv1a64(b"Z      0\nZ ref 0027:00000001 1\n")
    assert out[3].startswith("D      1        1 ") and out[4] == "Z      1        1 %016x" % osi_vm.fnv1a64(b"Z      1\n")
    whole = osi_vm.fnv1a64("".join(ln + "\n" for ln in lines).encode())
    assert out[5] == "T 7 %016x" % whole


# ===============================================================================================================
# 4. A6 / A9 D lines are unchanged (Decision 12) and the order change is not visible to them
# ===============================================================================================================
def legacy_digest(script_path, frames, keys, real, install, tmp_path):
    """The M2a-2 `--run` loop, verbatim in its behaviour (every key in KEYSPEC order, Process passes, no mouse, no camera, no Z)
    run on the same registry: its D lines are the reference for the new loop's D lines."""
    from fractions import Fraction as F
    script = Path(script_path).read_bytes()
    reg = osi_vm.NativeRegistry.load(osi_vm.DEFAULT_NATIVES, osi_vm.DEFAULT_MODS, TYPES)
    osi_vm.apply_overrides(reg, REPO / "data" / "osi" / "native_overrides.json")
    reg.return_log = []
    path = tmp_path / "legacy.dig"
    trace = osi_vm.DigestTrace(path)
    trace.line(osi_vm.header_line(script, osi_vm.DEFAULT_NATIVES, osi_vm.DEFAULT_MODS, osi_vm.DEFAULT_TYPES,
                                  " keys=%s" % keys.replace(" ", ",")))
    heap = osi_vm.Heap(trace)
    vm = osi_vm.Vm(script, reg, heap, trace)
    if real:
        ctx = framework.HostContext(vm, heap, framework.Vfs(install), framework.HostFlags(), reg.return_log)
        framework.bind_natives(reg, ctx)
    calls = osi_vm.key_calls(keys)
    dt = struct.unpack("<d", struct.pack("<Q", osi_vm.DT_BITS_DOUBLE))[0]
    time, pm_flag, proc_byte = 0.0, 0, 1
    assert vm.call("Setup", []).ok
    for k in range(frames):
        time = float(round_p(F(time) + F(dt), vm.fpu_pc))
        tb, db = osi_vm.f32_bits(time), osi_vm.f32_bits(dt)
        trace.line("K %6d %08x %08x %08x" % (k, tb, db, k))
        vm.begin_frame(k)
        for name, args in calls.get(k, []):
            assert vm.call(name, args).ok
        pm_flag ^= 1
        passes = 0
        while proc_byte == pm_flag:
            r = vm.call("Process", [Variant(T_FLOAT, tb), Variant(T_FLOAT, db), Variant(T_INT, k)])
            passes += 1
            assert r.ok
            if not (r.result.type == T_INT and r.result.value == 0):
                proc_byte = pm_flag ^ 1
                break
            if passes >= 11:
                break
    trace.close()
    return path.read_text().splitlines(), "\n".join(reg.return_log)


@pytest.mark.parametrize("real", [False, True])
def test_the_a6_and_a9_d_lines_and_natives_log_are_unchanged(lomn_data, tmp_path, real):
    install, base, keys = lomn_data / "install", lomn_data / "install" / "data" / "Base.osi", "down@100 down@160 enter@220 esc@280"
    ref, ref_log = legacy_digest(base, 300, keys, real, install, tmp_path)
    args = ["--run", base, "--frames", 300, "--keys", keys, "--digest", tmp_path / "new.dig", "--log-out", tmp_path / "new.log",
            "--overrides", REPO / "data" / "osi" / "native_overrides.json"] + (["--real-natives", "--install", install] if real else [])
    r = run_oracle(*args)
    assert r.returncode == 0 and "stop: none" in r.stdout, r.stdout + r.stderr
    new = (tmp_path / "new.dig").read_text().splitlines()
    assert [ln for ln in new if ln.startswith("D")] == [ln for ln in ref if ln.startswith("D")]
    assert (tmp_path / "new.log").read_text().rstrip("\n") == ref_log.rstrip("\n")            # no C -1 before frame 604
    assert len([ln for ln in new if ln.startswith("Z")]) == 300 and not [ln for ln in ref if ln.startswith("Z")]


# ===============================================================================================================
# 5. Base.osi: the real handler, F_evt, F_act, the replay (the install)
# ===============================================================================================================
@pytest.fixture(scope="module")
def base_run(lomn_data, tmp_path_factory):
    """Setup + 607 frames with the real natives, trace and natives.log kept (no keys, no mouse)."""
    d = tmp_path_factory.mktemp("base")
    base, install = lomn_data / "install" / "data" / "Base.osi", lomn_data / "install"
    r = run_oracle("--run", base, "--frames", 607, "--real-natives", "--install", install, "--overrides",
                   REPO / "data" / "osi" / "native_overrides.json", "--trace", d / "t.trace", "--log-out", d / "n.log")
    assert r.returncode == 0 and "stop: none" in r.stdout, r.stdout
    return d


def test_base_osi_f_evt_is_604_and_f_act_is_605(base_run):
    frame, first_evt, first_pc = None, None, None
    for ln in (base_run / "t.trace").read_text().splitlines():
        if ln.startswith("K "):
            frame = int(ln.split()[1])
        elif ln.startswith("C OnEvent 10 0010:00000006 ") and first_evt is None:
            first_evt = frame
        elif ln.startswith("I 0003854d") and first_pc is None:              # pc 230733: menuactive = 1 (plan F2)
            first_pc = frame
    assert (first_evt, first_pc) == (604, 605)
    assert event_frames((base_run / "n.log").read_text().splitlines()) == [604]


def test_base_osi_camera_z_lines_at_the_fire_frame_match_the_pins_of_hud_md_1_7(base_run):
    z = []
    for ln in (base_run / "t.trace").read_text().splitlines():
        if ln == "Z    604":
            z = []
        elif z is not None and ln.startswith("Z ") and not ln.startswith("Z    "):
            z.append(ln)
        elif ln.startswith("K "):
            if z and any(x.startswith("Z camera.eye") for x in z):
                break
            z = []
    # frame 604's block was collected by the last pass: re-read it exactly
    text = (base_run / "t.trace").read_text()
    block = text.split("Z    604\n")[1].split("\nK ")[0].splitlines()
    assert "Z camera.eye c1a617c2 4178c711 42dd472b" in block and "Z camera.target c1a617c2 4178c711 42cc8396" in block
    assert "Z camera.mode 00" in block and "Z camera.spline -" in block                 # the real handler set mode 0 and released it
    assert "Z camera.index 00 03" in block and "Z camera.duration 4010000000000000" in block and "Z camera.flags 01 01" in block
    assert any(x.startswith("Z ref 002a:") and x.endswith(" 1") for x in block)         # the camera: one engine reference


def test_base_osi_real_handler_never_sends_onevent6_again_through_frame_2400(lomn_data, tmp_path):
    # (b) hud.md §1.4: the real handler sets camera AI mode 0, which freezes the camera on the 0x452346 -> 0x45ab60 path
    r = run_oracle("--run", lomn_data / "install" / "data" / "Base.osi", "--frames", 2401, "--real-natives", "--install",
                   lomn_data / "install", "--overrides", REPO / "data" / "osi" / "native_overrides.json", "--log-out", tmp_path / "n.log")
    assert r.returncode == 0 and "stop: none" in r.stdout
    assert event_frames((tmp_path / "n.log").read_text().splitlines()) == [604]


def test_base_osi_replay_makes_the_camera_call_from_its_own_host_and_asserts_it(lomn_data, base_run, tmp_path):
    base, install = lomn_data / "install" / "data" / "Base.osi", lomn_data / "install"
    live = run_oracle("--run", base, "--frames", 607, "--real-natives", "--install", install, "--overrides",
                      REPO / "data" / "osi" / "native_overrides.json", "--digest", tmp_path / "live.dig")
    rep = run_oracle("--run", base, "--frames", 607, "--replay", base_run / "n.log", "--overrides",
                     REPO / "data" / "osi" / "native_overrides.json", "--digest", tmp_path / "rep.dig")
    assert live.returncode == 0 and rep.returncode == 0, rep.stdout
    d = lambda p: [ln for ln in p.read_text().splitlines() if ln.startswith("D")]       # noqa: E731
    assert d(tmp_path / "live.dig") == d(tmp_path / "rep.dig")
    # a log without the camera's record leaves the replay's host nothing to assert against: it never makes the call
    lines = (base_run / "n.log").read_text().splitlines()
    cut = [i for i, ln in enumerate(lines) if ln.startswith("C -1")][0]
    (tmp_path / "cut.log").write_text("\n".join(lines[:cut] + lines[cut + 1:]) + "\n")
    bad = run_oracle("--run", base, "--frames", 607, "--replay", tmp_path / "cut.log", "--overrides",
                     REPO / "data" / "osi" / "native_overrides.json")
    assert bad.returncode == 1 and "replay desync" in bad.stdout


# ===============================================================================================================
# 6. Conformance group h on the Python runner (conformance.md "Host-event fields", rules 21-23)
# ===============================================================================================================
H_RUNNABLE = ["h-cminus1-order", "h-mouse-every-frame", "h-z-ref"]


def run_h(name, expect=None, log=None, zout=None, trace_dir=None, tmp_path=None):
    """Runs corpus program h/<name> (optionally with its expect.json replaced by `expect`) on the oracle."""
    if expect is None:
        return osi_vm.run_program(H_DIR / (name + ".asm"), trace_dir, log=log, zout=zout)
    shutil.copy(H_DIR / (name + ".asm"), tmp_path / (name + ".asm"))
    (tmp_path / (name + ".expect.json")).write_text(json.dumps(expect))
    return osi_vm.run_program(tmp_path / (name + ".asm"), trace_dir, log=log, zout=zout)


def spec_of(name):
    return json.loads((H_DIR / (name + ".expect.json")).read_text())


@pytest.mark.parametrize("name", H_RUNNABLE)
def test_group_h_programs_pass_on_the_python_runner(name):
    assert run_h(name) == []


def test_group_h_conform_summary_runs_three_and_leaves_h_z_hud_pending(capsys):
    assert osi_vm.conform(H_DIR) == 0
    out = capsys.readouterr().out.splitlines()
    assert out == ["PENDING h-z-hud (T4a/T4b/T6a/T6b)", "conformance: 3/3 pass, 1 pending"]


def test_the_pending_marks_are_untouched_so_the_cpp_runner_keeps_skipping_them():
    import conform_pending
    assert [conform_pending.pending_task(H_DIR / (n + ".expect.json")) for n in H_RUNNABLE] == ["T4a/T4b"] * 3
    assert conform_pending.pending_task(H_DIR / "h-z-hud.expect.json") == "T4a/T4b/T6a/T6b"


@pytest.mark.parametrize("task, pending", [("T4a/T4b", False), ("T4a", False), ("T4b", False), ("T4a/T4b/T6a/T6b", True), ("T3/T4", True),
                                           ("T6b", True), ("T4b/T7b", True), ("", False), (None, False)])
def test_the_python_runner_ignores_cpp_only_and_done_task_ids(tmp_path, task, pending):
    p = tmp_path / "x.expect.json"
    p.write_text(json.dumps({"id": "x"} if task is None else {"id": "x", "pending": task}))
    assert (osi_vm._python_pending(p) is not None) is pending


def test_group_h_natives_log_order_of_one_frame():
    log = []
    assert run_h("h-cminus1-order", log=log) == []
    cam = "C -1 OnEvent 0010:00000006 002a:00000001 G 1 0010:00000000" + " 000f:00000000" * 7
    mouse = "C -1 OnEvent 0010:0000001a 0010:000001c2 0010:0000014a" + " 000f:00000000" * 7
    assert log == ["K      0", mouse, cam]                                       # VK and key-mapper Input write nothing
    assert not [ln for ln in log if ln.startswith("H ")]


def test_group_h_trace_order_is_the_process_map_order_whatever_the_list_order(tmp_path):
    td = tmp_path / "t"
    assert run_h("h-cminus1-order", trace_dir=td) == []
    calls = [ln.split() for ln in (td / "h-cminus1-order.trace").read_text().splitlines() if ln.startswith("C ")]
    assert [c[1] for c in calls] == ["main", "Input", "Input", "OnEvent", "Process", "OnEvent"]
    assert [c[4] for c in calls[1:3]] == ["0010:0000000d", "0010:000000fb"]       # the VK (Enter) before the key mapper (Esc)
    assert [c[3] for c in calls[3:4] + calls[5:]] == ["0010:0000001a", "0010:00000006"]
    ex = spec_of("h-cminus1-order")
    ex["host_events"].reverse()                                                # the list order does not decide (rule 23)
    assert run_h("h-cminus1-order", ex, tmp_path=tmp_path) == []


def test_group_h_expectations_bite(tmp_path):
    # a corrupted expectation fails: the order of the frame's calls (list order would give 53214), the Z block
    ex = spec_of("h-cminus1-order")
    ex["expect"]["globals"]["g"]["value"] = 53214
    assert run_h("h-cminus1-order", ex, tmp_path=tmp_path) != []
    ex = spec_of("h-cminus1-order")
    ex["expect"]["z"]["0"] = ["Z ref 002a:00000001 2"]                          # an unreleased host +1
    assert any(f.startswith("z[0]") for f in run_h("h-cminus1-order", ex, tmp_path=tmp_path))
    ex["expect"]["z"] = {"1": []}                                              # a frame that was not run
    assert run_h("h-cminus1-order", ex, tmp_path=tmp_path) == ["z[1]: that frame was not run"]


def test_group_h_mouse_every_frame_bites_on_the_mousespec(tmp_path):
    for bad in ("450,330@1 lmb+@2 450,370@3", "450,330@1 lmb+@3 450,370@3 lmb-@4", "lmb+@2 450,330@1 450,370@3 lmb-@4 450,1@5"):
        ex = spec_of("h-mouse-every-frame")
        ex["mouse"] = bad
        assert run_h("h-mouse-every-frame", ex, tmp_path=tmp_path) != [], bad


def test_group_h_z_ref_equals_the_running_sum_of_the_h_records():
    log, z = [], {}
    assert run_h("h-z-ref", log=log, zout=z) == []
    # the engine's references of a handle = the G of the record that minted it plus every H since (natives.md §3.1)
    ref, order = {}, []
    for ln in log:
        m = re.match(r"\d{8} t\.(make|held2) 0 -> (\w+:\w+) G (\d+)", ln)
        if m:
            ref[m.group(2)] = int(m.group(3))
            order.append(m.group(2))
        m = re.match(r"H (\w+:\w+) ([+-]\d)", ln)
        if m:
            ref[m.group(1)] += int(m.group(2))
    assert sorted(ref.items()) == [("0027:00000001", 1), ("0027:00000002", 3)]
    assert z[0] == ["Z ref %s %d" % (h, n) for h, n in sorted(ref.items())]


def test_group_h_unknown_things_fail_before_running_with_no_trace(tmp_path):
    td = tmp_path / "t"
    cases = [("bad position x", lambda e: e["host_events"].append({"frame": 0, "position": "x", "name": "OnEvent", "args": []})),
             ("bad natives_impl v", lambda e: e.update({"natives_impl": "v"})),
             ("then with frames", lambda e: e.update({"then": {"entry": "main"}}))]
    for want, mutate in cases:
        ex = spec_of("h-cminus1-order")
        mutate(ex)
        assert run_h("h-cminus1-order", ex, trace_dir=td, tmp_path=tmp_path) == [want], want
    assert not list(td.glob("*.trace"))
    ex = spec_of("h-z-ref")
    ex["host_events"] = [{"frame": 5, "position": "mouse", "name": "OnEvent", "args": []}]
    assert run_h("h-z-ref", ex, tmp_path=tmp_path)[0].startswith("host event frame 5")
    ex = spec_of("h-mouse-every-frame")
    ex["mouse"] = "garbage"
    assert run_h("h-mouse-every-frame", ex, tmp_path=tmp_path)[0].startswith("bad mouse")


def test_group_h_refs_need_a_game_argument(tmp_path):
    ex = spec_of("h-z-ref")
    ex["natives"][2]["refs"] = [{"arg": 3, "delta": 1}]                         # t.hold has one argument
    assert run_h("h-z-ref", ex, tmp_path=tmp_path) == ["refs arg 3 is not a game object"]


def test_a_group_h_program_with_an_error_in_a_frame_fails_with_its_frame(tmp_path):
    ex = spec_of("h-z-ref")
    asm = (H_DIR / "h-z-ref.asm").read_text().replace("proc:\n", "proc:\n" + "    Pop\n" * 5)
    (tmp_path / "e.asm").write_text(asm)
    (tmp_path / "e.expect.json").write_text(json.dumps(ex))
    fails = osi_vm.run_program(tmp_path / "e.asm")
    assert len(fails) == 1 and fails[0].startswith("frame 0: error")
