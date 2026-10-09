"""M2a-2 T2a: tools/osi/event_probe.py, the host-OnEvent injection probe (Decision 8 ruling, round 3).

The Toa's process path reaches 42 `OnEvent` call sites on the front-end (task-2a report). The probe injects each
site's event tuple, as the asm builds it, into the oracle's headless run and asks whether the script's path changes.
The synthetic tests hand-derive their numbers from the tiny scripts below; the pinned site table is read from the exe
with objdump (addresses and immediates cited in event_probe.SITES)."""
import importlib.util
import json
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
DATA = REPO / "data" / "osi"


def _load(name):
    spec = importlib.util.spec_from_file_location("osi_" + name, REPO / "tools" / "osi" / (name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = mod
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture(scope="module")
def ep():
    return _load("event_probe")


# OnEvent(0x36, ...) sets a global that Process reads (2 extra instructions per later frame); OnEvent(0x0f, ...) runs
# DebugOn, which always errors; every other id returns at once.
ASM = """
.globals flag
.function Setup 0
    PushConstant0
    SetVariableValue global:flag
    PushConstant0
    Return
.function Process 3
    GetVariableValue global:flag
    CompareAndBranchIfFalse @no
    PushConstanti8 7
    Pop
no:
    PushConstanti8 1
    Return
.function OnEvent 10
    GetVariableValue 0
    PushConstanti8 0x36
    EqualTo
    CompareAndBranchIfFalse @other
    PushConstanti8 1
    SetVariableValue global:flag
other:
    GetVariableValue 0
    PushConstanti8 0x0f
    EqualTo
    CompareAndBranchIfFalse @fine
    DebugOn
fine:
    PushConstant0
    Return
"""


@pytest.fixture(scope="module")
def image():
    sys.path[:0] = [str(REPO / "tools" / "osi")]
    import osiasm
    img, _ = osiasm.assemble(ASM)
    return img


def _rows(tmp_path):
    n = tmp_path / "n.json"
    n.write_text(json.dumps([]))
    return n


def test_synthetic_verdicts(ep, image, tmp_path):
    """Hand-derived. Baseline: Setup 4 instructions; each Process frame runs GetVariableValue, the branch (taken),
    PushConstanti8 1, Return = 4. After OnEvent(0x36) the branch falls through and runs PushConstanti8 7 + Pop: +2 per
    later frame. 10 frames, injection at frame 3 (before that frame's Process) -> frames 3..9 = 7 frames -> +14, first
    divergence at frame 3. OnEvent(0x0f) errors inside the injected call: ERROR. OnEvent(0x28) changes nothing: SAME."""
    res = ep.probe(image, _rows(tmp_path), None, DATA / "types.json", frames=10, inject_frames=[3],
                   tuples=[(0x36, (1,)), (0x0f, ()), (0x28, (("f", 10000.0),))], keys="")
    got = [(r.verdict, r.event, r.frame, r.delta) for r in res]
    assert got == [("CHANGED", 0x36, 3, 14), ("ERROR", 0x0f, 3, 0), ("SAME", 0x28, 3, 0)]
    assert res[0].divergence == "frame 3"
    assert res[2].divergence == "-"
    assert "DebugOn" in res[1].divergence or "error" in res[1].divergence
    line = ep.format_line(res[0])
    assert line.startswith("CHANGED 0x36 (1) 3 +14 frame 3")


def test_injected_instructions_are_not_counted(ep, image, tmp_path):
    """The injected call's own instructions (OnEvent's 7 for id 0x28) are reported separately, not as a path change."""
    res = ep.probe(image, _rows(tmp_path), None, DATA / "types.json", frames=4, inject_frames=[0, 3],
                   tuples=[(0x28, ())], keys="")
    assert [(r.verdict, r.frame, r.delta) for r in res] == [("SAME", 0, 0), ("SAME", 3, 0)]
    assert all(r.injected > 0 for r in res)


def test_site_table_matches_the_asm_scan(ep):
    """42 sites (task-2a report, round 2): 3 in GcCharacter::Process, CheckForNewAnim, ProcessAnimationFlags,
    GcToa::Process and 36 in GcMotionSystem. Event ids are the immediate passed to CreateVariant 0x413ed0 before each
    call (EXECUTED objdump scan); 'hell' / 'crsh' / 'impc' / 'watr' are the identifier immediates cited per site."""
    assert len(ep.SITES) == 42
    assert len({s.call for s in ep.SITES}) == 42
    by = {s.call: s for s in ep.SITES}
    assert by[0x56959F].event == 0x27 and by[0x5094FD].event == 0x0B
    assert by[0x589F17].event == 0x28 and by[0x589F17].args == (("f", 10000.0), ("i", 0x68656C6C))
    assert by[0x57E75B].event == 0x0F and by[0x57E75B].args == (("i", 0x77617472),)
    assert by[0x585058].event == 0x10 and by[0x581C04].event == 0x0F and by[0x584AD3].event == 0x0F
    assert sorted({s.event for s in ep.SITES}) == [0x0B, 0x0C, 0x0D, 0x0F, 0x10, 0x11, 0x1F, 0x22, 0x23, 0x27, 0x28,
                                                    0x2A, 0x2D, 0x2E, 0x2F, 0x36]


# OnEvent(0x2a) counts; Process runs 2 extra instructions per frame while the count is exactly 2. One injection never
# reaches the threshold; two do (fix round 1, m-5: an event the Toa fires repeatedly).
ASM_COUNT = """
.globals count
.function Setup 0
    PushConstant0
    SetVariableValue global:count
    PushConstant0
    Return
.function Process 3
    GetVariableValue global:count
    PushConstanti8 2
    EqualTo
    CompareAndBranchIfFalse @no
    PushConstanti8 7
    Pop
no:
    PushConstanti8 1
    Return
.function OnEvent 10
    GetVariableValue 0
    PushConstanti8 0x2a
    EqualTo
    CompareAndBranchIfFalse @fine
    GetVariableValue global:count
    PushConstanti8 1
    Add
    SetVariableValue global:count
fine:
    PushConstant0
    Return
"""


def test_repeated_injection_reaches_a_threshold(ep, tmp_path):
    """Hand-derived. Single shots at frame 3 or 5 leave the count at 1: SAME. With repeat=True one run injects at
    frames 3 AND 5: the count is 2 from frame 5, so frames 5..9 run +2 each = +10, first divergence frame 5."""
    sys.path[:0] = [str(REPO / "tools" / "osi")]
    import osiasm
    img, _ = osiasm.assemble(ASM_COUNT)
    once = ep.probe(img, _rows(tmp_path), None, DATA / "types.json", frames=10, inject_frames=[3, 5],
                    tuples=[(0x2A, ())], keys="")
    assert [(r.verdict, r.frame, r.delta) for r in once] == [("SAME", 3, 0), ("SAME", 5, 0)]
    rep = ep.probe(img, _rows(tmp_path), None, DATA / "types.json", frames=10, inject_frames=[3, 5],
                   tuples=[(0x2A, ())], keys="", repeat=True)
    assert [(r.verdict, r.frame, r.delta, r.divergence) for r in rep] == [("CHANGED", (3, 5), 10, "frame 5")]
    assert ep.format_line(rep[0]).startswith("CHANGED 0x2a () 3,5 +10 frame 5")


def test_at_parsing(ep):
    """--at takes a list, ranges A-B, or `all` (every frame of the run); ranges print compressed."""
    assert ep.parse_at("0,1,10,100,599", 600) == [0, 1, 10, 100, 599]
    assert ep.parse_at("2-5", 600) == [2, 3, 4, 5]
    assert ep.parse_at("all", 600) == list(range(600))
    assert ep.fmt_frames(tuple(range(2, 600))) == "2-599"
    assert ep.fmt_frames((0, 1, 10, 100, 599)) == "0,1,10,100,599"
    assert ep.fmt_frames(7) == "7"


def test_site_table_against_the_exe(ep, canonical_exe):
    """m-4 (fix round 1): each SITES.call is `E8` to Call 0x60b320 (raw bytes), the last string pushed before it is
    "OnEvent" (0x73b964 / 0x73df20), and the event id is the last immediate pushed before the first CreateVariant
    0x413ed0 after the previous Call in the same function (function starts from the .exc index; objdump as data)."""
    import shutil
    if not shutil.which("objdump"):
        pytest.skip("SKIPPED (no objdump)")
    problems = ep.verify_sites(canonical_exe)
    assert problems == []


def test_representative_values_for_computed_arguments(ep):
    """A computed id takes 'cnt1' (0x636e7431, the Toa's key [0x782884]), 0 and 1; a computed int 0 and 1; a computed
    float 0.0 and 1.0. Vector k takes the k-th value of each list (the last one when a list is shorter)."""
    s = ep.Site("t", 0x1, 0x11, (("id?",), ("f?",), ("i?",), ("i", 5)))
    assert ep.expand(s) == [(0x11, (0x636E7431, ("f", 0.0), 0, 5)), (0x11, (0, ("f", 1.0), 1, 5)),
                            (0x11, (1, ("f", 1.0), 1, 5))]
