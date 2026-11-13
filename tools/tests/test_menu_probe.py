"""M2a-3 T1: tools/osi/menu_probe.py, the menu probe. Synthetic scripts only (osiasm, no game data); the one real-install
test is the null control against `lomn_view` and skips without LOMN_DATA. Numbers are hand-derived in each docstring."""
import importlib.util
import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
DATA = REPO / "data" / "osi"
sys.path[:0] = [str(REPO / "tools" / "osi"), str(REPO / "tools" / "research")]
import osiasm  # noqa: E402
import osi  # noqa: E402


def _load(name):
    spec = importlib.util.spec_from_file_location("osi_" + name, REPO / "tools" / "osi" / (name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = mod
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture(scope="module")
def mp():
    return _load("menu_probe")


@pytest.fixture(scope="module")
def ep():
    return _load("event_probe")


# OnEvent(id): g = g * 100 + id. Process calls one native when g == 907 (OnEvent 9 then OnEvent 7), else none.
ASM_ORDER = """
.globals g
.function Setup 0
    PushConstant0
    SetVariableValue global:g
    PushConstant0
    Return
.function Process 3
    GetVariableValue global:g
    PushConstanti16 907
    EqualTo
    CompareAndBranchIfFalse @no
    PushConstantString "abcd"
    CallGameFunction scslosifoundation.stringtoidentifier 1
    Pop
no:
    PushConstanti8 1
    Return
.function OnEvent 10
    GetVariableValue global:g
    PushConstanti8 100
    Multiply
    GetVariableValue 0
    Add
    SetVariableValue global:g
    PushConstant0
    Return
"""

# OnEvent(26, x, y) calls one native, and a second one when x == 9; every other id does nothing.
ASM_MOUSE = """
.function Setup 0
    PushConstant0
    Return
.function Process 3
    PushConstanti8 1
    Return
.function OnEvent 10
    GetVariableValue 0
    PushConstanti8 26
    EqualTo
    CompareAndBranchIfFalse @done
    PushConstantString "abcd"
    CallGameFunction scslosifoundation.stringtoidentifier 1
    Pop
    GetVariableValue 1
    PushConstanti8 9
    EqualTo
    CompareAndBranchIfFalse @done
    PushConstantString "abcd"
    CallGameFunction scslosifoundation.stringtoidentifier 1
    Pop
done:
    PushConstant0
    Return
"""


def _img(text):
    return osiasm.assemble(text)[0]


def _run(ep, img, frames, **kw):
    return ep.run(img, DATA / "natives.json", DATA / "natives_beta150.json", DATA / "types.json", frames, "", 30.0,
                  natives_impl="real", **kw)


def _natives(mp, out):
    return mp.per_frame(out)


def test_two_injections_in_one_frame_keep_their_order(mp, ep):
    """Hand-derived. g = g*100 + id: events 9 then 7 give 907 (Process then calls 1 native per frame), 7 then 9 give 709
    (none). Injected at frame 3 (pre) of 6 frames: natives per frame [0,0,0,1,1,1] one way, all 0 the other."""
    img = _img(ASM_ORDER)
    a = _run(ep, img, 6, injections=[mp.parse_event("3:9"), mp.parse_event("3:7")])
    b = _run(ep, img, 6, injections=[mp.parse_event("3:7"), mp.parse_event("3:9")])
    assert a.stop == b.stop == "none"
    assert mp.per_frame(a) == [0, 0, 0, 1, 1, 1]
    assert mp.per_frame(b) == [0] * 6


def test_pre_runs_before_process_and_post_after_it(mp, ep):
    """Hand-derived, on the 907 script: with both events `pre` at frame 3, that frame's Process already sees 907, so the
    native runs in frames 3..5; with both `post` the first Process that sees it is frame 4's: frames 4..5."""
    img = _img(ASM_ORDER)
    pre = _run(ep, img, 6, injections=[mp.parse_event("3:9:pre"), mp.parse_event("3:7:pre")])
    post = _run(ep, img, 6, injections=[mp.parse_event("3:9:post"), mp.parse_event("3:7:post")])
    assert mp.per_frame(pre) == [0, 0, 0, 1, 1, 1]
    assert mp.per_frame(post) == [0, 0, 0, 0, 1, 1]
    # the same through the first-executed pc of the branch: `watch` reports the frames
    d = bytes(img)
    o = osi.parse(d)
    proc = dict((n.lower(), off) for n, off in osi.subroutines(o))["process"]
    pc = next(at for at, op, a in osi.disasm(d, proc) if op == 0x46)
    assert mp.watch_rows(pre, [pc]) == ["watch %d: 3 frame(s), first 3, frames 3-5" % pc]
    assert mp.watch_rows(post, [pc]) == ["watch %d: 2 frame(s), first 4, frames 4,5" % pc]


def test_event_defaults_and_arguments(mp):
    """`6` defaults to post (F19), every other id to pre; arguments are ints (0x ok) or floats."""
    assert mp.parse_event("10:6:0") == mp.ep.Injection(10, 6, (0,), "post", True)
    assert mp.parse_event("10:26:450,330") == mp.ep.Injection(10, 26, (450, 330), "pre", True)
    assert mp.parse_event("7:0x1b:1.5:post") == mp.ep.Injection(7, 27, (("f", 1.5),), "post", True)
    assert mp.parse_event("2:6:pre") == mp.ep.Injection(2, 6, (), "pre", True)
    assert mp.parse_event("4:40:2:quiet") == mp.ep.Injection(4, 40, (2,), "pre", False)
    assert mp.parse_event("4:6:post:quiet") == mp.ep.Injection(4, 6, (), "post", False)
    assert mp.parse_mouse("450,330@12") == (12, 450, 330)
    with pytest.raises(ValueError):
        mp.parse_event("nonsense")


def test_mouse_fires_every_frame_from_its_frame(mp, ep):
    """Hand-derived. `--mouse 5,6@2` then `9,6@4` on 6 frames: frames 0-1 nothing; frames 2-3 one native (x = 5); frames
    4-5 two natives (x = 9, the later entry replaces the earlier from frame 4 on)."""
    out = _run(ep, _img(ASM_MOUSE), 6, mouse=[mp.parse_mouse("5,6@2"), mp.parse_mouse("9,6@4")])
    assert out.stop == "none"
    assert mp.per_frame(out) == [0, 0, 1, 1, 2, 2]
    assert mp.frames_rows(out, 6) == ["frames: n 6 expected 6 natives/frame p50 1 p95 2 max 2, calls 6 (Setup 0)"]


ASM_RESET = ASM_ORDER.replace("""    GetVariableValue global:g
    PushConstanti8 100
    Multiply
    GetVariableValue 0
    Add
    SetVariableValue global:g
    PushConstant0
    Return
""", """    GetVariableValue 0
    PushConstanti8 26
    EqualTo
    CompareAndBranchIfFalse @digits
    PushConstant0
    SetVariableValue global:g
    PushConstant0
    Return
digits:
    GetVariableValue global:g
    PushConstanti8 100
    Multiply
    GetVariableValue 0
    Add
    SetVariableValue global:g
    PushConstant0
    Return
""")


def test_mouse_runs_before_the_frames_events(mp, ep):
    """Hand-derived, on the 907 script where OnEvent(26) RESETS g to 0 and other ids append digits. Mouse from frame 1;
    `pre` events 9 then 7 in frame 1. Mouse first (the stand-in's order): g = 0, 9, 907 -> frame 1 runs 1 native; frame 2
    the mouse resets g again -> 0. [0, 1, 0]. Mouse after the events would give 907 -> 0 before Process: [0, 0, 0]."""
    img = _img(ASM_RESET)
    out = _run(ep, img, 3, injections=[mp.parse_event("1:9"), mp.parse_event("1:7")], mouse=[(1, 1, 1)])
    assert mp.per_frame(out) == [0, 1, 0]


def test_quiet_injection_leaves_its_natives_out(mp, ep):
    """`:quiet` = event_probe's single-injection record rule: the mouse script's OnEvent(26, 5) makes 1 native; recorded it
    is in the frame's count, quiet it is not (frames [0,1,0,0] against [0,0,0,0])."""
    img = _img(ASM_MOUSE)
    assert mp.per_frame(_run(ep, img, 4, injections=[mp.parse_event("1:26:5")])) == [0, 1, 0, 0]
    assert mp.per_frame(_run(ep, img, 4, injections=[mp.parse_event("1:26:5:quiet")])) == [0, 0, 0, 0]


def test_single_injection_mode_is_unchanged(mp, ep):
    """`inject=` still leaves the injected call's natives and pcs out of the run (record False): the mouse script's
    OnEvent(26) injected at frame 1 contributes 0 natives to the frame record, as before M2a-3."""
    out = _run(ep, _img(ASM_MOUSE), 3, inject=(1, 26, (5, 6)))
    assert mp.per_frame(out) == [0, 0, 0] and out.injected > 0


def test_loads_report_strings_and_frames(mp, ep):
    """`loads` lists the string arguments of the file-loading natives with the frame; Setup is -1."""
    asm = """
.function Setup 0
    PushConstantString "Root/Data/A"
    CallGameFunction sctilefont.create 1
    Pop
    PushConstant0
    Return
.function Process 3
    PushConstanti8 1
    Return
.function OnEvent 10
    PushConstant0
    Return
"""
    out = _run(ep, _img(asm), 2, want_loads=True)
    assert out.stop == "none"
    assert mp.loads_rows(out) == ["load -1 sctilefont.create 'Root/Data/A'"]


def test_frame_range_parsing(mp):
    assert mp.parse_range("-1-4") == (-1, 4) and mp.parse_range("7") == (7, 7) and mp.parse_range("10-12") == (10, 12)
    with pytest.raises(ValueError):
        mp.parse_range("a-b")


def test_percentile_is_nearest_rank(mp):
    assert mp.percentile([1, 2, 3, 4, 5, 6, 7, 8, 9, 10], 50) == 5
    assert mp.percentile([1, 2, 3, 4, 5, 6, 7, 8, 9, 10], 95) == 10
    assert mp.percentile([], 50) == 0


def test_coverage_head_uses_the_coverage_grammar(mp, ep, capsys, tmp_path):
    """Through main(): the 2-call mouse script, 6 frames, `--mouse 5,6@2 --mouse 9,6@4` = 6 calls of 1 distinct native,
    which the registry holds at tier1 (natives.json row, no overrides file)."""
    p = tmp_path / "m.osi"
    p.write_bytes(_img(ASM_MOUSE))
    rc = mp.main(["--osi", str(p), "--frames", "6", "--mouse", "5,6@2", "--mouse", "9,6@4", "--overrides",
                  str(tmp_path / "none.json")])
    out = capsys.readouterr().out.splitlines()
    assert rc == 0
    assert out[0] == "coverage: natives distinct 1 (tier1 1, tier2 0, stub 0, mod 0, tier3 0), calls 6, unregistered 0"
    assert out[1].startswith("native scslosifoundation.stringtoidentifier") and "first      2 calls      6" in out[1]


def test_null_control_equals_lomn_view(mp, lomn_data, tmp_path):
    """The oracle's own coverage of the A9 window (no injection) equals `lomn_view --script 1 --natives real` over the same
    keys: 69 distinct, 1,705 calls, row for row (the head line and every native row)."""
    lomn_view = Path(os.environ.get("LOMN_VIEW", REPO / "build" / "native" / "lomn_view"))
    if not lomn_view.exists():
        pytest.skip("SKIPPED (no built lomn_view; set LOMN_VIEW)")
    inst = lomn_data / "install"
    keys = "down@100 down@160 enter@220 esc@280"
    log = tmp_path / "natives.log"
    subprocess.run([str(lomn_view), "--install", str(inst), "--script", "1", "--natives", "real", "--frames", "600",
                    "--keys", keys, "--digest", str(tmp_path / "d"), "--natives-log", str(log)], check=True,
                   capture_output=True, timeout=110)
    ref = subprocess.run([sys.executable, str(REPO / "tools" / "osi" / "coverage.py"), "--log", str(log)], check=True,
                         capture_output=True, text=True).stdout.splitlines()
    import io
    from contextlib import redirect_stdout
    buf = io.StringIO()
    with redirect_stdout(buf):
        assert mp.main(["--osi", str(inst / "data" / "Base.osi"), "--install", str(inst), "--frames", "600", "--keys",
                        keys]) == 0
    got = buf.getvalue().splitlines()
    assert got[0] == ref[0]
    assert "natives distinct 69" in got[0] and "calls 1705" in got[0]
    assert got == ref
