#!/bin/bash
# M1a-3 evidence (run from the scratch root: $W). Renders + null/state logs for vllg, ghut, crss; determinism; metrics.
set -u
W="$(cd "$(dirname "$0")/.." && pwd)"; cd "$W/work"; source tools/env.sh
V=build/native/lomn_view; E="$W/evidence"; L="$W/logs/evidence3.log"; mkdir -p "$E"; : > "$L"
PY="$W/work/.venv/bin/python"; D="$PY $W/scripts/pngdiff.py"
log() { echo "$@" | tee -a "$L"; }
for la in lev1:vllg lev2:ghut lev4:crss; do
  l=${la%%:*}; a=${la##*:}
  for run in 1 2 3; do timeout 120 $V --install "$LOMN_DATA/install" --level $l --area $a --frames 60 --out "$E/${a}_r$run.png" >/dev/null 2>>"$L" || log "FAIL render $a r$run"; done
  cp "$E/${a}_r1.png" "$E/$a.png"; cp "$E/${a}_r1.png.json" "$E/$a.png.json"
  for run in 2 3; do log "$a determinism r1 vs r$run: $($D "$E/${a}_r1.png" "$E/${a}_r$run.png" 16 8)"; done
  timeout 120 $V --install "$LOMN_DATA/install" --level $l --area $a --lighting forced --out "$E/${a}_forced.png" >/dev/null 2>>"$L"
  timeout 120 $V --install "$LOMN_DATA/install" --level $l --area $a --fog off --out "$E/${a}_nofog.png" >/dev/null 2>>"$L"
  # M1a-2's camera (fov 60, near 0.5, far 4000) without fog: compared with the M1a-2 unlit reference render.
  timeout 120 $V --install "$LOMN_DATA/install" --level $l --area $a --fov 60 --near 0.5 --far 4000 --fog off --out "$E/${a}_m1a2cam_nofog.png" >/dev/null 2>>"$L"
  log "$a vs M1a-2 unlit reference (same camera, fog off): $($D "$W/../m1a2-work/evidence_r2/$a.png" "$E/${a}_m1a2cam_nofog.png")"
  log "$a faithful vs fog off: $($D "$E/$a.png" "$E/${a}_nofog.png")"
  log "$a faithful vs forced lighting: $($D "$E/$a.png" "$E/${a}_forced.png")"
  for run in a b; do timeout 120 $V --install "$LOMN_DATA/install" --level $l --area $a --backend null --frames 2 --out "$E/${a}_null_$run.txt" --state-log "$E/${a}_state_$run.txt" >/dev/null 2>>"$L"; done
  cmp -s "$E/${a}_null_a.txt" "$E/${a}_null_b.txt" && cmp -s "$E/${a}_state_a.txt" "$E/${a}_state_b.txt" && log "$a null call log + state log identical across 2 runs" || log "FAIL $a logs differ"
done
timeout 120 $V --install "$LOMN_DATA/install" --level lev4 --area crss --yaw 67.5 --out "$E/crss_yaw67.png" >/dev/null 2>>"$L"
for r in 2 3; do timeout 120 $V --install "$LOMN_DATA/install" --level lev4 --area crss --yaw 67.5 --out "$E/crss_yaw67_r$r.png" >/dev/null 2>>"$L"; log "crss yaw67.5 r1 vs r$r: $($D "$E/crss_yaw67.png" "$E/crss_yaw67_r$r.png" 16 8)"; done
timeout 120 $V --install "$LOMN_DATA/install" --level lev4 --area crss --yaw 67.5 --lighting forced --out "$E/crss_yaw67_forced.png" >/dev/null 2>>"$L"
timeout 600 $V --install "$LOMN_DATA/install" --survey 1 --out "$E/survey.json" >> "$L" 2>&1
log "done"
