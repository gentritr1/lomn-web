"""Performance history (spec §7.1). Every measured run appends one line to perf/history.jsonl; `check` guards
regressions using noise measured from repeated runs, never a hand-picked threshold.

  perf_history.py append <metrics.json> --scene S [--accept]    add an entry (accepted entries are baselines)
  perf_history.py check  <metrics.json> --scene S               exit 1 if frame_ms_p50 regressed beyond noise

Noise = max(3 x stdev of frame_ms_p50 over the last 5 accepted entries for this machine+scene, 0.01 ms resolution).
With fewer than 3 accepted entries there is no baseline yet: check reports "no baseline" and passes.
"""
from __future__ import annotations

import argparse
import json
import platform
import statistics
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
HISTORY = REPO / "perf" / "history.jsonl"
RESOLUTION_MS = 0.01


def machine() -> str:
    return f"{platform.system()}-{platform.machine()}-{platform.processor() or 'cpu'}"


def commit() -> str:
    r = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=REPO, capture_output=True, text=True, timeout=30)
    return r.stdout.strip() or "uncommitted"


def entry(metrics: dict, scene: str, accepted: bool) -> dict:
    return {"commit": commit(), "machine": machine(), "scene": scene, "accepted": accepted,
            **{k: metrics[k] for k in ("frames", "expected_frames", "backend", "adapter", "frame_ms_p50", "frame_ms_p95",
                                       "draws_per_frame", "pipelines", "wall_ms")}}


def load(history: Path) -> list[dict]:
    return [json.loads(l) for l in history.read_text().splitlines() if l] if history.exists() else []


def verdict(history: list[dict], current: dict) -> tuple[bool, str]:
    if current["frames"] != current["expected_frames"]:
        return False, f"frame count {current['frames']} != expected {current['expected_frames']} (unreconciled sample)"
    base = [h["frame_ms_p50"] for h in history if h["accepted"] and h["machine"] == current["machine"]
            and h["scene"] == current["scene"] and h["backend"] == current["backend"]][-5:]
    if len(base) < 3:
        return True, f"no baseline yet ({len(base)} accepted runs; need 3)"
    noise = max(3 * statistics.pstdev(base), RESOLUTION_MS)
    ref = statistics.median(base)
    delta = current["frame_ms_p50"] - ref
    msg = f"p50 {current['frame_ms_p50']:.4f} ms vs baseline {ref:.4f} ms (delta {delta:+.4f}, noise {noise:.4f}, n={len(base)})"
    return delta <= noise, msg


def main(argv: list[str], history: Path = HISTORY) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["append", "check"])
    ap.add_argument("metrics", type=Path)
    ap.add_argument("--scene", required=True)
    ap.add_argument("--accept", action="store_true")
    a = ap.parse_args(argv)
    cur = entry(json.loads(a.metrics.read_text()), a.scene, a.accept)
    if a.cmd == "append":
        history.parent.mkdir(parents=True, exist_ok=True)
        with open(history, "a") as f:
            f.write(json.dumps(cur, sort_keys=True) + "\n")
        print("appended", cur["scene"], cur["frame_ms_p50"])
        return 0
    ok, msg = verdict(load(history), cur)
    print(("PERF OK: " if ok else "PERF REGRESSION: ") + msg)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
