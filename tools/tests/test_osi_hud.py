"""M2a-3 T6b: the HUD setters, show/hide and the Z hud/sprite/text/font lines (hud.md §4, §9.2), oracle side.
Expected values are written from hud.md, with the colour constant computed here in exact rationals."""
import struct
import sys
from fractions import Fraction
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(REPO / "tools" / "research"), str(REPO / "tools" / "osi")]

import osiasm  # noqa: E402
import osi_vm  # noqa: E402
from osi_natives import framework  # noqa: E402
from osi_natives import host_sources as hsm  # noqa: E402

TYPES = osi_vm.TypeTable.load(REPO / "data" / "osi" / "types.json")
FONT_T, TEXT_T, SPRITE_T = (TYPES.id(n) for n in ("ScTileFont", "ScTextObject", "GcBigSprite"))


def f32(x) -> str:
    return "%08x" % osi_vm._f32_bits_fraction(Fraction(x))


def colour(x) -> str:
    """f32(x * f32 0x3b808081), one rounding of the exact product (hud.md §4.1/§4.4)."""
    k = Fraction(struct.unpack("<f", struct.pack("<I", 0x3B808081))[0])
    return "%08x" % osi_vm._f32_bits_fraction(x * k)


SETUP = """.globals f g t s i j
.function main 0
    PushConstantString "Fnt"
    CallGameFunction sctilefont.create 1
    SetVariableValue global:f
    PushConstantString "Fnt2"
    CallGameFunction sctilefont.create 1
    SetVariableValue global:g
    PushConstantf32 400.0
    PushConstantf32 320.0
    PushConstantf32 0.0
    GetVariableValue global:f
    PushConstantString "Play"
    CallGameFunction sctextobject.create 5
    SetVariableValue global:t
    PushConstantf32 1.0
    PushConstantf32 2.0
    PushConstantf32 3.0
    PushConstantf32 4.0
    PushConstantString "Spr"
    PushConstanti8 1
    PushConstanti8 0
    CallGameFunction gcbigsprite.loadhudsprite 7
    SetVariableValue global:s
    GetVariableValue global:s
    CallGameFunction gchuddirector.addbigsprite 1
    SetVariableValue global:i
    GetVariableValue global:t
    CallGameFunction gchuddirector.addtextobject 1
    SetVariableValue global:j
{body}
    PushConstanti8 1
    Return
"""


def run(body: str):
    image, _ = osiasm.assemble(SETUP.format(body=body))
    tr = osi_vm.ListTrace()
    heap = osi_vm.Heap(tr)
    reg = osi_vm.NativeRegistry.load(osi_vm.DEFAULT_NATIVES, osi_vm.DEFAULT_MODS, TYPES)
    vm = osi_vm.Vm(image, reg, heap, tr, fpu_pc=24)
    log = []
    ctx = framework.HostContext(vm, heap, None, framework.HostFlags(), log)
    framework.bind_natives(reg, ctx)
    r = vm.call("main", [])
    run.trace = tr.lines
    return r, [ln for ln in hsm.z_block(vm, ctx, False) if not ln.startswith("Z ref ")], log


def call(fn, *args, obj="global:t") -> str:
    lines = ["    GetVariableValue " + obj] if obj else []
    for a in args:
        lines.append("    " + a)
    return "\n".join(lines + ["    CallGameFunction %s %d" % (fn, len(args) + (1 if obj else 0)), "    Pop"])


def H(t, h):
    return "%04x:%08x" % (t, h)


def test_constructor_fields_and_unwritten_fields():
    r, z, _ = run("")
    assert r.ok
    f, g, t, s = H(FONT_T, 1), H(FONT_T, 2), H(TEXT_T, 3), H(SPRITE_T, 4)
    assert z == sorted([
        "Z font.color %s -" % f, "Z font.file %s 3 466e74" % f, "Z font.transp %s -" % f,
        "Z font.color %s -" % g, "Z font.file %s 4 466e7432" % g, "Z font.transp %s -" % g,
        "Z hud.node 00000000 sprite %s -" % s, "Z hud.node 00000001 text %s -" % t,
        "Z sprite.alpha %s -" % s, "Z sprite.b6 %s 01" % s, "Z sprite.color %s -" % s, "Z sprite.file %s 3 537072" % s,
        "Z sprite.rect %s %s %s %s %s" % (s, f32(1), f32(2), f32(3), f32(4)), "Z sprite.zdepth %s -" % s,
        "Z text.font %s %s" % (t, f), "Z text.just %s -" % t, "Z text.juston %s -" % t,
        "Z text.pos %s %s %s %s" % (t, f32(400), f32(320), f32(0)), "Z text.scale %s -" % t,
        "Z text.string %s 4 506c6179" % t,
    ], key=lambda x: x.encode())


def test_every_setter_stores_what_hud_md_4_says():
    body = "\n".join([
        call("gcbigsprite.setzdepth", "PushConstantf32 -5.0", obj="global:s"),
        call("gcbigsprite.enablealphachannel", "PushConstanti32 0x1ff", obj="global:s"),
        call("gcbigsprite.sethudposition", *("PushConstantf32 %s.0" % v for v in (5, 6, 7, 8)), obj="global:s"),
        call("gcbigsprite.setcolor", *("PushConstanti32 %d" % v for v in (0x1ff, 128, 0, 1)), obj="global:s"),
        call("sctextobject.setposition", "PushConstantf32 1.5", "PushConstantf32 2.5", "PushConstantf32 3.5"),
        call("sctextobject.setscale", "PushConstantf32 0.5", "PushConstantf32 0.25"),
        call("sctextobject.setjustification", "PushConstanti32 0x10002"),
        call("sctextobject.enablejustification", "PushConstanti32 0x101"),
        call("sctextobject.setstring", 'PushConstantString ""'),
        call("sctilefont.setcolor", *("PushConstanti32 %d" % v for v in (255, -1, 7, 300)), obj="global:f"),
        call("sctilefont.enabletransparency", "PushConstanti8 1", obj="global:f"),
    ])
    r, z, _ = run(body)
    assert r.ok, r
    f, t, s = H(FONT_T, 1), H(TEXT_T, 3), H(SPRITE_T, 4)
    want = {
        "Z sprite.zdepth %s %s" % (s, f32(-5)),
        "Z sprite.alpha %s ff" % s,                                                  # the low byte of w1
        "Z sprite.rect %s %s %s %s %s" % (s, f32(5), f32(6), f32(7), f32(8)),
        "Z sprite.color %s %s %s %s %s" % (s, colour(255), colour(128), colour(0), colour(1)),   # low bytes
        "Z text.pos %s %s %s %s" % (t, f32(1.5), f32(2.5), f32(3.5)),
        "Z text.scale %s %s %s" % (t, f32(0.5), f32(0.25)),
        "Z text.just %s 0002" % t, "Z text.juston %s 01" % t, "Z text.string %s 0" % t,
        "Z font.color %s %s %s %s %s" % (f, colour(255), colour(-1), colour(7), colour(300)),  # the whole int (fild)
        "Z font.transp %s 01" % f,
    }
    assert want <= set(z), sorted(want - set(z))


def test_show_appends_once_and_hide_erases_keeping_the_order():
    def show(v):
        return call("gchuddirector.show", obj="global:" + v)

    def hide(v):
        return call("gchuddirector.hide", obj="global:" + v)
    r, z, _ = run("\n".join([show("i"), show("j"), show("i"), hide("i"), show("i")]))
    assert r.ok
    nodes = [ln for ln in z if ln.startswith("Z hud.node")]
    assert nodes == ["Z hud.node 00000000 sprite %s 1" % H(SPRITE_T, 4), "Z hud.node 00000001 text %s 0" % H(TEXT_T, 3)]
    r, z, _ = run("\n".join([hide("j"), show("j"), hide("j"), hide("j")]))       # hiding a hidden node: nothing
    assert r.ok and "Z hud.node 00000001 text %s -" % H(TEXT_T, 3) in z


def test_show_of_an_unknown_id_is_undefined():
    r, _, _ = run(call("gchuddirector.show", "PushConstanti32 99", obj=None).replace(" 1\n    Pop", " 1\n    Pop"))
    assert not r.ok and any("unknown HUD id" in ln or "gchuddirector.show" in ln for ln in run.trace if ln.startswith("E"))


def test_setfont_adds_the_new_font_then_releases_the_old_and_ignores_the_same_font():
    same = call("sctextobject.setfont", "GetVariableValue global:f")
    r, z, log = run(same)
    assert r.ok and not [ln for ln in log if ln.startswith("H %s" % H(FONT_T, 1))][1:]   # only the create's +1
    r, z, log = run(call("sctextobject.setfont", "GetVariableValue global:g"))
    assert r.ok and "Z text.font %s %s" % (H(TEXT_T, 3), H(FONT_T, 2)) in z
    h = [ln for ln in log if ln.startswith("H ")]
    assert h[-2:] == ["H %s +1" % H(FONT_T, 2), "H %s -1" % H(FONT_T, 1)]


def test_a_null_object_and_a_huge_font_colour_fail():
    r, _, _ = run(call("sctextobject.setscale", "PushConstantf32 1.0", "PushConstantf32 1.0", obj="global:nothing_set")
                  .replace("global:nothing_set", "global:i").replace("GetVariableValue global:i", "PushNothing"))
    assert not r.ok and any("setscale" in ln for ln in run.trace if ln.startswith("E"))
    r, _, _ = run(call("sctilefont.setcolor", "PushConstanti32 0x20000000", "PushConstanti8 0", "PushConstanti8 0",
                       "PushConstanti8 0", obj="global:f"))
    assert not r.ok and any("setcolor" in ln for ln in run.trace if ln.startswith("E"))
