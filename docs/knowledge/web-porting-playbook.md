# Web-porting playbook (from the LOMN port)

How to take a decompiled 2001-era DirectX 8 game to the browser on WebGPU, with players supplying their own files. Everything below was measured on 2026-10-07 (MacBook M1 Pro, macOS 26.5.2) unless marked INFERRED.

## 1. Route decision

| Route | When it wins | LOMN measurement |
|---|---|---|
| Emulate the original exe (Boxedwine/Wine → WebGL) | Low-poly 2D/DirectDraw titles; when native emulation already exceeds ~2× target fps | ~8 fps native on Mac. Rejected: the web build is slower. |
| Rebuild the engine in TS/JS reading the original formats | Simple engines whose logic lives in data/scripts | Rejected: physics, AI, camera and collision live in the exe |
| **Decompile + port (C++ → wasm32 + WebGPU)** | When the binary carries symbols/types (or a matching decomp exists) | Chosen: full NB11 debug info |

Probe each route cheaply first: a one-day emulation fps test, a one-week decompile-readability test. Let the numbers choose.

## 2. Toolchain pins and their traps (pins in `deps.lock`)

- **One Dawn release for both targets.**
  - Native uses the prebuilt `Dawn-<commit>-macos-latest-Release.tar.gz`.
  - Web uses the same release's `emdawnwebgpu_pkg` via `--use-port=<pkg>/emdawnwebgpu.port.py`, passed at both compile and link.
  - Never use the legacy `-sUSE_WEBGPU`.
- **Dawn's CMake package needs `find_package(Threads REQUIRED)` before `find_package(Dawn)`.** The target is `dawn::webgpu_dawn`. The real header is `include/dawn/webgpu.h`; `webgpu/webgpu.h` is a 33-line shim.
- **emdawnwebgpu requires C++ linking** (`em++`). Plain `emcc` errors with "requires C++".
- **Async device without Asyncify:**
  - `main()` requests adapter and device with `WGPUCallbackMode_AllowSpontaneous`, and the frame loop starts from the device callback.
  - The game's synchronous `CreateDevice` then just returns the held device.
  - Native does the same with `WaitAnyOnly` + `wgpuInstanceWaitAny`, and the instance needs the `TimedWaitAny` feature.
- **The browser surface texture must be released every frame** (`wgpuTextureRelease`) after the view is released.
- **Build render pipelines per target format**, lazily: the canvas format differs from offscreen `RGBA8Unorm`.

## 3. Bring-your-own files (copyright-safe distribution)

- **Chromium:** `showDirectoryPicker`; keep the handle in IndexedDB and read in place. No copy.
- **Safari/Firefox:** `<input webkitdirectory>` or drag-drop, then a one-time chunked copy to OPFS via `createSyncAccessHandle` in a worker, after a `navigator.storage.estimate()` quota check.
- **Validate cheaply:** SHA-256 the exe and block headers against an accepted-build list, not the whole 1.1 GB.
- **Keep the engine's synchronous file API** over an in-memory VFS that is prefetched per area behind the game's own loading transition. The manifest is generated from native access logs.
- **Paths:** normalise Windows-style (`\` → `/`, lower-case, fold `..` so nothing escapes a mount). `unmount_prefix` must have directory semantics, or `lev1/` also matches `lev10/`.

## 4. Determinism and the 64-bit vs wasm32 problem

- **Ship wasm32**, where the original 32-bit struct layouts hold. Generated headers carry `static_assert`s on size and offset for wasm32 only (473 headers, 1,770 asserts pass).
- **Make the same code correct on 64-bit native** (needed for fast tests):
  - Parse file bytes field by field, never `memcpy` them into structs that contain pointers.
  - Give script-visible handles a 32-bit handle table.
- **Run every suite twice:** natively, and as wasm32 under Node (null backend).
  - The deterministic loop's `trace.jsonl` and call log are byte-identical between arm64 and wasm32 (600 frames, `sim_time` 4.0000000968575478 at frame 599 on both).
  - Two Metal GPU runs produce byte-identical PNGs, so the same-machine SSIM baseline is 1.0.
- **Use the engine's own fixed step for tests** (`sFakeDeltaTime`), not 1/60.

## 5. Headless browser testing (WebGPU)

| Playwright config | Adapter | Canvas screenshot |
|---|---|---|
| default headless shell | none | n/a |
| headless shell + `--enable-unsafe-webgpu` | SwiftShader | **blank white while page state says "drawn"** (false positive) |
| `channel:'chromium'` + `--enable-unsafe-webgpu` | Apple Metal | correct (`ff8000` centre, `000040` corner) |

- **Always assert pixels and network**, never only a state flag.
- **Reconcile frame counts:** dt samples must equal frames − 1. Measured p50 dt was 16.70 ms (vsync 60 Hz).
- **`setInputFiles(dir)` on a `webkitdirectory` input passes paths, not bytes:** the 529-file, 1.10 GB install loads in 1.2 s, or 0.2 s with a persistent profile. (529 includes 2 macOS .DS_Store files; the game install itself is 527 files)
- **Every runner needs a negative test.** The smoke test fails in 1.5 s against an empty site.

## 6. Performance method

- Every measured run appends to `perf/history.jsonl`.
- A regression is a p50 change larger than noise **measured from accepted baseline runs**, `max(3σ, max−min)`. Never a hand-picked threshold.
- Print frame-count reconciliation next to every timing number.
- Optimise the biggest bucket of the script/physics/render/submit breakdown, and state the expected gain before the change.
- Known levers, in order, used only when a measurement points at them:
  - draw merging and pipeline pre-warm
  - OPFS pre-bake
  - `-O3`/`wasm-opt`
  - SIMD
  - streaming
  - threads (needs COOP/COEP)

## 7. Legal posture (INFERRED, not legal advice)

The precedent is the LEGO Island decomp → `isle-portable` web port: it hosts code only, users supply their own install, and no takedown was found as of 2026-10. Hosting game assets is a different and riskier act. A copyright holder's tolerance is revocable, so publish non-commercially and stay takedown-ready.
