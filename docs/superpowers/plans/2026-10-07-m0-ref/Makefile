# lomn-web top-level entry points. Recipes use TABs.
SHELL := /bin/bash
-include .env.local
export LOMN_DATA LOMN_CACHE
VENV := .venv
PY := $(VENV)/bin/python
EMSDK_ENV := source .toolchain/emsdk/emsdk_env.sh >/dev/null 2>&1
.PHONY: setup check-data decomp worklist native wasm site test-tools test-native test-wasm test-web check perf

setup: $(VENV)/.ok
	tools/toolchain.sh

$(VENV)/.ok: tools/requirements.txt
	python3 -m venv $(VENV) && $(VENV)/bin/pip install -q -r tools/requirements.txt && touch $@

check-data: $(VENV)/.ok
	$(PY) tools/lomn_data.py

decomp: check-data
	$(PY) tools/decomp/run_decomp.py --exe "$(LOMN_DATA)/reference/LEGOBionicle.exe"
	$(PY) tools/decomp/worklist.py build gen/decomp worklist/status.json
	$(PY) tools/decomp/worklist.py dashboard gen/decomp worklist/status.json docs/dashboard.html
	$(PY) tools/decomp/surface.py "$(LOMN_DATA)/reference/LEGOBionicle.exe" gen/decomp tools/decomp/data/d3d8_vtables.txt docs/surface.md
	$(PY) tools/census.py "$(LOMN_DATA)/install" gen/census && cp gen/census/census.md docs/census.md

worklist: $(VENV)/.ok
	$(PY) tools/decomp/worklist.py check gen/decomp worklist/status.json . --ctest-dir build/native

native:
	cmake -S engine -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build/native

wasm:
	$(EMSDK_ENV) && emcmake cmake -S engine -B build/wasm -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build/wasm

site: wasm
	rm -rf build/site && mkdir -p build/site && cp web/index.html build/wasm/lomn_web.js build/wasm/lomn_web.wasm build/site/

test-tools: $(VENV)/.ok
	$(EMSDK_ENV); $(VENV)/bin/pytest -q -rs tools

test-native: native
	ctest --test-dir build/native --output-on-failure --timeout 300

test-wasm: wasm native
	$(EMSDK_ENV) && ctest --test-dir build/wasm --output-on-failure --timeout 300
	$(PY) tools/harness/compare.py trace build/native/determinism/a/trace.jsonl build/wasm/determinism/a/trace.jsonl
	cmp build/native/determinism/a/calls.txt build/wasm/determinism/a/calls.txt

test-web: site
	cd web && timeout 180 node tests/smoke.mjs ../build/site ../build/pw-profile

perf: native
	build/native/lomn_native --backend webgpu --frames 600 --out build/perf
	$(PY) tools/harness/perf_history.py check build/perf/metrics.json --scene m0-triangle
	$(PY) tools/harness/perf_history.py append build/perf/metrics.json --scene m0-triangle

check: test-tools test-native test-wasm test-web
	@echo "make check: all layers green"
