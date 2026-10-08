"""M2a-1 T5 anchor guard for tools/research/oracle/mutate_vm.py (the mutate4 anchor-test pattern): a refactor of the VM must
not silently retire a mutant. mutate_vm.py aborts (rc 2) when an anchor stops matching; these tests say so at `pytest`
time instead of at the next mutation run. Checked against the tree this file lives in: every anchor (`old`) occurs exactly
once in its target file, `old` != `new` (except the comment-only control), every mutant names at least one check mutate_vm
can run and gives each a compiled EXPECT regex (the control gives None: it must bite nowhere), ids and names are unique,
and the documented acceptance size holds (33 consult rows + settled-semantics mutants >= 33 meaningful, one control)."""
from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "research" / "oracle"))
import mutate_vm  # noqa: E402


def _entries():
    return list(mutate_vm.M)


def test_every_anchor_matches_exactly_once() -> None:
    bad = []
    for mid, name, rel, old, new, checks in _entries():
        f = REPO / rel
        if not f.is_file():
            bad.append(f"[{mid}] {rel} does not exist ({name})")
            continue
        n = f.read_text().count(old)
        if n != 1:
            bad.append(f"[{mid}] anchor matches {n} times in {rel} ({name})")
    assert not bad, "mutate_vm would abort (rc 2) at the first of these:\n" + "\n".join(bad)


def test_every_mutant_changes_code_and_names_known_checks_with_a_regex() -> None:
    controls = 0
    for mid, name, rel, old, new, checks in _entries():
        assert checks and set(checks) <= set(mutate_vm.CHECKS), f"[{mid}] unknown check in {sorted(checks)}"
        if mutate_vm.is_control(mid):
            controls += 1
            assert all(r is None for r in checks.values()), f"[{mid}] the control must name no EXPECT regex"
            continue
        assert old != new, f"[{mid}] {name}: old == new"
        for c, r in checks.items():
            assert r, f"[{mid}] check {c} has no EXPECT regex (an unrelated failure must not count as a bite)"
            re.compile(r)
    assert controls == 1


def test_ids_and_names_are_unique_and_the_table_is_complete() -> None:
    ids = [e[0] for e in _entries()]
    names = [e[1] for e in _entries()]
    assert len(ids) == len(set(ids))
    assert len(names) == len(set(names))
    consult = [i for i in ids if re.fullmatch(r"C\d\d", i)]
    settled = [i for i in ids if re.fullmatch(r"S\d\d", i)]
    assert len(consult) == 33, consult  # consult §5 rows 1-33 (row 30 re-pointed, see its entry)
    assert len(consult) + len(settled) >= 33 and settled


def test_the_control_is_a_comment_only_change() -> None:
    (ctl,) = [e for e in _entries() if mutate_vm.is_control(e[0])]
    _, _, rel, old, new, _ = ctl
    assert rel.endswith(".cpp")
    assert old.lstrip().startswith("//") and new.lstrip().startswith("//") and "\n" not in old + new
