"""M2a-0 T3: the native registry (tools/osi/registry.py -> gen/osi/natives.json, docs/osi/types.md).

Numbers pinned here were measured on 2026-10-08 against the canonical exe, the re-run corpus in gen/decomp and Base.osi
(the commit body names the commands): 1,056 registrations, 1,049 distinct keys, 7 duplicated keys (all gchudsprite.*),
728 Base.osi pairs of which 78 are not registered, 45 game types, 391 .CRT initializers.

INDEPENDENCE. The generator reads the decompiled text (Ghidra). Three other readers check it here, none of which shares
its code path: (1) `reg_sites.json`, written by tools/decomp/seed_addrs.py from raw `E8` / `push imm32` bytes, matched to
rows by enclosing function + statement order (`test_fnptr_matches_reg_sites_for_every_row`, with its own matching code);
(2) the argument pushes in front of each call (`registry.asm_check`, re-asserted with pinned counts); (3) the bytes of the
map-insert branch that decides the duplicate winner (`test_duplicate_winner_branch_bytes`)."""
import importlib.util
import json
import struct
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
DOC = REPO / "docs" / "osi" / "types.md"


def _load_generator():
    spec = importlib.util.spec_from_file_location("osi_registry", REPO / "tools" / "osi" / "registry.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture(scope="module")
def reg():
    return _load_generator()


# ----------------------------------------------------------------------------------------------- synthetic fixtures
BASE = 0x400000
RDATA_VA = BASE + 0x2000


def synthetic_exe(strings: dict[int, bytes]) -> bytes:
    """Minimal PE32 with one section `.rdata` (RVA 0x2000, 0x100 bytes) holding `strings` at the given VAs."""
    rdata = bytearray(0x100)
    for va, s in strings.items():
        rdata[va - RDATA_VA:va - RDATA_VA + len(s) + 1] = s + b"\0"
    hdr = bytearray(0x400)
    hdr[0:2] = b"MZ"
    pe = 0x80
    struct.pack_into("<I", hdr, 0x3C, pe)
    hdr[pe:pe + 4] = b"PE\0\0"
    struct.pack_into("<HH", hdr, pe + 4, 0x14C, 1)
    struct.pack_into("<H", hdr, pe + 20, 0xE0)
    struct.pack_into("<I", hdr, pe + 24 + 28, BASE)
    sec = pe + 24 + 0xE0
    hdr[sec:sec + 8] = b".rdata".ljust(8, b"\0")
    struct.pack_into("<IIII", hdr, sec + 8, 0x100, 0x2000, 0x100, 0x400)
    return bytes(hdr) + bytes(rdata)


S_CLASS, S_FUNC, S_FUNC2, S_SHORT_CLS, S_SHORT_FN, S_TYPE = (RDATA_VA + o for o in (0x00, 0x20, 0x40, 0x60, 0x70, 0x80))

SYNTH_INIT = """// 0x00401000 FUN_00401000

void FUN_00401000(void)

{
  /* RegisterFunction mentioned in a comment must not count */
  _ScBaseString::_ScBaseString(&local_20,s_GcThing_00402000);
  _ScBaseString::_ScBaseString(&local_28,s_Run_00402020);
  ScOSISystem::RegisterFunction
            (ScGlobalOSISystem::theOSISystem,&local_20,&local_28,FUN_00401100,'\\x02','\\x01',0x10,
             0x12,0xf,0xf,0xf,0xf,0xf,0xf,0xf,0xf);
  _ScBaseString::_ScBaseString(&local_18,&DAT_00402060);
  _ScBaseString::_ScBaseString(&local_10,&DAT_00402070);
  if (theOSITypeGcThing == 0) {
    _ScBaseString::_ScBaseString(&local_30,s_GcThing_00402000);
    theOSITypeGcThing = ScOSISystem::RegisterType(ScGlobalOSISystem::theOSISystem,&local_30);
  }
  uVar16 = 0xf;
  uVar1 = GetVariantType<float>::GetIt();
  uVar2 = GetVariantType<Other*>::GetIt();
  uVar12 = '\\x03';
  pcVar11 = FUN_00401140;
  p_Var10 = &local_10;
  p_Var9 = &local_18;
  ScOSISystem::RegisterFunction
            (pSVar8,p_Var9,p_Var10,pcVar11,uVar12,uVar12,theOSITypeGcThing,uVar1,uVar2,0xf,0xf,
             0xf,0xf,0xf,0xf,uVar16);
  return;
}
"""
GETIT_OTHER = """// 0x00401200 GetVariantType<Other*>::GetIt

ushort __cdecl GetVariantType<Other*>::GetIt(void)

{
  if (theOSITypeOther == 0) {
    _ScBaseString::_ScBaseString(&local_10,s_Other_00402080);
    theOSITypeOther = ScOSISystem::RegisterType(ScGlobalOSISystem::theOSISystem,&local_10);
  }
  return theOSITypeOther;
}
"""
GETIT_FLOAT = """// 0x00401210 GetVariantType<float>::GetIt

ushort __cdecl GetVariantType<float>::GetIt(void)

{
  return 0x12;
}
"""


def synthetic_corpus(tmp_path: Path, init_text: str = SYNTH_INIT) -> Path:
    c = tmp_path / "decomp"
    (c / "funcs").mkdir(parents=True)
    (c / "funcs" / "00401000.c").write_text(init_text)
    (c / "funcs" / "00401200.c").write_text(GETIT_OTHER)
    (c / "funcs" / "00401210.c").write_text(GETIT_FLOAT)
    funcs = [{"addr": "0x00401000", "name": "FUN_00401000", "size": 0x100}, {"addr": "0x00401100", "name": "FUN_00401100", "size": 8},
             {"addr": "0x00401140", "name": "FUN_00401140", "size": 8}, {"addr": "0x00401200", "name": "GetVariantType<Other*>::GetIt", "size": 8},
             {"addr": "0x00401210", "name": "GetVariantType<float>::GetIt", "size": 8}]
    (c / "functions.jsonl").write_text("".join(json.dumps(f) + "\n" for f in funcs))
    sites = [{"site": "0x00401050", "fnptr": "0x00401100", "candidates": 1, "enclosing": "0x00401000"},
             {"site": "0x004010a0", "fnptr": "0x00401140", "candidates": 1, "enclosing": "0x00401000"}]
    (c / "reg_sites.json").write_text(json.dumps(sites))
    (c / "symbols.txt").write_text("0x401000 FUN_00401000\n")
    return c


@pytest.fixture
def synth(reg, tmp_path):
    exe = synthetic_exe({S_CLASS: b"GcThing", S_FUNC: b"Run", S_FUNC2: b"unused", S_SHORT_CLS: b"Short", S_SHORT_FN: b"Fn",
                         S_TYPE: b"Other"})
    return reg.Image(exe), synthetic_corpus(tmp_path)


# ----------------------------------------------------------------------------------------------- synthetic tests
def test_synthetic_two_calls_parsed_by_statement(reg, synth):
    image, corpus = synth
    rows, replay = reg.parse_corpus(corpus, image, [0x401000])
    assert len(rows) == 2                      # the comment's mention is ignored
    a, b = rows
    assert (a["cls"], a["name"], a["body"], a["max"], a["min"]) == ("GcThing", "Run", 0x401100, 2, 1)   # call spans two lines
    assert a["raw_types"] == [0x10, 0x12] + [0xF] * 8
    # second call: &DAT_ names read from the image, arguments hoisted into locals, GetIt results, a registered type
    assert (b["cls"], b["name"], b["body"], b["max"], b["min"]) == ("Short", "Fn", 0x401140, 3, 3)
    assert b["raw_types"][:3] == [("game", "GcThing"), 0x12, ("game", "Other")]
    assert [e["class"] for e in replay.order] == ["GcThing", "Other"]   # guard first, then the lazy GetIt
    assert (replay.guard_events, replay.getit_events) == (1, 1)
    assert [r["phase"] for r in rows] == ["crt", "crt"] and [r["crt_index"] for r in rows] == [0, 0]


def test_synthetic_type_names_and_nothing_slots(reg):
    assert reg.type_name(0xF, 2, 5) is None            # unused slot
    assert reg.type_name(0xF, 2, 1) == "nothing"       # declared nothing
    assert reg.type_name(0x10, 2, 0) == "int" and reg.type_name(0x12, 2, 0) == "float"
    assert reg.type_name(0x15, 2, 0) == "colour" and reg.type_name(0x16, 2, 0) == "string"
    assert reg.type_name(0x200, 2, 0) == "byref"
    assert reg.type_name(("game", "GcX"), 2, 0) == "type:GcX"
    assert reg.type_name(0x0, 2, 0) == "unknown:0x0" and reg.type_name(0x18, 2, 0) == "unknown:0x18"


def test_synthetic_unresolvable_arguments_raise(reg, synth, tmp_path):
    image, corpus = synth
    bad = SYNTH_INIT.replace("uVar12 = '\\x03';", "")
    (corpus / "funcs" / "00401000.c").write_text(bad)
    with pytest.raises(reg.RegistryError, match="uVar12"):
        reg.parse_corpus(corpus, image, [0x401000])


def test_synthetic_unguarded_register_type_raises(reg, synth):
    image, corpus = synth
    (corpus / "funcs" / "00401000.c").write_text(SYNTH_INIT.replace("if (theOSITypeGcThing == 0) {", "{"))
    with pytest.raises(reg.RegistryError, match="unguarded RegisterType"):
        reg.parse_corpus(corpus, image, [0x401000])


SYNTH_INIT2 = """// 0x00401300 FUN_00401300

void FUN_00401300(void)

{
  _ScBaseString::_ScBaseString(&local_20,s_Short_00402060);
  _ScBaseString::_ScBaseString(&local_28,s_Fn_00402070);
  if (theOSITypeGcLater == 0) {
    _ScBaseString::_ScBaseString(&local_30,s_GcLater_00402090);
    theOSITypeGcLater = ScOSISystem::RegisterType(ScGlobalOSISystem::theOSISystem,&local_30);
  }
  ScOSISystem::RegisterFunction
            (ScGlobalOSISystem::theOSISystem,&local_20,&local_28,FUN_00401400,'\\x01','\\x01',theOSITypeGcLater,0xf,
             0xf,0xf,0xf,0xf,0xf,0xf,0xf,0xf);
  return;
}
"""


def test_synthetic_registration_order_follows_crt_order_not_address_order(reg, synth, tmp_path):
    _, corpus = synth
    (corpus / "funcs" / "00401300.c").write_text(SYNTH_INIT2)
    exe = synthetic_exe({S_CLASS: b"GcThing", S_FUNC: b"Run", S_SHORT_CLS: b"Short", S_SHORT_FN: b"Fn", S_TYPE: b"Other",
                         RDATA_VA + 0x90: b"GcLater"})
    sites = json.loads((corpus / "reg_sites.json").read_text())
    sites.append({"site": "0x00401350", "fnptr": "0x00401400", "candidates": 1, "enclosing": "0x00401300"})
    (corpus / "reg_sites.json").write_text(json.dumps(sites))
    rows, replay = reg.parse_corpus(corpus, reg.Image(exe), [0x401300, 0x401000])   # 0x401300 listed (so run) first
    assert [e["class"] for e in replay.order] == ["GcLater", "GcThing", "Other"]
    assert [e["rank"] for e in replay.order] == [0, 1, 2]
    assert [r["crt_index"] for r in rows] == [0, 1, 1]
    assert rows[0]["raw_types"][0] == ("game", "GcLater")


def test_synthetic_winner_follows_crt_order_not_address_order(reg, synth):
    """Two initializers register the same key (Short.Fn). 0x401300 is listed FIRST in .CRT although it has the higher
    address (and the higher call-site address), so it runs first and its registration is the one the map keeps."""
    _, corpus = synth
    (corpus / "funcs" / "00401300.c").write_text(SYNTH_INIT2)
    exe = synthetic_exe({S_CLASS: b"GcThing", S_FUNC: b"Run", S_SHORT_CLS: b"Short", S_SHORT_FN: b"Fn", S_TYPE: b"Other",
                         RDATA_VA + 0x90: b"GcLater"})
    sites = json.loads((corpus / "reg_sites.json").read_text())
    sites.append({"site": "0x00401350", "fnptr": "0x00401400", "candidates": 1, "enclosing": "0x00401300"})
    (corpus / "reg_sites.json").write_text(json.dumps(sites))
    rows, _ = reg.parse_corpus(corpus, reg.Image(exe), [0x401300, 0x401000])
    for r, site in zip(rows, (0x401350, 0x401050, 0x4010A0)):
        r["site"] = site
    same = [r for r in rows if (r["cls"], r["name"]) == ("Short", "Fn")]
    assert [(r["init"], r["body"]) for r in same] == [(0x401300, 0x401400), (0x401000, 0x401140)]
    flags = reg.winner_flags(rows)
    assert [(r["init"], f) for r, f in zip(rows, flags) if r in same] == [(0x401300, True), (0x401000, False)]
    assert flags.count(True) == 2                       # GcThing.Run, and the Short.Fn winner
    # the answer does not depend on how the list is ordered
    assert reg.winner_flags(rows[::-1]) == flags[::-1]
    # a scinit registrar runs after every .CRT initializer, whatever its address
    late = dict(same[0], phase="scinit", crt_index=None, init=0x400100)
    assert reg.winner_flags(rows + [late])[-1] is False


def test_synthetic_cross_check_reports_fnptr_disagreement(reg, synth):
    image, corpus = synth
    rows, _ = reg.parse_corpus(corpus, image, [0x401000])
    assert reg.cross_check(rows, corpus)["bad"] == []
    sites = json.loads((corpus / "reg_sites.json").read_text())
    sites[1]["fnptr"] = "0x00401148"
    (corpus / "reg_sites.json").write_text(json.dumps(sites))
    bad = reg.cross_check(rows, corpus)["bad"]
    assert len(bad) == 1 and "fnptr" in bad[0][1]
    (corpus / "reg_sites.json").write_text(json.dumps(sites[:1]))     # a site missing for the second call
    assert any("more decomp calls" in m for _, m in reg.cross_check(rows, corpus)["bad"])


def test_statements_split_on_semicolons_not_lines(reg):
    st = reg.statements("a = f(1,\n   2);\n if (x) {\n  b = 'c';\n }\n  g(';', \")\");")
    assert st == ["a = f(1, 2)", "if (x)", "b = 'c'", "g(';', \")\")"]


def test_parse_int_forms(reg):
    assert [reg.parse_int(t) for t in ("0xf", "12", "'\\x05'", "'\\0'", "'\\n'", "' '", "FUN_1", "uVar1")] == [
        15, 12, 5, 0, 10, 32, None, None]


def test_hash_token_is_the_case_folded_polynomial(reg):
    assert reg.hash_token("A") == reg.hash_token("a") == 0x41
    assert reg.hash_token("ab") == 0x41 * 0x83 + 0x42
    assert reg.hash_token("GcTimer") == reg.hash_token("gctimer")
    assert reg.hash_token("a" * 40) < 1 << 32           # wraps to 32 bits


# ------------------------------------------------------------------------------------------------ measured pins
MISSING = (
    'bettersaver.getbooleanvalue', 'bettersaver.save', 'bettersaver.setbooleanvalue', 'gcareadirector.loadarea',
    'gccharacter.getteamid', 'gccharacter.rendercylinder', 'gccharacter.renderrays',
    'gccharacter.rendertriggerboxes', 'gccharacter.rendertriggerplanes', 'gccharacter.setclimbable',
    'gccharacter.setorientationrange', 'gccharacter.setteamid', 'gccharacter.settexturestate',
    'gcgame.getcurrentcamera', 'gclegocamera.setcameraaispline', 'gclegolite.create', 'gclegolite.createlight',
    'gclegolite.fire', 'gclegolite.lightning', 'gclegolite.lightoff', 'gclegolite.orbit', 'gclegolite.pulsate',
    'gclegolite.setglobalambient', 'gclegolite.togglelight', 'gclegolite.togglelite',
    'gclegosounddirector.get3dlistenerdistancefactor', 'gclegosounddirector.get3dlistenerdopplerfactor',
    'gclegosounddirector.get3dlistenerfrontcomponent', 'gclegosounddirector.get3dlistenerpositioncomponent',
    'gclegosounddirector.get3dlistenerrollofffactor', 'gclegosounddirector.get3dlistenerupcomponent',
    'gclegosounddirector.get3dlistenervelocitycomponent', 'gclegosounddirector.get3dsoundconeangle',
    'gclegosounddirector.get3dsoundconeorientationcomponent', 'gclegosounddirector.get3dsoundconeoutsidevolume',
    'gclegosounddirector.get3dsoundmaxdistance', 'gclegosounddirector.get3dsoundmindistance',
    'gclegosounddirector.get3dsoundpositioncomponent', 'gclegosounddirector.get3dsoundvelocitycomponent',
    'gclegosounddirector.getsoundfrequency', 'gclegosounddirector.getsoundpan',
    'gclegosounddirector.getsoundvolume', 'gclegosounddirector.set3dlistenerdistancefactor',
    'gclegosounddirector.set3dlistenerdopplerfactor', 'gclegosounddirector.set3dlistenerorientation',
    'gclegosounddirector.set3dlistenerposition', 'gclegosounddirector.set3dlistenerrollofffactor',
    'gclegosounddirector.set3dlistenervelocity', 'gclegosounddirector.set3dsoundconeangles',
    'gclegosounddirector.set3dsoundconeorientation', 'gclegosounddirector.set3dsoundconeoutsidevolume',
    'gclegosounddirector.set3dsoundmaxdistance', 'gclegosounddirector.set3dsoundvelocity',
    'gclegosounddirector.setsoundfrequency', 'gclegosounddirector.setsoundpan',
    'gclegosounddirector.setsoundplayoffset', 'gclegosounddirector.setsoundvolume',
    'gclegosounddirector.stopsound', 'gctoa.compareagainstsequence', 'gctoa.launchelementalpower',
    'gctoa.setclimbable', 'gctoa.setupdraggertipgeometry', 'gctoa.userealtime', 'gcwavesound.create',
    'gcwavesound.enableevents', 'gcwavesound.getlength', 'gcwavesound.getvolume', 'gcwavesound.isplaying',
    'gcwavesound.openfile', 'gcwavesound.pause', 'gcwavesound.play', 'gcwavesound.playmarker',
    'gcwavesound.setvolume', 'gcwavesound.stop', 'lsdebugger.setcollisionboxes', 'lsdebugger.settriggerboxes',
    'lsdebugger.settriggerplanes', 'lsdebugger.setwireframe',
)

TYPE_ORDER = (
    'GcRectangle', 'GcLine', 'GcCharacter', 'ScSLOSISLKCubicSpline', 'GcSprite', 'GcHudFont', 'GcBigSprite',
    'GcTimer', 'GcViewPort', 'GcLegoGepetto', 'GcLegoCamera', 'GcAnimSprite', 'GcHudSprite', 'GcFilledRectangle',
    'ScTextObject', 'GcLight', 'GcArea', 'ScAnimTexture', 'GcTriggerPlane', 'GcTargeteer', 'GcMoveableObj',
    'Gc3DSound', 'GcVectorProjectile', 'GcToa', 'GcCinemaEngine', 'GcCinemaEditor', 'GcEditorButton',
    'GcEditorSlider', 'GcSoundscapeEditor', 'GcConversationEngine', 'GcKeyMapper', 'GcMP3Player',
    'GcBossProjectile', 'ScPrinter', 'ScTileFont', 'ScSLOSIFileIO', 'ScSLOSIStringMapper', 'GcHive', 'GcIceBoss',
    'GcBaseBoss', 'GcWindBoss', 'GcFireBoss', 'GcMudBoss', 'GcHydraBoss', 'GcRockBoss',
)

DUP_KEYS = {"gchudsprite.create", "gchudsprite.setzdepth", "gchudsprite.setposition", "gchudsprite.enabletransparency",
            "gchudsprite.settransparency", "gchudsprite.enablealphachannel", "gchudsprite.setlinear"}


# ----------------------------------------------------------------------------------------------- real-data fixtures
@pytest.fixture(scope="module")
def res(reg, canonical_exe, lomn_data, gen_decomp):
    return reg.build(canonical_exe.read_bytes(), gen_decomp, lomn_data / "install" / "data" / "Base.osi")


@pytest.fixture(scope="module")
def rows(res):
    return res["rows"]


def _own_pe(exe: bytes):
    """Own few lines of PE parsing (section table -> VA reader), independent of seed_addrs/registry."""
    pe = struct.unpack_from("<I", exe, 0x3C)[0]
    nsec, optsz = struct.unpack_from("<H", exe, pe + 6)[0], struct.unpack_from("<H", exe, pe + 20)[0]
    base = struct.unpack_from("<I", exe, pe + 24 + 28)[0]
    secs = []
    for i in range(nsec):
        o = pe + 24 + optsz + 40 * i
        vsz, rva, rsz, roff = struct.unpack_from("<IIII", exe, o + 8)
        secs.append((exe[o:o + 8].rstrip(b"\0"), base + rva, min(vsz, rsz), roff))

    def read(va, n):
        for _, v, size, roff in secs:
            if v <= va and va + n <= v + size:
                return exe[roff + va - v: roff + va - v + n]
        raise AssertionError("%#x not file-backed" % va)
    return secs, read


# ----------------------------------------------------------------------------------------------- real-data tests
def test_row_counts_and_winners(res, rows):
    assert len(rows) == 1056
    keys = [r["key"] for r in rows]
    assert len(set(keys)) == 1049
    twice = {k for k in set(keys) if keys.count(k) == 2}
    assert twice == DUP_KEYS and all(keys.count(k) <= 2 for k in keys)
    for k in set(keys):
        assert sum(r["winner"] for r in rows if r["key"] == k) == 1, k
    assert sum(r["winner"] for r in rows) == 1049
    assert [r["phase"] for r in rows].count("scinit") == 2 and [r["phase"] for r in rows].count("crt") == 1054
    assert all(r["key"] == (r["cls"] + "." + r["name"]).lower() for r in rows)


def test_duplicate_winner_is_the_lower_crt_index(rows):
    for k in DUP_KEYS:
        a, b = [r for r in rows if r["key"] == k]
        win, lose = (a, b) if a["winner"] else (b, a)
        assert (win["init"], win["crt_index"]) == ("0x005277d0", 227), k
        assert (lose["init"], lose["crt_index"]) == ("0x00536910", 242), k
        assert win["body"] != lose["body"]


def test_every_body_is_a_function_start(rows, gen_decomp):
    starts = {int(json.loads(l)["addr"], 16) for l in (gen_decomp / "functions.jsonl").read_text().splitlines() if l}
    assert [r["body"] for r in rows if int(r["body"], 16) not in starts] == []
    assert len({r["body"] for r in rows}) == 1056


def test_min_max_and_slot_layout(rows):
    assert all(0 <= r["min"] <= r["max"] <= 10 for r in rows)
    assert sum(r["min"] != r["max"] for r in rows) == 75
    for r in rows:
        assert len(r["types"]) == 10
        # the binary stores 0x0f in every unused slot; a declared slot is never `nothing` (null iff index >= max)
        assert [t is None for t in r["types"]] == [i >= r["max"] for i in range(10)], (r["key"], r["types"])


def test_site_and_init_fields(res, rows):
    sites = [int(r["site"], 16) for r in rows]
    assert len(set(sites)) == 1056
    assert [r["crt_index"] for r in rows if r["phase"] == "crt"] == sorted(r["crt_index"] for r in rows if r["phase"] == "crt")
    crt = res["crt"]
    assert len(crt) == 391 and all(crt[r["crt_index"]] == int(r["init"], 16) for r in rows if r["phase"] == "crt")
    late = [r for r in rows if r["phase"] == "scinit"]
    assert {(r["key"], r["init"], r["crt_index"]) for r in late} == {
        ("debug.print", "0x0061d6a0", None), ("debug.assert", "0x0061d6a0", None)}


def test_fnptr_matches_reg_sites_for_every_row(res, gen_decomp):
    """The required T1-review cross-check, with its own matching code. Two independent readers: the rows' fnptr comes from
    Ghidra's decompiled text; reg_sites.json from raw exe bytes (E8 calls + the last `push imm32` into .text). A row is
    matched to its reg_sites entry by ENCLOSING FUNCTION + STATEMENT ORDER: the decomp lists a function's calls in text
    order, reg_sites sorts that function's call sites by address. Each of the 1,056 fnptrs is distinct, so a wrong pairing
    could not agree by chance. Disagreement is a finding, so nothing here tolerates a mismatch."""
    sites = json.loads((gen_decomp / "reg_sites.json").read_text())
    by_fn = {}
    for s in sorted(sites, key=lambda s: int(s["site"], 16)):
        by_fn.setdefault(s["enclosing"], []).append(s)
    parsed_by_fn = {}
    for p in res["parsed"]:
        parsed_by_fn.setdefault("0x%08x" % p["init"], []).append(p)
    assert sorted(by_fn) == sorted(parsed_by_fn) and len(by_fn) == 70
    n = 0
    for fn, ps in parsed_by_fn.items():
        assert len(ps) == len(by_fn[fn]), fn
        for p, s in zip(ps, by_fn[fn]):
            assert p["body"] == int(s["fnptr"], 16), (fn, s["site"])
            assert p["site"] == int(s["site"], 16)
            n += 1
    assert n == 1056
    assert sorted(p["body"] for p in res["parsed"]) == sorted(int(s["fnptr"], 16) for s in sites)
    assert res["cross_check"]["bad"] == []


def _asm(reg, canonical_exe, gen_decomp, parsed):
    funcs = reg.load_functions(gen_decomp)
    return reg.asm_check(canonical_exe.read_bytes(), parsed, reg.load_type_globals(gen_decomp), reg.ctor_addrs(funcs),
                         reg.Image(canonical_exe.read_bytes()))


def test_argument_pushes_in_the_asm_agree(res):
    """A third reader that uses nothing from the decomp except the rows it checks: the pushes in front of each call.
    min/max (`6a MIN 6a MAX 68 fnptr`); class and function identified BY ROLE (the two `lea; push` after the fnptr push, back
    to their string-constructor calls); each type slot is `6a ib`, `68 imm32` (the two byref slots), `call GetIt; push eax`
    (GetIt's value read from its own bytes: `mov eax,imm32; ret`, `xor eax,eax; ret` or the lazy `cmp word [g],0` guard) or
    `<load theOSIType*>; push eax`. 10,547 of 10,560 slots decode and agree; 13 are behind a register push the decoder
    does not track (unchecked, never counted as agreeing). Zero disagreements."""
    assert res["asm_check"]["bad"] == []
    assert res["asm_check"]["stats"] == {"minmax_ok": 1056, "names_ok": 1056, "types_ok": 10547, "types_unchecked": 13, "sites": 1056}


def test_byref_rows_are_checked_against_the_bytes(res, rows):
    byref = [p for p in res["parsed"] if 0x200 in p["raw_types"]]
    assert [(p["cls"], p["name"], p["site"]) for p in byref] == [("ScSLOSIFileIO", "SaveOSIObject", 0x61755B),
                                                                   ("ScSLOSIFileIO", "SaveOSIObjectToMemCard", 0x6175E2)]


@pytest.fixture(scope="module")
def mutate(reg, canonical_exe, gen_decomp, res):
    import copy

    def run(change):
        parsed = copy.deepcopy(res["parsed"])
        change(parsed)
        return _asm(reg, canonical_exe, gen_decomp, parsed)["bad"]
    return run


def test_asm_reader_catches_class_function_swap(mutate):
    def swap(parsed):
        for p in parsed:
            if (p["cls"], p["name"]) == ("GcTimer", "Create"):
                p["cls"], p["name"] = p["name"], p["cls"]
    bad = mutate(swap)
    assert bad and all("GcTimer" in m or "Create" in m for _, m in bad) and any("class" in m or "function" in m for _, m in bad)


def test_asm_reader_catches_other_mutants(mutate):
    def minmax(parsed):
        parsed[10]["min"], parsed[10]["max"] = parsed[10]["max"] + 1, parsed[10]["min"]

    def byref(parsed):                  # the imm32 push of a byref slot is now decoded
        for p in parsed:
            if p["name"] == "SaveOSIObject":
                p["raw_types"][1] = 0x16

    def getit(parsed):                  # a lazy GetIt resolved to the wrong class
        for p in parsed:
            if p["raw_types"][0] == ("game", "GcCharacter"):
                p["raw_types"][0] = ("game", "GcToa")
                return

    def literal(parsed):
        for p in parsed:
            if p["raw_types"][0] == 0x12:
                p["raw_types"][0] = 0x10
                return
    for change in (minmax, byref, getit, literal):
        assert mutate(change), change.__name__


def test_bodies_are_aligned_function_prologues_in_the_bytes(res, reg, canonical_exe):
    """Not circular with the seeding (every body was a seed): the bytes at each body are 16-byte aligned and begin with one
    of five measured prologue forms."""
    got, bad = reg.body_prologues(canonical_exe.read_bytes(), res["parsed"])
    assert bad == []
    assert got == {"push ebp; mov ebp,esp": 894, "push ebx; push esi": 124, "push ebx; mov ebx,[esp+d]": 31,
                   "mov ecx,[esp+d]": 4, "mov edx,[esp+d]": 3}


def test_winner_flags_are_what_build_reports(res, reg):
    assert [r["winner"] for r in res["rows"]] == reg.winner_flags(res["parsed"])


def test_crt_layout_matches_run_init(res, canonical_exe, gen_decomp):
    assert "PTR_FUN_0076f004" in (gen_decomp / "funcs" / "0065f530.c").read_text()      # __RunInit starts at 0x76f004
    exe = canonical_exe.read_bytes()
    secs, read = _own_pe(exe)
    crt_va = [v for n, v, _, _ in secs if n == b".CRT"][0]
    assert crt_va == 0x76F000
    d = struct.unpack("<393I", read(crt_va, 393 * 4))
    assert d[0] == 0 and d[392] == 0 and all(x != 0 for x in d[1:392])
    assert list(d[1:392]) == res["crt"]


def test_type_replay_order_and_site_accounting(res):
    assert [e["class"] for e in res["replay"].order] == list(TYPE_ORDER)
    # the same order re-derived from raw call sites only (call RegisterType + `mov [global], ax`, lazy GetIt calls)
    assert res["asm_type_order"] == list(TYPE_ORDER)
    assert len(TYPE_ORDER) == 45 and len(set(TYPE_ORDER)) == 45
    assert all(e["regname"] == e["class"] for e in res["replay"].order)
    # all 445 `call RegisterType` sites are explained: 395 guards in .CRT initializers (each parsed once), 5 lazy GetIt
    # bodies, 45 CreateVariant overloads
    assert res["type_sites"] == {"crt_initializer": 395, "getit": 5, "createvariant": 45}
    assert res["replay"].guard_events == 395
    assert {c["class"] for c in res["createvariant"]} == set(TYPE_ORDER)
    # every registered class that a row uses was registered before its first use (build would have raised otherwise)
    used = {t[5:] for r in res["rows"] for t in r["types"] if t and t.startswith("type:")}
    assert used <= set(TYPE_ORDER)


def test_types_defined_in_the_emitted_table(res, reg, rows):
    names = reg.type_table_names(res)
    used = {t for r in rows for t in r["types"] if t}
    assert used <= names
    assert used - {n for n in names if n.startswith("type:")} == {"int", "float", "colour", "string", "byref", "unknown:0x0"}
    unknown = [t for r in rows for t in r["types"] if t and t.startswith("unknown:")]
    assert unknown == ["unknown:0x0"] * 12       # 0x18, 0x21 and 0x22 never occur as type arguments
    doc = DOC.read_text()
    for n in names:
        assert "`%s`" % n in doc or n in doc, n


def test_base_osi_coverage(res, rows, lomn_data, reg):
    spec = importlib.util.spec_from_file_location("osi_research", REPO / "tools" / "research" / "osi.py")
    osi = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(osi)
    d = (lomn_data / "install" / "data" / "Base.osi").read_bytes()
    o = osi.parse(d)
    pairs = {(o["strings"][a[0]] + "." + o["strings"][a[1]]).lower()
             for _, off in osi.subroutines(o) for _, op, a in osi.disasm(d, off) if op == 0x82}
    assert len(pairs) == 728
    keys = {r["key"] for r in rows}
    missing = pairs - keys
    assert sorted(missing) == sorted(MISSING) and len(MISSING) == 78 and sorted(res["missing"]) == sorted(MISSING)
    assert {"bettersaver.save", "bettersaver.getbooleanvalue", "bettersaver.setbooleanvalue"} <= missing
    # the VM looks natives up by token pair: none of the missing names collides with a registered one
    toks = {reg.dual_token(r["cls"], r["name"]) for r in rows}
    assert not [m for m in missing if reg.dual_token(*m.split(".", 1)) in toks]
    assert len(toks) == 1049
    assert {r["key"] for r in rows if r["called_by_base_osi"]} == pairs - missing and len(pairs - missing) == 650
    assert sum(r["called_by_base_osi"] for r in rows) == 650


def test_duplicate_winner_branch_bytes(res, canonical_exe):
    """Why the first registration wins, read from the bytes (asm cited in docs/osi/types.md 'Duplicates')."""
    secs, read = _own_pe(canonical_exe.read_bytes())
    # RegisterFunction ends right after `call insert_one` (0x5fac1f): the result is ignored
    assert read(0x5FAC1F, 5) == bytes.fromhex("e86c060000") and read(0x5FAC24, 3) == bytes.fromhex("8d65f4")
    # insert_one: after the second compare (call 0x5fb370 at 0x5fb309), `test al,al ; je 0x5fb344`
    assert read(0x5FB309, 5) == bytes.fromhex("e862000000")
    assert read(0x5FB30E, 4) == bytes.fromhex("84c07432")                       # je +0x32 -> 0x5fb344
    # 0x5fb344: return the existing node (edi) with the byte at 0x74d66c; no insert_node_at call on this path
    assert read(0x5FB344, 11) == bytes.fromhex("8b450889" "38" "8a156cd67400")
    # the insert path calls insert_node_at (0x5fbcb0) and stores the byte at 0x74d668
    assert read(0x5FB31F, 5) == bytes.fromhex("e88c090000") and read(0x5FB32B, 6) == bytes.fromhex("8a1568d67400")
    assert (read(0x74D668, 1), read(0x74D66C, 1)) == (b"\x01", b"\x00")
    assert res["flags"] == {"inserted": 1, "refused": 0}


def test_types_md_is_current_and_cites_the_evidence(res, reg):
    # the committed doc carries the overrides summary line (natives.md 7.2): render it from the committed overrides file
    overrides = json.loads((REPO / "data" / "osi" / "native_overrides.json").read_text())
    text = reg.render_types(res, overrides)
    assert DOC.exists() and DOC.read_text() == text
    for fact in ("0x5fab30", "0x5fb290", "0x5fae00", "0x65f530", "0x5fa750", "0x74d66c", "**Winner:",
                 "## Duplicates", "## Not statically determinable", "0x20 upward", "static-init order"):
        assert fact in text, fact
    assert "gchudsprite.create" in text and text.count("`type:") >= 45


def test_cli_writes_rows_and_doc(reg, canonical_exe, lomn_data, gen_decomp, res, tmp_path):
    out_json, out_md = tmp_path / "osi" / "natives.json", tmp_path / "types.md"
    # with the committed overrides, as `make osi-tables` runs it (they add `tier` to the winner rows and the summary line
    # to the doc, and nothing else)
    ov = ["--overrides", str(REPO / "data" / "osi" / "native_overrides.json")]
    rc = reg.main(["--exe", str(canonical_exe), "--osi", str(lomn_data / "install" / "data" / "Base.osi"),
                   "--corpus", str(gen_decomp), "--out-json", str(out_json), "--out-types", str(out_md), *ov])
    assert rc == 0
    written = json.loads(out_json.read_text())
    for r in written:
        r.pop("tier", None)
    assert written == res["rows"]
    assert out_md.read_text() == DOC.read_text()
    assert reg.main(["--exe", str(canonical_exe), "--osi", str(lomn_data / "install" / "data" / "Base.osi"),
                     "--corpus", str(gen_decomp), "--out-json", str(out_json), "--out-types", str(out_md), *ov,
                     "--check"]) == 0


def test_non_canonical_exe_is_refused(reg, gen_decomp, tmp_path):
    with pytest.raises(SystemExit, match="not the canonical exe"):
        reg.build(b"MZ" + bytes(1000), gen_decomp, None)


# ------------------------------------------------------------------------------------- M2a-0 final fixes (F5, F6)
COMMITTED_JSON = REPO / "data" / "osi" / "natives.json"

# Base.osi 0x82 sites whose argc is outside the winner row's [min, max] (measured 2026-10-08 with the walk below):
# (key, argc, min, max, sites)
ARGC_VIOLATIONS = {
    ("gc3dsound.synctocharacter", 5, 2, 2, 1), ("gc3dsound.synctoobject", 5, 2, 2, 1),
    ("gcareadirector.hide", 1, 2, 2, 8), ("gcmodeldirector.addcharacter", 4, 5, 5, 2),
    ("gcmodeldirector.hide", 1, 2, 2, 1), ("gcmodeldirector.remove", 1, 2, 2, 1),
    ("gcmodeldirector.setinputstate", 2, 3, 3, 2), ("gcmodeldirector.show", 1, 2, 2, 1),
    ("gctimer.durationaverage", 0, 1, 1, 1), ("gctimer.durationpeek", 0, 1, 1, 2),
}


def test_argc_violations_are_computed_and_pinned(res, rows, lomn_data):
    got = {(v["key"], v["argc"], v["min"], v["max"], v["sites"]) for v in res["argc_violations"]}
    assert got == ARGC_VIOLATIONS
    assert sum(v["sites"] for v in res["argc_violations"]) == 20 and len(res["argc_violations"]) == 10
    # the same list from this test's own walk (only osi.py is shared)
    spec = importlib.util.spec_from_file_location("osi_research", REPO / "tools" / "research" / "osi.py")
    osi = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(osi)
    d = (lomn_data / "install" / "data" / "Base.osi").read_bytes()
    o = osi.parse(d)
    win = {r["key"]: r for r in rows if r["winner"]}
    own, sites = {}, 0
    for _, off in osi.subroutines(o):
        for _, op, a in osi.disasm(d, off):
            if op == 0x82:
                sites += 1
                r = win.get((o["strings"][a[0]] + "." + o["strings"][a[1]]).lower())
                if r and not r["min"] <= a[2] <= r["max"]:
                    k = (r["key"], a[2], r["min"], r["max"])
                    own[k] = own.get(k, 0) + 1
    assert sites == 4402
    assert {k + (n,) for k, n in own.items()} == ARGC_VIOLATIONS
    doc = DOC.read_text()
    assert "## Argument-count violations in `Base.osi`" in doc and "Script is calling %s.%s" in doc
    for key, argc, mn, mx, n in ARGC_VIOLATIONS:
        assert "`%s`" % key in doc


def test_builtin_0x14_row(reg):
    assert reg.BUILTIN[0x14] == "colour16"
    row = [l for l in DOC.read_text().splitlines() if l.startswith("| 0x14 |")]
    assert len(row) == 1 and "0x49" in row[0] and "0x60b9d0" in row[0] and "| 0 |" in row[0]


def test_committed_natives_json_is_fresh_and_data_free(reg, canonical_exe, lomn_data, gen_decomp, tmp_path):
    """data/osi/natives.json is committed so that M2a-2's stub generator needs no Ghidra; regenerating it must give the
    same bytes. It holds names, addresses, counts and type names only."""
    out_json, out_md = tmp_path / "natives.json", tmp_path / "types.md"
    # as `make osi-tables` runs it (M2a-2 T2b: with the committed overrides, which may name a mod key)
    assert reg.main(["--exe", str(canonical_exe), "--osi", str(lomn_data / "install" / "data" / "Base.osi"),
                     "--corpus", str(gen_decomp), "--out-json", str(out_json), "--out-types", str(out_md),
                     "--mods", str(lomn_data / "install" / "mods"), "--out-mods", str(tmp_path / "mods.json"),
                     "--overrides", str(REPO / "data" / "osi" / "native_overrides.json")]) == 0
    assert COMMITTED_JSON.exists() and out_json.read_bytes() == COMMITTED_JSON.read_bytes()
    assert (tmp_path / "mods.json").read_bytes() == (REPO / "data" / "osi" / "natives_beta150.json").read_bytes()
    rows = json.loads(COMMITTED_JSON.read_text())
    assert len(rows) == 1056
    fields = {"cls", "name", "key", "site", "body", "max", "min", "types", "crt_index", "init", "winner",
              "called_by_base_osi", "phase", "ret", "ret_class", "default", "sets_fpu_pc64"}
    # M2a-2 T2b: the overrides merge adds `tier` to the winner row of each overridden key, and nothing else
    overridden = {o["key"] for o in json.loads((REPO / "data" / "osi" / "native_overrides.json").read_text())}
    for r in rows:
        assert set(r) == (fields | {"tier"} if r["winner"] and r["key"] in overridden else fields)
        assert all(isinstance(r[k], str) for k in ("cls", "name", "key", "site", "body", "init", "phase"))
        assert all(t is None or isinstance(t, str) for t in r["types"]) and len(r["types"]) == 10
        assert isinstance(r["max"], int) and isinstance(r["min"], int)


def test_makefile_writes_the_committed_json():
    mk = (REPO / "Makefile").read_text()
    assert "--out-json data/osi/natives.json" in mk and "gen/osi/natives.json" not in mk
    assert "--out-types-json data/osi/types.json" in mk and "--out-mods data/osi/natives_beta150.json" in mk


# ---------------------------------------------------------------------------------- M2a-1 T0: ret column (D11), D8'
# Measured 2026-10-08 by a scratch script independent of registry.py (scratchpad m2a1-t0/ret_counts.py: the kind of
# each CreateVariant overload from the `->type = ...` store in its decompiled body; per winner body the overloads it calls
# in gen/decomp/calls.jsonl). Each body calls at most one overload (428 call one, 621 call none).
RET_KINDS = {"none": 621, "int": 223, "float": 129, "string": 16, "colour": 1, "game": 56, "copy": 3}
RET_KINDS_BASE = {"none": 408, "int": 127, "float": 60, "string": 12, "game": 41, "copy": 2}
COPY_KEYS = {"scslosifileio.loadosiobject", "scslosifileio.loadosiobjectfrommemcard", "scslosistringfuncs.getstringtable"}
# The 63 overloads by the type they store (docs/osi/opcodes.md "Variant types": 12 int, 2 float, 1 colour, 2 string,
# 45 game types, 1 copy).
OVERLOAD_KINDS = {"int": 12, "float": 2, "colour": 1, "string": 2, "game": 45, "copy": 1}
NEW_FIELDS = {"ret", "ret_class", "default", "sets_fpu_pc64"}


def test_createvariant_overload_kinds_agree_with_the_mangled_names(res):
    ov = res["overloads"]
    assert len(ov) == 63
    counts = {}
    for o in ov.values():
        counts[o["kind"]] = counts.get(o["kind"], 0) + 1
    assert counts == OVERLOAD_KINDS
    assert {o["cls"] for o in ov.values() if o["kind"] == "game"} == set(TYPE_ORDER)
    assert ov[0x6159E0]["kind"] == "copy" and ov[0x447890]["kind"] == "colour" and ov[0x4077F0]["kind"] == "float"


def test_ret_column_kind_counts_are_pinned(rows):
    win = [r for r in rows if r["winner"]]
    got, base = {}, {}
    for r in win:
        got[r["ret"]] = got.get(r["ret"], 0) + 1
        if r["called_by_base_osi"]:
            base[r["ret"]] = base.get(r["ret"], 0) + 1
    assert got == RET_KINDS and sum(got.values()) == 1049
    assert base == RET_KINDS_BASE and sum(base.values()) == 650
    assert {r["key"] for r in win if r["ret"] == "copy"} == COPY_KEYS
    for r in rows:
        assert (r["ret_class"] in TYPE_ORDER) if r["ret"] == "game" else r["ret_class"] is None, r["key"]
        # copy-kind natives: their real return is unknown until M2a-2, so the stub returns nothing (controller ruling)
        want = {"type": "0x0f", "value": 0} if r["ret"] == "copy" else None
        assert r["default"] == want, r["key"]


def test_ret_column_spot_checks(rows):
    win = {r["key"]: r for r in rows if r["winner"]}
    assert (win["sctilefont.create"]["ret"], win["sctilefont.create"]["ret_class"]) == ("game", "ScTileFont")
    assert win["sctilefont.setcolor"]["ret"] == "none"
    assert win["gcsaver.findmaska"]["ret"] == "int"
    assert win["gcscripttools.stringtofloat"]["ret"] == "float"


def test_sets_fpu_pc64_only_on_the_atof_native(rows):
    """D8': ___dec2num (0x65f80a) sets x87 precision control to 64 bits and never restores it; the only native whose
    body reaches it (calls.jsonl, not entering ScOSIVirtualMachine::*) is gcscripttools.stringtofloat (0x564d50 ->
    StringToFloat 0x5654b0 -> _atof 0x65c7b0 -> _strtod -> ___strtold -> ___dec2num)."""
    assert {r["key"] for r in rows if r["sets_fpu_pc64"]} == {"gcscripttools.stringtofloat"}


# ----------------------------------------------------------------------------------------------- types.json
TYPES_JSON = REPO / "data" / "osi" / "types.json"


def test_types_json_ids_follow_first_use_order(res, reg):
    t = reg.render_types_json(res)
    doc = json.loads(t)
    game = [e for e in doc["types"] if e["kind"] == "game"]
    assert [e["name"] for e in game] == ["type:" + c for c in TYPE_ORDER]
    assert [e["id"] for e in game] == list(range(0x20, 0x20 + 45))
    assert [e["rank"] for e in game] == list(range(45))
    builtin = {e["name"]: e["id"] for e in doc["types"] if e["kind"] != "game"}
    assert builtin == {"nothing": 0x0F, "int": 0x10, "string-id": 0x11, "float": 0x12, "colour16": 0x14, "colour": 0x15,
                       "string": 0x16, "byref": 0x200, "unknown:0x0": 0}
    assert TYPES_JSON.exists() and TYPES_JSON.read_text() == t


# ----------------------------------------------------------------------------------------------- mod natives (D12)
MODS_JSON = REPO / "data" / "osi" / "natives_beta150.json"
# Decoded 2026-10-08 from the DLL bytes (scratchpad m2a1-t0/mods_decode.py, then registry.mod_registrations):
# (dll, site, cls, name, body, max, min, declared types, ret)
MOD_ROWS = [
    ("BetterSaver.dll", "0x1000264f", "gcsaver", "clearcinema", "0x10001e40", 0, 0, [], "none"),
    ("BetterSaver.dll", "0x10002678", "bettersaver", "save", "0x10001f40", 0, 0, [], "none"),
    ("BetterSaver.dll", "0x100026a4", "bettersaver", "getbooleanvalue", "0x10001f70", 1, 1, ["int"], "int"),
    ("BetterSaver.dll", "0x100026cd", "bettersaver", "setbooleanvalue", "0x10001ff0", 2, 2, ["int", "int"], "none"),
    ("BetterSaver.dll", "0x100026f9", "bettersaver", "removebooleanvalue", "0x10002030", 1, 1, ["int"], "int"),
    ("BetterSaver.dll", "0x10002722", "bettersaver", "togglebooleanvalue", "0x100020a0", 1, 1, ["int"], "int"),
    ("BetterSaver.dll", "0x1000274e", "bettersaver", "getintegervalue", "0x10002130", 1, 1, ["int"], "int"),
    ("BetterSaver.dll", "0x10002777", "bettersaver", "setintegervalue", "0x100021b0", 2, 2, ["int", "int"], "none"),
    ("BetterSaver.dll", "0x100027a3", "bettersaver", "decrementintegervalue", "0x100021f0", 1, 1, ["int"], "int"),
    ("BetterSaver.dll", "0x100027cc", "bettersaver", "incrementintegervalue", "0x10002270", 1, 1, ["int"], "int"),
    ("BetterSaver.dll", "0x100027f8", "bettersaver", "removeintegervalue", "0x100022f0", 1, 1, ["int"], "int"),
    ("BetterSaver.dll", "0x10002821", "bettersaver", "getfloatvalue", "0x10002360", 1, 1, ["int"], "float"),
    ("BetterSaver.dll", "0x1000284d", "bettersaver", "setfloatvalue", "0x10002400", 2, 2, ["int", "float"], "none"),
    ("BetterSaver.dll", "0x10002876", "bettersaver", "removefloatvalue", "0x10002440", 1, 1, ["int"], "int"),
    ("OSIProject.DebugServer.dll", "0x10005933", "lsdebugger", "setwireframe", "0x10005690", 1, 1, ["int"], "none"),
    ("OSIProject.DebugServer.dll", "0x1000596d", "lsdebugger", "setcollisionboxes", "0x100056c0", 1, 1, ["int"], "none"),
    ("OSIProject.DebugServer.dll", "0x100059a7", "lsdebugger", "settriggerplanes", "0x100056f0", 1, 1, ["int"], "none"),
    ("OSIProject.DebugServer.dll", "0x100059e1", "lsdebugger", "settriggerboxes", "0x10005720", 1, 1, ["int"], "none"),
    ("OSIProject.DebugServer.dll", "0x10005a1b", "lsdebugger", "printstack", "0x10005750", 0, 0, [], "none"),
]
# The 7 mod keys Base.osi calls (D2 amendment), with their 0x82 site counts (osi.py walk; `BetterSaver.Save` x7 and
# `bettersaver.save` x1 fold to one key).
MOD_CALLED = {"bettersaver.save": 8, "bettersaver.getbooleanvalue": 6, "bettersaver.setbooleanvalue": 6,
              "lsdebugger.setwireframe": 2, "lsdebugger.setcollisionboxes": 2, "lsdebugger.settriggerplanes": 2,
              "lsdebugger.settriggerboxes": 2}


@pytest.fixture(scope="module")
def mods(reg, lomn_data, rows):
    return reg.mod_registrations(lomn_data / "install" / "mods", lomn_data / "install" / "data" / "Base.osi", rows)


def test_mod_rows_decoded_from_the_dll_bytes(mods):
    got = [(m["dll"], m["site"], m["cls"], m["name"], m["body"], m["max"], m["min"],
            [t for t in m["types"] if t is not None], m["ret"]) for m in mods]
    assert got == MOD_ROWS
    for m in mods:
        assert len(m["types"]) == 10 and m["types"][m["max"]:] == [None] * (10 - m["max"])
        assert m["key"] == (m["cls"] + "." + m["name"]).lower() and m["tier"] == "mod"
        assert m["collides_with_exe"] is False and m["winner"] is True
        assert m["sets_fpu_pc64"] is False and m["default"] is None and m["ret_class"] is None


def test_mod_rows_called_by_base_osi_and_site_literals(mods):
    called = {m["key"]: m for m in mods if m["called_by_base_osi"]}
    assert {k: m["base_osi_sites"] for k, m in called.items()} == MOD_CALLED
    for m in called.values():
        assert m["site_literals"] == "consistent", m["key"]       # never AMBIGUOUS: every literal matches its slot type
    assert all(m["base_osi_sites"] == 0 and m["site_literals"] is None for m in mods if not m["called_by_base_osi"])


def test_shim_forwards_max_then_min_to_register_function(lomn_data):
    """LOMNHook::Util::OSIRegisterFunction (ijl15.dll 0x100035a0) pushes its own [ebp+0x40]..[ebp+0x14] (t9..t0, then
    arg 5, arg 4) and calls 0x5fab30 (`mov eax, 0x5fab30; call eax`), so its 4th parameter lands in RegisterFunction's
    MAX slot and its 5th in MIN: the shim order is (fn, cls, name, MAX, MIN, t0..t9). Own PE reading."""
    dll = (lomn_data / "install" / "ijl15.dll").read_bytes()
    secs, read = _own_pe(dll)
    pushes = read(0x100035F7, 3) + read(0x10003600, 3) + read(0x10003607, 3)
    assert pushes == bytes.fromhex("ff7540ff753cff7538")
    body = read(0x10003610, 0x30)
    want = b"".join(bytes([0xFF, 0x75, d]) for d in range(0x34, 0x13, -4))
    assert want in body
    assert read(0x10003632, 7) == bytes.fromhex("b830ab5f00ffd0")


def test_committed_mods_json_is_fresh(reg, mods):
    assert MODS_JSON.exists() and MODS_JSON.read_text() == reg.render_rows(mods)
