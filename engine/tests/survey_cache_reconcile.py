"""M1a-4 Task 3 fix1 M-4: a `lomn_view --survey 1 --texture-cache on` JSON reconciles.

Every area row, failed rows included, carries a texture_cache block. The rows' hits, misses and evictions sum to the
top-level totals, and the last row's resident bytes equal the top-level bytes.
  survey_cache_reconcile.py SURVEY.json [--want-failed N] [--want-hits-min N]
Exit 0 = reconciled, 1 = not.
"""
import argparse, json, sys

ap = argparse.ArgumentParser()
ap.add_argument('survey')
ap.add_argument('--want-failed', type=int, default=None)
ap.add_argument('--want-hits-min', type=int, default=1)
a = ap.parse_args()
d = json.load(open(a.survey))
top, rows = d.get('texture_cache'), d['areas']
errs = []
if top is None:
    errs.append('no top-level texture_cache block')
missing = [r['area'] for r in rows if 'texture_cache' not in r]
if missing:
    errs.append('rows without a texture_cache block: %s' % missing)
failed = [r for r in rows if not r['ok']]
if a.want_failed is not None and len(failed) != a.want_failed:
    errs.append('failed rows %d, want %d' % (len(failed), a.want_failed))
if top is not None and not missing:
    for k in ('hits', 'misses', 'evictions'):
        s = sum(r['texture_cache'][k] for r in rows)
        if s != top[k]:
            errs.append('sum of row %s = %d != top-level %d' % (k, s, top[k]))
    if rows and rows[-1]['texture_cache']['bytes'] != top['bytes']:
        errs.append('last row bytes %d != top-level bytes %d' % (rows[-1]['texture_cache']['bytes'], top['bytes']))
    if top['hits'] < a.want_hits_min:
        errs.append('top-level hits %d < %d' % (top['hits'], a.want_hits_min))
print('rows %d (failed %d); top-level %s' % (len(rows), len(failed), top))
for e in errs:
    print('FAIL:', e)
print('reconciled' if not errs else 'NOT reconciled')
sys.exit(1 if errs else 0)
