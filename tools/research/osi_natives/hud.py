"""M2a-3 T6b: the HUD setters (hud.md §4.1-§4.4) as tier 1, the HUD director's show/hide (§4.2), and the `Z hud/sprite/
text/font` lines of hud.md §9.2. Written from docs/osi/hud.md, never from engine/. The state is kept per game handle as
the printed value of each field, so an object a conformance test native minted (no host object) is known too, with
every field `-` until a setter writes it."""
from __future__ import annotations

import struct

from osi_vm import NOTHING, T_NOTHING, T_STRING, VmError

from . import services
from .framework import register_group
from .objects import HudDirector, ScTileFont, ScTextObject

M32 = 0xFFFFFFFF
SPRITE = ("alpha", "b6", "color", "file", "rect", "zdepth")
TEXT = ("font", "just", "juston", "pos", "scale", "string")
FONT = ("color", "file", "transp")
K255 = struct.unpack("<f", struct.pack("<I", 0x3B808081))[0]    # [0x703c90..] (sprite) / [0x7581cc..] (font)


class HudModel:
    def __init__(self, ctx):
        self.sprite_type = self.text_type = self.font_type = 0
        self.fields: dict[int, dict[str, str]] = {}                # handle -> field -> printed value (unset: `-`)
        self.shown: list[int] = []                                 # mainHud's child vector (hud.md §3.1), node ids


def _hex32(*v) -> str:
    return " ".join("%08x" % (x & M32) for x in v)


def _bytes(b: bytes) -> str:
    """As on `S` lines (trace.md §1): `%d <hex>`, empty `0`."""
    return "%d %s" % (len(b), b.hex()) if b else "0"


def _handle(vm, h: int) -> str:
    return "%04x:%08x" % (vm.heap.game_type(h) & 0xFFFF, h)


def _colour(x: int) -> int:
    """f32(x * f32 0x3b808081): the exact product fits a double for a byte or |x| < 2^29, so one rounding."""
    return struct.unpack("<I", struct.pack("<f", x * K255))[0]


def _w(call, i: int) -> int:
    return call.arg(i).value & M32 if i < len(call.slots) else 0


def _self(call, what: str) -> int:
    """`this` = w0 with no null test: nothing is the original's null dereference."""
    if not call.slots or call.arg(0).type == T_NOTHING:
        raise VmError("Undefined: null object in " + what)
    return call.arg(0).value & M32


def _setter(ctx, what: str, field: str, value):
    def fn(call):
        ctx.subsystem(HudModel).fields.setdefault(_self(call, what), {})[field] = value(call)
        return NOTHING
    return fn


def created_sprite(ctx, h: int, rect, name: bytes, b6: int) -> None:
    ctx.subsystem(HudModel).fields[h] = {"rect": _hex32(*rect), "file": _bytes(name), "b6": "%02x" % (b6 & 0xFF)}


def created_text(ctx, vm, h: int, pos, font, text: bytes) -> None:
    ctx.subsystem(HudModel).fields[h] = {"pos": _hex32(*pos), "string": _bytes(text),
                                         "font": _handle(vm, font.handle) if font is not None and font.handle else "-"}


def created_font(ctx, h: int, name: bytes) -> None:
    ctx.subsystem(HudModel).fields[h] = {"file": _bytes(name)}


def _setstring(ctx):
    def fn(call):
        h = _self(call, "sctextobject.setstring")
        if len(call.slots) < 2 or call.arg(1).type != T_STRING:
            raise VmError("Undefined: string argument expected")
        ctx.subsystem(HudModel).fields.setdefault(h, {})["string"] = _bytes(
            services.c_string(call.vm.heap.text(call.arg(1).value)))         # ScFixedString operator= 0x611f57
        return NOTHING
    return fn


def _setfont(ctx):
    """0x611f90: the same font -> nothing (0x611fa9); else +1 on the new one (0x611fb1), -1 on the old one (0x611fbd,
    freed at 0), store +0x2c (0x611fce). A null font prints `-`."""
    def fn(call):
        h = _self(call, "sctextobject.setfont")
        nf = ctx.object_arg(call, 1, ScTileFont)
        t = ctx._objects.get(h)
        t = t if isinstance(t, ScTextObject) else None
        of = t.font if t is not None else None
        if t is not None and of is nf:
            return NOTHING
        if nf is not None:
            ctx.add_ref(nf)
        if t is not None:
            t.font = nf
        if of is not None:
            ctx.release(of)
        ctx.subsystem(HudModel).fields.setdefault(h, {})["font"] = (
            _handle(call.vm, nf.handle) if nf is not None and nf.handle else "-")
        return NOTHING
    return fn


def _font_setcolor(ctx):
    def fn(call):
        h = _self(call, "sctilefont.setcolor")
        v = []
        for i in range(1, 5):
            x = _w(call, i)
            x = x - (1 << 32) if x & 0x80000000 else x                      # fild dword: signed
            if abs(x) >= 1 << 29:
                raise VmError("unmodelled: sctilefont.setcolor component beyond 2^29")
            v.append(_colour(x))
        ctx.subsystem(HudModel).fields.setdefault(h, {})["color"] = _hex32(*v)
        return NOTHING
    return fn


# GcHudDirector::Show 0x47b320 / Hide 0x47b3e0: the id is looked up in the item table (a miss returns 0 at 0x650673 and
# the caller reads [0+0x10]: Undefined); mainHud is asked whether a child holds the node's object ([node+0x10]). Show:
# held -> nothing (0x47b36f), else push_back (0x47b3a6). Hide: not held -> nothing (0x47b42f), else the node itself is
# erased from the vector if present (0x47b46b-0x47b4a7).
def _node_object(ctx, ident: int, what: str):
    for item, i in ctx.subsystem(HudDirector).items:
        if i == ident:
            return item.obj
    raise VmError("Undefined: %s of an unknown HUD id (0x650673 returns 0)" % what)


def _shown(ctx, obj) -> bool:
    objs = {i: item.obj for item, i in ctx.subsystem(HudDirector).items}
    return any(objs[i] is obj for i in ctx.subsystem(HudModel).shown)


def _show(ctx):
    def fn(call):
        ident = _w(call, 0)
        obj = _node_object(ctx, ident, "GcHudDirector::Show")
        if not _shown(ctx, obj):
            ctx.subsystem(HudModel).shown.append(ident)
        return NOTHING
    return fn


def _hide(ctx):
    def fn(call):
        ident = _w(call, 0)
        obj = _node_object(ctx, ident, "GcHudDirector::Hide")
        shown = ctx.subsystem(HudModel).shown
        if _shown(ctx, obj) and ident in shown:
            shown.remove(ident)
        return NOTHING
    return fn


def z_lines(vm, ctx) -> list[str]:
    """The `Z hud.node`, `Z sprite.*`, `Z text.*`, `Z font.*` lines (unsorted)."""
    m = ctx.subsystem(HudModel)
    out = []
    for h in sorted(vm.heap.games):
        t = vm.heap.game_type(h) & 0xFFFF
        kind, keys = {m.sprite_type: ("sprite", SPRITE), m.text_type: ("text", TEXT),
                      m.font_type: ("font", FONT)}.get(t, (None, None)) if t else (None, None)
        if kind is None:
            continue
        f = m.fields.get(h, {})
        out += ["Z %s.%s %s %s" % (kind, k, _handle(vm, h), f.get(k, "-")) for k in keys]
    for item, ident in ctx.subsystem(HudDirector).items:
        o = item.obj
        order = str(m.shown.index(ident)) if ident in m.shown else "-"
        if o is not None and o.handle:
            kind = "text" if (vm.heap.game_type(o.handle) & 0xFFFF) == m.text_type else "sprite"
            obj = _handle(vm, o.handle)
        else:
            kind = obj = "-"
        out.append("Z hud.node %08x %s %s %s" % (ident & M32, kind, obj, order))
    return out


def bind(reg, ctx) -> None:
    m = ctx.subsystem(HudModel)
    m.sprite_type, m.text_type, m.font_type = (reg.types.id(n) for n in ("GcBigSprite", "ScTextObject", "ScTileFont"))
    byte1 = lambda c: "%02x" % (_w(c, 1) & 0xFF)                                  # noqa: E731
    rows = {
        "gcbigsprite.setzdepth": _setter(ctx, "gcbigsprite.setzdepth", "zdepth", lambda c: _hex32(_w(c, 1))),
        "gcbigsprite.enablealphachannel": _setter(ctx, "gcbigsprite.enablealphachannel", "alpha", byte1),
        "gcbigsprite.sethudposition": _setter(ctx, "gcbigsprite.sethudposition", "rect",
                                              lambda c: _hex32(*(_w(c, i) for i in range(1, 5)))),
        "gcbigsprite.setcolor": _setter(ctx, "gcbigsprite.setcolor", "color",
                                        lambda c: _hex32(*(_colour(_w(c, i) & 0xFF) for i in range(1, 5)))),
        "sctextobject.setposition": _setter(ctx, "sctextobject.setposition", "pos",
                                            lambda c: _hex32(*(_w(c, i) for i in range(1, 4)))),
        "sctextobject.setscale": _setter(ctx, "sctextobject.setscale", "scale", lambda c: _hex32(_w(c, 1), _w(c, 2))),
        "sctextobject.setjustification": _setter(ctx, "sctextobject.setjustification", "just",
                                                 lambda c: "%04x" % (_w(c, 1) & 0xFFFF)),
        "sctextobject.enablejustification": _setter(ctx, "sctextobject.enablejustification", "juston", byte1),
        "sctextobject.setstring": _setstring(ctx),
        "sctextobject.setfont": _setfont(ctx),
        "sctilefont.setcolor": _font_setcolor(ctx),
        "sctilefont.enabletransparency": _setter(ctx, "sctilefont.enabletransparency", "transp", byte1),
        "gchuddirector.show": _show(ctx),
        "gchuddirector.hide": _hide(ctx),
    }
    for k, f in rows.items():
        reg.bind(k, f)


register_group("hud", bind)
