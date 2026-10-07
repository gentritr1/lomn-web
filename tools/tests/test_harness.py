import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "harness"))
from compare import first_trace_mismatch, ssim  # noqa: E402
from perf_history import verdict  # noqa: E402


def test_trace_exact_and_tolerance():
    a = [{"frame": 0, "sim_time": 0.1}, {"frame": 1, "sim_time": 0.2}]
    assert first_trace_mismatch(a, a, 0) is None
    b = [{"frame": 0, "sim_time": 0.1}, {"frame": 1, "sim_time": 0.2000001}]
    assert "line 1 sim_time" in first_trace_mismatch(a, b, 0)
    assert first_trace_mismatch(a, b, 1e-6) is None
    assert "length" in first_trace_mismatch(a, a[:1], 0)


def test_ssim_identical_and_different():
    img = np.zeros((8, 8, 3), np.uint8)
    img[2:6, 2:6] = 255
    assert abs(ssim(img, img) - 1.0) < 1e-12
    assert ssim(img, 255 - img) < 0.0


def h(p50, accepted=True, machine="m", scene="s"):
    return {"frame_ms_p50": p50, "accepted": accepted, "machine": machine, "scene": scene, "backend": "null"}


def cur(p50, frames=600):
    return {**h(p50), "frames": frames, "expected_frames": 600}


def test_perf_no_baseline_passes():
    ok, msg = verdict([h(1.0)], cur(5.0))
    assert ok and "no baseline" in msg


def test_perf_regression_beyond_noise_fails():
    hist = [h(1.00), h(1.01), h(0.99)]
    assert verdict(hist, cur(1.02))[0] is True          # within 3 sigma (~0.0245)
    assert verdict(hist, cur(1.10))[0] is False


def test_perf_unreconciled_frames_fail():
    ok, msg = verdict([h(1.0)] * 3, cur(1.0, frames=599))
    assert not ok and "unreconciled" in msg
