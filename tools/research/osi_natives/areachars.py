"""M2a-3 T11b: an area's characters for the Python oracle's `AreaDirector.area_source` seam, read from the area's own
`<area>_CHAR.slb` (natives.md §4/§5: `LoadCharacters` 0x53f3f0 asks `AddCharacter` for each record; `GetSLKFile` null skips the
block, 0x5002ad). Written from the data and natives.md only, never from the C++ engine.

File format (EXECUTED on the install's 81 `_char.slb`; the 30 that fail a strict size check are cinematic files with another
layout, or area files whose records carry non-empty lists): u32 (equal to the count in every area file; its meaning is not
cited), u32 count, u32 offset of the records (`LoadCharacters` reads the count at [this+0x9c] beside the pointer at
[this+0xa0], so the count is the dword at +4; T11b review m-1) (12 in every
area file), count records of 0x34 bytes whose first dword is the character id: its four ASCII bytes are stored reversed, so the
little-endian dword equals ConvertStringToID of the text. The last dword is the relocation trailer 0x00c0ffee. Only the id is
modelled (a character's motion id is its own id, natives.md §2.4 / the `initmotionsystem` row)."""
from __future__ import annotations

import struct

from .directors import AreaObject
from .framework import BlockFiles

RECORD = 0x34
TRAILER = 0x00C0FFEE


def read_char_ids(data: bytes) -> list[int]:
    if len(data) < 16 or struct.unpack_from("<I", data, len(data) - 4)[0] != TRAILER:
        raise ValueError("_char.slb: no 0x00c0ffee trailer")
    _first, count, ptr = struct.unpack_from("<III", data, 0)
    if ptr + count * RECORD > len(data) - 4:
        raise ValueError("_char.slb: %d records run past the end of the file" % count)
    return [struct.unpack_from("<I", data, ptr + i * RECORD)[0] for i in range(count)]


def area_characters(vfs, level: str, area: str) -> list[AreaObject]:
    """The characters block of `levels/<level>/<area>.blk`: [] when the file or its CHAR entry is missing."""
    path = "Root/Data/Levels/%s/%s.blk" % (level, area)
    e = BlockFiles(vfs.root, vfs).find(path, "%s_char.slb" % area)
    if e is None:
        return []
    data = vfs.read(path)[e.offset:e.offset + e.size]
    return [AreaObject("characters", i, i) for i in read_char_ids(data)]
