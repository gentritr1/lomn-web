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

### 3.1 What M1b built and measured (2026-10-07, REF4; Chromium + Apple Metal)

- **Read-driven prefetch instead of a shipped manifest.** The engine's synchronous `fs` runs over a `PrefetchFileSystem`
  (`engine/include/lomn/prefetch_fs.h`): an index of every file (path, size; no bytes) plus resident byte ranges. A load
  runs in rounds; each read whose bytes are not resident fails as a recorded *miss* (the loaders skip and continue), JS
  fetches the misses straight into heap buffers (8 MiB chunks: `reserve` -> `HEAPU8.set` -> `commit`), and the load runs
  again until a round has no miss. 8 rounds for a real area (header -> table -> area header -> table -> SLBs/OSI ->
  `.x` -> textures -> clean), ~20–43 ms of probing; a revisit replays the cached set in 1 round. The last round's reads
  equal the native access log exactly (vllg/ghut/crss + both synthetic areas, compared line by line).
  - Why not ship the manifest: it depends on the data, and the accepted-build list identifies builds by exe hash; the
    data behind the canonical exe could not be checked, so "identical for every accepted build" is unprovable. The
    engine derives the same sets from the player's own files instead, and nothing derived from game data is shipped.
- **Boot set = the reads common to all areas**: the Art.blk header + table and Base.osi, 632,334 bytes, against the
  census's whole-file 167.4 MB (Art.blk is 166.8 MB but only its 146 KB table is read). Main.blk and lang_eng.blk are
  not read by the viewer.
- **Validation = SHA-256 of the exe + a fingerprint of every `.blk` entry table** (`web/app/fingerprint.js`, oracle
  `tools/byo_fingerprint.py`): 189 tables, 643,748 bytes, 323 ms in the page. The exe hash alone cannot tell modified data.
- **OPFS copy measured**: 505 files, 1.10 GB in 7.3 s (151 MB/s) through `createSyncAccessHandle` in a worker; the
  quota check (`persist()` + `estimate()`) runs before the first byte is written.
- **Pixels**: browser (canvas `bgra8unorm`, Depth32Float surface depth) vs native offscreen (`rgba8unorm`) = 0 differing
  pixels at 1280x720 on every compared area. Screenshot on whole CSS pixels: a canvas at a fractional y gave a 721-row
  capture (layout with integer line heights fixed it). A CSS `display:` rule on an element beats the `hidden`
  attribute: a "hidden" loading overlay dimmed the canvas to 55 % until `[hidden]{display:none!important}`.
- **Memory**: the wasm heap's high-water mark is the largest texture set (lev0/frnt: 83.06 MiB); INITIAL_MEMORY 96 MiB
  makes growth (a heap copy without pthreads) not happen in normal use.
- **A "flat counts across switches" leak check can pass while nothing is released**: alternating two areas keeps the
  union of both read sets resident, which is also flat. The mutation "never release on switch" survived it; the check
  that bites is *resident bytes == this area's own read set* after every switch. Same for any cache: compare with the
  exact expected content, not with the previous visit.

- **Content-hashed names need the module glue to stop fetching by name.** `build_site.mjs` publishes every JS/wasm file
  as `<name>.<10 hex>.<ext>` and rewrites the quoted specifiers (leaves first). Emscripten 6.0.11's glue ignores a
  `wasmBinary` module option (`var wasmBinary;` is never assigned: the request 404'd on the hashed site); passing
  `instantiateWasm` with bytes the page fetched itself works, costs no second download, and gives the page the SHA-256
  of exactly the wasm it runs (`metrics.build`, which the tests compare with the build directory's file).
- **Every zero needs a control in the same build**: the pixel gate reports a null (same build, two fresh pages: 0) and a
  positive control (fog off: 424,494 pixels over the tolerance on levt/stat); network and leak checks are shown to fire
  on one foreign request and one leaked texture.
- **Responsive canvas**: CSS `aspect-ratio: 16/9` gives x.5 px heights; set the canvas height to a whole pixel from its
  width, or screenshots resample. Flex bars that wrap need integer control heights for the same reason.

### 3.1b Revision 2 (Fable review, 2026-10-07): what a public URL needs that the tests did not show

Each fix below was shown failing on the old code first (a red run), then fixed.
- **`Release` is not `Destroy` in the browser.** emdawnwebgpu's `wgpuTextureRelease` drops the JS wrapper; the GPU
  memory waits for garbage collection. An init-script hook counting `GPUDevice.createTexture` minus
  `GPUTexture.destroy` measured 174 -> 1,238 live textures (≈ 1.0 GB of texture bytes) over 30 real area switches
  before the fix, flat after it. Call `wgpuTextureDestroy`/`wgpuBufferDestroy` before Release, and defer them to after
  the submit when a frame is open (a destroyed object in a pending command buffer fails validation).
- **Device loss must reach the page.** Logging the lost/uncaptured-error callbacks leaves a frozen canvas with a ticking
  HUD. Forward the first one to the page (state `error` + Reload) and cancel the main loop. Test it with CDP
  `Browser.crashGpuProcess` (the old page stayed `area-ready`); natively a WGSL that does not compile fires the
  uncaptured-error path.
- **A reconciliation's expectation must not come from the samples it checks.** Snapping 1000/p50 to a refresh rate turns
  a steady 48 fps on a 60 Hz display into "48 Hz, no drops". Measure the display rate once, before any load (the empty
  frames), and compare against it; a 20 ms spin per frame must flag DROPPING (it did; the old rule called it clean).
- **CSP for a WebAssembly page** needs `script-src 'self' 'wasm-unsafe-eval'` (not `unsafe-eval`; link with
  `-sDYNAMIC_EXECUTION=0`) and `style-src 'unsafe-inline'` for an inline `<style>`. A CSP-blocked fetch may never reach
  Playwright's request events: record `securitypolicyviolation` so the network check still flags the attempt.
- **A mutation bite must be attributed.** With ~70 browser checks per run, "the run failed" is not "the mutation was
  caught": a loaded host's timing check or a build left mutated by the previous entry fails it too (both happened in the
  first r2 run). Name the check each mutation must break, and rebuild after restoring every engine mutation.
- **Revisits fail in ways first visits cannot**: a remembered folder moved, a copy missing a file, a copy interrupted
  before its completion manifest. Each must lead back to the folder choice with a reason, never to a dead `error`.

### 3.2 Prefetch manifest format (`lomn_view --manifest 1`, native; `make manifest`)

One JSON object, `format: "lomn-prefetch-manifest"`, `version: 1`:
- `files`: `[{path, size}]`, every file any range reads (normalised path `root/data/...`, size in bytes);
- `boot`: `{ranges, reads, bytes}`, what `view::boot_reads` reads (the Art.blk header + table, Base.osi);
- `areas`: `[{area: "lev1/vllg", ok, reads, bytes, bytes_beyond_boot, files, ranges}]`, one per `<lev>/<area>.blk`;
- `ranges` entries are `[file_index, offset, length]`, distinct and sorted; a whole-file read is `[f, 0, size]`;
  zero-length reads are not listed (they move no bytes; the browser's `touched()` cannot list them either; only `lev4/haka.blk`, an empty entry table, has one on Beta 1.5.0);
- `summary`: area count, failures, the largest area (bytes, reads), the union of all ranges, the ranges common to every
  area and whether they equal the boot set, and the census boot set (whole files) for comparison.
The manifest holds names, offsets and sizes of the player's files: it is written under `build/` and never committed or
shipped. In the browser the same per-area sets come from the engine's own reads (`lomn_fs_touched`) and are cached in
IndexedDB under (data fingerprint, wasm SHA-256, area); every cached range is re-checked against the file index.

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
- **What Playwright cannot drive (M1b):** `showDirectoryPicker` (a native dialog) and a real permission prompt. Drive the
  code after them instead: hand the page an OPFS `FileSystemDirectoryHandle` (the type the picker returns) and stub
  `FileSystemHandle.prototype.queryPermission` for the re-grant branch; say in the report which part stays UNVERIFIED.
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

## 8. Parsing untrusted game files (from M1a-1; evidence in `audits/2026-10-07-m1a1-parser-security.md`)

- **Fidelity and robustness are different properties, and they need different tests.** Independent oracles over the real install proved the readers byte-faithful (39/39), and ASan over the whole install was clean. Neither could see that a 672 KB crafted blockfile aborts the browser: every real file is benign.
- **Attack every loop that an attacker-controlled count drives.**
  - Write a crafted time/memory probe for each such loop.
  - Run a sanitizer mutation fuzz natively **and** as wasm32 (32-bit `size_t`, exceptions off, 2 GB heap cap).
  - Quadratic copies and repeated decodes are the typical findings, not out-of-bounds reads.
- **Caps come from measured maxima, never from guesses.**
  - First measure the real maximum (e.g. the longest unterminated-name run, 2 bytes; the most `.x` objects in a file, 5,489).
  - Then set the cap with stated headroom and put the measurement in a comment next to the constant.
  - Re-run the oracles afterwards, so a cap can never silently reject real data.
- **Keep the public data model stable when hardening.** Bound the work (cap and reject) rather than changing a field's type that downstream tools read (`lookup_name` stayed a `std::string`).
- **Failed operations leave no partial state.** Build into locals and commit on success. A failed `open`/`enter_area` must not leave stale results visible.

- **Every new parser gets the audit, not just the first ones** (M1a-3: the OSI reader shipped in the reference without the M1a-1 caps; three reviews found an unbounded call list, per-record name copies in textures.slb (1.27 GB on wasm32 from 64 MiB) and per-method name copies). Count every attacker-controlled *accumulation*, not only loops. Evidence: `audits/2026-10-07-m1a3-osi-textures-parser-security.md`.
- **Cross-checks: a mismatch is a defect until traced through every use, and compare everything.** "Same bits, printed differently" hid a sign bug (`cond ? int32_t : uint32_t` decays to unsigned) that only an all-8,406-methods comparison exposed.
- **An oracle is independent only where it re-derives.** Constants it transcribes from the implementation fail together (the byte-swapped "sky1"/"watr" sort ids passed every test until the oracle was swept over all 123 areas). Cite such constants from the binary, and sweep the full corpus at least once per milestone (`make a9-sweep`).

## 7. Legal posture (INFERRED, not legal advice)

The precedent is the LEGO Island decomp → `isle-portable` web port: it hosts code only, users supply their own install, and no takedown was found as of 2026-10. Hosting game assets is a different and riskier act. A copyright holder's tolerance is revocable, so publish non-commercially and stay takedown-ready.
