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
