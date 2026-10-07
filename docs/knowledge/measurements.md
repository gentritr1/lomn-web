# Measurements (raw, dated)

Machine unless stated otherwise: MacBook M1 Pro, macOS 26.5.2, Apple Metal.
Canonical exe: `LEGOBionicle.exe`, SHA-256 `09b55e62…0f0f`, build 2001-10-22.

## Binary and debug info (2026-10-06/07, `tools/decomp/nb11.py`)

| Fact | Value |
|---|---|
| NB11 block | 2,174,012 bytes at file offset 0x3d2200 |
| Distinct public/global names | 7,057 (7,058 entries) |
| Classes in mangled names / `Gc*` game classes | 505 / 160 |
| Modules / with code / with source paths | 675 / 484 / 37 |
| Struct layouts (unique) / members | 473 / 1,817 |
| Procs with params+locals / named variables | 580 / 1,265 |
| PE imports | 16 DLLs, 117 functions |
| `ScProcessManager::sFakeDeltaTime` | 0.006666666828095913 (`.data` @ 0x712d68) |
| `ScDrawableContext::mD3DDevice` / `mpD3D` | 0x738bc4 / 0x738bc0 |

## Ghidra pipeline (Ghidra 12.1.4, JDK 21)

| Fact | Value |
|---|---|
| Functions / named (non-`FUN_`) | 5,618 / 5,429 |
| Script-applied: structs, members / labels / local funcs, params, locals / D3D8 globals | 473, 1,814 / 7,042 / 429, 259, 551 / 4 |
| `IDirect3DDevice8` methods used | 30 of 97 (IDirect3D8 12/15, Texture8 5/19, VB/IB 2/14) |
| x87 compare rewrites / remaining noise | 723 / 82 (after the camera fix; 695 / 80 before) |
| `in_stack`/`in_EAX` artefacts (open gap) | 3,649 (3,279 before the camera decompiled) |
| Corpus | 406 files; 494 functions in original source paths; 9.9 MB |
| M1 seed set | 49 roots, 53 render call sites, 1,464 required = 1,428 exported functions + 36 imported (EXTERNAL) functions the M1 path calls; 1,906 candidates |
| Determinism | two exports + a fresh re-analysis: 0 differing files |
| `GcLegoCamera::Process` | 37,133 bytes; fails at 120 s (timeout) and at the 50 MB payload; with 1,024 MB / 900 s it decompiles to 3,372 lines in ~5 min |
| `make decomp` wall time | ~6 min before the camera fix; 27.6 min fresh / 19.8 min cached after (all 5,618 decompiled; member accesses 45,891) |
| Export with `ParallelDecompiler` (work-stealing, 8 cores) | 325 s, but NOT run-to-run deterministic: 4–9 of 5,618 functions differ per run pair (one early comparison happened to show 0) |
| Export with 8 fixed chunks (adopted) | ~205–250 s; 4 runs pairwise byte-identical; `make decomp` fresh 5:32–5:56, cached 3:42–4:43 |

## Game data (player install, Beta 1.5.0)

| Fact | Value |
|---|---|
| Files / bytes | 527 / 1,102,325,971 |
| `.blk` / Bink / mp3 | 737.6 MB / 245.1 MB / 101.3 MB |
| `Base.osi` / `Main.blk` / `lang_eng.blk` / `Art.blk` | 486,274 / 54,108 / 50,232 / 166.8 MB |
| Areas / largest (incl. level common blk) | 123 / `lev6/vllg` 35.0 MB |
| Installer exe SHA-256 | `b372afb9…3ab8` (798 bytes differ from canonical) |

## Runtime

| Measurement | Value | How |
|---|---|---|
| Original game under Boxedwine 26R1 Mac (Wine 10) | ~8 fps median | ffmpeg unique frames, 237 / 21 s, in-game Onu-Wahi |
| Native WebGPU (Dawn) triangle, 600 frames | p50 0.0215 ms/frame, 50.7 ms wall | `lomn_native --backend webgpu` |
| Browser (Chromium + Metal) triangle | 120 frames, 119 dt samples, p50 16.70 ms (vsync) | `web/tests/smoke.mjs` |
| Native vs wasm32 trace | byte-identical over 600 frames | `make test-wasm` |
| Metal PNG reruns | byte-identical (SSIM 1.0) | `compare.py ssim` |
| Playwright `setInputFiles(dir)` on the full install | 1,182 ms first, 209 ms with a persistent profile | `spikes/spike_c.mjs` |

## Data path (M1a-1, 2026-10-07, `lomn_datacheck` vs `tools/research/oracle/check_datacheck.py`)

| Measurement | Value | How |
|---|---|---|
| Oracle checks (A1–A6) | 39/39 in 20.8–21.1 s (oracle time, mostly Python) | `make datacheck` (also inside `make check` when `LOMN_DATA` is set) |
| BLK / OBJ / `.x` / DDS coverage | 189 BLK; 117 OBJ tables (11 cin skipped) → 183 / 136 / 35 / 943 records with flags 0x10 / 0x110 / 0x8 / other (see the reference REPORT.md histogram); 2,653 `.x`; 3,241 DDS + 6 zero-byte rejected | oracle A1, A3–A5 |
| BC decode | bit-exact on 181,568 blocks vs an independent `Fraction`-arithmetic decoder | oracle A5 (`bcdec.py`) |
| `lomn_datacheck` whole install, Release, M1 Pro | 2.12 / 1.78 / 1.99 s wall, 57.3 / 59.2 / 58.2 MB peak RSS (host loadavg ≈ 8); reference build measured 2.4–2.9 s / ≈ 51 MB earlier — RSS delta unexplained, not investigated | `/usr/bin/time -l`, 3 runs |
| Same under ASan+UBSan (Debug) | rc 0, 0 bytes stderr, 62 s; JSON identical to Release except `timing` | `build/asan` |
| Mutation smoke | 10/10 deliberate parser/routing mutations caught by unit tests | `tools/research/oracle/mutate.py` |
| Unit tests after M1a-1 | 55 doctest cases native / 51 wasm32 (4 disk-only) | `lomn_tests` |

## First pixels (M1a-2, 2026-10-07, `lomn_view` on the real install, Release, Apple M1 Pro / Metal via Dawn, 1280x720, 1 frame)

Renders, JSON and logs stay under `build/m1a2/` (git-ignored); nothing derived from game data is committed.

Commands (`source tools/env.sh` first; each with `timeout 300`):
`build/native/lomn_view --install "$LOMN_DATA/install" --level lev1 --area vllg --out build/m1a2/vllg.png` (likewise `lev2 ghut`, `lev4 crss`; `--level lev4 --area crss --yaw 67.5` for `crss_yaw67`).

**Per-area metrics** (3 runs per area; ranges are min-max over the 3. Run 1 follows a fresh build, so its `load_ms` is partly page-cache cold; runs 2 and 3 are warm. Host loadavg 4.11 / 4.90 / 4.87 at the start, 4.68 / 5.30 / 5.20 after the last run; the machine was not idle, so read these as order-of-magnitude):

| Area | load ms | decode ms | upload ms | first-frame submit ms | readback ms |
|---|---|---|---|---|---|
| lev1/vllg | 7.4-23.1 | 6.8-6.9 | 4.4-4.7 | 3.6-4.0 | 2.6-8.0 |
| lev2/ghut | 6.3-23.0 | 9.7-10.0 | 5.5-6.4 | 3.6-4.9 | 2.8-6.5 |
| lev4/crss | 10.5-34.4 | 24.9-25.3 | 9.1-10.8 | 3.6-4.3 | 2.9-4.9 |

| Area | draws (last frame) | `rejected_last_frame` | GPU textures | texture bytes uploaded (with mips) | geometry bytes | CPU copies released | heap in use, after upload -> after release (MiB) | heap after release with `--keep-cpu on` (MiB) | phys footprint after release (MiB) | peak RSS (MiB) |
|---|---|---|---|---|---|---|---|---|---|---|
| vllg | 31 | 0 | 28 | 6,995,936 | 336,420 | 7,332,352 | 8.8 -> 1.8 | 8.8 | 22.6-23.0 | 51.8-52.9 |
| ghut | 30 | 0 | 31 | 10,052,916 | 324,408 | 10,377,320 | 11.7 -> 1.8 | 11.7 | 29.9-30.2 | 59.7-60.5 |
| crss | 42 | 0 | 38 | 26,678,568 | 366,324 | 27,044,888 | 27.8-27.9 -> 2.0 | 27.8-27.9 | 79.2-79.4 | 80.1-80.4 |

GPU textures = drawn textures + 1 (the white fallback). Draws are exactly the expected 31 / 30 / 42, with 0 rejected calls. Heap in use = malloc zone bytes in use (the CPU copies freed by release show up there); RSS and footprint barely move on release because freed pages stay mapped. `crss_yaw67` (`--yaw 67.5`) gives the same 42 draws, 0 rejected.

**Pixel check against the controller's REF2 revision-2 renders** (`build/m1a2/pxdiff.py`: counts pixels where any of R, G, B, A differs, plus the max per-channel delta; allowance is <= 16 pixels at <= 8 levels):

| Render | differing pixels (of 921,600) | max channel delta |
|---|---|---|
| vllg | 0 (3 runs: 0 / 0 / 0) | 0 |
| ghut | 0 (0 / 0 / 0) | 0 |
| crss | 0 (0 / 0 / 0) | 0 |
| crss_yaw67 | 0 | 0 |

The 3 ASan+UBSan renders are also 0 pixels different from the Release renders.

**Survey** (`build/native/lomn_view --install "$LOMN_DATA/install" --survey 1 --out build/m1a2/survey.json`): 123 areas, 0 failures, total load 2,665 ms (Release, warm). Maxima: obj_records/placements 55 (`lev6/ta03`), draws 165 (`lev6/ta11`), textures 76 (`lev2/vllg`), texture bytes with mips 56,994,412 (`lev0/frnt`), geometry bytes 1,649,376 (`lev6/ta14`). Route sums over all areas: main 183 / trans 136 / back 35 / moveable 943 (the oracle A3 totals), 0 ignored, 0 disabled, 0 missing models, 0 failed textures, 16 missing textures (all in `fren/frnt`). All maxima are far below the R16 caps (1,024 placements, 4,096 draws, 1,024 textures, 256 MiB, 64 MiB).

**Mutation bite check** (scratch copy of `git archive HEAD` built Release, the REF2 scripts run only there):
- `timeout 3600 python3 mutate.py`: 26 mutations, 25 CAUGHT (by `unit`, `a7_null_calls`, `a7_gpu_pixels` or `gpu_mesh`) and the no-op control (`mip dh` rewrite) MISSED as intended; the restored tree's build and ctest both rc 0.
- `timeout 1800 python3 mutate2.py`: 2 blend mutations, both CAUGHT by the `lomn_gpu_mesh_test` blend probe; restored gpu test rc 0.

**ASan + UBSan** (`build/asan`: Debug, `-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer`, `-DLOMN_WEBGPU=ON`; Dawn itself is not instrumented): `ctest` 11/11 pass (including the 5 GPU tests); `lomn_tests` 97 cases / 53,335 assertions, rc 0; survey rc 0 with identical maxima to Release; vllg, ghut and crss renders rc 0 and stderr 0 bytes. The only stderr text is one line from `lomn_tests` (`cannot create .../lomn_loop_badout_*/plain_file/sub: Not a directory`), printed by the negative test that checks an unwritable output path; the Release `lomn_tests` prints the same line.

**`make check`**: `39/39 checks passed (19.2 s)`, `worklist check: 5618 functions, 0 errors`, final line `make check: all layers green`.
