"""A9 full sweep (M1a-3 Task 8A, E2c): the C++ D3D8 state log (lomn_view --backend null --state-log) against the
independent Python oracle (a9_oracle.py) on EVERY area of a real install. Opt-in (`make a9-sweep`, needs LOMN_DATA);
not part of `make check` (minutes of CPU).

  a9_sweep.py --view build/native/lomn_view --install "$LOMN_DATA/install" --out build/a9-sweep [--jobs N]

Areas are enumerated here independently of lomn_view's --survey: every <area>.blk under data/levels/<level>/ except the
level's own <level>.blk (case-insensitive). Prints one line per differing area and a final "N/M equal"; exit 1 unless
every area is equal. The shim logs stay in --out (game-derived: never commit them).
"""
import argparse, os, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))


def ci_child(parent, name):
    for n in os.listdir(parent):
        if n.lower() == name.lower():
            return os.path.join(parent, n)
    return None


def areas(install):
    data = ci_child(install, 'data')
    levels = ci_child(data, 'levels') if data else None
    if not levels:
        raise SystemExit('no data/levels under %s' % install)
    out = []
    for lev in os.listdir(levels):
        p = os.path.join(levels, lev)
        if not os.path.isdir(p):
            continue
        for f in os.listdir(p):
            stem, ext = os.path.splitext(f.lower())
            if ext == '.blk' and stem != lev.lower() and len(f) >= 5:
                out.append((lev.lower(), stem))
    return sorted(out)


def one(args, la):
    lev, area = la
    state = os.path.join(args.out, '%s_%s.state.txt' % (lev, area))
    calls = os.path.join(args.out, '%s_%s.calls.txt' % (lev, area))
    r = subprocess.run([args.view, '--install', args.install, '--level', lev, '--area', area, '--backend', 'null',
                        '--out', calls, '--state-log', state], capture_output=True, text=True, timeout=300)
    if r.returncode != 0:
        return la, False, 'lomn_view exit %d: %s' % (r.returncode, (r.stderr or r.stdout).strip()[-300:])
    r = subprocess.run([sys.executable, os.path.join(HERE, 'a9_oracle.py'), '--install', args.install, '--level', lev,
                        '--area', area, '--compare', state], capture_output=True, text=True, timeout=300)
    return la, r.returncode == 0, (r.stdout + r.stderr).strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--view', required=True)
    ap.add_argument('--install', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) // 2))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    todo = areas(a.install)
    with ThreadPoolExecutor(a.jobs) as ex:
        res = list(ex.map(lambda la: one(a, la), todo))
    equal = 0
    for (lev, area), ok, msg in res:
        if ok:
            equal += 1
        else:
            print('DIFF %s/%s: %s' % (lev, area, msg.replace('\n', ' | ')))
    print('a9-sweep: %d/%d areas equal (oracle == shim)' % (equal, len(res)))
    return 0 if equal == len(res) and res else 1


if __name__ == '__main__':
    sys.exit(main())
