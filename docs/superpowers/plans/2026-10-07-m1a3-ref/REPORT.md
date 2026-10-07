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

## Revision 2 (2026-10-07): the controller's review round

The builder ran every check below in the same scratch copy, and the repo was not touched. The bundle was rebuilt in place, and this REPORT is kept. New in the bundle:
- `tasks/`: the task chain.
- `scripts/`: `changes.py`, `build_tasks.py` and `apply_tasks.sh`.
- `logs/`:
  - `osi-*`, `fuzz-osi-*`, `ffp-oracle.log`
  - `task-chain.log` and `chain/T1..T7.log`
  - `renders-r2-vs-r1.log`, `oracle-r2.log`

**Task split.** B2 below replaces the r1 "Proposed task split" section above.

**r1 bundle defect.** The r1 bundle shipped a stray `engine/tests/data/a7_calls.txt.json`. It holds the timing metrics `lomn_view` writes next to `--out`; it was produced when the A7 log was regenerated. r2 deletes it, so remove it from the committed r1 as well.

### B1. OSI reader denial of service: fixed
**The pre-fix failure reproduced (executed).**
- **End to end:** `lomn_view` on a written synthetic install whose `Base.osi` was replaced (`tools/research/osi_craft.py calls`). The file is 600,337 bytes: 100,000 × CallGameFunction with argc 127.
  - Result: **485.8 MB max RSS**, against 9.8 MB for the benign install (`logs/osi-dos-prefix.log`).
- **In process:** the same `probe_osi.cpp`, compiled against the r1 `osi.cpp` (`logs/osi-probe-prefix.log`).
  - The `calls` probe grew memory by **+540.7 MB natively** and **+413.0 MB as wasm32**.
  - Next, `funcs-shared` (65,535 functions sharing one 32,767-instruction subroutine) **hung for more than 60 s** on both targets. The r1 `check_all` re-walked shared bytecode once per function.

**Caps.** Every cap is measured against the real `Base.osi` (Beta 1.5.0, 486,274 bytes) with `tools/research/osi_maxima.py`, which uses the independent Python reader (`logs/osi-maxima.json`).

| Quantity | Measured max (where) | Cap | Headroom |
|---|---|---|---|
| File size | 486,274 B | 8 MiB (was 64 MiB) | 17× |
| Instructions per subroutine | 1,707 (`setupshoefitterscreen`) | 32,768 (was 1,048,576) | 19× |
| Instructions per file (`check_all`, unique subroutines) | 151,522 (3,606 subroutines) | 4,194,304 | 27× |
| CallGameFunction per file (`check_all`) | 4,402 | 131,072 | 29× |
| Calls per method (`eval_calls`) | 78 (`onakoro_puzzle.initfull`) | 2,048 | 26× |
| argc per call | 9 | 127 | 14×: the s8 operand's own bound; negative argc is now an error (it was clamped) |
| Arguments per method (sum of argc) | 128 | 4,096 | 32× |
| String bytes copied into arguments per method | 570 (longest string: 145 B) | 16,384 | 28× |
| Literal-stack depth | 16 | 256 | 16× |

The argc cap is the only one under 16×, because the format cannot express more. The memory bound comes from arguments per method instead.

**Semantics.**
- A cap hit is an error with no partial state:
  - `parse()` leaves an empty `OsiFile` (the r1 version left partial tables);
  - `eval_calls()` leaves an empty list.
- `check_all` walks each distinct bytecode offset once, across functions and methods together.
- The `std::stoul` symbol round trip is gone.
- The area path (`load_script`) calls only `parse` and `eval_calls("initfull")`. An error there becomes a warning, and the area keeps its default fog.

**After the fix (executed).**
- **`osi_probe`** (a new ctest: `engine/tests/probe_osi.cpp`, native and wasm32 under Node, 2 GB cap) runs 9 crafted inputs. Each must end with its named cap error (`funcs-shared` must be accepted) within 5 s and +64 MB.
  - Release, native: every probe takes ≤ 0.02 s and adds ≤ +3.4 MB peak RSS.
  - Release, wasm32: every probe takes ≤ 0.031 s. Linear memory stayed at the initial 16 MiB except for building the 8 MiB input (19.2 MB).
  - ASan build: every probe takes ≤ 1.05 s and adds ≤ +10.8 MB (`logs/osi-probe.log`, `logs/asan-run.log`).
- **End to end:** the four crafted installs load with a warning at **12.5–15.5 MB RSS** in 0.02 s (`logs/osi-dos-postfix.log`). Under ASan the crafted installs give rc 0 and empty stderr.
- **Unit tests:** 3 new cases.
  - Every per-method cap sits exactly at its limit and one past it.
  - `check_all` walks shared bytecode once, and its total-instruction and total-call caps are hit.
  - A failed parse leaves no tables, and an oversized file is rejected.
- **Sanitizer mutation fuzz** (`engine/tests/fuzz_osi.cpp`, `make fuzz-osi`; ASan+UBSan with `-fno-sanitize-recover=all`; invariants: caps hold on every accepted input, and a failed parse or eval leaves nothing):

| Target | Seeds | Inputs | Accepted | Findings |
|---|---|---|---|---|
| Native | synthetic | 2,000,000 | 290,448 | 0 |
| Native | synthetic + real `Base.osi` | 40,000 | 9,926 | 0 |
| wasm32 (Node) | synthetic | 300,000 | 43,370 | 0 |
| wasm32 (Node) | synthetic + real `Base.osi` | 3,000 | 730 | 0 |

- **Canary:** `osi.cpp` with the PushString bounds widened by 8. Both targets stop on it: native aborts with rc 134, and wasm32 reports an ASan container-overflow. A zero-finding run is therefore meaningful (`logs/fuzz-osi-canary.log`).

### B2. The task chain builds and passes step by step
`scripts/apply_tasks.sh tasks <fresh BASE tree>` copies `tasks/Tn/files/` cumulatively over a fresh copy of BASE `f47cc91`. After every task it runs `make test-native test-wasm`. Each `tasks/Tn.txt` lists every file the task carries, marks it full or partial, and says what the partial hunk holds. After T7 the tree is byte-identical to the reference (`diff -rq` over `engine/`, `tools/` and `Makefile`). Results are in `logs/task-chain.log`, with per-task logs in `logs/chain/`:

| Task | Files | Content | `make test-native test-wasm` | Native ctest | wasm32 ctest |
|---|---|---|---|---|---|
| T1 | 6 | rhi additions + probes | rc 0, 34 s | 18/18 | 3/3 |
| T2 | 10 | lomn_d3d, gpu_ffp, ffp_oracle | rc 0, 13 s | 20/20 | 3/3 |
| T3 | 12 | OSI reader + caps, textures.slb, osi.py, osi_probe | rc 0, 20 s | 21/21 | 4/4 |
| T4 | 6 | loader data and the synthetic stat scene; A7 still M1a-2 | rc 0, 16 s | 21/21 | 4/4 |
| T5 | 14 | SageFrame + renderer; A7 regenerated, A9 introduced | rc 0, 19 s | 24/24 | 5/5 |
| T6 | 8 | survey / camera / Node disk FS / datacheck-wasm / mat_sum | rc 0, 27 s | 24/24 | 5/5 |
| T7 | 3 | `make fuzz-osi`, `osi_craft.py` | rc 0, 7 s | 24/24 | 5/5 |

Restructuring that made this possible:
- **`test_d3d.cpp` split into three:**
  - `test_d3d.cpp` (T2): d3d and ffp only;
  - `test_area_states.cpp` (T4): lights, fog and textures.slb from the loader;
  - `test_sage_frame.cpp` (T5): SageFrame plus A10.
- **`test_sage_osi.cpp`** builds its OSI with `tests/osi_builder.h` and no longer needs `mount_synthetic_install` (T3). `synthetic.cpp` writes the same program for the A9 scene.
- **textures.slb parser:** a new direct unit test in `test_sage_slb.cpp` (T3).
- **T4 keeps the M1a-2 renderer.** Vertex-colour quantisation moved to T5, because it changes the A7 vertex-buffer checksum. A7 keeps its M1a-2 content until T5, where it is regenerated, and A9 starts in the same task.
- **`lomn_view` is split:**
  - T5: `--synthetic 2`, `--state-log`, `--write-install`, `--lighting`, `--fog`, the `states` metrics, and `--alpha-ref` removed;
  - T6: the survey, the exe camera defaults and sources, `untextured_materials`, and the disk file system under Node.
- **T7 is tooling only:** `make fuzz-osi` and `osi_craft.py`. `osi_maxima.py` sits in T3 with the caps it justifies.

### S3. An independent check of the FFP formulas
- **`tools/research/oracle/ffp_oracle.py`:** written from the DirectX 8.1 SDK formula pages in float64, not from `ffp.cpp`. It covers:
  - lighting: ambient, diffuse, emissive, point attenuation with range cut-off, directional;
  - the COLOR1 source;
  - LINEAR / EXP / EXP2 fog;
  - the 8-bit alpha-test comparison;
  - stage-0 operations and arguments.
- **`engine/tests/ffp_cases.cpp` (`lomn_ffp_cases`):** dumps every input the C++ reference reads, plus its results. The cases are:
  - gpu_ffp's 14 cases, through the shared `tests/ffp_cases.h`, which `test_gpu_ffp.cpp` now draws from;
  - 4,000 seeded random samples: rigid world matrices, 0–8 lights, every op and argument, alphas on the 8-bit grid half the time.
- **ctest `ffp_oracle`:** 4,014 cases with 0 mismatches.
  - Max |vertex difference|: 1.3e-5; max |fragment difference|: 1.2e-7.
  - 2,875 fragments kept and 1,061 discarded, all agreeing.
  - 78 fragment checks skipped: FOGENABLE with both fog modes NONE, where D3D8 takes the factor from the specular alpha. The area path never does this.
- **Conventions shared on purpose:** an unbound texture samples white (V3), and alpha is 1 when blending is off.
- **Known limit:** the C++ reference and the shader transform normals by W. The oracle uses W⁻ᵀ. The two agree for the rigid worlds the area path builds (RotX × translation), but a scaled world would diverge (forward note F3).
- **Mutations to the C++ reference:**
  - attenuation drops a2: caught only by `ffp_oracle`;
  - the alpha test truncates instead of rounding: caught only by `ffp_oracle`;
  - EXP2 squares only w: caught by `ffp_oracle` and `gpu_ffp`.

  The first two are invisible to gpu_ffp, whose cases use a2 = 0 and exact-byte alphas.

### S4. The call finders, renamed
- **C++:** `OsiFile::find_calls` is now **`eval_calls`**, the same algorithm and name as Python's `osi.eval_calls`.
- **Python:** `find_calls` is now **`find_plain_push_calls`**, and the CLI flag `--calls` is now **`--plain-calls`**. The docstring explains that a computed argument such as vllg's `0 - 40` reads as None and the slots shift, so the fog colour prints 0.
- **New CLI:** `osi.py Base.osi --eval CLASS METHOD`. For `--eval lego_area_vllg initfull` it prints SetFog(−402183, −40.0, −1200.0, 0.05), which equals the C++ value.

### S5. User-eyeball gates (A8) and forward notes
**A8 gates.** None of these is closed by code. Each needs the user to compare against the original game.
- **G1, ghut net:**
  - Fable confirmed the TFACTOR leak is faithful to the code.
  - Still to check: does the original show the net as a translucent sheet, like `ghut.png`?
- **G2, vllg fog colour:** pink-white 0xFFF9DCF9 from 40 to 1200 (`vllg.png` against `vllg_nofog.png`).
- **G3, vertex colours out of [0, 1] are clamped:**
  - Ghut has 1,146 values above 1 and 3,086 below 0. On D3DXCOLOR → DWORD the original could wrap instead.
  - Where to look: the gradients on ghut's lightstones.
- **G4, camera framing:** FOV 72 (vertical) and near 0.01, compared with screenshots of the original at the POS start.

**Forward notes.**
- **F1 (V2, ZBIAS → depth_bias −1 per step):** check on a device at the first moveable or mTransparency < 1 fade (M1b/M2). The original sets ZBIAS 16 for faded models, and the WebGPU depth-bias scale is not the D3D8 one.
- **F2 (8 lights and V6, per-draw light packing):** revisit when lighting turns on (night vision, M2). Then check:
  - GetLightID cycling over 8 slots;
  - which lights each draw sees;
  - the look against the original.
- **F3:** non-rigid world matrices (any future scaled placement) need W⁻ᵀ normals, both in the shader and in the CPU reference.

### Must-hold results (executed on the final tree)
- **`make check`:** exit 0 in 2:01 (`logs/make-check.log`).
  - pytest: 67.
  - Native ctest: 24/24. New: `ffp_oracle` and `osi_probe`.
  - wasm ctest: 5/5. New: `osi_probe`.
  - Traces and the A7/A9 `cmp`s identical.
  - Web smoke pixels 255,128,0 / 0,0,64.
  - datacheck: 39/39 native and 39/39 wasm32.
  - native == wasm32 on the vllg/ghut/crss null and state logs.
  - worklist: 0 errors.
- **Unit tests:** 132 cases, 172,064 assertions.
- **ASan+UBSan** (`logs/asan-run.log`):
  - ctest 24/24 and unit 132/132;
  - `osi_probe` bounded;
  - vllg/ghut/crss renders: rc 0, empty stderr, 0 px different from Release;
  - 2-frame null and state logs identical to Release;
  - survey 123/0 with empty stderr;
  - crafted installs: rc 0, empty stderr.
- **Renders against r1 evidence:**
  - vllg, ghut and crss PNGs are **byte-identical**, and crss at yaw 67.5 differs by 0 px.
  - The 2-frame null call logs and D3D8 state logs are identical: 579, 422 and 660 lines.
  - In the 123-area survey, 0 areas differ outside timing fields; maxima and frames are equal (`logs/renders-r2-vs-r1.log`).
  - There are no differing pixel classes to explain.
- **A9 oracle:** synthetic stat 246 lines equal; real vllg/ghut/crss equal (ctest `a9_oracle_synthetic` and `a9_oracle_real`).
- **Mutations:** **40 of 42 caught**. 9 are new: 6 in the OSI caps (caught by `unit` and `osi_probe`) and 3 in the CPU reference (caught by `ffp_oracle`). The 2 survivors are expected: the no-op control, and the NaN mutation that only UBSan catches. After restoring, the build rc is 0 and the full ctest rc is 0 (`logs/mutations3.log`).
