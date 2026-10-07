#!/bin/bash
# Revision-2 real-data evidence: renders vs the revision-1 PNGs, crss --yaw 67.5, null-log determinism, survey maxima,
# RSS with CPU copies kept vs released. Writes ../evidence_r2 next to work/ (never committed).
set -euo pipefail
cd "$(dirname "$0")/work"; source tools/env.sh
V=./build/native/lomn_view; I="$LOMN_DATA/install"; E=../evidence_r2; P=../evidence
mkdir -p $E
for la in lev1/vllg lev2/ghut lev4/crss; do
  l=${la%/*}; a=${la#*/}
  timeout 300 $V --install "$I" --level $l --area $a --frames 60 --out $E/$a.png >/dev/null
  echo -n "$a vs revision-1 evidence: "; python3 ../pngtool.py diff $E/$a.png $P/$a.png
  for run in a b; do timeout 300 $V --install "$I" --level $l --area $a --backend null --out $E/${a}_null_$run.txt >/dev/null; done
  cmp $E/${a}_null_a.txt $E/${a}_null_b.txt && echo "$a null log identical across 2 runs"
  for keep in on off; do
    for run in 1 2 3; do timeout 300 $V --install "$I" --level $l --area $a --keep-cpu $keep --out $E/${a}_keep${keep}_$run.png >/dev/null; done
  done
done
timeout 300 $V --install "$I" --level lev4 --area crss --yaw 67.5 --out $E/crss_yaw67.png >/dev/null
echo -n "crss_yaw67 vs revision-1 evidence: "; python3 ../pngtool.py diff $E/crss_yaw67.png $P/crss_yaw67.png
timeout 600 $V --install "$I" --survey 1 --out $E/survey.json >/dev/null
python3 - "$E" <<'PY'
import json, sys
E = sys.argv[1]
a = json.load(open(E + '/survey.json'))['max']; b = json.load(open('../out/survey.json'))['max']
print('survey maxima unchanged vs revision 1:', a == b, a)
for area in ('vllg', 'ghut', 'crss'):
    rows = {k: [json.load(open(f'{E}/{area}_keep{k}_{r}.png.json')) for r in (1, 2, 3)] for k in ('on', 'off')}
    f = lambda k, key: sorted(x[key] / 1048576 for x in rows[k])
    print(area, 'cpu_bytes_released', rows['off'][0]['cpu_bytes_released'],
          '| rss after upload MiB keep:', ['%.1f' % v for v in f('on', 'rss_after_release_bytes')],
          'release:', ['%.1f' % v for v in f('off', 'rss_after_release_bytes')],
          '| peak MiB keep:', ['%.1f' % v for v in f('on', 'rss_peak_bytes')], 'release:', ['%.1f' % v for v in f('off', 'rss_peak_bytes')])
PY
