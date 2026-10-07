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


def test_trace_nan_never_matches():
    nan = float("nan")
    assert first_trace_mismatch([{"v": nan}], [{"v": 1.0}], 0) is not None
    assert first_trace_mismatch([{"v": 1.0}], [{"v": nan}], 1e9) is not None
    assert first_trace_mismatch([{"v": nan}], [{"v": nan}], 0) is not None


def test_trace_bool_vs_int_is_mismatch():
    assert first_trace_mismatch([{"v": True}], [{"v": 1}], 0) is not None
    assert first_trace_mismatch([{"v": 1}], [{"v": True}], 1) is not None
    assert first_trace_mismatch([{"v": True}], [{"v": False}], 0) is not None
    assert first_trace_mismatch([{"v": True}], [{"v": True}], 0) is None


def test_trace_key_set_mismatch():
    mm = first_trace_mismatch([{"a": 1}], [{"a": 1, "b": 2}], 0)
    assert mm is not None and "keys" in mm


def _write(p, rows):
    p.write_text("".join(json.dumps(r) + "\n" for r in rows))
    return str(p)


def test_compare_cli_exit_codes(tmp_path, capsys):
    from compare import main
    a = _write(tmp_path / "a.jsonl", [{"frame": 0, "t": 1.0}])
    b = _write(tmp_path / "b.jsonl", [{"frame": 0, "t": 2.0}])
    assert main(["trace", a, a]) == 0
    assert main(["trace", a, b]) == 1
    assert main(["trace", a, str(tmp_path / "missing.jsonl")]) == 2
    assert "ERROR" in capsys.readouterr().err
    bad = tmp_path / "bad.jsonl"
    bad.write_text("{not json\n")
    assert main(["trace", a, str(bad)]) == 2


def test_compare_cli_size_mismatch_is_tool_error(tmp_path, capsys):
    from PIL import Image
    from compare import main
    Image.new("RGB", (4, 4)).save(tmp_path / "s.png")
    Image.new("RGB", (5, 5)).save(tmp_path / "t.png")
    assert main(["ssim", str(tmp_path / "s.png"), str(tmp_path / "t.png")]) == 2
    assert "ERROR" in capsys.readouterr().err


def test_perf_commit_unknown_when_git_missing(monkeypatch):
    import perf_history

    def boom(*a, **k):
        raise FileNotFoundError("git")

    monkeypatch.setattr(perf_history.subprocess, "run", boom)
    assert perf_history.commit() == "unknown"


def test_perf_commit_unknown_on_timeout(monkeypatch):
    import subprocess
    import perf_history

    def boom(*a, **k):
        raise subprocess.TimeoutExpired("git", 30)

    monkeypatch.setattr(perf_history.subprocess, "run", boom)
    assert perf_history.commit() == "unknown"


def test_perf_check_does_not_call_git(tmp_path, monkeypatch):
    import perf_history
    metrics = {"frames": 600, "expected_frames": 600, "backend": "null", "adapter": "a", "frame_ms_p50": 1.0,
               "frame_ms_p95": 1.0, "draws_per_frame": 1.0, "pipelines": 1, "wall_ms": 1.0}
    m = tmp_path / "metrics.json"
    m.write_text(json.dumps(metrics))

    def boom(*a, **k):
        raise AssertionError("git must not run on check")

    monkeypatch.setattr(perf_history, "commit", boom)
    assert perf_history.main(["check", str(m), "--scene", "s"], history=tmp_path / "h.jsonl") == 0
