"""M2a-2 T8b: the directors, saver and Beta 1.5.0 mods native group of the Python oracle: the game singletons, the area
director's level/area state and string table, the saver's `.data` fields and its INI cheat gate, BetterSaver's boolean
map, and the cinematic player. Written from docs/osi/natives.md (§1.2 the ABI, §1.3 the citation format, §2 game objects
and engine references, §4 the re-entrant chains, §5 D16, §9 saver and mods) and from the asm of the canonical exe and of
BetterSaver.dll, read as data with objdump; never from engine/.

Every model below carries its citation in `CITES` (natives.md §1.3 form; one source for the code, the tests and
data/osi/native_overrides.json). A reading from the decomp alone is marked INFERRED; everything else was checked on the
instruction listing (tools/tests/test_osi_natives_directors.py pins each address).

What is NOT modelled, and why:
- The INI: `gcsaver.cheatmenu` reads `[Misc] CheatMenu` through GetPrivateProfileStringA. The decided value is the
  default ("" -> int 0; the owner's Bionicle.ini has no such key). `IniSettings` is the seam M2a-3 fills.
- The area content. `GcAreaLoader::LoadArea` (0x4fedd0) reads the area's `.slb` files and builds its objects; the oracle
  host has none, so `AreaDirector.area_source` is the seam (default: no objects). With no object loaded, the chains of
  natives.md §4 make no engine call, which is the A9 path. The chain itself (the SetupMoveable guard 0x574ef1 ->
  SetUnderWater 0x589050 -> `Call`; RemoveArea -> CleanUpArea -> GcModelDirector::Remove -> `Call`) is modelled and tested
  with an injected source (the characters it creates enter the model director's map, objects.ModelDirector.map).
- SetCurrentLevel's three SLK loads (material.slb, the texture table, the level text table): their errors are swallowed
  (the catch flags are written only with 0, like LoadArea's, natives.md §5.2) and no native of this group reads them.
- GcModelDirector::Remove's class-specific teardown (0x47f3c2-0x47f4ba): only what reaches the VM and the map erase are kept.
- A hive's child characters (~GcHive 0x623b70 -> Remove): see the comment above AreaDirector.set_current_area.
- BetterSaver's debug output (OutputDebugString) and the save routine 0x10002b00 (saves are M4).
- The cinematic player (Bink): D1 completes the call instantly, so nothing of the video is modelled.
"""
from __future__ import annotations

import struct
from typing import Callable, NamedTuple, Optional

from . import framework
from .framework import BlockFiles, GameObject, register_group
from .services import c_string, convert_string_to_id, cut
import osi_vm
from osi_vm import NOTHING, T_INT, T_STRING, Variant, VmError

M32 = 0xFFFFFFFF
NULL_ID = 0x3F3F3F3F               # [0x70b3bc] / [0x70b3c0] in .data: the null ScIdentifier ("????")

# --------------------------------------------------------------------------------------------------------------
# Citations (natives.md §1.3). The same text is the `cite` of the tier1 rows of data/osi/native_overrides.json.
# --------------------------------------------------------------------------------------------------------------
CITES = {
    "gcgame.getmainview":
        "// body 0x4366e0, method 0x43ae20 <GcGame::GetMainView> -> 0x49d3c0 <ScScreen::GetViewPort>; asm pin: 0x43ae26 "
        "mov edx,[0x8391b0] (the main view id), 0x43ae32 mov ecx,[0x70fa64] (the screen), 0x49d3f2 cmp ebx,edx (item+8 "
        "against the id); CreateVariant(GcViewPort*) 0x436700 (AddRef 0x436730, null 0x436713)",
    "gcgame.getkeymapper":
        "// body 0x4367f0, method 0x43ae90 <GcGame::GetKeyMapper>; asm pin: 0x43ae90 mov eax,[0x83aa98]; "
        "CreateVariant(GcKeyMapper*) 0x436810 (AddRef 0x436840, null 0x436823)",
    "gcgame.getmp3player":
        "// body 0x436940, method 0x43aea0 <GcGame::GetMP3Player>; asm pin: 0x43aea0 mov eax,[0x83879c]; "
        "CreateVariant(GcMP3Player*) 0x436960 (AddRef 0x436990, null 0x436973)",
    "gcviewport.getcamera":
        "// body 0x4474e0, method 0x448080 <GcViewPort::GetCamera>; asm pin: 0x44809e mov edi,[ebx+0x30] (the node, a "
        "temporary reference taken at 0x4480aa and released at 0x4480d6), 0x4480ca mov edi,[edx+0x1c] (the "
        "camera); CreateVariant(GcLegoCamera*) 0x447500 (AddRef 0x447530, null 0x447513)",
    "gcareadirector.getcurrentlevel":
        "// body 0x486c40, method 0x486c40 <GcAreaDirector::GetCurrentLevel>; asm pin: 0x486c46 mov edx,[0x83902c] "
        "(the level identifier, null 0x3f3f3f3f from the static init 0x48db4b); int variant 0x407df0 (0x407dfb type 0x10)",
    "gcareadirector.setcurrentlevel":
        "// body 0x486c10, method 0x489a60 <GcAreaDirector::SetCurrentLevel> -> 0x489560 <GcAreaDirector::RemoveArea> "
        "-> 0x5017d0 <GcAreaLoader::CleanUpArea> -> 0x47f320 <GcModelDirector::Remove>; asm pin: 0x489ab4-0x489ace guard "
        "(level and area not null), 0x489af6 mov [0x83902c],edx, 0x48a455 movzx eax,[eax+3] / 0x48a459 add eax,-0x30 "
        "(GcSaver::SetLevel 0x5d5740), Remove's Call 0x47f680 (OnEvent 0x70a8b6) after push 0x2b at 0x47f5c1",
    "gcareadirector.setcurrentarea":
        "// body 0x486ef0, method 0x48a540 <GcAreaDirector::SetCurrentArea> -> 0x4892f0 <GcAreaDirector::LoadArea> -> "
        "0x4fedd0 <GcAreaLoader::LoadArea> -> 0x5d5500 <GcSaver::SetCurrentArea> -> 0x10001ae0 <BetterSaver CleanupLoad> "
        "-> 0x501dc0 <GcAreaLoader::RemoveObject>; asm pin: 0x48a57f mov [0x83b0b4],edx, 0x48a588 call LoadArea, 0x48a593 "
        "call 0x5d5500, 0x5d5506 mov [0x747490],edx, LoadArea's SetUnderWater guard 0x574ef1 -> 0x589154 / 0x589249",
    "gcareadirector.getscriptfluffstring":
        "// body 0x487250, method 0x48acc0 <GcAreaDirector::GetScriptFluffString> -> 0x5e86e0 "
        "<GcStringTableLoader::GetString>; asm pin: 0x48ad29 cmp [0x8391c0],0 (the table), 0x48ad7a push 0x70b7eb (the "
        "empty string) / 0x48ad97 push 0x70b7e7 (\"XXX\"), 0x5e870d cmp edi,edx (index against count-1), 0x5e8675 inc "
        "dword [ebx] (InitStrings 0x5e8600 skips each length byte)",
    "gcsaver.cheatmenu":
        "// body 0x5d35b0, method 0x5d4fc0 <GcSaver::CheatMenu>; asm pin: 0x5d4fdb push 0x74764b (\"\"), 0x5d4fe0 push "
        "0x747679 (\"CheatMenu\"), 0x5d4fe5 push 0x74761d (\"Misc\"), 0x5d4fca push 0x83b410 (the INI path), 0x5d502c "
        "push 0x74765f (\"LEGOTester\") with the compare 0x4f5990; CreateVariant(int) 0x407f30 (byte)",
    "gcsaver.getammo":
        "// body 0x5d3910, method 0x5d3910 <GcSaver::GetAmmo>; asm pin: 0x5d3913 mov ax,[0x7474c8] (a word, .data 0), "
        "u16 variant 0x4183f0 (0x4183fa movzx edx,word)",
    "gcsaver.gettoa":
        "// body 0x5d36b0, method 0x5d36b0 <GcSaver::GetToa>; asm pin: 0x5d36b3 mov eax,[0x74749c] (a dword, .data "
        "0x3f3f3f3f; written by AddToa 0x47e221), int variant 0x407960 (0x407965 type 0x10, the dword)",
    "gcareadirector.setnearestmax":
        "// body 0x486fe0, method 0x486fe0 <GcAreaDirector::SetNearestMax (inline)>; asm pin: 0x486fe9 mov dl,[edx] (the "
        "low byte of argument 0), 0x486feb mov [0x70b37c],dl, 0x486ff1 {0xf, 0} (returns nothing); read by getnearestmax "
        "0x487013, GcAreaDirector::GetNearest 0x488160, GcCinemaEngine::Play 0x593d48 / Stop 0x5941cd",
    "gcareadirector.getnearestmax":
        "// body 0x487010, method 0x487010 <GcAreaDirector::GetNearestMax (inline)>; asm pin: 0x487013 mov al,[0x70b37c] "
        "(a byte, .data 4), CreateVariant 0x47d250 (0x47d255 type 0x10, 0x47d25a movzx byte)",
    "gcsaver.setammo":
        "// body 0x5d3840, method 0x5d3840 <GcSaver::SetAmmo>; asm pin: 0x5d3845 lea edx,[esp+0x10] (argument 0), 0x5d3849 "
        "mov dx,[edx], 0x5d384c mov [0x7474c8],dx (a word; the field getammo reads at 0x5d3913), 0x5d3853 {0xf, 0} "
        "(returns nothing)",
    "bettersaver.getbooleanvalue":
        "// body BetterSaver.dll 0x10001f70, method 0x10001f70 <BetterSaver::GetBooleanValue>; asm pin: "
        "0x10001f77 movzx esi,word (the u16 key), 0x10001fc8 xor al,al (absent), 0x10001fd1 setne cl, 0x10001fd8 mov "
        "ecx,0x10 (type int)",
    "bettersaver.setbooleanvalue":
        "// body BetterSaver.dll 0x10001ff0, method 0x10004730 <BetterSaver::SetBooleanValue>; asm pin: "
        "0x10001ffb movzx eax,word (the u16 key), 0x10002005 setne [ebp+0x13], 0x10004792 / 0x100047c9 store the byte, "
        "0x10004889 erase, 0x100048ab bts / 0x100048e1 btr on [0x8391a0] (table 0x1000a040), SetQuest 0x5d5e20 for keys "
        "14/16/18",
    "bettersaver.save":
        "// body BetterSaver.dll 0x10001f40, method 0x10002b00 <BetterSaver::Save>; asm pin: 0x10001f43 "
        "call 0x10002b00, 0x10001f56 mov ecx,0xf / 0x10001f62 mov [eax],cx (returns nothing)",
    "gchuddirector.playcinematic":
        "// body 0x47b100, method 0x47b820 <GcHudDirector::PlayCinematic>; asm pin: 0x47b867 cmp byte [0x70a3b0],0 (the "
        "movies switch, 1 in .data, cleared by -nointro 0x5380bb), 0x47b8ca call 0x5e2f40, 0x47b8fd call 0x47b920 "
        "(stop); returns nothing 0x47b119",
}

# The tier2 rows (taint-safe on the A9 path; no model): body addresses per natives.md §7.1.
TIER2 = {
    # GcAreaDirector::SetFog: 0x487090 -> 0x48a7a0 -> 0x5e5450 stores [0x74a344] (the mode dword) and [0x74a338] / [0x74a33c]
    # (arguments 1 and 2 negated, the constant-true branch 0x48a7a7) / [0x74a340]; read only by the render-state function
    # GcGraphicsOptions::SetFog 0x5e5090 (0x5e510f-0x5e51eb); no native reads it, no VM call: render state -> M2a-3
    "gcareadirector.setfog": 0x487090,
    "gckeymapper.setusecontext": 0x5ceed0,
    "gcmp3player.setupforplay": 0x5dc840,
    "gcmp3player.play": 0x5dc970,
    "gcmp3player.stop": 0x5dc9f0,
    # the saver setters: the store is not modelled, no bound native reads it back (getmask/gethealth/getenergy are unbound)
    "gcsaver.setmask": 0x5d35f0,
    "gcsaver.sethealth": 0x5d3650,
    "gcsaver.setenergy": 0x5d3680,
}


# --------------------------------------------------------------------------------------------------------------
# Game singletons (D13, natives.md §2.1/§2.4)
# --------------------------------------------------------------------------------------------------------------
class GcViewPort(GameObject):
    """The main view (ScScreen's view list, SetUpView 0x43a54c): the engine holds it, so G 1 (natives.md §2.4)."""


class GcKeyMapper(GameObject):
    """[0x83aa98], the global ScAutoRefPtr set in SetUpControllers (+1 at 0x439583): G 1."""


class GcMP3Player(GameObject):
    """[0x83879c], the global ScAutoRefPtr set in SetUpDatabase (+1 at 0x43a40c): G 1."""


class GcLegoCamera(GameObject):
    """The camera of a view, [[vp+0x30]+0x1c]; the camera node holds it: G 1 (INFERRED, natives.md §2.4)."""


class EngineObjects:
    """The engine's singletons for this context. Each lives for the whole run (the engine reference never drops), so its
    handle is minted once and every getter returns that handle (identity while live)."""

    def __init__(self, ctx):
        self.objs: dict[str, GameObject] = {}
        self.cameras: dict[int, GameObject] = {}

    def get(self, name: str, make: Callable[[], GameObject]) -> GameObject:
        o = self.objs.get(name)
        if o is None:
            o = self.objs[name] = make()
        return o

    def camera_of(self, viewport: GameObject, make: Callable[[], GameObject]) -> GameObject:
        """GcViewPort::GetCamera 0x448080: the camera of this view (one per view)."""
        key = id(viewport)
        c = self.cameras.get(key)
        if c is None:
            c = self.cameras[key] = make()
        return c


def _singleton(ctx, name, cls, type_id):
    """A getter that returns an engine singleton through its CreateVariant overload (+1 variant reference, null never)."""
    def fn(call):
        o = ctx.subsystem(EngineObjects).get(name, lambda: cls(type_id, 1))
        return ctx.create_variant(o, type_id)
    return fn


def _get_camera(ctx, vp_type, cam_type):
    def fn(call):
        vp = ctx.object_arg(call, 0)
        if vp is None:                                     # unreachable: 0x82 refuses nothing for type:GcViewPort
            raise VmError("Undefined behaviour: GcViewPort::GetCamera on a null view (0x44809e reads [0+0x30])")
        cam = ctx.subsystem(EngineObjects).camera_of(vp, lambda: GcLegoCamera(cam_type, 1))
        return ctx.create_variant(cam, cam_type)           # CreateVariant 0x447500
    return fn


# --------------------------------------------------------------------------------------------------------------
# The saver (natives.md §9.1)
# --------------------------------------------------------------------------------------------------------------
class IniSettings:
    """GetPrivateProfileStringA's data: (section, key) -> bytes, matched case-insensitively. Empty by default: the decided
    value for this install (the owner's Bionicle.ini has no [Misc] CheatMenu). `language` is `[0x70b384]` (0 = English,
    the .data byte and the owner's `Language=0`). INI -> M2a-3."""

    def __init__(self, ctx=None):
        self.values: dict[tuple[bytes, bytes], bytes] = {}
        self.language = 0

    @staticmethod
    def _k(s: bytes) -> bytes:
        return bytes(c + 32 if 0x41 <= c <= 0x5A else c for c in s)

    def set(self, section: bytes, key: bytes, value: bytes) -> None:
        self.values[(self._k(section), self._k(key))] = value

    def get_private_profile_string(self, section: bytes, key: bytes, default: bytes, size: int = 0xFF) -> bytes:
        """The API result for a buffer of `size` bytes: the value, or the default, cut to size-1 characters."""
        v = self.values.get((self._k(section), self._k(key)), default)
        return cut(v)[:size - 1]

    @classmethod
    def from_text(cls, text: bytes) -> "IniSettings":
        """A minimal `[section]` / `key=value` reader (lines without '=' and `;` comments are skipped)."""
        ini, sec = cls(), b""
        for ln in text.splitlines():
            ln = ln.strip()
            if not ln or ln.startswith(b";"):
                continue
            if ln.startswith(b"[") and ln.endswith(b"]"):
                sec = ln[1:-1]
            elif b"=" in ln:
                k, v = ln.split(b"=", 1)
                ini.set(sec, k.strip(), v.strip())
        return ini


class Saver:
    """GcSaver's `.data`/`.bss` fields that this group reads or writes, with their values at Setup (after InitData ->
    ResetData 0x5d5490, natives.md §9.1)."""

    def __init__(self, ctx):
        self.ammo = 0                   # [0x7474c8] word: the ONE store (setammo 0x5d384c, the Toa ctor 0x56898b, getammo 0x5d3913)
        self.toa_id = NULL_ID           # [0x74749c] dword: AddToa 0x47e221 (.data 0x3f3f3f3f; no script native reads it)
        self.level = 0                  # [0x74748c] byte: ResetData 0x5d5493
        self.area = NULL_ID             # [0x747490] dword: ResetData 0x5d549a
        self.conditions = 0             # [0x8391a0] dword: ResetData 0x5d54ec
        self.quest = bytearray(8)       # [0x83b37f + level] bytes (SetQuest 0x5d5e20), level 1..6 used
        # the area-object lists CleanupLoad walks: entries are (object id, area id); the exe's counts are the bytes
        # [0x83b401] (list 0x8384a0) and [0x83b3fa] (list 0x7f79f0); BetterSaver keeps its own vector (0x1000a81c)
        self.pickups_a: list[tuple[int, int]] = []     # 0x8384a0
        self.pickups_b: list[tuple[int, int]] = []     # 0x7f79f0
        self.better: list[tuple[int, int]] = []        # BetterSaver's token vector

    def set_quest(self, level: int, quest: int) -> None:
        """GcSaver::SetQuest 0x5d5e20: level (low byte) 1..6 and quest 1..3 (a byte): `or byte [0x83b37f+level], 1|2|4`
        through the jump table at 0x7474fc (0x5d5e42 -> 1, 0x5d5e50 -> 2, 0x5d5e60 -> 4)."""
        level &= 0xFF
        quest &= 0xFF
        if 1 <= level <= 6 and 1 <= quest <= 3:
            self.quest[level] |= (1, 2, 4)[quest - 1]

    def set_current_area(self, ctx, area_id: int) -> None:
        """GcSaver::SetCurrentArea 0x5d5500: [0x747490] = id (0x5d5506), then CleanupLoad (0x5d550c), which BetterSaver
        replaces with 0x10001ae0 (natives.md §9.3)."""
        self.area = area_id & M32
        self.cleanup_load(ctx)

    def cleanup_load(self, ctx) -> None:
        """BetterSaver 0x10001ae0: loader = GcAreaDirector::Get(&[0x747490]) (0x10001ae7 -> 0x48a810); none -> return
        (0x10001af5). Then, for each BetterSaver vector entry whose second dword is the saver's area, RemoveObject(entry)
        TWICE (0x10001b2d `mov esi,2` ... 0x10001b48 `jne`); then the same single call for each entry of the exe's lists
        0x8384a0 (count byte [0x83b401]) and 0x7f79f0 (count [0x83b3fa])."""
        ad = ctx.subsystem(AreaDirector)
        loader = ad.loaders.get(self.area)
        if loader is None:
            return
        for obj_id, area_id in list(self.better):
            if area_id == self.area:
                for _ in range(2):
                    ad.remove_object(ctx, loader, obj_id)
        for lst in (self.pickups_a, self.pickups_b):
            for obj_id, area_id in list(lst[:0xFF]):       # the count is a byte
                if area_id == self.area:
                    ad.remove_object(ctx, loader, obj_id)


def _set_ammo(ctx):
    def fn(call):
        ctx.subsystem(Saver).ammo = call.arg(0).value & 0xFFFF                  # mov dx,[edx]; mov [0x7474c8],dx (0x5d3849/0x5d384c)
        return NOTHING                                                         # {0xf, 0} (0x5d3853-0x5d3858)
    return fn


def _get_toa(ctx):
    return lambda call: Variant(T_INT, ctx.subsystem(Saver).toa_id & M32)      # mov eax,[0x74749c] 0x5d36b3; 0x407960 (dword)


def _get_ammo(ctx):
    def fn(call):
        return Variant(T_INT, ctx.subsystem(Saver).ammo & 0xFFFF)              # mov ax,[0x7474c8]; 0x4183f0 movzx word
    return fn


CHEAT_NEEDLE = b"LEGOTester"           # 0x74765f


def cheat_menu(ctx) -> int:
    """GcSaver::CheatMenu 0x5d4fc0: GetPrivateProfileStringA("Misc", "CheatMenu", "", buf, 0xff, ini) (0x5d4fca-0x5d4fea),
    assigned into a 255-capacity string (0x401850, npos), then 1 when it equals "LEGOTester" (0x5d502c: the compare 0x4f5990
    is a length and memcmp compare, so case-sensitive and exact), else 0. The result is the byte bl (0x5d5056 / 0x5d506d)."""
    s = ctx.subsystem(IniSettings).get_private_profile_string(b"Misc", b"CheatMenu", b"", 0xFF)
    return 1 if s == CHEAT_NEEDLE else 0


def _cheat_menu(ctx):
    return lambda call: Variant(T_INT, cheat_menu(ctx))                        # CreateVariant(int) 0x407f30: movzx byte


# --------------------------------------------------------------------------------------------------------------
# BetterSaver's boolean map (natives.md §9.2)
# --------------------------------------------------------------------------------------------------------------
class BetterSaverState:
    """The std::map<u16, bool> at 0x1000a80c. Empty after the ResetData hook 0x10001680 (natives.md §9.2); a `false`
    store erases the key (0x10004889 -> 0x100050c0), so a present key is always true."""

    def __init__(self, ctx):
        self.bools: dict[int, bool] = {}
        self.saves = 0                  # bettersaver.save calls; the routine 0x10002b00 writes the file in M4

    def get(self, key: int) -> bool:
        return self.bools.get(key & 0xFFFF, False)

    def set(self, ctx, key: int, value: bool) -> None:
        """BetterSaver 0x10004730 (ecx = &key, edx = &value): true -> assign or insert (0x100047c9 / 0x1000483f); false ->
        erase when present (0x10004889). Then, for a key below 0x20 (0x1000488e), bit `key` of the exe's [0x8391a0]
        (pointer table entry 0x1000a040) is set (bts 0x100048ab) or cleared (btr 0x100048e1); and for a true store of key
        14, 16 or 18, GcSaver::SetQuest(level byte [0x74748c] via table entry 0x1000a07c, 1, 2 or 3) (0x100048b3-0x100048da)."""
        key &= 0xFFFF
        if value:
            self.bools[key] = True
        else:
            self.bools.pop(key, None)
        if key < 0x20:
            sv = ctx.subsystem(Saver)
            if value:
                sv.conditions |= 1 << key
                q = {0xE: 1, 0x10: 2, 0x12: 3}.get(key)
                if q is not None:
                    sv.set_quest(sv.level, q)
            else:
                sv.conditions &= ~(1 << key) & M32


def _get_boolean_value(ctx):
    def fn(call):
        k = call.arg(0).value & 0xFFFF                                         # movzx esi, word [ebp+0x10] (0x10001f77)
        return Variant(T_INT, 1 if ctx.subsystem(BetterSaverState).get(k) else 0)   # setne cl 0x10001fd1, type 0x10 0x10001fd8
    return fn


def _set_boolean_value(ctx):
    def fn(call):
        k = call.arg(0).value & 0xFFFF                                         # movzx eax, word [ebp+0x10] (0x10001ffb)
        v = (call.arg(1).value & M32) != 0                                     # cmp dword [ebp+0x14],0; setne (0x10001ff4/0x10002005)
        ctx.subsystem(BetterSaverState).set(ctx, k, v)
        return NOTHING                                                         # {0x0f, 0} (0x10002011-0x1000201d)
    return fn


def _bs_save(ctx):
    def fn(call):
        ctx.subsystem(BetterSaverState).saves += 1                             # call 0x10002b00 (10001f43): the file is M4
        return NOTHING                                                         # {0x0f, 0} (0x10001f56-0x10001f62)
    return fn


# --------------------------------------------------------------------------------------------------------------
# The cinematic player (D1)
# --------------------------------------------------------------------------------------------------------------
class Cinematics:
    """GcHudDirector::PlayCinematic 0x47b820. `enabled` is the byte [0x70a3b0] (1 in .data; -nointro clears it, 0x5380bb).
    Every request is recorded; none stays pending: with D1 (skip_video) the call returns at once, and without it the
    original blocks until the video ends and stops the player (0x47b8fd -> 0x47b920 -> 0x5e3050), so the post-state is
    the same in both cases. `playing` is True only while a request is being handled."""

    def __init__(self, ctx):
        self.enabled = True
        self.playing = False
        self.requests: list[tuple[bytes, bool]] = []


def _play_cinematic(ctx):
    def fn(call):
        cin = ctx.subsystem(Cinematics)
        name = c_string(ctx.vm.heap.text(call.arg(0).value))                   # assign into a 255-capacity string (0x47b862)
        flag = (call.arg(1).value & 0xFF) != 0                                 # mov al, byte [edx] (0x47b10e)
        if cin.enabled:                                                        # cmp byte [0x70a3b0], 0 (0x47b867)
            cin.playing = True
            cin.requests.append((name, flag))                                  # 0x5e2cc0 / 0x5e2f40 / virtual +8
            cin.playing = False                                                # 0x47b8fd -> 0x47b920 -> 0x5e3050 (stop)
        return NOTHING                                                         # {0x0f, 0} (0x47b119)
    return fn


# --------------------------------------------------------------------------------------------------------------
# The area director
# --------------------------------------------------------------------------------------------------------------
class AreaInfo(NamedTuple):
    """GcAreaLoader's entry (_GcAreaInfo, 12 bytes): object id, the director that owns it, the object type byte."""
    obj_id: int
    director: str           # "area" | "model" | "nolight" | "back" | "trans" | "physics" | "light" | "sounds"
    otype: int = 0


MOVEABLE_SETUP_ID = 0x6C626C64          # 'lbld' [0x734f10]


class AreaObject(NamedTuple):
    """What an area's data loads (the seam `AreaDirector.area_source` returns): `block` is the LoadArea block that loads
    it ("objects" 0x4ff93e, "characters" 0x5002ec, "hives" 0x500c0b); `motion_id` is the id its GcMotionSystem is set up
    with (the field the guard 0x574eeb reads, [ms+0x14])."""
    block: str
    obj_id: int
    motion_id: int
    director: str = ""      # "" = the block's director in the binary: characters "model" ([0x83aa9c], 0x53f4fc), objects and
    otype: int = 0          # hives "area" ([0x83a518], 0x626328; objects may also use back/nolight/trans, none reach the VM)
    plain: bool = False     # a hive record with [rec+0x2c] == 0 (0x62623c cmp / 0x626240 je): ctor 0x6239b0, never the VM


class AreaLoader:
    """GcAreaLoader (0x1c bytes, 0x4892f0): the area's entry list (this+8..)."""

    def __init__(self):
        self.entries: list[AreaInfo] = []


class ModelObjects:
    """The slice of GcModelDirector that Remove 0x47f320 reads: the active flag [0x83aa4c] (non-null after the director's
    init 0x47d46d), the model map 0x782a04 (id -> object: the ONE map, objects.ModelDirector.map, which addtoa fills and
    AddCharacter 0x47f040 fills for an area's characters) and the main character's id [0x782884] ('cnt1', written by the
    static init 0x482d43)."""

    TOA_ID = 0x636E7431

    def __init__(self, ctx):
        self.ctx = ctx
        self.active = True

    @property
    def objects(self) -> dict:
        from . import objects
        return self._ctx_model(objects).map

    def _ctx_model(self, objects):
        return self.ctx.subsystem(objects.ModelDirector)

    def add_character(self, ctx, obj_id: int) -> None:
        """GcModelDirector::AddCharacter 0x47f040 as LoadCharacters 0x53f3f0 calls it (0x53f473): a new GcCharacter (0x368 bytes)
        for the id, Add 0x480eb0 into the map (an existing key keeps its value). The null-id rename (0x47f059 cmp [0x70a81c]
        = 0x3f3f3f3f -> 0x480260) and the character's block-file load are not modelled."""
        from . import objects
        md = ctx.subsystem(objects.ModelDirector)
        if md.get(obj_id) is None:
            md.add(objects.GcCharacter(objects._tid(ctx, "GcCharacter"), obj_id & M32, obj_id & M32), obj_id & M32)

    def remove(self, ctx, obj_id: int) -> None:
        """GcModelDirector::Remove 0x47f320: director not active -> return (0x47f32c); id not in the map -> return
        (0x47f36b); else the class-specific teardown (0x47f3c2-0x47f4ba, not modelled), the map erase (0x47f50c-0x47f5a9; the
        map's ScAutoRefPtr copy is released, INFERRED from the erase 0x4dd4f0) and unless the id is the main character's
        ([0x782884], 0x47f5b3) the engine call OnEvent(0x2b, id): a0 is CreateVariant 0x413ed0 of the byte 0x2b (0x47f5c1),
        a1 is CreateVariant 0x407df0 of the id (0x47f5d9); a2..a9 nothing. No catch on the chain (exc.py): an error leaves
        the native."""
        if not self.active:
            return
        from . import objects
        md = ctx.subsystem(objects.ModelDirector)
        ch = md.get(obj_id)
        if ch is None:
            return
        del md.map[obj_id & M32]
        ctx.release(ch)
        if obj_id != self.TOA_ID:
            ctx.engine_call("OnEvent", [Variant(T_INT, 0x2B), Variant(T_INT, obj_id & M32)])


class AreaDirector:
    """GcAreaDirector's globals: the current level [0x83902c] and area [0x83b0b4] (both the null id after the static init
    0x48db4b / 0x48db5f), the area map 0x783a88 (id -> loader), the main character's motion state for the guard 0x574eeb."""

    def __init__(self, ctx):
        self.level = NULL_ID
        self.area = NULL_ID
        self.loaders: dict[int, AreaLoader] = {}
        self.loading = False                                # [0x83b40f], set by LoadArea 0x4892fe
        self.area_source: Optional[Callable[[int], list]] = None
        self.under_water = 0                                # the byte [0x73c5a8]: 0 in .data, written only by 0x589264
        self.main_character_id = 0x636E7431                 # [0x7796f4] ('cnt1', static init 0x43c193); [0x7032b8] is 0 until AddToa
        self.set_under_water_calls = 0
        self.nearest_max = 4                                # the byte [0x70b37c]: 4 in .data (setnearestmax 0x486feb, getnearestmax 0x487013)

    # --- GcGame::MainCharacterID 0x43af90: one implementation, objects.main_character_id (the Toa branch 0x43afb2-0x43afcb)
    def main_character(self, ctx) -> int:
        from . import objects
        return objects.main_character_id(ctx)

    def set_current_level(self, ctx, name: bytes) -> None:
        """GcAreaDirector::SetCurrentLevel 0x489a60 (see CITES)."""
        s = c_string(name)                                                       # the copy 0x489a83 (0x4f56c0)
        new_id = convert_string_to_id(s)                                         # StringToIdentifier 0x489a96
        if self.level != NULL_ID and self.area != NULL_ID:                      # 0x489ab4-0x489ace
            self.remove_area(ctx, self.area)                                     # 0x489add
        self.level = new_id                                                      # 0x489af6
        # the three SLK loads (0x489b65-0x48a44b) swallow every error and no native here reads them: not modelled
        b3 = (s[3] if len(s) > 3 else 0)                                         # movzx eax, byte [name+3] (0x48a455)
        ctx.subsystem(Saver).level = (b3 - 0x30) & 0xFF                          # GcSaver::SetLevel 0x5d5740 (0x48a45f)

    def remove_area(self, ctx, area_id: int) -> None:
        """GcAreaDirector::RemoveArea 0x489560: loader = Get(id) (0x489574); CleanUpArea (0x489589); erase from the map
        (0x48959c); [0x83b0b4] = null id (0x4895a1)."""
        loader = self.loaders.get(area_id)
        if loader is None:
            # Get returns null and CleanUpArea reads [this+0x10] (0x5017d0): an access violation in the original
            raise VmError("Undefined behaviour: RemoveArea on an area with no loader")
        self.clean_up_area(ctx, loader)
        self.loaders.pop(area_id, None)
        self.area = NULL_ID

    def clean_up_area(self, ctx, loader: AreaLoader) -> None:
        """GcAreaLoader::CleanUpArea 0x5017d0: every entry's director removes it, in list order; then the list is torn
        down (the emitter, dragger, sound and conversation cleanups reach no engine call)."""
        for e in list(loader.entries):
            self.director_remove(ctx, e)
        loader.entries.clear()

    def director_remove(self, ctx, e: AreaInfo) -> None:
        """The director dispatch shared by CleanUpArea and RemoveObject 0x501dc0: only the model director's Remove calls
        into the VM (reentry closure, tools/osi/reach.py); the others are not modelled."""
        if e.director == "model":
            ctx.subsystem(ModelObjects).remove(ctx, e.obj_id)

    def remove_object(self, ctx, loader: AreaLoader, obj_id: int) -> None:
        """GcAreaLoader::RemoveObject 0x501dc0: the first entry whose object id matches is removed by its director and
        erased from the list; no entry -> nothing."""
        for i, e in enumerate(loader.entries):
            if e.obj_id == obj_id:
                self.director_remove(ctx, e)
                del loader.entries[i]
                return

    # F7b: the removal chain is modelled for the characters the load side creates (LoadCharacters 0x53f3f0: AddCharacter
    # 0x47f040 into the model map, an entry {id, [0x83aa9c]} pushed 0x53f514). CleanUpArea dispatches each entry on its director
    # (0x5017e6-0x501938); only the model director's Remove 0x47f320 reaches the VM among the direct calls. NOT modelled:
    # a hive's children. ~GcHive 0x623b70 calls Remove for each id of [hive+0x261] at [hive+0x2a8] (0x623bb9) and the hive
    # entry's director is [0x83a518] (-> GcAreaDirector::Remove 0x488990, which makes no direct call to the VM); whether that
    # removal drops the hive's last reference is not established (UNVERIFIED), and the hive's children are not created here.
    def set_current_area(self, ctx, name: bytes) -> None:
        """GcAreaDirector::SetCurrentArea 0x48a540."""
        s = c_string(name)                                                       # the copy 0x48a550 (0x4f56c0)
        area_id = convert_string_to_id(s)                                        # StringToIdentifier 0x48a55d
        self.area = area_id                                                      # 0x48a57f
        self.load_area(ctx, s, area_id)                                          # 0x48a588 (push 1, push name)
        ctx.subsystem(Saver).set_current_area(ctx, area_id)                      # 0x48a593 -> 0x5d5500

    def load_area(self, ctx, name: bytes, area_id: int) -> None:
        """GcAreaDirector::LoadArea 0x4892f0: [0x83b40f] = 1, a new GcAreaLoader, GcAreaLoader::LoadArea (0x48935a), then
        Add (0x48938c) under the id of the name; a duplicate id is not added (the loader is dropped)."""
        self.loading = True
        loader = AreaLoader()
        self.loader_load_area(ctx, loader, area_id)
        if area_id not in self.loaders:
            self.loaders[area_id] = loader

    def loader_load_area(self, ctx, loader: AreaLoader, area_id: int) -> None:
        """GcAreaLoader::LoadArea 0x4fedd0: three blocks reach the VM, each in a try with three catches that rethrow
        nothing (natives.md §5.2): LoadObjects 0x4ff93e, LoadCharacters 0x5002ec, LoadHives 0x500c0b. Inside them an
        object whose motion id is the main character's runs SetUnderWater (0x574ef1 guard), whose `Call` is the nested
        engine call; an error ends that block (the unwound LoadObjects) and loading goes on with the next block."""
        objs = self.area_source(area_id) if self.area_source is not None else []
        for block in ("objects", "characters", "hives"):
            for o in objs:
                if o.block != block:
                    continue
                # GcMoveableObj::GcMoveableObj 0x5432a0 calls SetupMoveable 0x545c00 (0x54408b) only when the id [obj+0x80]
                # equals the .data constant 'lbld' [0x734f10] (cmp 0x544065, jne 0x54406b); that id is also the motion
                # id [ms+0x14] (0x543f44 -> 0x5748ec), and the guard 0x574eeb-0x574ef1 needs it to equal the main
                # character's id. 'cnt1' != 'lbld', so the guard never passes while the main character is 'cnt1'.
                # A hive record with [rec+0x2c] == 0 uses ctor 0x6239b0 (0x6262fa) and never reaches SetupMoveable.
                # The 'lbld' gate is for the objects and hives blocks only: the ctor 0x5432a0 has exactly three callers
                # (0x4842dc GcNoLightDirector, 0x489937 AddMoveableObj = objects, 0x6236d4 GcHive ctor). Characters reach
                # SetupMoveable through GcCharacter::SetupMoveable 0x418850 (vtable slot at file offset 0x2edc58) ->
                # 0x574b80 with no 'lbld' check: a character record fires when its id equals the main character's.
                gated = block != "characters"
                if (not o.plain and (o.motion_id == MOVEABLE_SETUP_ID or not gated)
                        and o.motion_id == self.main_character(ctx)):               # cmp ecx,edx 0x574ef1
                    r = self.set_under_water(ctx, self.under_water, swallow=True)
                    if not r.ok:                                                 # swallowed: the object is not completed,
                        break                                                    # the block ends (0x4ff996 / 0x500344 / 0x500c63)
                if block == "characters" and not o.director:
                    ctx.subsystem(ModelObjects).add_character(ctx, o.obj_id)       # AddCharacter 0x47f040 (0x53f473), Get non-null
                loader.entries.append(AreaInfo(o.obj_id, o.director or ("model" if block == "characters" else "area"),
                                               o.otype))            # {id, director} pushed by 0x4709d0 (0x53f514 / 0x626340)

    def set_under_water(self, ctx, flag: int, swallow: bool = False):
        """GcMotionSystem::SetUnderWater 0x589050: both branches make the engine call OnEvent(0x36, b) with b = 1 for a
        non-zero flag (0x58905c, push 1 at 0x58908b) and 0 otherwise (push 0 at 0x5891a1). a0 is CreateVariant 0x413ed0 of
        the byte 0x36, a1 CreateVariant 0x407f30 of the byte b."""
        self.set_under_water_calls += 1
        b = 1 if (flag & 0xFF) else 0
        return ctx.engine_call("OnEvent", [Variant(T_INT, 0x36), Variant(T_INT, b)], swallow=swallow)


def _set_nearest_max(ctx):
    def fn(call):
        ctx.subsystem(AreaDirector).nearest_max = call.arg(0).value & 0xFF     # mov dl,[edx]; mov [0x70b37c],dl (0x486fe9/0x486feb)
        return NOTHING                                                         # {0xf, 0} (0x486ff1-0x486ff6)
    return fn


def _get_nearest_max(ctx):
    return lambda call: Variant(T_INT, ctx.subsystem(AreaDirector).nearest_max & 0xFF)   # mov al,[0x70b37c] 0x487013; 0x47d250 movzx byte


def _get_current_level(ctx):
    return lambda call: Variant(T_INT, ctx.subsystem(AreaDirector).level)       # mov edx,[0x83902c] 0x486c46; 0x407df0


def _set_current_level(ctx):
    def fn(call):
        ctx.subsystem(AreaDirector).set_current_level(ctx, ctx.vm.heap.text(call.arg(0).value))
        return NOTHING                                                          # {0x0f, 0} (0x486c21)
    return fn


def _set_current_area(ctx):
    def fn(call):
        ctx.subsystem(AreaDirector).set_current_area(ctx, ctx.vm.heap.text(call.arg(0).value))
        return NOTHING                                                          # {0x0f, 0} (0x486f01)
    return fn


# --------------------------------------------------------------------------------------------------------------
# The fluff string table (GcStringTableLoader, natives.md §5.3 of the plan: text from the level data)
# --------------------------------------------------------------------------------------------------------------
LANG_SUFFIXES = ("eng", "frn", "ger", "dan", "dut", "ita", "spa", "swe")        # 0x487855.. : the [0x70b384] byte -> file names
RELOC_MAGIC = 0xC0FFEE


class SlbImage:
    """A relocatable `.slb` image (ScSLKManager::GetSLKFile 0x4032e0): the file bytes, plus the relocation table at the
    end: the magic 0xc0ffee at (n-4)&~3, the count just below it, then `count` offsets (low two bits masked) of dwords
    that get the image base added. A dword that is not in the table keeps its raw value (an invalid address when
    dereferenced: Undefined behaviour here)."""

    def __init__(self, data: bytes):
        self.data = data
        self.relocs: set[int] = set()
        n = len(data)
        end = (n - 4) & ~3
        if end >= 4 and struct.unpack_from("<I", data, end)[0] == RELOC_MAGIC:
            cnt = struct.unpack_from("<I", data, end - 4)[0]
            for i in range(cnt):
                p = end - 8 - 4 * i
                if p < 0:
                    break
                self.relocs.add(struct.unpack_from("<I", data, p)[0] & ~3)

    def u32(self, off: int) -> int:
        if off < 0 or off + 4 > len(self.data):
            raise VmError("Undefined behaviour: read outside the string table image")
        return struct.unpack_from("<I", self.data, off)[0]

    def ptr(self, off: int) -> int:
        """A pointer field: the offset it holds in the image (base + offset in the original)."""
        v = self.u32(off)
        if off not in self.relocs or v >= len(self.data):
            raise VmError("Undefined behaviour: unrelocated or out-of-image pointer in the string table")
        return v


class FluffTable:
    """GcStringTableLoader over an SlbImage: [this+0x98] = the record count (the file's first dword), [this+0x9c] = the
    record array (its second dword, a pointer). A record is {id a, id b, count, string-pointer table}; InitStrings
    0x5e8600 (run once at load) adds 1 to every string pointer (0x5e8675: the stored pointer is the string's length byte)."""

    def __init__(self, image: SlbImage):
        self.image = image
        self.count = image.u32(0)
        self.records = image.ptr(4) if self.count else 0
        self.init_strings()

    def record(self, i: int) -> tuple[int, int, int, int]:
        """(id a, id b, string count, the raw string-pointer table field); the field is dereferenced only for count > 0."""
        o = self.records + 16 * i
        return (self.image.u32(o), self.image.u32(o + 4), self.image.u32(o + 8), o + 12)

    def init_strings(self) -> None:
        """InitStrings 0x5e8600: for each record and each of its strings, pointer += 1 (0x5e8675), then the text is
        assigned into a 255-capacity string (0x4f5c00), which throws kOutOfRange for a text longer than 255."""
        for i in range(self.count):
            _a, _b, n, tf = self.record(i)
            tbl = self.image.ptr(tf) if n else 0
            for j in range(n):
                p = self.image.ptr(tbl + 4 * j) + 1
                if len(self.text_at(p)) > 0xFF:
                    raise VmError("kOutOfRange")

    def text_at(self, p: int) -> bytes:
        d = self.image.data
        if p < 0 or p > len(d):
            raise VmError("Undefined behaviour: string pointer outside the image")
        e = d.find(b"\0", p)
        return d[p:] if e < 0 else d[p:e]

    def get_string(self, a: int, b: int, c: int) -> bytes:
        """GcStringTableLoader::GetString 0x5e86e0: the first record with ids (a, b) (0x5e86fb / 0x5e8707); the index
        against count-1 as unsigned (0x5e870c-0x5e870f: `dec edx; cmp edi,edx; jbe`) reads the string, a larger one gives
        the empty string (0x74ae14); no record gives the empty string (0x5e877f). A record of count 0 would read the table at
        an unchecked index (count-1 wraps): Undefined behaviour."""
        for i in range(self.count):
            ra, rb, n, tf = self.record(i)
            if ra == a and rb == b:
                if n == 0:
                    raise VmError("Undefined behaviour: string table record with count 0 indexed")
                if (c & M32) > n - 1:
                    return b""
                return self.text_at(self.image.ptr(self.image.ptr(tf) + 4 * (c & M32)) + 1)
        return b""


class StringTables:
    """[0x8391c0]: GcAreaDirector::Init (0x4875b0, run in DoSetUp before Setup) loads `fluff_<lang>.slb` from
    Lang_<lang>.blk (the current block file, 0x487bbb-0x487bcf; found by its base name), then InitStrings. The oracle host
    loads it on first use, from the install's VFS. A missing block file or entry leaves the table null (the original's
    GetSLKFile would throw kFileOpen in Init; the host has no Init)."""

    def __init__(self, ctx):
        self.ctx = ctx
        self._fluff: Optional[FluffTable] = None
        self._tried = False

    def fluff(self) -> Optional[FluffTable]:
        if self._fluff is None and not self._tried:
            self._tried = True
            self._fluff = self._load()
        return self._fluff

    def _load(self) -> Optional[FluffTable]:
        ctx = self.ctx
        vfs = ctx.vfs
        lang = ctx.subsystem(IniSettings).language
        if vfs is None or not (0 <= lang < len(LANG_SUFFIXES)):
            return None
        suf = LANG_SUFFIXES[lang]
        blk_path = "Root/Data/Languages/Lang_%s.blk" % suf
        data = vfs.read(blk_path)
        if data is None:
            return None
        ent = BlockFiles(vfs.root, vfs).find(blk_path, "fluff_%s.slb" % suf)
        if ent is None:
            return None
        return FluffTable(SlbImage(data[ent.offset:ent.offset + ent.size]))


def get_script_fluff_string(ctx, a: int, b: int, c: int) -> bytes:
    """GcAreaDirector::GetScriptFluffString 0x48acc0: table null, or b == 0, or a == 0 -> "XXX" (0x48ad29-0x48ad3a); else
    GetString(a, b, c); a result equal to "" (0x70b7eb) becomes "XXX" (0x48ad7a-0x48ad97). The result is copied into the
    caller's 255-capacity string with Init(npos) (0x48adf2), which throws kOutOfRange for a text of 255 or more characters
    (InitStrings' `operator=` at load accepts exactly 255, so a 255-character text loads and then fails here)."""
    t = ctx.subsystem(StringTables).fluff()
    if t is None or b == 0 or a == 0:
        s = b"XXX"
    else:
        s = t.get_string(a, b, c)
    if s == b"":
        s = b"XXX"
    return c_string(s)                       # the final ScFixedString::Init(npos) 0x48adf2: kOutOfRange at len >= 0xff


def _get_script_fluff_string(ctx):
    def fn(call):
        a, b, c = (call.arg(i).value & M32 for i in range(3))
        s = get_script_fluff_string(ctx, a, b, c)
        return Variant(T_STRING, ctx.vm.heap.new_string(s))                     # CreateVariant 0x46de00
    return fn


# --------------------------------------------------------------------------------------------------------------
# The group
# --------------------------------------------------------------------------------------------------------------
def bind(reg, ctx) -> None:
    t = reg.types.id
    reg.bind("gcgame.getmainview", _singleton(ctx, "main_view", GcViewPort, t("GcViewPort")))
    reg.bind("gcgame.getkeymapper", _singleton(ctx, "key_mapper", GcKeyMapper, t("GcKeyMapper")))
    reg.bind("gcgame.getmp3player", _singleton(ctx, "mp3_player", GcMP3Player, t("GcMP3Player")))
    reg.bind("gcviewport.getcamera", _get_camera(ctx, t("GcViewPort"), t("GcLegoCamera")))
    reg.bind("gcareadirector.getcurrentlevel", _get_current_level(ctx))
    reg.bind("gcareadirector.setcurrentlevel", _set_current_level(ctx))
    reg.bind("gcareadirector.setcurrentarea", _set_current_area(ctx))
    reg.bind("gcareadirector.getscriptfluffstring", _get_script_fluff_string(ctx))
    reg.bind("gcsaver.cheatmenu", _cheat_menu(ctx))
    reg.bind("gcsaver.getammo", _get_ammo(ctx))
    reg.bind("gcsaver.setammo", _set_ammo(ctx))
    reg.bind("gcsaver.gettoa", _get_toa(ctx))
    reg.bind("gcareadirector.setnearestmax", _set_nearest_max(ctx))
    reg.bind("gcareadirector.getnearestmax", _get_nearest_max(ctx))
    reg.bind("bettersaver.getbooleanvalue", _get_boolean_value(ctx))
    reg.bind("bettersaver.setbooleanvalue", _set_boolean_value(ctx))
    reg.bind("bettersaver.save", _bs_save(ctx))
    reg.bind("gchuddirector.playcinematic", _play_cinematic(ctx))


register_group("directors", bind)
