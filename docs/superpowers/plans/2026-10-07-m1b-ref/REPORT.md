# REF4 (M1b browser viewer) — builder report, revision 2

## Revision 2 (after FABLE-REVIEW.md, READY WITH FIXES; controller rulings C1/C2/I1/I2)

Same scratch copy, same rules (no deploy, no network, -j4, no subagents). Every behaviour fix was first run on the
unfixed code (red, logs `logs/r2-red-*`), then fixed and re-run (green). Host loadavg 7–48 during r2 (other agents).

### Final verification on the revised tree (executed)
- `make check` (final): **green** (`logs/make-check-r2-final.log`, 4 min 28 s, loadavg 10.8 -> 9.8): pytest 74; native ctest 30/30; wasm ctest 7/7; native == wasm32 trace/A7/A9 logs (`cmp`); M0 smoke; web unit 14/14; byo_synthetic 73/73; datacheck 39/39 native + wasm32; real-area null/state logs native == wasm32; a13_real 59/59; worklist 0 errors.
- `a13_real` (in make check, sweep on by default): 59/59: vllg/ghut/crss/frnt 0 px vs native (9 comparisons, max delta 0); null 0 px on vllg/ghut/crss; positive control 155,532 px; I1 probe flat at 29 textures / 10.7 MB over 30 vllg<->frnt switches; sweep 122 areas in 12.9 s, max 8 rounds (cap 12), over-fetch 0 B, 0 heap growth from 100,663,296 B; fps 601 frames in 9.98 s vs 598.5–601.3 expected at the measured 60 Hz display, not DROPPING, dt p50 16.7 / p95 16.8–17.4 ms, frame CPU p50 0.4–0.6 ms; first visit / revisit load vllg 165 / 110, ghut 113 / 116, crss 174 / 150 ms; build Release -O3, wasm 332,642 B, SHA-256 == build dir.
- Pixel gate: tolerance 0; null (same build, two fresh pages) 0 px on levt/test, vllg, ghut, crss; native vs browser 0 px
  on every comparison (incl. 328x185 / 968x545 / 1280x720 after resizes); positive control fog off (`?debug=1&fog=off`):
  levt/stat 424,494 px over tolerance, lev1/vllg 155,532 px.
- Responsive sweep 360..1920 px (40 widths): no h-scroll, no overlap, min clearance 8 px (BYO page and viewer); every
  resize draws all draws, 0 rejected, no `[webgpu]` message.
- Mutations (`logs/mutations4-r2b.log`, 34 entries, 11 new for C1, C2, I1, I2, I3, I4, I6, M3): 33 CAUGHT, each on a
  targeted check (browser bites attributed via `EXPECT`), no-op control MISSED. The first r2 run (`mutations4-r2.log`)
  had two harness defects found by reading its logs (no wasm rebuild after restoring an engine mutation; node check
  missed `unit_r2_test.mjs`); fixed, full rerun.
- ASan+UBSan on the final engine (`logs/asan-run-r2.log`): ctest 30/30; 122-area manifest == Release; vllg/ghut/crss
  byte-identical to M1a-3; survey 123 total / 122 addressable / 0 failures; 0 B stderr.
- Task chain (`logs/chain/task-chain.log`): **PASS** — T1 71 s, T2 35 s, T3 59 s, T4 31 s, T5a 16 s, T5b 78 s, T6 77 s; each `make test-native test-wasm test-web` green over a fresh BASE (native 29 -> 30 tests from T3, wasm 7, M0 smoke pixels, byo_synthetic 73/73 from T5b); after T6 the tree equals the reference.

### Fixes, each red then green
| Item | Red (old code) | Green |
|---|---|---|
| C1 device loss | CDP `Browser.crashGpuProcess`: page stays `area-ready` (877 frames later) | `error` "GPU device lost (reason 1): …", Reload shown, loop cancelled. rhi additive `set_lost_handler` (null: log line; WebGPU: first non-Destroyed loss or first uncaptured error, delivered once, also if it happened before the handler). Native probe 6: a WGSL that does not compile reaches the handler once |
| C2 data policy (D13) | accepted exe + unknown tables rejected | soft-accept: persistent warning "Game data differs from the known build — rendering may differ (…fingerprint…)", `metrics.validation = {data: "unknown", warning}`; rejected-list exe, unknown exe, exe without `.blk`, empty folder still hard-reject |
| I1 Destroy | 30 real vllg<->frnt switches: live GPUTexture 174 -> 1,238 (≈ 1.0 GB of texture bytes left to GC); synthetic 0 of 154 destroyed | `wgpuTextureDestroy`/`wgpuBufferDestroy` before Release; inside an open frame deferred to `end_frame()` after the submit (probe 2 renders, probe 5 8 cycles green); real: 29 textures / 10.7 MB at every vllg, synthetic 150 of 154 destroyed. Driver-level memory plateau: owner check (Activity Monitor) |
| I2 reconciliation | snapped p50: a 20 ms spin at 60 Hz -> "48 Hz, expected 90.1 vs 90" = clean | display rate measured once before the first area (p10 of 90 empty-frame intervals); HUD "measured fps X vs display Y Hz" + DROPPING; spin of 1.5 periods (init script) and `?debug=1&debug_spin=N` both flag DROPPING. Under loadavg 28–45 headless Chromium ran at 30 Hz and the display measured 30 Hz (honest); the test sizes the spin from the measured rate |
| I3 moved folder | dead `error` "A requested file or directory could not be found" | `need-files` "The remembered folder … could not be read (moved or deleted)", picker shown, handle forgotten |
| I4 CSP | none | meta CSP (`script-src 'self' 'wasm-unsafe-eval'`, `connect-src 'self'`, …), `-sDYNAMIC_EXECUTION=0`; the foreign fetch is blocked by the browser and still flagged (securitypolicyviolation) |
| I5 rounds | rounds recorded for 4 areas only, sweep opt-in | sweep default; all 122 areas, fresh discovery: histogram {2:1, 3:4, 5:1, 6:107, 7:1, 8:8}, max 8 measured first -> asserted cap 12 (hard limit 16); over-fetch 0 B in every area |
| I6 incomplete copy | copy missing Base.osi loads (default fog) | manifest shape-checked + compared (`sameFiles`); mismatch -> cleared, `need-files` with the reason |
| I7 interrupted copy | manifest-less dir stays | removed at start, message |
| M2 | stale build keys stay; no cap | pruned at boot; cached sets > 64 MiB ignored |
| M3 | W with body focus moves the camera | canvas focus or pointer lock only |
| M4 | two picks -> 2 engines, uncaptured "TextureView … cannot be used with [Device]" | one engine; requestPointerLock rejection caught |
| M6 | manifest "loads" lev4/lev4c (= the level file) | manifest 122 rows + `not_addressable`; survey 123 total / 122 addressable (red: ctest `view_cli_manifest_addressable`) |
| M8 | — | empty folder -> rejected (green before and after) |
| M11/M12 | unit red | UTF-8 byte cap (shared `paths.js`), case duplicates skipped with a warning, worker re-checks paths |
| M13 | `?fog=off` honoured anywhere | needs `?debug=1` (also `debug_spin`, `fetch_concurrency`, `nocache`) |
| M14 | exact no-growth assert | <= one 16 MiB step, printed (measured growth 0) |
| M7 | — | documented (web/tests/README.md: cross-GPU procedure); pixel failures print the adapter |
| M1 (measure only) | — | 8 fetches in flight vs 1: median 532 vs 613 ms, spreads 329 / 236: within noise, not adopted |
| M5, M9 | — | M5 recorded as reviewed; M9 = the mutation + ASan reruns above |

### Intermediate failures (kept in logs, all resolved)
- `make-check-r2.log` (first r2 run): I2 failed because the loaded host (loadavg 28–45) ran headless Chromium at 30 Hz
  and the test assumed 60 Hz -> the display rate is now the p10 interval of 90 samples and the test sizes its spin from
  the measured rate; second run: the real positive control used `?fog=off` without `?debug=1` (M13 now ignores that,
  0 px difference) -> URL fixed. Final run green.
- `mutations4-r2.log`: harness defects (see Mutations above) -> `mutations4-r2b.log`.
- Engine change while measuring: none after the final make check, mutation and ASan runs (D14 kept the existing -O3).

### New decisions
- **D13 (C2):** accepted exe + unrecognised `.blk` tables is soft-accepted with a visible warning and the reason in
  metrics; a rejected-list exe or a non-game folder hard-rejects. The owner tightens it by adding the canonical build's
  `blk_tables_sha256` (`tools/byo_fingerprint.py`) to `builds.json` `data`.
- **D14 (M10):** keep Release `-O3` (+ wasm-opt, which emcc runs at both levels): 4 ABBA-interleaved reps per arm,
  loadavg 13–20, load of 4 areas median 613 vs 605 ms (spread 236 / 525), frame CPU p50 0.7 vs 0.7 ms (spread 0.1):
  indistinguishable; wasm 332,641 vs 344,390 B is the only difference beyond the (zero) null spread. Amends spec §4.6.

### Plan quality (Fable's lanes and Review Focus per task)
Lanes: [T1 -> T2], [T3], [T4], [T5a] are independent (parallel worktrees possible); T5b needs all four; T6 last.
| Task | Adds in r2 | Review Focus (re-run, not read) |
|---|---|---|
| T1 | I1 explicit Destroy / deferred; C1 `set_lost_handler` (null log line, WebGPU sink) | gpu_destroy probe 2 still renders after the deferred Destroy; probe 6 handler once; `cmp` a9/a7 calls native vs wasm (pre-M1b log text unchanged) |
| T2 | — | `release()` after a failed upload leaves 0 live objects (FailingDevice case) |
| T3 | M6 addressable areas | `touched() == record_reads()` on both targets; `discover()` <= 8; ctest `view_cli_manifest_addressable` red on the old tool |
| T4 | — | untouched camera == default view bit for bit |
| T5a | C2 verdict (D13), I6 helpers, M11, M12 (`paths.js`) | JS fingerprint == Python on the real install; `classify()` verdicts incl. soft accept; unit_r2 red on the old modules |
| T5b | C1 page side, I2, I3, I4, I7, M2, M3, M4, M13 | the pre-fix failures reproduced first (frozen canvas after `crashGpuProcess`; `error` after deleting the folder; …); pixels 0 vs native on levt/stat after resize |
| T6 | I5 sweep + cap, I1 real probe, M14, A/B harness (M10/M1), mutations, docs | every zero has its control firing in the same run (pixel positive, network, leak); every mutation bite attributed |

### UNVERIFIED (owner checks, named)
1. The real `showDirectoryPicker` gesture and dialog on a disk folder. 2. The real permission re-prompt (tests stub
`queryPermission`). 3. Mouse look and pointer lock on a real screen (tests drag with Playwright's synthetic mouse). 4.
Safari and Firefox (not installed; a third-party download). 5. GPU memory plateau in Activity Monitor over 30
vllg<->frnt switches (the browser probe counts GPU objects, not driver memory). 6. Real device-loss causes (driver
reset, GPU switch, eviction): only the GPU-process crash was driven. 7. Spec 1.8 eyeball + fps on the owner's Mac; A8
gates G1–G4. 8. The canonical exe's data fingerprint (D13 soft-accepts it meanwhile) and the REBUILT exe hash.

---

# REF4 (M1b browser viewer) — builder report, revision 1 (saved by controller; harness blocked the builder's write)
Built 2026-10-07 in a scratch copy of BASE af5a77c (git 4e9956e). Host shared M1 Pro, loadavg 7–21. Playwright 1.63, channel 'chromium' + --enable-unsafe-webgpu, adapter apple/metal-3. Only Chromium installed (WebKit/Firefox not run).

## Executed results
- make check green twice (≈4 min each, loadavg 11–18): pytest 74; native ctest 29/29; wasm ctest 7/7; native==wasm32 logs identical; M0 smoke pixels; byo_synthetic 55/55; datacheck 39/39 native and wasm32; a13_real 55/55; worklist 0.
- Task chain T1→T2→T3→T4→T5a→T5b→T6 each pass `make test-native test-wasm test-web` cumulatively over fresh BASE; after T6 tree == reference.
- ASan+UBSan: ctest 29/29; 123-area manifest == Release; 3 renders byte-identical to M1a-3; 0 B stderr. (Predates addendum; only later engine change is browser-only main_web.cpp.)
- Mutations (tools/research/oracle/mutate4.py): 22 defects, all caught after 2 tests strengthened (first run missed "DLLs inside data/ copied" and "never release on area switch"); no-op control missed as intended. (Run predates addendum.)

## Pixel gate (tolerance 0, from measurement)
Native vs browser, 13 comparisons (vllg, ghut, crss, frnt, levt/test, levt/stat): 0 px. After resize to 328x185, 968x545, 1280x720: 0 px. Null (same build/area, two fresh pages): 0. Positive control fog off: levt/stat 424,494 px; lev1/vllg 155,532 px (= M1a-3 native figure). Network and leak detectors each fired on a planted fault (off-manifest request; leaked texture).

## Metrics (Chromium + Metal, 1280x720)
| Area | First load (8 rounds) | Revisit (1 round) | Bytes beyond boot | Frames counted vs expected @60 Hz | dt p50/p95 | CPU p50 | malloc |
|---|---|---|---|---|---|---|---|
| lev1/vllg | 168/145 ms | 106/78 ms | 2.03 MB | 601 vs 599.6/600.0 | 16.7/16.8–17.4 | 0.7 ms | 2.9 MB |
| lev2/ghut | 84/72 ms | 88/163 ms | 2.47 MB | 601 vs 600.1/600.2 | 16.7/17.5–18.2 | 0.5 ms | 3.3 MB |
| lev4/crss | 217/162 ms | 122/160 ms | 5.76 MB | 601 vs 599.8/599.5 | 16.7/17.5–18.3 | 0.6 ms | 6.6 MB |
| lev0/frnt | 234–311 ms | — | 12.4 MB | — | — | — | 13.3 MB |
Boot set 632,334 B (census whole-file boot set 167.4 MB). Wasm memory peak 83.06 MiB (frnt) → INITIAL_MEMORY 96 MiB; boot→frnt→9 switches: no heap growth. First visit: validation 0.3–0.6 s; OPFS copy 1.10 GB in 7.3–8.6 s. Measured build: Release -O3 -DNDEBUG, Emscripten 6.0.11, wasm 326,077 B; tests assert SHA-256 == build dir file.

## Built, by brief item
1. rhi: destroy_{texture,buffer,pipeline,sampler,bind_group} both backends, validation via registry; live_objects(); surface Depth32Float (set_surface_depth) same variants/bias units; resize(w,h) reconfigures + recreates depth; frame with mismatched surface/depth size skipped, logged once. AreaRenderer::release(dev) owns everything incl. after failed upload; bind groups destroyed first. Tests: exact null log lines native+wasm32; Dawn probe gpu_destroy (dependency rule, destroy in frame, 300+100 create/destroy loops, headless rejections, 8 upload/release cycles identical pixels, flat counts).
2. Prefetch manifest: range logger (path, offset, len); `lomn_view --manifest 1`, `make manifest`; format in playbook §3.2. Over 123 areas: boot set = ranges common to all = Art.blk header+table + Base.osi = 632,334 B; per area mean 2.98 MB, max 12.43 MB (lev0/frnt); max 98 reads (lev2/vllg); union 201 MB / 133 files.
3. PrefetchFileSystem: index of every file (path, size) + resident ranges; JS writes 8 MiB chunks straight into heap buffers; read outside resident set = recorded miss + Io error (never hang); rounds capped 16; a round fetching nothing new = error; boot set pinned before main() (INVOKE_RUN=0); switch releases ranges the new area didn't read; unit tests native+wasm32.
4. BYO page: Chromium showDirectoryPicker, handle in IndexedDB, permission re-request, read in place. Fallback <input webkitdirectory> → validate → persist()+estimate() → worker copy via createSyncAccessHandle, progress, completion manifest last → revisit skips copy. Named rejections: unknown exe, rejected-list exe, foreign data. JS parsing bounded.
5. Viewer: area picker (122), faithful renderer, free-fly (WASD, Q/E, Shift, drag or pointer lock), canvas follows container; ?hud=1: build line, fps with counted vs window×rate, dt p50/p95, script/physics(0)/replay/submit/CPU, draws/pipelines/textures, live objects, heap, load stats; same on window.__lomn.metrics.
6. Network: asset-manifest.json lists every built file; every page checks requests ⊆ list, same origin, total ≤ site size.
7. A13: web/tests/byo_synthetic.mjs in make check (synthetic install via --write-install); web/tests/a13_real.mjs (make a13-real) in make check when LOMN_DATA set, exit 77 otherwise; web/tests/README.md lists what each test drives and what stays UNVERIFIED.
8. Memory: above. 9. Evidence: scratchpad/ref4-work/evidence/ (run3-make-check/shots, synthetic-final/shots, run1-16mib, run2-make-check); docs/knowledge/measurements.md M1b section.

## Addendum
1. Pixel gate above (pixel_gate block in test output). 2. Off-manifest fetch + other origin flagged; _lomn_debug_leak_texture → exactly +1. 3. metrics.build (type, flags, emscripten, compiler, SHA-256 of instantiated wasm) + canvas + adapter; tests assert SHA == build file; loadavg logged per run. 4. fps only beside counted vs expected. 5. 40 widths 360–1920: no horizontal scroll/overlap, min clearance 8 px both pages, HUD inside canvas, every resize all draws 0 rejected, no WebGPU message, exact pixels at 3 sizes. 6. build_site.mjs: JS/wasm as name.<10 hex>.ext, references rewritten leaves-first, index.html + JSON stable; no CSS file; verifies no stale names; Emscripten 6 ignores plain wasmBinary → page fetches+hashes wasm and passes via instantiateWasm (no second download). No deploy, no vercel.json. 7. Picker gesture, re-prompt, mouse look UNVERIFIED (owner).

## Decisions
- D1 reject, not cascade: destroying texture/sampler/uniform buffer referenced by a live bind group frees nothing, counted as rejection; destroying after a draw used it in the open frame is allowed (Dawn keeps alive). Evidence: null logs, Dawn probes, mutations 7, 9, 10.
- D2 manifest not shipped; derived in browser from engine misses (8 rounds first visit, 20–43 ms probing; 1 round revisit from IndexedDB cache keyed (data fingerprint, wasm SHA, area), cached ranges re-checked against index). Reason: canonical exe's data unavailable → "identical for every accepted build" unprovable. Browser reads == native --manifest exactly on 5 areas.
- D3 validation: exe hash + fingerprint of every .blk entry table; tools/builds.json gains `data` (beta-1.5.0-data, 542c54c9…) and empty `rejected`; JS == Python fingerprint (pytest, real + crafted).
- D4 copy/index set: LEGOBionicle.exe + data/** minus dotfiles, .exe, .dll: 505 files, 1,099,991,943 B; quota check uses this total.
- D5 boot set prefetched before main(), pinned. D6 release on switch keeps shared ranges; peak resident 18.8 MB during crss→frnt; test: resident == shown area's read set.
- D7 INITIAL_MEMORY 96 MiB. D8 tolerance 0. D9 122 areas in browser (lev4c.blk truncates to level blockfile under the 4-char area id; native survey counts 123). D10 JS input → C++ fly camera; untouched camera == default view exactly. D11 M0 triangle at ?scene=triangle. D12 content hashing via build script.

## UNVERIFIED (owner check)
Real showDirectoryPicker gesture/dialog; real permission re-prompt (stubbed); mouse look on a real screen (Playwright synthetic drag; pointer lock not exercised); Safari/Firefox (not installed — third-party download); REBUILT by name (exe hash unknown → generic "Unknown build"); canonical exe's data; driver-level GPU memory release; other GPUs/HiDPI; real quota exhaustion; spec 1.8 eyeball + fps acceptance; G1–G4.

## Notes
area.cpp changes small: upload ownership (3 lines), timing (4), 2 path constants, release() appended. rhi additive; native==wasm32 logs identical. Under load the M0 smoke counted 177 vs 241 expected frames (startup drops), reported not gated. Mutation and ASan runs predate the addendum.

## Proposed task split
T1 rhi destroy/surface · T2 AreaRenderer::release · T3 prefetch FS + manifest · T4 fly camera · T5a BYO modules · T5b viewer + site · T6 real-data A13, mutations, docs.
