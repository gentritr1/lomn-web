# PNG compare (Pillow): differing pixels and the largest per-channel difference; optional tolerance check.
# pngdiff.py A B [max_pixels max_level]
import sys
from PIL import Image
a = Image.open(sys.argv[1]).convert('RGBA'); b = Image.open(sys.argv[2]).convert('RGBA')
assert a.size == b.size, (a.size, b.size)
pa, pb = a.tobytes(), b.tobytes()
n = 0; m = 0
for i in range(0, len(pa), 4):
    d = max(abs(pa[i + k] - pb[i + k]) for k in range(3))
    if d: n += 1; m = max(m, d)
print('%d of %d pixels differ, max channel difference %d' % (n, a.size[0] * a.size[1], m))
if len(sys.argv) > 4:
    sys.exit(0 if n <= int(sys.argv[3]) and m <= int(sys.argv[4]) else 1)
