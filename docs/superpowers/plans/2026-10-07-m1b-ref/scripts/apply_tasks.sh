#!/bin/bash
# REF4 task chain check: copies tasks/Tn/files/ cumulatively over a fresh BASE tree (af5a77c) and runs
# `make test-native test-wasm test-web` after every task; finally the tree must equal the reference tree.
# Usage: apply_tasks.sh <tasks_dir> <chain_dir> <repo_with_BASE_commit> <reference_tree> <log_dir>
#   <repo_with_BASE_commit>: a git checkout whose HEAD is BASE (git archive HEAD gives the BASE files)
#   The toolchain shims (.toolchain, gen, .env.local, web/node_modules, .venv) are symlinked from <repo_with_BASE_commit>.
# Builds run with CMAKE_BUILD_PARALLEL_LEVEL=4 (shared host). Exit 0 only if every task passes and the tree matches.
set -uo pipefail
TASKS="$(cd "$1" && pwd)"; CHAIN="$2"; REPO="$(cd "$3" && pwd)"; REF="$(cd "$4" && pwd)"; LOGS="$5"
export CMAKE_BUILD_PARALLEL_LEVEL=4
mkdir -p "$LOGS"
rm -rf "$CHAIN" && mkdir -p "$CHAIN"
CHAIN="$(cd "$CHAIN" && pwd)"
git -C "$REPO" archive HEAD | tar -x -C "$CHAIN" || { echo "git archive failed"; exit 2; }
for s in .toolchain gen .env.local; do ln -s "$(readlink "$REPO/$s" 2>/dev/null || echo "$REPO/$s")" "$CHAIN/$s"; done
ln -s "$(readlink "$REPO/web/node_modules" 2>/dev/null || echo "$REPO/web/node_modules")" "$CHAIN/web/node_modules"
mkdir -p "$CHAIN/.venv" && touch "$CHAIN/.venv/.ok"
for s in bin include lib pyvenv.cfg; do ln -s "$(readlink "$REPO/.venv/$s")" "$CHAIN/.venv/$s"; done
summary="$LOGS/task-chain.log"
echo "task chain over BASE $(git -C "$REPO" rev-parse --short HEAD), started $(date +%T), loadavg $(sysctl -n vm.loadavg 2>/dev/null)" > "$summary"
fail=0
for t in T1 T2 T3 T4 T5a T5b T6; do
  [ -d "$TASKS/$t/files" ] || { echo "$t: missing" | tee -a "$summary"; fail=1; break; }
  cp -R "$TASKS/$t/files/." "$CHAIN/"
  t0=$(date +%s)
  ( cd "$CHAIN" && source tools/env.sh && export PATH=/opt/homebrew/bin:$PATH && make test-native test-wasm test-web ) > "$LOGS/$t.log" 2>&1
  rc=$?
  nat=$(grep -E "tests passed, [0-9]+ tests failed out of" "$LOGS/$t.log" | head -1)
  was=$(grep -E "tests passed, [0-9]+ tests failed out of" "$LOGS/$t.log" | sed -n 2p)
  web=$(grep -E "^byo_synthetic: |pixels \{" "$LOGS/$t.log" | tr '\n' ' ')
  echo "$t rc=$rc $(( $(date +%s) - t0 )) s | native: $nat | wasm: $was | web: $web" | tee -a "$summary"
  [ $rc -eq 0 ] || { fail=1; break; }
done
if [ $fail -eq 0 ]; then
  d=$(diff -rq -x node_modules -x __pycache__ -x .pytest_cache "$CHAIN/engine" "$REF/engine"; diff -rq -x node_modules "$CHAIN/web" "$REF/web";
      diff -rq -x __pycache__ "$CHAIN/tools" "$REF/tools"; diff -rq "$CHAIN/docs" "$REF/docs"; diff -q "$CHAIN/Makefile" "$REF/Makefile")
  if [ -n "$d" ]; then echo "tree differs from the reference:"; echo "$d"; fail=1; else echo "after T6 the tree equals the reference (engine, web, tools, docs, Makefile)"; fi | tee -a "$summary"
fi
echo "finished $(date +%T), loadavg $(sysctl -n vm.loadavg 2>/dev/null), result $([ $fail -eq 0 ] && echo PASS || echo FAIL)" | tee -a "$summary"
exit $fail
