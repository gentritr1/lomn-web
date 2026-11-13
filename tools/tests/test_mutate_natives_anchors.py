"""M2a-2 T10 anchor guard for tools/research/oracle/mutate_natives.py (the mutate_vm anchor-test pattern): a refactor of a
native must not silently retire a mutant. Checked against the tree this file lives in: every anchor (`old`) occurs exactly
once in its target file, `old` != `new` (except the comment-only control), every mutant names the `a9own` check with a
compiled EXPECT regex on the own-models cmps (CMP3 / CMP4; the control gives None: it must bite nowhere), the six
mutants of the plan (Review focus 3) are there, ids and names are unique, and each named native is a registered tier-1
row with a C++ body bound in the native group its file registers (so the mutated code is the code the A9 run calls).
Whether a mutant changes the native trace on the A9 path is checked at run time by mutate_natives.py's NO-EFFECT verdict,
not by this anchors test (which only proves the anchors still match)."""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "research" / "oracle"))
import mutate_natives  # noqa: E402


def test_every_anchor_matches_exactly_once() -> None:
    bad = []
    for mid, name, key, rel, old, new, checks in mutate_natives.M:
        f = REPO / rel
        n = f.read_text().count(old) if f.is_file() else -1
        if n != 1:
            bad.append(f"[{mid}] anchor matches {n} times in {rel} ({name})")
    assert not bad, "mutate_natives would abort (rc 2) at the first of these:\n" + "\n".join(bad)


def test_every_mutant_changes_code_and_names_the_own_models_cmp() -> None:
    controls = 0
    for mid, name, key, rel, old, new, checks in mutate_natives.M:
        assert set(checks) == {"a9own"}, f"[{mid}] unknown check in {sorted(checks)}"
        if mutate_natives.is_control(mid):
            controls += 1
            assert checks["a9own"] is None
            assert old.lstrip().startswith("//") and new.lstrip().startswith("//") and "\n" not in old + new
            continue
        assert old != new, f"[{mid}] {name}: old == new"
        r = checks["a9own"]
        assert r and r.startswith("^CMP"), f"[{mid}] the EXPECT must name the own-models cmp (CMP3 / CMP4): {r!r}"
        re.compile(r)
    assert controls == 1


def test_the_plan_six_and_unique_ids() -> None:
    ids = [e[0] for e in mutate_natives.M]
    names = [e[1] for e in mutate_natives.M]
    assert len(ids) == len(set(ids)) and len(names) == len(set(names))
    assert [i for i in ids if not mutate_natives.is_control(i)] == ["M1", "M2", "M3", "M4", "M5", "M6"]
    keys = {e[2] for e in mutate_natives.M}
    for k in ("scslosifoundation.stringtoidentifier", "gcsaver.cheatmenu", "scslosifileio.doesfileexist",
              "gcareadirector.getcurrentlevel", "gclegocamera.getcameraaimode"):
        assert k in keys, k


def test_each_named_native_is_a_bound_tier1_row() -> None:
    rows = {r.get("key") or (r["cls"] + "." + r["name"]).lower(): r for r in json.loads((REPO / "data/osi/natives.json").read_text())
            if r.get("winner", True)}
    ov = json.loads((REPO / "data/osi/native_overrides.json").read_text())
    tiers = {r["key"]: r["tier"] for r in (ov if isinstance(ov, list) else [dict(v, key=k) for k, v in ov.items()])}
    for mid, name, key, rel, old, new, checks in mutate_natives.M:
        if mutate_natives.is_control(mid) or key.startswith("("):
            continue
        assert key in rows, f"[{mid}] {key} is not a registered native"
        assert str(tiers.get(key, rows[key].get("tier"))) in ("tier1", "1"), f"[{mid}] {key} is not tier 1"
        cls, fn = key.split(".")
        assert re.search(r'\{"%s", "%s", n_\w+\}' % (re.escape(cls), re.escape(fn)), (REPO / rel).read_text()), \
            f"[{mid}] {key} is not bound in {rel}"


def test_first_diff_reports_a_difference_on_line_1(tmp_path) -> None:
    """A mutant can change the very first D line (Setup's): the report names it instead of raising (T10 fix)."""
    a, b = tmp_path / "a", tmp_path / "b"
    a.write_bytes(b"D     -1 1 x\nD 0\n")
    b.write_bytes(b"D     -1 1 y\nD 0\n")
    assert mutate_natives.mutate_vm.first_diff(a, b, "CMP3 D") == ["CMP3 D line 1 after '': cpp='D     -1 1 x' py='D     -1 1 y'"]
    assert mutate_natives.mutate_vm.first_diff(a, a, "CMP3 D") == []
