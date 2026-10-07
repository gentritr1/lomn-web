# LOMN Web — M1 design spec (level viewer)

Date: 2026-10-07 · Status: approved by controller in autonomous mode after a Fable 5.1 review; revision 3 adds measured corrections from the M1a-1 reference build (§7) · Parent spec: `2026-10-07-lomn-web-port-design.md` (§8 M1a/M1b, §12 amendments)
Evidence: `docs/knowledge/research/area-loading.md`, `docs/knowledge/asset-survey.md`, `docs/knowledge/measurements.md`, `docs/surface.md`, Fable review 2026-10-07. VERIFIED/INFERRED tags are carried over from those sources.

## 1. Goal

Load any area of the Beta 1.5.0 data **the way the original engine does**, and render its static geometry with the original textures, vertex colours, lighting, alpha passes and render states.

- **M1a:** natively first, as headless PNGs.
- **M1b:** in the browser via bring-your-own files.

Behaviour is defined by the decompiled functions named here (addresses in the research doc). It is reproduced, not reinvented.

## 2. What the original does

### 2.1 Loading
1. **Level entry** (`GcAreaDirector::SetCurrentLevel`): makes `levels/<lev>/<lev>.blk` current, then loads `material.slb` and `textures.slb`.
2. **Area entry** (`SwitchCurrentArea` → `GcAreaLoader::LoadArea`):
   - Flushes the model, texture and hierarchy caches.
   - Opens `levels/<lev>/<area>.blk` as the area blockfile.
   - Runs the loaders in order: OBJ, POS, LIGHT, SPOT, TRIGGER, CHAR, INSTANCE, SOUNDS, CONV, EMIT, VINE, HIVE.
3. **Lookup** (`SrBlockFileLoad`/`SrBlockFileExists`, `ScBlockFile::Find`):
   - The directory part of the path is dropped, and the name is binary-searched case-insensitively.
   - `Load` searches area → level → Art. `Exists` searches Art first. **Both orders are reproduced.**
   - Entries are read on demand.
   - Game paths are mixed case (`Root/Data/Levels`, `Characters`) while the disk has `levels` and `characters`, so **every VFS lookup is case-insensitive**.
4. **SLB:**
   - A little-endian memory image.
   - The trailing `0x00C0FFEE`, preceded by `count` and `count` pointer-field offsets.
   - Each pointer field holds a data-relative offset to fix up.
   - Fields may sit unaligned. The reader reproduces the original's offset masking exactly as `GetSLKFile` does; the plan states the mask after re-checking the decompiled code.
   - Cinematic tables (`cin*_obj.slb`, 11 files) use a different header and are out of scope.
5. **Placement** (`GcObjectLoader::LoadObjects`): `<area>_OBJ.slb` holds 60-byte records `{u32 id4cc @0, vec3 pos @4, vec3 orient @0x10, u32 unused @0x1c, vec3 ext0 @0x20, vec3 ext1 @0x2c, u32 flags @0x38}`.

   Routing follows the **decompiled test order**:

   | Test | Destination |
   |---|---|
   | `0x10` and `0x20` | moveable |
   | `0x10` and `0x100` | trans (water/glass) |
   | `0x10` (otherwise) | main |
   | `0x20` (without `0x10`) | no-light moveable |
   | `0x8` (without `0x10`/`0x20`) | back (sky) |
   | anything else | ignored |

   On Beta data this gives 183 main / 136 trans / 35 back / 943 moveable (the 34 records with flags 0x131/0x132 are moveable). It differs from a 0x8-first order only for flags such as 0x18/0x28/0x118, which Beta never uses (pinned by a unit test). Model path: `<area prefix>/Xs/<id>`.
6. **World transform:** `RotX(-1.5707982)` (constant @0x72a444), then the record position written to `_41.._43`. Row-vector D3D convention.

### 2.2 Models and textures
- **Model files:** binary `.x` (`xof 0303bin 0032`), loaded via `D3DXLoadSkinMeshFromXof` → `ConvertToBlendedMesh`. If a file has several meshes, **the last mesh wins**. Only `characters/bull.blk/mklf.x` has 3 meshes, and it is out of scope.
- **Template data in use:** Mesh, MeshNormals, MeshTextureCoords, **MeshVertexColors** (1,990/2,653 files), MeshMaterialList, Material, TextureFilename.
- **Template data unused:**
  - `FrameTransformMatrix` is identity in every sampled file, and the game ignores it.
  - `VertexDuplicationIndices` appears only as a template declaration, never as data.
- **Vertex format:** position, normal, **diffuse (vertex colour) when present**, one UV set. The attribute table is sorted by attribute id; each subset is drawn with `DrawIndexedPrimitive`.
- **Textures** (`ScDrawableTexture::Load`):
  - Extension-less names resolve to `<lev>/textures/<name>`, trying `.dds` then `.tga`.
  - Only materials used by a subset are loaded.
  - Alpha is on for DXT3 and A8R8G8B8, off for DXT5.
  - DDS files ship with mip count 0, and D3DX generates mips.
- **Materials** (`ScDrawableMaterial::LoadIndexed`):
  - Diffuse, specular, power and emissive come from the `.x`; Ambient is forced to a constant.
  - Then the name-substring overrides apply (`_glass` changes diffuse/transparency; `eye`/`on_`/`ga_`… set emissive).

### 2.3 Rendering (the fidelity target)
- **Camera:**
  - `ScPerspectiveCamera(fovDeg, near, far, aspect)` → `D3DXMatrixPerspectiveFovRH` (**right-handed**; degrees × constant @0x70d508).
  - The camera classes have no native callers, so FOV, near and far come from `Base.osi` script literals. M1 mines them from the OSI; until they're mined, the viewer uses a documented fallback and flags it.
  - The viewer's start camera is the area's first POS record with flag `0x1000`.
- **Global state** (`AppMain`, `CreateDirectXWindow`):
  - `D3DRS_AMBIENT` black.
  - MAG/MIN filter ANISOTROPIC (max 3), MIP filter LINEAR.
  - Addressing never set, so WRAP.
  - Z enabled (a W-buffer if caps allow; WebGPU uses a depth buffer — a documented deviation), `ZFUNC LESSEQUAL`.
  - Clear colour 0xff000000. The clear Z value needs a disassembly check: it decompiles as 0.0.
- **Render mode 0x11** (`SrPushRenderMode`, used by area/back/trans nodes): LIGHTING on, **CULL NONE**, alpha blend off, ZWRITE on, fog off. `GcArea::Render` forces MAGFILTER LINEAR per draw.
- **Lighting:**
  - `GrSetDefaultLighting(levelType)`: type < 3 gives ambient light `LmbA` (0.47, 0.47, 0.47); otherwise a directional `SunL` (0.856, −0.516, 0.033) plus a `Shad` fill.
  - `_LIGHT.slb` records add point lights (0x40) and directional lights (0x80, with an opposite fill).
  - **Colour source:** the D3D8 default `DIFFUSEMATERIALSOURCE = COLOR1`, which the exe never changes outside morph models. So **the lit diffuse comes from the vertex colour when present**, otherwise from the material.
  - The plan states where `levelType` comes from, after reading the call site.
- **Fog:**
  - Table fog LINEAR; colour/start/end from `GcGraphicsOptions` statics (0xffffffff / 4701 / 4702 @0x74a338).
  - Scripts set it via `GcAreaDirector::SetFog`, which **negates** start/end.
  - `GcArea::Render` enables fog only while drawing `plnt`/`tre1`. M1 reproduces the defaults and flags the script-driven values as pending until the OSI VM exists (M2).
- **DrawX two passes** (0x5528c0):
  1. **Opaque subsets:** ZBIAS 0. When `mTransparency < 1`, ZBIAS 16 plus a TFACTOR alpha.
  2. **Alpha subsets** (`IsAlphaOn`: DXT3/A8R8G8B8):
     - ALPHABLEND on, ALPHATEST on, `ALPHAFUNC GREATER` (corrected 2026-10-07: DrawX 0x5528c0 sets render state 0x19 = D3DRS_ALPHAFUNC to 5 = D3DCMP_GREATER; GREATEREQUAL would be 7);
     - ALPHAREF 0 or 1 per the code;
     - ALPHAOP SELECTARG2(TEXTURE).
- **Draw order:** MovingLight, Area, NoLight, StaticLight, Shadow, Main, **Back (after Area)**, Trans. Trans is sorted by camera distance (`GcTransDirector::SortIt`). Child iteration order is INFERRED; the plan reads it before implementing.
- **Texture transform:** `TEXTURETRANSFORMFLAGS = COUNT2` when a UV offset ≠ 0. A static offset applies in M1; scrolling animation does not.

## 3. Scope and plan split

M1a is split into four plans, ordered so the first end-to-end pixels arrive early:

| Plan | Content | Acceptance |
|---|---|---|
| **M1a-1 Data path** (no GPU) | Case-insensitive VFS lookup; BLK reader (+LZSS); block lookup chain with both orders; SLB reader + OBJ/POS/LIGHT views; binary `.x` tokens → mesh (incl. vertex colours, last-mesh-wins, attribute table); DDS (DXT1/3/5, A8R8G8B8) + TGA loaders | A1–A6 |
| **M1a-2 First pixels** | Unlit, textured draw of an area straight on `rhi`. RotX + POS-start camera, RH projection with the fallback FOV. `lomn_view` CLI → PNG. RGBA8 upload with CPU box-filter mips. | A7, A8 preliminary |
| **M1a-3 Faithful states** | D3D8 subset shim (methods in `docs/surface.md`) + fixed-function → WGSL (vertex-colour lighting, default lights, two-pass alpha, fog, filters, draw order + trans sort). Base.osi camera and fog literals mined. | A9, A10, A8 final |
| **M1a-4 Hardening** | Fuzz corpus + bounds rules (§5); `perf/history.jsonl` entries for load and frame; texture cache flag | A11, A12 |

**M1b** comes after, with its own plan:
- the bring-your-own-files page (Chromium handle path plus the OPFS fallback, quota check, validation);
- a memory VFS with per-area prefetch from access logs;
- the viewer UI and the `?hud=1` overlay (A13).

**Out of scope:**
- characters (CHAR, `.bhd`/`.bkd`) and moveable-object behaviour (moveables are drawn static at their record pose);
- triggers, sound, conversation, emitters, vines, hives, cinematics;
- collision (`.ocl`/`.bcl`/`.cdx`) and OSI scripts;
- sun flare and animated textures.

## 4. Acceptance (observable; measured, never invented)

| # | Check | Oracle (independent) |
|---|---|---|
| A1 | All **189** `.blk` open; per-file entry count and name order match | `tools/research/blk.py` |
| A2 | For 9 sample areas, every texture name resolves to the same blockfile and entry under **both** `Load` and `Exists` orders | `tools/research/texres.py` extended with the Exists order |
| A3 | All area OBJ files (excluding the 11 `cin*_obj.slb`) parse. Routing counts with §2.1 precedence: 183 main / 136 trans / 35 back / 943 moveable (incl. the 34 0x131/0x132) | research survey |
| A4 | All 2,653 `.x` parse; `failed == []`; meshes-per-file = 1 except `bull.blk/mklf.x` = 3. Vertex/face/subset counts equal the independent parser's for every file. | `tools/research/xbin.py` |
| A5 | All 3,241 non-empty DDS decode; the 6 zero-byte entries are rejected. DXT1/3/5 blocks match an independent decoder **bit-exactly** on every block of 20 sampled textures (181,568 blocks), using the Direct3D BC definition (endpoint = code/(2^n−1), exact interpolation, round to nearest; DXT3/5 colour always in 4-colour mode). | independent Python decoder (`Fraction` arithmetic) |
| A6 | The set of models drawn per area = the OBJ records routed to main/trans/back/moveable, never the whole blk (vllg must not draw `main1`, `tre1`, `rock`, `drll`) | OBJ parse |
| A7 | L1 contract: one `.x` → shim/rhi → null call log + Dawn PNG; per-frame draw/pipeline counts reported | null backend |
| A8 | Visual: lev1/vllg, lev2/ghut, lev4/crss from the POS start. Preliminary (unlit) after M1a-2; final after M1a-3. **User eyeball** against original-game screenshots the user captures. Confirms or corrects RotX(−90°) and handedness. | user (deferred to their check-in; does not block other plans) |
| A9 | **State log:** null backend, one vllg frame. The recorded state sequence equals §2.3 (render mode 0x11 states, filters, two-pass alpha states for DXT3 subsets, fog toggled only around `plnt`/`tre1`) | expected sequence written from the decompiled code |
| A10 | **Vertex format:** the 10 vllg models' vertex buffers include DIFFUSE, and the lit colour uses it (null-backend shader-input check) | `xbin.py` vertex colours |
| A11 | Fuzz: truncated / bit-flipped / length-inflated variants of real BLK, SLB, `.x`, DDS and TGA give no crash and a reported error; ASan + UBSan builds clean natively | fuzz harness |
| A12 | Perf entries recorded: per-area load ms (native), frame p50/p95, draws/frame, pipelines, texture bytes. **No thresholds** until measured. | `perf/history.jsonl` |
| A13 | Browser: the same 3 areas render in Chromium via the BYO page; network shows only build assets; fps overlay with reconciliation | Playwright smoke |

## 5. Design decisions

- **Independent oracles only.** Every loader is checked against a separate implementation: the Python research scripts, a format-spec decoder, or the measured survey counts.
- **Binary `.x` parser is our code.** No D3DX. We reproduce the attribute sort, last-mesh-wins and the vertex layout the game observes.
- **Textures: RGBA8 upload + CPU box-filter mips** (decided from measurements, Fable review):
  - BC-format textures can't be rendered into for mip generation.
  - Per-area texture volume is small: vllg needs 27 textures, 1.32 MB; crss 37, 5.0 MB.
  - A BC upload path is revisited only if a measurement shows load time or memory pressure.
- **No draw merging or pipeline pre-warm in M1.** Measured cost is trivial: vllg 31 subsets / 5.7k vertices, ghut 30 / 5.3k, crss 42 / 6.1k. Merging identical subsets would save 0.6% across all level meshes. Parent §7.1 forbids levers that no measurement points at.
- **On-demand entry reads**, like the original: `lev1.blk` has 225 DDS but vllg needs 23.
- **Texture cache across area switches** sits behind a flag. Default **off**, matching the original's flush; it's a pixel-invisible optimisation to measure in M1b.
- **Security** (BYO input is untrusted):
  - **General:**
    - All size arithmetic in u64, or `count ≤ remaining / stride`, because wasm32 `size_t` is 32-bit.
  - **BLK:**
    - `count × 56 ≤ size`.
    - Names are 40 bytes and may be unterminated.
    - `offset + size` is overflow-checked.
  - **SLB:**
    - `count × 4 ≤ len`.
    - Every reloc slot, and every fixed-up target, lies inside the buffer.
    - OBJ `ptr + n × 60 ≤ len`.
  - **`.x`:**
    - Nesting depth cap 64 (the data's max is 5).
    - Indices `< nVertices`, material index `< nMaterials`, face sizes 3–4.
    - List counts bounded by the remaining bytes.
  - **LZSS:** output capped at the declared `usize`, input bounded by `csize`, and an expansion-ratio cap.
  - **DDS:** reject cube and volume textures; cap dimensions at 4096; cap the mip count at log2 + 1.
  - **TGA:** RLE bounds.
  - **VFS paths:** normalised; `..` folded; never escape a mount (M0 behaviour).

## 6. Risks

| Risk | Mitigation |
|---|---|
| D3DX attribute sort / vertex layout differs from ours | A4 per-file equality against `xbin.py`; A8 visuals |
| World transform or handedness wrong | A8 is the explicit gate; one constant to flip |
| Camera and fog live in `Base.osi` | Mine the literals in M1a-3; documented fallback until then; full fidelity with the M2 VM |
| Fixed-function lighting mismatch | A9 state log + A10 vertex format, then visuals; an unlit debug mode for comparison |
| Clear-Z / W-buffer semantics | Disassembly check in M1a-3; depth buffer with LESSEQUAL in WebGPU |
| Untrusted parsing | §5 rules + A11 fuzz + sanitizer builds |

## 7. Revision 3: measured corrections from the M1a-1 reference build (2026-10-07)

Source: the reference implementation and oracles in the M1a-1 plan bundle; 39/39 oracle checks green.

| # | Earlier text | Measured fact / decision |
|---|---|---|
| R1 | Routing table with 0x8 first | Decompiled order (§2.1 updated) |
| R2 | OBJ fields summing to 56 bytes | An unread u32 sits at +0x1c; the extents are at +0x20/+0x2c (§2.1 updated) |
| R3 | "All 3,247 DDS decode" | 6 entries are zero bytes: lev2 `b.dds` and `rockpath_1.dds`; `characters/Textures.blk` `_legs`, `_legs_bump`, `_legs_bump_2`, `alpha`. No `.x` references them. A5 updated. |
| R4 | (silent) | Two BLK entry names fill all 40 bytes with no terminator. The original `StrCmp` reads past them into `mBlkSize`, so `Find` misses them. **Reproduced on purpose** for fidelity; no drawn model's used subset needs them. |
| R5 | (silent) | D3DX splits vertices where normal indices differ from position indices (2,528 of 2,655 meshes): 1,950,451 → 1,962,024 (+0.59%). The split order is INFERRED; it affects buffer layout, not pixels. |
| R6 | vllg 23 textures / 1.0 MB | 27 / 1.32 MB (25 / 1.23 MB without moveables) |
| R7 | BC "published formula" | No single bit-exact published formula exists; the Direct3D definition is chosen (A5) |
| R8 | TGA alpha | 242 32-bpp TGAs declare 0 alpha bits. The decoder returns the 4th byte plus `alpha_bits`; the renderer treats `alpha_bits == 0` as opaque (X8R8G8B8; D3DX behaviour INFERRED). Confirm in A8. |
| R9 | M0 `fs.h` frozen | Additive API approved: `virtual size(path)` and `virtual read_range(path, offset, len)`. Defaults go through `read`; the memory/disk implementations override them with u64 bounds and access logging. Needed for on-demand entry reads. |
| R10 | DDS mip count 0 | 2 DXT5 files store mip count 1. Mips are generated at upload regardless. |

Measured cost (native Release, M1 Pro, warm file cache, 3 runs):

| What | Value |
|---|---|
| Whole-data `lomn_datacheck` | 2.4–2.9 s; entry I/O dominates (391–478 MiB/s) |
| `.x` parsing | 588–632 MiB/s (24× faster after removing an O(n²) `reserve`, which was also a denial-of-service risk) |
| DDS decode | 636–663 MiB/s |
| TGA decode | ≈ 1.8 GiB/s |
| SLB | < 1 ms in total |
| ASan + UBSan full-data run | clean |
| Deliberate code mutations caught by the tests | 10 of 10 |


### 7.1 Corrections from M1a-1 hardening and the M1a-2 reference (2026-10-07)

| # | Earlier text | Measured fact / decision |
|---|---|---|
| R11 | §5 security bounds (BLK/`.x`/DDS) | Added hostile-input caps, each derived from a measured Beta maximum: BLK name run 512 B (max 2), entry 64 MiB (max 2.51 MB), `.x` objects 131,072 (max 5,489), materials 65,536 (max 3,110), texture name 576 B (max 36), `.x` input 64 MiB. Evidence: `docs/knowledge/audits/2026-10-07-m1a1-parser-security.md`. |
| R12 | §2.3 `ALPHAFUNC GREATEREQUAL` | `GREATER`: DrawX 0x5528c0 sets D3DRS_ALPHAFUNC (0x19) to 5. §2.3 corrected. |
| R13 | §2.3 "ALPHAREF 0 or 1 per the code" | **History-dependent.** `SrPushRenderMode` 0x531100 sets ALPHAREF 1 only when *leaving* a 0x80000 mode, and 0 when entering one; the D3D8 default is 0. M1a-2 uses 1 behind `--alpha-ref`; M1a-3 derives it from the real mode sequence. Pixels differ only where texture alpha is exactly 1/255. |
| R14 | §2.3 alpha pass "ALPHABLEND on" | DrawX turns ALPHABLEND **off** for the alpha pass (alpha-test only) when the flag at `*param_2+0xc` is set (0x5528c0 ≈ lines 295–298). M1a-2 blends every alpha subset. M1a-3 must read which callers set the flag before A8 is judged final. |
| R15 | Parent spec: depth `depth24plus-stencil8` | M1a-2 uses `Depth32Float` without stencil (no stencil use before shadows). `GcShadow::Render` 0x5dbdb0 enables STENCIL, so stencil arrives with shadows (M2) as an additive render-target option. The original's clear Z decompiles as 0.0, which is unusable with LESSEQUAL; M1a-2 clears to 1.0. A disassembly check is pending (M1a-3). |
| R16 | §5 per-area volume | Survey of all 123 areas (M1a-2 reference): at most 55 drawn placements (lev6/ta03), 165 draws (lev6/ta11), 76 textures (lev2/vllg), 57.0 MB of RGBA8+mips texture bytes (lev0/frnt), and 1.27 MB of geometry (lev6/ta14). Caps: 1,024 / 4,096 / 1,024 / 256 MiB / 64 MiB. RGBA8 with mips is ≈ 5.3× the DDS size, which feeds the M1b memory budget. |
