"""M2a-3 T10: tools/harness/pixel_diff.py on synthetic images (no game data). The expected numbers are counted by hand
from the drawn rectangles, not taken from the tool."""
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "harness"))
import pixel_diff  # noqa: E402


def img(tmp_path, name, draw):
    a = np.zeros((40, 60, 3), np.uint8)
    a[:] = (10, 20, 30)                      # background
    draw(a)
    p = tmp_path / name
    Image.fromarray(a).save(p)
    return p


def ink(x0, y0, x1, y1, c=(250, 250, 250)):
    def d(a):
        a[y0:y1, x0:x1] = c
    return d


MASK = {"regions": {"all": [0, 0, 60, 40], "left": [0, 0, 30, 40]}, "text": {"Play Game": [5, 5, 55, 35]}}


def test_identical_images_zero_mismatch_same_boxes(tmp_path):
    a = img(tmp_path, "a.png", ink(10, 12, 20, 18))
    out = pixel_diff.run(a, a, MASK, 0)
    assert out == ["region all mismatch 0.000000 (0/2400 px) T=0", "region left mismatch 0.000000 (0/1200 px) T=0",
                   "text Play_Game A 10 12 20 18 B 10 12 20 18 delta 0 0 0 0"]


def test_shifted_ink_counts_and_delta(tmp_path):
    a = img(tmp_path, "a.png", ink(10, 12, 20, 18))          # 10x6 = 60 px
    b = img(tmp_path, "b.png", ink(12, 13, 22, 19))          # shifted +2,+1: symmetric difference 60 + 60 - 2 * (overlap 8x5 = 40) = 40 px
    out = pixel_diff.run(a, b, MASK, 0)
    assert out[0] == "region all mismatch %.6f (40/2400 px) T=0" % (40 / 2400)
    assert out[2] == "text Play_Game A 10 12 20 18 B 12 13 22 19 delta 2 1 2 1"


def test_threshold_ignores_small_differences(tmp_path):
    a = img(tmp_path, "a.png", ink(10, 12, 20, 18))
    b = img(tmp_path, "b.png", lambda x: (ink(10, 12, 20, 18)(x), ink(40, 30, 50, 35, (13, 20, 30))(x)))  # +3 in R
    assert pixel_diff.run(a, b, MASK, 2)[0].startswith("region all mismatch 0.020833 (50/2400 px)")
    assert pixel_diff.run(a, b, MASK, 3)[0].startswith("region all mismatch 0.000000 (0/2400 px)")


def test_exclude_removes_pixels_from_regions_and_ink(tmp_path):
    a = img(tmp_path, "a.png", ink(10, 12, 20, 18))
    b = img(tmp_path, "b.png", lambda x: (ink(10, 12, 20, 18)(x), ink(40, 20, 50, 30)(x)))  # 100 px changed
    m = dict(MASK, exclude=[[40, 20, 50, 30]])
    out = pixel_diff.run(a, b, m, 0)
    assert out[0] == "region all mismatch 0.000000 (0/2300 px) T=0"
    assert out[2] == "text Play_Game A 10 12 20 18 B 10 12 20 18 delta 0 0 0 0"   # the excluded block is not ink


def test_no_ink_and_null_tag(tmp_path):
    a = img(tmp_path, "a.png", lambda x: None)
    out = pixel_diff.run(a, a, MASK, 0, null=True)
    assert out[2] == "text Play_Game A none B none delta - null" and out[0].endswith(" null")


def test_cli_errors(tmp_path, capsys):
    a = img(tmp_path, "a.png", lambda x: None)
    Image.fromarray(np.zeros((10, 10, 3), np.uint8)).save(tmp_path / "small.png")
    (tmp_path / "m.json").write_text(json.dumps(MASK))
    assert pixel_diff.main([str(a), str(tmp_path / "small.png"), "--mask", str(tmp_path / "m.json")]) == 2
    (tmp_path / "bad.json").write_text(json.dumps({"regions": {"x": [0, 0, 99, 5]}}))
    assert pixel_diff.main([str(a), str(a), "--mask", str(tmp_path / "bad.json")]) == 2
    assert pixel_diff.main([str(a), str(a), "--mask", str(tmp_path / "m.json")]) == 0
    assert "region all mismatch 0.000000" in capsys.readouterr().out
