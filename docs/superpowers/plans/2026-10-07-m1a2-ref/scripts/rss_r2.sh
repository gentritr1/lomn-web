#!/bin/bash
# Memory with the CPU copies kept (--keep-cpu on) vs released after upload (default), 3 runs each, 1 frame.
set -euo pipefail
cd "$(dirname "$0")/work"; source tools/env.sh
V=./build/native/lomn_view; I="$LOMN_DATA/install"; E=../evidence_r2/rss; mkdir -p $E
for la in lev1/vllg lev2/ghut lev4/crss; do l=${la%/*}; a=${la#*/}
  for run in 1 2 3; do for keep in on off; do timeout 300 $V --install "$I" --level $l --area $a --keep-cpu $keep --out $E/${a}_$keep$run.png >/dev/null; done; done
done
python3 - "$E" <<'PY'
import json, sys
E = sys.argv[1]
M = lambda v: '%.1f' % (v / 1048576)
print('| area | CPU copies released (bytes) | heap in use after upload -> after release, MiB (default) | heap in use after upload, MiB (--keep-cpu on) | phys_footprint after release, MiB (keep / release) | peak RSS, MiB (keep / release) |')
for a in ('vllg', 'ghut', 'crss'):
    k = [json.load(open(f'{E}/{a}_on{r}.png.json')) for r in (1, 2, 3)]
    o = [json.load(open(f'{E}/{a}_off{r}.png.json')) for r in (1, 2, 3)]
    rng = lambda xs, key: M(min(x[key] for x in xs)) + '-' + M(max(x[key] for x in xs))
    print(f"| {a} | {o[0]['cpu_bytes_released']:,} | {rng(o,'heap_in_use_after_upload_bytes')} -> {rng(o,'heap_in_use_after_release_bytes')} | {rng(k,'heap_in_use_after_release_bytes')} | {rng(k,'footprint_after_release_bytes')} / {rng(o,'footprint_after_release_bytes')} | {rng(k,'rss_peak_bytes')} / {rng(o,'rss_peak_bytes')} |")
PY
