#!/bin/bash
# Writes m1a2-ref/MANIFEST.txt: status + sha256 for every bundled file; lists (does not bundle) the evidence PNGs.
set -euo pipefail
cd "$(dirname "$0")/m1a2-ref"
{
  echo "# M1a-2 reference bundle manifest, revision 2 (BASE 443bd37). M = modified vs BASE, A = added. sha256 of the bundled copy."
  echo "# Repo files: copies under files/<path>; reproduce from BASE with: patch -p1 < engine.diff; patch -p0 Makefile < makefile.diff"
  (cd files && find . -type f | sed 's|^\./||' | sort) | while read -r f; do
    if [ -e "../base/$f" ]; then st=M; else st=A; fi
    cmp -s "files/$f" "../work/$f" || { echo "COPY MISMATCH $f" >&2; exit 1; }
    printf "%s %s %s\n" $st "$(shasum -a 256 "files/$f" | cut -d' ' -f1)" "$f"
  done
  echo "# Bundle support (not repo files)"
  for f in REPORT.md engine.diff makefile.diff $(find logs scripts evidence-metrics -type f | sort); do printf "S %s %s\n" "$(shasum -a 256 "$f" | cut -d' ' -f1)" "$f"; done
  echo "# Evidence PNGs (game-derived, NOT bundled; scratch paths relative to this dir):"
  for p in vllg ghut crss crss_yaw67; do echo "E ../evidence_r2/$p.png"; done
  echo "E ../evidence/vllg_novc.png"; echo "E ../evidence/ghut_novc.png"
} > MANIFEST.txt
