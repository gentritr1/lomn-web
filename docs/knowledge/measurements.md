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

## Faithful states, OSI caps and fuzz (M1a-3 Task 7, 2026-10-07, `lomn_view` on the real install, Release, Apple M1 Pro / Metal via Dawn, 1280x720, `--frames 60`)

Commands (`source tools/env.sh` first; each with `timeout 120`): `build/native/lomn_view --install "$LOMN_DATA/install" --level lev1 --area vllg --frames 60 --out build/m1a3/vllg_r1.png` (likewise `lev2 ghut`, `lev4 crss`; `--level lev4 --area crss --yaw 67.5` for `crss_yaw67`, 1 frame). Nothing derived from the game data is committed; renders and JSON stay under `build/m1a3/`.

**Per-area faithful-state metrics** (3 runs per area, identical across runs; read from the `states` and `scene` blocks of `<out>.png.json`; `frames_rendered` 60 / 60 for all three):

| Area | draws | models | pipelines | shader variants | state calls / frame | lit / fogged / blended / alpha-tested draws | lights (source) | fog (source) |
|---|---|---|---|---|---|---|---|---|
| lev1/vllg | 31 | 10 | 3 | 5 | 212 | 0 / 23 / 2 / 4 | 23 (`light_slb`) | 40..1200, colour 0xFFF9DCF9 (OSI script fog) |
| lev2/ghut | 30 | 1 | 2 | 4 | 137 | 0 / 30 / 3 / 2 | 5 (`light_slb`+default) | 4701..4702, white (default) |
| lev4/crss | 42 | 14 | 4 | 8 | 234 | 0 / 38 / 8 / 10 | 2 (`light_slb`) | 4701..4702, white (default) |
| lev4/crss `--yaw 67.5` (1 frame) | 42 | 14 | 4 | 9 | 239 | 0 / 38 / 8 / 10 | 2 (`light_slb`) | default |

`rejected_last_frame` is 0 everywhere; `unsupported_bits` 0. Lit draws are 0 because the area render modes clear LIGHTING (spec R18). `crss_yaw67` renders 1 frame (not 60), and its state-call and shader-variant counts differ from the 60-frame `crss` run by 5 and 1 (cause not investigated).

**Timings** (3 runs per area; ranges are min-max. Host loadavg at the start of the three render batches 4.72 / 6.70 / 6.95 and at the end 4.53 / 6.60 / 6.91; the machine was not idle and the GPU was shared, so read these as order-of-magnitude. Run 1 follows other work and is partly page-cache cold):

| Area | load ms | decode ms | upload ms | first-frame submit ms | p50 frame submit ms (60 frames) | readback ms | peak RSS (MiB) |
|---|---|---|---|---|---|---|---|
| vllg | 10.3-16.6 | 8.3-8.4 | 4.8-5.1 | 9.5-9.9 | 0.32-0.34 | 7.0-11.3 | 57.3-59.6 |
| ghut | 7.5-30.5 | 11.8-14.4 | 4.9-8.0 | 7.7-7.9 | 0.31-0.34 | 11.3-17.2 | 64.0-66.6 |
| crss | 13.2-35.5 | 29.9 | 8.3-9.1 | 11.3-11.7 | 0.40-0.42 | 16.4-25.1 | 81.7-85.4 |
| crss_yaw67 (1 frame) | 13.8-22.2 | 29.8-31.2 | 9.6-13.0 | 11.2-15.8 | n/a (1 frame) | 3.8-5.3 | 81.7-82.8 |

Heap in use after upload -> after release of the CPU copies (MiB): vllg 9.0 -> 2.0, ghut 11.9 -> 2.0, crss 28.1 -> 2.2. Texture bytes uploaded 6,995,936 / 10,052,916 / 26,678,568 and geometry bytes 336,420 / 324,408 / 366,324, unchanged from the M1a-2 section above.

**Pixel check against the controller's REF3 renders** (`docs/superpowers/plans/2026-10-07-m1a3-ref/scripts/pngdiff.py A B 16 8`; counts pixels where any of R, G, B differs, plus the max per-channel delta; allowance is <= 16 pixels at <= 8 levels): vllg, ghut, crss and crss_yaw67 are **0 of 921,600 differing pixels, max delta 0, in all 3 runs each**. The ASan+UBSan renders are also 0 / 0 against both REF3 and the Release renders.

**Survey** (`build/native/lomn_view --install "$LOMN_DATA/install" --survey 1 --out build/m1a3/survey.json`, host loadavg 4.73 -> 4.51, 4.9 s wall): 123 areas, 0 failures, 0 rejected draws, 0 unsupported bits. `total_load_ms` 3,126 (sum of per-area load 1,732 + decode 1,392; the M1a-2 survey measured 2,665 ms without the OSI and state work, on a different load). Over all areas:
- lit draws 0 (of 3,958 draws); fogged 3,375, blended 364, alpha-tested 434;
- pipelines per area: max 4 (`fren/frnt`), distribution 0:6, 1:10, 2:33, 3:44, 4:30 areas;
- fog source: OSI script fog in 53 areas, default (4701..4702) in 70; lights source: `light_slb` 51, `light_slb`+default 48, default 24;
- maxima: state calls 830 (`lev6/ta03`), lights 209 (`lev6/ta08`), shader variants 11 (`lev1/clf2`); the M1a-2 maxima (draws 165 `lev6/ta11`, textures 76 `lev2/vllg`, texture bytes with mips 56,994,412 `lev0/frnt`, geometry 1,649,376 `lev6/ta14`, placements 55 `lev6/ta03`) are unchanged.

**OSI maxima on the real `install/data/Base.osi` and the caps** (`tools/research/osi_maxima.py`, independent Python reader; byte-identical output to REF3's `logs/osi-maxima.json`; caps in `engine/include/lomn/sage/osi.h`):

| Quantity | Real maximum | Cap | Headroom |
|---|---|---|---|
| file bytes | 486,274 | 8 MiB | 17x |
| instructions per subroutine | 1,707 (`setupshoefitterscreen`) | 32,768 | 19x |
| instructions total (unique subroutines) | 151,522 | 4,194,304 | 27x |
| `CallGameFunction` calls total | 4,402 | 131,072 | 29x |
| calls per method | 78 (`onakoro_puzzle.initfull`) | 2,048 | 26x |
| argc per call | 9 | 127 | 14x |
| args per method | 128 | 4,096 | 32x |
| string-arg bytes per method | 570 (longest single 145) | 16,384 | 28x |
| literal stack depth | 16 | 256 | 16x |

Other file facts: 2,749 strings (38,191 bytes), 23 functions (23 unique offsets), 395 classes, 8,406 methods, 3,606 unique subroutines, branch targets per subroutine max 48.

**OSI fuzz** (`make fuzz-osi FUZZ_OSI_N=2000000 FUZZ_OSI_N_WASM=300000`, ASan+UBSan with `-fno-sanitize-recover=all`, 4 min 10 s wall for the whole target including both compiles, loadavg 7.61 -> 4.95; the Makefile defaults are 200,000 native / 20,000 wasm32):

| Target | Seeds | Inputs | Accepted | Method evaluations | Findings |
|---|---|---|---|---|---|
| native | synthetic (seed 1) | 2,000,000 | 290,456 | 288,788 | 0 |
| native | synthetic + real `Base.osi` (seed 2) | 2,000 | 482 | 5,008 | 0 |
| wasm32 under Node | synthetic (seed 3) | 300,000 | 43,261 | 42,942 | 0 |

REF3's own run quoted 290,448 / 43,370 accepted for the same seeds and sources (`osi.cpp`, `osi_builder.h`, `osi.h` are byte-identical to REF3's); the 8 and 109 input differences were not chased (the native run is deterministic here: two runs of 200,000 gave identical counts). `tools/research/osi_craft.py` crafted `Base.osi` files (kinds `calls`, `stack`, `funcs`, `nops`: 600,337 / 600,337 / 824,617 / 1,048,912 bytes) installed into a written synthetic install and run through `lomn_view --backend null` (ASan+UBSan and Release): rc 0, empty stderr, peak RSS 39.4-44.8 MB (ASan) and 13.1-16.2 MB (Release), 0.13-0.18 s (ASan) and 0.02-0.03 s (Release).

**Mutation bite check** (scratch copy of `git archive HEAD` at `eb4a6d7`, Release, REF3 `mutate3.py` run only there; ctest subset `unit|a7_|a9_|gpu_mesh|gpu_ffp|osi_probe|ffp_oracle`; loadavg 5.84 -> 7.75, about 4 min): 42 entries, **40 CAUGHT**; the two MISSED are the no-op control (intended) and the NaN vertex-colour mutation (intended: the float-to-int conversion of NaN is undefined behaviour that no value test can see). That NaN mutation was then applied in the scratch copy under the ASan+UBSan build: `unit` aborts with `ffp.cpp:270:38: runtime error: nan is outside the range of representable values of type 'unsigned int'` (ctest 23/24, only `unit` failing), so it is caught under UBSan. The restored build rc 0 and the restored full ctest rc 0.

**ASan + UBSan** (`build/asan`: Debug, `-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer`, `-DLOMN_WEBGPU=ON`; Dawn itself is not instrumented): `ctest` 24/24 pass (including the GPU tests); `lomn_tests` 132 cases / 172,064 assertions, rc 0; the vllg, ghut, crss and crss_yaw67 renders rc 0 with 0 bytes of stderr; the survey rc 0 with 0 bytes of stderr and maxima identical to Release. The only stderr text is from `lomn_tests` negative tests (the unwritable-output-path line and three `[area] render skipped` lines), identical to the Release `lomn_tests` stderr.

**Tests**: `make test-native` 24/24 and `make test-wasm` 5/5 (native ctest + wasm32 under Node, including the native == wasm32 call-log and state-log comparisons) on the committed tree.

## A11 fuzz corpus over every real format (M1a-4 Task 1, 2026-10-07, `engine/tests/fuzz_formats.cpp`)

**What was run:**
- **Target:** `make fuzz-formats FUZZ_N=200000` with `LOMN_DATA` set (Beta 1.5.0 install).
- **Builds:** `build/asan/native` and `build/asan/wasm`, Debug, `-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all`.
- **Inputs:** seed 1, 200,000 inputs per format.
- **Real seeds, read only:** 16 per format sampled with the seed (≤ 256 KiB each). LZSS gets 8 real-derived seeds and
  OSI gets `Base.osi`.

How to run it and what each number means: `tools/research/fuzz_formats.md`.

**Native, final tree (run 5; per-format totals byte-identical to runs 2, 3 and 4):**
- 202.8 s for 2,000,000 inputs (runs 3 and 4: 354.7 s and 553.2 s at loadavg 16-73).
- Worst per-input CPU time 29.6 ms (`osi`; worst wall time 179 ms), against the 2 s limit.
- Worst per-input heap growth 2.04 MiB (`tga`), against the 64 MiB limit.
- 0 inputs re-timed.

| Format | Inputs | Accepted | Deep reach | Findings |
|---|---|---|---|---|
| blk | 200,000 | 169,905 | 167,264 (83.6 %) | 0 |
| lzss | 200,000 | 35,046 | 137,556 (68.8 %) | 0 |
| slb.obj | 200,000 | 55,350 | 54,687 (27.3 %) | 0 |
| slb.pos | 200,000 | 45,258 | 44,723 (22.4 %) | 0 |
| slb.light | 200,000 | 47,479 | 46,863 (23.4 %) | 0 |
| slb.tex | 200,000 | 37,246 | 37,583 (18.8 %) | 0 |
| x | 200,000 | 18,480 | 27,543 (13.8 %) mesh decoding; 190,843 (95.4 %) token stream | 0 |
| dds | 200,000 | 84,944 | 84,944 (42.5 %) | 0 |
| tga | 200,000 | 65,506 | 108,281 (54.1 %) | 0 |
| osi | 200,000 | 51,750 | 51,750 (25.9 %) | 0 |

slb.tex deep reach exceeds its accepted count because a failure inside the record loop also counts as deep.

**`.x` deep reach corrected in fix round 1** (review I-1). It used to count "past the 16-byte header into the token
stream" (95.4 % real, 90.9 % smoke), so the 5 % floor could not fail. It now counts mesh decoding entered: accepted
with a mesh, or one of `decode_mesh`'s own errors.

The new count is pinned against an independent counter at `decode_mesh`'s entry (a scratch probe), input by input. In
each run it found 0 inputs counted deep without entering and 0 that entered without counting:

| Run | Deep = decode_mesh entered |
|---|---|
| smoke | 1,917 (9.6 %) |
| real seeds, 20,000 | 2,785 (13.9 %) |
| real seeds, 200,000 (`--formats x`, Release; the same input stream as the table above) | 27,543 (13.8 %) |

**The floor now has teeth:** with a scratch `parse_x` that rejects every input before `decode_mesh`:
- the old definition still passes the smoke (deep 90.9 %, rc 0);
- the new one fails it: `x deep reach 0.0% is below the 5.0% minimum`, rc 1.

TGA deep reach counts accepted images, raw type 2 included, plus failures inside the RLE packet loop.

**wasm32 under Node (2 GB cap):** identical per-format totals to native. The make target's `cmp` of the two logs passed
("fuzz-formats: native == wasm32 per-format totals"), and make exited 0.
- **Provenance (final review M-6):** this 200,000-input wasm32 leg is the implementer's run on the `088d599`-era driver (the first A11 commit). The later driver edits (the cleanup S3 `Fragment` refactor, the `.x` deep-reach counter) were not re-run at 200,000 on wasm32. At `343af0d` the 20,000-input smoke totals are identical natively and on wasm32 (the final review compared the per-format total lines with `cmp`), and the native totals did not change across those edits (the final review's `c62e6b7` vs `343af0d` comparison).
- The wasm32 leg took 5,154 s (whole target 5,397 s, 17:59-19:29, loadavg about 7-12).
- Worst per-input CPU time 436 ms (`osi`; worst wall time 1.55 s); `x` 125 ms CPU / 706 ms wall.
- 0 inputs re-timed.
- Worst heap growth 2.04 MiB.
- Linear memory peaked at 824 MiB of the 2 GB cap, with the 32 MiB quarantine (see below).

**Per-input time under host load:** the driver checks CPU time, not wall time.
- In wasm32 run 3 (about 17:30, loadavg about 27), two real-seeded OSI inputs measured 2.34 s and 2.07 s of wall
  time.
- Replayed alone, they took under 2 s on 2 of 3 timed replays. Replay process times for identical work ranged from
  14 s to 52 s.
- In native run 4 (loadavg 35-73 on 8 cores), 8 OSI inputs needed wall-time re-runs, with a 1.94 s minimum.
- The brief's 2 s limit is therefore checked as CPU time, with the minimum of up to 3 runs.
- Scratch controls are still reported:
  - a `decode_dds` that spins for 2.1 s of CPU gives `CPU time 2.100140 s >= 2 s on 3 runs`;
  - an infinite loop trips the 10 s CPU watchdog, exit 4.

**Smoke subset (ctest `fuzz_formats_smoke`, Release, synthetic seeds only, 20,000 per format):**
- 0 findings.
- Deep reach 9.6 % (x, mesh decoding; its token stream 90.9 %) to 81.7 % (blk).
- 0.64 s native, 0.29-0.38 s wasm32.
- Per-format totals byte-identical native vs wasm32.

**Findings, all fixed in this task** (commits `74a5a69` and `2856ce9`, each with a unit test that fails on the old
code):
1. `decode_lzss` / `BlockFile::read` left partial output on failure. Found by the synthetic smoke run: 8,261 and
   1,185 findings out of 20,000.
2. `Slb::parse` kept its bytes and partial fix-ups after a rejection. Found in 1,816-3,522 of 20,000 inputs per SLB
   view.
3. `decode_tga` allocated `w·h·bpp` before checking the data length. Real-seeded `tga` input 49594 grew the heap by
   67,119,136 bytes: 1 finding in the first 2,000,000-input native run.

`make datacheck` after the fixes: 39/39, including every one of the 2,566 `.tga` entries decoding.

**wasm32 ASan memory** (the reason the driver sets `quarantine_size_mb=32` on wasm32):
- **Default 256 MiB quarantine:** the linear memory stepped up at each format change: 1,111 → 1,402 → 1,693 →
  1,887 → 2,048 MiB over blk → slb.light. The run then aborted with `AddressSanitizer: out of memory` at `x` input
  442. This happened in 2 runs, at the same input.
- **That input alone:** a benign 24 KB rejection, 0 findings, 246 MB peak RSS under Node.
- **32 MiB quarantine:** 630 / 630 / 727 / 727 / 727 / 727 MiB through slb.tex, and 824 MiB at `x` 20,000, past
  input 442. That experiment run was stopped there.

**Proof the fuzzer bites.** Scratch copies (`git archive` of the final tree under `/private/tmp/claude-501/lomn-bites2`)
each reintroduce one past defect. Each was run with the make target's ASan flags, `--seed 1`, and `--formats <family>`.
Each format's input stream is independent of the others, so the input numbers are those of the full run.

| Defect reintroduced | Synthetic smoke (20,000) | Normal run (200,000, real seeds) |
|---|---|---|
| M1a-1 BLK name-run cap (`kMaxNameRun`) removed | 83 findings, first at input 227: `lookup_name exceeds 40 + kMaxNameRun` | 484 findings, first at input 308 |
| M1a-3 `textures.slb` per-record name-copy cap (`kMaxSpecTextureName`) removed | 16 findings, first at input 793: `name longer than kMaxSpecTextureName accepted` | 287 findings, first at input 381 |
| One TGA RLE bound removed (the run-packet `in_range`) | ASan `heap-buffer-overflow ... READ of size 3` at `image.cpp:179`; the death callback names `tga input 157` | the same, at `tga input 1334`; wasm32 ASan reports it at the same input 1334, with the default and with the 32 MiB quarantine |

**Control:** removing the `textures.slb` record-COUNT cap (`kMaxSpecTextures`) instead gives 0 findings in 200,000
inputs. Mutation cannot build 1,025+ records that each have their own fix-up slot. `slb_probe` catches that one: it
accepts the 64 MiB table at +651 MB and fails.

## Texture decode cache across area loads (M1a-4 Task 3, 2026-10-07, `lomn_view --survey 1 [--texture-cache on]`)

**Setup:**
- Host and build: Release, Apple M1 Pro (8 cores), Beta 1.5.0 install, 123 areas.
- Metric: Σ per-area `decode_ms`. That covers DDS/TGA decode plus CPU mips; with the cache on it also includes the
  SHA-256 of the entry bytes, the lookup and the mip copy.
- Order: interleaved A B A B A B (A = flag absent / OFF, B = `--texture-cache on`), 3 reps per arm.
- Load: 1-min loadavg recorded at the start and end of every rep.

**Reconciliation:** every rep loaded 123/123 areas with 0 failures. Every ON rep had hits + misses = 1828 + 812 = 2640
= Σ textures (0 decode failures), with 382 evictions. Resident bytes ended at 267,565,168 of the 268,435,456-byte cap.

**Final code (ARMv8 SHA-256 path), commit 3fe8f2b:**

| rep | arm | Σ decode_ms | survey total_load_ms | wall s | loadavg 1-min start / end |
|---|---|---|---|---|---|
| 1 | OFF | 1183.4 | 6150.1 | 8.16 | 11.66 / 11.45 |
| 2 | ON | 676.2 | 1740.7 | 3.22 | 11.45 / 10.85 |
| 3 | OFF | 1125.7 | 2120.1 | 3.58 | 10.85 / 10.94 |
| 4 | ON | 651.1 | 1672.6 | 3.14 | 10.94 / 10.94 |
| 5 | OFF | 1127.7 | 2218.7 | 3.70 | 10.94 / 10.46 |
| 6 | ON | 670.4 | 1850.1 | 3.34 | 10.46 / 10.35 |

- **decode_ms:** OFF median 1127.7 ms, with an OFF-vs-OFF null spread of 57.6 ms. ON median 670.4 ms, spread 25.1 ms.
  - Effect: **−457 ms (−40.5 %)**.
  - The ranges do not overlap, and the effect is 7.9× the null spread.
  - The reviewer's independent run replicated it (2 reps per arm, loadavg 5-6): −472 ms (−40.0 %), no overlap.
- **total_load_ms:** no claim. Rep 1's 6.1 s outlier (load_ms 4964, I/O) makes the OFF null spread 4030 ms.
- The 15-min loadavg was about 17-22, so these numbers are provisional under TEST-005.

**Superseded first build (portable SHA-256 only): ON was SLOWER.** Same method, 3 reps per arm, 1-min loadavg
about 6.2-7.0 (15-min about 24-25).

| rep | arm | Σ decode_ms | survey total_load_ms | wall s | loadavg 1-min start / end |
|---|---|---|---|---|---|
| 1 | OFF | 1110.6 | 2675.2 | 4.13 | 7.04 / 6.80 |
| 2 | ON | 1606.7 | 2676.9 | 4.13 | 6.80 / 6.57 |
| 3 | OFF | 1118.3 | 2174.7 | 3.63 | 6.57 / 6.57 |
| 4 | ON | 1626.3 | 2688.0 | 4.16 | 6.57 / 6.37 |
| 5 | OFF | 1104.7 | 2103.8 | 3.54 | 6.37 / 6.18 |
| 6 | ON | 1605.8 | 2625.0 | 4.09 | 6.18 / 6.40 |

- decode_ms: OFF median 1110.6 ms (spread 13.6). ON median 1606.7 ms (spread 20.5).
- Effect: **+496 ms (+44.7 %)**. The ranges do not overlap.
- **Cause:** SHA-256 throughput on a 256 MiB buffer is 208-215 MB/s with the portable rounds and 1267-1377 MB/s with
  the ARMv8 SHA-2 instructions. That led to the ARMv8 path, which gives the same digest: tested against `shasum` and
  the portable path, and by the reviewer at 20 sizes on arm64 ARMv8, arm64 portable and x86-64 under Rosetta.

**Where the cache pays off:**
- Only on native arm64 with the SHA-2 extension.
- **x86-64** (no SHA-NI path) and **wasm32** use the portable rounds. There the cache is likely slower than no cache,
  as measured above for portable code on arm64. INFERRED; x86-64 and wasm32 timings were not measured.
- The flag defaults to off everywhere and stays off in the browser and on x86-64 (controller ruling T3-1). A cheaper
  wasm key is future work, only if the browser ever enables the cache.
- Outside `--survey`, a run loads one area and can never hit, so lomn_view warns on stderr.

## Browser viewer (M1b reference REF4, 2026-10-07, Chromium (Playwright 1.63 `channel: 'chromium'`) + Apple Metal, 1280x720)

Shared host: loadavg 7.4 -> 9.8 (run 1, `A13_SWEEP=1`, INITIAL_MEMORY 16 MiB: the memory measurement) and 14.6 -> 14.3
(run 2, inside `make check`, INITIAL_MEMORY 96 MiB). Times are order-of-magnitude. Commands: `make manifest`,
`make a13-real` (`web/tests/a13_real.mjs`), `make test-web` (`web/tests/byo_synthetic.mjs`). Nothing derived from the
game data is committed: renders, screenshots and the real manifest stay under `build/`.

**Prefetch manifest** (`lomn_view --manifest 1`, native Release, 4.15 s wall, 74 MB peak RSS): 123 areas (the survey's
count; `lev4/lev4c.blk` truncates to the level blockfile under the game's `%.4s` area id, so the browser lists 122),
0 failures. Boot set = the ranges common to every area = Art.blk header (12 B) + table (146,048 B) + Base.osi
(486,274 B) = **632,334 bytes** in 3 reads, against the census boot set of 167,412,690 bytes (whole Main.blk, Base.osi,
lang_eng.blk, Art.blk; the viewer reads neither Main.blk nor lang_eng.blk). Per area beyond the boot set: mean
2,980,916 B, max 12,434,082 B (lev0/frnt; 13,066,416 B incl. boot), max 98 reads (lev2/vllg). Union over all areas:
2,712 ranges, 201,356,965 B in 133 files.

**BYO**: validation (exe SHA-256 + 189 `.blk` tables, 643,748 B) 323 ms (run 1) / 597 ms (run 2); OPFS copy of the
copy set (505 files, 1,099,991,943 B) 7.26 s / 8.63 s; first visit from `setInputFiles` to lev1/vllg on screen 8.6 s /
10.5 s. Boot prefetch 3 rounds, 632,334 B, 6.5 ms; revisit 1 round (cached set).

| Area | first visit: rounds, load total ms | revisit (cached set, 1 round) ms | bytes beyond boot | engine probe / decode / upload ms (run 1) | draws / pipelines | textures / bytes with mips | fps window (run 1): frames, ms, expected intervals at 60 Hz | dt p50 / p95 ms (run 1; run 2) | replay / submit / frame CPU p50 ms |
|---|---|---|---|---|---|---|---|---|---|
| lev1/vllg | 8, 168 / 233 | 106 / 168 | 2,027,888 | 31.7 / 16.7 / 6.7 | 31 / 3 | 28 / 6,995,936 | 601, 9,976.9, 598.6 | 16.7 / 16.8; 16.7 / 17.5 | 0.5 / 0.2 / 0.7 |
| lev2/ghut | 8, 84 / 109 | 88 / 57 | 2,469,250 | 19.9 / 13.2 / 6.1 | 30 / 2 | 31 / 10,052,916 | 601, 9,985.5, 599.1 | 16.7 / 18.2; 16.7 / 17.5 | 0.3 / 0.2 / 0.5 |
| lev4/crss | 8, 217 / 188 | 122 / 257 | 5,763,178 | 43.0 / 34.1 / 23.8 | 42 / 4 | 38 / 26,678,568 | 601, 9,980.5, 598.8 | 16.7 / 18.3; 16.7 / 17.5 | 0.4 / 0.2 / 0.6 |
| lev0/frnt | 8, 234 / 311 | — | 12,434,082 | — | 47 / — | 56,994,416 B | — | — | — |

Reconciliation: dt samples = frames in window - 1 (600 / 601) on every area; frames counted vs window x 60 Hz:
601 counted vs 599.6–600.1 expected (the window holds 0.2 % more intervals than 60.000 Hz predicts: the display runs
at about 60.1 Hz; no dropped frames). All 122 browser areas loaded in a sweep
(13.8 s, first visit), each drawing exactly its expected draws with 0 rejected calls.

**Memory budget** (run 1, start 16 MiB): wasm memory after lev1/vllg 16 MiB (no growth), lev2/ghut 19.25 MiB, lev4/crss
40.0 MiB, lev0/frnt **83.06 MiB** (87,097,344 B), then no growth over 9 switches and the 122-area sweep. malloc in use:
boot 0.73 MiB (run 2, at "ready"), vllg 2.88 MB, ghut 3.31 MB, crss 6.64 MB, frnt 13.32 MB, after switches back to crss
6.64 MB; prefetched file bytes resident = the area's set incl. boot; peak 18.8 MB (crss -> frnt: both sets resident
until the switch completes). INITIAL_MEMORY = **96 MiB**; run 2 confirmed 100,663,296 B from "ready" through frnt and
9 switches (0 growths).

**Pixels**: browser canvas vs native `lomn_view` render of the same build: **0 differing pixels of 921,600** (max delta 0)
for lev1/vllg, lev2/ghut, lev4/crss (first visit via OPFS, revisit, handle path), lev0/frnt, and the synthetic levt/test
and levt/stat; tolerance therefore 0 (`LOMN_PIXEL_TOLERANCE` overrides it for another GPU). The native renders are
byte-identical to the M1a-3 evidence PNGs.

**Pixel gate with controls** (REF4 addendum; run 3 = `make check` 16:30–16:34, loadavg 17.7 -> 11.5): tolerance 0 (max
per-channel delta). Null (same build, same area, two fresh pages): levt/test, lev1/vllg, lev2/ghut, lev4/crss all 0
differing pixels. Native vs browser: 0 on all 9 real comparisons and 4 synthetic ones, and 0 on levt/stat after resizes to
328x185, 968x545 and 1280x720. Positive control (fog off, `?fog=off`): levt/stat 424,494 pixels over the tolerance (max
delta 28), lev1/vllg 155,532 (max 29; the M1a-3 native "faithful vs fog off" figure was also 155,532 px, max 29).
Network and leak detectors fire on their controls (one foreign path + one foreign origin flagged; one leaked texture
counted). Measured configuration on every run: Release, `-O3 -DNDEBUG`, Emscripten 6.0.11, wasm 326,077 B (SHA-256
`d6c5687f…`, equal to the build directory's file), canvas 1280x720 at DPR 1, adapter `apple/metal-3`. Responsive sweep
(40 widths, 360..1920 px): no horizontal scroll or overlap, minimum clearance 8 px on the BYO page and the viewer; every
resize drew all draws with 0 rejected calls and no `[webgpu]` message. Run 3 fps windows: 601 frames counted vs
599.0 / 599.2 / 598.5 expected at 60 Hz, dt p50 16.7 / p95 17.4–17.5 ms. Under the same load the M0 triangle smoke
counted 177 frames vs 241 expected in its 4.0 s window (startup frames dropped while the host was at loadavg ≈ 18).

**Discovery == native log**: the browser's last-round reads equal the `lomn_view --manifest` ranges line for line for
lev1/vllg (45 ranges), lev2/ghut (39), lev4/crss (59), levt/test and levt/stat.

**Mutation bite check** (`tools/research/oracle/mutate4.py`, scratch copy, 23 entries: prefetch fs, rhi destroy rules,
release order and ownership, fly camera, area list, JS validation, the chunk writer, the quota check, surface depth,
release on switch): first run 20 CAUGHT, 3 MISSED: the no-op control (intended), "DLLs under data/ are copied" (no test
had a DLL inside data/) and "never release on switch" (the flat-count check passed: see the playbook §3.1). Both tests
were strengthened; the rerun of those two plus the control: 2 CAUGHT, control MISSED. Builds and lomn_tests restored rc 0.

**Sanitizers** (`build/asan`, Debug, ASan+UBSan, `-fno-sanitize-recover=all`, Dawn not instrumented): ctest 29/29
(incl. `gpu_destroy` and the new unit cases), the 123-area manifest identical to Release (wall time excluded), the
vllg/ghut/crss renders byte-identical to the M1a-3 evidence, 0 bytes of stderr.

## Browser viewer revision 2 (REF4 r2 after the Fable review, 2026-10-07; host loadavg 10–48)

Every behaviour fix was first run on the unfixed code (logs `r2-red-*`), then fixed and re-run (`r2-green-*`, `make check`).

| Item | Before the fix (red run) | After the fix |
|---|---|---|
| C1 device loss | `Browser.crashGpuProcess` (CDP) on an `area-ready` page: still `area-ready`, 877 frames later, no message | state `error` "GPU device lost (reason 1): A valid external Instance reference no longer exists.", Reload shown, loop stopped; native gpu_destroy probe 6: a WGSL that does not compile reaches the handler once |
| C2 accepted exe + unknown tables | rejected | shown, warning "Game data differs from the known build — rendering may differ (…fingerprint…)", `metrics.validation.data = "unknown"`; rejected-list exe, unknown exe, no-`.blk` folder and empty folder still rejected |
| I1 GPU memory at switch time | init-script hook (createTexture − destroy): 30 real vllg<->frnt switches 174 -> 1,238 live textures, est. 111 MB -> 1,007 MB of texture bytes waiting for GC; synthetic 14 -> 154, 0 destroyed | real: 29 textures / 10.7 MB at every vllg; synthetic 4 live, 150 of 154 destroyed |
| I2 reconciliation | no display rate; a 20 ms spin at 60 Hz ran 48 fps and the snapped-p50 rule said "48 Hz: expected 90.1 vs counted 90" (clean) | display rate measured before the first area (p10 of 90 empty-frame intervals): 60 Hz -> DROPPING. Under loadavg 28–45 headless Chromium ran the page at 30 Hz (p10 32.7 ms) and the display measured 30 Hz; a 1.5-period spin (50 ms) gave 19.7 fps -> DROPPING; `?debug=1&debug_spin=50` the same |
| I3 moved remembered folder | `error`: "A requested file or directory could not be found…" (dead end) | `need-files`: "The remembered folder … could not be read (moved or deleted)", picker shown, handle forgotten |
| I4 CSP | none | `default-src 'self'; script-src 'self' 'wasm-unsafe-eval'; …`; the foreign fetch is blocked (securitypolicyviolation) and still flagged; `-sDYNAMIC_EXECUTION=0` links |
| I6 copy missing a file | loaded (`area-ready`, default fog without Base.osi) | cleared, `need-files`: "…was incomplete (missing data/Base.osi) and was removed" |
| I7 interrupted copy | the manifest-less directory stayed | removed at start, message "An incomplete copy … was removed" |
| M2 stale cache keys / cap | a key of another wasm build stayed | pruned at boot; cached sets over 64 MiB ignored (unit test) |
| M3 key scope | W with the page body focused moved the camera | only with canvas focus or pointer lock |
| M4 double pick | 2 wasm fetches, 2 engines on one canvas (uncaptured "TextureView … cannot be used with [Device]") | 1 engine; the second call returns false |
| M6 addressable areas | ctest: a 5-character stem listed as an area | manifest: 122 rows + `not_addressable: ["lev4/lev4c"]`; survey: 123 total, 122 addressable |
| M11/M12/I6 helpers | unit tests failed (missing / old behaviour) | UTF-8 byte cap, case duplicates skipped with a warning, worker path check, copy-manifest shape + comparison |
| M13 debug switches | `?fog=off` honoured on any URL | needs `?debug=1` |

**I5 sweep (all 122 areas, fresh discovery, default in `make a13-real`):** rounds histogram {2: 1, 3: 4, 5: 1, 6: 107,
7: 1, 8: 8}; maximum 8 (lev0/frnt, lev1/atrm and others) -> asserted cap 12 (1.5x), engine hard limit 16. Over-fetch (bytes
moved but not read by the final round) 0 in every area. 20.9 s at loadavg ≈ 28. Memory: 0 growth from "ready"
(100,663,296 B) through frnt, 9 switches and the sweep.

**M10 / D14 (-O3 vs -O2 + wasm-opt)** (`web/tests/measure_arms.mjs`, 4 reps per arm, ABBA-interleaved, loadavg 13–20):
wasm 332,641 B (-O3) vs 344,390 B (-O2); total load of vllg+ghut+crss+frnt (no cache) median 613 ms (spread 236) vs
605 ms (spread 525); frame CPU p50 0.7 vs 0.7 ms (spread 0.1). Load and CPU are indistinguishable; the wasm size is the
only difference beyond the (zero) null spread: -O3 kept, spec §4.6 amended.

**M1 (measure only)**: 8 fetches in flight vs 1, same build: median 532 vs 613 ms (spreads 329 / 236): within the noise
on this loaded host; not adopted (the switch stays a debug option, `?debug=1&fetch_concurrency=8`).

**Mutations r2** (`mutate4.py`, 34 entries incl. 11 new for C1, C2, I1, I2, I3, I4, I6, M3; scratch copy, 18:08–18:23):
33 CAUGHT, the no-op control MISSED, **as recorded on 2026-10-07 18:08-18:23 on a host whose display measured 30 Hz; entry 29
(I2 "expectation from the snapped p50 again") is host-rate dependent and was MISSED when rerun alone on a 60 Hz display
(2026-10-07 21:47, spin 1.5 periods = 25 ms snapping to 48 Hz: the old circular rule still read DROPPING). T8 P11 made the spin
2.0 display periods; entry 29 alone is CAUGHT again, twice (2026-10-07 21:58 and 23:07, both on a 60 Hz display, spin 33 ms).
The tally after P11 is therefore 33 CAUGHT only if the other 32 still bite: the full 34-entry rerun on a 60 Hz host is PENDING
(T8 P14: not run here; entry 29 is the only one re-verified).** A browser-checked mutation counts only when a check it targets failed (`EXPECT` in
the script); every engine mutation is rebuilt out again before the next one. The first r2 run reported 33 CAUGHT too,
but it had two harness defects, found by reading its logs: the wasm build was not rebuilt after restoring an engine
mutation (later web-only mutations ran on the I2-mutated module) and the node check omitted `unit_r2_test.mjs`. Lesson:
a mutation harness must attribute each bite to the targeted check and restore builds, or a flaky or leftover failure
reads as a bite.

**ASan + UBSan r2** (final engine sources): ctest 30/30, the 122-area manifest identical to Release (`not_addressable`
lev4/lev4c), vllg/ghut/crss renders byte-identical to the M1a-3 evidence, survey 123 total / 122 addressable / 0
failures, 0 bytes of stderr.

## M1 final gate (merged M1a-4 + M1b, 2026-10-08)

Run by the controller on a frozen `git archive 8b91a14` of `m1-integration` (own node_modules, symlinked toolchain), host load 36–42 from other sessions, 60 Hz display.

- **`make check`** with `LOMN_DATA`: all layers green — pytest 111 passed, 0 skips / xfails / xpasses; native ctest 36/36; wasm ctest 9/9; native == wasm32 traces and logs identical; node 59; byo_synthetic 123/123; datacheck 39/39 native and wasm32; a13_real 60/60; worklist 0 errors.
- **`make a9-sweep`:** 123/123 areas equal (oracle == shim). **Renders** lev1/vllg, lev2/ghut, lev4/crss byte-identical to the M1a-3 references.
- **Full `mutate4.py`, all 38 entries** (run in chunks 1–13, 14–26, 23 + 27–38 with the fixed scorer, 33–38): **37 CAUGHT, each on its targeted check; the no-op control (#38) MISSED.** This closes the "full rerun PENDING" note above.
  - Entry #23 was first scored MISSED although its targeted `FAIL C1: GPU process crash …` line was in the log: `mutate4.run()` kept only the last 1,500 characters of the FAIL lines, and #23 breaks 7 checks. Fixed in `d72cc7c` (every FAIL line is kept; `tools/tests/test_mutate4_run.py` fails on the old `run()`); #23 re-run: CAUGHT on C1.
  - One chunk was killed by a 30-minute tool limit mid-entry #33 and left that mutation applied in the frozen copy; the file was restored from the commit, the tree re-compared (0 differences) and #33–#38 re-run.

## M2a-1 OSI VM (2026-10-08/09, native Release Apple M1 Pro + wasm32 under Node 20; game data: Beta 1.5.0 install)
- **A6 differential:** Base.osi Setup + 600 frames + keys. The digests are byte-identical across native, wasm32 and the Python oracle, in both own-stub and replay modes.
  - Setup takes 48,850 instructions; 600 frames take 390,706 (553 per frame).
  - The trace window is 55,170 lines.
- **Conformance:** 528 programs: 521 at the A5 gate, + 6 falsifiers from the mutation run (5 in group a, 1 in group b), + `b-eng-reenter-depth`. Results: 528/528 on native, wasm32 and the oracle, and 528/528 C++ vs Python traces byte-identical.
- **Mutation bite:**
  - 47 entries (33 consult + 13 settled + the control);
  - 44 caught at first; the 2 survivors were closed by new corpus programs, and the control stays MISSED.
  - A review on 2026-10-09 showed the pre-fix atof model gives 492 C++-vs-oracle mismatches on 6,013 boundary strings, and 0 after the I1 fix (24,052 strings).
- **Fuzz** (`make fuzz-osi-vm`, ASan+UBSan): 5,000 inputs, 96/96 opcodes fetched, 0 internal errors, 0 invariant failures.
- **Engine-call nesting:**
  - Base.osi reaches a maximum depth of 1 in A6. This is a floor: the stub natives never re-enter. Re-measure in M2a-2.
  - Port cap: 64.
  - wasm stack: 1,648 bytes per level, 116,240 bytes at the cap. The default 64 KiB runs out near level 33. `STACK_SIZE` = 512 KiB (4.5× need).

## M2a-2 baseline (Task 1, 2026-10-09; Python oracle with typed stubs on the Beta 1.5.0 `Base.osi`; load average 5 to 11 from other sessions)

Every row was re-measured with the committed tools (`tools/osi/coverage.py`, `flip_probe.py`, `reach.py`, `exc.py`). The numbers below are the ones later tasks pin.
`$OSI` is `$LOMN_DATA/install/data/Base.osi`; `$T` is `--natives data/osi/natives.json --mods data/osi/natives_beta150.json --types data/osi/types.json` (spell the three options out in zsh). Traces go to a scratch directory and are not committed.

| Fact | Value | Command |
|---|---|---|
| Setup with stubs | 48,850 instructions, 980 native calls, **54 distinct natives**, 0 `P` lines | `osi_vm.py --run $OSI $T --frames 0 --trace S.trace`, then `coverage.py --trace S.trace` (`natives distinct 54 … calls 980, unregistered 0`) |
| Setup + 600 frames + keys `down@100 down@160 enter@220 esc@280` | 390,706 instructions, **1,699 native calls, 69 distinct natives** (68 exe rows + 1 Beta 1.5.0 mod row, `bettersaver.getbooleanvalue`, 1 call at frame 3); 15 are first hit after Setup: 1 at frame 0 (`gcstatusscreen.hideinventory`), 14 at frame 3 | `osi_vm.py --run $OSI $T --frames 600 --keys "down@100 down@160 enter@220 esc@280" --trace A6.trace`, then `coverage.py --trace A6.trace` (the same report is `make osi-coverage`, about 10 s); the first-hit frames are column 6 of the `native` rows |
| The 30-frame set equals the 600-frame set | the 69 natives hit by Setup + 30 frames are the same 69 as above, so a 30-frame flip probe covers every native of the stub path | `flip_probe.run_once(…, frames=30).natives` against the A6 report |
| Natives whose return changes the path (Setup + 30 frames, one alternative each) | **16 of 69**: **7 CHANGED** (`bettersaver.getbooleanvalue` +8, `gcareadirector.getcurrentlevel` +24, `gcsaver.cheatmenu` −37,713, `gcgame.getmp3player` −11, `gctimer.create` +8, `scslosifileio.doesfileexist` +293, `scslosifoundation.stringtoidentifier` +24) and **9 CHANGED-NULL** (`sctilefont.create` −69,359, `sctextobject.create` −61,030, `gcbigsprite.loadhudsprite` −67,381, `gcgame.getmainview` −47,736, `gcviewport.getcamera` −47,730, `gcgame.getkeymapper` −47,604, `gcmodeldirector.gettoa` −44,797, `gcmodeldirector.getcharacter` −44,719, `scslosislkcubicspline.create` −16,455); 53 SAME. Baseline 70,222 instructions, 13,534 distinct PCs; a repeated baseline is identical (null control) and no native changes the PC set without changing the instruction total or the other way round. The 69-run probe takes 6 to 7 s | `flip_probe.py --osi $OSI --frames 30 \| awk '{print $1}' \| sort \| uniq -c` (`7 CHANGED`, `9 CHANGED-NULL`, `53 SAME`) |
| Natives that reach `ScOSIVirtualMachine::Call` 0x60b320 / `CallWithReturn` 0x60b450 **through direct calls (a FLOOR)** | **35 of 1,049** by direct calls in `calls.jsonl`, which has no virtual or callback edges. On the stub path, with call depth: `gcmodeldirector.addtoa` 3, `gccharacter.setupmoveable` 4, `gcareadirector.setcurrentlevel` 5, `gcareadirector.setcurrentarea` 6. The deepest are depth 11 (`gccharacter.setholder`, `gcfireboss.drop`, `gcmodeldirector.setinputstate`, `gctoa.setholder`, `gcwindboss.introfinished`). **Gap, listed (D6):** the closure of the two targets holds 226 functions; **49 of them have no caller at all and are no native body**, so they are entered only through a vtable or a handler table, and any native that reaches one is missing from the 35. They are printed as `indirect` lines. Entered by (scan of the exe as data): 38 sit in a vtable slot, 10 in a lone data pointer of a handler table (`GcLegoCamera::KeyPressed`, `GcKeyMapper::KeyPressed`, `GcCharacterAIController::Patrol/Die/Stun`, `GcDragItemCollisionEvent::HandleEvent`, `GcToaCombatCollisionEventHandler::HandleEvent`, `GcHydraBoss::Suck/Dead`, `GcRockBoss::HandleEvent`), 1 is the PE entry point (`_SafMainCRTStartup`); 0 are pushed as a code address. The 49: `_SafMainCRTStartup`, `GcCollisionCylinderSystem::Collide/Respond`, `GcCollisionPolySystem::Collide/Respond`, `GcCollisionSphereSystem::Collide/Respond`, `GcCollisionAttackSystem::Respond`, `GcCollisionPhysicsGroup::Process`, `ScSLOSIProcess::Process`, `GcTimer::Trigger`, `GcAnimationEventTrigger::Trigger`, `GcLegoCamera::KeyPressed/Process`, `GcCharacterAIController::Patrol/Die/Stun`, `GcDragItemCollisionEvent::HandleEvent`, `GcToaCombatCollisionEventHandler::HandleEvent`, `GcObjectMoveInterpreter::Process`, `GcBAGroup::Process`, `GcMouseDriver::Process`, `GcToa::PreProcess/Process`, `GcCinemaEngine::Process` and its deleting destructor, `GcCinemaEditor::Process`, `GcConversationEngine::Process`, `GcKeyMapper::KeyPressed/Process`, `GcProjectile::MainGeometryRespond`, `GcVectorProjectile::MainGeometryRespond`, `ScSLOSIAnalogControllerAction::Fire`, `GcHive::Process` and its deleting destructor, `GcIceBoss/WindBoss/FireBoss/MudBoss/HydraBoss/RockBoss::Process`, `GcBaseBoss/MudBoss/RockBoss::CheckCollisions`, `GcFireBoss::PreProcess/HandleEvent`, `GcHydraBoss::Suck/Dead`, `GcRockBoss::HandleEvent`. Not followed: `ScOSIVariant::Dispose` 0x60a930 makes virtual calls (a released game handle can run a destructor that re-enters the VM); no vtable is resolved to its callers. M2a-2 tiers that rest on "this native cannot re-enter" must treat 35 as a minimum | `reach.py --vm-reentry \| tail -1` (`vm-reentry (direct-call floor): …`) and `reach.py --vm-reentry \| grep ^indirect` |
| Direct callers of 0x60b320 / 0x60b450 | **71** functions: 69 call `Call` and 2 call `CallWithReturn` (`ScSLOSIProcess::Process` and `Call` itself, so `Call` is a caller of the second target). Spec §3.3 says 69 (the callers of `Call`); opcodes.md counts 144 raw `E8` sites. Task 2a reconciles the three counts | the summary line of `reach.py --vm-reentry` |
| Setup's script-visible arguments | `setcurrentlevel("lev0")`, `setcurrentarea("frnt")`, `addtoa("onua")`, `doesfileexist("Root/Data/AreaViewer.txt")`, `sctilefont.create("Root/Data/Art/Fonts/BionicleFont")` ×4, `gctimer.create(0, 0, 5.0)` (the float is `0012:40a00000`), `gchuddirector.playcinematic` ×3 (`Root/Data/cinematics/*.bik`, second argument 1) | `grep '^N ' S.trace` for those natives, with the `S` lines of the argument handles decoded from hex |
| `P` lines (string → float) on the stub A9 path | 0 in Setup and in the 600-frame trace | `grep -c '^P ' A6.trace` |
| One oracle run of Setup + 30 frames | about 0.1 s (69 runs plus the baseline in 6 to 7 s) | the `flip_probe:` line on stderr |

Rows from the plan that are CITED from the asm and not re-measurable by these tools (the Setup timer is never started, `CreateVariant(GcTimer*)`, engine-call nesting 1 / cap 64 / 1,648 B per level) are unchanged. So is the plan's sub-claim "every engine → script call on those chains names `OnEvent`": **Task 1 did not verify it** (it needs the arguments of each `Call` site on the four chains; Task 2a decodes those chains).

### The `.exc` decoder (`tools/osi/exc.py`, the exe read as data)

| Function | Info record | Catch landing pads |
|---|---|---|
| `BCLCallGameFunction` 0x6015a0 | 0x6dafe4, flags 0xc8, 24 call sites | outer `0x601c94` (`!ScCatchMessage!`), `0x601cb3` (`!exception!`), `0x601cd3` (`...`); inner, around the native call at 0x601af5: `0x601b10`, `0x601b30`, `0x601b50` |
| `GcGame::SetUpScripting` 0x4390d0 | 0x675566, flags 0xc8, 20 call sites | `0x439324`, `0x439343`, `0x439363` |
| `_WinMain@16` 0x401020 | 0x66702e, flags 0x08, 2 call sites | `0x401080`, `0x401093`, `0x4010b0` |

Corrections to opcodes.md "Exception tables", found while writing the decoder (the document is not edited here):
- An info record is **4 bytes long in 4,966 index entries, not 4,605**: bit 0 set (4,605) or bytes 1 to 3 zero with bit 0 clear (361: flags 0x00, 0x08, 0x48, 0xc8 and similar). The long layout holds in the other 2,036 entries. A decoder that follows the document reads the next record as a call-site table for those 361.
- Kind **0x07** is 14 bytes (`{flags, i32 frame offset, destructor, u32}`, an array destructor): the record after it is a known kind in 3,017 of 3,017 non-final occurrences. Kinds 0x05, 0x06, 0x09, 0x0b and 0x12 are still not decoded; `exc.py` stops that chain and prints `truncated N`.
- A catch's landing pad appears once per call-site chain, so the catch records of `0x6015a0` are listed per site; the summary line gives the distinct pads.

### Static reachability of the 20 argc-violation sites (`reach.py --argc-violations --0x19`)

M2a-0 claimed the 20 `Base.osi` sites whose argc is outside the registered `[min, max]` are all statically unreachable. **Confirmed under the plan's rule: 0 of 20 are reachable**, and still 0 when an UNRESOLVED 0x19 may name any method (`wild`).
- The walk covers 3,606 subroutines (23 global functions + 3,583 distinct method bodies); 2,825 are reachable, 319 of the 395 classes are constructed in reachable code. The roots are `setup`, `process`, `input`, `onevent`; the 16 `0x50` sites all push a global-function offset directly. The four global functions that are not reachable: `setupdemoscreen`, `setuplanguagetest`, `setupshoefitterscreen`, `setuptestlevel`.
- The reasons, per owner class: `lego3dsound`, `ss_base3dobject`, `ss_base3dcharacter` are never constructed (no `0x13` anywhere); `cavedigging` is constructed only in the method `lego_testlevel.lego_testlevel`, and `lego_testlevel` is constructed only in `setuptestlevel`, which no `0x50` or pushed offset reaches; `lego_timer` is constructed, but the symbols `durationpeek`, `durationlast` and `durationaverage` are never taken by a reachable `0x12`/`0x17`/`0x19`.
- **The 12 `0x19` sites** are all UNRESOLVED: each name comes from `GetThisMemberValue` (`base_safeevent.functionpointer` 10 times, `gs_tweakobject.func` twice), never from a literal. All 12 sit in unreachable methods (`base_safeevent.execute`, `.executearguments`, `gs_tweakobject.incvalue`, `.decvalue`), so no UNRESOLVED site widens the closure. Task 10's runtime trace is their confirmation. `0x83` has 0 sites.
- **Engine roots**: the exe contains no string for any of the other 19 global function names (case-insensitive byte search of the whole exe for each name, and for the substrings `setuplevel`, `setuptest`, `testlevel`), so the engine does not call them by name; it holds `Setup`, `Process`, `Input` and `OnEvent` as NUL-delimited tokens. UNVERIFIED: a name built at run time from pieces.
- **Caveat the walk cannot see**: `ScOSIVirtualMachine::ConstructObject` 0x60be00 has a second caller, `ScSLOSIFileIO::GetVariantFromBuffer` 0x6160a0 (deserialising a saved object), reached from the natives `scslosifileio.loadosiobject` and `scslosifileio.loadosiobjectfrommemcard`. A class could be built from a save file without a `0x13`. Neither native is on the stub path (not in the 69).

- 2026-10-09 (M2a-2 T2a): the Task 1 wording above that calls `.exc` kind 0x07 "an array destructor" and lists kinds 0x05, 0x06, 0x09, 0x0b and 0x12 as undecoded is superseded by `docs/osi/opcodes.md` "Exception tables" (T2a): every kind is now decoded with the runtime's own sizes (0x07 is a 14-byte member destructor), and all 7,002 indexed functions report `truncated 0` (`tools/tests/test_osi_exc.py`).

## M2a-2 T6: `lomn_view --script 1` and the `-fwasm-exceptions` scope (Decision 12) (2026-10-09; native Release Apple M1 Pro + wasm32 under Node; Beta 1.5.0 install; load average 6.6 to 36 from other sessions)
- **`lomn_view.wasm` size (Decision 12).** `ls -l build/wasm/lomn_view.wasm` after `make wasm` on the base commit `f770742` and after this task:
  - before: 462,989 bytes (`lomn_view.js` 75,481);
  - after: 681,587 bytes (`lomn_view.js` 74,535): **+218,598 bytes (+47.2 %)**.
  - The delta is everything `--script` links: the whole `lomn_osi` library (VM, heap, native framework, ScriptHost), compiled with `-fwasm-exceptions`, plus the inherited 512 KiB `STACK_SIZE` (linear memory, not file size). The flag's own share was not isolated: `lomn_osi` cannot be built without exception catching (D10).
  - T6 fix 1 (the install guard and the per-run User/ directory, `std::filesystem`): 693,588 bytes (+12,001 over T6; `ls -l` after `make wasm`).
  - Scope: `lomn_view` (the node tool) inherits the flag; `lomn_web` (the browser build) does not link `lomn_osi`, so its size is unchanged until M2b. Gap: the `datacheck-wasm` null-log run (M1 unaffected) is the controller's.
- **`--natives stub` adds no behaviour.** `build/native/lomn_view --install "$LOMN_DATA/install" --script 1 --natives stub --frames 600 --keys "down@100 down@160 enter@220 esc@280" --digest … --natives-log …`: 601 `D` lines equal to `lomn_osi_run`'s digest from `f770742`; natives.log (2,299 lines) byte-identical; Setup 48,850 instructions, 600 frames 390,706 (553 per frame), 69 distinct natives (stub 68, mod 1), 1,699 calls, unregistered 0; nesting high-water 1, stack high-water 57.
- **Native == wasm32** (the same command under `node build/wasm/lomn_view.js`, plus `--trace … --trace-frames -1:2 --out …`): digest (603 lines), natives.log (2,299 lines), trace window (55,170 lines) and stdout (79 lines) byte-identical. stdout carries no wall-clock field; the timings are in the `--out` JSON only. One run each: native 0.25 s wall, wasm32 0.61 s wall (single samples on a loaded host, not a benchmark).
- **`--natives real`** (the default) fails closed on both targets while no game-native group is bound (Tasks 7a–9a): exit 4, `real natives: no native group binds a body`, no report; stdout and stderr identical native vs wasm32.

## M2a-2 Task 5: the A10 taint pass on the stub run (2026-10-09; Python oracle, typed stubs, empty overrides; load average 10 to 36 from other sessions)

`tools/research/osi_taint.py` (docs/osi/natives.md §7.3). `$OSI` is `$LOMN_DATA/install/data/Base.osi`. These are measured values of the stub run (D6), not targets. Every tier-2 verdict here holds only for the stub run: a native whose result reaches a native that Tasks 7–9 make tier 1 would become UNSAFE through the `tier1-arg` sink on the merged run, so Task 10 re-runs the pass with the merged overrides and `--real-natives`. (Corrected in fix 1, review M5: the first version named `gcareadirector.getscriptfluffstring` → `stringtoidentifier` as such a case, which was never measured and is wrong. With `stringtoidentifier` set to tier1 by a scratch override, the A9 run gives `tier1-arg 0` and `getscriptfluffstring` stays safe: its result is appended to an array, and `stringtoidentifier`'s arguments on this path are string constants.)

| Fact | Value | Command |
|---|---|---|
| A9 path: Setup + 600 frames + keys `down@100 down@160 enter@220 esc@280` | `taint: natives tainted 69, sinks 38 (branch 38, gamevar 0, tier1-arg 0, process-result 0, name 0)`; 390,706 instructions (the A6 total), stop none, 0.6 s. After fix 1 (the `refstore` and `errpath` sinks, the tier1 output over-approximation, the type-only annotation): `taint: natives tainted 69, sinks 38 (branch 38, gamevar 0, tier1-arg 0, process-result 0, name 0, refstore 0, errpath 0), type-only 27`, the same 14 UNSAFE / 55 safe | `osi_taint.py --osi $OSI --frames 600 --keys "down@100 down@160 enter@220 esc@280" \| head -1` (overrides `data/osi/native_overrides.json` = `[]`) |
| UNSAFE natives on the A9 path (sink sites, first frame) | **14 of 69**: `scslosifoundation.stringtoidentifier` (11 sites, Setup), `gcgame.getresolutionheight` (8, Setup), `gcgame.getresolutionwidth` (4, Setup), `gcgame.getmp3player` (3, Setup), `gclegocamera.getcameraaimode` (3, Setup; one more site first at frame 100, the first key), `gcsaver.getammo` (2, Setup), `gcareadirector.getcurrentlevel` (1, Setup; it shares a site with `stringtoidentifier`), `gchuddirector.addbigsprite`, `gctimer.create`, `scslosifileio.doesfileexist`, `gcsaver.cheatmenu`, `gcmodeldirector.gettoa` (1 each, Setup), `bettersaver.getbooleanvalue`, `scslosislkcubicspline.create` (1 each, frame 3). Every sink is a 0x53 branch. The other 55 are safe | the `native … verdict UNSAFE` and `sink` lines of the same run |
| Setup + 30 frames (the flip-probe window) | `taint: natives tainted 69, sinks 37 (branch 37, …)`: the same 14 UNSAFE natives (the frame-100 site is outside the window) | `osi_taint.run_base(script, 30, "", "data/osi/native_overrides.json")` |
| Flip implies sink (Review focus 5) | the 7 flip-CHANGED natives are all UNSAFE, each with a branch sink; of the 9 CHANGED-NULL natives, 2 are UNSAFE (`gcmodeldirector.gettoa`, `scslosislkcubicspline.create`) and 7 are safe, and all 9 are "must (game-returning)" natives of Tasks 7–9 (no tier1 override exists yet). 5 flip-SAME natives are UNSAFE (`getresolutionwidth`, `getresolutionheight`, `addbigsprite`, `getammo`, `getcameraaimode`): the probe tries one alternative, the pass sees every use | `pytest tools/tests/test_osi_taint.py -k flip` |
| Falsifier bite | a pass that drops the tag on 0x22 stores gives 22 sinks on Setup + 30 frames and loses `gcareadirector.getcurrentlevel`'s only sink: `flip_violations` names it (`CHANGED gcareadirector.getcurrentlevel: no sink (verdict safe)`) | `test_flip_implies_sink_bites_a_taint_bug` |
| No behaviour change from the hook | the A6 Python digest after adding `Vm.wrap_handlers`: 601 `D` lines, and the whole file (`T 398519 7c3b6a5fe0ba2d1a`), byte-identical to the one before; the 541 conformance programs write byte-identical traces with the pass attached | `osi_vm.py --run $OSI $T --frames 600 --keys … --digest`, before and after; `test_wrap_handlers_hook_changes_no_trace` |
| Fix 1: type-only sinks (review M6) | 27 of the 38 sites test only the type of the native's value on this run (`x == nothing`, `!= nothing`): all 12 of `getresolutionwidth`/`height`, `addbigsprite`'s, `gctimer.create`'s, all 3 of `getmp3player`, `gettoa`'s, `scslosislkcubicspline.create`'s, 1 of `getammo`'s 2 and 7 of `stringtoidentifier`'s 11 (there the other operand of the `==` is a nothing on this path). **7 natives are `UNSAFE type-only`** (every sink type-only): `getresolutionwidth`, `getresolutionheight`, `addbigsprite`, `gctimer.create`, `getmp3player`, `gettoa`, `scslosislkcubicspline.create`; on this run any value of the right type takes the same path. `getammo` stays UNSAFE by value (`this.x < 10`) | `osi_taint.py --osi $OSI --frames 600 --keys "down@100 down@160 enter@220 esc@280"`, the `type-only` marks |
| Fix 1: `stringtoidentifier` as tier1 (the reviewer's scratch override, which also sets `gcsaver.cheatmenu` tier2 with default int 0, its stub value) | `taint: natives tainted 68, sinks 28 (branch 28, …, tier1-arg 0, …, refstore 0, errpath 0), type-only 20`; `getscriptfluffstring` safe; the falsifier still holds (a CHANGED tier1 native satisfies it) | the same command with `--overrides` naming it `tier1`; `test_flip_implies_sink_with_a_tier1_override` |
| Fix 1: flip implies sink on the full A9 path (600 frames + the A9 keys; review M7, checked, not pinned) | 7 CHANGED, 9 CHANGED-NULL, 53 SAME, 0 violations: every CHANGED native UNSAFE, the 9 CHANGED-NULL natives the same set as on 30 frames | the reviewer's scratch probe `flip600.py` (`build/review/t5/`, not committed) against the fixed pass |

## M2a-2 Task 10: A8–A10 on `Base.osi` with real natives (2026-11-13; native Release Apple M1 Pro, wasm32 under Node for the stack rows; Beta 1.5.0 install; load average 5.8 to 48.9 from other sessions)

Base: `ad8789a` (= `d5ac541`, the F7 merge, + docs) plus this task's changes. `$OSI` is `$LOMN_DATA/install/data/Base.osi`, `$I` is `$LOMN_DATA/install`, `$K` is `down@100 down@160 enter@220 esc@280`, `$T` is `--natives data/osi/natives.json --mods data/osi/natives_beta150.json --types data/osi/types.json`. Outputs went to an untracked `build/` directory.

| Fact | Value | Command |
|---|---|---|
| A9 C++ leg | Setup 49,539 instructions; 600 frames, 391,395 instructions in all; **insns/frame p50 553, p95 553 (n = 600 of 600 frames)**; 1,705 native calls; natives/frame p50 1, p95 1 (n = 600); stop none; 601 `D` lines | `build/native/lomn_view --install $I --script 1 --natives real --frames 600 --keys "$K" --digest … --natives-log … --trace … --trace-frames -1:2 --out …` |
| A8 coverage report (real natives) | `coverage: natives distinct 69 (tier1 35, tier2 34, stub 0, mod 0, tier3 0), calls 1705, unregistered 0`; equal byte for byte to `coverage.py --log` on the run's natives.log and to `coverage.py --trace` on its full trace | the same run; `coverage.py --log …`, `coverage.py --trace …` |
| cmp 2/4, 3/4, 4/4 (the osi-a9 legs this task runs; cmp 1/4 is the controller's wasm leg) | `D` lines native == Python replaying the native natives.log (601 lines); native == Python **own models** (`--real-natives --install $I`); natives.log native == Python own models (2,462 lines); the trace window -1:2 (55,898 lines) and the full trace (399,247 lines) byte-identical native vs Python | `osi_vm.py --run $OSI $T --real-natives --install $I --frames 600 --keys "$K" --digest … --log-out …`; `osi_vm.py --run $OSI $T --frames 600 --keys "$K" --replay native.natives.log --digest …`; `cmp` |
| Taint with real natives (A10) | `taint: natives tainted 34, sinks 0 (branch 0, gamevar 0, tier1-arg 0, process-result 0, name 0, refstore 0, errpath 0), type-only 0`; every one of the 34 tier-2 natives `verdict safe`. One pass, no UNSAFE: the iteration is at its fixed point, and the worklist is **empty** (no UNSAFE non-tier-1 native, no tier3 or stub hit) | `osi_taint.py --osi $OSI --real-natives --install $I --frames 600 --keys "$K" --overrides data/osi/native_overrides.json` |
| Flip probe with real natives (Review focus 5) | 69 natives: **CHANGED 8, CHANGED-NULL 9, SAME 52** (30 frames, 19.2 s). All 17 are tier 1 in the merged overrides. Against the stub run (7 + 9): `gcareadirector.getcurrentlevel` is SAME (its real value is not 0, so the flip to 1 keeps the path); `gcmodeldirector.addtoa` and `gccharacter.setupmoveable` are new CHANGED (both tier 1). **Those 2 of the 17 measure side-effect loss, not return sensitivity** (fix 1, m3): in `real` mode the flipped native's model never runs, so `setupmoveable` loses its nested `OnEvent(0x36)` (the −310) and `addtoa` creates no Toa; the other 15 are return-value flips | `flip_probe.py --osi $OSI --frames 30 --natives-impl real --install $I` |
| `P` lines (the atof rounding chain) | **0** in the full real-natives trace, both engines: no string → float conversion switches the precision on this path, so there is no converted text to check | `grep -c '^P ' native.trace python.trace` |
| Engine-call nesting M (D15) | **2**, both engines (`nesting high-water 2`, `engine-call nesting: max 2 (cap 64)`); stack high-water 57. The two re-entrant chains, both in Setup: `gcmodeldirector.addtoa` → `C 00000442 OnEvent(0x32, 250)` and `gccharacter.setupmoveable` → `C 00000452 OnEvent(0x36, 0)` | the A9 run's stdout and natives.log |
| wasm32 bytes per level on the real chains, by stack-pointer delta (fix 1, I-1: the independent trap measurement is the next row; the two differ) | **1,616** (addtoa) and **1,600** (setupmoveable): the stack pointer at the outer `Vm::call("Setup")` entry, 592,256, and at the nested `Vm::call("OnEvent")` entries, 590,640 and 590,656 (611 entries in all: 609 at depth 1, 2 at depth 2). The run's `D` lines equal the native ones | a scratch build (not committed) printing `emscripten_stack_get_current()` at each `Vm::call` entry, `node build/wasm/lomn_view.js --install $I --script 1 --frames 600 --keys "$K" --digest …` |
| wasm32 stack `lomn_view --script` needs (`-sSTACK_OVERFLOW_CHECK=2`): the **lomn_view startup peak**, not the VM chain (fix 1, I-1) | traps at `STACK_SIZE` 12,976 (`stack overflow (Attempt to set SP …)`), passes at 13,232 with the same `D` lines as native; 512 KiB and 13,232 pass, 4,096 traps. The trapping stack is `__canonical` ← `refuse_install_outputs` ← `script_mode` ← `main` (the `std::filesystem` canonicalisation in the output-path check): no `Vm::call` frame is on it, so this is the startup peak (std::filesystem canonicalisation in the output-path check) and the VM chain peaks lower. It says nothing about bytes per level. Binary search in 16-byte steps down to a 256-byte gap, 13 builds | `-DLOMN_OSI_STACK_SIZE=<n>` on a scratch copy of the link option, `lomn_view` relinked per step, the A9 run under node |
| wasm32 bytes per level by the trapping `STACK_SIZE` on an engine-call chain (fix 1, I-1; independent of the stack-pointer delta) | **1,984 bytes per level**, linear over 16..64 levels. The chain is the D15 / I2 nesting-cap program `b-eng-reenter-depth` (`t.reenter` re-enters `main` while `context_depth < calls.depth`), assembled by `conform_assemble.py` with `calls.depth` = d and an `ok` expectation (scratch corpus, not committed), `lomn_osi_conform` built for wasm32 with `-sSTACK_OVERFLOW_CHECK=2`, `STACK_SIZE` varied at relink, 90 runs, each `pass` (`1/1 pass`) or `trap` (`stack overflow (Attempt to set SP …)`), bisected to a 32-byte gap. Trap / pass: d = 16: 42,736 / 42,768; d = 32: 74,464 / 74,496; d = 48: 106,208 / 106,240; d = 63: 135,968 / 136,000; d = 64: 137,968 / 138,000. bytes/level = Δ(STACK_SIZE)/Δd: 16→32 1,983 to 1,984, 32→48 1,984, 48→63 1,984, 16→64 1,984 (resolution ±32 bytes over the gaps). d = 1 traps at 19,312 and passes at 19,344: the conform runner's own startup peak, above its 11.0 K chain base (42,736 − 16 × 1,984 = 10,992), so d = 1 is not on the line and is not used. The cap chain (64 active calls) needs **138,000** bytes in all. **The trap figure is 23 % above the stack-pointer delta (1,616 / 1,600)**: the conformance chain re-enters through the runner's declarative native, which has larger frames than `addtoa` / `setupmoveable` | scratch `CMakeLists.txt` with `-sSTACK_SIZE=${LOMN_PROBE_STACK}` (restored, `cmp` against `git show HEAD:engine/CMakeLists.txt`), `emcmake cmake -S engine -B build/wasm-i1 -DLOMN_PROBE_STACK=<n> && cmake --build build/wasm-i1 --target lomn_osi_conform`, `node build/wasm-i1/lomn_osi_conform.js <one-program corpus> --osi-dir …` per size and depth |
| Decision 11 re-applied with the trap-bracket figure (fix 1, I-1) | M = 2: 64 ≥ 4 × 2, the cap stays **64**. 4.5 × 64 × **1,984** = **571,392 > 524,288**: the rule as written needs `STACK_SIZE` ≥ 571,392 (576 KiB = 589,824), not 512 KiB. With the stack-pointer figure it fitted (4.5 × 64 × 1,616 = 465,408). The cap chain traps only below 138,000 bytes, so 512 KiB is 3.8 × the need at the cap (the rule asks 4.5 ×). Not changed in fix 1 itself; the controller's ruling in the next row ("Decision 11, controller ruling (2026-11-13)") raised `STACK_SIZE` to 576 KiB | arithmetic on the rows above |
| Decision 11, controller ruling (2026-11-13) | `STACK_SIZE` raised from 524,288 to **589,824** (576 KiB) in `engine/CMakeLists.txt` (INTERFACE on `lomn_osi`): 4.5 × 64 × 1,984 = 571,392 ≤ 589,824. The cap stays 64. Cost: 64 KiB more linear memory per wasm instance; `lomn_web` does not link `lomn_osi` until M2b | the rule in `docs/osi/natives.md` Decision 11 and the row above |
| Host sources (C3): 900 frames, no keys | **0 `C -1` records** on both engines (the only `C` records are the 2 nested calls above); `D` lines native == Python own models over 901 lines | `lomn_view --install $I --script 1 --natives real --frames 900 --digest … --natives-log …`; `osi_vm.py --run $OSI $T --real-natives --install $I --frames 900 --digest … --log-out …`; `grep -c '^C -1'` |
| swallow_count (D16, item 3) | **0 nested calls in `LoadArea` blocks, 0 swallowed errors**: 0 `X` records in both natives.logs (600-frame A9 and 900-frame runs, both engines), 0 `E` and 0 `X` in the full traces; no `C` record carries `gcareadirector.setcurrentarea`'s sequence number (0x441). Expected 0 (natives.md §5.3) | `grep -c '^X '`, `grep '^C '` on the natives.logs |
| M2a-2 T12 fix a (C++): A9 leg after the block-file-slot, RemoveArea and GetString changes | 601 `D` lines and natives.log (2,462 lines, 0 `X`) **byte-identical** to the base Python engine's (`tools/research/osi_vm.py` at 8b26f05, unchanged in the fix-a tree; T10 measured it equal to the base native run): the changed paths (a firing area-loaded character, a failed `addtoa`, `RemoveArea` with no loader, a fluff entry with n = 0) are not on A9 | `lomn_view --install $I --script 1 --natives real --frames 600 --keys "down@100 down@160 enter@220 esc@280" --digest … --natives-log …`; `osi_vm.py --run $OSI <tables> --real-natives --install $I` (same frames and keys); `cmp` |
| event_probe with real natives (D8, item 4) | single shot `--at 0,1,10,100,599`: **235 injections, SAME 229, CHANGED 6** (227.4 s); the 6 are `OnEvent(0x28, 10000.0, 'hell'/'crsh'/'impc')` at frames 0 and 1, divergence at the injected frame (-2391 at frame 0, -2083 at frame 1). Repeat: listed frames 47 runs SAME 43 / CHANGED 4 (45.3 s); `2-599` 47 / 43 / 4 (divergences frame 2, and frame 4 for `(2, 'impc')`, as Task 2a); `all` 47 / 43 / 4 (frames 0 and 2). **No CHANGED outside frames 0–4.** The counts equal Task 2a's stub-run counts | `event_probe.py --osi $OSI --natives-impl real --install $I [--repeat --at …]` |
| Mutation bite on the own-models cmps (`mutate_natives.py`, Review focus 3) | **CAUGHT 6, control MISSED, rc 0**; each source restored and `cmp`-checked (7 of 7 `restore cmp OK`, 4 of 4 final). M1 stringtoidentifier byte-swapped, M2 cheatmenu → 1, M3 doesfileexist → 1, M5 getcurrentlevel + 1, M6 getcameraaimode + 1: CMP4 natives.log at the mutated native's own line; M4 CreateVariant's first pass without the variant's +1: CMP3 `D` line 1 (Setup). Two of the plan's six were **NO-EFFECT** on a first run and were replaced, not counted: the later-pass AddRef in `create_variant` (natives.log and `D` lines unchanged over 600 frames; fix 1, m4: the path runs once on A9 (`later:GcToa 1`, the review's per-class count of later-pass `create_variant` calls), `gcmodeldirector.getcharacter` re-minting the handle `gettoa` minted (0x51, natives.log seq 0x1bc and 0x1be), and the full trace has 0 `F g` records, so the Toa is never released in 600 frames; the `D` lines and natives.log carry no per-handle reference counts, so A9 cannot see this mutant; unit T3-2 and 3 more `osi*` cases catch it) and playcinematic not instant (`cs.playing` is not read on the A9 path; fix 1, m5: it is written and never read by either engine's code, `directors.cpp` `n_playcinematic` and `directors.py` `_play_cinematic`, so Decision 1 (the cinematic is complete when the native returns) has no falsifier on any script path; only the unit tests read it, `test_osi_directors.cpp` ~1594/1603 and `test_osi_natives_directors.py` ~1280/1314) | `mutate_natives.py <scratch tree> --all` (scratch copy configured `-DLOMN_WEBGPU=OFF`) |
| mutate_vm S07 (item 2) | the new unit test "T10 S07: a nested engine call's marker is site+6" fails under S07 and passes on the real code; `mutate_vm.py <scratch> --only S07` reports `unit` BITE, CAUGHT. The old test (test_osi_vm.cpp) never ran under S07 because the D15 cap test segfaults first and ends the doctest run. **Fix 1 (m2, and the controller's S01/S02 finding):** the `unit` check now runs one doctest process per test file (a crash in one file no longer hides another's bite; the loop exits non-zero when any file fails) and S07's EXPECT names only the new case. On a scratch copy, `mutate_vm.py <scratch> --only S01,S02,S07,CTRL`: S01 CAUGHT (unit BITE `FindMemberID miss-cache poisoning`, the existing EXPECT, a real bite on the defect: the case that pins the cache), S02 CAUGHT (same case), S07 CAUGHT (unit BITE `osi natives T10 S07: a nested engine call's marker is site+6`; tracecmp BITE `DIFF ./b/b-eng-reenter.trace`), CTRL MISSED, every restore `cmp` OK. No EXPECT was widened | `mutate_vm.py <scratch> --only S07` |
| N-1 (dispose before the M-7 throw) | a native returning a fresh string without its AddRef: the string is disposed before the re-raise (live strings unchanged, one `F s`); GREEN in `lomn_tests` and `lomn_tests_asan` (no leak report), RED before the fix | `lomn_tests -tc='*T10 N-1*'`, `lomn_tests_asan -tc='osi*'` |
| N-2 (the M-7 exit check's cost) | the check runs on 1,702 of 1,705 native returns, visiting 204,096 handle-map entries in all (max 155 live game handles). ABAB, 9 runs per arm, load 8.57 → 9.00: script ms/frame p50, median of runs, **0.2910 with the check [0.2860..0.2990] vs 0.2900 without [0.2850..0.3010]** (n = 9 runs × 600 frames per arm); setup ms median 32.337 vs 32.765. The difference (0.001 ms/frame) is far inside either arm's spread: no cost is resolvable | `n2.py 9` (two builds: the check, and the check replaced by an early return; not committed) |
| T6 m2 (coverage on error runs) | a registered native that fails its argument check before entry (getresolutionheight with 3 arguments) is now counted, so `lomn_view`'s report equals `coverage.py --trace` on error runs: `coverage: natives distinct 2 (tier1 2, tier2 0, stub 0, mod 0, tier3 0), calls 2, unregistered 0`; RED before the fail observer | `pytest tools/tests/test_lomn_view_script.py -k failing_before_entry` |
| Fix 1, m1 (0x83 call of an unregistered name) | 0x83's lookup is inside its try, so the miss is a call and an unregistered name in `coverage.py --trace`; `lomn_view` counted `calls 1, unregistered 0` (RED) and now prints `coverage: natives distinct 1 (tier1 1, tier2 0, stub 0, mod 0, tier3 0), calls 2, unregistered 1`, equal to `coverage.py --trace` on the run's trace (Setup calls `getresolutionwidth`, then 0x83 with the name `nosuchnative`). The hook is `Vm::set_native_miss_observer` on `bcl_83`'s `!e` branch; 0x82's miss (outside the try, `Call to unregistered Game Function` E line) was already counted | `pytest tools/tests/test_lomn_view_script.py -k 0x83_call` |
| Fix 1, m6 (`lomn_osi_run --coverage` sort) | it shares ScriptHost's hit table, so it now has the same pre-entry tie-break as `lomn_view`'s report. The only test of that output (`osi_a6.cmake` ~115-117) checks the `coverage:` line is present, not the row order, and no A6 program has a pre-entry failure, so none was added | reading `lomn_osi_run.cpp`; `grep -rn -e '--coverage' engine/tests tools/tests Makefile` |
| A15 row (`perf/osi-a9.jsonl`, no thresholds) | the final code's A9 native run: insns/frame p50 553, p95 553 (n = 600); script ms/frame **p50 0.371, p95 2.083 (n = 600)**, setup 58.596 ms, rss 21,561,344 bytes, `D` lines identical to the measure-first leg (601). Load average 48.86 at the start and the end, so the ms figures are a loaded host's (the measure-first leg at load 13.22 → 12.46 gave p50 0.318, p95 0.418) | `lomn_view --install $I --script 1 --natives real --frames 600 --keys "$K" --digest … --out …`, `os.getloadavg()` before and after |
