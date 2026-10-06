from census import census, render_md
from surface import render


def test_surface_render_counts():
    md = render({"d3d8.dll": ["Direct3DCreate8"]},
                [{"addr": "0x1", "iface": "IDirect3DDevice8", "method": "SetRenderState"},
                 {"addr": "0x2", "iface": "IDirect3DDevice8", "method": "SetRenderState"}],
                {"0x1": "A::f", "0x2": "B::g"}, {"IDirect3DDevice8": {"SetRenderState": 50, "Present": 15}})
    assert "## PE imports: 1 DLLs, 1 functions" in md
    assert "## IDirect3DDevice8: 1 of 2 methods used" in md
    assert "| 50 | SetRenderState | 2 | `A::f`, `B::g` |" in md


def test_census_areas(tmp_path):
    lev = tmp_path / "data" / "levels" / "lev1"
    lev.mkdir(parents=True)
    (lev / "lev1.blk").write_bytes(b"c" * 10)
    (lev / "vllg.blk").write_bytes(b"a" * 5)
    (lev / "cave.blk").write_bytes(b"a" * 7)
    (tmp_path / "data" / "Main.blk").write_bytes(b"m" * 3)
    c = census(tmp_path)
    assert c["total_files"] == 4 and c["boot_set"] == {"data/Main.blk": 3}
    assert c["largest_area"] == {"level": "lev1", "area": "cave", "area_bytes": 7, "with_common_bytes": 17}
    assert "largest: `lev1/cave`" in render_md(c)


def test_real_census(lomn_data):
    c = census(lomn_data / "install")
    assert c["total_files"] >= 510 and c["largest_area"] is not None
