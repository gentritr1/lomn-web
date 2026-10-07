#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
B=m1a2-ref
rm -rf $B && mkdir -p $B/files $B/logs $B/scripts $B/evidence-metrics
FILES=(Makefile engine/CMakeLists.txt engine/include/lomn/rhi.h engine/src/rhi/null_device.cpp engine/src/rhi/webgpu_device.cpp
  engine/src/rhi/registry.h engine/include/lomn/view/area.h engine/include/lomn/view/math.h engine/include/lomn/view/mips.h
  engine/include/lomn/view/synthetic.h engine/src/view/area.cpp engine/src/view/math.cpp engine/src/view/mips.cpp
  engine/src/view/synthetic.cpp engine/tools/lomn_view.cpp engine/tests/a7_calls.cmake engine/tests/data/a7_calls.txt
  engine/tests/test_gpu_area.cpp engine/tests/test_rhi_mesh.cpp engine/tests/test_view_area.cpp engine/tests/test_view_mips.cpp)
for f in "${FILES[@]}"; do mkdir -p "$B/files/$(dirname "$f")"; cp "work/$f" "$B/files/$f"; done
diff -ruN base/engine work/engine > $B/engine.diff || true
diff -u base/Makefile work/Makefile > $B/makefile.diff || true
cp make-check-final2.log $B/logs/make-check.log
cp asan-final.log $B/logs/asan-ctest-unit.log
cp mutate.log $B/logs/mutations.log
cp out/asan/survey.err $B/logs/asan-survey.stderr
cp out/survey.json $B/evidence-metrics/survey.json
for a in vllg ghut crss; do cp evidence/$a.png.json $B/evidence-metrics/$a.metrics.json; done
cp out/a7_calls.txt.json $B/evidence-metrics/a7_synthetic.metrics.json
cp evidence.sh farcheck.sh mutate.py mutate2.py pngtool.py bundle.sh $B/scripts/
# Every changed/new path must differ from BASE (or be new) and every diff path must be in FILES.
for f in "${FILES[@]}"; do if [ -e "base/$f" ] && cmp -s "base/$f" "work/$f"; then echo "UNCHANGED $f"; exit 1; fi; done
{ diff -rq base work 2>/dev/null || true; } | grep -v -E "Only in work: (build|\.toolchain|\.env\.local|gen|\.venv|\.pytest_cache)|Only in work/web: node_modules" \
  | sed -E 's/^Files base\/([^ ]+) and .*/\1/; s/^Only in work\/?([^:]*): (.*)/\1\/\2/; s/^\///' | sort > changed_paths.txt
echo "changed/new paths vs BASE:"; cat changed_paths.txt
