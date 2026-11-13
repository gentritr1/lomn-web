"""M2a-3 T11b: the Python oracle loads an area's characters from its own `_char.slb` (tools/research/osi_natives/areachars.py),
written from docs/osi/natives.md §4/§5 and the install's data, not from the C++ engine.

Format (EXECUTED on the 81 `_char.slb` of the install, `levels/*/*.blk`): u32 count, u32 capacity, u32 pointer to the records
(12 in every area file); records of 0x34 bytes whose first dword is the character id (its four ASCII bytes reversed, so the
little-endian dword is the id as ConvertStringToID reads the text); then the relocation table and the trailer 0x00c0ffee.
Data-free cases build the file bytes; the real-data cases skip without LOMN_DATA."""
import os
import struct
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "research"))
sys.path.insert(0, str(REPO / "tools" / "tests"))

from osi_natives import areachars  # noqa: E402

LOMN_DATA = os.environ.get("LOMN_DATA")


def char_slb(ids, extra=b""):
    """A `_char.slb` image: header, one 0x34 record per id (empty lists point at the end of the records), trailer."""
    end = 12 + 52 * len(ids)
    body = struct.pack("<III", len(ids), len(ids), 12)
    for i in ids:
        body += i[::-1] + b"\0" * 28 + struct.pack("<IIIII", 0, 0, end, 0, end)
    relocs = [8] + [12 + 52 * k + o for k in range(len(ids)) for o in (40, 48)]
    return body + extra + b"".join(struct.pack("<I", r) for r in reversed(relocs)) + struct.pack("<II", len(relocs), 0xC0FFEE)


def test_reader_returns_the_record_ids_in_file_order():
    assert areachars.read_char_ids(char_slb([b"vlgr", b"cnt1", b"vlgr"])) == [0x766C6772, 0x636E7431, 0x766C6772]
    assert areachars.read_char_ids(char_slb([])) == []


def test_reader_takes_the_count_next_to_the_record_pointer():
    """LoadCharacters 0x53f3f0 reads the count at [this+0x9c] and the records at [this+0xa0] (adjacent dwords), so the count
    is u32 +4, beside the pointer at +8 (T11b review m-1; the census and natives.md §5.4 read +4). +0 equals +4 in every
    stock file, so only a file where they differ tells the two readings apart."""
    img = bytearray(char_slb([b"vlgr", b"bats"]))
    struct.pack_into("<I", img, 0, 7)
    assert areachars.read_char_ids(bytes(img)) == [0x766C6772, 0x62617473]


@pytest.mark.parametrize("mutate", [lambda b: b[:-4], lambda b: b[:10], lambda b: b[:-4] + b"\0\0\0\0",
                                    lambda b: b[:8] + struct.pack("<I", 100000) + b[12:]])
def test_reader_refuses_a_malformed_file(mutate):
    with pytest.raises(ValueError):
        areachars.read_char_ids(mutate(char_slb([b"vlgr", b"vlgr"])))


def blk(entries):
    out = struct.pack("<4sII", b"BLKF", 1, len(entries))
    off = 12 + 56 * len(entries)
    body = b""
    for name, data in entries:
        out += struct.pack("<40sIIII", name.encode(), len(data), len(data), 0, off + len(body))
        body += data
    return out + body


def make_install(tmp_path, ids=(b"vlgr", b"vlgr", b"onua")):
    d = tmp_path / "install" / "data" / "levels" / "lev1"
    d.mkdir(parents=True)
    (d / "vllg.blk").write_bytes(blk([("vllg_CHAR.slb", char_slb(list(ids))), ("vllg_OBJ.slb", b"xxxx")]))
    (d / "emty.blk").write_bytes(blk([("emty_LIGHT.slb", b"xxxx")]))
    return tmp_path / "install"


def test_source_reads_the_area_file_case_insensitively_and_skips_a_missing_one(tmp_path):
    from osi_natives import framework
    vfs = framework.Vfs(make_install(tmp_path))
    objs = areachars.area_characters(vfs, "lev1", "vllg")
    assert [(o.block, o.obj_id, o.motion_id) for o in objs] == [("characters", i, i) for i in (0x766C6772, 0x766C6772, 0x6F6E7561)]
    assert areachars.area_characters(vfs, "lev1", "emty") == []                 # no CHAR entry: the block is skipped (0x5002ad)
    assert areachars.area_characters(vfs, "lev1", "none") == []                 # no area file


def test_x3_removal_fires_one_onevent_per_tracked_character(tmp_path):
    """setcurrentlevel lev1; setcurrentarea vllg; setcurrentlevel lev2: the oracle now loads the area's characters, so
    RemoveArea calls OnEvent(0x2b, 'vlgr') (and 'onua') as the C++ engine does. 'vlgr' twice in the file: the second record's id
    is already tracked, so both engines skip it whole before any Add (natives.md gap 1; the original gives it a numbered id),
    one map entry, so one removal event for it."""
    from test_osi_natives_directors import make_vm
    asm = ('.function main 0\nPushConstantString "lev1"\nCallGameFunction gcareadirector.setcurrentlevel 1\nPop\n'
           'PushConstantString "vllg"\nCallGameFunction gcareadirector.setcurrentarea 1\nPop\n'
           'PushConstantString "lev2"\nCallGameFunction gcareadirector.setcurrentlevel 1\nReturn\n'
           '.function OnEvent 2\nPushConstanti8 1\nReturn\n')
    vm, ctx, tr, log = make_vm(asm, install=make_install(tmp_path))
    assert vm.call("main", []).ok
    cs = [l for l in log if l.startswith("C ")]
    assert [l.split()[2:5] for l in cs] == [["OnEvent", "0010:0000002b", "0010:766c6772"],
                                            ["OnEvent", "0010:0000002b", "0010:6f6e7561"]]


@pytest.mark.skipif(not LOMN_DATA, reason="SKIPPED (no LOMN_DATA)")
def test_real_vllg_records():
    from osi_natives import framework
    vfs = framework.Vfs(Path(LOMN_DATA) / "install")
    ids = [o.obj_id for o in areachars.area_characters(vfs, "lev1", "vllg")]
    assert ids == [0x766C6772] * 6                                               # `vllg_CHAR.slb` 384 bytes: 6 'vlgr' records
    assert [o.obj_id for o in areachars.area_characters(vfs, "lev0", "frnt")] == []     # natives.md §5.3: no frnt_char.slb
    assert [o.obj_id for o in areachars.area_characters(vfs, "fren", "frnt")] == [0x746E6761] * 2  # levels/fren/frnt.blk: 'tnga' x2


def test_a_record_whose_id_is_already_tracked_is_skipped_whole(tmp_path):
    """natives.md F7a item 2 / gap 1: a tracked id would get a numbered unique id in the original, not modelled, so both engines
    skip the record: no entry, so the second area's removal makes no OnEvent (x-sequence y1 of tools/osi/area_diff.py)."""
    from test_osi_natives_directors import make_vm
    inst = make_install(tmp_path, ids=(b"vlgr",))
    (inst / "data" / "levels" / "lev1" / "scrp.blk").write_bytes(blk([("scrp_CHAR.slb", char_slb([b"vlgr"]))]))
    asm = ('.function main 0\nPushConstantString "lev1"\nCallGameFunction gcareadirector.setcurrentlevel 1\nPop\n'
           'PushConstantString "vllg"\nCallGameFunction gcareadirector.setcurrentarea 1\nPop\n'
           'PushConstantString "scrp"\nCallGameFunction gcareadirector.setcurrentarea 1\nPop\n'
           'PushConstantString "lev2"\nCallGameFunction gcareadirector.setcurrentlevel 1\nReturn\n'
           '.function OnEvent 2\nPushConstanti8 1\nReturn\n')
    vm, ctx, tr, log = make_vm(asm, install=inst)
    assert vm.call("main", []).ok
    assert [l for l in log if l.startswith("C ")] == []
