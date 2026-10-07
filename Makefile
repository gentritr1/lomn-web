# lomn-web top-level entry points. Recipes use TABs.
SHELL := /bin/bash
-include .env.local
export LOMN_DATA LOMN_CACHE
VENV := .venv
PY := $(VENV)/bin/python
EMSDK_ENV := source .toolchain/emsdk/emsdk_env.sh >/dev/null 2>&1
.PHONY: setup check-data decomp worklist native wasm site test-tools test-native test-wasm test-web check perf datacheck

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

check: test-tools test-native test-wasm test-web
	@if [ -n "$(LOMN_DATA)" ]; then $(MAKE) datacheck; else echo "datacheck SKIPPED (no LOMN_DATA)"; fi
	@if [ -d gen/decomp ]; then $(MAKE) worklist; else echo "worklist SKIPPED (run make decomp first)"; fi
	@echo "make check: all layers green"
