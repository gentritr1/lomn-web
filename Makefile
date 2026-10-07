# lomn-web top-level entry points. Recipes use TABs.
SHELL := /bin/bash
-include .env.local
export LOMN_DATA LOMN_CACHE
VENV := .venv
PY := $(VENV)/bin/python
EMSDK_ENV := source .toolchain/emsdk/emsdk_env.sh >/dev/null 2>&1
.PHONY: setup check-data decomp worklist native wasm site test-tools test-native test-wasm test-web check perf datacheck datacheck-wasm fuzz-osi

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

worklist: $(VENV)/.ok
	timeout 1800 $(PY) tools/decomp/worklist.py check gen/decomp worklist/status.json . --ctest-dir build/native

native:
	timeout 600 cmake -S engine -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release && timeout 1800 cmake --build build/native

wasm:
	$(EMSDK_ENV) && timeout 600 emcmake cmake -S engine -B build/wasm -G Ninja -DCMAKE_BUILD_TYPE=Release && timeout 1800 cmake --build build/wasm

site: wasm
	rm -rf build/site && mkdir -p build/site && cp web/index.html build/wasm/lomn_web.js build/wasm/lomn_web.wasm build/site/

test-tools: $(VENV)/.ok
	$(EMSDK_ENV); timeout 1800 $(VENV)/bin/pytest -q -rs tools

test-native: native
	ctest --test-dir build/native --output-on-failure --timeout 300

test-wasm: wasm test-native $(VENV)/.ok
	$(EMSDK_ENV) && ctest --test-dir build/wasm --output-on-failure --timeout 300
	timeout 120 $(PY) tools/harness/compare.py trace build/native/determinism/a/trace.jsonl build/wasm/determinism/a/trace.jsonl
	cmp build/native/determinism/a/calls.txt build/wasm/determinism/a/calls.txt
	cmp build/native/a7/a/calls.txt build/wasm/a7/a/calls.txt
	cmp build/native/a9/a/state.txt build/wasm/a9/a/state.txt
	cmp build/native/a9/a/calls.txt build/wasm/a9/a/calls.txt

test-web: site
	cd web && timeout 60 node --test tests/serve_test.mjs
	cd web && timeout 180 node tests/smoke.mjs ../build/site ../build/pw-profile

perf: native $(VENV)/.ok
	timeout 300 build/native/lomn_native --backend webgpu --frames 600 --out build/perf
	timeout 120 $(PY) tools/harness/perf_history.py check build/perf/metrics.json --scene m0-triangle
	timeout 120 $(PY) tools/harness/perf_history.py append build/perf/metrics.json --scene m0-triangle

datacheck: native $(VENV)/.ok
	timeout 1200 $(PY) tools/research/oracle/check_datacheck.py --exe build/native/lomn_datacheck --install "$(LOMN_DATA)/install"

# M1a-3: the same oracle against the wasm32 datacheck under Node (NODERAWFS disk file system), then native == wasm32 on
# the real-data null logs of vllg / ghut / crss (call log and D3D8 state log).
datacheck-wasm: wasm native $(VENV)/.ok
	$(EMSDK_ENV) && timeout 1800 $(PY) tools/research/oracle/check_datacheck.py --exe build/wasm/lomn_datacheck_node.sh --install "$(LOMN_DATA)/install"
	$(EMSDK_ENV) && for la in lev1:vllg lev2:ghut lev4:crss; do l=$${la%%:*}; a=$${la##*:}; \
	  timeout 300 build/native/lomn_view --install "$(LOMN_DATA)/install" --level $$l --area $$a --backend null --out build/native/$$a.calls.txt --state-log build/native/$$a.state.txt >/dev/null && \
	  timeout 300 node build/wasm/lomn_view.js --install "$(LOMN_DATA)/install" --level $$l --area $$a --backend null --out build/wasm/$$a.calls.txt --state-log build/wasm/$$a.state.txt >/dev/null && \
	  cmp build/native/$$a.calls.txt build/wasm/$$a.calls.txt && cmp build/native/$$a.state.txt build/wasm/$$a.state.txt || exit 1; done; echo "native == wasm32: real-area null logs"

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

check: test-tools test-native test-wasm test-web
	@if [ -n "$(LOMN_DATA)" ]; then $(MAKE) datacheck datacheck-wasm; else echo "datacheck SKIPPED (no LOMN_DATA)"; fi
	@if [ -d gen/decomp ]; then $(MAKE) worklist; else echo "worklist SKIPPED (run make decomp first)"; fi
	@echo "make check: all layers green"
