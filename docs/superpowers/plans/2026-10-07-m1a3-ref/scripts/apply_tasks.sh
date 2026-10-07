#!/bin/bash
# Applies the M1a-3 task chain cumulatively to a FRESH BASE (f47cc91) tree and runs `make test-native test-wasm` after
# every task (REF3 revision 2, item 2). TREE must be a BASE checkout whose .toolchain / .venv / .env.local work (as for
# make check). Stops at the first failing task. Usage: apply_tasks.sh TASKS_DIR TREE [LOGDIR]
set -uo pipefail
TASKS="$(cd "$1" && pwd)"; TREE="$(cd "$2" && pwd)"; LOGS="${3:-$TREE/build/task-chain}"
mkdir -p "$LOGS"
for n in 1 2 3 4 5 6 7; do
  t="T$n"
  [ -d "$TASKS/$t/files" ] || { echo "$t: missing $TASKS/$t/files"; exit 1; }
  (cd "$TASKS/$t/files" && find . -type f | sed 's|^\./||') | while IFS= read -r f; do
    mkdir -p "$TREE/$(dirname "$f")" && cp "$TASKS/$t/files/$f" "$TREE/$f"
  done
  start=$(date +%s)
  (cd "$TREE" && timeout 3000 make test-native test-wasm) > "$LOGS/$t.log" 2>&1
  rc=$?
  secs=$(( $(date +%s) - start ))
  nat=$(grep -E "tests passed" "$LOGS/$t.log" | sed -n 1p)
  was=$(grep -E "tests passed" "$LOGS/$t.log" | sed -n 2p)
  nfiles=$(cd "$TASKS/$t/files" && find . -type f | wc -l | tr -d ' ')
  echo "$t ($nfiles files): make test-native test-wasm rc=$rc in ${secs}s | native: ${nat:-?} | wasm32: ${was:-?}"
  [ $rc -eq 0 ] || { echo "$t FAILED - see $LOGS/$t.log"; tail -30 "$LOGS/$t.log"; exit 1; }
done
echo "task chain: T1..T7 each built and passed make test-native + make test-wasm"
