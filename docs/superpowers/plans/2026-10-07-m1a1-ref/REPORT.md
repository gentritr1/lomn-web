# M1a-1 data path — reference implementation report (condensed)

Built 2026-10-07 by a Claude Opus subagent in an isolated scratch copy of the M0 engine. Independently re-verified by the controller: native ctest 3/3, and `oracle/check_datacheck.py` 39/39 (21.1 s).

## Contents (`MANIFEST.txt`)
- **`lomn::sage` (each header names the original function + address):**
  - `blockfile`: LoadHeader/Find/StrCmp/LoadDiskBlock + SrDecodeLZSS with bounds and a ratio cap
  - `blockchain`: SrBlockFileLoad (area → level → Art) and SrBlockFileExists (Art first), over `lomn::fs`
  - `slb`: GetSLKFile fix-ups, exactly masked + bounded; OBJ/POS/LIGHT views; decompiled routing order
  - `xfile`: binary `.x` token parser, last mesh wins, D3DX-style vertex split, attribute table
  - `image`: DDS DXT1/3/5 + A8R8G8B8 → RGBA8 (Direct3D BC definition); TGA type 2/10
  - `bytes.h`: a bounded little-endian cursor
- **`tools/lomn_datacheck.cpp`** (native) emits one JSON document of whole-data summaries.
- **Tests:** 36 doctest cases on synthetic and malformed inputs (natively and as wasm32 under Node).
- **Oracles:** `check_datacheck.py` (A1–A6), `bcdec.py` (independent BC decoder using `Fraction` arithmetic), `texres_ext.py` (Load + Exists orders, 40-byte names), `mutate.py` (10 deliberate code mutations; all were caught by the tests), surveys.
- **M0 changes (additive, spec R9):**
  - `fs.h` gains `virtual size()` / `virtual read_range()`, implemented in memory_fs/disk_fs.
  - CMake adds the `lomn_sage` library, `lomn_datacheck` and the tests.
  - `test_fs.cpp` gains 6 range-read assertions.
  - Full diff: `engine_vs_m0.diff`.

## Results
| Check | Result |
|---|---|
| A1 | 189 BLK; entry names and order equal `blk.py` |
| A2 | 9 areas; Load and Exists resolution equal `texres_ext.py` |
| A3 | 117 OBJ tables (11 cin excluded) → 183/136/35/943; flag histogram 0x10×183, 0x110×136, 0x8×35, 0x31×81, 0x32×810, 0x131×18, 0x132×16, 0x432×18 |
| A4 | 2,653 `.x` equal `xbin.py` per file; `mklf.x` has 3 meshes |
| A5 | 3,241 non-empty DDS decode; 6 zero-byte entries rejected; BC bit-exact on 181,568 blocks |
| A6 | vllg draws dor2, dr00, ham1–3, main, pic1, pic2, plnt, vllw — never main1/tre1/rock/drll |
| Extra | TGA 2,566 decode; SLB 1,177 pass; POS 116 tables / 1,750 records (598 with flag 0x1000); LIGHT 99 / 1,608 |
| Sanitizers | ASan + UBSan clean on the unit tests and a full-data run (JSON identical to the release run) |

## Measurements (native Release, M1 Pro, warm cache, 3 runs)
| What | Result |
|---|---|
| Whole data | 2.42 / 2.50 / 2.88 s, peak RSS ≈ 51 MB |
| Entry reads (dominant cost) | 391–478 MiB/s |
| `.x` | 588–632 MiB/s (it was 27.7 MiB/s before an O(n²) `reserve` fix, which was also a denial-of-service risk) |
| DDS | 636–663 MiB/s |
| TGA | ≈ 1.8 GiB/s |
| SLB | < 1 ms |
| Vertex split | 1,950,451 → 1,962,024 |

## Decisions recorded in M1 spec §7 (R1–R10)
- Decompiled routing order.
- OBJ unread u32 at +0x1c.
- 6 empty DDS entries.
- 40-byte unterminated names reproduced.
- Vertex split order (INFERRED).
- vllg 27 textures / 1.32 MB.
- Direct3D BC definition.
- TGA `alpha_bits == 0` treated as opaque.
- FS API additions.
- 2 DXT5 files with mip count 1.

## Unverified
- Nothing has been checked against the running game (40-byte names, split order and BC rounding are inferences).
- wasm32 is covered by the synthetic unit tests only.
- LZSS was tested only synthetically.
- Material name overrides belong to M1a-3.
