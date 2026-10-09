"""M2a-2 T7b: the services-and-time native group of the Python oracle: string <-> identifier, file-exists, the timer
constructor, the process manager's real-time switch and the resolution getters. Written from docs/osi/natives.md (§1.2
the ABI, §1.3 the citation format, §2.4 initial engine references, §8.2 `doesfileexist`, §9.3 the BetterSaver hooks) and
from the asm of the canonical exe and of BetterSaver.dll, read as data with objdump; never from engine/.

Every model below carries its citation in `CITES` (natives.md §1.3 form, one source for the code, the tests and
data/osi/native_overrides.json). A reading from the decomp alone is marked INFERRED; everything else was checked on the
instruction listing (tools/tests/test_osi_natives_services.py pins each address).

What is NOT modelled, and why:
- GcTimer::Start's reload of the duration from +0x70 (0x445999-0x4459c3): +0x70 is never initialised by the
  constructor (0x445840), and the path never starts a timer (natives.md §6.2).
- A started timer firing: no host hook in M2a-2 (natives.md §6.2: `gctimer.create(0, 0, 5.0)` is the only call).
- The per-run `User/` store behind BetterSaver's DoesFileExist hook: the oracle host has no Save, so it holds no file.
- The INI behind the resolution: the cited default (640 x 480) stands until M2a-3.
"""
from __future__ import annotations

import struct
from typing import Optional

from . import framework
from .framework import BlockFiles, GameObject, register_group
import osi_vm
from osi_vm import NOTHING, T_FLOAT, T_INT, T_STRING, Variant, VmError, f32_bits

M32 = 0xFFFFFFFF

# --------------------------------------------------------------------------------------------------------------
# Citations (natives.md §1.3). The same text is the `cite` of the tier1 rows of data/osi/native_overrides.json.
# --------------------------------------------------------------------------------------------------------------
CITES = {
    "scslosifoundation.stringtoidentifier":
        "// body 0x617700, method 0x617810 <ScSLOSIFoundation::StringToIdentifier> → 0x5fb060 "
        "<ScOSISystem::ConvertStringToID>; asm pin: 0x5fb093/0x5fb09e/0x5fb0a9/0x5fb0b2 shift immediates "
        "(first character in the high byte)",
    "scslosifoundation.identifiertostring":
        "// body 0x6177a0, method 0x6178c0 <ScSLOSIFoundation::IdentifierToString>; asm pin: 0x6178e6/0x6178dd/0x617908/"
        "0x6178f7 byte-swap immediates (shl 0x18, shl 8, shr 8, shr 0x18), 4-byte copy at 0x617938, strlen copy at 0x61796b",
    "scslosifileio.doesfileexist":
        "// body 0x615950, method 0x615ef0 <ScSLOSIFileIO::DoesFileExist> → 0x54ddc0 <SrBlockFileExists> → 0x60fec0 "
        "<ScBlockFile::Find>; asm pin: 0x54ddc7 Art.blk object 0x83b0b8, 0x54ddf0 slot [0x736128], 0x54de11 slot "
        "[0x73612c], 0x54de41 FileExists 0x54de70; BetterSaver hook 0x10001da0 takes paths containing User/ (0x10001dbe)",
    "gctimer.create":
        "// body 0x4454f0, method 0x445840 <GcTimer::GcTimer> → 0x445990 <GcTimer::Start>; asm pin: 0x445906/0x44590f "
        "ctor Start guard on the second argument's low byte, 0x4459c6-0x4459f1 Start's AddTrigger 0x4c2440 and flag +0x78, "
        "0x4458fc duration double at +0x68",
    "scprocessmanager.userealdeltatime":
        "// body 0x4a8ac0, method 0x4a90e0 <ScProcessManager::UseRealDeltaTime>; asm pin: 0x4a90f2 mov [0x712d78], dl "
        "(low byte of the argument); D3 constants 0x712d68/0x712dc0/0x712dc8/0x712dd0/0x712dd8 (Process 0x4a9170)",
    "gcgame.getresolutionwidth":
        "// body 0x4367b0, method 0x43ae50 <GcGame::GetResolutionWidth>; asm pin: 0x43ae5d fld [screen+0x10] with the "
        "screen at [0x70fa64]; default 640 from 0x7475a2 (LoadIni writes it, 0x5d4a62); float variant 0x4077f0",
    "gcgame.getresolutionheight":
        "// body 0x4367d0, method 0x43ae70 <GcGame::GetResolutionHeight>; asm pin: 0x43ae7f fld [screen+0x14] with the "
        "screen at [0x70fa64]; default 480 from 0x7475a6 (LoadIni writes it, 0x5d4a7c); float variant 0x4077f0",
}


# --------------------------------------------------------------------------------------------------------------
# Strings and identifiers
# --------------------------------------------------------------------------------------------------------------
def cut(b: bytes) -> bytes:
    """A C string: up to the first NUL (strlen, 0x6573c0)."""
    i = b.find(b"\0")
    return b if i < 0 else b[:i]


def c_string(b: bytes) -> bytes:
    """ScFixedString::assign(const char*, npos) 0x401850 into a capacity-0xff string: strlen (0x401881 when the length
    argument equals [0x726a44] = npos), then kOutOfRange when len >= capacity (`cmp edi, [esi+8]; setae` 0x401894,
    SrThrow 0x4d91f0 at 0x4018a8)."""
    s = cut(b)
    if len(s) >= 0xFF:
        raise VmError("kOutOfRange")
    return s


def convert_string_to_id(s: bytes) -> int:
    """ScOSISystem::ConvertStringToID 0x5fb060: length != 4 gives 0xffffffff (0x5fb06e-0x5fb075); else the four bytes
    as a big-endian dword (shift immediates 0x18/8/-8/-0x18 at 0x5fb093/0x5fb09e/0x5fb0a9/0x5fb0b2)."""
    if len(s) != 4:
        return M32
    return (s[0] << 24) | (s[1] << 16) | (s[2] << 8) | s[3]


def identifier_to_string(i: int) -> bytes:
    """ScSLOSIFoundation::IdentifierToString 0x6178c0: byte-swap the id (0x6178d7-0x617908), copy the 4 bytes into a
    string (assign with length 4, 0x617938/0x617944), then copy that string again with npos (0x61796b-0x61797b), which is
    strlen: the text ends at the first NUL. There is no table: every id has a text, so there is no 'unknown id' text."""
    return cut((i & M32).to_bytes(4, "big"))


def _string_to_identifier(ctx):
    def fn(call):
        text = c_string(call.vm.heap.text(call.arg(0).value))            # 0x617849-0x617855: c_str + assign(npos)
        return Variant(T_INT, convert_string_to_id(text))                # the validity flag at 0x617882 is discarded
    return fn


def _identifier_to_string(ctx):
    def fn(call):
        return Variant(T_STRING, call.vm.heap.new_string(identifier_to_string(call.arg(0).value)))   # CreateVariant 0x46de00
    return fn


# --------------------------------------------------------------------------------------------------------------
# Block files and DoesFileExist
# --------------------------------------------------------------------------------------------------------------
ART_BLK = "Root/Data/Art/Art.blk"           # the path 0x703523 that GcGame::LoadArtBlockHeader 0x43b030 loads into 0x83b0b8


def compare_names(a: bytes, b: bytes) -> int:
    """The block-file name compare 0x60fd70: C strings, only A-Z folded (`or 0x60` at 0x60fdbf / 0x60fdd2), an unsigned
    byte compare; -1 when a < b, 0 when equal, 1 when a > b."""
    a, b = cut(a), cut(b)
    for i in range(max(len(a), len(b)) + 1):
        ca = a[i] if i < len(a) else 0
        cb = b[i] if i < len(b) else 0
        if ca == 0 and cb == 0:
            return 0
        if ca == 0:
            return -1
        if cb == 0:
            return 1
        fa = ca | 0x60 if 0x41 <= ca <= 0x5A else ca
        fb = cb | 0x60 if 0x41 <= cb <= 0x5A else cb
        if fa != fb:
            return -1 if fa < fb else 1
    return 0


def _entry_name(e) -> bytes:
    n = e.name
    return n if isinstance(n, bytes) else n.encode("latin-1")


def block_find(entries, name: bytes) -> int:
    """ScBlockFile::Find 0x60fec0: the base name (after the last '\\\\' 0x5c or '/' 0x2f, 0x60fef6-0x60ff05), then a binary
    search over the 0x38-byte entries: mid = (lo + hi) >> 1 (0x60ff14), equal (0x60fd30) returns mid, `lo == hi - 1`
    (0x60ff39) returns -1, otherwise name < entry (0x60fd10) sets hi = mid, else lo = mid. No entries gives -1 (0x60fedf).
    The table is searched as stored: an unsorted table can miss a name that is in it."""
    if entries is None or len(entries) <= 0:
        return -1
    name = cut(name)
    base = name
    for i, c in enumerate(name):
        if c in (0x5C, 0x2F):
            base = name[i + 1:]
    lo, hi = 0, len(entries)
    while True:
        mid = (lo + hi) >> 1
        cmp = compare_names(base, _entry_name(entries[mid]))
        if cmp == 0:
            return mid
        if lo == hi - 1:
            return -1
        if cmp < 0:
            hi = mid
        else:
            lo = mid


class BlockFileSlots:
    """The block files SrBlockFileExists 0x54ddc0 reads: Art.blk (object 0x83b0b8, loaded in DoSetUp by 0x436a55), then the
    slots [0x736128] and [0x73612c]. Both slots are 0 at Setup (natives.md §8.2). A slot is a list of entries (objects
    with `.name`) or None for null; the natives that save and restore a slot (ScTileFont, LoadHudSprite) leave it null."""

    def __init__(self, ctx):
        self.ctx = ctx
        self.current: Optional[list] = None
        self.second: Optional[list] = None
        self._blocks: Optional[BlockFiles] = None

    def art_entries(self) -> Optional[list]:
        """The entries of the Art.blk object, or None when its header is not loaded (IsHeaderLoaded 0x60fff0: the entry
        table pointer is null). No VFS or no Art.blk file is "not loaded"; a file that does not parse is treated the same."""
        vfs = self.ctx.vfs
        if vfs is None:
            return None
        if self._blocks is None:
            self._blocks = BlockFiles(vfs.root, vfs)
        try:
            return self._blocks.entries(ART_BLK)
        except ValueError:
            return None


def has_user_needle(name: bytes) -> bool:
    """BetterSaver's DoesFileExist hook 0x10001da0: strstr(path, "User/") (0x10001dbe, needle at 0x1000833c), case-sensitive."""
    return b"User/" in cut(name)


def file_exists(ctx, name: bytes) -> bool:
    """ScFileManager-based FileExists 0x54de70: Open(name, [0x71edf8] | [0x71ee10] = 0x41) then Close, inside a try whose
    catch gives false (0x54df52). A path whose first component is no mounted volume throws in Open (0x4db0f0 returns null,
    SrThrow 0x4daef...), so it is false. INFERRED: the Root volume is the install directory and an existing regular file
    opens (the VFS contract); a directory does not."""
    vfs = ctx.vfs
    if vfs is None:
        return False
    return vfs.exists(name.decode("latin-1"))


def sr_block_file_exists(ctx, name: bytes) -> bool:
    """SrBlockFileExists 0x54ddc0 (natives.md §8.2): Art.blk when its header is loaded and Find >= 0 (0x54ddc7-0x54dde4);
    else, when [0x736128] is non-null (0x54ddf0), Find in it, then in [0x73612c] if that is non-null (0x54de11), else
    false; when [0x736128] is null, FileExists (0x54de41)."""
    name = cut(name)
    st = ctx.subsystem(BlockFileSlots)
    art = st.art_entries()
    if art is not None and block_find(art, name) >= 0:
        return True
    if st.current is not None:
        if block_find(st.current, name) >= 0:
            return True
        return st.second is not None and block_find(st.second, name) >= 0
    return file_exists(ctx, name)


def does_file_exist(ctx, name: bytes) -> bool:
    """ScSLOSIFileIO::DoesFileExist 0x615ef0 with BetterSaver's hook 0x10001da0 (natives.md §9.3): a path containing
    "User/" is resolved in the per-run User store (not modelled: false); any other path goes to the original."""
    name = cut(name)
    if has_user_needle(name):
        return False
    return sr_block_file_exists(ctx, name)


def _does_file_exist(ctx):
    def fn(call):
        return Variant(T_INT, 1 if does_file_exist(ctx, call.vm.heap.text(call.arg(0).value)) else 0)   # CreateVariant 0x407f30
    return fn


# --------------------------------------------------------------------------------------------------------------
# GcTimer
# --------------------------------------------------------------------------------------------------------------
class GcTimer(GameObject):
    """A GcTimer (GcEventTrigger 0x517510 -> ScTrigger 0x4c19e0 -> SxReferenceCountable 0x4f2b80). Fields: the event id
    (GcTimerEvent 0x445800), the duration double at +0x68 and the started flag at +0x78. The constructors add no reference
    to `this` (natives.md §2.4, G 0)."""

    def __init__(self, type_id: int, event_id: int, duration: float):
        super().__init__(type_id, 0)
        self.event_id = event_id
        self.duration = duration
        self.started = False

    def start(self, ctx) -> None:
        """GcTimer::Start 0x445990: once (flag +0x78, 0x4459c6), AddTrigger 0x4c2440 stores the timer in the trigger map,
        which takes one reference (`inc [trigger+4]`, 0x4c25e5; INFERRED that the map's copy keeps it, as in natives.md
        §2.4). Nothing fires it: no host hook in M2a-2."""
        if not self.started:
            ctx.add_ref(self)
            self.started = True


def _gctimer_create(ctx, type_id):
    def fn(call):
        event_id = call.arg(0).value & M32                               # mov edx, [esi] (0x445514): a dword
        start = (call.arg(1).value & 0xFF) != 0                          # mov al, byte [edi] (0x445523), cmp byte (0x445906)
        duration = osi_vm.bits_to_f(call.arg(2).value)                   # fld dword (0x44551c), fstp qword (0x445520)
        t = GcTimer(type_id, event_id, duration)
        if start:
            t.start(ctx)                                                 # 0x44590f
        return ctx.create_variant(t, type_id)                            # CreateVariant 0x445550
    return fn


# --------------------------------------------------------------------------------------------------------------
# The process manager's time (D3)
# --------------------------------------------------------------------------------------------------------------
# .data constants of ScProcessManager::Process 0x4a9170, read by address (tools/tests pins each):
FAKE_DT = 0.006666666828095913           # 0x712d68: dt of the fake-time path (the D3 value 0.0066667)
REAL_DT_DEFAULT = 0.016666000708937645   # 0x712db0 (the first ten passes' "previous") and 0x712dc0 (after a gap)
REAL_GAP = 0.5                           # 0x712db8: a clock step above this is a gap
REAL_W_NEW = 0.07000000029802322         # 0x712dc8
REAL_W_OLD = 0.9300000071525574          # 0x712dd0
REAL_DT_MAX = 0.06599999964237213        # 0x712dd8 (compare) and 0x712de0 (the clamp value)


class ProcessTime:
    """ScProcessManager's time state: the flag byte [0x712d78] (0 in .data: the fake-time path), the smoothed dt
    [0x712d70], the pass counter [0x712d88], the last clock reading (pm+0x10) and the time (pm+8)."""

    def __init__(self, ctx):
        self.use_real = 0
        self.smoothed = 0.0
        self.count = 0
        self.last = 0.0
        self.time = 0.0

    def step(self, now: float):
        """The time step of ScProcessManager::Process (0x4a9200-0x4a92f2) for a clock reading `now`: the smoothing runs on
        every pass (0x4a9212-0x4a92a5), then the flag picks the dt: fake time adds FAKE_DT to the time (0x4a92c8-0x4a92e3),
        real time takes the smoothed dt and the clock as the time (0x4a92ae-0x4a92c6). Returns (dt, "fake" | "real")."""
        prev = self.smoothed                                             # 0x4a920c-0x4a9212
        if self.count < 10:                                              # 0x4a9218
            prev = REAL_DT_DEFAULT                                       # 0x4a9221
        diff = now - self.last                                           # 0x4a922d-0x4a9239
        if diff > REAL_GAP:                                              # 0x4a923c, test ah,0x45 / jne
            new = REAL_DT_DEFAULT                                        # 0x4a9249
        else:
            new = diff * REAL_W_NEW + REAL_W_OLD * prev                  # 0x4a9266-0x4a9278
        self.smoothed = new
        self.count += 1                                                  # 0x4a9280
        if self.smoothed > REAL_DT_MAX:                                  # 0x4a928c
            self.smoothed = REAL_DT_MAX                                  # 0x4a9299
        self.last = now                                                  # 0x4a92f2
        if self.use_real:                                                # 0x4a92a5
            self.time = now
            return self.smoothed, "real"
        self.time += FAKE_DT
        return FAKE_DT, "fake"


def _use_real_delta_time(ctx):
    def fn(call):
        if ctx.flags.deterministic:                                      # D3: a logged no-op (the VM logs the call)
            return NOTHING
        ctx.subsystem(ProcessTime).use_real = call.arg(0).value & 0xFF   # mov al, byte [edx] (0x4a8aca); mov [0x712d78], dl
        return NOTHING                                                   # the bool the method returns is discarded (0x4a8ad3)
    return fn


# --------------------------------------------------------------------------------------------------------------
# The resolution
# --------------------------------------------------------------------------------------------------------------
DEFAULT_RESOLUTION = (640.0, 480.0)      # "640" / "480" at 0x7475a2 / 0x7475a6, the values LoadIni writes (0x5d4a62, 0x5d4a7c)


class Screen:
    """The screen object at [0x70fa64]: width at +0x10 and height at +0x14, floats. INI -> M2a-3."""

    def __init__(self, ctx):
        self.width, self.height = DEFAULT_RESOLUTION


def _float(x: float) -> Variant:
    return Variant(T_FLOAT, f32_bits(x))                                 # CreateVariant 0x4077f0: {0x12, float32}


def _resolution_width(ctx):
    return lambda call: _float(ctx.subsystem(Screen).width)


def _resolution_height(ctx):
    return lambda call: _float(ctx.subsystem(Screen).height)


# --------------------------------------------------------------------------------------------------------------
# The group
# --------------------------------------------------------------------------------------------------------------
def bind(reg, ctx) -> None:
    reg.bind("scslosifoundation.stringtoidentifier", _string_to_identifier(ctx))
    reg.bind("scslosifoundation.identifiertostring", _identifier_to_string(ctx))
    reg.bind("scslosifileio.doesfileexist", _does_file_exist(ctx))
    reg.bind("gctimer.create", _gctimer_create(ctx, reg.types.id("GcTimer")))
    reg.bind("scprocessmanager.userealdeltatime", _use_real_delta_time(ctx))
    reg.bind("gcgame.getresolutionwidth", _resolution_width(ctx))
    reg.bind("gcgame.getresolutionheight", _resolution_height(ctx))


register_group("services", bind)
