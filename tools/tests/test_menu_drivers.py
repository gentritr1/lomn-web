"""M2a-3 T10: the menu-run (FrameHost) paths of flip_probe.py and osi_taint.py run the same instructions as the M1 run of
`make osi-menu` (lomn_view --out: setup 49,539, total 1,135,590 over 905 frames; native == Python own models by cmp 3/4).
The mutant anchors of tools/research/oracle/mutate_menu.py each match exactly once."""
import importlib
import re
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(REPO / "tools" / "osi"), str(REPO / "tools" / "research"), str(REPO / "tools" / "research" / "oracle")]
DATA = REPO / "data" / "osi"
M1 = dict(frames=905, keys="down@700 down@760", mouse="450,330@0 450,370@755")


@pytest.fixture(scope="module")
def install(lomn_data):
    p = lomn_data / "install"
    if not (p / "data" / "Base.osi").exists():
        pytest.skip("SKIPPED (no Base.osi in LOMN_DATA)")
    return p


def test_flip_probe_menu_baseline_matches_the_m1_run(install):
    flip = importlib.import_module("flip_probe")
    r = flip.run_once((install / "data" / "Base.osi").read_bytes(), DATA / "natives.json", DATA / "natives_beta150.json",
                      DATA / "types.json", deadline=120, natives_impl="real", install=install, **M1)
    assert (r.stop, r.setup_instructions, r.instructions, len(r.natives)) == ("none", 49539, 1135590, 75)


def test_taint_menu_run_matches_the_m1_run(install):
    taint = importlib.import_module("osi_taint")
    rep = taint.run_base((install / "data" / "Base.osi").read_bytes(), M1["frames"], M1["keys"], None,
                         real_natives=True, install=install, mouse=M1["mouse"])
    assert (rep.stop, rep.frames, rep.instructions) == ("none", 905, 1135590)
    assert not rep.unsafe()


def test_menu_paths_need_real_natives():
    flip = importlib.import_module("flip_probe")
    with pytest.raises(ValueError, match="need --natives-impl real"):
        flip.run_once(b"", DATA / "natives.json", DATA / "natives_beta150.json", DATA / "types.json", 1, 1.0, mouse="1,1@0")


def test_mutate_menu_anchors_match_once_and_name_checks():
    mm = importlib.import_module("mutate_menu")
    ids = [m[0] for m in mm.M]
    assert len(ids) == len(set(ids)) and "CTRL" in ids
    for mid, name, rel, old, new, checks in mm.M:
        assert (REPO / rel).read_text().count(old) == 1, f"[{mid}] anchor in {rel}"
        assert old != new and set(checks) <= {"menu", "ctest", "pytest"}, mid
        for c, (arg, want) in checks.items():
            assert (want is None) == mm.is_control(mid), mid
            if want:
                re.compile(want)
    ctrl = next(m for m in mm.M if m[0] == "CTRL")
    assert ctrl[3].startswith("//") and ctrl[4].startswith("//")
