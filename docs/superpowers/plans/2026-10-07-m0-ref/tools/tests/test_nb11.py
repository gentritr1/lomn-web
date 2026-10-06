import re
import struct

import pytest

from nb11 import Nb11, Nb11Error, read_numeric, read_pascal


def test_numeric_inline_and_typed():
    assert read_numeric(struct.pack("<H", 0x1234), 0) == (0x1234, 2)
    assert read_numeric(struct.pack("<HH", 0x8002, 0xBEEF), 0) == (0xBEEF, 4)
    assert read_numeric(struct.pack("<Hi", 0x8003, -5), 0) == (-5, 6)


def test_numeric_unknown_leaf_raises():
    with pytest.raises(Nb11Error):
        read_numeric(struct.pack("<H", 0x800A), 0)


def test_pascal():
    assert read_pascal(b"\x03abcX", 0) == ("abc", 4)


def test_not_pe_rejected():
    with pytest.raises(Nb11Error, match="not a PE"):
        Nb11(b"\0" * 64)


def test_publics_match_probe(nb):
    pubs = nb.publics()
    names = {n for _, n in pubs}
    assert len(names) == 7057
    classes = {m.group(1) for n in names for m in [re.match(r"\?[^@]+@([A-Za-z_0-9]+)@@", n)] if m}
    assert len(classes) == 505
    assert sum(c.startswith("Gc") for c in classes) == 160
    assert (0x738BC4, "?mD3DDevice@ScDrawableContext@@2PAUIDirect3DDevice8@@A") in pubs


def test_modules(nb):
    mods = nb.modules()
    assert len(mods) == 675
    assert sum(1 for m in mods if m.text_ranges) == 484
    assert sum(1 for m in mods if m.source_files) == 37
    mud = [m for m in mods if m.obj_name == "GfMudBoss.obj"][0]
    assert mud.source_files[0] == r"C:\Projects\lego\Character\Bosses\GfMudBoss.cpp"


def test_types_and_locals(nb):
    tl, pl = nb.type_lines(), nb.proc_lines()
    assert sum(l.startswith("S ") for l in tl) == 473
    assert sum(l.startswith("M ") and not l.endswith(" vftable") for l in tl) == 1817
    assert sum(l.startswith("F ") for l in pl) == 580
    assert sum(l.startswith("V ") for l in pl) == 1265
    i = tl.index("S GcCharacter 872")
    assert "M 276 struct:ScAutoRefPtr_GcCharacterMoveInterpreter 4 mMoveInterpreter" in tl[i:i + 60]
    j = tl.index("S ScAutoRefPtr_GcMoveInterpreter 4")
    assert tl[j + 1] == "M 0 ptr:struct:GcMoveInterpreter 4 mPointee"


def test_imports(nb):
    imp = nb.imports()
    assert len(imp) == 16 and sum(map(len, imp.values())) == 117
    assert imp["d3d8.dll"] == ["Direct3DCreate8"]


def test_fake_delta_time_initial_value(nb):
    va = next(v for v, n in nb.publics() if n == "?sFakeDeltaTime@ScProcessManager@@2NA")
    rva = va - nb.image_base
    name, sva, _, raw, _ = next(s for s in nb.sections if s[1] <= rva < s[1] + max(s[2], s[4]))
    assert name == ".data"
    assert struct.unpack_from("<d", nb.d, raw + rva - sva)[0] == 0.006666666828095913
