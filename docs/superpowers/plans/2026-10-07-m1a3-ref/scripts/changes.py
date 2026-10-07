"""Line-level change selector for building per-task intermediate files (REF3 revision 2, item 2).
  changes.py list BASE FINAL            numbered changes (difflib opcodes) with a preview
  changes.py build BASE FINAL SEL OUT   BASE with only the selected changes (SEL: "1,3-5", "all", "none") applied
A missing BASE counts as empty."""
import difflib, os, sys


def lines(p):
    return open(p).read().splitlines(keepends=True) if p and os.path.exists(p) else []


def ops(a, b):
    return [o for o in difflib.SequenceMatcher(None, a, b, autojunk=False).get_opcodes() if o[0] != 'equal']


def parse_sel(s, n):
    if s == 'all': return set(range(1, n + 1))
    if s == 'none' or s == '': return set()
    out = set()
    for part in s.split(','):
        if '-' in part:
            x, y = part.split('-'); out.update(range(int(x), int(y) + 1))
        else:
            out.add(int(part))
    bad = [i for i in out if i < 1 or i > n]
    if bad: sys.exit('selection out of range 1..%d: %s' % (n, bad))
    return out


def main():
    cmd, base, final = sys.argv[1], sys.argv[2], sys.argv[3]
    a, b = lines(base), lines(final)
    o = ops(a, b)
    if cmd == 'list':
        for i, (tag, i1, i2, j1, j2) in enumerate(o, 1):
            print('#%d %s base %d-%d final %d-%d' % (i, tag, i1 + 1, i2, j1 + 1, j2))
            for l in a[i1:i2][:3]: print('   -' + l.rstrip()[:150])
            for l in b[j1:j2][:4]: print('   +' + l.rstrip()[:150])
        return
    sel = parse_sel(sys.argv[4], len(o))
    out, pos = [], 0
    for i, (tag, i1, i2, j1, j2) in enumerate(o, 1):
        out += a[pos:i1]
        out += b[j1:j2] if i in sel else a[i1:i2]
        pos = i2
    out += a[pos:]
    os.makedirs(os.path.dirname(os.path.abspath(sys.argv[5])), exist_ok=True)
    open(sys.argv[5], 'w').write(''.join(out))


if __name__ == '__main__':
    main()
