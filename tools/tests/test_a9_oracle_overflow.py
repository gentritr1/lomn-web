"""M1a-4 fix round 1 (I-2): finite positions whose float products overflow, in tools/research/oracle/a9_oracle.py.

A tiny install written here (no game data): a model with a vertex at x = 3e38 placed at OBJ x = 3e38 (world corner inf:
the placement is skipped), two quads at x = 3e38 (finite corners, but the bounds centre (min + max) * 0.5 overflows: the
camera falls back to the origin) and a POS start at (3e38, 0, 3e38) (eye + the unit forward step rounds back to eye in x/z:
eye - at is vertical and LookAtRH's x axis is zero; the same fallback, keeping a given --yaw). When build/native/lomn_view exists, the shim's state log of each area must equal
the oracle's (the same rule on both sides); without it that half is skipped."""
import json
import math
import struct
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "research" / "oracle"))
import a9_oracle  # noqa: E402

VIEW = REPO / "build" / "native" / "lomn_view"
ORACLE = REPO / "tools" / "research" / "oracle" / "a9_oracle.py"


def u32(v):
    return struct.pack("<I", v)


def blk(entries):
    """'BLKF' v1: count, 56-byte entries {name[40], size, size, flags 2, offset} in name order, then the payloads."""
    entries = sorted(entries)
    out = bytearray(b"BLKF" + u32(1) + u32(len(entries)))
    off = 12 + 56 * len(entries)
    for name, data in entries:
        out += name.encode().ljust(40, b"\0") + u32(len(data)) + u32(len(data)) + u32(2) + u32(off)
        off += len(data)
    for _, data in entries:
        out += data
    return bytes(out)


def x_triangle(pos):
    """Binary .x ('xof 0303bin 0032'): one Mesh with 3 vertices, one face, one untextured material."""
    b = bytearray(b"xof 0303bin 0032")
    name = lambda s: b.extend(struct.pack("<HI", 1, len(s)) + s.encode())
    ints = lambda v: b.extend(struct.pack("<HI", 6, len(v)) + b"".join(u32(x) for x in v))
    floats = lambda v: b.extend(struct.pack("<HI", 7, len(v)) + b"".join(struct.pack("<f", x) for x in v))
    name("Mesh"); b.extend(struct.pack("<H", 10))
    ints([3]); floats(pos); ints([1, 3, 0, 1, 2])
    name("MeshMaterialList"); b.extend(struct.pack("<H", 10)); ints([1, 1, 0])
    name("Material"); b.extend(struct.pack("<H", 10)); floats([1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0]); b.extend(struct.pack("<H", 11))
    b.extend(struct.pack("<HH", 11, 11))
    return bytes(b)


def obj_table(recs):
    """{0, n, n, ptr 16} + 60-byte records {id (byte-reversed 4CC), pos, orient, 0, 6 x 0.0, flags} + relocation tail."""
    b = bytearray(u32(0) + u32(len(recs)) + u32(len(recs)) + u32(16))
    for rid, pos, flags in recs:
        b += rid[::-1].encode() + struct.pack("<3f", *pos) + struct.pack("<3f", 0, 0, 0) + u32(0) + struct.pack("<6f", *[0] * 6)
        b += u32(flags)
    return bytes(b + u32(12) + u32(1) + u32(0x00C0FFEE))


def pos_table(recs):
    b = bytearray(u32(0) + u32(len(recs)) + u32(12))
    for rid, pos, flags in recs:
        b += rid[::-1].encode() + struct.pack("<3f", *pos) + u32(flags)
    return bytes(b + u32(8) + u32(1) + u32(0x00C0FFEE))


QUAD = x_triangle([-8, 0, -8, 8, 0, -8, 8, 0, 8])
HUGE = x_triangle([3e38, 0, 0, 1, 0, 0, 0, 0, 1])
AREAS = {
    "ovf1": [("quad.x", QUAD), ("huge.x", HUGE),
             ("ovf1_obj.slb", obj_table([("huge", (3e38, 0, 0), 0x10), ("quad", (0, 0, -10), 0x10)]))],
    "ovf2": [("quad.x", QUAD), ("ovf2_obj.slb", obj_table([("quad", (3e38, 0, 0), 0x10), ("quad", (3e38, 0, -9), 0x10)]))],
    "ovf3": [("quad.x", QUAD), ("ovf3_obj.slb", obj_table([("quad", (0, 0, -10), 0x10)])),
             ("ovf3_pos.slb", pos_table([("strt", (3e38, 0, 3e38), 0x1000)]))],
}
CAM = {"eye_height": 3.0, "fov": 72.0, "near": 0.01, "far": 4700.0, "pitch": -10.0, "yaw": None, "width": 1280, "height": 720}


@pytest.fixture(scope="module")
def install(tmp_path_factory):
    root = tmp_path_factory.mktemp("ovf")
    lv = root / "data" / "levels" / "levt"
    lv.mkdir(parents=True)
    (lv / "levt.blk").write_bytes(blk([("readme.txt", b"x")]))
    for area, entries in AREAS.items():
        (lv / (area + ".blk")).write_bytes(blk(entries))
    return root


def oracle_camera(root, area, **over):
    ins = a9_oracle.Install(str(root), "levt", area)
    places, start = a9_oracle.load_area(ins)
    return places, a9_oracle.camera(places, start, dict(CAM, **over))


def test_overflowing_world_bounds_skip_the_placement(install):
    places, (eye, view, proj) = oracle_camera(install, "ovf1")
    assert [p["id"] for p in places] == ["quad"]  # "huge" (world x 6e38 = inf) is skipped
    assert all(math.isfinite(v) for v in list(eye) + view + proj)
    assert eye != (0.0, 0.0, 0.0)  # the bounds centre of the remaining quad, not the fallback


def test_overflowing_camera_falls_back_to_the_origin(install):
    places, (eye, view, proj) = oracle_camera(install, "ovf2")
    assert len(places) == 2  # finite corners: both placed
    assert eye == (0.0, 0.0, 0.0)
    assert all(math.isfinite(v) for v in view + proj)
    _, (eye3, view3, _) = oracle_camera(install, "ovf3", yaw=45.0)
    _, (_, view0, _) = oracle_camera(install, "ovf3", yaw=0.0)
    assert eye3 == (0.0, 0.0, 0.0)
    assert all(math.isfinite(v) for v in view3)
    assert view3 != view0  # the given --yaw is kept


@pytest.mark.skipif(not VIEW.exists(), reason="build/native/lomn_view not built")
@pytest.mark.parametrize("area,extra", [("ovf1", []), ("ovf2", []), ("ovf3", ["--yaw", "45"])])
def test_shim_equals_oracle_on_overflow(install, tmp_path, area, extra):
    out, state = tmp_path / "c.txt", tmp_path / "s.txt"
    r = subprocess.run([str(VIEW), "--install", str(install), "--level", "levt", "--area", area, "--backend", "null",
                        "--out", str(out), "--state-log", str(state)] + extra, capture_output=True, text=True, timeout=120)
    assert r.returncode == 0, r.stderr
    cam = json.loads(Path(str(out) + ".json").read_text())["camera"]
    assert ("warning" in cam) == (area != "ovf1"), cam
    r = subprocess.run([sys.executable, str(ORACLE), "--install", str(install), "--level", "levt", "--area", area,
                        "--compare", str(state)] + extra, capture_output=True, text=True, timeout=120)
    assert r.returncode == 0, r.stdout + r.stderr
