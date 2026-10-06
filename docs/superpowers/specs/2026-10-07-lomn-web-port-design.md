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

- **Copyright.** Never host, bundle, cache server-side or upload game data or the original exe. The reconstructed engine is a derivative of LEGO/Saffire code: same posture as isle-portable (non-commercial, no assets, takedown-ready). LEGO's tolerance is revocable. **User acknowledgement of this risk is required before any public deploy.**
- **Target content:** vanilla **Beta 1.5.0** first. REBUILT is out of scope until a later spec (it depends on LOMNHook mod natives such as `BetterSaver`).
- **Canonical binary:** `LOMN-Beta/native/LEGOBionicle.exe`, SHA-256 `09b55e62f78824607e31433ddfcea827ec836523afd8de3fcc9f3f3f4ec70f0f`. The installer's exe differs by 798 bytes (community patches); M0 classifies them, and each fix becomes a source-level flag.
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
- **Keep the original semantics until parity passes:** the `CPoolHall` pool allocator (scripts may depend on reuse), 32-bit pointers (**wasm32 only**; OSI stores handles as 32-bit ints), x87 behaviour handled per §4.5. Refactors wait until a trace passes.

### 4.2 Platform layer
- **SDL3** for window, keyboard/mouse/gamepad, audio (later) — native and Emscripten.
- **`rhi.h`**: a small render interface with two backends: **WebGPU** (webgpu.h C API; browser via Emscripten `--use-port=emdawnwebgpu`, native via **Dawn**, both pinned to **one Dawn revision**; never Emscripten's legacy `-sUSE_WEBGPU`) and **null** (records calls; lets loaders and the shim run in tests without a GPU).
- **D3D8-subset shim** on top of `rhi.h`: implements only the `IDirect3D8`/`IDirect3DDevice8`/texture/buffer methods the game calls. M0 enumerates them from the vtable call sites. `CheckDeviceFormat`/`GetDeviceCaps` answer deterministically; per Fable the game probes `D3DFMT_D15S1/D24S8/D24X8` (UNVERIFIED by me), so depth = `depth24plus-stencil8`.
- **Fixed-function → WGSL generator** (custom; nothing exists for D3D8→WebGPU, and DXVK-style projects target Vulkan, so we borrow semantics only):
  - Pipeline cache key = blend, depth/stencil, cull, vertex layout, texture-stage combiner enums only.
  - Alpha test, fog, lighting and light count are **uniform-driven branches**, which keeps the pipeline count to dozens, not thousands.
  - Pipelines are pre-warmed from a recorded state log.
- **Uniforms/vertices:** one per-frame ring buffer with 256-byte-aligned dynamic offsets; bind groups cached per texture. Consecutive draws merge when `ScDxStates` reports no state change, since that cache is the natural batching boundary. `DrawPrimitiveUP` (HUD) streams into a ring vertex buffer.
- **Textures:** upload BC1–3 directly when the device has `texture-compression-bc`; otherwise CPU-decode DXT → RGBA8 (textures are small). Never encode ASTC.
- **D3DX:** reimplement the 7 closed-source helpers. `MatrixLookAt`/`PerspectiveFov` must be bit-faithful, validated against recorded outputs. `.x` parsing (text + binary) replaces `d3dxof`/`D3DXLoadSkinMeshFromXof`. Images use `stb_image` (DDS via own small loader).
- **Later milestones:** libavcodec for Bink and AVI, `Gc3DSound` mapped to WebAudio (EAX reverb approximated), printing → PNG download.

### 4.3 Files: BYO loader and VFS
- Player picks the game folder (`<input webkitdirectory>`, drag-drop, or File System Access API).
- **Validation is cheap:** SHA-256 of the exe and the `.blk` headers only, not 1.1 GB. Unknown builds are rejected with a clear message.
- Files are copied once into **OPFS** in a worker, with a progress bar. A manifest marks completion, so revisits skip the copy.
- **Sync I/O solution:** the engine keeps its synchronous file API over an **in-memory VFS**. Before boot, the bridge prefetches the boot set (`Main.blk`, `Base.osi`, language). Before each area switch, it prefetches that area's files asynchronously behind the game's own loading transition.
  - **Rejected:** Asyncify (≈2× code size, slow) and engine-in-worker (OffscreenCanvas WebGPU support uneven).
  - The per-area prefetch manifest is **generated** by logging VFS accesses in the native build.
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
- Build: `-O2` + `wasm-opt`, single module, initial memory 128–256 MB with growth.
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

Five layers. Each later layer needs the previous ones green.

| Layer | What | Runs where | Who runs it |
|---|---|---|---|
| L0 unit | Loaders, parsers, D3DX math, shader generator | native, `ctest` | Claude, every change (related tests) |
| L1 contract | One `.x` from the VFS → shim → null backend (call log) and Dawn offscreen → PNG | native headless | Claude |
| L2 deterministic replay | Engine in fake-delta mode with an input log: `--frames N --shot k… --trace` → PNGs + `metrics.json` + `trace.jsonl` | native headless | Claude |
| L3 browser | Playwright with **one reused browser**: load the build, feed the fixture folder through the real BYO input, wait on `window.__lomn.state`, screenshot, read in-engine metrics, assert **zero game-data network requests** | Chromium (+ WebKit when supported) | Claude |
| L4 parity vs original | Same input log replayed in the original game in fake-delta mode → reference frames/traces; compare state traces first, SSIM second | Original game on user's machine | **User launches** the capture; Claude compares |

- **Metrics rules:**
  - Every fps/frame-time number is printed with its reconciliation (`frames counted` vs `window × expected rate`).
  - The engine reports a script / physics / render / submit breakdown.
  - **Thresholds are set only after the first measurement**, never invented in advance.
- **Fixture data:** env var `LOMN_DATA` points at the user's local Beta 1.5.0 folder. It is never committed (`.gitignore` blocks `*.exe`, `*.blk`, `/fixtures/`, …).
- **Per-function differential tests** (original x86 function in a sandboxed CPU emulator vs our C++) are **optional and need the user's OK**, because they execute original code. The preferred later oracle is the LOMNHook trace mod.

## 8. M0 and M1: scope and acceptance

### M0 — Foundations

| # | Deliverable | Acceptance (observable) |
|---|---|---|
| 0.1 | `tools/decomp/`: cv.py, cvtypes2.py, Ghidra scripts, x87 post-pass, metrics; `make decomp` | From the canonical exe, one command produces the corpus (one file per original source path), `include/original/*.h`, `worklist.json`; a second run is byte-identical; printed metrics ≥ probe numbers (5,429 named, 473 structs, 1,817 members, 580 procs). |
| 0.2 | Header generator | `include/original/*.h` compiles standalone with clang (`-fsyntax-only` on every header). |
| 0.3 | D3D8 + import surface inventory | Table of every `IDirect3DDevice8`/`IDirect3D8` vtable slot called (address + caller count), and every import; committed as `docs/surface.md`. |
| 0.4 | Build skeleton: CMake, pinned emsdk + one Dawn revision, SDL3; `rhi.h` with WebGPU + null backends | The same `rhi` code draws a triangle natively (headless PNG, centre pixel = expected colour) and in the browser (Playwright screenshot, same check). |
| 0.5 | Frozen interfaces: `IFileSystem` (sync read, `Root/` + `User/` mounts), `rhi.h`, platform time/input | Header review by Claude; L0 tests use the null backend + memory VFS. |
| 0.6 | Deterministic loop skeleton | `--frames 600 --shot 0,300,599` twice gives identical PNGs and traces; `metrics.json` frame count = 600. |
| 0.7 | Harness: native runner + Playwright runner (one reused browser, state waits, no fixed sleeps) + trace/SSIM compare | Each runner exits non-zero on the first missing element; all CLI commands carry timeouts. |
| 0.8 | Exe patch classification (repo exe vs installer exe, 798 bytes) | Each changed byte range mapped to a function and a named fix flag in `docs/patches.md`. |
| 0.9 | Work queue + dashboard | `worklist.json` lists every function: address, name, original file, status (`decompiled` → `cleaned` → `ported` → `tested`); a static HTML page renders the counts. |

### M1 — Level viewer (first visible result)

| # | Deliverable | Acceptance (observable) |
|---|---|---|
| 1.1 | BYO loader page: folder pick / drag-drop → validation → OPFS copy with progress → manifest | On the fixture folder: unknown build rejected with a message; known build accepted; second visit skips the copy; Playwright network log shows **0 requests for game data**. |
| 1.2 | Memory VFS + per-area prefetch; manifest generated by native access logging | Boot set + one area load from OPFS → VFS without blocking the main thread for longer than one frame (measured, reported). |
| 1.3 | BLK reader (reconstructed), `.x` (text + binary) parser, DDS/TGA/BMP/JPG loaders, `D3DXLoadSkinMeshFromXof` + `CreateTextureFromFileInMemory[Ex]` reimpl | All `.x` files referenced by lev1 parse (count reported = count found); 0 unresolved texture references; L0 tests per format. |
| 1.4 | Area composition: which files make up an area, taken from the reconstructed area loader (no guessing) | Every lev1 area loads its full object list; the count matches the decomp's loader logic. |
| 1.5 | D3D8 shim + WGSL generator + pipeline cache + ring buffers + BC/RGBA texture policy | L1 contract test green; pipeline count and draw-call count per frame reported. |
| 1.6 | Viewer UI: pick level → area, free-fly camera, wireframe/texture toggle | **User eyeball acceptance:** side-by-side screenshots of 3 lev1 areas vs the original game (user-captured, approximately matching viewpoint), approved by the user. fps reported with reconciliation on the user's Mac in Chrome; no fps threshold until measured. |
| 1.7 | Preview deploy (optional, gated) | Only after the user explicitly OKs deploying and the copyright posture (§3); headers verified with `curl -sI`. |

## 9. Roadmap after M1 (each gets its own spec)

- **M2:** OSI VM (`ScOSIVirtualMachine`, 109 methods) runs `Base.osi` to the main menu: fonts, HUD, input.
- **M3:** one playable area: character controller, collision, camera, BKD animation. Parity via L4 traces; LOMNHook trace mod.
- **M4:** audio, Bink/AVI video, saves (OPFS + export).
- **M5:** full progression, bosses, AI.
- **M6:** release hardening: browser matrix, performance, public URL.
- **M7 (optional):** REBUILT content, gamepad polish, touch controls.

## 10. Work split (Claude subagents, M0–M1)

Four streams, with interfaces frozen in M0 (0.5) before parallel work starts:

1. **Decomp + headers + work queue** (0.1, 0.2, 0.3, 0.8, 0.9)
2. **RHI / shim / platform** (0.4, 1.5)
3. **VFS / BYO / loaders** against `rhi.h` + `IFileSystem` (1.1–1.4)
4. **Harness / CI** (0.6, 0.7) and integration of 1.6

**Integration contract test:** one `.x` from the VFS → shim → headless PNG. It must pass before streams 2 and 3 merge. Claude reviews every unit by re-running its acceptance check (not by reading the diff) and owns all merges.

## 11. Risks

| Risk | Mitigation |
|---|---|
| Decompiled code for untyped classes (e.g. `GcCollisionCylinderSystem`) is slow to clean | Work queue prioritises by milestone need; layouts inferred from constructors/usage are recorded back into the headers. |
| Float drift breaks physics parity | §4.5 rules; trace tolerances measured; deterministic mode. |
| WebGPU feature gaps (Safari/Firefox) | Chromium first; WebKit in L3 when available; null backend keeps logic testable. |
| OPFS quota / first-load time (~1 GB) | Measure in M1; pre-bake only if needed; clear progress UI. |
| Legal takedown | No assets, non-commercial, isle-portable posture, user-acknowledged before deploy. |
| Reference captures need the user | Keep them few and scripted; one command per scenario. |
