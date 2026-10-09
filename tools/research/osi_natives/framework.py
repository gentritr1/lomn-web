"""M2a-2 T4: the oracle's native framework, the host side of every native model. Written from docs/osi/natives.md §1.2
(the port's native ABI), §2 (game objects, D13), §3 (natives.log v2, D14), §5.4 (D16) and §8 (block files), and from
the lomn::fs contract comment (engine/include/lomn/fs.h) for the VFS. Never from the C++ engine.

- GameObject: one engine object the script can hold. Its handle is minted when it first passes through
  HostContext.create_variant (`A g` then), not when it is constructed. Before that it is not script-visible and its
  engine references are carried by the G of the record that mints it (natives.md §3.1).
- HostContext: what a native body may do besides reading its arguments: create_variant, object_arg, add_ref / release
  (engine references; each writes an `H` record once the object has a handle), engine_call (`C <seq>`, then Call; a
  swallowing native passes swallow=True: D16 and the `X <seq>` record), subsystem (lazily built per context), flags,
  vfs. A native never touches VM state otherwise.
- register_group / bind_natives: the native groups of Tasks 7b/8b/9b register a bind function; bind_natives runs them
  all (each one binds its rows, which marks them implemented).
- Vfs / BlockFiles: plain file reads under the install (Root/ resolved case-insensitively) and the `.blk` tables.
"""
from __future__ import annotations

import os
import struct
import sys
from pathlib import Path
from typing import Callable, NamedTuple, Optional

_RESEARCH = Path(__file__).resolve().parents[1]
if str(_RESEARCH) not in sys.path:
    sys.path.insert(0, str(_RESEARCH))

import osi_vm  # noqa: E402  (osi_vm aliases itself when it runs as __main__, so the classes are shared)
from osi_vm import NOTHING, T_NOTHING, CallResult, Variant, VmError, is_game  # noqa: E402


class HostFlags(NamedTuple):
    deterministic: bool = True    # D3: userealdeltatime is a logged no-op, the fake-time path stays on
    skip_video: bool = True       # D1: playcinematic completes instantly


class GameObject:
    """One engine object (D13). `engine_refs` is the number of references the engine holds before the object is first
    returned to the script (cited per class in natives.md §2.4). After minting, the heap holds the count: total =
    engine references + variant references."""

    def __init__(self, type_id: int, engine_refs: int = 0):
        self.type_id = type_id
        self.engine_refs = engine_refs
        self.handle = 0               # 0 until the first create_variant
        self.deleted = False

    def type(self) -> int:
        return self.type_id

    def on_delete(self) -> None:
        """The original's deleting destructor (SxReferenceCountable::Free 0x4f2bc0 -> vtable[0]); after the `F g`."""


class HostContext:
    def __init__(self, vm, heap, vfs=None, flags: Optional[HostFlags] = None, log: Optional[list] = None):
        self.vm, self.heap = vm, heap
        self._vfs = vfs
        self._flags = flags or HostFlags()
        self._objects: dict[int, GameObject] = {}       # live handle -> host object
        self._subsystems: dict[type, object] = {}
        self.test_objects: dict[str, GameObject] = {}   # conformance: the `returns.object` names (rule 18)
        if log is not None:
            vm.natives.return_log = log
        vm.host = self
        heap.free_hook = self._freed

    @classmethod
    def of(cls, vm) -> "HostContext":
        """The context attached to vm, or a default one (no VFS, default flags) attached now."""
        return vm.host if vm.host is not None else cls(vm, vm.heap)

    # --- game objects (natives.md §1.2 Results / Reference counts, §2)
    def create_variant(self, obj: Optional[GameObject], type_id: Optional[int] = None) -> Variant:
        """CreateVariant(T*): null -> nothing; otherwise +1 reference (inc [obj+4]), minting the handle on first use
        with the object's engine references. The variant's type tag is the overload's class (the native's declared
        return, `type_id`), the handle is the object's: one handle per live object, whatever the static type (§2.3).
        The handle is minted under the type of that first variant (its A g line)."""
        if obj is None:
            return NOTHING
        t = obj.type_id if type_id is None else type_id
        h = obj.handle
        if h and self._objects.get(h) is obj:
            return self.heap.duplicate(Variant(t, h))
        if obj.deleted:
            raise VmError("Undefined: create_variant on a deleted game object")
        h = self.heap.new_game(t, obj.engine_refs)
        obj.handle = h
        self._objects[h] = obj
        return Variant(t, h)

    def object_arg(self, call, i: int, cls: Optional[type] = None) -> Optional[GameObject]:
        """Argument i as its host object; nothing -> None. A handle minted outside this context (a stub, the replay)
        is adopted as a plain GameObject. A wrong class cannot reach a native (0x82's ConvertVariant refuses it), so
        it is an "Undefined" error here."""
        v = call.arg(i)
        if v.type == T_NOTHING:
            return None
        if not is_game(v.type):
            raise VmError("Undefined: object_arg type")
        obj = self._objects.get(v.value)
        if obj is None:
            if v.value not in self.heap.games:
                raise VmError("Undefined behaviour: game handle %08x is not live" % v.value)
            obj = GameObject(self.heap.game_type(v.value), 0)
            obj.handle = v.value
            self._objects[v.value] = obj
        if cls is not None and not isinstance(obj, cls):
            raise VmError("Undefined: object_arg type")
        return obj

    def add_ref(self, obj: GameObject) -> None:
        """An engine-side AddRef. On an object with a handle: +1 on the heap and `H <type>:<handle> +1`."""
        h = obj.handle
        if h and self._objects.get(h) is obj:
            self.heap.add_ref(h)
            self.vm.log_h(h, +1)
        else:
            obj.engine_refs += 1

    def release(self, obj: GameObject) -> None:
        """An engine-side Release. On an object with a handle: `H … -1`, then -1 on the heap; a total of 0 writes
        `F g` and deletes the host object (natives.md §2.2). Without a handle, 0 deletes it with no trace."""
        h = obj.handle
        if h and self._objects.get(h) is obj:
            self.vm.log_h(h, -1)
            self.heap.release(h)
        else:
            obj.engine_refs -= 1
            if obj.engine_refs <= 0 and not obj.deleted:
                obj.deleted = True
                obj.on_delete()

    def _freed(self, h: int) -> None:
        obj = self._objects.pop(h, None)
        if obj is not None:
            obj.handle = 0
            obj.deleted = True
            obj.on_delete()

    # --- nested engine calls (natives.md §1.2 Nested engine calls; D16)
    def engine_call(self, name, args=(), swallow: bool = False) -> CallResult:
        """Call(name, a0..a9) from inside the executing native: writes `C <seq> <name> <a0>..<a9>` (the query name),
        runs the call (its result is disposed by the call protocol) and returns its CallResult. An error propagates out
        of the native, unless swallow=True (only where the original catches: the three LoadArea blocks, natives.md
        §5.2, and the conformance `swallow` natives): then D16 rules 1-2 apply here (the VM stays as at the throw, the
        E line and `X <seq>` are written) and CallResult(found=True, ok=False) is returned. Outside any native (a host
        route) the records are the reserved `C -1 …` and, for a swallow, `X -1`."""
        vm = self.vm
        seq = vm.native_seq()
        vm.log_c(seq, name, args)
        try:
            r = vm.call(name, list(args))
        except VmError as e:
            if not swallow:
                raise
            vm.swallow_error(e, seq)
            return CallResult(True, False, NOTHING)
        if seq < 0 and swallow and not r.ok:
            # A host route (no native executing, seq -1: D16's process entries): the call was the outermost one, so
            # D10 already wrote its E line (and P) and left the VM as at the throw. The host's catch swallows it: no
            # last_error stays set, the error is recorded, and natives.log gets the reserved `X -1`.
            e = vm.last_error
            e.pc_after = vm.pc
            vm.swallowed.append(e)
            vm.last_error = None
            vm.log_x(seq)
        return r

    # --- the rest of the ABI
    def subsystem(self, cls):
        """One instance of `cls` per context, built lazily as cls(context) (the engine's singletons: saver, director…)."""
        s = self._subsystems.get(cls)
        if s is None:
            s = self._subsystems[cls] = cls(self)
        return s

    @property
    def flags(self) -> HostFlags:
        return self._flags

    @property
    def vfs(self):
        return self._vfs


# --- native groups (Tasks 7b/8b/9b self-register; bind_natives runs them all)
_GROUPS: dict[str, Callable] = {}


def register_group(name: str, fn: Callable) -> None:
    if name in _GROUPS:
        raise ValueError("native group %r registered twice" % name)
    _GROUPS[name] = fn


def unregister_group(name: str) -> None:
    _GROUPS.pop(name, None)


def bind_natives(reg, ctx: HostContext) -> None:
    """Runs every registered group's bind function, in registration order. Each binds its rows with reg.bind(key, fn),
    which marks them implemented."""
    for fn in list(_GROUPS.values()):
        fn(reg, ctx)


# --- the VFS (the lomn::fs contract: engine/include/lomn/fs.h header comment)
def _ascii_lower(s: str) -> str:
    return "".join(chr(ord(c) + 32) if "A" <= c <= "Z" else c for c in s)


class Vfs:
    """Read-only game paths ("Root/…") under the install directory. Paths are normalised Windows-style: '\\' -> '/',
    ASCII lower-case, duplicate '/' collapsed, "." dropped and ".." folded; a ".." that would climb out of the first
    segment consumes it, so the result no longer starts with root/ and the path is refused (BadMount). "root/…"
    resolves case-insensitively inside install_dir. "User/…" (read-write) is not modelled here."""

    def __init__(self, install_dir):
        self.root = Path(install_dir)

    @staticmethod
    def normalize(path: str) -> str:
        out: list[str] = []
        for seg in _ascii_lower(path.replace("\\", "/")).split("/"):
            if seg in ("", "."):
                continue
            if seg == "..":
                if out:
                    out.pop()
                continue
            out.append(seg)
        return "/".join(out)

    def _resolve(self, path: str) -> Optional[Path]:
        parts = self.normalize(path).split("/")
        if parts[0] != "root" or len(parts) < 2:
            return None
        cur = self.root
        for seg in parts[1:]:
            if not cur.is_dir():
                return None
            match = None
            for e in sorted(os.listdir(cur)):
                if _ascii_lower(e) == seg:
                    match = e
                    break
            if match is None:
                return None
            cur = cur / match
        return cur

    def exists(self, path: str) -> bool:
        p = self._resolve(path)
        return p is not None and p.is_file()

    def read(self, path: str) -> Optional[bytes]:
        p = self._resolve(path)
        return p.read_bytes() if p is not None and p.is_file() else None


# --- block files (tools/research/blk.py's format: 'BLKF', u32 version, u32 count, count x 56-byte entries)
class BlockEntry(NamedTuple):
    name: str
    block_size: int
    size: int
    flags: int
    offset: int


class BlockFiles:
    """The `.blk` tables under the install, read as data. An entry is {char name[40], u32 block size, u32 file size,
    u32 flags, u32 offset}; the names are sorted case-insensitively in the files read so far (blk.py `sortedci`), and
    `find` compares them case-insensitively (INFERRED for the original's Find; the path -> entry-name mapping that
    SrBlockFileExists uses, natives.md §8.2, is Task 7b's model, not this class's)."""

    HEADER = struct.Struct("<4sII")
    ENTRY = struct.Struct("<40sIIII")

    def __init__(self, install_dir, vfs: Optional[Vfs] = None):
        self.vfs = vfs or Vfs(install_dir)
        self._cache: dict[str, Optional[list[BlockEntry]]] = {}

    def entries(self, blk_path: str) -> Optional[list[BlockEntry]]:
        key = Vfs.normalize(blk_path)
        if key not in self._cache:
            data = self.vfs.read(blk_path)
            self._cache[key] = None if data is None else self._parse(data, blk_path)
        return self._cache[key]

    def _parse(self, data: bytes, where: str) -> list[BlockEntry]:
        if len(data) < self.HEADER.size:
            raise ValueError("%s: truncated .blk header" % where)
        magic, _ver, n = self.HEADER.unpack_from(data, 0)
        if magic != b"BLKF":
            raise ValueError("%s: bad .blk magic %r" % (where, magic))
        if self.HEADER.size + n * self.ENTRY.size > len(data):
            raise ValueError("%s: %d entries run past the end of the file" % (where, n))
        out = []
        for i in range(n):
            name, bs, fs, fl, off = self.ENTRY.unpack_from(data, self.HEADER.size + i * self.ENTRY.size)
            out.append(BlockEntry(name.split(b"\0")[0].decode("latin-1"), bs, fs, fl, off))
        return out

    def find(self, blk_path: str, name: str) -> Optional[BlockEntry]:
        ents = self.entries(blk_path)
        if ents is None:
            return None
        q = _ascii_lower(name)
        for e in ents:
            if _ascii_lower(e.name) == q:
                return e
        return None


__all__ = ["HostFlags", "GameObject", "HostContext", "register_group", "unregister_group", "bind_natives", "Vfs",
           "BlockEntry", "BlockFiles", "osi_vm"]
