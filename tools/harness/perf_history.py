"""Performance history (spec §7.1). Every measured run appends one line to perf/history.jsonl; `check` guards
regressions using noise measured from repeated runs, never a hand-picked threshold.

  perf_history.py append <metrics.json> --scene S [--accept]    add an entry (accepted entries are baselines)
  perf_history.py check  <metrics.json> --scene S               exit 1 if frame_ms_p50 regressed beyond noise
  perf_history.py areas  [--reps N] [--accept] ...              M1a-4 A12: lomn_view on lev1/vllg, lev2/ghut, lev4/crss

Noise = max(3 x stdev, max - min) of frame_ms_p50 over the last 5 accepted entries for this machine+scene+backend:
both terms are measured from the baseline runs themselves.
With fewer than 3 accepted entries there is no baseline yet: check reports "no baseline" and passes.

A12 per-area rows (scene "area:<lev>/<area>", metrics from lomn_view's <out>.json):
  * The sample is reconciled BEFORE anything is recorded: frames rendered == frames expected, and the frame-to-frame dt
    samples == frames - 1. A mismatch exits non-zero and writes nothing. The reconciliation line is printed next to every
    p50/p95.
  * Recorded without thresholds: load/decode/upload ms, frame_ms p95, draws, pipelines, texture_bytes, frame dt p50,
    loadavg at the start and end of the run, and whether another GPU process overlapped. Only frame_ms_p50 is guarded.
  * A row measured while the host was loaded (1-minute loadavg > LOAD_LIMIT at the start or the end) or while another
    GPU/browser process ran is recorded with "provisional": true. The guard uses only non-provisional accepted rows, so a
    provisional baseline can neither hide nor cause a regression; it simply is not a baseline yet.
"""
from __future__ import annotations

import argparse
import json
import os
import platform
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
HISTORY = REPO / "perf" / "history.jsonl"
LOAD_LIMIT = 6.0     # 1-minute loadavg above which a recorded row is provisional (owner rule 2026-10-07, A12)
AREAS = ("lev1/vllg", "lev2/ghut", "lev4/crss")


def machine() -> str:
    return f"{platform.system()}-{platform.machine()}-{platform.processor() or 'cpu'}"


def commit() -> str:
    """Short HEAD hash, or "unknown" when git is missing, hangs, or REPO is not a git checkout."""
    try:
        r = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=REPO, capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return "unknown"
    return r.stdout.strip() if r.returncode == 0 and r.stdout.strip() else "unknown"


def entry(metrics: dict, scene: str, accepted: bool) -> dict:
    if metrics.get("tool") == "lomn_view":
        return view_entry(metrics, scene, accepted)
    return {"machine": machine(), "scene": scene, "accepted": accepted,
            **{k: metrics[k] for k in ("frames", "expected_frames", "backend", "adapter", "frame_ms_p50", "frame_ms_p95",
                                       "draws_per_frame", "pipelines", "wall_ms")}}


def view_entry(m: dict, scene: str, accepted: bool) -> dict:
    """A history row from lomn_view's metrics: frames = rendered, expected_frames = requested (--frames)."""
    return {"machine": machine(), "scene": scene, "accepted": accepted, "backend": m["backend"], "adapter": m["adapter"],
            "frames": m["frames_rendered"], "expected_frames": m["frames"], "frame_dt_samples": m["frame_dt_samples"],
            "frame_ms_p50": m["frame_submit_ms_p50"], "frame_ms_p95": m["frame_submit_ms_p95"],
            "frame_dt_ms_p50": m["frame_dt_ms_p50"], "load_ms": m["load_ms"], "decode_ms": m["decode_ms"],
            "upload_ms": m["upload_ms"], "draws": m["draws_last_frame"], "pipelines": m["pipelines"],
            "texture_bytes": m["texture_bytes_uploaded"]}


def reconcile(row: dict) -> tuple[bool, str]:
    """frames == expected_frames, and (when the row carries dt samples) dt samples == frames - 1."""
    ok = row["frames"] == row["expected_frames"]
    line = f"frames {row['frames']}/{row['expected_frames']}"
    if "frame_dt_samples" in row:
        want = row["frames"] - 1
        ok = ok and row["frame_dt_samples"] == want
        line += f", dt samples {row['frame_dt_samples']}/{want}"
    return ok, line + (" reconciled" if ok else " UNRECONCILED")


def is_provisional(loadavg_start: float, loadavg_end: float, overlap: bool) -> bool:
    return max(loadavg_start, loadavg_end) > LOAD_LIMIT or overlap


def load(history: Path) -> list[dict]:
    return [json.loads(l) for l in history.read_text().splitlines() if l] if history.exists() else []


def verdict(history: list[dict], current: dict) -> tuple[bool, str]:
    ok, line = reconcile(current)
    if not ok:
        return False, f"{line} (unreconciled sample)"
    same = [h for h in history if h["accepted"] and h["machine"] == current["machine"]
            and h["scene"] == current["scene"] and h["backend"] == current["backend"]]
    ignored = sum(1 for h in same if h.get("provisional"))
    base = [h["frame_ms_p50"] for h in same if not h.get("provisional")][-5:]
    if len(base) < 3:
        extra = f"; {ignored} provisional ignored" if ignored else ""
        return True, f"no baseline yet ({len(base)} non-provisional accepted runs{extra}; need 3) [{line}]"
    noise = max(3 * statistics.pstdev(base), max(base) - min(base))
    ref = statistics.median(base)
    delta = current["frame_ms_p50"] - ref
    msg = (f"p50 {current['frame_ms_p50']:.4f} ms vs baseline {ref:.4f} ms (delta {delta:+.4f}, noise {noise:.4f}, "
           f"n={len(base)}) [{line}]")
    return delta <= noise, msg


def write_row(history: Path, row: dict) -> None:
    history.parent.mkdir(parents=True, exist_ok=True)
    with open(history, "a") as f:
        f.write(json.dumps(row, sort_keys=True) + "\n")


# ---- make perf-areas -------------------------------------------------------------------------------------------------

def read_processes() -> dict[int, tuple[int, str, str]]:
    """pid -> (ppid, comm, args). comm (the executable path, spaces allowed) and args come from two `ps` calls because
    a path with spaces cannot be split out of one args column; a process present in only one listing is dropped."""
    comm_out = subprocess.run(["ps", "-Ao", "pid=,ppid=,comm="], capture_output=True, text=True, timeout=30).stdout
    args_out = subprocess.run(["ps", "-Ao", "pid=,args="], capture_output=True, text=True, timeout=30).stdout
    args: dict[int, str] = {}
    for line in args_out.splitlines():
        parts = line.split(None, 1)
        if len(parts) == 2 and parts[0].isdigit():
            args[int(parts[0])] = parts[1]
    procs: dict[int, tuple[int, str, str]] = {}
    for line in comm_out.splitlines():
        parts = line.split(None, 2)
        if len(parts) == 3 and parts[0].isdigit() and parts[1].isdigit() and int(parts[0]) in args:
            procs[int(parts[0])] = (int(parts[1]), parts[2], args[int(parts[0])])
    return procs


GPU_EXECUTABLES = {"lomn_view", "lomn_native"}
GPU_SCRIPTS = {"byo_synthetic.mjs", "a13_real.mjs", "mutate4.py", "byo_synthetic_mut.mjs"}
GPU_INTERPRETERS = re.compile(r"^(node|python[0-9.]*)$")
# A GPU process of a Chromium that a test launches (Playwright's Chromium / headless shell, or a profile under build/).
# The always-running ones, the Claude app and the user's own Google Chrome, are not perf contention and are excluded.
TEST_CHROMIUM = re.compile(r"ms-playwright|chrome-headless-shell|/Chromium\.app/|pw-profile|playwright", re.I)


def is_gpu_process(comm: str, args: str) -> bool:
    """argv-based, not a substring grep: a compiler, a build tool, an editor or a shell whose command line merely names
    lomn_view is not a GPU user."""
    base = os.path.basename(comm)
    if base in GPU_EXECUTABLES:
        return True
    if GPU_INTERPRETERS.match(base):
        return any(os.path.basename(t) in GPU_SCRIPTS for t in args.split())
    if "--type=gpu-process" in args.split():
        return bool(TEST_CHROMIUM.search(comm) or TEST_CHROMIUM.search(args))
    return False


def gpu_process_matches(procs: dict[int, tuple[int, str, str]], me: int | None = None) -> list[str]:
    """Matching processes that are not `me`, an ancestor of `me` (make, timeout, the shell) or a descendant of `me`."""
    me = os.getpid() if me is None else me
    ancestors = set()
    p = me
    while p in procs and p not in ancestors:
        ancestors.add(p)
        p = procs[p][0]

    def ours(pid: int) -> bool:
        if pid in ancestors:
            return True
        seen = set()
        while pid in procs and pid not in seen:
            if pid == me:
                return True
            seen.add(pid)
            pid = procs[pid][0]
        return False
    return [f"{pid} {args[:120]}" for pid, (_, comm, args) in sorted(procs.items())
            if is_gpu_process(comm, args) and not ours(pid)]


def other_gpu_processes() -> list[str]:
    return gpu_process_matches(read_processes())


def wait_gpu_idle(deadline_s: float) -> bool:
    """Poll until no other GPU/browser perf process is running; False when the deadline passes first."""
    end = time.monotonic() + deadline_s
    announced = False
    while True:
        busy = other_gpu_processes()
        if not busy:
            return True
        if time.monotonic() >= end:
            print("GPU busy at the deadline: " + "; ".join(busy[:3]), file=sys.stderr)
            return False
        if not announced:
            print(f"waiting for {len(busy)} other GPU process(es) to finish (deadline {deadline_s:.0f}s): {busy[0]}", flush=True)
            announced = True
        time.sleep(2)


def run_view(cmd: list[str], timeout_s: float) -> tuple[int, bool]:
    """Run lomn_view; poll for another GPU process while it runs. Returns (exit code, overlap seen)."""
    proc = subprocess.Popen(cmd)
    end = time.monotonic() + timeout_s
    overlap = False
    while True:
        try:
            return proc.wait(timeout=2), overlap
        except subprocess.TimeoutExpired:
            pass
        if time.monotonic() >= end:
            proc.kill()
            proc.wait()
            print(f"lomn_view timed out after {timeout_s:.0f}s", file=sys.stderr)
            return 124, overlap
        if other_gpu_processes():
            overlap = True


def areas_main(a: argparse.Namespace, history: Path) -> int:
    install = a.install or (os.environ.get("LOMN_DATA", "") and os.path.join(os.environ["LOMN_DATA"], "install"))
    if not install or not os.path.isdir(install):
        print("ERROR: perf-areas needs LOMN_DATA (source tools/env.sh) or --install", file=sys.stderr)
        return 2
    view = Path(a.view)
    if not view.exists():
        print(f"ERROR: {view} not built (make native)", file=sys.stderr)
        return 2
    out_dir = Path(a.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    sha = commit()
    failures = 0
    provisional_rows = 0
    summary = []
    for area in a.areas.split(","):
        level, name = area.split("/")
        scene = f"area:{area}"
        out = out_dir / f"{level}_{name}.png"
        cmd = [str(view), "--install", install, "--level", level, "--area", name, "--backend", "webgpu",
               "--frames", str(a.frames), "--out", str(out)]
        # rep 0 is the unrecorded warm-up (OS file cache, shader/driver caches); reps 1..N are recorded
        for rep in range(0, a.reps + 1):
            tag = f"{scene} " + ("warm-up" if rep == 0 else f"rep {rep}/{a.reps}")
            if not wait_gpu_idle(a.gpu_wait_s):
                print(f"PERF NOT RUN: {tag}: another GPU process held the GPU past the deadline", file=sys.stderr)
                return 3
            for f in (out, Path(str(out) + ".json")):
                f.unlink(missing_ok=True)       # a stale metrics file must never be read as this run's
            la_start = os.getloadavg()[0]
            rc, overlap = run_view(cmd, a.run_timeout_s)
            la_end = os.getloadavg()[0]
            if rc != 0:
                print(f"PERF ERROR: {tag}: lomn_view exit {rc}; nothing recorded", file=sys.stderr)
                failures += 1
                break
            try:
                tool = json.loads(Path(str(out) + ".json").read_text())
                row = entry(tool, scene, a.accept)
                # "expected" is what THIS runner asked for, never the tool's own report of what it did
                row["expected_frames"] = a.frames
            except (OSError, ValueError, KeyError) as e:
                print(f"PERF ERROR: {tag}: unreadable metrics ({e!r}); nothing recorded", file=sys.stderr)
                failures += 1
                break
            ok, recon = reconcile(row)
            if not ok:
                print(f"PERF ERROR: {tag}: {recon}; nothing recorded", file=sys.stderr)
                failures += 1
                break
            if tool["frames"] != a.frames:   # rendered == requested, but the tool believes it was asked for something else
                print(f"PERF ERROR: {tag}: lomn_view reports it was asked for {tool['frames']} frames, not {a.frames}; "
                      "nothing recorded", file=sys.stderr)
                failures += 1
                break
            prov = is_provisional(la_start, la_end, overlap)
            note = (f"loadavg {la_start:.1f}->{la_end:.1f}" + (", GPU overlap" if overlap else "")
                    + (", PROVISIONAL" if prov else ""))
            line = f"p50 {row['frame_ms_p50']:.4f} p95 {row['frame_ms_p95']:.4f} ms"
            if rep == 0:
                print(f"warm-up  {tag}: {line} [{recon}] {note}", flush=True)
                continue
            good, msg = verdict(load(history), row)   # msg carries the reconciliation line
            print(("PERF OK: " if good else "PERF REGRESSION: ") + f"{tag}: {line}; {msg}; {note}", flush=True)
            if not good:
                failures += 1
                continue
            row.update(commit=sha, loadavg_start=round(la_start, 2), loadavg_end=round(la_end, 2), gpu_overlap=overlap,
                       provisional=prov)
            write_row(history, row)
            provisional_rows += prov
            summary.append((scene, row, prov))
    for scene, row, prov in summary:
        print(f"  {scene}: p50 {row['frame_ms_p50']:.4f} p95 {row['frame_ms_p95']:.4f} ms, {row['draws']} draws, "
              f"{row['pipelines']} pipelines, {row['texture_bytes']} texture bytes" + (" (provisional)" if prov else ""))
    if provisional_rows:
        print(f"PERF PROVISIONAL: {provisional_rows} of {len(summary)} rows were recorded under load (> {LOAD_LIMIT:g}) "
              "or GPU overlap and are ignored by the guard; re-seed baselines on a quiet host.")
    return 1 if failures else 0


def main(argv: list[str], history: Path = HISTORY) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["append", "check", "areas"])
    ap.add_argument("metrics", type=Path, nargs="?")
    ap.add_argument("--scene")
    ap.add_argument("--accept", action="store_true")
    ap.add_argument("--loadavg-start", type=float)
    ap.add_argument("--loadavg-end", type=float)
    ap.add_argument("--overlap", action="store_true")
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--frames", type=int, default=600)
    ap.add_argument("--areas", default=",".join(AREAS))
    ap.add_argument("--view", default="build/native/lomn_view")
    ap.add_argument("--install")
    ap.add_argument("--out-dir", default="build/perf-areas")
    ap.add_argument("--gpu-wait-s", type=float, default=900)
    ap.add_argument("--run-timeout-s", type=float, default=300)
    a = ap.parse_args(argv)
    if a.cmd == "areas":
        return areas_main(a, history)
    if a.metrics is None or not a.scene:
        ap.error("append/check need <metrics.json> and --scene")
    try:
        cur = entry(json.loads(a.metrics.read_text()), a.scene, a.accept)
    except (OSError, ValueError, KeyError) as e:
        print(f"ERROR: unreadable metrics {a.metrics}: {e!r}", file=sys.stderr)
        return 2
    if a.cmd == "append":
        ok, recon = reconcile(cur)
        if not ok:
            print(f"ERROR: {a.scene}: {recon}; nothing recorded", file=sys.stderr)
            return 1
        cur["commit"] = commit()   # only an appended row records a commit; `check` never touches git
        if a.loadavg_start is not None or a.loadavg_end is not None or "frame_dt_samples" in cur:
            # per-area rows always carry loadavg and `provisional`; unspecified loadavg is read from the host now
            now = os.getloadavg()[0]
            ls = now if a.loadavg_start is None else a.loadavg_start
            le = now if a.loadavg_end is None else a.loadavg_end
            cur.update(loadavg_start=ls, loadavg_end=le, gpu_overlap=a.overlap, provisional=is_provisional(ls, le, a.overlap))
        write_row(history, cur)
        print("appended", cur["scene"], cur["frame_ms_p50"], f"[{recon}]")
        return 0
    ok, msg = verdict(load(history), cur)
    print(("PERF OK: " if ok else "PERF REGRESSION: ") + msg)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
