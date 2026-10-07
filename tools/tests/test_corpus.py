import json

from corpus import RangeIndex, build, module_path


def mod(obj, ranges, src=()):
    return {"obj_name": obj, "text_ranges": ranges, "source_files": list(src)}


def test_module_path_prefers_source_cpp():
    m = mod("GfMudBoss.obj", [], [r"C:\Projects\lego\Character\Bosses\GfMudBoss.h",
                                  r"C:\Projects\lego\Character\Bosses\GfMudBoss.cpp"])
    assert module_path(m) == "Character/Bosses/GfMudBoss.cpp"


def test_module_path_unplaced_obj():
    assert module_path(mod(r"obj\i386\dilib1.obj", [])) == "_unplaced/dilib1.cpp"


def test_range_lookup_boundaries():
    idx = RangeIndex([mod("A.obj", [[0x1000, 0x10]]), mod("B.obj", [[0x1010, 0x10]])])
    assert idx.lookup(0x1000) == "_unplaced/A.cpp"
    assert idx.lookup(0x100F) == "_unplaced/A.cpp"
    assert idx.lookup(0x1010) == "_unplaced/B.cpp"
    assert idx.lookup(0x1020) == "_unplaced/_no_module.cpp"
    assert idx.lookup(0x0FFF) == "_unplaced/_no_module.cpp"


def test_build_groups_and_sorts(tmp_path):
    (tmp_path / "funcs").mkdir()
    rows = [("0x00001008", "A::second"), ("0x00001000", "A::first"), ("0x00002000", "Z::lost")]
    (tmp_path / "functions.jsonl").write_text("".join(json.dumps({"addr": a, "name": n}) + "\n" for a, n in rows))
    for a, n in rows:
        (tmp_path / "funcs" / f"{int(a, 16):08x}.c").write_text(f"// {a} {n}\nbody_{n}\n")
    fmap = build(tmp_path, [mod("A.obj", [[0x1000, 0x100]], [r"C:\Projects\lego\Area\A.cpp"])], tmp_path / "out")
    assert fmap == {"0x00001008": "Area/A.cpp", "0x00001000": "Area/A.cpp", "0x00002000": "_unplaced/_no_module.cpp"}
    text = (tmp_path / "out" / "corpus" / "Area" / "A.cpp").read_text()
    assert text.index("A::first") < text.index("A::second")
