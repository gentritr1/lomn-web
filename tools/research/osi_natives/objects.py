"""M2a-2 T9b: the objects / model director / camera / HUD native group of the Python oracle. Written from
docs/osi/natives.md (§1.2 the ABI, §1.3 the citation format, §2 game objects and engine references, §3 natives.log,
§4 re-entry, §7 tiers) and from the asm of the canonical exe, read as data with objdump; never from engine/.

Every model below carries its citation in `CITES` (natives.md §1.3 form; one source for the code, the tests and
data/osi/native_overrides.json). A reading from the decomp alone is marked INFERRED; everything else was checked on the
instruction listing (tools/tests/test_osi_natives_objects.py pins each address). The decomp supplied names only.

What this group models (script-visible behaviour, Decision 6): the identity of the objects the natives hand out and the
engine references every native takes or drops on them (one `H` record per AddRef / Release event, temporaries
included), the nested `OnEvent` calls of `addtoa` and `setupmoveable`, and the state another native reads back (the
camera AI mode, the character's motion system, the model-director map, the HUD item index). What it does not model:
rendering and file formats (M2a-3), the Toa process path (the documented D8 host gap), the INI (`GcSaver` debug test
0x5d4e40: false on the owner's install, ini -> M2a-3) and the host clock (`GetTime` 0x4fa460).

Divergences from the plan text, found on the asm (reported to the controller):
- `gcmodeldirector.show('cnt1')` adds the Toa to a render group's child vector (temp AddRef 0x47f9df, push_back copy
  0x419a29, temp Release 0x47f9f9) as well as to the BAGroup (SetCharacter 0x54b344): the engine holds the Toa 3 times
  after `show`, not 2 as natives.md 2.4 / 3.4 say, and four H records are written, not one.
- `gccharacter.setupmoveable` only calls `OnEvent` if the character has a motion system (`[char+0x124]`, 0x418856).
  `gccharacter.initmotionsystem` (tier 2, no body, controller ruling fix 1) rebuilds it with the same id ([char+0x100]
  0x40c60b -> [ms+0x14] 0x5748ec; the ctor never calls OnEvent), so setupmoveable models the system as present.
- `gclegocamera.setcameraspline` / `settargetspline` hold the spline in the same field (+0x3a4) the AI-mode setter
  releases (0x45b5a8): they take engine references on a game argument, so they are tier 1 too.
"""
from __future__ import annotations

from typing import Optional

from . import framework, services
from .framework import GameObject, register_group
import osi_vm
from osi_vm import NOTHING, T_INT, Variant, VmError

M32 = 0xFFFFFFFF

ID_CNT1 = 0x636E7431                    # 'cnt1': [0x782884] (static init 0x482d43), [0x7cb1cc] (0x570be3), [0x7796f4] (0x43c193)
SC_RENDER_NODE_GROUP_ID = 0xCD92FE7B    # the class id (name hash 0x4f6af0, h = h * 0x83 + c) of "ScRenderNodeGroup" (0x71385b)
GC_CHARACTER_CLASS_ID = 0xAE7C7B09      # the class id of "GcCharacter" (0x6ffe32), the info 0x838b4c returned by 0x4196b0
TOA_INDEX = {0x74616875: 5, 0x6C657761: 4, 0x6B6F7061: 3, 0x706F6861: 2, 0x67616C69: 1, 0x6F6E7561: 0}

# --------------------------------------------------------------------------------------------------------------
# Citations (natives.md §1.3). The same text is the `cite` of the tier1 rows of data/osi/native_overrides.json.
# --------------------------------------------------------------------------------------------------------------
CITES = {
    "sctilefont.create":
        "// body 0x611910, method 0x626b40 <ScTileFont::ScTileFont>; asm pin: 0x611918 push 0x190, 0x61192c ctor call "
        "with the name argument, 0x626c24 name assign (npos) into a capacity-0xff string (0x626c11), 0x4f2b88 base "
        "count 0, 0x611936 CreateVariant 0x611950 (AddRef 0x611980)",
    "sctextobject.create":
        "// body 0x611cd0, method 0x610c40 <ScTextObject::ScTextObject>; asm pin: 0x611d17 the 5-argument ctor, 0x610c83 "
        "font stored at +0x2c, 0x610c92 inc [font+4] when the font is non-null, 0x610cce text assign (npos), 0x611d21 "
        "CreateVariant 0x611d40 (AddRef 0x611d70)",
    "gcbigsprite.loadhudsprite":
        "// body 0x43dad0, method 0x440460 <GcBigSprite::LoadHudSprite> -> 0x43f210 <GcBigSprite::GcBigSprite>; asm pin: "
        "0x440468 save the current block file, 0x440471 set it null, 0x44048a new 0x21c, 0x4404b0 ctor, 0x4404be restore, "
        "0x4404c4 returns the new pointer (null only if new fails), 0x43f890 w5 / 0x43fcc8 w6 flag bytes, 0x43f711 mount "
        "of the .col volume, 0x43f912 ReadMe.bin",
    "scslosislkcubicspline.create":
        "// body 0x61a2e0, method 0x61a4d0 <ScSLOSISLKCubicSpline::ScSLOSISLKCubicSpline> -> 0x61a580 <GetSLKFile>; asm "
        "pin: 0x61a2e8 push 0xc, 0x61a2f9 ctor call, 0x61a4ef member +8 = 0, 0x61a514 GetSLKFile of the name, 0x61a303 "
        "CreateVariant 0x61a320 (AddRef 0x61a350)",
    "gcmodeldirector.addtoa":
        "// body 0x47cc30, method 0x47e170 <GcModelDirector::AddToa> -> 0x567d10 <GcToa::GcToa>; asm pin: 0x47cc36 push 0 "
        "(second argument), 0x47e194 StringToIdentifier of the name, 0x47e1d0 the GcToa ctor, 0x567e27 [toa+0x380] = 0xfa, "
        "0x56898b ammo, 0x568a59 Call(OnEvent 0x32, [toa+0x380]) unconditional, 0x47e1f1 save + 0x419330 character block "
        "file, 0x47e221 [0x74749c], 0x47e4bd Add 0x480eb0, 0x47e4ca duplicate returns with no 0x54db20 restore, 0x47e527 "
        "[0x7032b8], 0x47e57d table to SetCurrentToa 0x58b9e0, 0x47e61e restore, 0x47e657 returns the inserted flag",
    "gcmodeldirector.gettoa":
        "// body 0x47cf80, method 0x480f70 <GcModelDirector::Get>; asm pin: 0x47cf86 key [0x782884] = 'cnt1' (0x482d43), "
        "0x47cf93 Get, 0x47cf9e CreateVariant 0x47cfb0 (null 0x47cfc3, AddRef 0x47cfe0, type GcToa)",
    "gcmodeldirector.getcharacter":
        "// body 0x47ce90, method 0x480f70 <GcModelDirector::Get>; asm pin: 0x47ceb4 Get of the id (argument 0; argument 1 "
        "is read and ignored), 0x47cebf CreateVariant 0x47ced0 (null 0x47cee3, AddRef 0x47cf00, type GcCharacter)",
    "gcmodeldirector.show":
        "// body 0x47cd70, method 0x47f830 <GcModelDirector::Show> -> 0x419890 <vector push_back>; asm pin: 0x47f855 Get, "
        "0x47f880 class test (ScRenderNodeGroup id [0x838b44]), 0x47f992 'cnt1' branch, 0x47f9b6 Contains 0x4abd60 on "
        "[0x8390a8], 0x47f9df temp AddRef, 0x47f9eb push_back (0x419a29 copy, 0x419a4e old elements released), 0x47f9f9 temp "
        "Release, 0x47fa41 the other group [0x83aa4c], 0x47fae2 shown-id list, 0x47fb09 SetShadowType, 0x47fb42 "
        "SetCharacter 0x54b330 (AddRef 0x54b344)",
    "gccharacter.setupmoveable":
        "// body 0x4093c0, method 0x418850 <GcCharacter::SetupMoveable> -> 0x574b80 <GcMotionSystem::SetupMoveable> -> "
        "0x589050 <GcMotionSystem::SetUnderWater>; asm pin: 0x418856 no motion system -> nothing, 0x574ef1 guard [ms+0x14] == "
        "MainCharacterID() 0x43af90, 0x574ef5 byte [0x73c5a8], 0x589154 OnEvent(0x36, 1) / 0x589249 OnEvent(0x36, 0)",
    "gclegocamera.getcameraaimode":
        "// body 0x44ed10, method 0x44ed18 <GcLegoCamera field +0x3a1>; asm pin: 0x44ed18 mov al, byte [edx+0x3a1], "
        "0x44ed40 CreateVariant movzx byte (type 0x10), initial 0 from the ctor 0x44f6a0",
    "gclegocamera.setcameraaimode":
        "// body 0x44ece0, method 0x45b490 <GcLegoCamera::SetCameraAIMode>; asm pin: 0x44ecee argument low byte, 0x45b4a7 "
        "store [cam+0x3a1], 0x706fd8 limit 0xf, 0x45b4fe mode 0xb sets +0x454 / +0x2f1, 0x45b53f modes 2 and 3 stamp the "
        "time, 0x706fdc / 0x706fe0 (2 and 3), 0x45b5a8 release of the spline holder [cam+0x3a4]",
    "gclegocamera.setcameraspline":
        "// body 0x44f180, method 0x45b380 <GcLegoCamera::SetCameraSpline>; asm pin: 0x45b38b same pointer -> no change, "
        "0x45b397 inc [new+4], 0x45b3a9 dec [old+4], 0x45b3b7 store [cam+0x3a4]",
    "gclegocamera.settargetspline":
        "// body 0x44f1e0, method 0x45b400 <GcLegoCamera::SetTargetSpline>; asm pin: 0x45b40b same pointer -> no change, "
        "0x45b417 inc [new+4], 0x45b429 dec [old+4], 0x45b437 store [cam+0x3a4]",
    "gchuddirector.addbigsprite":
        "// body 0x47afe0, method 0x47b730 <GcHudDirector::AddBigSprite> -> 0x47b940 <GcHudDirector::Add>; asm pin: 0x47b75a "
        "item ctor 0x4ac750, 0x4ac7d3 inc [sprite+4] when non-null, 0x47b94f id = counter [0x83aa3c] (initial 0 from "
        "[0x839124], 0x47cb64), 0x47b958 counter + 1, 0x47b995 refused -> 0, 0x47affd CreateVariant 0x407df0 (int)",
    "gchuddirector.addtextobject":
        "// body 0x47b010, method 0x47b780 <GcHudDirector::AddTextObject> -> 0x47b940 <GcHudDirector::Add>; asm pin: "
        "0x47b7aa item ctor 0x4ac750, 0x4ac7d3 inc [text object+4] when non-null, 0x47b94f id = counter [0x83aa3c], "
        "0x47b02d CreateVariant 0x407df0 (int)",
}

# The on-path natives of these classes that stay typed stubs: their return never reaches a sink (osi_taint verdict
# safe on the merged run) and they hold no engine reference on a game object.
TIER2_KEYS = [
    "gcbigsprite.enablealphachannel", "gcbigsprite.setcolor", "gcbigsprite.sethudposition", "gcbigsprite.setzdepth",
    "gc3dsound.synclistenertocamera", "gccharacter.disableallraycollisions", "gccharacter.initmotionsystem",
    "gccharacter.setbounce", "gccollisionphysicsgroup.settoaposition", "gcstatusscreen.hideinventory",
    "gchuddirector.hide", "gchuddirector.show",
    "gclegocamera.setcampos", "gclegocamera.settargpos",
    "gclightdirector.setcavelighting", "gclightdirector.toggleconstantlights",
    "gcmodeldirector.setnearestmax",
    "gctoa.addmask", "gctoa.setcodelife", "gctoa.switchmask",
    "sctextobject.enablejustification", "sctextobject.setjustification", "sctextobject.setscale", "sctextobject.setstring",
    "sctilefont.setcolor",
]


def _tid(ctx, name: str) -> int:
    return ctx.vm.natives.types.id(name)


def font_name(b: bytes) -> bytes:
    """The ScTileFont constructor assigns the name into a capacity-0xff string with npos (0x626c24 -> 0x401850): strlen,
    kOutOfRange at len >= 0xff. A script string is at most 254 bytes, so the bound is unreachable from a script."""
    return services.c_string(b)


# --------------------------------------------------------------------------------------------------------------
# Host objects. Each takes the registered type id of its class (the overload's type word); engine_refs is the number of
# references the engine holds before the object is first handed to the script (natives.md 2.4).
# --------------------------------------------------------------------------------------------------------------
class ScTileFont(GameObject):
    """ScTileFont (ctor 0x626b40, not ported: the font file is M2a-3). Only the constructor argument is kept."""

    def __init__(self, type_id: int, name: bytes = b""):
        super().__init__(type_id, 0)
        self.name = name


class ScTextObject(GameObject):
    """ScTextObject (ctor 0x610c40): holds its font at +0x2c with one engine reference, released by the destructor."""

    def __init__(self, ctx, type_id: int, x: float, y: float, z: float, font: Optional[GameObject], text: bytes):
        super().__init__(type_id, 0)
        self.ctx, self.x, self.y, self.z, self.font, self.text = ctx, x, y, z, font, text

    def on_delete(self) -> None:
        f, self.font = self.font, None
        if f is not None:
            self.ctx.release(f)                       # the member's destructor (dec [font+4]) after the deleting dtor starts


class GcBigSprite(GameObject):
    """GcBigSprite (ctor 0x43f210). Kept for M2a-3: the rectangle, the name, the two flag bytes and whether the source
    was found through the block-file oracle (Art.blk base name, then the slots, then loose files)."""

    def __init__(self, type_id: int, rect, name: bytes, flag5: int, flag6: int, source_present: bool):
        super().__init__(type_id, 0)
        self.rect, self.name, self.flag5, self.flag6, self.source_present = rect, name, flag5, flag6, source_present


class ScSLOSISLKCubicSpline(GameObject):
    def __init__(self, type_id: int, slk_name: bytes):
        super().__init__(type_id, 0)
        self.slk_name = slk_name


class GcMotionSystem:
    """GcMotionSystem (ctor 0x5742a0): not script-visible; `id` is [ms+0x14], the character id it was made for."""

    def __init__(self, ident: Optional[int]):
        self.id = ident


class GcCharacter(GameObject):
    """GcCharacter. char_id is [char+0x100], node_id the render node id [char+0x10], name_id [char+0xfc], motion_system
    [char+0x124], attached [char+0x1d0] (0 in the ctor, 0x40bbba)."""
    CLASS_INFO_ID = GC_CHARACTER_CLASS_ID

    def __init__(self, type_id: int, name_id: Optional[int] = None, char_id: Optional[int] = None, engine_refs: int = 0):
        super().__init__(type_id, engine_refs)
        self.name_id, self.char_id, self.node_id = name_id, char_id, 0x3F3F3F3F
        self.motion_system: Optional[GcMotionSystem] = None
        self.motion_systems_made = 0
        self.attached: Optional["GcCharacter"] = None
        self.parent = None
        self.shadow_type: Optional[int] = None


class GcToa(GcCharacter):
    """GcToa (ctor 0x567d10): ammo is [toa+0x380], 0xfa from its only writer 0x567e27."""

    def __init__(self, type_id: int, name_id: int):
        super().__init__(type_id, name_id, ID_CNT1)
        self.ammo = 0xFA
        self.collision_group = None


class GcLegoCamera(GameObject):
    """The camera node (ctor 0x44f3e0): AI mode byte +0x3a1 (0 from 0x44f6a0), spline holder +0x3a4 (null, 0x44f6ad)."""

    def __init__(self, type_id: int):
        super().__init__(type_id, 0)
        self.aimode = 0
        self.spline_holder: Optional[GameObject] = None
        self.flag454 = 0
        self.byte2f1 = 0
        self.entered_spline_mode = False
        self.spline_params = None


def _rehost(ctx, obj: GameObject, cls: type) -> GameObject:
    """A game handle minted outside this group (a registry stub, or the replay) has no host object: framework.object_arg
    adopted it as a plain GameObject. Give it the class its native needs, keeping its handle and counts (natives.md 1.2)."""
    if isinstance(obj, cls):
        return obj
    n = cls.__new__(cls)
    n.__dict__.update(obj.__dict__)
    if cls is GcLegoCamera:
        GcLegoCamera.__init__(n, obj.type_id)
    elif issubclass(cls, GcCharacter):
        GcCharacter.__init__(n, obj.type_id)
    elif cls is ScTileFont:
        n.name = b""
    elif cls is ScSLOSISLKCubicSpline:
        n.slk_name = b""
    n.handle, n.engine_refs, n.deleted = obj.handle, obj.engine_refs, obj.deleted
    if n.handle and ctx._objects.get(n.handle) is obj:
        ctx._objects[n.handle] = n
    return n


def _arg(ctx, call, i: int, cls: type, what: str, required: bool = True):
    """Argument i as a `cls` host object. nothing -> None (a fault if `required`: the original dereferences the null)."""
    obj = ctx.object_arg(call, i)
    if obj is None:
        if required:
            raise VmError("Undefined: null %s (the original dereferences a null pointer)" % what)
        return None
    return _rehost(ctx, obj, cls)


# --------------------------------------------------------------------------------------------------------------
# Reference-counted child vectors (the ScRenderNodeGroup child vector at +0x44, vector<ScAutoRefPtr> 0x419890)
# --------------------------------------------------------------------------------------------------------------
class RefVector:
    """{capacity +0, size +4, data +8}. push_back(const T&) copies the element (+1): in place when size < capacity
    (0x419928-0x41992b), else it reallocates to capacity 1 (from 0) or twice the capacity (0x419954 / 0x419966), copies
    the old elements (+1 each, 0x4199b8), then the new one (+1, 0x419a29), then destroys the old buffer's elements (-1
    each, 0x419a4e)."""

    def __init__(self):
        self.cap = 0
        self.items: list = []

    def push_back(self, ctx, obj) -> None:
        if len(self.items) < self.cap:
            if obj is not None:
                ctx.add_ref(obj)
            self.items.append(obj)
            return
        new_cap = 1 if self.cap == 0 else self.cap * 2
        old = list(self.items)
        for o in old:
            if o is not None:
                ctx.add_ref(o)
        if obj is not None:
            ctx.add_ref(obj)
        for o in old:
            if o is not None:
                ctx.release(o)
        self.items.append(obj)
        self.cap = new_cap


class RenderGroup(RefVector):
    """A ScRenderNodeGroup as the model director uses it ([0x8390a8], [0x83aa4c], made by 0x47d3xx before Setup)."""

    def contains(self, node_id: int) -> bool:
        """Contains(id, depth 1, flag 1) 0x4abd60: a direct child whose node id [child+0x10] equals `node_id`."""
        return any(c is not None and c.node_id == node_id for c in self.items)


# --------------------------------------------------------------------------------------------------------------
# Engine singletons (lazily built per context: HostContext.subsystem)
# --------------------------------------------------------------------------------------------------------------
class ModelDirector:
    """GcModelDirector's statics: the identifier map 0x782a04 (id -> ScAutoRefPtr<GcCharacter>), the two render groups,
    the shown-id list [0x83a9c0], the main Toa [0x7032b8]. The saver's toa id and ammo are written by addtoa (0x47e221,
    0x56898b) and belong to the saver model: they are kept here, for it to read."""

    def __init__(self, ctx):
        self.ctx = ctx
        self.map: dict = {}
        self.group_cnt1 = RenderGroup()          # [0x8390a8]
        self.group_other = RenderGroup()         # [0x83aa4c]
        self.shown_ids: list = []                # [0x83a9c0], count [0x83a9bc]
        self.main_toa: Optional[GcToa] = None    # [0x7032b8]
        self.saver_toa_id: Optional[int] = None  # [0x74749c]
        self.saver_ammo: Optional[int] = None    # [0x7474c8]
        self.toa_index: Optional[int] = None     # GcStaticSoundsPtrArray::SetCurrentToa
        self.events: list = []                   # 0x515f80 (id, second, mode), kept for M2a-3

    def get(self, ident: int) -> Optional[GcCharacter]:
        """Get 0x480f70: the character stored under the id, or null."""
        return self.map.get(ident & M32)

    def add(self, obj: GcCharacter, key: int) -> bool:
        """Add 0x480eb0: the temporary AddRef (0x480ec7), insert 0x482270 (std::map semantics: an existing key keeps its
        value; a new one stores a copy, +1, INFERRED one copy at 0x4dd49f), the temporary Release (0x480f08). Returns the
        inserted flag (0x480f19)."""
        ctx = self.ctx
        ctx.add_ref(obj)
        inserted = key not in self.map
        if inserted:
            ctx.add_ref(obj)
            self.map[key] = obj
        ctx.release(obj)
        return inserted


class BAGroup:
    """GcBAGroup's character slot [0x83903c]."""

    def __init__(self, ctx):
        self.ctx = ctx
        self.character: Optional[GcCharacter] = None

    def set_character(self, obj: Optional[GcCharacter]) -> None:
        """SetCharacter 0x54b330: the same pointer -> nothing; else AddRef the new (0x54b344), Release the old
        (0x54b350-0x54b35f), store (0x54b364)."""
        if self.character is obj:
            return
        if obj is not None:
            self.ctx.add_ref(obj)
        old = self.character
        if old is not None:
            self.ctx.release(old)
        self.character = obj


class UnderWater:
    """The global byte [0x73c5a8] (0 in .data, written only by SetGlobalUnderWater 0x589264)."""

    def __init__(self, ctx):
        self.global_flag = 0


class HudItem:
    def __init__(self, obj, ident: int = 0):
        self.obj, self.id = obj, ident


class HudDirector:
    """GcHudDirector's item table (vector of {item, id} at 0x782458) and the id counter [0x83aa3c] (0 from [0x839124],
    set by the static init 0x47cb64)."""

    def __init__(self, ctx):
        self.ctx = ctx
        self.next_id = 0
        self.items: list = []

    def add(self, ctx, obj) -> int:
        """AddBigSprite 0x47b730 / AddTextObject 0x47b780: the item ctor 0x4ac750 AddRefs the object (0x4ac7d3), then Add
        0x47b940 takes the counter as the id (0x47b94f), increments it (0x47b958) and returns it, or 0 when the container
        refuses a duplicate id (0x47b995)."""
        if obj is not None:
            ctx.add_ref(obj)
        ident = self.next_id
        self.next_id = (ident + 1) & M32
        if any(i == ident for _, i in self.items):
            return 0
        self.items.append((HudItem(obj, ident), ident))
        return ident


# --------------------------------------------------------------------------------------------------------------
# Engine functions the natives model
# --------------------------------------------------------------------------------------------------------------
def toa_index(ident: int) -> int:
    """The table at 0x47e57d-0x47e612: six cumulative subtractions of the ids tahu, lewa, kopa, poha, gali, onua select 5..0;
    any other id selects 0 (0x47e5c5 jmp 0x47e610)."""
    return TOA_INDEX.get(ident & M32, 0)


def main_character_id(ctx) -> int:
    """GcGame::MainCharacterID 0x43af90: 'cnt1' ([0x7796f4], 0x43c193) while there is no main Toa; else the id [+0x100] of
    the character the Toa has attached at +0x1d0 (0x43afb8-0x43afcb), 'cnt1' if none (0x43afe4)."""
    t = ctx.subsystem(ModelDirector).main_toa
    if t is None or t.attached is None:
        return ID_CNT1
    return t.attached.char_id if t.attached.char_id is not None else 0x3F3F3F3F


def set_under_water(ctx, flag: int) -> None:
    """GcMotionSystem::SetUnderWater 0x589050: OnEvent(0x36, 1) at 0x589154 for a nonzero byte, OnEvent(0x36, 0) at 0x589249
    for zero; both through CreateVariant 0x407f30 / 0x413ed0 (type 0x10). The fields it touches are not script-visible."""
    ctx.engine_call("OnEvent", [Variant(T_INT, 0x36), Variant(T_INT, 1 if (flag & 0xFF) else 0)])


def setup_moveable(ctx, ch: GcCharacter) -> None:
    """GcCharacter::SetupMoveable 0x418850: with no motion system ([char+0x124], 0x418856) nothing happens; else
    GcMotionSystem::SetupMoveable 0x574b80 runs (fields only, no early exit) and ends with the guard
    [ms+0x14] == MainCharacterID() (0x574ef1) -> SetUnderWater(byte [0x73c5a8])."""
    ms = ch.motion_system
    if ms is None:       # initmotionsystem is a tier-2 stub with no body: the path reaches 0x574ef1 unconditionally, so the
        ms = ch.motion_system = GcMotionSystem(ch.char_id)    # system is modelled as present, id [char+0x100] (0x40c60b)
    if ms.id is not None and ms.id == main_character_id(ctx):
        set_under_water(ctx, ctx.subsystem(UnderWater).global_flag)


def set_camera_ai_mode(ctx, cam: GcLegoCamera, mode: int) -> None:
    """GcLegoCamera::SetCameraAIMode 0x45b490 (byte mode)."""
    mode &= 0xFF
    old = cam.aimode
    cam.aimode = mode                                                    # 0x45b4a7
    if mode < 0x0F:                                                      # [0x706fd8] = 0xf (0x45b4b5-0x45b4bd)
        if mode in (0xA, 8, 6, 0xC, 5):                                  # 0x45b4c8-0x45b4f3
            pass
        elif mode == 0xB:                                                # 0x45b4fc-0x45b50c
            cam.flag454 = 1
            cam.byte2f1 = 0xB
        elif mode in (9, 0xE, 4):                                        # 0x45b510-0x45b529
            pass
        elif mode in (2, 3):                                             # 0x45b52b-0x45b545: GetTime(0) into +0x3b0
            cam.entered_spline_mode = True
    if old in (2, 3) and mode not in (2, 3):                             # 0x45b558-0x45b587 ([0x706fdc] = 2, [0x706fe0] = 3)
        held, cam.spline_holder = cam.spline_holder, None                # 0x45b590-0x45b5b6
        if held is not None:
            ctx.release(held)


def _set_spline(ctx, cam: GcLegoCamera, spline) -> None:
    """The common reference part of SetCameraSpline 0x45b380 / SetTargetSpline 0x45b400: the same pointer changes nothing
    (0x45b38b / 0x45b40b); else AddRef the new (0x45b397), Release the old (0x45b3a9), store (0x45b3b7)."""
    if cam.spline_holder is spline:
        return
    if spline is not None:
        ctx.add_ref(spline)
    old = cam.spline_holder
    if old is not None:
        ctx.release(old)
    cam.spline_holder = spline


def show(ctx, ident: int, second: int) -> None:
    """GcModelDirector::Show 0x47f830(id, second)."""
    md = ctx.subsystem(ModelDirector)
    ident &= M32
    ch = md.get(ident)                                                   # 0x47f855
    if ch is not None and ch.CLASS_INFO_ID == SC_RENDER_NODE_GROUP_ID:   # 0x47f880-0x47f8a5: never true for a GcCharacter
        raise VmError("Undefined: GcModelDirector::Show on an ScRenderNodeGroup-exact entry (unreachable)")
    if ch is None:                                                       # [eax+0x10] at 0x47f99b / 0x47fa33: null dereference
        raise VmError("Undefined: GcModelDirector::Show on an id with no character (null dereference)")
    cnt1 = ident == ID_CNT1                                              # 0x47f990-0x47f992
    group = md.group_cnt1 if cnt1 else md.group_other                    # [0x8390a8] / [0x83aa4c]
    if group.contains(ch.node_id):                                       # 0x47f9b6-0x47f9bb / 0x47fa4e-0x47fa53
        return
    ch.parent = group                                                    # SetParent 0x40fc00: [char+0x20] = group
    ctx.add_ref(ch)                                                      # temporary ScAutoRefPtr, 0x47f9df / 0x47fa7a
    group.push_back(ctx, ch)                                             # 0x419890
    ctx.release(ch)                                                      # the temporary's destructor, 0x47f9f9
    if cnt1:
        pass                                                             # 0x474580 AddCollisionPhysics(ids only), then 0x47fb0e
    else:
        if ident not in md.shown_ids:                                    # 0x47faa4-0x47fad8
            md.shown_ids.append(ident)                                   # 0x426010; then 0x474580 (ids only)
        ch.shadow_type = 1                                               # 0x47fb04-0x47fb09: SetShadowType(1)
    md.events.append((ident, second & M32, 1))                           # 0x47fb28: 0x515f80(&id, &second, 1)
    if cnt1:
        ctx.subsystem(BAGroup).set_character(ch)                         # 0x47fb42: SetCharacter 0x54b330


def add_toa(ctx, name: bytes, flag: int = 0) -> int:
    """GcModelDirector::AddToa 0x47e170(name, 0). Returns the inserted flag (0x47e657)."""
    md = ctx.subsystem(ModelDirector)
    name = services.c_string(name)
    ident = services.convert_string_to_id(name)                          # 0x47e194 -> 0x617810 -> ConvertStringToID
    toa = GcToa(_tid(ctx, "GcToa"), ident)                               # 0x47e1ac new 0x430, ctor 0x47e1d0
    md.saver_ammo = toa.ammo                                             # 0x56898b: [0x7474c8] = [toa+0x380]
    # INI-gated block 0x568a5e -> 0x5d4e40 ([Cheat] Urinate == "Pohatu", body 0x568a6b-0x568c02): modelled as INI-absent
    # (false) until M2a-3, so it is skipped here.
    ctx.engine_call("OnEvent", [Variant(T_INT, 0x32), Variant(T_INT, toa.ammo & 0xFFFF)])   # 0x568a59, unconditional
    toa.node_id = toa.char_id                                            # 0x47e1db-0x47e1eb: [toa+0x10] = [toa+0x100]
    slots = ctx.subsystem(services.BlockFileSlots)
    saved = slots.current                                                # 0x47e1f1: GetCurBlockFile
    slots.current = _character_block(ctx, ident)                         # 0x47e1fe: LoadCharBlockHeader 0x419330 sets it
    md.saver_toa_id = ident                                              # 0x47e221: [0x74749c] = id
    if not md.add(toa, ID_CNT1):                                         # 0x47e4bd Add; 0x47e4ca: return 0 with no restore
        return 0
    toa.collision_group = object()                                       # 0x47e522 InitializeCollisionPhysics 0x56c270
    md.main_toa = toa                                                    # 0x47e527
    md.toa_index = toa_index(ident)                                      # 0x47e57d-0x47e612: SetCurrentToa
    slots.current = saved                                                # 0x47e61e: SetCurBlockFile(saved)
    return 1


def _character_block(ctx, ident: int) -> list:
    """The character block file ScBlockFile::LoadHeader 0x610000 reads: "Root/Data/Characters/%.4s.blk" of the id's text
    (format 0x6ff527, 0x4193c3). A missing or malformed file leaves the header unloaded (fopen null / bad magic return
    without an error, 0x610041 / 0x610072): an empty entry table, and the slot is still set."""
    vfs = ctx.vfs
    if vfs is None:
        return []
    text = services.identifier_to_string(ident)[:4].decode("latin-1")
    try:
        ents = framework.BlockFiles(vfs.root, vfs).entries("Root/Data/Characters/%s.blk" % text)
    except ValueError:
        ents = None
    return list(ents) if ents is not None else []


# --------------------------------------------------------------------------------------------------------------
# Native bodies
# --------------------------------------------------------------------------------------------------------------
def _argc(call) -> int:
    return len(call.slots)


def _byte(call, i: int) -> int:
    """wi of an absent argument is 0 (0x6027bf); a present integer is read as the byte the wrapper pushes."""
    return (call.arg(i).value & 0xFF) if i < _argc(call) else 0


def _sctilefont_create(ctx, type_id):
    def fn(call):
        name = font_name(call.vm.heap.text(call.arg(0).value))                    # 0x626c24
        return ctx.create_variant(ScTileFont(type_id, name), type_id)              # 0x611936 -> 0x611950
    return fn


def _sctextobject_create(ctx, type_id):
    def fn(call):
        x, y, z = (osi_vm.bits_to_f(call.arg(i).value) for i in range(3))
        font = _arg(ctx, call, 3, ScTileFont, "ScTileFont", required=False)
        text = services.c_string(call.vm.heap.text(call.arg(4).value))              # 0x610cce
        obj = ScTextObject(ctx, type_id, x, y, z, font, text)
        if font is not None:
            ctx.add_ref(font)                                                      # 0x610c92: inc [font+4]
        return ctx.create_variant(obj, type_id)                                    # 0x611d21 -> 0x611d40
    return fn


def _loadhudsprite(ctx, type_id):
    def fn(call):
        rect = tuple(osi_vm.bits_to_f(call.arg(i).value) for i in range(4))
        name = services.c_string(call.vm.heap.text(call.arg(4).value))
        present = services.sr_block_file_exists(ctx, name + b".tga")                # the block-file oracle, as doesfileexist
        obj = GcBigSprite(type_id, rect, name, _byte(call, 5), _byte(call, 6), present)
        return ctx.create_variant(obj, type_id)                                    # 0x43db16 -> 0x43da20
    return fn


def _spline_create(ctx, type_id):
    def fn(call):
        name = services.c_string(call.vm.heap.text(call.arg(0).value))              # GetSLKFile 0x61a580
        return ctx.create_variant(ScSLOSISLKCubicSpline(type_id, name), type_id)   # 0x61a303 -> 0x61a320
    return fn


def _addtoa(ctx):
    def fn(call):
        ok = add_toa(ctx, call.vm.heap.text(call.arg(0).value), 0)
        return Variant(T_INT, 1 if ok else 0)                                      # CreateVariant 0x407f30 (byte)
    return fn


def _gettoa(ctx, type_id):
    def fn(call):
        return ctx.create_variant(ctx.subsystem(ModelDirector).get(ID_CNT1), type_id)   # Get(key [0x782884]), 0x47cf9e
    return fn


def _getcharacter(ctx, type_id):
    def fn(call):
        return ctx.create_variant(ctx.subsystem(ModelDirector).get(call.arg(0).value & M32), type_id)   # 0x47cebf
    return fn


def _show(ctx):
    def fn(call):
        show(ctx, call.arg(0).value & M32, call.arg(1).value & M32)
        return NOTHING                                                             # {0x0f, 0}, 0x47cd9b
    return fn


def _setupmoveable(ctx):
    def fn(call):
        setup_moveable(ctx, _arg(ctx, call, 0, GcCharacter, "GcCharacter"))
        return NOTHING                                                             # {0x0f, 0}, 0x409403
    return fn


def _getcameraaimode(ctx):
    def fn(call):
        return Variant(T_INT, _arg(ctx, call, 0, GcLegoCamera, "GcLegoCamera").aimode & 0xFF)   # 0x44ed18 / 0x44ed40
    return fn


def _setcameraaimode(ctx):
    def fn(call):
        set_camera_ai_mode(ctx, _arg(ctx, call, 0, GcLegoCamera, "GcLegoCamera"), call.arg(1).value & 0xFF)
        return NOTHING
    return fn


def _setcameraspline(ctx):
    def fn(call):
        cam = _arg(ctx, call, 0, GcLegoCamera, "GcLegoCamera")
        spline = _arg(ctx, call, 1, ScSLOSISLKCubicSpline, "ScSLOSISLKCubicSpline", required=False)
        _set_spline(ctx, cam, spline)                                              # 0x45b380
        cam.spline_params = tuple(call.arg(i).value for i in range(2, _argc(call)))   # +0x3b8, +0x3a8, +0x460, +0x461
        return NOTHING
    return fn


def _settargetspline(ctx):
    def fn(call):
        cam = _arg(ctx, call, 0, GcLegoCamera, "GcLegoCamera")
        spline = _arg(ctx, call, 1, ScSLOSISLKCubicSpline, "ScSLOSISLKCubicSpline", required=False)
        _set_spline(ctx, cam, spline)                                              # 0x45b400
        cam.spline_params = tuple(call.arg(i).value for i in range(2, _argc(call)))   # +0x3b8, +0x3bc, +0x3a8, +0x460, +0x461
        return NOTHING
    return fn


def _hud_add(ctx):
    def fn(call):
        obj = ctx.object_arg(call, 0)                                              # a GcBigSprite / ScTextObject, or null
        return Variant(T_INT, ctx.subsystem(HudDirector).add(ctx, obj))             # CreateVariant 0x407df0
    return fn


# --------------------------------------------------------------------------------------------------------------
# The group
# --------------------------------------------------------------------------------------------------------------
def bind(reg, ctx) -> None:
    T = reg.types.id
    reg.bind("sctilefont.create", _sctilefont_create(ctx, T("ScTileFont")))
    reg.bind("sctextobject.create", _sctextobject_create(ctx, T("ScTextObject")))
    reg.bind("gcbigsprite.loadhudsprite", _loadhudsprite(ctx, T("GcBigSprite")))
    reg.bind("scslosislkcubicspline.create", _spline_create(ctx, T("ScSLOSISLKCubicSpline")))
    reg.bind("gcmodeldirector.addtoa", _addtoa(ctx))
    reg.bind("gcmodeldirector.gettoa", _gettoa(ctx, T("GcToa")))
    reg.bind("gcmodeldirector.getcharacter", _getcharacter(ctx, T("GcCharacter")))
    reg.bind("gcmodeldirector.show", _show(ctx))
    reg.bind("gccharacter.setupmoveable", _setupmoveable(ctx))
    reg.bind("gclegocamera.getcameraaimode", _getcameraaimode(ctx))
    reg.bind("gclegocamera.setcameraaimode", _setcameraaimode(ctx))
    reg.bind("gclegocamera.setcameraspline", _setcameraspline(ctx))
    reg.bind("gclegocamera.settargetspline", _settargetspline(ctx))
    reg.bind("gchuddirector.addbigsprite", _hud_add(ctx))
    reg.bind("gchuddirector.addtextobject", _hud_add(ctx))


register_group("objects", bind)
