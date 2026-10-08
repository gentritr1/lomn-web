# Browser tests (M1b)

All browser tests launch ONE persistent Chromium per file (`channel: 'chromium'` + `--enable-unsafe-webgpu`: the real
GPU; the default headless shell falls back to SwiftShader, whose canvases screenshot blank) and wait on
`window.__lomn.state` / metrics, never on time. Pixels are always checked.

| File | Runs in | Data |
|---|---|---|
| `serve_test.mjs`, `unit_test.mjs` | `make test-web` (node --test) | none |
| `smoke.mjs` | `make test-web` | none: the M0 triangle (`index.html?scene=triangle`) |
| `byo_synthetic.mjs` | `make test-web` | the synthetic install `lomn_view --write-install` writes + a stand-in exe; test-only `builds.json` entries (`build_site.mjs --extra-builds`) |
| `a13_real.mjs` | `make a13-real` (inside `make check` when `LOMN_DATA` is set; exit 77 = skipped otherwise) | the real Beta 1.5.0 install |

## Which path each step exercises

| Path | How the test drives it | Exercised for real | Not exercised (UNVERIFIED) |
|---|---|---|---|
| Safari/Firefox fallback: `<input webkitdirectory>` -> validate -> `persist()` + `estimate()` -> copy to OPFS in a worker (`createSyncAccessHandle`, 8 MiB chunks) -> completion manifest -> read from OPFS | `page.setInputFiles('#byo-input', <install dir>)` (Playwright passes the directory's files with `webkitRelativePath`) | the page code, the worker, OPFS writes and reads, the revisit that skips the copy (persistent profile) — **in Chromium** | the same code in **Safari and Firefox** (no WebKit/Firefox Playwright build is installed here; installing one is a third-party download) |
| Chromium handle path: `showDirectoryPicker` -> handle in IndexedDB -> read in place | `__lomn.useDirectoryHandle(handle)` with the OPFS directory handle of the copy (the same `FileSystemDirectoryHandle` type the picker returns) | everything after the picker: walking the handle, validation, reading in place (no copy), IndexedDB persistence, the revisit that reuses the stored handle | the **picker dialog itself** (needs a user gesture; Playwright cannot drive it) and a handle onto a **real disk folder** |
| Permission re-request on revisit | `FileSystemHandle.prototype.queryPermission` stubbed to `'prompt'` (and `requestPermission` to `'granted'`), then a real click on the re-grant button | the page's `need-permission` state, the button, the resumed load | the **browser's real permission prompt** and a real `'prompt'` state |
| Quota-short | `navigator.storage.estimate` stubbed to 10 bytes free | the message, and that nothing is copied | a real full disk / real quota |
| Rejections (unknown exe, a rejected-list exe, accepted exe + foreign data) | crafted synthetic folders through the input | the validation code and the named messages | the real **REBUILT** exe (its SHA-256 is not known; `builds.json` `rejected` is empty, so REBUILT gets the generic "Unknown build" message that names it) |
| Revisit after the folder moved (I3), an incomplete copy (I6), an interrupted copy (I7) | an OPFS "folder" adopted then deleted; a copied file deleted; the completion manifest deleted | the page's recovery to `need-files` with a reason (the picker shown, the stale handle or copy removed) | a real disk folder moved by the user |
| Device loss (C1) | CDP `Browser.crashGpuProcess` on an `area-ready` page | the engine's lost handler -> state `error` with the reason, the Reload button, the main loop stopped | a real driver reset, GPU switch or tab eviction (same callback, other causes) |
| After a device loss (final I-1) | the same crash, then a direct `showArea`, a programmatic `change` on the picker, `_lomn_area_show`; and a crash while a switch waits on its prefetch reads (`Blob.prototype.arrayBuffer` held by the test) | the picker and camera reset disabled, every switch refused, state stays `error` with the loss reason, no frames | a loss during the engine-side upload itself (no hook to hold it there) |
| Control-character file names (final I-2) | a synthetic install with `a<TAB>b.blk` and `c<LF>d.blk` beside the real blockfiles, through the input | both skipped with a warning naming the character, nothing copied, the area list and a switch intact | — |
| No WebGPU (final I-3) | `navigator.gpu` hidden by an init script, `?pick=1` | the named final message before a folder is chosen; a folder chosen anyway copies nothing (no copy worker, OPFS listing unchanged) | a real Safari/Firefox without WebGPU (none installed here) |
| Folder not remembered (final m-3) | the picker stubbed to an OPFS handle, `IDBObjectStore.put` throwing for the handle key | the page stays `area-ready` with a warning, no `error` | a real IndexedDB quota failure |
| Debug badge (final m-1) | `?debug=1&fog=off` and `&debug_spin=3` without `?hud=1`; `?fog=off` without `?debug=1` | the badge names the active switches; absent without `?debug=1` | — |
| Real screen input | — | — | **owner check, UNVERIFIED**: the real `showDirectoryPicker` gesture and dialog, the real permission re-prompt, and mouse look on a real screen (the tests drag with Playwright's synthetic mouse in headless Chromium; pointer lock is not exercised) |

## Gates and their controls (every zero needs a detector that is shown to fire)

| Gate | Measured value | Control that must fire |
|---|---|---|
| Pixels, native vs browser (tolerance = max per-channel delta; LOMN_PIXEL_TOLERANCE, default **0**) | 0 differing pixels of 921,600 on levt/test, levt/stat, lev1/vllg, lev2/ghut, lev4/crss, lev0/frnt, and on levt/stat at 328x185 / 968x545 / 1280x720 after resizes | **null**: the same build and area on two fresh pages (levt/test; vllg/ghut/crss first visit vs revisit): 0 / max delta 0. **positive**: fog off (`?fog=off`, a debug switch) vs the native faithful render: levt/stat 424,494 pixels over the tolerance, lev1/vllg see `a13_real` |
| Network discipline | only asset-manifest files, bytes <= the site | a fetch of `/not-in-manifest.bin` and of another origin are both flagged |
| Leaks (flat live objects per area across switches) | flat | one texture leaked on purpose (`_lomn_debug_leak_texture`) changes the key by exactly +1 texture |
| Release on switch | resident file bytes == the shown area's own read set | mutation "never release" (mutate4.py #22) is caught |
| Build under test | `metrics.build.wasm_sha256` == SHA-256 of `build/wasm/lomn_web.wasm`, type Release, flags `-O3 -DNDEBUG` | (identity check, not a detector) |
| Device-lost reporting (C1) | a crashed GPU process -> `error` "GPU device lost (reason 1) …" + Reload | natively, gpu_destroy probe 6: a WGSL that does not compile reaches the handler once; pre-fix the page stayed `area-ready` (r2 red log) |
| GPU memory returned at switch time (I1) | an init-script hook counts GPUTexture/GPUBuffer created minus `destroy()`ed: flat over 30 switches (synthetic; real vllg<->frnt) | pre-fix the same hook counted 174 -> 1,238 textures and ≈ 1.0 GB of texture bytes left to garbage collection over 30 real switches |
| fps reconciliation (I2) | measured fps vs the display rate measured before the first area; `dropping` flag | a 20 ms per-frame spin (init script) and `?debug=1&debug_spin=25` both flag DROPPING; the old snapped-p50 expectation called the same run clean |
| CSP (I4) | `connect-src 'self'` etc. in `index.html` | the foreign-origin fetch is blocked by the browser (securitypolicyviolation) and still flagged by the test |
| Data acceptance (C2, D13) | accepted exe + unknown tables: shown with a warning, `metrics.validation.data = "unknown"` | a rejected-list exe, an unknown exe, an exe without `.blk` files and an empty folder still hard-reject |
| Responsive (40 widths, 360..1920 px) | no horizontal scroll, no overlap, minimum clearance 8 px (BYO page and viewer), HUD inside the canvas, every resize draws all draws with 0 rejected calls and no `[webgpu]` message | the pixel checks at three widths need a valid depth buffer (levt/stat has overlapping quads) |

**Another GPU (M7).** Tolerance 0 holds because native Dawn/Metal and Chromium's Dawn/Metal run the same code on the same
GPU; the null control cannot see cross-GPU drift. On another GPU or browser: (1) run the null (same build, two fresh
pages) and native-vs-browser there, (2) set `LOMN_PIXEL_TOLERANCE` to the larger max delta seen, (3) check that the
positive control (fog off) still exceeds it by a wide margin. A failing pixel check prints the adapter for this reason.

**Debug switches** need `?debug=1` (M13): `fog=off`, `debug_spin=MS`, `fetch_concurrency=N`, `nocache=1`. Without it a
pasted URL always renders faithfully. With it, a `debug` badge in the page header names the active switches (HUD or not).
