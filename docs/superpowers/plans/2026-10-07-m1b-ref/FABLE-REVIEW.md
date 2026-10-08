# FABLE review of REF4 (M1b browser viewer) — hostile reference review

Reviewer: Fable 5.1, 2026-10-07. Host loadavg 41 during the review; nothing was built or run. Every claim below is tagged
**EXECUTED** (I ran a command on the bundle/tree and quote the result) or **INFERRED** (from reading code, logs and the
builder's artifacts). The builder's REPORT was treated as an unverified premise; its executed results (make check x2,
a13_real 55/55, byo_synthetic 55/55, ASan 29/29, mutations 22/23) are consistent with the logs I read but are only
**INFERRED** until the controller reproduces them on the frozen copy.

## Verdict: READY WITH FIXES

The reference is sound in its two load-bearing designs (reject-not-cascade destroy; miss-driven prefetch rounds) and the
evidence for them is of the right kind (exact null logs on both targets, a Dawn probe, browser reads == native access
log, 0-pixel gate with null and positive controls, resident-bytes == own-read-set after every switch). Nothing I found
leaks game data, parses user input unbounded, or renders unescaped names. What is missing is robustness and policy for a
*public* URL: a lost GPU device leaves a silently frozen canvas, a moved game folder leaves a dead `error` state, GPU
memory release in the browser depends on JS garbage collection, the HUD's "expected" frame count is derived from the
very measurement it reconciles, the page has no CSP, and the build-acceptance policy has one data fingerprint for two
accepted exes. All are plan tasks, none invalidates the reference.

---

## Findings

### Critical (must be in the plan; a public URL ships broken without them)

**C1. Device loss and uncaptured errors never reach the page.**
`engine/src/rhi/webgpu_device.cpp:952-958` — the device-lost and uncaptured-error callbacks only `fprintf(stderr)`;
`engine/src/app/main_web.cpp` installs no hook, `frame()` keeps submitting, `window.__lomn.state` stays `area-ready`.
Scenario: GPU process crash/reset (driver update, macOS GPU switch, `chrome://gpucrash`, tab eviction) → black canvas,
HUD fps still ticking, no message, no way to recover except reload. **INFERRED** (read the callbacks; grep for
`DeviceLost` shows only the stderr line).
Fix (rhi additive, T1 + T5b): add `Device::set_lost_handler(std::function<void(std::string)>)` (default no-op) to
`rhi.h`; WebGPU backend forwards the lost callback (non-Destroyed reasons) and the first uncaptured error; null backend
records `set_lost_handler` as a log line. `main_web.cpp` sets it in the device callback → `set_state("error",
"GPU device lost: …")`, `emscripten_cancel_main_loop()`, `has_area = false`. Page: on `error` after `ready`, show a
"Reload" button. Test (T6): Playwright `const cdp = await page.context().newCDPSession(page); await
cdp.send('Browser.crashGpuProcess')` on an `area-ready` page → `waitState(['error'])` within 10 s and the message
contains "device lost". Review focus: the test must prove the *pre-fix* page stays `area-ready` after the crash.

**C2. Build acceptance has two accepted exes but one data fingerprint.**
`tools/builds.json` — `accepted` lists `beta-repo-native` and `beta-1.5.0-installer`; `data` lists only
`beta-1.5.0-data` (measured from the installer). `web/app/fingerprint.js:86-88` rejects "accepted exe + unknown tables"
outright. Scenario: a player with the canonical (repo) build whose `.blk` tables differ from the installer's by even one
byte is told "Modified or REBUILT data is not supported" although both exe and data are genuine. The REPORT lists
"canonical exe's data" as UNVERIFIED, i.e. nobody knows whether the tables match. **INFERRED** (read builds.json and
classify()).
Fix (T5a; owner decision, see "Escalate"): either (a) the owner supplies the canonical build's `blk_tables_sha256`
via `tools/byo_fingerprint.py` before any public URL, or (b) `classify()` returns `{ok: true, warning}` for accepted exe
+ unknown tables (state `accepted-with-warning`, message names the fingerprint, metrics carry `data: "unknown"`), since
every parser is already bounded and a table mismatch is a fidelity issue, not a safety issue. Recommendation: (b) now,
tightened to a rejection only if the owner later confirms the canonical data matches the installer. Test: unit case for
the warning verdict; byo_synthetic `foreign-data` case asserts the new state/message.

### Important (should be in the plan; measurable correctness, honesty or robustness gaps)

**I1. GPU memory release in the browser relies on JS garbage collection.**
`engine/src/rhi/webgpu_device.cpp` destroy_* paths call only `wgpuTextureRelease`/`wgpuBufferRelease`; there is no
`wgpuTextureDestroy`/`wgpuBufferDestroy` anywhere. **EXECUTED**: `grep -n 'wgpuTextureDestroy\|wgpuBufferDestroy'
engine/src/rhi/webgpu_device.cpp` → no matches. Natively, Release on the last ref frees immediately (Dawn refcount), so
the gpu_destroy probe is green; in emdawnwebgpu, Release drops the JS wrapper reference and the GPUTexture's memory is
reclaimed when the wrapper is collected. Scenario: switching among large areas (frnt = 57 MB of RGBA8+mips) keeps
several areas' textures alive until GC; on a 2–4 GB integrated GPU this is stutter or a lost device (→ C1), and
`live_objects()` (registry counts) cannot see it. The REPORT itself marks "driver-level GPU memory release" UNVERIFIED.
Fix (T1): in `destroy_texture`/`destroy_buffer`, when `!frame_open_` call `wgpuTextureDestroy`/`wgpuBufferDestroy`
before Release; when a frame is open, Release now and queue the Destroy to `end_frame()` (an explicit destroy before
the frame's `queue.submit` would fail validation for a command buffer that references it, which is exactly the
"destroy inside a frame" case probe 2 tests). Keep the rhi contract text ("destroy inside an open frame is allowed")
true by construction. Verification: gpu_destroy probe 2 (destroy-in-frame still renders) and probe 5 (8 cycles) stay
green natively; in the browser, a13_real's 9 switches stay 0-rejected with no `[webgpu]` message. Owner-only: Activity
Monitor GPU memory during 30 vllg↔frnt switches should plateau, not stair-step. Tag the browser part UNVERIFIED in the
plan unless a measurement path exists.

**I2. The HUD's "expected" interval count is derived from the measurement it reconciles.**
`engine/src/app/main_web.cpp:114-122` `nominal_hz()` snaps `1000/p50` to the nearest common refresh rate; `:367`
`expected = window × hz`. Scenario: a CPU-bound viewer at a steady 45 fps on a 60 Hz display gives p50 = 22 ms →
snapped to 48 Hz → "601 frames vs 599 expected at 48 Hz": the reconciliation says "no drops" while a quarter of the
display's frames are missed. This breaks the §7 metrics rule the HUD exists for. **INFERRED**.
Fix (T5b): measure the display rate once from the empty-canvas loop before the first area (the `!has_area` branch of
`frame()` draws nothing: take the p50 of its first 60+ intervals, snap that, store `display_hz`), report
`nominal_hz = display_hz` and also print `measured_hz = 1000/p50`; if the two differ by > 5 %, the HUD prints "DROPPING"
beside fps. Test (T6): byo_synthetic throttles with `page.evaluate(() => { const t = performance.now(); while
(performance.now() - t < 10) {} })` injected per frame via a `?debug_spin=10` query switch, then asserts
`expected_intervals` still uses 60 Hz and the drop ratio < 0.9 is reported.

**I3. Revisit of a moved/deleted game folder is a dead end.**
`web/app/main.js:52-67` — on revisit `handleSource(handle)` walks the stored handle; a `NotFoundError` propagates to
`fail(err)` → state `error`, the picker button is hidden (`#byo` is still visible but `#pick-dir` only shows if the
early branch ran). The user has no visible way to choose another folder; only `?pick=1` works. Same for an OPFS copy
whose directory walk throws. **INFERRED**.
Fix (T5b): wrap the revisit branches; on any error, `idbDelete('install-handle')` (or `opfsClear()` for the OPFS branch),
show `#pick-dir`, `setState('need-files', 'The remembered folder "<name>" could not be read (moved or deleted). Choose
it again.')`. Test (T6, byo_synthetic): adopt an OPFS handle, delete the OPFS dir via `navigator.storage.getDirectory()
.removeEntry`, reload → state `need-files` with that message, picker visible.

**I4. No Content-Security-Policy; "no game data leaves the machine" is enforced only by a test.**
`web/index.html` has no CSP meta. The network-discipline test proves our code makes no foreign request; a CSP makes it a
browser guarantee against any future dependency or injected script. **INFERRED**.
Fix (T5b): `<meta http-equiv="Content-Security-Policy" content="default-src 'self'; img-src 'self' data: blob:;
connect-src 'self'; worker-src 'self'; object-src 'none'; base-uri 'none'; form-action 'none'">`. Emscripten 6
MODULARIZE/ES6 glue needs no `unsafe-eval` (`-sDYNAMIC_EXECUTION=0` should be added to the link line to assert it).
Test: smoke/byo_synthetic assert the meta is present and the positive-control fetch to another origin now fails with a
CSP console error (the `networkCheck` must keep flagging the *attempt*; the response must be blocked). The deploy
later mirrors it as a header (§8 cache rules are unaffected).

**I5. The 16-round cap is sound for the fingerprinted data but the per-area round count is unrecorded, and the full
sweep is off by default.**
`web/app/engine.js:12` MAX_ROUNDS 16; `web/tests/a13_real.mjs:156-173` the sweep over all 122 areas runs only with
`A13_SWEEP=1` and records memory/draw problems, not rounds. **EXECUTED**: `logs/a13-real.log` (run 1) contains
"sweep: all 122 areas load and draw their expected draws with 0 rejected", so every area converged in ≤ 16 rounds once;
"8 rounds" is recorded for only 4 areas. Soundness argument (INFERRED from code): `load_area` (`area.cpp:710-723`)
constructs a fresh `Loader` per call, so no state crosses rounds; a round's misses have priority over its load failure
(`main_web.cpp:255`), so an early abort still yields the next layer; BlockChain fallbacks (`blockchain.cpp:120-128`)
can fetch ranges native never reads, but they are untouched in the final round and freed by `release_untouched()`
(`load.bytes == bytes_beyond_boot` for all 4 areas shows none occurred). Convergence depth is a property of the fixed
engine + fingerprinted data, not of the user.
Fix (T6): sweep on by default in `make a13-real` (13.8 s measured), record `rounds`, `bytes` and `bytes − touched` per
area in `summary.sweep`, assert `max rounds ≤ 12` (headroom under 16) and print the max with its area; fail the test if
any area needs > 12 so a loader change that deepens the chain is caught before the cap bites.

**I6. The `files` list in the OPFS completion manifest is never verified, although the code comment says it is.**
`web/app/byo.js:127` "or a file missing / of another size" → `opfsCopy()` only checks `format`, `version`,
`Array.isArray(files)`. Revisit validation (exe hash + 189 tables) covers the critical files, and a missing loose file
surfaces as a load error, except `Base.osi`: `boot_reads` ignores a NotFound and the area renders with default fog and
only a warning count. **INFERRED**.
Fix (T5a): in `opfsCopy()`, check `done.files` is ≤ MAX_FILES entries of `{path: string ≤ MAX_PATH, size: safe
integer}`; in `useSource` for an `opfs` source compare `src.entries` (path, size) with `done.files` and treat any
difference as "copy incomplete" → `opfsClear()` + `need-files`. Unit test with a tampered manifest; byo_synthetic case
that deletes one copied file and reloads.

**I7. A partial copy is left occupying quota until the next pick, and the next quota check happens after clearing it
only on the input path.**
`web/app/main.js:69-71,104` — `opfsClear()` runs only in `onInput`; `start()` leaves a manifest-less `lomn-install` dir
(an interrupted 1 GB copy) in place. **INFERRED**. Fix (T5b): in `start()`, if `opfsCopy()` is null but the directory
exists, `opfsClear()` and say so in the `need-files` message. Test: write a partial dir without manifest, reload, assert
the dir is gone before the input is used.

### Minor (fix in passing or record as accepted)

**M1.** `web/app/engine.js:100-126` fetches a round's misses sequentially (`for … await fetchRange`). fetch_ms is 40–55 %
of first-visit load (62/145, 39/72, 83/162 ms in run 3). A bounded-concurrency fetch (e.g. 8 in flight) is a JS-only
change; per §7.1 measure first (first visit on the OPFS and handle paths, 5 reps, before/after) and adopt only if the
median moves beyond run-to-run spread. HDD seeks dominate on the handle path, so expect little there.

**M2.** `engine.js:128-140` cached range sets are revalidated against the index but have no byte cap; a tampered IDB
entry can make the page prefetch the whole 1.1 GB install into the wasm heap (→ growth to 2 GB → tab OOM). Cap the
cached prefetch at, say, 64 MiB (largest area 12.4 MB) and fall back to discovery. Also prune `prefetch|<fp>|<other
wasmSha>|*` keys on boot: the cache grows by ~700 KB per deployed build and is never cleaned.

**M3.** `main.js:222-227` `preventDefault()` fires for every Shift/arrow/WASD keydown anywhere on the page (except
inputs), e.g. Shift+Tab focus navigation. Scope key handling to `document.activeElement === canvas ||
pointerLockElement === canvas`.

**M4.** `main.js:236` `c.requestPointerLock?.()` returns a promise in current Chromium; a rejection (user exits lock
early) is an unhandled rejection. Add `?.catch(() => {})`. Also `onPickDir`/`adoptHandle` and the revisit `useSource`
do not take the `busy` guard: two picks in a row create two engines on one canvas. Set `busy` in `useSource`.

**M5.** Null-device `destroy()` and WebGPU destroy both erase from the registry; ids are never reused; no UAF or
double-destroy path found (INFERRED from `registry.h:238-254`, `webgpu_device.cpp` destroy_*). The M0 bind-group cache
keyed `(pipeline<<32 | buffer)` is released on either side's destroy. Mesh bind-group layouts in `mesh_layouts_` are
intentionally never released (bounded set). Record this as reviewed.

**M6.** D9: `view::list_areas` drops stems > 4 chars (lev4c), while `lomn_view --survey/--manifest` list 123 and "load"
`lev4/lev4c` as `lev4/lev4` (the level blockfile) with ok=true. Make the native tools use `list_areas` (or mark lev4c
`addressable:false` in the manifest) so the two counts agree and the manifest's 123rd row is not a duplicate load of
the level file.

**M7.** Pixel tolerance 0 is plausible only because native Dawn/Metal and Chromium's Dawn/Metal run the same code on the
same GPU; the null control (two fresh pages) cannot detect cross-GPU drift. Keep 0 as the same-machine gate; the plan
must state the procedure for another GPU (run null + native-vs-browser first, set `LOMN_PIXEL_TOLERANCE`, re-check the
positive control still exceeds it) and the test must print the adapter string in its failure message.

**M8.** §7 "each runner is also run against an empty fixture folder and must exit non-zero": byo_synthetic has no
empty-folder case. Add `setInputFiles('#byo-input', emptyDir)` → `rejected` with "LEGOBionicle.exe was not found".

**M9.** Mutation and ASan runs predate the addendum; the addendum touched only `main_web.cpp`, the page and tests, and
mutations 19–22 use byo_synthetic as their oracle. The controller's reproduction should re-run `mutate4.py --only
19,20,21,22,23` on the frozen copy; ASan cannot cover `main_web.cpp` (browser-only) by construction.

**M10.** Spec §4.6 says `-O2 + wasm-opt`; the measured build is `-O3 -DNDEBUG` (CMake Release). Record as a spec
amendment or switch; either is fine, but the perf history must say which.

**M11.** `web/app/byo.js:76-88` `filesSource` silently skips over-long/deep paths while `walkHandle` throws; `MAX_PATH`
is 1024 UTF-16 units in JS but `kMaxPathBytes` is 1024 UTF-8 bytes in C++, so a long non-ASCII path passes JS and
makes `index()` throw for the whole install. Count bytes (`new TextEncoder().encode(path).length`) in JS. Two names
that normalise to one key (`data/Foo` + `data/foo` on a case-sensitive FS) make `add_file` return "duplicate path" and
abort the page; skip the duplicate with a warning instead.

**M12.** `opfs_worker.js:6-11` trusts the main thread's path segments. The API rejects `.`/`..`/empty names, so there is
no traversal, but add the same check in the worker (defence in depth, one line).

**M13.** `?fog=off` and `_lomn_debug_leak_texture` ship in production. Harmless (all exports are reachable anyway), but
gate the query switch behind `?debug=1` so a pasted URL cannot silently show an unfaithful render.

**M14.** INITIAL_MEMORY 96 MiB leaves 13 MiB above the 83 MiB high-water mark. Growth is harmless (one heap copy), but
the test asserts *no* growth, which may become flaky with a different malloc pattern. Keep the assertion as a
same-machine regression gate and have it print the growth step rather than fail on a single 16 MiB step.

---

## Rulings on D1–D12

- **D1 reject-not-cascade: APPROVE**, with I1 (explicit Destroy when no frame is open; deferred to end_frame otherwise).
  Evidence of the right kind exists: exact null log lines on native and wasm32 (`test_rhi_destroy.cpp`, in lomn_tests
  on both targets), Dawn probes 1–5, mutations 7/9/10 bite. "Frees nothing, counted as rejection" cannot leak under real
  switching because `AreaRenderer::release` destroys bind groups first and `release_failures` is asserted 0 across 9
  real switches. The additive-only rhi rule held (all M1b calls are non-pure virtuals with defaults; M0/M1a log text
  unchanged). Native == wasm32 null logs: INFERRED from the shared unit suite running on both (wasm ctest 7/7).
- **D2 browser-derived prefetch: APPROVE**, with I5. Sound (fresh Loader per round; misses outrank load failure; the
  final round is miss-free so its reads equal native's — verified line-by-line on 5 areas). Not quadratic: each round
  re-runs one load over resident data; cost ≤ rounds × load (probe total 19–48 ms). A hostile area cannot reach the
  browser: data is fingerprinted, so the only data is the 122 areas that all converged. The Io-miss path never changes
  the shown result because only the miss-free round is uploaded. Cache key (data fingerprint, wasm SHA, area) is
  correct; every cached range is re-checked against the index. First visit on a slow disk: 45–59 small reads, worst
  case ~1 s on an HDD, 8 synchronous probes totalling < 50 ms. A shipped manifest would save ~70–100 ms per first visit
  (first 145–234 ms vs revisit 78–163 ms) at the cost of shipping user-data-derived names: not now; revisit if M2's
  larger load sets deepen the chain or an HDD measurement shows > 1 s.
- **D3 exe hash + blk tables: APPROVE the mechanism** (JS == Python oracle on crafted and real installs; bounded);
  **ESCALATE the policy** (C2).
- **D4 copy set: APPROVE.** Copying the exe (6 MB) is justified: the revisit re-validates from the copy.
- **D5 boot set pinned before main(): APPROVE** (632,334 B, 3 reads; census comparison honest about the 167 MB gap).
- **D6 release keeps shared ranges: APPROVE**; the "resident == own read set" check is the one that bites (mutation 22
  survived the flat-count check; the playbook records the lesson).
- **D7 INITIAL_MEMORY 96 MiB: APPROVE** (measured high-water 83.06 MiB, 0 growths through frnt + 9 switches + the
  122-area sweep); see M14.
- **D8 tolerance 0: APPROVE as a same-machine gate**; see M7.
- **D9 122 areas: APPROVE**; see M6.
- **D10 JS input → C++ fly camera: APPROVE** (`test_fly_camera.cpp:24-39` asserts the untouched camera's view/proj
  matrices are byte-identical to the default).
- **D11 triangle at ?scene=triangle: APPROVE.**
- **D12 content hashing in build_site.mjs: APPROVE** (leaves-first rewrite; stale-name check; `instantiateWasm` with
  page-hashed bytes). Deploy headers remain for 1.9 per §8 of the harness.

---

## Spec compliance (parent §8 1.6–1.9, M1 A13; what is claimed vs shown)

| Row | Status | Gap |
|---|---|---|
| 1.6 BYO | Built; Chromium only | Real `showDirectoryPicker` gesture and permission prompt (owner); Safari/Firefox not run; REBUILT only by generic message (hash unknown); C2 policy |
| 1.7 Network | Built and tested with a firing control | Add CSP (I4) |
| 1.8 Viewer + HUD | Built; HUD honest except I2 | **User eyeball acceptance** of 3 lev1 areas vs the original and the fps readout on the owner's Mac (spec text) |
| 1.9 Deploy | Not in scope | — |
| A13 | Built on the real Chromium path | Stubs: `estimate()`, `queryPermission`/`requestPermission`, picker replaced by an OPFS handle (documented in web/tests/README.md) |
| M1a §7.2 1.1 manifest | Built (`--manifest`), boot set vs census compared | M6 |
| §4.3 `User/` saves in OPFS | `write()` to user/ is in-memory only | Out of M1b; note for M2 |
| §4.6 `-O2 + wasm-opt` | `-O3` | M10 |

Claims resting on stubs: quota-short message, permission re-request UI, the "picked" handle path (real
`FileSystemDirectoryHandle` type, but from OPFS, not a disk folder). Everything else runs the real Chromium WebGPU path.

---

## Plan quality (T1 · T2 · T3 · T4 · T5a · T5b · T6)

Boundaries are right and the chain log shows each step green over BASE. Changes:
- **Lanes:** [T1→T2], [T3], [T4], [T5a] are independent and can run in parallel worktrees; T5b needs all four; T6 last.
- **Add to T1:** I1 (Destroy when no frame open / deferred), C1's rhi `set_lost_handler` (additive, null log line).
- **Add to T5a:** C2 verdict change (or the owner's second fingerprint), I6, M2, M11, M12.
- **Add to T5b:** C1 page side, I2, I3, I4, I7, M3, M4, M13.
- **Add to T6:** I5 (sweep default, rounds recorded), C1 CDP crash test, I3 test, M8, M9 rerun, M7 procedure text.
- **Review Focus to pin per task:** T1 — gpu_destroy probe 2 still renders after the Destroy change (the deferred path);
  null log text unchanged for every pre-M1b call (cmp a9 calls.txt native vs wasm). T2 — `release()` after a failed
  upload leaves 0 live objects (FailingDevice case). T3 — `touched() == record_reads()` on the synthetic install is
  asserted on both targets; `discover()` returns ≤ 8. T5a — JS fingerprint == Python on the real install (LOMN_DATA);
  `classify()` verdicts incl. the C2 change. T5b — the pre-fix failure of C1/I3 reproduced before the fix (a frozen
  canvas after `Browser.crashGpuProcess`; `error` state after deleting the OPFS dir); pixels 0 vs native on levt/stat
  after resize (depth valid). T6 — every zero has its control firing in the same run (pixel positive, network, leak).

---

## Owner-only verifications (cannot be driven by Playwright or decided by Claude)

1. Real `showDirectoryPicker` gesture and dialog on a disk folder; the real permission re-prompt on revisit (Chromium).
2. Safari and Firefox: the OPFS copy, `persist()` prompt (Firefox shows one), and whether Safari's default origin quota
   admits a 1.1 GB copy (if not, the quota-short message is the UX; confirm it reads well).
3. Mouse look and pointer lock on a real screen; WASD feel.
4. Spec 1.8 eyeball acceptance: 3 lev1 areas side by side with the original game (user-captured), and the fps readout on
   the owner's Mac in Chrome; thresholds set after.
5. GPU memory plateau during 30 vllg↔frnt switches (Activity Monitor) after I1.
6. C2 policy: supply the canonical build's data fingerprint, or accept soft-accept-with-warning.
7. G1–G4 and the REBUILT exe hash (to fill `builds.json` `rejected` with a named entry).

## What I could not verify (host overloaded; no builds or runs)

Every executed result in the REPORT (make check x2, a13_real 55/55, byo_synthetic 55/55, ASan 29/29, mutations) is
INFERRED from the bundled logs; the controller should reproduce `make check` with LOMN_DATA and `mutate4.py --only
19,20,21,22,23` on the frozen copy. The two EXECUTED checks of mine were greps on the built glue (`Module["HEAPU8"]` is
reassigned in `updateMemoryViews`, so the per-chunk heap-view re-read in `engine.js:89` is correct after growth) and on
the backend source (no `wgpuTextureDestroy`/`wgpuBufferDestroy`).
