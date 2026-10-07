# M1: how LOMN loads an area (SAGE engine, LEGOBionicle.exe)

Sources (2026-10-07): decompiled functions as produced by `make decomp` in `gen/decomp/funcs/<addr>.c` (not committed), struct layouts from `gen/decomp/types.txt`, string literals read from the exe's data section at the virtual addresses Ghidra named (`tools/research/vastr.py`; the exe was only read, never run), and BLK/SLB bytes parsed with `tools/research/{blk,slb,texres}.py`.
Tags: **VERIFIED (how)** means I read it in decompiled code, the exe data or the game files. **INFERRED** means it is my interpretation and has not been checked at runtime.

---

## 1. Area load sequence

### 1a. Level entry: `GcAreaDirector::SetCurrentLevel` (0x00489a60)
This runs once per level, before any area loads. It is called from script: it has no direct callers in calls.jsonl.
1. `sLevelID = StringToIdentifier(levelName)`. Then `ScBlockFile::Cleanup(&sLevelBlock)`, `Cleanup(&sAreaBlock)` and `LoadLevelBlockHeader(false)`, which makes the **level** blockfile the *current* one. **VERIFIED** (decompiled lines 84–90).
2. Loads `Root/Data/Levels/%.4s/material.slb` into `GcMaterialTable` (0x70b680). **VERIFIED** (string read from exe).
3. Loads `Root/Data/Levels/%.4s/textures.slb` into `GcSpecTextureLoader` (`_sSpecialTextures`) (0x70b6a3). **VERIFIED**.
4. Loads the per-language strings `Root/Data/Levels/%.4s/%.4s_eng.slb` (and the other 7 languages) from `Root/Data/Languages/Lang_xxx.blk`. **VERIFIED**.

### 1b. Area entry
There are three ways in:
- Script calls `SetCurrentArea(name)` (0x0048a540). The OSI thunk at 0x00486ef0 does the call. It sets `sCurAreaID` and then calls `LoadArea(name, true)`. **VERIFIED**.
- Area switch in play. `GcCollisionPhysicsGroup::SwitchAreaWithTransition` (0x00476f90) only stores `sNextArea` and calls `SpinMask` (starts a fade). `GcCollisionPhysicsGroup::Process` (0x00475fc0) watches `sFadeState` 1→2→3 and at the right fade state calls `GcAreaDirector::SwitchCurrentArea(sNextArea)`. **VERIFIED** (the call is present). The exact timing within the fade is **INFERRED**.
- `GcAreaDirector::SwitchAreas` → `GcCollisionPhysicsGroup::SwitchAreas` (0x00476cf0) → `SwitchCurrentArea`, followed by Toa placement at start positions. **VERIFIED**.

`SwitchCurrentArea` (0x0048a5a0) does the following. **VERIFIED**:
1. Fires the script `OnEvent`.
2. Calls `RemoveArea(sCurAreaID)` (0x00489560). That runs `GcAreaLoader::CleanUpArea`, sets `sCurAreaID='????'`, calls `ScDrawableModel::FlushCache()`, `ScDrawableTexture::FlushCache()` and `ScModelHierarchy::FlushCache()`, then `ScBlockFile::Cleanup(&sAreaBlock)` and `Cleanup(&sLevelBlock)`.
3. Sets `sCurAreaID = new` and calls `LoadArea(name, true)`, then the camera AI and `GcSaver::SetCurrentArea`.

`GcAreaDirector::LoadArea` (0x004892f0) creates a `GcAreaLoader`, calls `GcAreaLoader::LoadArea(areaName, SrAsString(sLevelID), show)`, then `Add(loader, areaId)`. **VERIFIED**.

### 1c. `GcAreaLoader::LoadArea` (0x004fedd0, 945 lines, messy)
In order. **VERIFIED** unless marked otherwise:
1. Loading HUD: font `Root/Data/Art/Fonts/BionicleFont` and `Root/Data/Art/hud/loading.tga`. While these load, the current blockfile is set to NULL. That makes `SrBlockFileLoad` look in Art.blk first and then loose files.
2. Builds 12 path strings (level = `param_2`, area = `param_1`):
   - `Root/Data/Levels/%.4s/%.4s`. This is the **area prefix**, used later for models.
   - `Root/Data/Levels/%.4s/%.4s/%.4s_{obj,pos,light,trigger,char,instance,sounds,emit,vine,hive,spot}.slb`.
3. `LoadLevelBlockHeader(true)` sets the *second* blockfile to `Root/Data/Levels/<lev>/<lev>.blk`. Then `LoadAreaBlockHeader()` sets the *current* blockfile to `Root/Data/Levels/<lev>/<sCurAreaID>.blk`. Both use the format string `Root/Data/Levels/%.4s/%.4s.blk` (0x70b7ec). Each header is loaded only if `!IsHeaderLoaded`.
4. Loaders run in this order. Each one is wrapped in an SAGE try/catch loop, so a missing SLB throws `kFileOpen` and is skipped, with a loading-bar step between loaders:
   1. `GcObjectLoader::LoadObjects` (obj)
   2. `GcPositionLoader::LoadPositions` (pos)
   3. `GcLightLoader::LoadLights`, or `GrSetDefaultLighting` on exception (light)
   4. `GcSpotLoader::LoadLights` (spot)
   5. `GcTriggerLoader::LoadTriggers`
   6. `GcCharacterLoader::LoadCharacters`
   7. `GcInstanceLoader::LoadInstances`
   8. `Gc3DSoundLoader::CreateSounds`
   9. `GcConversationEngine::LoadConversation`
   10. `GcEmitterLoader::Load`
   11. `GcVineLoader::LoadVines`
   12. `GcHiveLoader::LoadHives`
   
   Ghidra dropped the path argument of every `X::GetSLKFile(const char*)` call. Matching each loader to its path string (e.g. obj → `_obj.slb`) is **INFERRED** from the names and the build order.
5. Hides the HUD and ends the black-out.

### 1d. Blockfiles and paths
- Volume `Root` is mounted at the exe directory: `GcGame::SetUpMountVolume` (0x00439710) → `SrMountSystemVolume("Root", exeDir)`. `ScSystemVolume::ConvertToFullNativePath` (0x004f77e0) gives `exeDir + "/" + rest`. Real files are `data/levels/...` in lowercase. The engine relies on Windows' case-insensitive filesystem, and characters live in `Textures.blk` but are requested as `textures.blk`. **VERIFIED** (code + `ls`). A browser loader **must match file paths case-insensitively**.
- The blockfiles used during an area load are listed below. **VERIFIED** (strings):

| Blockfile | Slot | Loaded by |
|---|---|---|
| `Root/Data/Levels/<lev>/<area>.blk` | current | LoadAreaBlockHeader 0x0048bc10 |
| `Root/Data/Levels/<lev>/<lev>.blk` | second | LoadLevelBlockHeader 0x0048bd10 |
| `Root/Data/Art/Art.blk` | global fallback | GcGame::LoadArtBlockHeader 0x0043b030 |
| `Root/Data/Characters/%.4s.blk` | current, per character | GcCharacter::LoadCharBlockHeader 0x00419330 |
| `Root/Data/Characters/textures.blk` | current/second | LoadCharTexBlockHeader 0x0048be30 |
| `Root/Data/Sounds/Sounds.blk` | current | LoadSoundBlockHeader 0x0043b000 |

## 2. ScBlockFile / ScBlock

Layouts. **VERIFIED** (types.txt):
- `ScBlockFile` is 292 bytes: `mVer, mFlags, mNumBlocks, mNumAllocated, ScBlock* mBlocks, uchar* mBlockFile, FixedString<255> mBlockFileName`.
- `ScBlock` is 56 bytes: `char mName[40]; u32 mBlkSize; u32 mFullSize; u32 mFlags; u32 f` (offset).

Functions. **VERIFIED** (code):
- **`LoadHeader`** (0x00610000): `fopen(path,"rb")`, checks the magic `0x464b4c42` ("BLKF"), `fseek(8)`, reads `mNumBlocks`, allocates `n*0x38`, `fseek(12)` and reads the whole table, then `fclose`. The **version field is never checked**. Only the header table stays in memory.
- **`Find`** (0x0060fec0):
  - It first **removes everything up to the last `/` or `\`**, so the directory part of a request is ignored. `Root/Data/Levels/lev1/vllg/Xs/main.x` becomes `main.x`.
  - It then does a **binary search** over the entry table with `ScBlock::StrCmp` (0x0060fd70). That comparison is case-insensitive: it folds A–Z with `|0x60`. `operator<` is 0x0060fd10 and `operator==` is 0x0060fd30.
  - There is no hash. The search therefore *requires* the table to be sorted case-insensitively. **VERIFIED by parsing**: all 188 BLKs in the install are sorted that way.
- **`LoadDiskBlock`** (0x006100f0): `Find`, then `fopen` again, `fseek(offset)` and read one entry. If `mFlags & 1`, it reads `mBlkSize` bytes and runs `SrDecodeLZSS` into `mFullSize`. Otherwise it reads `mFullSize` raw bytes.
- **`LoadDiskBlockPart`** (0x00610370): reads a byte range from a stored entry. It returns garbage, not NULL, if the entry is compressed.
- `ScBlock::Clean` frees `f` only when `(mFlags & 2) == 0`. So bit 1 means "f is a file offset" and an in-memory variant also exists. That meaning is **INFERRED**.
- The engine **reads individual entries on demand** and never a whole blockfile. There is **no data cache** at this layer.

Caching happens higher up. **VERIFIED**:
- `ScSLKManager` keeps a map from the full path string to `ScSLKFile`.
- `ScDrawableModel::Create` and `ScDrawableTexture::Create` each keep a `map<string, AutoRef>` keyed by the full path.
- All of these are flushed in `RemoveArea`.

Lookup chain, `SrBlockFileLoad` (0x0054db60). **VERIFIED**:
- If a current blockfile is set: current → second → Art.blk. **Loose files on disk are never tried.**
- If the current blockfile is NULL: Art.blk → `ScFileManager::GetFileData` (disk).
- `SrBlockFileExists` (0x0054ddc0) uses a different order: Art.blk **first**, then current, then second. Disk is used only when the current blockfile is NULL.

## 3. Entry types in area and level blockfiles

### 3a. Real entries
**VERIFIED** (parsed):
- **`levels/lev1/vllg.blk`** (29 entries):
  - 18 `.x`: dor2, dr00, drll, fhv0, ghv0–2, ham1–3, main, main1, pic1, pic2, plnt, rock, tre1, vllw
  - `main.BCL`, `vllw.bcl`
  - `vllg_{CHAR,CONV,HIVE,LIGHT,OBJ,POS,SOUNDS,SPOT,TRIGGER}.slb`
- **`levels/lev2/ghut.blk`**: 17 `.x`, `main.OCL`, `ghut_{CHAR,LIGHT,OBJ,POS,SOUNDS,TRIGGER}.slb`.
- **`levels/lev1/lev1.blk`** (256 entries): 225 `.dds`, `on_lev_10xx_sky_sun.TGA`, `material.slb`, `textures.slb`, plus cinematic tables `cin1..6_{cam,char,obj,snd,text}.slb`.
- Across all level blks:
  - Area blks contain `.x`, `.slb`, `.bcl`/`.ocl` and sometimes `.txt`.
  - Level blks contain `.dds` and `.slb`.
  - Character blks add `.bhd` (hierarchy), `.bkd` (keyframes) and `.cdx`.
  - **Every entry in every BLK has flags = 2 and BLK version = 1.**

### 3b. Which loader consumes each entry type

| Entry | Consumer | Evidence |
|---|---|---|
| `<name>.x` | `ScDrawableModel::Load` (0x00553bd0) → `LoadX` (0x00553800) | **VERIFIED**. LoadX appends `".x"`, `SrBlockFileLoad`s it and parses it with `DirectXFileCreate` + `RegisterTemplates(D3DRM_XTEMPLATES)` + `CreateEnumObject(memory)`. `LoadFrames` (0x00554680) runs `D3DXLoadSkinMeshFromXof` and then ID3DXSkinMesh vtbl+0x68 to produce `mpMesh` and `GetAttributeTable`. |
| `<name>.bsd` | `ScDrawableModel::LoadBSD` (strip) | **VERIFIED** that the file is checked for. None exist in the install. |
| `<name>.bhd` / `.bkd` | `ScModelHierarchy::Create` (0x0055ace0) / animations | **VERIFIED** for `.bhd`. With no `.bhd` the hierarchy is NULL. Character data only. |
| `.dds` / `.tga` | `ScDrawableTexture::Load` (0x005561c0) → `D3DXCreateTextureFromFileInMemory` | **VERIFIED**. See Q4. |
| `*.slb` | `ScSLKManager::GetSLKFile` (0x004032e0) | **VERIFIED**. See below. |
| `bcls/<name>.ocl`, then `.bcl` | `GcOctreeDirector::ImportOCL`, falling back to `ImportBCL` | **VERIFIED**. Called from `GcArea::GcArea` with path `<prefix>/bcls/<name>.ocl\|bcl`. The OBJ id `main` goes to `sMain`; other ids go to `sSecondary`. This is collision. |
| `.cdx` | not traced | — |

**SLB format** ("SLK binary"). **VERIFIED** (code + bytes):
- An SLB is a raw memory image of a C struct.
- The last aligned u32 is `0x00C0FFEE`. Before it are `u32 count` and then `count` u32 offsets of pointer fields.
- Each pointer field holds a data-relative offset that gets fixed up to an absolute address.
- All records seen are **little-endian**.
- 4CC IDs are stored as LE u32, so the file bytes read `wllv` for `vllw`.
- Pointers can be **unaligned**: OBJ records start at 0x16.
- Strings are pointer + Pascal length byte. In `textures.slb` the code reads `name+1`.

**Object placement** (`<area>_OBJ.slb`), `GcObjectLoader::LoadObjects` (0x0053fc60). **VERIFIED**:
- Header: `u32 slot` (overwritten with the area id), `u32 n`, `u32 n`, `ptr` to records.
- Records are 60 bytes:
  - `+0` id (4CC, which is also the model name)
  - `+4` position vec3
  - `+0x10` orientation vec3
  - `+0x20` and `+0x2c` two vec3 (collision extents)
  - `+0x38` flags

Routing by flags. **VERIFIED** (code):

| Flags | Destination |
|---|---|
| `0x10` and not `0x20` and not `0x100` | `GcAreaDirector::AddMainArea` (0x00489650), a static GcArea |
| `0x10` and `0x100` | `GcTransDirector::AddMainArea` (water/transparent) |
| `0x10` and `0x20` | `GcAreaDirector::AddMoveableObj` (0x00489700), e.g. doors |
| `0x20` alone | `GcNoLightDirector::AddMoveableObj` |
| `0x8` | `GcBackDirector::AddArea` (sky/background) |
| anything else | ignored |

Survey of the 117 area OBJ files. **VERIFIED** (parsed):
- 183 main-area records, 136 trans, 35 back, 943 moveable.
- Every record has a matching `<id>.x` in its area blk.

Other area tables:
- **POS** (`GcPositionLoader::LoadPositions` 0x00502a50): 20-byte records `{id, vec3, flags}`. Flag `0x1000` is a start position; `0x40|0x20` is a pickup sprite. **VERIFIED**.
- **LIGHT** (0x00470010): 44-byte records `{id, pos, range?, ?, rgba, flags}`. `0x40` is a point light. `0x80` is a directional light, and the loader also adds an opposite fill light "Shad" with a hard-coded colour. **VERIFIED**; field names partly **INFERRED**.
- **INSTANCE** (0x0058e590): 36-byte records `{instId, modelId, pos…}`. **No `_instance.slb` exists anywhere in the install. VERIFIED**.

## 4. How a model's textures are resolved

**VERIFIED** (LoadX 0x00553800, ScDrawableTexture::Load 0x005561c0, `gTextureTypes` @0x7381d0):
1. The model path is `<areaPrefix>/Xs/<id>` (format `/Xs/%s` in `GcArea::GcArea` 0x00504220). For example `Root/Data/Levels/lev1/vllg/Xs/main`.
2. For each material index that **appears in the mesh attribute table**, LoadX takes the D3DX `pTextureFilename`. It removes the last 3 path components and appends `/textures/<name>`, giving `Root/Data/Levels/lev1/textures/on_lev_20xx_ocean`. Materials that no subset uses are never loaded.
3. Texture names in the `.x` files have **no extension**. `ScDrawableTexture::Load` tries the name as given if it has an extension listed in the table. Otherwise it tries `<name>.dds` and then `<name>.tga`, testing each with `SrBlockFileExists`. The table holds only `{3,".dds"}, {3,".tga"}`; type 3 means D3DX load. The AVI and raw paths (types 1/2) can't be reached.
4. Because `Find` drops directories, the request becomes `<name>.dds`. It is looked up in the area blk, then the **level blk**, then Art.blk.

Textures are **shared across all areas of a level** because they live in `<lev>.blk`. However, the caches are flushed on every area switch, so they are re-read each time.

Measured resolution. **VERIFIED** (`texres.py`):
- In lev1/vllg, lev2/ghut, lev1/l1a7, lev3/boss, lev4/crss, lev5/le08, lev6/ta11, lev7/kpka and lev0/frnt, **every resolved texture came from the level blk as `.dds`**.
- vllg has 46 names that resolve nowhere. All of them come from `main1.x` and `rock.x`, which the area OBJ table doesn't place.

Material properties come from `ScDrawableMaterial::LoadIndexed(path, D3DMATERIAL8*, n)` (0x00550060):
- It uses the `.x` diffuse, specular, power and emissive values.
- It then applies **name-substring overrides**: `_glass` changes diffuse/trans, names containing `eye`/`on_`/`ga_`/… change emissive, and there are Toa/mask texture special cases. **VERIFIED**.
- The level's `textures.slb` adds per-texture special behaviour via `GcSpecTextureLoader::LoadTextures` (0x0046f270): records `{name, f, f, f, animFrames}` with path `<lev>/textures/<name>`, for example ocean and river scrolling, plus `ScAnimTexture` when frames are present. **VERIFIED** (bytes). That the three floats are alpha and the U/V scroll speeds is **INFERRED**.

## 5. Minimal faithful "load area X, render static geometry" path

Ranked by importance:

1. **BLK reader**: `ScBlockFile::LoadHeader` 0x00610000 (clean), `Find` 0x0060fec0 with `ScBlock::StrCmp` 0x0060fd70 (clean), `LoadDiskBlock` 0x006100f0 (clean).
2. **Lookup chain**: `SrBlockFileLoad` 0x0054db60 / `SrBlockFileExists` 0x0054ddc0 (clean), `LoadAreaBlockHeader` 0x0048bc10, `LoadLevelBlockHeader` 0x0048bd10 (clean).
3. **SLB fix-up**: `ScSLKManager::GetSLKFile` 0x004032e0 (clean; reloc logic is fully visible).
4. **Placement**: `GcObjectLoader::LoadObjects` 0x0053fc60. Moderate: Ghidra mistyped the records as `_GcAreaInfo`, but the offsets are readable.
5. **Area object**:
   - `GcAreaDirector::AddMainArea` 0x00489650, `GcTransDirector::AddMainArea` 0x00485a50, `GcBackDirector::AddArea` 0x0047a620 (clean).
   - `GcArea::GcArea` 0x00504220 (messy, 521 lines, but the paths and matrix are clear). The world matrix is `Identity × D3DUtil_SetRotateXMatrix(-1.5707982)` (constant @0x72a444 read from the exe).
   - `LoadObjects` then adds the record position into matrix `_41.._43`. The code writes `pSVar11[0xb].mRefCount`, which is GcArea+0x5c, the matrix pointer, plus 0x30.
   - So world = RotX(−90°) followed by translate(pos). The model is Z-up and is converted to Y-up as (x, z, −y), using D3D row-vector convention. **VERIFIED by reading; not yet checked at runtime**.
6. **Draw**: `GcArea::Render` 0x00505840 (clean) calls the model's Draw (vtbl+0x14) with that matrix and count 1. `ScDrawableModel::DrawX` 0x005528c0 (messy, 427 lines): with no hierarchy it does `SetTransform(WORLD, matrix)` and then loops over the attribute-table subsets.
7. **Model**: `ScDrawableModel::Create` 0x00554340 / `Load` 0x00553bd0 / `LoadX` 0x00553800 / `LoadFrames` 0x00554680. These are clean, though the COM vcalls are unnamed. The browser needs a **binary `.x` parser**: every file starts `xof 0303bin 0032`.
8. **Materials and textures**: `ScDrawableMaterial::LoadIndexed` 0x00550060 (messy heuristics), `ScDrawableTexture::Create` 0x00556ca0 / `Load` 0x005561c0 (clean), and a DDS decoder for DXT3/DXT5.
9. **Optional fidelity**:
   - `SetCurrentLevel` 0x00489a60 (messy; textures.slb and material.slb)
   - `GcSpecTextureLoader::LoadTextures` 0x0046f270 (clean)
   - `GcLightLoader::LoadLights` 0x00470010 (moderate) / `GrSetDefaultLighting`
   - Moveable props via `AddMoveableObj` 0x00489700 → `GcMoveableObj` ctor 0x005432a0 (path `%s/Xs/%s`, also `%s/bcls/%s.bcl`, `%s/%.4s.slb`)
   - `SrDecodeLZSS` 0x004d2920: clean, standard Okumura LZSS with N=4096, F=18, threshold 2, initial fill 0x20, r=0xFEE. Needed only for flag bit 0, which no entry in this install sets.

## 6. Surprises / risks

- **Directory paths are ignored**: only the filename matters, searched area → level → Art. Two areas in one level can't conflict, but a texture with the same name in an area blk overrides the level copy. **VERIFIED**.
- **`SrBlockFileExists` and `SrBlockFileLoad` search in different orders** (Art first vs. Art last). This can diverge for names that exist in both. In the 9 sampled areas no resolved texture also existed in Art under the extension that was chosen. Toa skin names such as `gali` and `TAHU` exist in both, but the `.dds` probe wins in the level blk. **VERIFIED** (sample only).
- **Loose files are dead while a blockfile is current**. For example `levels/lev3/material.slb` on disk is shadowed by `lev3.blk/material.slb`. **VERIFIED** (code path).
- **Not every `.x` in an area blk is placed**: vllg `main1.x` and `tre1.x` are not referenced by any area SLB or by Base.osi (`rock.x` and `drll.x` are used only by `lev1.blk/cin2_obj.slb`). A viewer must drive placement from `_OBJ.slb` and must not render everything in the blk. `tre1` and `plnt` do appear as hard-coded IDs in `GcArea::Render`, which turns on fog while they draw. **VERIFIED**.
- **Some areas have no `_OBJ.slb`**: lev4 `gly1`, `hutt`, `mosh`, and `haka`, which has 0 entries. These are probably cut content, and a faithful load places nothing. **VERIFIED** (data); "cut content" is **INFERRED**.
- **Alpha rule**: `mAlphaOn` is true for DXT3 and A8R8G8B8 and **false for DXT5**. The level DDS files are 1963 DXT5 and 318 DXT3, all stored with mip count 0. **VERIFIED** (code + parsed headers). The visual consequence is **INFERRED**.
- `LoadFrames` overwrites `mpSkinMesh` for every Mesh it finds and ignores `FrameTransformMatrix`, so one mesh per `.x` is assumed. That every file contains exactly one mesh is **INFERRED** from token counts.
- Hard-coded 4CC special cases in `GcArea::Render`: `plnt`/`tre1` turn fog on; `bech` + `main` triggers `SunFlare` with lens-flare TGAs from Art. Separately, the record whose id is `main` sends its collision to the main octree. **VERIFIED**.
- The BLK version is never checked, and all files are version 1. Integers are little-endian. `LoadHeader` swaps the fread size and count arguments, which is harmless. **VERIFIED**.
- D3DX calls in play: `D3DXLoadSkinMeshFromXof`, skin-mesh conversion/`GetAttributeTable`, `D3DXCreateTextureFromFileInMemory` (default mip generation; that the default is used is **INFERRED**), `DirectXFileCreate`, and the D3DUtil matrix helpers.

## Open verification gaps

1. World transform (RotX(−90°) + translation in `_41.._43`) needs a runtime or render check against a known screenshot.
2. The `GetSLKFile` path → loader mapping in `GcAreaLoader::LoadArea` is inferred, because Ghidra dropped the string arguments. The fix is to disassemble around 0x004fef00 or later.
3. The single-mesh-per-`.x` assumption needs a real binary `.x` parse.
4. LIGHT and OBJ field semantics beyond position and flags.
5. The meaning of the `textures.slb` floats.
