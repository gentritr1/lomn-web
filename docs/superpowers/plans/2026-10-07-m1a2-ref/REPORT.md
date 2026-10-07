# M1a-2 first pixels: reference implementation report (condensed)

Built 2026-10-07 by an Opus subagent in an isolated scratch copy of BASE `443bd37` (branch `m1a1-data-path`). The controller wrote this report from the builder's hand-back; the builder's environment could not write `.md` files. The controller's independent re-verification is recorded in the SDD ledger.

## Contents (`MANIFEST.txt`)

**Changed (5 files):**
- `Makefile`: `test-wasm` also runs `cmp` on the native and wasm32 A7 logs.
- `engine/CMakeLists.txt`
- `engine/include/lomn/rhi.h`
- `engine/src/rhi/null_device.cpp`
- `engine/src/rhi/webgpu_device.cpp`, restructured (381 diff lines). M0 objects are now created through a shared registry.

**Added:**
- `engine/src/rhi/registry.h`: validation shared by both backends. Bad ids or sizes create nothing (id 0) or are rejected and never reach Dawn.
- New library `lomn_area`:
  - `view/math.*`: D3D row-vector matrices, `PerspectiveFovRH`, rotation about X.
  - `view/mips.*`: CPU box-filter mips.
  - `view/area.*`: area to draw list to rhi.
  - `view/synthetic.*`: the scene for the A7 test.
- `engine/tools/lomn_view.cpp`: CLI that writes a PNG plus `<png>.json` metrics, or a call log with `--backend null`, or the 123-area survey.
- Tests:
  - `test_rhi_mesh.cpp`, `test_view_mips.cpp`, `test_view_area.cpp`
  - `test_gpu_area.cpp`: Dawn pixel probes plus a blend probe; returns 77 when there is no adapter.
  - `a7_calls.cmake` and `data/a7_calls.txt`: the checked-in expected null call log.

`engine.diff` and `makefile.diff` reproduce the work tree from BASE.

## rhi additions (additive; M0 calls log byte-identical text)

- **New calls:**
  - `create_render_target_ex` with Depth32Float
  - vertex and index buffers, filled by the existing `write_buffer`
  - `create_texture` and `write_texture` per mip (RGBA8Unorm)
  - `create_sampler`
  - `create_mesh_pipeline`
  - `create_bind_group`: uniform with dynamic offset, plus texture and filtering sampler, in a fixed layout
  - `draw_indexed`
  - `FrameStats.indices`
- **Defaults:** the new virtuals have "unsupported" defaults rather than being pure. Making them pure broke M0's test double.
- **Alpha test:** a `discard` in the `fs_alpha` entry point. WebGPU has no fixed-function alpha test.
- **M0 pipelines in a depth pass:** compare Always with no depth write, so their output is unchanged.

## Mips

- A full chain down to 1×1.
- Each texel is the exact area-weighted mean of what it covers; odd sizes split the middle texel.
- Integer round-half-up and straight alpha.
- Each level is built from the previous one.

## What is drawn

- **Order:** all routes that are not ignored, as main, moveable, no-light, back, then trans sorted far to near. Within an object, opaque subsets come before alpha subsets (DrawX's two passes).
- **World transform:** `RotX(−1.5707982)` then translate (v' = v · Rx · T). Moveable orientation is not applied: none of the three areas use it, and about 20 records elsewhere do.
- **Missing assets:** a missing texture becomes 1×1 white with a warning, and a missing model is skipped with a warning. `Art.blk` is optional.

## Unlit shading (preliminary A8)

- **Colour** is texture rgb × diffuse. Diffuse is the vertex colour if present, else the material colour: D3D8's `DIFFUSEMATERIALSOURCE = COLOR1` rule with the light term removed. **Alpha** is the texture alpha.
- **Why vertex colours:** they carry baked shading. With `--vertex-colors off` the renders look flat.
- **Alpha subsets** (DXT3, A8R8G8B8, TGA with alpha bits) use SRC_ALPHA/INV_SRC_ALPHA blending. They have an alpha test of "greater than 1/255" and depth write on. **Destination alpha is preserved.**
  - This fixes a bug that mutation testing found: blending also blended the framebuffer alpha, leaving 75 (vllg) and 14,560 (ghut) semi-transparent PNG pixels.
- DXT5 and DXT1 subsets draw opaque.

**Spec correction (proposed):** the decompiled DrawX sets ALPHAFUNC = 5 = D3DCMP_GREATER, not GREATEREQUAL as spec §2.3 says. ALPHAREF 1 is inferred from `SrPushRenderMode`.

## Camera fallback (all CLI-overridable; real values live in Base.osi scripts)

- Eye at the first POS record with flag 0x1000, plus 3.0 up (inferred).
- Yaw toward the xz centre of the placed geometry's bounds. Pitch −10°.
- FOV 60° vertical, near 0.5, far 4000. Setting far to 100,000 changes at most 3 of 921,600 pixels.
- Projection is `D3DXMatrixPerspectiveFovRH` in its documented form: `_33 = zf/(zn−zf)`, `_34 = −1`, `_43 = zn·zf/(zn−zf)`. It maps view z = −zn to 0 and −zf to 1, which is already WebGPU's range, so the matrix is used unchanged.
- D3D row-major matrices are uploaded as-is, so WGSL `M * v` equals D3D `v * M`.

## Caps (survey of all 123 area blockfiles)

| Cap | Measured maximum (area) | Value |
|---|---|---|
| Drawn placements per area | 55 (lev6/ta03) | 1024 |
| Draws per area | 165 (lev6/ta11) | 4096 |
| Textures per area | 76 (lev2/vllg) | 1024 |
| Texture bytes per area (RGBA8 with mips) | 57.0 MB (lev0/frnt) | 512 MiB (wasm32 heap) |
| Geometry bytes per area | 1.27 MB (lev6/ta14) | 64 MiB |

The boundaries are unit-tested. A deterministic 1,500-case mutation fuzz runs natively, as wasm32 and under ASan. Accepted areas render with zero rejected calls, and rejected areas leave no partial state.

## Results (builder-executed)

| Check | Result |
|---|---|
| `make check` | Green. pytest 67; native ctest 10/10; wasm ctest 3/3; native = wasm32 for the determinism log and the A7 log; web smoke pixels; datacheck 39/39; worklist 0 errors |
| ASan+UBSan | ctest 10/10 including the Dawn GPU tests; unit 93/93. On real data the survey and the 3 renders give rc 0 with empty stderr; the ASan render is pixel-identical to Release |
| Unit suite | 65 → 93 cases |
| Mutations | 17 of 18 caught. The missed one (additive blend) exposed the destination-alpha bug; after the fix and a new blend test, `mutate2.py` catches it. The no-op control survives |
| Determinism on real data | 3 runs per area give pixel-identical PNGs; the null logs are identical |
| Oracle consistency | vllg draws exactly the A6 set. Over all 123 areas: 183 main / 136 trans / 35 back / 943 moveable (the A3 totals), with 0 missing models |

**Real-data metrics** (1280×720, Dawn/Metal, 3 runs; the upper end of each load range is the cold-cache run):

| | vllg | ghut | crss |
|---|---|---|---|
| Models / draws per frame | 10 / 31 | 1 / 30 | 14 / 42 |
| Alpha subsets | 4 | 2 | 10 |
| Vertices / triangles | 5,700 / 5,235 | 5,268 / 5,962 | 6,080 / 6,207 |
| Textures (alpha on) | 27 (4) | 30 (2) | 37 (6) |
| Texture bytes, RGBA8 + mips | 6,995,932 | 10,052,912 | 26,678,564 |
| Pipelines | 2 | 2 | 2 |
| Load ms | 7.3–24.1 | 6.0–20.5 | 11.0–108 |
| Decode + mips ms | 6.8–7.3 | 9.5–9.8 | 24.2–25.3 |
| Upload ms | 4.4–7.4 | 4.9–5.3 | 8.4–16.8 |
| CPU submit ms, first frame / p50 | 3.5–8.5 / 0.2 | 3.6–4.0 / 0.2 | 3.4–10.1 / 0.2 |

GPU frame time was not measured, because there are no timestamp queries.

## What the PNGs show (viewed by the builder and the controller, not yet compared with the game)

- vllg (a lava cave village) and ghut (a hut interior) are upright and textured.
- Alpha foliage, vines and the rope net show no black boxes.
- crss's default yaw faces a canyon wall; `--yaw 67.5` looks down the tunnel.
- The ghut lightstones and the table are fully saturated from their vertex colours. This is unresolved until the A8 comparison.
- All three are darker and flatter than the lit game, because there are no lights, fog or emissive yet.

## Gaps M1a-3 must close

1. Lighting: default lights and `_LIGHT.slb`, plus the emissive and `_glass` overrides.
2. Fog around `plnt`/`tre1`.
3. Exact alpha states: ALPHAFUNC, the real ALPHAREF, the DrawX flag at `*param_2+0xc`, and the ZBIAS/TFACTOR path when transparency is below 1.
4. The real director draw order, the trans sort direction, and how the sky is handled relative to the camera.
5. Base.osi camera values, the start heading, and bit-faithful D3DX matrices.
6. The sampler (in D3D, anisotropy applies to minification only), `textures.slb` UV scroll, and the texture transform.
7. Moveable orientation.
8. The clear-Z disassembly check (it decompiles as 0.0; 1.0 is used) and W-buffer vs Depth32Float.
9. Native = wasm32 is proven on the synthetic scene only, because the disk file system does not exist in the wasm build (see M1a-1 final review R1).

## Open questions and risks

- **Spec amendment:** ALPHAFUNC GREATER.
- **A8:** the user captures each area's start view. Our view is matched with `--yaw`/`--pitch`/`--fov`, and those values become M1a-3 data.
- **ghut stone colours:** unresolved until the A8 comparison.
- **Survey oddities (data, not bugs):** `levels/fren/frnt.blk` has 16 textures that don't resolve. `lev4/lev4c.blk` can't be reached with a 4-character area id (it truncates to `lev4`).
- **Texture memory:** RGBA8 with mips is about 5.3× the DDS size (vllg 7.0 MB vs 1.32 MB), and lev0/frnt reaches 57 MB. This matters for M1b's browser memory budget.

## Reproduce

```bash
make check
make wasm && cmp build/native/a7/a/calls.txt build/wasm/a7/a/calls.txt
source tools/env.sh
build/native/lomn_view --install "$LOMN_DATA/install" --level lev1 --area vllg --out vllg.png
build/native/lomn_view --install "$LOMN_DATA/install" --level lev4 --area crss --yaw 67.5 --out crss_yaw67.png
build/native/lomn_view --install "$LOMN_DATA/install" --level lev2 --area ghut --backend null --out calls.txt
build/native/lomn_view --install "$LOMN_DATA/install" --survey 1 --out survey.json
build/native/lomn_view --synthetic 1 --backend null --out a7.txt && cmp a7.txt engine/tests/data/a7_calls.txt
```

To run under the sanitizers, configure `build/asan` with these flags, then run `ctest --test-dir build/asan` and `build/asan/lomn_tests`:

```bash
-DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
-DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
```

The scripts are in `scripts/`: `evidence.sh`, `farcheck.sh`, `mutate.py`, `mutate2.py`.
