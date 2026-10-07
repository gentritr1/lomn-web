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


# ---- M1a-4 Task 2 (A12): per-area perf history ------------------------------------------------------------------------

def view_metrics(p50=1.0, frames_rendered=600, dt_samples=None, requested=600):
    """The shape of lomn_view's <out>.json (only the keys perf-areas reads)."""
    return {"tool": "lomn_view", "backend": "webgpu", "adapter": "apple/metal-3", "frames": requested,
            "frames_rendered": frames_rendered, "load_ms": 100.0, "decode_ms": 50.0, "upload_ms": 20.0,
            "frame_submit_ms_p50": p50, "frame_submit_ms_p95": p50 * 1.5,
            "frame_dt_samples": frames_rendered - 1 if dt_samples is None else dt_samples, "frame_dt_ms_p50": p50,
            "draws_last_frame": 700, "pipelines": 12, "texture_bytes_uploaded": 123456}


def area_row(p50, accepted=True, provisional=None):
    r = {"frame_ms_p50": p50, "accepted": accepted, "machine": "m", "scene": "area:lev1/vllg", "backend": "webgpu"}
    if provisional is not None:
        r["provisional"] = provisional
    return r


def area_history(tmp_path, rows):
    p = tmp_path / "h.jsonl"
    p.write_text("".join(json.dumps(r) + "\n" for r in rows))
    return p


def area_metrics_file(tmp_path, **kw):
    m = tmp_path / "metrics.json"
    m.write_text(json.dumps(view_metrics(**kw)))
    return m


def area_check(tmp_path, rows, p50):
    import perf_history
    cur = perf_history.entry(view_metrics(p50=p50), "area:lev1/vllg", False)
    cur["machine"] = "m"
    return perf_history.verdict(perf_history.load(area_history(tmp_path, rows)), cur)


def test_area_row_with_unreconciled_frames_is_rejected(tmp_path, capsys):
    import perf_history
    h = tmp_path / "h.jsonl"
    # frames rendered != frames expected
    m = area_metrics_file(tmp_path, frames_rendered=599)
    assert perf_history.main(["append", str(m), "--scene", "area:lev1/vllg", "--accept"], history=h) != 0
    assert not h.exists()                       # nothing written
    assert "599" in capsys.readouterr().err     # the reconciliation line names the numbers
    # dt samples != frames - 1
    m = area_metrics_file(tmp_path, dt_samples=600)
    assert perf_history.main(["append", str(m), "--scene", "area:lev1/vllg"], history=h) != 0
    assert not h.exists()
    # a reconciled sample is recorded, with the extra fields and the provisional flag
    m = area_metrics_file(tmp_path)
    assert perf_history.main(["append", str(m), "--scene", "area:lev1/vllg", "--accept",
                              "--loadavg-start", "2.0", "--loadavg-end", "40.0"], history=h) == 0
    row = json.loads(h.read_text())
    assert row["frames"] == row["expected_frames"] == 600 and row["frame_dt_samples"] == 599
    assert row["provisional"] is True and row["loadavg_start"] == 2.0 and row["loadavg_end"] == 40.0
    assert row["draws"] == 700 and row["texture_bytes"] == 123456 and row["upload_ms"] == 20.0


def test_area_provisional_rows_are_ignored_by_the_guard(tmp_path):
    prov = [area_row(1.0, provisional=True) for _ in range(5)]
    ok, msg = area_check(tmp_path, prov, 9.0)         # 9x slower than provisional rows: still no baseline
    assert ok and "no baseline" in msg and "provisional" in msg
    # one real row among provisional ones is still fewer than 3 usable baselines
    ok, _ = area_check(tmp_path, prov + [area_row(1.0, provisional=False)] * 2, 9.0)
    assert ok
    # non-provisional rows are what the guard uses: provisional rows with other values do not move the baseline
    real = [area_row(v, provisional=False) for v in (1.00, 1.01, 0.99)]
    ok, msg = area_check(tmp_path, [area_row(5.0, provisional=True)] * 5 + real, 1.005)
    assert ok and "n=3" in msg


def test_area_regression_larger_than_noise_fails(tmp_path):
    base = [area_row(v, provisional=False) for v in (1.00, 1.02, 0.98, 1.01, 0.99)]   # noise = max(3 sd, range) = 0.04
    ok, msg = area_check(tmp_path, base, 1.0 + 0.5)
    assert not ok and "delta +0.5" in msg and "noise 0.04" in msg


def test_area_change_within_noise_passes(tmp_path):
    base = [area_row(v, provisional=False) for v in (1.00, 1.02, 0.98, 1.01, 0.99)]
    ok, msg = area_check(tmp_path, base, 1.0 + 0.03)
    assert ok and "noise 0.04" in msg
    ok, _ = area_check(tmp_path, base, 0.5)           # faster is never a regression
    assert ok


def test_area_provisional_threshold_is_loadavg_over_6():
    import perf_history
    assert perf_history.is_provisional(6.0, 6.0, False) is False
    assert perf_history.is_provisional(6.01, 1.0, False) is True
    assert perf_history.is_provisional(1.0, 6.01, False) is True
    assert perf_history.is_provisional(1.0, 1.0, True) is True      # another GPU process overlapped
