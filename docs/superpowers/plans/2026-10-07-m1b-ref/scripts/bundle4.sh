#!/bin/bash
# Builds <ref tree>/bundle: files/ (every added/changed repo file vs BASE), per-area diffs, MANIFEST.txt (sha256 + status),
# logs/ (absolute paths replaced by $W / $R / $LOMN_DATA / $HOME), scripts/, tasks/. REPORT.md is kept across rebuilds.
# Never copies game data, renders, screenshots, the real prefetch manifest (names/offsets/sizes of the player's files) or
# gen/ output.
# Usage: bundle4.sh <ref_tree> <work_dir>
set -euo pipefail
R="$(cd "$1" && pwd)"; W="$(cd "$2" && pwd)"
source "$R/tools/env.sh"
B="$R/bundle"
# REPORT.md and FABLE-REVIEW.md (written by the controller / reviewer) are kept across rebuilds.
KEEPDIR="$(mktemp -d "$W/keep.XXXXXX")"
for k in REPORT.md FABLE-REVIEW.md; do [ -f "$B/$k" ] && cp "$B/$k" "$KEEPDIR/"; done
rm -rf "$B"; mkdir -p "$B/files" "$B/logs" "$B/scripts"
cp "$KEEPDIR"/* "$B/" 2>/dev/null || true; rm -rf "$KEEPDIR"
rm -rf "$W/base" && mkdir -p "$W/base" && git -C "$R" archive HEAD | tar -x -C "$W/base"
cd "$R"
CHANGED=(); ADDED=()
while IFS= read -r f; do
  f="${f#./}"
  if [ -f "$W/base/$f" ]; then cmp -s "$f" "$W/base/$f" || CHANGED+=("$f"); else ADDED+=("$f"); fi
done < <(find engine tools web docs Makefile -type f ! -path '*/node_modules/*' ! -path '*/__pycache__/*' ! -name '*.pyc' ! -path '*/.pytest_cache/*' | sort)
for f in "${CHANGED[@]}" "${ADDED[@]}"; do mkdir -p "$B/files/$(dirname "$f")"; cp "$f" "$B/files/$f"; done
(cd "$W" && diff -ruN base/engine "$R/engine" | sed "s|$R|ref|g" > "$B/engine.diff" || true)
(cd "$W" && diff -ruN -x node_modules base/web "$R/web" | sed "s|$R|ref|g" > "$B/web.diff" || true)
(cd "$W" && diff -ruN -x __pycache__ base/tools "$R/tools" | sed "s|$R|ref|g" > "$B/tools.diff" || true)
(cd "$W" && diff -ruN base/docs "$R/docs" | sed "s|$R|ref|g" > "$B/docs.diff" || true)
(cd "$W" && diff -uN base/Makefile "$R/Makefile" | sed "s|$R|ref|g" > "$B/makefile.diff" || true)
cp "$W/scripts/"{build_tasks4.py,apply_tasks.sh,bundle4.sh} "$B/scripts/"
cp "$R/tools/research/oracle/mutate4.py" "$B/scripts/"
python3 "$W/scripts/build_tasks4.py" "$R" "$B/tasks" >/dev/null
TMPROOT="/private/tmp/$(id -un | sed 's/.*/claude/')-$(id -u)"  # the agent scratch root (built, so this file never names it)
scrub() { sed -e "s|$W|\$W|g" -e "s|$R|\$R|g" -e "s|$LOMN_DATA|\$LOMN_DATA|g" -e "s|$HOME|\$HOME|g" -e 's|/var/folders/[^ ]*|$TMPDIR/...|g' -e "s|$TMPROOT/[^ \"]*|\$TMP/...|g"; }
for l in "$W"/logs/*.log "$W"/logs/*.json; do [ -f "$l" ] && scrub < "$l" > "$B/logs/$(basename "$l")"; done
[ -d "$W/logs/chain" ] && { mkdir -p "$B/logs/chain"; for f in "$W"/logs/chain/*.log; do scrub < "$f" > "$B/logs/chain/$(basename "$f")"; done; }
# Metrics only (no renders, no screenshots): the browser tests' results.json files.
for r in synthetic synthetic-make-check synthetic-final run1-16mib run2-make-check run3-make-check r2-final-real r2-final-synthetic; do
  [ -f "$W/evidence/$r/results.json" ] && scrub < "$W/evidence/$r/results.json" > "$B/logs/results-$r.json"
done
{
  echo "M1b browser-viewer reference bundle (REF4). BASE $(git -C "$R" rev-parse --short HEAD) (= af5a77c). Built $(date -u +%Y-%m-%dT%H:%MZ)."
  echo "files/ mirrors the repo paths; engine.diff / web.diff / tools.diff / docs.diff / makefile.diff reproduce the tree from BASE."
  echo
  echo "CHANGED (${#CHANGED[@]}):"; for f in "${CHANGED[@]}"; do echo "  $(shasum -a 256 "$f" | cut -c1-16)  $f"; done
  echo "ADDED (${#ADDED[@]}):"; for f in "${ADDED[@]}"; do echo "  $(shasum -a 256 "$f" | cut -c1-16)  $f"; done
  echo; echo "logs/:"; (cd "$B" && find logs -type f | sort | while read -r f; do echo "  $(shasum -a 256 "$f" | cut -c1-16)  $f"; done)
  echo "scripts/:"; for f in "$B"/scripts/*; do echo "  $(shasum -a 256 "$f" | cut -c1-16)  scripts/$(basename "$f")"; done
  echo "tasks/:"; (cd "$B" && find tasks -type f | sort | while read -r f; do echo "  $(shasum -a 256 "$f" | cut -c1-16)  $f"; done)
} > "$B/MANIFEST.txt"
if grep -rIl "$HOME\|$LOMN_DATA\|$TMPROOT" "$B"; then echo "absolute path leaked"; exit 1; fi
if find "$B" -iname '*.png' -o -iname '*.blk' -o -iname '*.osi' -o -iname '*.exe' -o -iname '*.dds' | grep -q .; then echo "binary game/render file in the bundle"; exit 1; fi
if grep -rIl "root/data/art/art.blk	\|lomn-prefetch-manifest\",\"version\"" "$B/logs" 2>/dev/null; then echo "a real prefetch manifest leaked into logs"; exit 1; fi
echo "bundle: $B ($(find "$B" -type f | wc -l | tr -d ' ') files)"
