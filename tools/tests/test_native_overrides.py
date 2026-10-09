"""M2a-2 T2b: data/osi/native_overrides.json and its merge into the registry tables (docs/osi/natives.md §7.2;
plan Decision 3, minor m6).

What is pinned:
- the schema of the committed overrides file (a sorted list of {key, tier, cite, note[, default]} rows);
- the merge: `registry.apply_overrides` sets `tier` (and replaces `default`) on the winner row of each named key, in
  natives.json for an exe key and in natives_beta150.json for a mod key, and leaves every other row byte-identical;
- the empty file is a byte-level no-op on both committed tables (m6);
- `make osi-tables` passes the overrides file to registry.py and regenerates the error-PC tables of opcodes.md;
- docs/osi/types.md carries the per-tier override counts.

These tests read only committed files (data/osi/*.json, the Makefile, docs/osi/types.md), so they need no game data.
`make osi-tables` itself (the real regeneration) is run by the controller and was run once in Task 2b (commit body)."""
import copy
import importlib.util
import json
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
DATA = REPO / "data" / "osi"
OVERRIDES = DATA / "native_overrides.json"


@pytest.fixture(scope="module")
def reg():
    spec = importlib.util.spec_from_file_location("osi_registry_ovr", REPO / "tools" / "osi" / "registry.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture(scope="module")
def rows():
    # the PRE-merge table: the committed one carries the overrides tiers, so strip them (generated winners have no `tier`)
    out = json.loads((DATA / "natives.json").read_text())
    for r in out:
        r.pop("tier", None)
    return out


@pytest.fixture(scope="module")
def mods():
    out = json.loads((DATA / "natives_beta150.json").read_text())
    for m in out:
        m["tier"] = "mod"                                   # the generated mod tier
    return out


def _ok_row(**kw):
    # gcsaver.cheatmenu: body 0x5d35b0 (natives.md §9.1), a decided default of int 0 (Decision 9)
    row = {"key": "gcsaver.cheatmenu", "tier": "tier2", "default": {"type": "0x10", "value": 0},
           "cite": "body 0x5d35b0 -> GcSaver::CheatMenu 0x5d4fc0 (natives.md §9.1)", "note": "ini -> M2a-3"}
    row.update(kw)
    return row


def test_committed_overrides_file_is_a_valid_list(reg, rows, mods):
    data = json.loads(OVERRIDES.read_text())
    assert isinstance(data, list)
    assert reg.check_overrides(data, rows, mods) == []


def test_a_valid_row_passes(reg, rows, mods):
    assert reg.check_overrides([_ok_row()], rows, mods) == []
    tier1 = _ok_row(tier="tier1", cite="// body 0x5d35b0, method 0x5d4fc0 <GcSaver::CheatMenu>; asm pin: 0x5d502c cmp")
    del tier1["default"]
    assert reg.check_overrides([tier1], rows, mods) == []
    mod = {"key": "bettersaver.getbooleanvalue", "tier": "tier1", "note": "empty map -> 0",
           "cite": "// body BetterSaver.dll 0x10001f70, method 0x10005ab0 <std::map find>; asm pin: 0x10001fd1 setne"}
    assert reg.check_overrides([mod], rows, mods) == []


@pytest.mark.parametrize("bad, problem", [
    (_ok_row(key="gcsaver.nosuchnative"), "unknown key"),
    (_ok_row(tier="stub"), "tier"),
    (_ok_row(tier="1"), "tier"),
    (_ok_row(extra=1), "field"),
    (_ok_row(tier="tier1"), "default"),                       # a tier-1 body decides its result
    (_ok_row(default={"type": "0x10"}), "default"),
    (_ok_row(default={"type": "0x12", "bits": "3F800000"}), "default"),
    (_ok_row(default={"type": "0x10", "value": -1}), "default"),
    (_ok_row(default={"text": ""}), "default"),
    # fix 1 (review m-3): floats are bits only; only scalar types can be a stub default
    (_ok_row(default={"type": "0x12", "value": 0}), "default"),
    (_ok_row(default={"type": "0x27", "value": 1}), "default"),        # a game type
    (_ok_row(default={"type": "0x16", "value": 1}), "default"),        # a dynamic string handle
    (_ok_row(default={"type": "0x01", "value": 1}), "default"),        # a script object
    (_ok_row(default={"type": "0x0f", "value": 7}), "default"),        # nothing carries 0
    (dict({k: v for k, v in _ok_row().items() if k != "default"}, tier="tier1",
          cite="body 0x5d35b0 -> GcSaver::CheatMenu 0x5d4fc0"), "cite"),   # tier1: the §1.3 shape
    (dict({k: v for k, v in _ok_row().items() if k != "default"}, tier="tier1",
          cite="// body 0x5d35b0, method 0x5d4fc0 <GcSaver::CheatMenu>"), "cite"),   # no asm pin
    (_ok_row(cite="GcSaver::CheatMenu 0x5d4fc0"), "cite"),    # does not name the row's body
    (_ok_row(cite=""), "cite"),
    (_ok_row(note=""), "note"),
    ({k: v for k, v in _ok_row().items() if k != "cite"}, "field"),
    ({k: v for k, v in _ok_row().items() if k != "note"}, "field"),
])
def test_schema_rejects(reg, rows, mods, bad, problem):
    problems = reg.check_overrides([bad], rows, mods)
    assert problems and any(problem in p for p in problems), problems


def test_duplicate_and_unsorted_keys_are_rejected(reg, rows, mods):
    a = _ok_row()
    b = _ok_row(key="gcsaver.getmask", cite="body 0x5d36d0", default={"type": "0x10", "value": 0x3F3F3F3F})
    assert reg.check_overrides([a, b], rows, mods) == []           # "gcsaver.cheatmenu" < "gcsaver.getmask"
    ok = [_ok_row(default={"type": d, "value": 0}) for d in ("0x0f", "0x10", "0x14", "0x15")]
    ok.append(_ok_row(default={"type": "0x12", "bits": "3f800000"}))
    assert all(reg.check_overrides([o], rows, mods) == [] for o in ok)
    assert any("sorted" in p for p in reg.check_overrides([b, a], rows, mods))
    assert any("duplicate" in p for p in reg.check_overrides([a, a], rows, mods))
    assert reg.check_overrides({"gcsaver.cheatmenu": a}, rows, mods) == ["the overrides file must be a JSON list"]


def test_empty_overrides_change_no_byte(reg, rows, mods):
    """Minor m6: the empty file is a byte-level no-op on both committed tables."""
    r2, m2 = reg.apply_overrides(copy.deepcopy(rows), copy.deepcopy(mods), [])
    assert r2 == rows and m2 == mods
    assert reg.render_rows(r2) == reg.render_rows(rows) and reg.render_rows(m2) == reg.render_rows(mods)
    # and the committed tables are the pre-merge ones plus the committed overrides (M2a-2 T8b)
    merged_r, merged_m = reg.apply_overrides(copy.deepcopy(rows), copy.deepcopy(mods), json.loads(OVERRIDES.read_text()))
    assert reg.render_rows(merged_r).encode() == (DATA / "natives.json").read_bytes()
    assert reg.render_rows(merged_m).encode() == (DATA / "natives_beta150.json").read_bytes()


def test_merge_touches_only_the_named_winner_row(reg, rows, mods):
    over = [_ok_row()]
    r2, m2 = reg.apply_overrides(copy.deepcopy(rows), copy.deepcopy(mods), over)
    changed = [i for i, (a, b) in enumerate(zip(rows, r2)) if a != b]
    assert len(changed) == 1
    row = r2[changed[0]]
    assert row["key"] == "gcsaver.cheatmenu" and row["winner"]
    assert row["tier"] == "tier2" and row["default"] == {"type": "0x10", "value": 0}
    assert set(row) - set(rows[changed[0]]) == {"tier"}              # cite and note stay in the overrides file
    assert m2 == mods
    before, after = reg.render_rows(rows).splitlines(), reg.render_rows(r2).splitlines()
    assert [i for i, (a, b) in enumerate(zip(before, after)) if a != b] == [changed[0] + 1]   # +1: the "[" line


def test_merge_without_default_keeps_the_generated_default(reg, rows, mods):
    over = [{"key": "scslosifileio.loadosiobject", "tier": "tier3", "note": "copy kind: D11 stub is wrong (§8.1)",
             "cite": "body 0x615990 -> ScSLOSIFileIO::LoadOSIObject 0x615f10"}]
    r2, _ = reg.apply_overrides(copy.deepcopy(rows), copy.deepcopy(mods), over)
    row = next(r for r in r2 if r["key"] == "scslosifileio.loadosiobject" and r["winner"])
    assert row["tier"] == "tier3" and row["default"] == {"type": "0x0f", "value": 0}


def test_mod_key_merges_into_the_mod_table(reg, rows, mods):
    over = [{"key": "bettersaver.getbooleanvalue", "tier": "tier1", "note": "empty map -> int 0",
             "cite": "// body BetterSaver.dll 0x10001f70, method 0x10005ab0 <std::map find>; asm pin: 0x10001fd1 setne"}]
    r2, m2 = reg.apply_overrides(copy.deepcopy(rows), copy.deepcopy(mods), over)
    assert r2 == rows
    row = next(m for m in m2 if m["key"] == "bettersaver.getbooleanvalue")
    assert row["tier"] == "tier1"
    assert sum(a != b for a, b in zip(mods, m2)) == 1


def test_apply_refuses_a_bad_file(reg, rows, mods):
    with pytest.raises(reg.RegistryError, match="unknown key"):
        reg.apply_overrides(copy.deepcopy(rows), copy.deepcopy(mods), [_ok_row(key="gcsaver.nosuchnative")])


def test_makefile_merges_overrides_and_regenerates_error_pc():
    mk = (REPO / "Makefile").read_text()
    block = mk.split("\nosi-tables:", 1)[1].split("\n\n", 1)[0]
    assert "--overrides data/osi/native_overrides.json" in block
    assert "tools/osi/error_pc.py" in block and "--write docs/osi/opcodes.md" in block
    # error_pc.py runs after opcodes.py, which rewrites the same file (it keeps text outside its table verbatim)
    assert block.index("tools/osi/opcodes.py") < block.index("tools/osi/error_pc.py")


def test_types_md_reports_the_override_tiers(reg):
    data = json.loads(OVERRIDES.read_text())
    line = reg.tier_summary(data)
    assert line in (REPO / "docs" / "osi" / "types.md").read_text()
    assert reg.tier_summary([_ok_row(), _ok_row(key="x", tier="tier1")]).startswith(
        "Registry overrides (`data/osi/native_overrides.json`, docs/osi/natives.md §7): 2 rows: tier1 1, tier2 1, tier3 0.")
