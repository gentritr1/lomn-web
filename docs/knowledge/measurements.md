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
| Files / bytes | 529 / 1,102,338,267 |
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
