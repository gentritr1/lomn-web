"""M2a-3 T3: MOUSESPEC (docs/osi/hud.md §11) in tools/osi/keys.py, and the KEYSPEC expansion it sits next to.

Every expected tuple is hand-written from hud.md §5.1 (GcMouseDriver::Process 0x55c160): OnEvent(26, x, y) on every run,
then per button 1..4: 23 newly down, 24 still down, 25 newly up; all arguments are ints (CreateVariant 0x413ed0 / 0x4183f0
/ 0x42a5b0)."""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "osi"))
import keys  # noqa: E402

I = 0x10


def m26(f, x, y):
    return (f, [(I, 26), (I, x), (I, y)])


def btn(f, ev, b):
    return (f, [(I, ev), (I, b)])


def test_keyspec_unchanged():
    """The A6 KEYSPEC expansion is untouched (Decision 12)."""
    assert keys.expand("down@100 esc@280") == [
        (100, [(I, keys.C1), (I, 0x28), (I, 2), (0x12, 0), (0x12, 0)]),
        (101, [(I, keys.C1), (I, 0x28), (I, 1), (0x12, 0), (0x12, 0)]),
        (280, [(I, keys.C1), (I, 251), (I, 2)]),
        (281, [(I, keys.C1), (I, 251), (I, 1)]),
    ]


def test_no_mousespec_no_mouse_source():
    """Decision 4: no MOUSESPEC -> no mouse source, so A6/A9 keep their calls."""
    assert keys.expand_mouse("", 5) == []


def test_onevent26_every_frame_with_the_current_position():
    """A position token holds from its frame on; the cursor is (0, 0) before the first one (hud.md §11)."""
    assert keys.expand_mouse("450,330@1 450,370@3", 4) == [
        m26(0, 0, 0), m26(1, 450, 330), m26(2, 450, 330), m26(3, 450, 370)]


def test_press_hold_release_follow_the_down_state():
    """23 on the first frame down, 24 every later frame down, 25 on the first frame up; 26 first in each frame."""
    assert keys.expand_mouse("10,20@0 lmb+@1 lmb-@4", 6) == [
        m26(0, 10, 20),
        m26(1, 10, 20), btn(1, 23, 1),
        m26(2, 10, 20), btn(2, 24, 1),
        m26(3, 10, 20), btn(3, 24, 1),
        m26(4, 10, 20), btn(4, 25, 1),
        m26(5, 10, 20),
    ]


def test_buttons_in_number_order_within_a_frame():
    """Buttons 1, 2, 3, 4 (lmb, rmb, mmb, xmb) in that order, whatever the token order."""
    assert keys.expand_mouse("xmb+@0 rmb+@0 lmb+@0 mmb+@0", 1) == [
        m26(0, 0, 0), btn(0, 23, 1), btn(0, 23, 2), btn(0, 23, 3), btn(0, 23, 4)]


def test_release_of_an_up_button_and_press_of_a_down_one_send_nothing_new():
    """Only state transitions matter, as in the driver's sampling: up->up sends nothing, down->down is a 24."""
    assert keys.expand_mouse("rmb-@0 rmb+@1 rmb+@2", 3) == [
        m26(0, 0, 0), m26(1, 0, 0), btn(1, 23, 2), m26(2, 0, 0), btn(2, 24, 2)]


def test_tokens_at_or_after_the_run_end_have_no_effect():
    assert keys.expand_mouse("1,2@0 lmb+@2", 2) == [m26(0, 1, 2), m26(1, 1, 2)]


@pytest.mark.parametrize("spec", [
    "450,330", "450@3", "a,b@3", "450,330@-1", "-1,5@0", "5,65536@0", "lmb@3", "lmb*@3", "zmb+@3",
    "1,1@2 2,2@2", "lmb+@2 lmb-@2",
])
def test_bad_tokens_are_refused(spec):
    with pytest.raises(ValueError):
        keys.expand_mouse(spec, 10)


def test_cli_prints_one_onevent_line_per_call():
    out = subprocess.run([sys.executable, str(Path(keys.__file__)), "--mouse", "7,8@0 lmb+@1", "2"],
                         capture_output=True, text=True, check=True).stdout.splitlines()
    assert out == [
        "0 OnEvent 0010:0000001a 0010:00000007 0010:00000008",
        "1 OnEvent 0010:0000001a 0010:00000007 0010:00000008",
        "1 OnEvent 0010:00000017 0010:00000001",
    ]
