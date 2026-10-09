# lomn-web top-level entry points. Recipes use TABs.
SHELL := /bin/bash
-include .env.local
export LOMN_DATA LOMN_CACHE
VENV := .venv
PY := $(VENV)/bin/python
EMSDK_ENV := source .toolchain/emsdk/emsdk_env.sh >/dev/null 2>&1
.PHONY: setup check-data decomp worklist native wasm site test-tools test-native test-wasm test-web check perf perf-areas datacheck datacheck-wasm fuzz-osi fuzz-osi-vm fuzz-formats fuzz-formats-help a9-sweep a13-real manifest osi-tables osi-diff osi-coverage conform-trace-cmp

setup: $(VENV)/.ok
	timeout 3600 tools/toolchain.sh

$(VENV)/.ok: tools/requirements.txt
	timeout 900 python3 -m venv $(VENV) && timeout 900 $(VENV)/bin/pip install -q -r tools/requirements.txt && touch $@

check-data: $(VENV)/.ok
	timeout 300 $(PY) tools/lomn_data.py

decomp: check-data
	mkdir -p docs gen
	timeout 3900 $(PY) tools/decomp/run_decomp.py --exe "$(LOMN_DATA)/reference/LEGOBionicle.exe"
	timeout 300 $(PY) tools/decomp/worklist.py build gen/decomp worklist/status.json
	timeout 300 $(PY) tools/decomp/worklist.py dashboard gen/decomp worklist/status.json docs/dashboard.html
	timeout 300 $(PY) tools/decomp/surface.py "$(LOMN_DATA)/reference/LEGOBionicle.exe" gen/decomp tools/decomp/data/d3d8_vtables.txt docs/surface.md
	timeout 300 $(PY) tools/census.py "$(LOMN_DATA)/install" gen/census && cp gen/census/census.md docs/census.md

# M2a-0: OSI VM citation tables (docs/osi/). opcodes.md: generated columns from the canonical exe (as data), the decomp
# corpus and Base.osi; the authored columns (stack_effect, errors, notes) are kept by op.
# M2a-0 T3: registry.py writes data/osi/natives.json (COMMITTED: 1,056 native rows, names/addresses/counts/type names only,
# so M2a-2 needs no Ghidra run) and docs/osi/types.md (the variant-type
# table: builtin ids, the game-type order rule, duplicates). It needs `make decomp` first (reads gen/decomp).
# M2a-1 T0: also the `ret`/`sets_fpu_pc64` columns, data/osi/types.json (game-type ids) and data/osi/natives_beta150.json
# (the Beta 1.5.0 mod natives, decoded from install/mods/*.dll read as data). All three are COMMITTED.
# M2a-2 T2b: registry.py merges data/osi/native_overrides.json (tiers, decided defaults; docs/osi/natives.md §7.2; an
# empty file changes no byte), and error_pc.py rewrites the error-PC block of opcodes.md after opcodes.py has run.
osi-tables: $(VENV)/.ok
	timeout 300 $(PY) tools/osi/opcodes.py --exe "$(LOMN_DATA)/reference/LEGOBionicle.exe" --osi "$(LOMN_DATA)/install/data/Base.osi" --corpus gen/decomp --out docs/osi/opcodes.md
	timeout 300 $(PY) tools/osi/registry.py --exe "$(LOMN_DATA)/reference/LEGOBionicle.exe" --osi "$(LOMN_DATA)/install/data/Base.osi" --corpus gen/decomp --out-json data/osi/natives.json --out-types docs/osi/types.md --out-types-json data/osi/types.json --mods "$(LOMN_DATA)/install/mods" --out-mods data/osi/natives_beta150.json --overrides data/osi/native_overrides.json
	timeout 300 $(PY) tools/osi/error_pc.py --exe "$(LOMN_DATA)/reference/LEGOBionicle.exe" --write docs/osi/opcodes.md

worklist: $(VENV)/.ok
	timeout 1800 $(PY) tools/decomp/worklist.py check gen/decomp worklist/status.json . --ctest-dir build/native

native:
	timeout 600 cmake -S engine -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release && timeout 1800 cmake --build build/native

wasm:
	$(EMSDK_ENV) && timeout 600 emcmake cmake -S engine -B build/wasm -G Ninja -DCMAKE_BUILD_TYPE=Release && timeout 1800 cmake --build build/wasm

# M1b: index.html + web/app/*.js + the wasm build + builds.json, and asset-manifest.json (the network-discipline list).
site: wasm
	timeout 120 node web/tools/build_site.mjs build/wasm build/site

# A skipped test is not a pass, and neither is an expected failure (M1b T7, T8 P15). pytest runs with -rsxX and its report is
# gated by tools/pytest_gate.py (unit-tested on synthetic reports in tools/tests/test_pytest_gate.py): every skip must match
# the allowlist PYTEST_SKIP_ALLOW (a regex over the "SKIPPED [n] file:line: reason" line; EMPTY = no skip is allowed), every
# xfail / xpass must match PYTEST_XFAIL_ALLOW (EMPTY = none), the counts listed must equal the "N skipped / xfailed / xpassed"
# of pytest's summary line (so a changed report format cannot hide any), and a skip is also tolerated when LOMN_DATA is unset
# and the line ENDS with the reason "(no LOMN_DATA)" (anchored: "needs node and LOMN_DATA" no longer hides a missing node; the
# data-free subset is all that can run then; check prints "SKIPPED (no LOMN_DATA)" for the data layers too). test-tools needs
# build/native (test_m1a1_oracles runs lomn_datacheck), so it runs after `native`. The log is build/pytest-tools.log.
PYTEST_SKIP_ALLOW ?=
PYTEST_XFAIL_ALLOW ?=
test-tools: native $(VENV)/.ok
	@mkdir -p build
	$(EMSDK_ENV); set -o pipefail; timeout 1800 $(VENV)/bin/pytest -q -rsxX tools 2>&1 | tee build/pytest-tools.log
	@$(PY) tools/pytest_gate.py build/pytest-tools.log --allow-skip "$(PYTEST_SKIP_ALLOW)" --allow-xfail "$(PYTEST_XFAIL_ALLOW)" $$([ -z "$$LOMN_DATA" ] && echo --no-data)

test-native: native
	ctest --test-dir build/native --output-on-failure --timeout 300

test-wasm: wasm test-native $(VENV)/.ok
	$(EMSDK_ENV) && ctest --test-dir build/wasm --output-on-failure --timeout 300
	timeout 120 $(PY) tools/harness/compare.py trace build/native/determinism/a/trace.jsonl build/wasm/determinism/a/trace.jsonl
	cmp build/native/determinism/a/calls.txt build/wasm/determinism/a/calls.txt
	cmp build/native/a7/a/calls.txt build/wasm/a7/a/calls.txt
	cmp build/native/a9/a/state.txt build/wasm/a9/a/state.txt
	cmp build/native/a9/a/calls.txt build/wasm/a9/a/calls.txt
	cmp build/native/a9/f2/state.txt build/wasm/a9/f2/state.txt

# M1b: + the data-free BYO/viewer test on a synthetic install (needs the native lomn_view for the fixture and the
# reference renders) and the fingerprint/JS unit tests.
test-web: site native
	cd web && timeout 60 node --test tests/serve_test.mjs tests/network_check_test.mjs tests/lib_wait_test.mjs tests/a13_checks_test.mjs tests/unit_test.mjs tests/unit_r2_test.mjs
	cd web && timeout 180 node tests/smoke.mjs ../build/site ../build/pw-profile
	cd web && timeout 600 node tests/byo_synthetic.mjs ../build/wasm ../build/byo-syn ../build/native/lomn_view

# M1b A13 on the real install (LOMN_DATA; skipped otherwise): OPFS copy of the install into a throwaway Playwright
# profile (removed afterwards), lev1/vllg, lev2/ghut, lev4/crss vs native renders, switches, revisit, handle path, fps.
a13-real: site native
	@if [ -z "$(LOMN_DATA)" ]; then echo "a13-real SKIPPED (no LOMN_DATA)"; exit 0; fi
	cd web && timeout 1800 node tests/a13_real.mjs ../build/site ../build/a13-real ../build/native/lomn_view ../build/wasm

# M1b: the per-area prefetch manifest of the install (build/m1b/manifest.json; derived data: never committed).
manifest: native
	@if [ -z "$(LOMN_DATA)" ]; then echo "manifest: LOMN_DATA is not set"; exit 2; fi
	mkdir -p build/m1b && timeout 300 build/native/lomn_view --install "$(LOMN_DATA)/install" --manifest 1 --out build/m1b/manifest.json

perf: native $(VENV)/.ok
	timeout 300 build/native/lomn_native --backend webgpu --frames 600 --out build/perf
	timeout 120 $(PY) tools/harness/perf_history.py check build/perf/metrics.json --scene m0-triangle
	timeout 120 $(PY) tools/harness/perf_history.py append build/perf/metrics.json --scene m0-triangle

# M1a-4 A12 (opt-in, not part of `make check`): lomn_view on the real GPU over lev1/vllg, lev2/ghut and lev4/crss, 600 frames,
# one unrecorded warm-up then REPS (default 3) recorded runs per area. Each run is reconciled (frames == expected, dt samples ==
# frames - 1) before it is recorded, guarded on frame_ms_p50 by the measured noise, and marked provisional when the 1-minute
# loadavg is above 6 or another GPU process overlapped. `make perf-areas ACCEPT=1 REPS=5` seeds accepted baselines. Waits (with
# a deadline) while another GPU/browser perf process runs; never measures concurrently with one.
perf-areas: native $(VENV)/.ok
	timeout 7200 $(PY) tools/harness/perf_history.py areas --reps $(or $(REPS),3) $(if $(ACCEPT),--accept) --view build/native/lomn_view --install "$(LOMN_DATA)/install"

datacheck: native $(VENV)/.ok
	timeout 1200 $(PY) tools/research/oracle/check_datacheck.py --exe build/native/lomn_datacheck --install "$(LOMN_DATA)/install"

# M1a-3: the same oracle against the wasm32 datacheck under Node (NODERAWFS disk file system), then native == wasm32 on
# the real-data null logs of vllg / ghut / crss (call log and D3D8 state log). Task 8B F2: a failing environment
# (emsdk_env) exits the loop line explicitly ("&& for ...; done; echo" fell through to the success echo). Not set -e:
# emsdk_env.sh is sourced and may run commands that fail harmlessly.
datacheck-wasm: wasm native $(VENV)/.ok
	$(EMSDK_ENV) && timeout 1800 $(PY) tools/research/oracle/check_datacheck.py --exe build/wasm/lomn_datacheck_node.sh --install "$(LOMN_DATA)/install"
	$(EMSDK_ENV) || { echo "datacheck-wasm: emsdk environment failed"; exit 1; }; for la in lev1:vllg lev2:ghut lev4:crss; do l=$${la%%:*}; a=$${la##*:}; \
	  timeout 300 build/native/lomn_view --install "$(LOMN_DATA)/install" --level $$l --area $$a --backend null --out build/native/$$a.calls.txt --state-log build/native/$$a.state.txt >/dev/null && \
	  timeout 300 node build/wasm/lomn_view.js --install "$(LOMN_DATA)/install" --level $$l --area $$a --backend null --out build/wasm/$$a.calls.txt --state-log build/wasm/$$a.state.txt >/dev/null && \
	  cmp build/native/$$a.calls.txt build/wasm/$$a.calls.txt && cmp build/native/$$a.state.txt build/wasm/$$a.state.txt || exit 1; done; echo "native == wasm32: real-area null logs"

# M1a-3 Task 8A E2c: the A9 shim state log vs the independent Python oracle on EVERY area of the install (123 on Beta
# 1.5.0); must print "123/123 areas equal". Opt-in (needs LOMN_DATA; not part of make check). Logs stay in build/a9-sweep.
a9-sweep: native $(VENV)/.ok
	@if [ -z "$(LOMN_DATA)" ]; then echo "a9-sweep: LOMN_DATA is not set"; exit 2; fi
	timeout 1800 $(PY) tools/research/oracle/a9_sweep.py --view build/native/lomn_view --install "$(LOMN_DATA)/install" --out build/a9-sweep

# M1a-3 REF3 r2: ASan+UBSan mutation fuzz of the OSI reader (engine/tests/fuzz_osi.cpp), natively and as wasm32 under
# Node; seeds from tests/osi_builder.h plus, natively and when LOMN_DATA is set, the real Base.osi. On demand only.
FUZZ_OSI_N ?= 200000
FUZZ_OSI_N_WASM ?= 20000
FUZZ_FLAGS := -std=c++17 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -Iengine/include -Iengine/src/sage -Iengine/tests
fuzz-osi:
	mkdir -p build/fuzz
	timeout 600 clang++ $(FUZZ_FLAGS) engine/tests/fuzz_osi.cpp engine/src/sage/osi.cpp -o build/fuzz/fuzz_osi
	timeout 1800 build/fuzz/fuzz_osi $(FUZZ_OSI_N) 1
	@if [ -n "$(LOMN_DATA)" ]; then timeout 1800 build/fuzz/fuzz_osi 2000 2 "$(LOMN_DATA)/install/data/Base.osi"; fi
	$(EMSDK_ENV) && timeout 900 em++ $(FUZZ_FLAGS) -sNODERAWFS=1 -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=2GB engine/tests/fuzz_osi.cpp engine/src/sage/osi.cpp -o build/fuzz/fuzz_osi.js
	$(EMSDK_ENV) && timeout 1800 node build/fuzz/fuzz_osi.js $(FUZZ_OSI_N_WASM) 3

# M2a-1 T5: ASan+UBSan mutation-fuzz SEED harness of the script VM (engine/tests/fuzz_osi_vm.cpp), natively. Seeds: the
# conformance corpus assembled into build/fuzz/osi-vm-seeds (never committed) plus, when LOMN_DATA is set, the real
# Base.osi; typed stubs from data/osi; FUZZ_OSI_VM_STEPS instructions per engine call. On demand only (the campaign is M2a-4).
FUZZ_OSI_VM_N ?= 5000
FUZZ_OSI_VM_STEPS ?= 100000
OSI_VM_SRCS := engine/src/osi/heap.cpp engine/src/osi/convert.cpp engine/src/osi/registry.cpp engine/src/osi/trace.cpp \
  engine/src/osi/vm.cpp engine/src/osi/vm_handlers.cpp engine/src/osi/natives/framework.cpp engine/src/sage/osi.cpp
fuzz-osi-vm: $(VENV)/.ok
	mkdir -p build/fuzz
	rm -rf build/fuzz/osi-vm-seeds && timeout 120 $(PY) tools/osi/conform_assemble.py tests/osi/conformance build/fuzz/osi-vm-seeds
	timeout 900 clang++ $(FUZZ_FLAGS) -ffp-contract=off engine/tests/fuzz_osi_vm.cpp $(OSI_VM_SRCS) -o build/fuzz/fuzz_osi_vm
	timeout 1800 build/fuzz/fuzz_osi_vm $(FUZZ_OSI_VM_N) 1 data/osi build/fuzz/osi-vm-seeds $(if $(LOMN_DATA),"$(LOMN_DATA)/install/data/Base.osi",) --steps $(FUZZ_OSI_VM_STEPS)

# M1a-4 A11: ASan+UBSan (-fno-sanitize-recover) mutation fuzz of every real file format (engine/tests/fuzz_formats.cpp:
# BLK + LZSS, SLB OBJ/POS/LIGHT/textures, binary .x, DDS, TGA, OSI), natively (build/asan/native) and as wasm32 under
# Node (build/asan/wasm, 2 GB cap), with real seeds sampled from LOMN_DATA when it is set. On demand only (not part of
# check; ctest fuzz_formats_smoke is the fast synthetic subset). FUZZ_N inputs per format. Logs and finding inputs stay
# under build/asan. When FUZZ_N == FUZZ_N_WASM the per-format totals must be byte-identical between the two targets.
# How to run and read it: tools/research/fuzz_formats.md (`make fuzz-formats-help` prints it).
FUZZ_N ?= 20000
FUZZ_N_WASM ?= $(FUZZ_N)
FUZZ_SEED ?= 1
FUZZ_TIMEOUT ?= 10800
ASAN_CXXFLAGS := -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all
ASAN_CMAKE := -G Ninja -DCMAKE_BUILD_TYPE=Debug -DLOMN_WEBGPU=OFF "-DCMAKE_CXX_FLAGS=$(ASAN_CXXFLAGS)" "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined"
FUZZ_ARGS = --n $(1) --seed $(FUZZ_SEED) $(if $(LOMN_DATA),--data "$(LOMN_DATA)/install",)
fuzz-formats-help:
	@cat tools/research/fuzz_formats.md

fuzz-formats:
	timeout 600 cmake -S engine -B build/asan/native $(ASAN_CMAKE) >/dev/null && timeout 1800 cmake --build build/asan/native --target lomn_fuzz_formats
	$(EMSDK_ENV) && timeout 600 emcmake cmake -S engine -B build/asan/wasm $(ASAN_CMAKE) >/dev/null && timeout 1800 cmake --build build/asan/wasm --target lomn_fuzz_formats
	set -o pipefail; timeout $(FUZZ_TIMEOUT) build/asan/native/lomn_fuzz_formats $(call FUZZ_ARGS,$(FUZZ_N)) --crash-dir build/asan/native/fuzz-crashes | tee build/asan/native/fuzz_formats.txt
	set -o pipefail; $(EMSDK_ENV) && timeout $(FUZZ_TIMEOUT) node build/asan/wasm/lomn_fuzz_formats.js $(call FUZZ_ARGS,$(FUZZ_N_WASM)) --crash-dir build/asan/wasm/fuzz-crashes --last-input build/asan/wasm/last_input.txt | tee build/asan/wasm/fuzz_formats.txt || { echo "fuzz-formats wasm32 failed; last input started: $$(sed 's/ *$$//' build/asan/wasm/last_input.txt 2>/dev/null)"; exit 1; }
	@if [ "$(FUZZ_N)" = "$(FUZZ_N_WASM)" ]; then grep '^fuzz_formats: ' build/asan/native/fuzz_formats.txt > build/asan/native/fuzz_totals.txt && \
	  grep '^fuzz_formats: ' build/asan/wasm/fuzz_formats.txt > build/asan/wasm/fuzz_totals.txt && \
	  cmp build/asan/native/fuzz_totals.txt build/asan/wasm/fuzz_totals.txt && echo "fuzz-formats: native == wasm32 per-format totals"; fi

# M2a-1 T4 (A6): the differential run of the real Base.osi: Setup + 600 Process frames + the key taps, all natives typed
# stubs (D11) + the Beta 1.5.0 mod rows (D12). Native lomn_osi_run, wasm32 lomn_osi_run under node, and the independent
# Python oracle (own stubs, and replaying the native natives.log: the A9 path) must give byte-identical digests, and the
# full trace of Setup + frames 0-2 must be byte-identical. Prints the four cmp results and the summary of each leg.
# OSI_PERF=1 appends this run's A15 row (instructions/natives/script ms per frame, stack high-water) to perf/osi-a6.jsonl.
OSI_KEYS ?= down@100 down@160 enter@220 esc@280
OSI_FRAMES ?= 600
OSI_DIFF := build/osi-diff
OSI_TABLES := --natives data/osi/natives.json --mods data/osi/natives_beta150.json --types data/osi/types.json
osi-diff: native wasm $(VENV)/.ok
	@if [ -z "$(LOMN_DATA)" ]; then echo "osi-diff: LOMN_DATA is not set"; exit 2; fi
	rm -rf $(OSI_DIFF) && mkdir -p $(OSI_DIFF)
	set -o pipefail; timeout 300 build/native/lomn_osi_run "$(LOMN_DATA)/install/data/Base.osi" --frames $(OSI_FRAMES) --keys "$(OSI_KEYS)" \
	  --digest $(OSI_DIFF)/native.digest --natives-log $(OSI_DIFF)/native.natives.log --trace $(OSI_DIFF)/native.window --trace-frames -1:2 \
	  --perf-out $(OSI_DIFF)/osi-a6.jsonl --commit "$$(git rev-parse --short HEAD)" | tee $(OSI_DIFF)/native.summary
	set -o pipefail; $(EMSDK_ENV) && timeout 600 node build/wasm/lomn_osi_run.js "$(LOMN_DATA)/install/data/Base.osi" --frames $(OSI_FRAMES) \
	  --keys "$(OSI_KEYS)" --digest $(OSI_DIFF)/wasm.digest --trace $(OSI_DIFF)/wasm.window --trace-frames -1:2 | tee $(OSI_DIFF)/wasm.summary
	set -o pipefail; timeout 600 $(PY) tools/research/osi_vm.py --run "$(LOMN_DATA)/install/data/Base.osi" $(OSI_TABLES) --frames $(OSI_FRAMES) \
	  --keys "$(OSI_KEYS)" --digest $(OSI_DIFF)/python.digest | tee $(OSI_DIFF)/python.summary
	timeout 600 $(PY) tools/research/osi_vm.py --run "$(LOMN_DATA)/install/data/Base.osi" $(OSI_TABLES) --frames $(OSI_FRAMES) \
	  --keys "$(OSI_KEYS)" --replay $(OSI_DIFF)/native.natives.log --digest $(OSI_DIFF)/python_replay.digest > $(OSI_DIFF)/python_replay.summary
	timeout 600 $(PY) tools/research/osi_vm.py --run "$(LOMN_DATA)/install/data/Base.osi" $(OSI_TABLES) --frames $(OSI_FRAMES) \
	  --keys "$(OSI_KEYS)" --trace $(OSI_DIFF)/python.trace > /dev/null
	awk '/^K      3 /{exit} {print}' $(OSI_DIFF)/python.trace > $(OSI_DIFF)/python.window
	@cmp $(OSI_DIFF)/native.digest $(OSI_DIFF)/wasm.digest && echo "cmp 1/4 digest native == wasm32: OK"
	@cmp $(OSI_DIFF)/native.digest $(OSI_DIFF)/python_replay.digest && echo "cmp 2/4 digest native == Python (replaying the native natives.log): OK"
	@cmp $(OSI_DIFF)/native.digest $(OSI_DIFF)/python.digest && echo "cmp 3/4 digest native == Python (own stubs): OK"
	@cmp $(OSI_DIFF)/native.window $(OSI_DIFF)/python.window && cmp $(OSI_DIFF)/native.window $(OSI_DIFF)/wasm.window && \
	  echo "cmp 4/4 full trace Setup + frames 0-2 native == Python == wasm32: OK ($$(wc -l < $(OSI_DIFF)/native.window) lines)"
	@echo "summary (native):"; cat $(OSI_DIFF)/native.summary
	@if [ -n "$(OSI_PERF)" ]; then cat $(OSI_DIFF)/osi-a6.jsonl >> perf/osi-a6.jsonl && echo "appended to perf/osi-a6.jsonl"; fi

# M2a-2 T1 (A8): the coverage report of a typed-stub run of the real Base.osi in the Python oracle: which natives were hit,
# by tier (tools/osi/coverage.py; tiers come from data/osi/native_overrides.json once it exists, else every exe row is a
# stub). OSI_COV_FRAMES=0 reports Setup only; the default is the A6 path (600 frames + OSI_KEYS). The trace goes to
# build/osi-coverage/ (untracked, about 15 MB). Light: about 10 s, no native or wasm build.
OSI_COV_FRAMES ?= 600
osi-coverage: $(VENV)/.ok
	@if [ -z "$(LOMN_DATA)" ]; then echo "osi-coverage: LOMN_DATA is not set"; exit 2; fi
	mkdir -p build/osi-coverage
	timeout 110 $(PY) tools/research/osi_vm.py --run "$(LOMN_DATA)/install/data/Base.osi" $(OSI_TABLES) --frames $(OSI_COV_FRAMES) \
	  --keys "$(OSI_KEYS)" --trace build/osi-coverage/python.trace > build/osi-coverage/python.summary
	timeout 60 $(PY) tools/osi/coverage.py --trace build/osi-coverage/python.trace

# M2a-1 T4: the conformance corpus traces of the C++ runner and the Python oracle, compared file by file with cmp.
# M2a-2: a program with "pending": "<task>" is not run by either runner (each prints `PENDING <group>/<id> (<task>)` and writes
# no trace), so the cmp loop never sees it; this target additionally requires the two PENDING lists to be identical, requires
# that neither side left a trace for a pending program, and prints the pending count. CONFORM_CORPUS / CONFORM_TRACE_DIR let
# tools/tests/test_osi_conform_pending.py run it over a temporary corpus.
CONFORM_CORPUS ?= tests/osi/conformance
CONFORM_TRACE_DIR ?= build/conform-trace
conform-trace-cmp: native $(VENV)/.ok
	rm -rf $(CONFORM_TRACE_DIR) && mkdir -p $(CONFORM_TRACE_DIR)
	timeout 120 $(PY) tools/osi/conform_assemble.py $(CONFORM_CORPUS) $(CONFORM_TRACE_DIR)/osi
	set -o pipefail; timeout 300 build/native/lomn_osi_conform $(CONFORM_CORPUS) --osi-dir $(CONFORM_TRACE_DIR)/osi --trace-dir $(CONFORM_TRACE_DIR)/cpp | tee $(CONFORM_TRACE_DIR)/cpp.out
	set -o pipefail; timeout 300 $(PY) tools/research/osi_vm.py --conform $(CONFORM_CORPUS) --trace-dir $(CONFORM_TRACE_DIR)/py | tee $(CONFORM_TRACE_DIR)/py.out
	@d=$(CONFORM_TRACE_DIR); mkdir -p $$d/cpp $$d/py; n=0; bad=0; for f in $$( (cd $$d/cpp && find . -name '*.trace'; cd ../py && find . -name '*.trace') | sort -u); do \
	  n=$$((n+1)); cmp -s "$$d/cpp/$$f" "$$d/py/$$f" || { bad=$$((bad+1)); echo "DIFF $$f"; }; done; \
	  echo "conform-trace-cmp: $$((n-bad))/$$n traces byte-identical (C++ vs Python)"; \
	  { grep '^PENDING ' $$d/cpp.out || true; } | sort > $$d/cpp.pending; { grep '^PENDING ' $$d/py.out || true; } | sort > $$d/py.pending; \
	  pbad=0; cmp -s $$d/cpp.pending $$d/py.pending || { pbad=1; echo "PENDING-DIFF the runners disagree on the pending programs:"; diff $$d/cpp.pending $$d/py.pending; }; \
	  while read -r _ prog _; do for side in cpp py; do [ ! -e "$$d/$$side/$$prog.trace" ] || { pbad=1; echo "PENDING-TRACE $$side/$$prog.trace"; }; done; done < $$d/cpp.pending; \
	  echo "conform-trace-cmp: $$(wc -l < $$d/cpp.pending | tr -d ' ') pending (no trace on either side)"; [ $$bad -eq 0 ] && [ $$pbad -eq 0 ]

check: test-tools test-native test-wasm test-web
	@if [ -n "$(LOMN_DATA)" ]; then $(MAKE) datacheck datacheck-wasm a13-real; else echo "datacheck, a13-real SKIPPED (no LOMN_DATA)"; fi
	@if [ -d gen/decomp ]; then $(MAKE) worklist; else echo "worklist SKIPPED (run make decomp first)"; fi
	@echo "make check: all layers green"
