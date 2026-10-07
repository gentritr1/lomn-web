# M1a-3 faithful states: reference implementation report (REF3)

Built 2026-10-07 by an Opus subagent in an isolated scratch copy of BASE `f47cc91` (branch `m1a2-first-pixels`). The controller wrote this report from the builder's hand-back, because the builder's environment cannot write `.md` files. The controller's independent reproduction is recorded in the M1a-3 ledger.

## Bundle
- `files/`: repo-relative copies of the 23 changed and 16 added files.
- `engine.diff`, `makefile.diff` and `tools.diff`: these reproduce the work tree from BASE.
- `MANIFEST.txt`: sha256 of every file.
- `logs/`: make-check, mutations3, evidence3, asan-run, metrics and `a7_calls_r2_to_r3.diff`. Absolute paths are replaced.
- `scripts/`: `mutate3.py`, `evidence3.sh`, `pngdiff.py`, `bundle3.sh` and `peread.py`.
- The bundle contains no game data, renders or decompiled code blocks.

## Results (builder-executed)
- **`make check`:** exit 0, 1:44 wall.
  - pytest: 67.
  - Native ctest: 22/22.
  - wasm ctest: 4/4.
  - Traces and the A9 state log: native == wasm32.
  - Web smoke: pixels correct.
  - datacheck: 39/39 native and 39/39 wasm32.
  - New `datacheck-wasm`: the null and state logs equal native for vllg, ghut and crss on real data.
  - Worklist: 0 errors.
- **Unit tests:** 128 cases, 172,010 assertions.
- **GPU tests (Dawn/Metal):**
  - `gpu_ffp`: 14/14. The uber-shader matches the CPU reference of the D3D8 formulas within ±2.
  - `gpu_area`: the test scene (10 draws) and the stat scene (19 draws: fog, plnt alpha-test, glass, texture-transform probes).
  - `gpu_mesh`: the winding, depth-bias and out-of-frame probes.
- **ASan+UBSan:**
  - ctest 22/22 and unit 128/128.
  - Real renders: 0 px different from Release.
  - The 2-frame null and state logs are byte-identical to Release.
  - Survey: 123 areas, 0 failures.
  - One UBSan finding was fixed: a NaN converted to unsigned in `quantize_unorm8`. It is now NaN-safe throughout, with a regression test; the reintroduced bug is caught under UBSan.
- **A9 oracle:** the independent Python oracle (`tools/research/oracle/a9_oracle.py`) matches the shim line for line: stat 246, vllg 298, ghut 221, crss 330. A negative control is detected. It runs as the ctests `a9_oracle_synthetic` and `a9_oracle_real`; the real one exits 77 without `LOMN_DATA`.
- **Mutations:** 31 of 33 caught. The two survivors are the no-op control, which must survive, and the NaN mutation, which only UBSan catches (verified separately).

## Metrics (native, Metal, 1280×720, 60 frames)

| | vllg | ghut | crss |
|---|---|---|---|
| Models / draws | 10 / 31 | 1 / 30 | 14 / 42 |
| Pipelines | 3 | 2 | 4 |
| State calls per frame | 212 (218 on the first frame) | 137 | 234 |
| Shader variants | 5 | 4 | 8 |
| Lights | 23 | 5 | 2 |
| Fog | OSI, 40..1200 | default | default |

All three areas: load 8.6–24.1 ms, decode 6.6–7.0 ms, upload 3.6–10.1 ms, submit (first frame) 7.9–11.7 ms, submit (median) 0.25–0.31 ms.

**123-area survey: 0 failures.**
- Pipelines per area: max 4, mean 2.67.
- Shader variants: max 11 (lev1/clf2), mean 5.2.
- State calls: max 830 (lev6/ta03).
- Lights: max 209 (lev6/ta08).
- Fog from Base.osi in 53 areas.
- Light source: light_slb 51, light_slb plus default 48, default only 24.
- Lit draws: 0 (see D2).
- Null-texture draws: 16, all in fren/frnt, which reference missing textures.
- Unsupported state bits: 0.

**Uber-shader rather than generated WGSL,** decided from these counts. There is one WGSL module (3,902 B) with a 1,024 B uniform block per draw. Pipelines differ only in blend, depth write, depth func, cull and depth bias, which gives at most 4 per area.

## Render comparisons (differing pixels out of 921,600)
- **Determinism:** r1 vs r2/r3 is 0 px for every area. Null and state logs are identical across runs.
- **vs the M1a-2 unlit reference** (same camera, fog off):
  - vllg: 181,456 px, max 44.
  - ghut: 275,447 px, max 241. Two causes: vertex colours are now clamped (up to 320.6; 1,146 values above 1 and 3,086 below 0), and the net now draws as a sheet with TFACTOR alpha.
  - crss: 20,073 px, max 11.
- **Faithful vs fog off:** vllg 155,532 px (max 29). ghut and crss show 0, because the default fog distances are 4701/4702.

## Decisions (decompiled-function evidence)

**D1 (correction R17).** Area render modes are 0x14634, not 0x11.
- Sources: GcGame::SetUpRenderer 0x004395d0 and SetUpDatabase 0x004399e0, read in the disassembly.
- 0x11 is the ScRenderNodeGroup constructor flag.
- Modes: main 0x10635, HUD 0x8c034, back 0x10234, area/nolight 0x14634, trans 0x10034, shadow 0x4400.
- Mode bits, decoded in SrPushRenderMode 0x00531100:
  - bit 0 = LIGHTING
  - 0x400 = cull CW, 0x800 = cull CCW
  - 0x200 = fog
  - 0x10000 = W-buffer
  - 0x4000 = solid fill
  - 0x80000 = sprite mode (ALPHAREF 0 on entry, 1 on exit)

**D2.** LIGHTING is off for area, back and trans draws in normal play. Lighting only reaches those nodes through GcLightDirector::GetNearest 0x0048eb40 → GcRenderNodeGroup::SetLighting 0x00516c00, and only in night vision. `--lighting forced` models that path without its emissive term.

**D3.** ALPHAREF is 1 in the steady state, because the HUD mode 0x8c034 runs after the main view and the loading HUD runs before the first frame.

**D4.** The DrawX context flag (+0xc) is set only for plnt/tre1, by GcArea::Render 0x00505840 (asm 0x5058a8).
- DrawX 0x005528c0 then turns blending off and uses the alpha test only.
- `SetFog(false)` after plnt/tre1 leaves later areas unfogged until the next mode push. This is modelled.

**D5.** Cull is CW for area and nolight, NONE for back and trans. Clears:
- RenderLoop 0x0049d550: full target, Z 0.0 (0x70f7d0).
- ScRenderer::ClearRect: Z 1.0 (0x730f48) and colour 0x00000000.

This resolves R15. `begin_frame_ex` is not needed.

**D6.** W-buffer is on (mCanUseW = 1 at 0x74a324). We use Depth32Float (deviation V1).

**D7 (correction).** MAXANISOTROPY is never set, so anisotropy is 1. MAG LINEAR is set directly on the device on every GcArea draw (0x50584a), and MIN is ANISO, which is effectively linear. History-dependent HUD POINT filtering is not modelled.

**D8.** SRCBLEND/DESTBLEND are set in CreateDirectXWindow 0x0052e570. The fog statics are bit-cast (0x5e511e).

**D9. Fog and camera defaults.**
- Fog defaults at 0x74a338: start 4701, end 4702, density 0.001, colour 0xffffffff.
- Draw distance: 4700 (0x74a328).
- Camera: FOV 72 (0x704e00), near 0.01 (0x704e1c), far = GetDrawDistance (GcViewPort ctor 0x004479f0).
- Base.osi never calls SetDrawDistance or SetFOV.
- vllg's SetFog literals: colour 0xFFF9DCF9, start 40, end 1200, density 0.05. Cross-checked with the Python `osi.py` against the SAGESharp format.
- The class pattern "lego_area_<area>"+level digit is INFERRED; it maps 91 of 112 areas.
- levelType comes from asm 0x4ffd0d.

**D10. Lights.**
- LoadLights 0x00470010: point att1 = 1/(0.15·range); intensity ×1.2 when above 0.9; a default light when there is no 0x80 record.
- GrSetDefaultLighting 0x00470560 supplies the constants.
- GcFastLight setters from 0x0058d6a0.
- Cave intensity 0.25 (0x70bd68).

**D11. Materials.**
- LoadIndexed 0x00550060: ambient 0.7; `_glass` transparency 0; eye emissive table; alpha formats.
- LoadX 0x00553800 creates a material only for textured subsets.
- SetTexture 0x0054f260: the TFACTOR transparency path, and ALPHAARG2 leaking into the next draw.
- textures.slb (0x0046f270 / 0x00553da0) is matched with exact strcmp.

**D12. Draw order.**
- Trans: SortIt 0x00485bf0. GreatestZ 0x00554c20 is the greatest-Y vertex plus the unrotated position. Sorting is by xz distance, farthest first, with ids ending in 'w' last.
- Moveables: GetNearest 0x00487e90, nearest first.

**D13.** The ScDxStates redundancy filter (0x0064b1e0 / 0x0064b250) is modelled. Direct device calls are unfiltered.

## Formulas (CPU reference in `ffp.cpp`; `gpu_ffp` confirms them on the GPU)
- **Colour:** Me + Ma·(global ambient + ΣLa·att) + Σ Md·Ld·max(N·L, 0)·att.
  - Md comes from COLOR1 when the vertex has a diffuse colour.
  - Point light: att = 1/(a0 + a1·d + a2·d²), and 0 beyond range.
  - Specular is off.
- **Table (W) fog:**
  - LINEAR: (end − w)/(end − start)
  - EXP: e^(−d·w)
  - EXP2: e^(−(d·w)²)
- **Alpha test:** compares in 8-bit, round(α·255) against ALPHAREF.
- **Stages:** TFACTOR and the stage ops SELECTARG1/2 and MODULATE.
- **Texture transform:** COUNT2.
- **Vertex colours:** quantized to D3DCOLOR (clamped).

## Deviations
- **V1:** Depth32Float instead of the W-buffer.
- **V2:** ZBIAS maps to −1 depth-bias unit per step, capped at 16. UNVERIFIED against D3D8 hardware.
- **V3:** A NULL texture samples as white.
- **V4:** CULL_CW maps to Front with FrontFace CW. Verified by a GPU probe.
- **V5:** MovingLight placement is simplified (INFERRED).
- **V6:** Lights are packed per draw.
- **V7:** Lighting is computed in world space.
- **V8:** The clear's alpha is forced to 1.
- **V9:** No spot lights (none in the data).
- **V10:** Untextured materials are drawn (0 in the data).
- **V11:** Vertex colours are clamped where D3DXCOLOR conversion could wrap. INFERRED; listed for A8.

## rhi (additive)
- New: `FrontFace`, `depth_bias` (kMaxDepthBias 65536), and `front_face` in MeshPipelineDesc.
- The null log shows `front=cw` and `depth_bias=N` only when they differ from the default.
- Out-of-frame draws are rejected and counted in both backends.

## Logs
- **A7** was updated once (`logs/a7_calls_r2_to_r3.diff`):
  - pipelines are created lazily (4, uber WGSL, 1,024 B uniforms, front=cw);
  - samplers use aniso=1;
  - the uniform buffer is 10,240 B, with offsets in steps of 1,024;
  - bind groups are per (texture, sampler);
  - the quad's vertex checksum changed because colours are now quantized;
  - back and trans draws follow SortIt order.
- **A9** (new): `engine/tests/data/a9_state.txt`, 246 filtered lines, equal to the oracle output on native and wasm32.

## Carry-overs
- **Done:**
  - Out-of-frame draw parity.
  - wasm32 datacheck, plus native == wasm32 logs on real areas. NODERAWFS could not walk the symlinked install root; fixed with canonical paths in `disk_fs` and the walker.
  - Material floats in A4 (`mat_sum`).
- **Not needed:** `begin_frame_ex`.
- **Deferred to M1b:** access-log granularity.

## Open questions (for the user's A8 visual check)
1. Should ghut's net look the way the TFACTOR leak makes it look?
2. ghut's lightstone gradients: clamp or wrap?
3. vllg's pink fog 0xFFF9DCF9.
4. FOV 72 and near 0.01, compared with real screenshots.
5. Whether fog persists across areas (ghut and crss get the default fog only when loaded first).
6. sMaxObjectDist and the default moveable mTrans are not modelled.

## Proposed task split (from the builder)
- **T1:** rhi additions (front face, depth bias, out-of-frame parity) and the gpu_mesh probes.
- **T2:** lomn_d3d `device8` and `ffp` (uber-shader, CPU reference, gpu_ffp, unit tests).
- **T3:** the SAGE OSI reader, the textures.slb parser, and the Python `osi.py`.
- **T4:** area loader additions (materials, lights, script fog, special textures, colour quantization, greatest-Y).
- **T5:** SageFrame plus the AreaRenderer rewrite, the synthetic "stat" scene, the A7/A9 logs and the A9 oracle.
- **T6:** lomn_view CLI, survey and metrics; node disk FS; datacheck-wasm; material floats in A4.
- **T7:** documentation, evidence and measurements.
