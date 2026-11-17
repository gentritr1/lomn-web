"""A13 harness (M2a-3 T10): per-region pixel mismatch and text ink boxes between two screenshots of the same screen.

  pixel_diff.py A.png B.png --mask M.json [--threshold T] [--null]

M.json names rectangles in A's pixel space, [x0, y0, x1, y1) half-open:
  {"regions": {"hud": [..], ...},   per-region mismatch fraction
   "text":    {"Play Game": [..]},  ink bounding box in A and in B, and the per-edge delta (B - A)
   "exclude": [[..], ...]}          pixels left out of every region (e.g. the Toa, Decision 8)
A pixel mismatches when its largest channel difference (0..255, RGB) is > T. Ink in a text rectangle: pixels whose largest
channel distance from that rectangle's background colour in the same image (the most common colour on its 1-px border) is
> T. --null marks the run as the null control (A and A' are two captures of the same original screen): the numbers are its
noise floor. Thresholds are NOT chosen here: they are measured on the owner's shots and written into measurements.md
with the command, before any port-vs-original number is computed.

Output (one line each): `region <name> mismatch <frac> (<n>/<m> px) T=<T> [null]` and
`text <name> A <x0 y0 x1 y1|none> B <...|none> delta <dx0 dy0 dx1 dy1|-> [null]`.
Exit 0 on success, 2 on a tool error (unreadable image, size mismatch, bad mask).
"""
from __future__ import annotations

import argparse
import json
import sys
from collections import Counter

import numpy as np
from PIL import Image


def load(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.int16)


def rect(r, w, h):
    if not (isinstance(r, list) and len(r) == 4 and all(isinstance(v, int) for v in r)):
        raise ValueError("a rectangle is [x0, y0, x1, y1] of ints: %r" % (r,))
    x0, y0, x1, y1 = r
    if not (0 <= x0 < x1 <= w and 0 <= y0 < y1 <= h):
        raise ValueError("rectangle %r outside the %dx%d image or empty" % (r, w, h))
    return x0, y0, x1, y1


def keep_mask(mask, w, h):
    keep = np.ones((h, w), bool)
    for r in mask.get("exclude", []):
        x0, y0, x1, y1 = rect(r, w, h)
        keep[y0:y1, x0:x1] = False
    return keep


def mismatch(a, b, keep, r, t):
    x0, y0, x1, y1 = r
    d = np.abs(a[y0:y1, x0:x1] - b[y0:y1, x0:x1]).max(axis=2) > t
    k = keep[y0:y1, x0:x1]
    m = int(k.sum())
    n = int((d & k).sum())
    return n, m


def ink_box(img, keep, r, t):
    x0, y0, x1, y1 = r
    sub = img[y0:y1, x0:x1]
    border = np.concatenate([sub[0], sub[-1], sub[:, 0], sub[:, -1]])
    bg = np.array(Counter(map(tuple, border.tolist())).most_common(1)[0][0], dtype=np.int16)
    ink = (np.abs(sub - bg).max(axis=2) > t) & keep[y0:y1, x0:x1]
    ys, xs = np.nonzero(ink)
    if not len(xs):
        return None
    return [x0 + int(xs.min()), y0 + int(ys.min()), x0 + int(xs.max()) + 1, y0 + int(ys.max()) + 1]


def run(a_path, b_path, mask, t, null=False):
    a, b = load(a_path), load(b_path)
    if a.shape != b.shape:
        raise ValueError("size mismatch: %s vs %s" % (a.shape[1::-1], b.shape[1::-1]))
    h, w = a.shape[:2]
    keep = keep_mask(mask, w, h)
    tag = " null" if null else ""
    out = []
    for name, r in mask.get("regions", {}).items():
        n, m = mismatch(a, b, keep, rect(r, w, h), t)
        out.append("region %s mismatch %.6f (%d/%d px) T=%d%s" % (name, n / m if m else 0.0, n, m, t, tag))
    for name, r in mask.get("text", {}).items():
        r = rect(r, w, h)
        ba, bb = ink_box(a, keep, r, t), ink_box(b, keep, r, t)
        fmt = lambda x: " ".join(map(str, x)) if x else "none"  # noqa: E731
        delta = " ".join(str(q - p) for p, q in zip(ba, bb)) if ba and bb else "-"
        out.append("text %s A %s B %s delta %s%s" % (name.replace(" ", "_"), fmt(ba), fmt(bb), delta, tag))
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--mask", required=True)
    ap.add_argument("--threshold", type=int, default=0)
    ap.add_argument("--null", action="store_true")
    o = ap.parse_args(argv)
    try:
        with open(o.mask) as f:
            mask = json.load(f)
        lines = run(o.a, o.b, mask, o.threshold, o.null)
    except (OSError, ValueError, json.JSONDecodeError) as e:
        print("ERROR: %s" % e, file=sys.stderr)
        return 2
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
