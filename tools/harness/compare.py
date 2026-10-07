"""Compare two runs: traces first (exact, or numeric tolerance), images second (SSIM).

  compare.py trace A/trace.jsonl B/trace.jsonl [--tol 0]      exit 1 on first mismatch (prints it)
  compare.py ssim  a.png b.png                                prints SSIM in [-1,1]
  compare.py ssim-min a.png b.png --min X                     exit 1 if SSIM < X (X must come from a measured baseline)
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image


def first_trace_mismatch(a: list[dict], b: list[dict], tol: float) -> str | None:
    if len(a) != len(b):
        return f"length {len(a)} != {len(b)}"
    for i, (x, y) in enumerate(zip(a, b)):
        if x.keys() != y.keys():
            return f"line {i}: keys {sorted(x)} != {sorted(y)}"
        for k in x:
            vx, vy = x[k], y[k]
            if isinstance(vx, (int, float)) and isinstance(vy, (int, float)):
                if abs(vx - vy) > tol:
                    return f"line {i} {k}: {vx!r} != {vy!r} (tol {tol})"
            elif vx != vy:
                return f"line {i} {k}: {vx!r} != {vy!r}"
    return None


def load_trace(p: Path) -> list[dict]:
    return [json.loads(l) for l in p.read_text().splitlines() if l]


def ssim(a: np.ndarray, b: np.ndarray) -> float:
    """Global-window SSIM on luma (Wang et al. 2004 constants); images must have equal size."""
    if a.shape != b.shape:
        raise ValueError(f"size {a.shape} != {b.shape}")
    def luma(x):
        x = x.astype(np.float64)
        return 0.299 * x[..., 0] + 0.587 * x[..., 1] + 0.114 * x[..., 2]
    x, y = luma(a), luma(b)
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    mx, my = x.mean(), y.mean()
    vx, vy, cov = x.var(), y.var(), ((x - mx) * (y - my)).mean()
    return float(((2 * mx * my + c1) * (2 * cov + c2)) / ((mx ** 2 + my ** 2 + c1) * (vx + vy + c2)))


def load_png(p: Path) -> np.ndarray:
    return np.asarray(Image.open(p).convert("RGB"))


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    t = sub.add_parser("trace"); t.add_argument("a", type=Path); t.add_argument("b", type=Path); t.add_argument("--tol", type=float, default=0.0)
    s = sub.add_parser("ssim"); s.add_argument("a", type=Path); s.add_argument("b", type=Path)
    m = sub.add_parser("ssim-min"); m.add_argument("a", type=Path); m.add_argument("b", type=Path); m.add_argument("--min", type=float, required=True)
    a = ap.parse_args(argv)
    if a.cmd == "trace":
        mm = first_trace_mismatch(load_trace(a.a), load_trace(a.b), a.tol)
        print("traces identical" if mm is None else f"TRACE MISMATCH: {mm}")
        return 0 if mm is None else 1
    v = ssim(load_png(a.a), load_png(a.b))
    print(f"ssim={v:.6f}")
    return 0 if a.cmd == "ssim" or v >= a.min else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
