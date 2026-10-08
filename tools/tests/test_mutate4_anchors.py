"""M1 close-out I-2/anchor guard: mutate4.py aborts with rc 2 ("pattern not found exactly once") the moment a mutation's anchor stops
matching, and entry #27 went stale at 4b4ad7b (T8 P5 moved `wgpuTextureDestroy(t);` into gpu_destroy_and_release) without any test noticing
until a full rerun was attempted. This loads mutate4's table and asserts, against the tree it lives in, that every anchor (`old`) occurs
exactly once in its target file, that `old` and `new` differ, that every EXPECT regex compiles and names a real entry that has a browser
check, and that every check name is one mutate4 can run."""
from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "research" / "oracle"))
import mutate4  # noqa: E402

KNOWN_CHECKS = {"unit", "gpu", "node", "browser"}


def _entries():
    return [(i, *e) for i, e in enumerate(mutate4.M, 1)]


def test_every_anchor_matches_exactly_once() -> None:
    bad = []
    for i, name, rel, old, new, checks in _entries():
        f = REPO / rel
        if not f.is_file():
            bad.append(f"[{i}] {rel} does not exist ({name})")
            continue
        n = f.read_text().count(old)
        if n != 1:
            bad.append(f"[{i}] anchor matches {n} times in {rel} ({name})")
    assert not bad, "mutate4 would abort (rc 2) at the first of these:\n" + "\n".join(bad)


def test_every_mutation_changes_something_and_names_known_checks() -> None:
    for i, name, rel, old, new, checks in _entries():
        if "no-op control" in name:
            continue  # a comment-only change: its old and new differ in a comment, and it must be MISSED
        assert old != new, f"[{i}] {name}: old == new"
        assert checks and set(checks) <= KNOWN_CHECKS, f"[{i}] {name}: unknown check in {checks}"


def test_every_expect_regex_compiles_and_belongs_to_a_browser_entry() -> None:
    names = {e[0]: e[4] for e in mutate4.M}
    for name, pattern in mutate4.EXPECT.items():
        re.compile(pattern)
        assert name in names, f"EXPECT names no mutation: {name!r}"
        assert "browser" in names[name], f"EXPECT is only consulted for browser checks: {name!r} has {names[name]}"


def test_entry_names_are_unique() -> None:
    names = [e[0] for e in mutate4.M]
    assert len(names) == len(set(names))
