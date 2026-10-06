# LOMN Web — design spec (architecture + milestones M0–M1)

Date: 2026-10-07 · Status: draft for user review · Owner: Claude (spec, review, git) · Implementers: Claude subagents only (no Codex)

## 1. Goal and success

**Goal.** A public URL where BIONICLE fans play *The Legend of Mata Nui* (2001, Saffire, cancelled) in a desktop browser, by loading **their own copy** of the game files. The site hosts only our engine; game data never leaves the player's machine.

**What "done" means for the whole project:** the Beta 1.5.0 content is playable start to finish in Chrome/Edge/Safari on desktop, matching the original game's behaviour as measured by the parity checks in §7. "100% working" is not promised; each milestone ships only when its observable acceptance checks pass.

**This spec details M0 (foundations) and M1 (level viewer).** Later milestones (§9) get their own spec → plan cycle once M1's measurements exist.

## 2. Evidence this design rests on

All VERIFIED in the 2026-10-06/07 probes unless marked.

| Fact | How verified |
|---|---|
| No engine source exists; game = `LEGOBionicle.exe` (Win32, Direct3D 8, built 2001-10-22) + ~1.1 GB data. Scripts (419 LSS → `Base.osi`) own puzzles/UI/cutscenes; movement, collision, AI, camera, animation, rendering live in the exe. | Repo inspection; Fable consult counted 730 distinct native functions called from scripts. |
| The exe embeds full CodeView NB11 debug info: 7,057 names, 505 classes, 37 line-table modules, 580 functions with named params/locals, ~4.8k class layouts, 675 object files / 728 original source paths. | Own parser (`cv.py`, `cvtypes2.py`). |
| Ghidra + our importer names 5,429 / 5,618 functions (96.6 %), 473 structs, 1,817 members; decompiled code is readable (e.g. `GcMudBoss::Burrow`). | Headless Ghidra 12.1.4 run; before/after metrics on 36 functions. |
| The exe imports only ~117 OS/DirectX functions: d3d8 `Direct3DCreate8` (rest via COM vtables), 7 D3DX helpers (incl. `D3DXLoadSkinMeshFromXof`), `DirectXFileCreate`, DirectInput8, DirectSound, 7 Bink, 3 IJL (JPEG), 8 AVIFile, printing (GDI/WINSPOOL), WINMM timers, 41 KERNEL32, 28 USER32. | PE import table parse. |
| The engine already has a deterministic fixed-step mode: `ScProcessManager::sFakeDeltaTime`, `sUseReal`, `UseRealDeltaTime(bool)`, `sCreaturePause`. | Symbol table. |
| Area loading is explicit (`SwitchAreaWithTransition`, area loaders); game paths use `Root/…` (and per Fable, `User/…`) prefixes. | Symbols + strings. |
| Running the original exe in an emulator is not viable on the web: ~8 fps on an M1 Pro **native** Boxedwine (web is slower). | User-run test, ffmpeg unique-frame count (237 frames / 21 s). |
| Precedent: LEGO Island → `isledecomp/isle-portable` (LGPL-3.0) with a web frontend; site hosts code only, users supply their install. No takedown found. | Fable consult (repo + searches); end-user upload flow INFERRED. |

## 3. Constraints and non-goals

- **Copyright.** Never host, bundle, cache server-side or upload game data or the original exe. The reconstructed engine is a derivative of LEGO/Saffire code: same posture as isle-portable (non-commercial, no assets, takedown-ready). LEGO's tolerance is revocable. **The user intends a fully public release (site + source), stated 2026-10-07.** Each publishing step (first deploy, making the repo public) is still confirmed by the user at that moment, together with the repo license choice.
- **Target content:** vanilla **Beta 1.5.0** first. REBUILT is out of scope until a later spec (it depends on LOMNHook mod natives such as `BetterSaver`).
- **Binaries and data are never in the repo.**
  - Every tool reads them from `$LOMN_DATA`, a local folder the user provides.
  - The **canonical decomp input** is the `LEGOBionicle.exe` from the LOMN-Beta GitHub repo's `native/` folder. SHA-256 `09b55e62f78824607e31433ddfcea827ec836523afd8de3fcc9f3f3f4ec70f0f`, stored at `$LOMN_DATA/reference/LEGOBionicle.exe`.
  - The **player's install** (Beta 1.5.0 installer) has an exe that differs by 798 bytes (community patches). Its SHA-256 is pinned in M0a, and classifying the patches happens later (§9).
- **Accepted builds:** the BYO validator accepts a fixed set of SHA-256s (canonical + installer 1.5.0). REBUILT and unknown builds are rejected with a message naming the build.
- **Platforms:** desktop browsers with WebGPU first. Mobile/touch is a non-goal until a later milestone (the game is keyboard/gamepad-centric).
- **Non-goals for M0–M1:** gameplay, OSI script VM, audio, Bink/AVI video, saves, mod support, public launch.
- **Process:** I never run the original exe myself; any run of the original game is user-launched (reference captures, trace mods).

## 4. Architecture

```
 Browser page (TS)                      Engine (C++17 → wasm32, also native macOS)
 ┌──────────────────────┐   manifest   ┌──────────────────────────────────────────────┐
 │ BYO loader: pick dir │─────────────►│ Platform layer (replaces Win32/DirectX)       │
 │ → validate → OPFS     │  area bytes  │  IFileSystem   rhi.h      input/time/audio    │
 │ prefetch per area     │─────────────►│  (memory VFS)  (WebGPU|null) (SDL3)            │
 └──────────────────────┘              │       ▲            ▲                           │
                                       │  D3D8-subset shim (Direct3DDevice8, ScDxStates)│
                                       │  D3DX reimpl (7 fns)  .x/DDS/TGA/BMP/JPG/BLK   │
                                       │       ▲                                        │
                                       │ Reconstructed game code, original file layout  │
                                       │  include/original/*.h generated from NB11      │
                                       └──────────────────────────────────────────────┘
```

### 4.1 Reconstructed code
- C++17, **functional (not byte-matching) reconstruction**, laid out by the original source paths (e.g. `src/Character/Bosses/GfMudBoss.cpp`). Matching isn't needed because the symbols are already our oracle for names; behaviour parity is proven by traces (§7).
- `include/original/*.h` is **generated** from NB11 (class layouts, enums, method declarations) before anyone writes code, so every implementer codes against the original names.
- **Keep the original semantics until parity passes:** the `CPoolHall` pool allocator (scripts may depend on reuse), and x87 behaviour handled per §4.5. Refactors wait until a trace passes.
- **Layout rule (resolves wasm32 vs 64-bit native):** the shipped target is **wasm32**, where the NB11 layouts hold. Generated `static_assert(sizeof(T) == <NB11 size>)` checks are compiled for the wasm32 target only. To make the same code also correct on the 64-bit native test build:
  - File bytes are always **parsed field by field**, never `memcpy`'d into structs that contain pointers.
  - Anything stored as a 32-bit integer by scripts or saves (OSI object handles) goes through a **32-bit handle table**, never a cast pointer.
  - Consequently every L0–L2 suite runs **twice**: natively, and as wasm32 under Node with the null backend (§7). A test that passes on only one of them is a failure.

### 4.2 Platform layer
- **SDL3** for window, keyboard/mouse/gamepad, audio (later) — native and Emscripten.
- **`rhi.h`**: a small render interface with two backends:
  - **WebGPU**, via the webgpu.h C API. The browser build uses Emscripten's `--use-port=emdawnwebgpu`, a remote port that needs **Emscripten ≥ 4.0.10** (Fable checked the port source). The native build uses **Dawn**. Both are pinned to **one Dawn release**; never Emscripten's legacy `-sUSE_WEBGPU`.
  - **null**, which records calls so loaders and the shim run in tests without a GPU.
- **Pins:** exact versions of emsdk, the Dawn release, SDL3, clang, CMake, Node and Playwright are recorded in `deps.lock` in M0b. CMake and the CI check refuse to build with anything else.
- **Device hand-off (no Asyncify):** WebGPU device creation is async, but the game's `Direct3DCreate8`/`CreateDevice` are synchronous.
  - JS requests the adapter and device **before `main()`** (Module `preRun`) and hands the device to the shim, which only wraps it.
  - Native does the same synchronously via Dawn.
  - M0b spikes this hand-off first, because importing a JS device through emdawnwebgpu is INFERRED, not yet proven.
- **D3D8-subset shim** on top of `rhi.h`: implements only the `IDirect3D8`/`IDirect3DDevice8`/texture/buffer methods the game calls. M0 enumerates them from the vtable call sites. `CheckDeviceFormat`/`GetDeviceCaps` answer deterministically; per Fable the game probes `D3DFMT_D15S1/D24S8/D24X8` (UNVERIFIED by me), so depth = `depth24plus-stencil8`.
- **Fixed-function → WGSL generator** (custom; nothing exists for D3D8→WebGPU, and DXVK-style projects target Vulkan, so we borrow semantics only):
  - Pipeline cache key = blend, depth/stencil, cull, vertex layout, texture-stage combiner enums only.
  - Alpha test, fog, lighting and light count are **uniform-driven branches**, which keeps the pipeline count to dozens, not thousands.
  - Pipelines are pre-warmed from a recorded state log.
- **Uniforms/vertices:** one per-frame ring buffer with 256-byte-aligned dynamic offsets; bind groups cached per texture. `DrawPrimitiveUP` (HUD) streams into a ring vertex buffer.
- **Draw merging** happens only when all three hold: identical render state (per `ScDxStates`), the same vertex/index buffer, and contiguous index ranges with the same transform. Anything else is a separate draw; the per-frame draw count is reported.
- **Textures:** upload BC1–3 directly when the device has `texture-compression-bc`; otherwise CPU-decode DXT → RGBA8 (textures are small). Never encode ASTC.
- **D3DX:** reimplement the 7 closed-source helpers. `MatrixLookAt`/`PerspectiveFov` must be bit-faithful, validated against recorded outputs. `.x` parsing (text + binary) replaces `d3dxof`/`D3DXLoadSkinMeshFromXof`. Images use `stb_image` (DDS via own small loader).
- **Later milestones:** libavcodec for Bink and AVI, `Gc3DSound` mapped to WebAudio (EAX reverb approximated), printing → PNG download.

### 4.3 Files: BYO loader and VFS
- **Chromium (File System Access API):**
  - The player picks the folder with `showDirectoryPicker`.
  - The handle is stored in IndexedDB, and permission is re-requested on revisit.
  - **There is no copy**: files are read straight from the player's folder.
- **Safari/Firefox fallback:**
  - Files arrive via `<input webkitdirectory>` or drag-drop and are copied once into **OPFS** in a worker.
  - The worker writes in chunks via `createSyncAccessHandle`, never whole-file `arrayBuffer()`, and shows a progress bar.
  - A manifest marks completion, so revisits skip the copy.
- **Quota first:** before any copy, call `navigator.storage.estimate()` and `persist()`. If free quota is below the data size (from the M0a census), stop with a visible message; never fail halfway.
- **Validation is cheap:** SHA-256 of the exe plus the `.blk` headers only, not 1.1 GB, checked against the accepted set (§3).
- **Sync I/O solution:** the engine keeps its synchronous file API over an **in-memory VFS**. Before boot, the bridge prefetches the boot set (`Main.blk`, `Base.osi`, language). Before each area switch, it prefetches that area's files asynchronously behind the game's own loading transition.
  - **Rejected:** Asyncify (≈2× code size, slow) and engine-in-worker (OffscreenCanvas WebGPU support uneven).
  - The per-area prefetch manifest is **generated** by logging VFS accesses in the native build.
  - Prefetch streams in 8–64 MB chunks directly into the wasm heap, avoiding a 2× peak from JS buffer → `HEAPU8.set`.
  - On an area switch, the previous area's buffers are released, except files shared with the next area.
- Mounts: `Root/` → game data (read-only), `User/` → saves/settings in OPFS (export/import later).
- **No pthreads through M3:** no COOP/COEP headers needed, Safari-safe; audio uses an AudioWorklet.

### 4.4 Main loop and timing
- Reuse the original `ScProcessManager` loop.
- **Deterministic mode** = `UseRealDeltaTime(false)` with `sFakeDeltaTime`, whose initial value is read from the decomp, not chosen by us. Tests and reference captures use this mode.
- **Play mode** = real delta, driven by `requestAnimationFrame`; the tab hidden → `sCreaturePause`.

### 4.5 Float precision
- MSVC6 default x87 control word (53-bit precision; no `_controlfp` imports): doubles ≈ IEEE, but float temporaries stay 53-bit inside expressions.
- Where the decomp shows values kept in x87 registers across operations, compute in `double` and round at the store.
- `_ftol` = truncate.
- Exact x87 parity is a **hard limit**; parity checks use tolerances calibrated by measurement (§7).

### 4.6 Hosting (later public URL)
- Static files on Vercel. `index.html` uses `Cache-Control: max-age=0, must-revalidate` plus a Vercel CDN cache.
- wasm/js/css get content-hashed names (a stamp step after Emscripten), served `public, max-age=31536000, immutable`, Brotli.
- Verified after deploy with `curl -sI` per asset class (`x-vercel-cache`).
- Build: `-O2` + `wasm-opt`, single module.
- Memory: initial size = boot set + largest area + engine overhead, all measured by the M0a census, not guessed. `MAXIMUM_MEMORY=2GB`. Growth without pthreads copies the heap, so it should be rare.
- Licensing: `THIRD_PARTY.md` lists each dependency's license (SDL3 zlib, Dawn BSD-3, stb MIT/PD, zlib, Playwright Apache-2.0, later libavcodec LGPL, which carries a relinking obligation for a static wasm). The repo's own license is an **open question for the user** before it's published (isle-portable uses LGPL-3.0).
- **Deploy is outward-facing → requires explicit user OK at that time.**

## 5. Custom tooling (decided)

| Build custom | When | Why |
|---|---|---|
| NB11 → Ghidra + `include/original` header exporter | **Now (M0)** | Exists as probe scripts; becomes the foundation. |
| Fixed-function → WGSL generator + pipeline cache | **Now (M1)** | Nothing equivalent exists for D3D8→WebGPU. |
| Deterministic replay in-engine (fake-delta + input log, `--frames/--shot/--trace`) | **Now (M0)** | The test harness needs it; the engine already supports fake delta. |
| Per-function work queue + progress dashboard (one JSON) | **Now (M0)** | Drives subagent assignment, shows progress. |
| Trace-compare tool (state traces + SSIM) | **Now (M0, minimal)** | The parity oracle (§7). |
| OPFS pre-bake (parsed meshes, transcoded textures) | Later | Only if M1's measured load time demands it. |
| LOMNHook trace mod (logs args/returns in the real game) | Later (M3) | Parity oracle for gameplay; user-run. |
| Custom Boxedwine build with `BOXEDWINE_RECORDER` | Later, if needed | `-record/-play` are compiled out of release builds (Fable, from source). |
| Own x86 emulator, own FS layer, own video decoders, own windowing/audio | **Never** | Use Ghidra, the memory VFS, libavcodec, SDL3, Dawn, stb, zlib, Playwright, wasm-opt. |

## 6. Hard limits (cannot be engineered away)

- No exact x87 float parity.
- No EAX reverb; audio is approximate.
- No exclusive fullscreen and no vsync control (the browser schedules frames).
- Touch play needs bespoke controls.
- iOS memory (~1–1.5 GB) and OPFS quota limits.
- Firefox WebGPU coverage is uneven.
- DLL mods can't load; their fixes become flags instead.
- Behaviour can only be verified where the user runs the original game.
- Game data can never be shipped.
- LEGO's tolerance is revocable.
- Printing degrades to a PNG download.

## 7. Testing (replaces manual emulator runs + screen recordings)

Five layers. L0–L3 gate every milestone; **L4 is not a gate for M0/M1** (it needs a fake-delta hook inside the original game, which comes with the LOMNHook trace mod in M3).

| Layer | What | Runs where | Who runs it |
|---|---|---|---|
| L0 unit | Loaders, parsers, D3DX math, shader generator | native **and** wasm32-under-Node, `ctest` | Claude, every change (related tests only) |
| L1 contract | One `.x` from the VFS → shim → null backend (call log); plus Dawn offscreen → PNG (native only) | native headless + wasm32/Node (null) | Claude |
| L2 deterministic replay | Fake-delta mode + input log: `--frames N --shot k… --trace` → PNGs + `metrics.json` + `trace.jsonl` + null-backend call log | native headless + wasm32/Node (null) | Claude |
| L3 browser | Playwright, `launchPersistentContext` with a fixed profile dir (so run 2+ hits the manifest/handle and skips the copy). Feeds the fixture through the real BYO input (`setInputFiles` with one directory path; Fable checked the docs), waits on `window.__lomn.state` (no fixed sleeps), screenshots, reads `window.__lomn.metrics`, checks network. WebGPU launch flags are pinned by the M0b spike; results are GPU-dependent and reported with the GPU name. | Chromium (+ WebKit when its WebGPU works) | Claude |
| L4 parity vs original | Same input log in the original game in fake-delta mode → reference frames/traces; compare state traces first, SSIM second | Original game on the user's machine | **User launches**; Claude compares |

- **Metrics rules:**
  - Every fps/frame-time number is printed with its reconciliation (`frames counted` vs `window × expected rate`) and the engine's script/physics/render/submit breakdown.
  - **Thresholds are set only after the first measurement**, never invented in advance.
  - Image comparisons use an SSIM baseline measured first on the same machine (run twice, compare), because GPU readback is not guaranteed bit-identical.
- **Fixture data:** `$LOMN_DATA` (§3), never committed.
  - Tests that need it print `SKIPPED (no LOMN_DATA)` and return ctest skip code 77.
  - The **fixture-free subset** is what `make check` gates on.
  - There is no cloud CI holding game data. If the repo later goes public, GitHub Actions runs only the fixture-free subset.
- **Negative tests:** each runner is also run against an empty fixture folder and must exit non-zero within its timeout.
- **Per-function differential tests** (original x86 function in a sandboxed CPU emulator vs our C++) are **optional and need the user's OK**, because they execute original code. The preferred later oracle is the LOMNHook trace mod.

### 7.1 Optimization loop ("make it work, then keep making it faster")

1. **Correct first.** A change ships only when L0–L3 are green; speed never trades away a passing parity check.
2. **Measure, don't guess.**
   - Every L2/L3 run appends one line to `perf/history.jsonl`: commit, machine/GPU, scene, frames with reconciliation, p50/p95 frame time, the script/physics/render/submit breakdown, draw and pipeline counts, wasm size, first-load time, peak heap.
   - The dashboard (0.5) charts it.
3. **Target the biggest bar.** Each optimization names the breakdown bucket it targets and the expected change *before* it's built. It lands only if the re-measured number moves by more than run-to-run noise (noise measured by repeating the same run).
4. **Regression guard.** `make check` compares the current run with the last accepted entry for the same machine and scene, and fails on a regression larger than measured noise.
5. **Known levers, in order** (used only when a measurement points at them):
   - draw merging and pipeline pre-warm
   - texture/mesh pre-bake into OPFS
   - `-O3` vs `-O2` + `wasm-opt` passes
   - SIMD (`-msimd128`) for skinning/math
   - streaming tuning
   - threads (pthreads + COOP/COEP), only after M3 and only if the CPU bucket dominates

## 8. M0 and M1: scope and acceptance

Split into four plan-sized pieces: **M0a → M0b → M1a → M1b**. The first implementation plan covers **M0a + M0b only**; M1 is planned after M0's measurements (census, spikes) exist.

### M0a — Decomp foundation and facts (stream 1)

| # | Deliverable | Acceptance (observable) |
|---|---|---|
| 0.1 | `tools/decomp/` (cv.py, cvtypes2.py, Ghidra scripts, x87 post-pass, metrics); `make decomp`, cached by exe SHA-256 (the full rerun is a periodic check, not every build) | From `$LOMN_DATA/reference/LEGOBionicle.exe`: corpus (one file per original source path), `include/original/*.h`, `worklist.json`, `metrics.json`. A second run gives a **normalised** corpus (timestamps/addresses-of-analysis noise stripped) that is `diff`-identical, and an identical `metrics.json`. Metrics ≥ probe: 5,429 named, 473 structs, 1,817 members, 580 procs. |
| 0.2 | Header generator + layout asserts | `clang --target=wasm32 -std=c++17 -fsyntax-only` passes on every header; a ctest compiles generated `static_assert(sizeof(T)==N)` for every NB11 class (wasm32 only). |
| 0.3 | Import + D3D8 surface inventory | `docs/surface.md`: every import, and every `IDirect3D8`/`IDirect3DDevice8` vtable slot called (slot, method name, caller count, caller list). Spot check: 5 random rows re-verified in Ghidra by the reviewer. |
| 0.4 | M1 seed set | Call-graph reachability from the area loaders (`SwitchAreaWithTransition` et al.), BLK/`.x`/texture readers and the D3D8 call sites, tagged `m1-required` in `worklist.json`, with the count reported. |
| 0.5 | Worklist rules + dashboard | `tools/worklist check` enforces: `ported` ⇒ the source file exists; `tested` ⇒ a named ctest exists and passes. A static HTML page renders the counts. The check runs in every review. |
| 0.6 | Data census | `docs/census.md` from `$LOMN_DATA`: file counts and sizes by type, size of the boot set, size of each area's files, largest area. Feeds memory sizing (§4.6) and the quota check (§4.3). |
| 0.7 | Installer exe hash | SHA-256 of the player-install exe recorded in the accepted-build list. |

### M0b — Build, interfaces, harness (streams 2 + 4)

| # | Deliverable | Acceptance (observable) |
|---|---|---|
| 0.8 | Spikes, done first: (a) JS-acquired WebGPU device handed into emdawnwebgpu; (b) Playwright headless Chromium WebGPU launch flags; (c) `setInputFiles` directory + persistent profile | Each spike is a committed script whose output shows the success condition (device used for a clear-colour draw; `navigator.gpu` adapter name printed headless; file count seen by the page = file count on disk). |
| 0.9 | Build skeleton: CMake, `deps.lock` pins, SDL3; `rhi.h` with **null backend first**, then WebGPU (Dawn native + emdawnwebgpu) | The same `rhi` code draws a triangle natively (headless PNG, centre pixel = expected colour ±2/255) and in the browser (Playwright screenshot, same check). |
| 0.10 | Frozen interfaces: `IFileSystem` (sync read, `Root/`+`User/` mounts), `rhi.h`, platform time/input | The memory-VFS and null-backend L0 tests pass natively and under Node; Claude signs off the headers before streams 2/3 fork. |
| 0.11 | Deterministic loop skeleton | `--frames 600 --shot 0,300,599` run twice: `trace.jsonl` and the null-backend call log are byte-identical; PNG SSIM between runs ≥ the measured same-machine baseline; `metrics.json` frames = 600. |
| 0.12 | Harness: native runner, Playwright runner (one persistent context, state waits), trace/SSIM compare, `THIRD_PARTY.md` | Negative tests (§7) exit non-zero within timeout; every CLI call has an explicit timeout. |

### M1a — Native level viewer (streams 2 + 3)

| # | Deliverable | Acceptance (observable) |
|---|---|---|
| 1.1 | Memory VFS over `$LOMN_DATA` + native access logging → per-area prefetch manifest | Manifest lists every file each lev1 area opens; the boot set matches the census. |
| 1.2 | Reconstructed BLK reader, `.x` (text + binary) parser, DDS/TGA/BMP/JPG, `D3DXLoadSkinMeshFromXof`, `CreateTextureFromFileInMemory[Ex]` | Per lev1 area: `referenced` (from loader resolution), `parsed` (vertex count > 0) and `failed` lists; acceptance `failed == []`; L0 tests per format. |
| 1.3 | Area composition from the reconstructed area loader (seed set 0.4) | **Independent check:** the object/file list per area equals either a user-captured file-access log of the original game loading that area (user-run, one command) or the list from the level definition data. It is never compared against our own code. |
| 1.4 | D3D8 shim + WGSL generator + pipeline cache + ring buffers + BC/RGBA policy | L1 contract green on both null and Dawn; per-frame pipeline count and draw count reported. |
| 1.5 | Native viewer: pick area, free-fly camera, `--shot` | PNGs of 3 lev1 areas produced headless. |

### M1b — Browser viewer (streams 3 + 4)

| # | Deliverable | Acceptance (observable) |
|---|---|---|
| 1.6 | BYO page: Chromium handle path + Safari/Firefox OPFS fallback, quota check, validation, progress | Unknown/REBUILT build rejected with a named message; accepted build loads; revisit skips the picker/copy; quota-short case shows the message (simulated via the quota API). |
| 1.7 | Network discipline | Every request URL is in the built-asset manifest, and total response bytes ≤ build size (Playwright). |
| 1.8 | Browser viewer with `?hud=1` overlay showing fps + reconciliation + breakdown (also on `window.__lomn.metrics`) | **User eyeball acceptance:** side-by-side screenshots of 3 lev1 areas vs the original game (user-captured, approximately matching viewpoint), approved by the user. The fps readout is read by the user on their Mac in Chrome; thresholds are set after this measurement. |
| 1.9 | Preview deploy (optional, gated) | Only after the user explicitly OKs deploying, the copyright posture (§3) and the repo license; headers verified with `curl -sI`. |

## 9. Roadmap after M1 (each gets its own spec)

- **M2:** OSI VM (`ScOSIVirtualMachine`, 109 methods) runs `Base.osi` to the main menu: fonts, HUD, input.
- **M3:** one playable area: character controller, collision, camera, BKD animation. Parity via L4 traces + LOMNHook trace mod (user-run). Classification of the 798 patched bytes into fix flags happens here, earlier only if a patch touches the loaders.
- **M4:** audio, Bink/AVI video, saves (OPFS + export).
- **M5:** full progression, bosses, AI.
- **M6:** release hardening: browser matrix, performance, public URL.
- **M7 (optional):** REBUILT content, gamepad polish, touch controls.

## 10. Work split (Claude subagents)

1. **Decomp + headers + worklist + census** (M0a), which can start immediately.
2. **RHI / shim / platform**: null backend (0.9) first, then WebGPU, then the shim (1.4).
3. **VFS / BYO / loaders** (1.1–1.3, 1.6). Starts only after 0.9's **null backend implementation** and 0.10's frozen interfaces exist, not just the headers.
4. **Harness** (0.8 spikes, 0.11, 0.12, 1.7) and integration of 1.5/1.8.

**Integration contract test:** one `.x` from the VFS → shim → headless PNG must pass before streams 2 and 3 merge. Claude reviews every unit by **re-running its acceptance check** (not by reading the diff), runs `tools/worklist check`, and owns all merges.

## 11. Risks

| Risk | Mitigation |
|---|---|
| Decompiled code for untyped classes (e.g. `GcCollisionCylinderSystem`) is slow to clean | Work queue prioritises by milestone need; layouts inferred from constructors/usage are recorded back into the headers. |
| Float drift breaks physics parity | §4.5 rules; trace tolerances measured; deterministic mode. |
| WebGPU feature gaps (Safari/Firefox) | Chromium first; WebKit in L3 when available; null backend keeps logic testable. |
| OPFS quota / first-load time (~1 GB) | Chromium reads the player's folder in place (no copy); fallback copy only after a quota check; census-based sizing; pre-bake only if measured load time demands it. |
| Legal takedown | No assets, non-commercial, isle-portable posture, user-acknowledged before deploy. |
| Reference captures need the user | Keep them few and scripted; one command per scenario. |
| 64-bit native vs wasm32 behaviour differs | Layout rule (§4.1); every L0–L2 suite also runs as wasm32 under Node. |
| Toolchain churn (Dawn/emdawnwebgpu/WebGPU headers) | One pinned Dawn release for both targets in `deps.lock`; upgrades are separate reviewed changes. |
