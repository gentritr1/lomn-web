# Asset survey (player install, Beta 1.5.0) — 2026-10-07

Measured by walking every `.blk` under `$LOMN_DATA/install/data` with a ~40-line Python reader. The format follows `sage-file-formats/BLK/BLK.bt` (MPL-2.0, JrMasterModelBuilder).

## BLK container
- Layout: header `"BLKF"`, `u32 version`, `u32 count`, then `count` × 56-byte entries: `char name[40]`, `u32 size_compressed`, `u32 size_uncompressed`, `u32 flags` (2 = stored, 3 = compressed), `u32 offset`. Little-endian.
- **All 189 blockfiles are version 1; all 11,455 entries are stored (flag 2), none compressed.** Loading Beta data is pure byte slicing.
- When a build does compress (REBUILT's makefile option), the codec is Okumura LZSS (`sage-blkfile/blkfile.py`), **not** zlib. The M1 reader implements LZSS for robustness but the target data never exercises it.

## Entry types (11,455 total)

| Type | Count | Notes |
|---|---|---|
| `.dds` | 3,247 | 267.2 MB; DXT5 2,871 · DXT3 362 · DXT1 2 · uncompressed 6; sizes mostly 256² (1,513) and 128² (818), max 1024×512 |
| `.x` | 2,653 | **all binary DirectX `.x` with 32-bit floats** (`xof 0302bin 0032`); 194.6 MB; median 25 KB, max 2.1 MB |
| `.tga` | 2,566 | |
| `.slb` | 1,177 | SAGE data tables (AI info, anim events, cinematics …) |
| `.bkd` | 747 | animation (BKD) |
| `.wav` | 345 | |
| `.bhd` | 331 | |
| `.bcl` | 153 | |
| `.jpg` | 120 | (IJL in the original) |
| `.cdx` | 34 | collision — format "unknown" in the community docs |
| other | `.txt` 17, `.bin` 14, `.col` 12, `.db` 12, `.t` 12, `.bmp` 8, `.ocl` 5 | |

## Consequences for M1
- **Texture policy:** BC2/BC3 are uploaded directly where `texture-compression-bc` exists. Elsewhere, CPU-decode to RGBA8. The textures are small, so the cost is bounded: 267 MB of DXT expands to roughly 4× as RGBA, but only the textures one area references, not all of them.
- **Mesh loader:** only the **binary** `.x` path is needed for Beta data. The text `.x` path stays optional (and gets tests if implemented).
- **Prefetch:** areas are slices of `levels/<lev>/<area>.blk` plus the level's common blk (largest area 35.0 MB, from census) — there is no decompression step to budget for.
