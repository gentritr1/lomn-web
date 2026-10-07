#!/bin/bash
# Builds $W/m1a3-ref: files/ (every added/changed repo file vs BASE f47cc91), diffs, MANIFEST.txt (sha256 + status),
# logs/ (absolute paths replaced by $W / $R / $LOMN_DATA / $HOME), scripts/. Never copies game data, renders, real-area
# state/call logs or gen/ output.
set -euo pipefail
W="$(cd "$(dirname "$0")/.." && pwd)"
R="${R:-${R:-<repo>}}"
source "$W/work/tools/env.sh"
B="$W/m1a3-ref"; rm -rf "$B"; mkdir -p "$B/files" "$B/logs" "$B/scripts"
cd "$W/work"
CHANGED=(); ADDED=()
while IFS= read -r f; do
  f="${f#./}"
  if [ -f "$W/base/$f" ]; then cmp -s "$f" "$W/base/$f" || CHANGED+=("$f"); else ADDED+=("$f"); fi
done < <(find engine tools Makefile -type f ! -path '*/__pycache__/*' ! -name '*.pyc' | sort)
for f in "${CHANGED[@]}" "${ADDED[@]}"; do mkdir -p "$B/files/$(dirname "$f")"; cp "$f" "$B/files/$f"; done
(cd "$W" && diff -ruN base/engine work/engine > "$B/engine.diff" || true)
(cd "$W" && diff -uN base/Makefile work/Makefile > "$B/makefile.diff" || true)
(cd "$W" && diff -ruN -x __pycache__ base/tools work/tools > "$B/tools.diff" || true)
cp "$W/scripts/"{mutate3.py,evidence3.sh,pngdiff.py,bundle3.sh} "$B/scripts/"
cp "$W/tools/peread.py" "$B/scripts/peread.py"
for f in "$B"/scripts/*; do sed -i '' -e "s|$R|\${R:-<repo>}|g" -e "s|$HOME|\$HOME|g" "$f"; done
scrub() { sed -e "s|$W|\$W|g" -e "s|$R|\$R|g" -e "s|$LOMN_DATA|\$LOMN_DATA|g" -e "s|$HOME|\$HOME|g" -e 's|/var/folders/[^ ]*|$TMPDIR/...|g'; }
for l in make-check.log mutations3.log evidence3.log asan-run.log datacheck-wasm.log; do [ -f "$W/logs/$l" ] && scrub < "$W/logs/$l" > "$B/logs/$l"; done
[ -f "$W/tmp/a7_r2_to_r3.diff" ] && cp "$W/tmp/a7_r2_to_r3.diff" "$B/logs/a7_calls_r2_to_r3.diff"
[ -f "$W/logs/oracle.log" ] && scrub < "$W/logs/oracle.log" > "$B/logs/oracle.log"
[ -f "$W/logs/metrics.txt" ] && scrub < "$W/logs/metrics.txt" > "$B/logs/metrics.txt"
{
  echo "M1a-3 faithful-states reference bundle. BASE f47cc91 (branch m1a2-first-pixels). Built $(date -u +%Y-%m-%dT%H:%MZ)."
  echo "files/ mirrors the repo paths; engine.diff / makefile.diff / tools.diff reproduce work from base."
  echo
  echo "CHANGED (${#CHANGED[@]}):"; for f in "${CHANGED[@]}"; do echo "  $(shasum -a 256 "$f" | cut -c1-16)  $f"; done
  echo "ADDED (${#ADDED[@]}):"; for f in "${ADDED[@]}"; do echo "  $(shasum -a 256 "$f" | cut -c1-16)  $f"; done
  echo; echo "logs/:"; for f in "$B"/logs/*; do echo "  $(shasum -a 256 "$f" | cut -c1-16)  logs/$(basename "$f")"; done
  echo "scripts/:"; for f in "$B"/scripts/*; do echo "  $(shasum -a 256 "$f" | cut -c1-16)  scripts/$(basename "$f")"; done
} > "$B/MANIFEST.txt"
grep -rIl "$HOME\|$LOMN_DATA" "$B" && { echo "absolute path leaked"; exit 1; } || true
echo "bundle: $B ($(find "$B" -type f | wc -l | tr -d ' ') files)"
