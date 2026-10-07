#!/bin/bash
# Real-data evidence for M1a-2 (never committed). Run from anywhere; writes into ../evidence next to work/.
set -euo pipefail
cd "$(dirname "$0")/work"; source tools/env.sh
V=./build/native/lomn_view; I="$LOMN_DATA/install"; E=../evidence
for la in lev1/vllg lev2/ghut lev4/crss; do
  l=${la%/*}; a=${la#*/}
  for run in 1 2 3; do timeout 300 $V --install "$I" --level $l --area $a --frames 60 --out $E/${a}_run$run.png >/dev/null; done
  cp $E/${a}_run1.png $E/$a.png; cp $E/${a}_run1.png.json $E/$a.png.json
  for run in 2 3; do python3 ../pngtool.py diff $E/${a}_run1.png $E/${a}_run$run.png | sed "s/^/$a run1 vs run$run: /"; done
  python3 -c "
import sys; sys.path.insert(0,'..'); import pngtool
w,h,x=pngtool.read('$E/$a.png'); print('$a pixels with alpha != 255:', sum(1 for i in range(3,len(x),4) if x[i]!=255))"
  for run in a b; do timeout 300 $V --install "$I" --level $l --area $a --backend null --out $E/${a}_null_$run.txt >/dev/null; done
  cmp $E/${a}_null_a.txt $E/${a}_null_b.txt && echo "$a null call log identical across 2 runs ($(wc -l < $E/${a}_null_a.txt) lines)"
done
timeout 300 $V --install "$I" --level lev4 --area crss --yaw 67.5 --out $E/crss_yaw67.png >/dev/null
timeout 300 $V --install "$I" --level lev1 --area vllg --vertex-colors off --out $E/vllg_novc.png >/dev/null
timeout 300 $V --install "$I" --level lev2 --area ghut --vertex-colors off --out $E/ghut_novc.png >/dev/null
echo done
