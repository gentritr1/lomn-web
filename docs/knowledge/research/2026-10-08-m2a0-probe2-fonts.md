# M2a-0 probe 2: tile fonts (.col reader, .fnt/.info metrics, string widths)

Scratch only. Nothing committed; PNGs are renders of game data and stay in this directory.
Files here: `colfont.py` (.col/.db reader, LZSS, own hash), `fonts.py` (.fnt layer, own TGA decoder, metrics, renderer),
`survey.py` (all-font checks), `render.py` (PNGs), `render_all.png` + `render_<Font>_<String>.png`.
Run: `export LOMN_DATA=<scratch>/lomn-data; .venv/bin/python <this dir>/survey.py` (all label commands below are variants of this).
Labels: EXECUTED = I ran it and saw the output; INFERRED = read from decomp/bytes only; ESTIMATE = a guess.

## 0. Headline findings

1. EXECUTED: the reader works on all 9 fonts. Every directory entry is named (153/153 for BionicleFont), every glyph TGA decodes, glyph TGAs map 1:1 onto `.fnt` records.
2. The engine does NOT parse the text `.info`. `ScTileFont::ScTileFont` reads a binary `<Name>.fnt` that lives **inside** the `.col` (INFERRED from the decomp and the strings ".fnt"/".col"/"%d_%d" read as data from the exe; EXECUTED that the entry exists and parses). `.info` is the input of the offline tool "TiffSpliter" (named in the .info comments). The `.info` next to the fonts on disk (`Fonts/BionicleFont.info`, 5681 B) is **not** the one the shipped `.fnt` was built from: the copy inside the .col is 5687 B and differs (kerning -4 vs 0, space 12 vs 8, charset ends \xDE vs \xE0).
3. EXECUTED: the width formula reproduces the real ink: for BionicleFont the hard-ink extent of the rendered strings is exactly 84 / 57 / 32 px = GetStrLen.
4. Widths: BionicleFont "New Game" 84, "Options" 57, "Quit" 32; BionicleFontHi 165, 113, 57 (scale 1.0).

## 1. Layouts, with citations

### 1.1 .col header (ScCollectiveFile::ReadHeader 0x4cf390; Open 0x4cf150)
INFERRED (decomp) + EXECUTED (bytes of BionicleFont.col, `xxd`, and `colfont.Col`). All integers **big-endian on disk**; ReadHeader byte-swaps when the stream's swap flag is set.

| off | size | meaning | BionicleFont value | cite |
|---|---|---|---|---|
| 0x00 | u32 | version | 2 | ReadHeader (obj+8); not validated in Open |
| 0x04 | u32 | entry count | 0x99 = 153 | ReadHeader (obj+0xc); passed as `count` to ScHashEntries ctor 0x4cf9e0 |
| 0x08 | u32 | flags; bit0 cleared on load; `flags & 0xe`: 0 = ScNameEntries, 4 = ScHashEntries, else throw kFileIsByNumber | 0x4 (hash mode) | Open 0x4cf150 |
| 0x0c | u32 | total file size (end offset of last entry) | 120396 = file size (EXECUTED, all 9 fonts: `total == st_size`) | ReadHeader (obj+0x14); ScHashEntries::GetInfo uses it for the last entry's size |
| 0x10 | 0x14 B | raw block, read verbatim, never interpreted | all 0xFF (EXECUTED) | ReadHeader (obj+0x18) |
| 0x24 | count x 16 B | directory | see 1.2 | ScHashEntries::ReadEntries 0x4cfa50 |

(The consult says "four u32 + a 0x14-byte block": correct. Offsets above are file offsets; the decomp's obj+8.. are the in-memory copies.)

### 1.2 Directory entry (ScHashEntries::ReadEntries 0x4cfa50, ::GetInfo 0x4cfc20)
16 bytes, four BE u32, sorted ascending by hash (GetInfo binary-searches; EXECUTED `sortedByHash=True` for all 9 fonts):

| field | meaning | cite |
|---|---|---|
| +0 hash | `SrHashString(name)` | GetInfo calls SrHashString 0x4f6af0 |
| +4 flags | `0x11` = compressed; `0x01` = stored. `flags & 0xf0` selects the compression callback (SubFileLoadEntry 0x4cf5f0 -> SrGetCompressionCallback 0x4d3410). Only nibbles 0x10 and 0x00 occur in fonts. | |
| +8 offset | absolute file offset of the data | GetInfo |
| +12 usize | uncompressed size (passed as 3rd arg to the callback's Decompression) | GetInfo copies it to ScEntryInfo+0xc; SubFileLoadEntry |
| (derived) csize | `next.offset - offset`; last entry: `total - offset` | GetInfo |

Data starts at `0x24 + 16*count` (EXECUTED: first entry offset 0x9b4 = 0x24 + 153*16).
Compression 0x10 = LZSS (EXECUTED: `data()` with my LZSS gives valid TGA/text for all 9 fonts, sizes equal `usize`). Callback registration order in SrCompressionInit 0x4d3700 is LZZ/RLE/None, and ScLZZCompression::Decompression 0x4d3460 -> SrDecodeLZSS 0x4d2920; the id values themselves live in .bss (set at runtime) so the nibble->codec mapping 0x10 = LZSS is EXECUTED-by-result, not read from code. LZSS: 4096-byte ring pre-filled 0x20, write cursor 0xfee, 8-bit flag byte LSB first, bit set = literal, else 2 bytes: pos = b0 | (b1 & 0xf0) << 4, len = (b1 & 0xf) + 3.
Entry hash function (SrHashString 0x4f6af0): `h = h*0x83 + byte` over the name, u32, case-sensitive, name relative to the volume, no ".\" prefix. EXECUTED: 153/153 names in the .db hash to a directory entry and to the hash stored in the .db.
Surprise, not hash-related: nothing in the directory is a "glyph index"; names are `<Font><row>_<col>.tga` (`%d_%d`, row 0 only: rows=1).

### 1.3 .col.db (names). ScNameEntries 0x4cf6d0..0x4cf9b0 is a different, older, 0x24-stride format (flags + 0x14-char name + 3 u32) that Open selects only when `flags & 0xe == 0`.
The shipped .db has flags 6, so it cannot be opened by `ScCollectiveFile::Open` (INFERRED: would hit the throw branch). EXECUTED layout of the shipped `.col.db`: 36-byte BE header identical to the .col header shape (version 2, count 153, flags 6, 0, 0x14 x 0xFF), then per entry, **little-endian**: `u32 hash, u32 mtime(0x3b4d17xx), u8 len, name[len]` where name starts with `.\`. Same order as the directory; 153 records, 0 trailing bytes. So the .db is tool-side metadata (original source paths), not read at runtime (ESTIMATE; no runtime reader found in the functions I read). It also lists the build-dir junk packed into the .col: `.bin`, `.col`, `.col.db`, `.col.t`, `.fnt`, `.info`, `ReadMe.txt`, and a whole-sheet `<Font>.tga`.
`.col.t`: 1024 B = 256 x u32 of the same mtime (EXECUTED `xxd`); also packed as an entry. Unused by the reader (ESTIMATE).

### 1.4 .fnt (inside the .col) -- ScTileFont::ScTileFont 0x626b40
Opened via `ScFileManager::Open` on `<Name>/<Name>.fnt` after `SrMountCollectiveVolume` 0x4d0ee0 mounts `<Name>.col` under the volume name `<Name>`. The stream's swap flag is forced **false** (this is the in-memory sub-file), so `.fnt` is **little-endian** (EXECUTED: values read sane as LE). Header 32 bytes, then `rows*columns` records of 18 bytes (EXECUTED: 32 + 145*18 = 2642 = usize, no trailing bytes):

| field | meaning | BionicleFont | BionicleFontHi |
|---|---|---|---|
| i32 rows | tile rows | 1 | 1 |
| i32 columns | tile columns | 145 | 79 |
| i32 numberOfCharacters | glyph records (== rows*columns, EXECUTED all fonts) | 145 | 79 |
| i32 charWidth | tile width = quad width | 16 | 32 |
| i32 charHeight | tile height = quad height | 16 | 32 |
| i32 kerning | added to every advance | 0 | 4 |
| i32 baseline | used by Draw/DrawJust/GetStrHeight | 12 | 12 |
| i32 spaceWidth | advance of ' ' (before kerning) | 8 | 16 |

Record: `u16 ascii, i32 width, i32 height, i32 xOffset, i32 yOffset` (ascii is a byte-swapped u16 only if swap flag; chars >= 0xF0 lead bytes form 2-byte codes, none are used in the shipped fonts: EXECUTED no ascii > 0xFF). ctor also computes maxWidth / minWidth over all records (BionicleFont 15/3; Hi 25/2). Record i (i = row*columns + col) owns texture `<Name>/<Name><row>_<col>` + ".tga" (name pattern `%d_%d` read from the exe; EXECUTED that all 145/79 such entries exist, all 16x16 resp. 32x32). Defaults at construction (EXECUTED: float constants read from the exe's .rdata as data, not run): xScale = yScale = 1.0, mRotate = rotate-X(pi) (a flipped unit quad).

### 1.5 .info (tool input, INFERRED from its own comments + EXECUTED cross-check against the .fnt)
Not read by the engine. Parse: drop lines starting `//`; remaining 7 lines, in this order:
1. tile width -> `.fnt` charWidth; 2. tile height -> charHeight; 3. kerning (may be negative) -> kerning; 4. baseline (comment: pixels from the bottom of the tile) -> baseline; 5. space size -> spaceWidth; 6. background RGBA "0 0 0 0" (colour ignored when measuring a glyph's extent); 7. charset in tile order, one line, `\xHH` escapes for bytes >= 0xA0; 0xF0..0xFE lead bytes take a following byte; 0xFF reserved for the script callback (Draw calls `mFunctionCallback` on 0xFF).
EXECUTED (`parse_info` on the .info **inside** BionicleFont.col): (16,16,0,12,8), 145 charset entries, and the charset equals the `.fnt` ascii column in order (True). The on-disk `Fonts/BionicleFont.info` instead gives (16,16,-4,12,12) and 143 chars (A0..DE). The two .info files therefore describe different builds; use the .fnt (runtime truth).
Per-glyph width/height/xOffset/yOffset are produced by the tool from the tile art. EXECUTED check against the tga alpha: left edge matches xOffset for all 145 glyphs, record width = ink width - 1 (tool measured max-min), vertical offsets are in the TGA's native bottom-up row order (see 3.3).

## 2. Reader results (EXECUTED: `survey.py`, all 9 fonts)

| font | dir entries | .db records | glyphs (.fnt) | glyph TGAs present | glyph size | tile cw x ch | kern | base | space | notes |
|---|---|---|---|---|---|---|---|---|---|---|
| BionicleFont | 153 | 153 | 145 | 145/145 | 16x16 | 16x16 | 0 | 12 | 8 | 146 .tga: +1 `BionicleFont.tga` whole sheet 2320x16 |
| BionicleFontHi | 86 | 86 | 79 | 79/79 | 32x32 | 32x32 | 4 | 12 | 16 | no .info packed; charset 0-9, punctuation, A-Z, a-z (no accents) |
| BionicleFontSh | 87 | 87 | 79 | 79/79 | 32x32 | 32x32 | 4 | 12 | 16 | |
| BionicleFontshad | 87 | 87 | 79 | 79/79 | 32x32 | 32x32 | 4 | 12 | 16 | |
| Tech_Demo_Font01 | 102 | 102 | 46 | 46/46 | 32x32 | 32x32 | 8 | 12 | 16 | also packs .tif/.psd sources |
| Tech_Demo_Font_Highlighted | 104 | 104 | 46 | 46/46 | 32x32 | 32x32 | 8 | 12 | 16 | 2 .col/.db/.t inside |
| tech_font_01 | 167 | 167 | 79 | 79/79 | 32x32 | 32x32 | 8 | 12 | 16 | only 50 distinct ascii of 79 records |
| tech_font_01_hilight | 170 | 170 | 79 | 79/79 | 32x32 | 32x32 | 8 | 12 | 16 | one extension-less entry |

Every font: `.col` total field == file size; all hashes in .db == directory hashes (set equality); directory sorted by hash; compression flags only 0x11 (glyph TGAs) or 0x01 (everything else); `numberOfCharacters == rows*columns`.

The consult's "153 entries" for BionicleFont = 145 glyph TGAs + the whole-sheet `BionicleFont.tga` + 7 non-TGA tool leftovers (.bin, .col, .col.db, .col.t, .fnt, .info, ReadMe.txt). Only 145 are glyphs (EXECUTED).
Glyph vs charset: 145 = 145 for BionicleFont (EXECUTED charset equality). BionicleFont charset: `0-9 ! ? . : ; , " ( ) &`, A-Z, `- / % # < > +`, a-z, `'`, then 0xA0..0xE0 (65 accented/latin-1 codes). Note the order is "A-Z then punctuation then a-z" (tile order).
Glyph TGAs: 32 bpp uncompressed type 2, descriptor byte 0x00 (bottom-left origin, alpha bits declared 0 but **real alpha present**), carry a TGA 2.0 extension/developer area + "TRUEVISION-XFILE" footer (1627-byte file for 16x16x4 = 1042 B of header+pixels). Alpha is a real mask (0 in background); glyph colour is white (BionicleFont) / yellow (BionicleFontHi).

## 3. Metrics from ScTileFont (INFERRED from decomp; EXECUTED against rendered ink)

### 3.1 Width. GetWidth 0x6289a0, GetStrLen 0x628ae0
Per char c (a byte, or a 16-bit code when the first byte is 0xF0..0xFE):
- ' ' (0x20): `(spaceWidth + kerning) * xScale`
- known glyph: `(kerning + width[c]) * xScale`
- unknown char: `(maxWidth + kerning) * xScale`
String: `GetStrLen = round( sum(GetWidth(c)) - kerning*xScale )`; stops at '\n'; initial sum is 0 (float constant read from exe .rdata 0x75822c = 0.0). The glyph lookup is a linear search by `ascii` (first match wins). 0xFF escape bytes are skipped (callback chars, no width). `round` is x87 round-to-nearest-even; irrelevant at integer scales.
Pen advance in Draw 0x627dd0 is the same: `x += (width + kerning)*xScale` (space: `(spaceWidth + kerning)*xScale`), so GetStrLen == pen_end - trailing kerning.

### 3.2 Draw 0x627dd0 (spacing / kerning / baseline)
- Entry: `y += baseline * yScale`; early-out returns 100 unless the renderer priority is 100 when mDoTransparency/mDoAlphaChannel (both true by default); sets texture stage/blend (SRCALPHA/INVSRCALPHA), texture factor from mDiffuseColor (that is how the white glyphs are tinted), cull none, bilinear (mLinear true).
- '\n': `y += round(charHeight*yScale)`, `x` resets to the starting x.
- Per glyph: a quad of `charWidth*xScale` x `charHeight*yScale` (the **tile**, not the ink) with its left edge at `pen - xOffset*xScale`; then `pen = pen_left + (xOffset + width + kerning)*xScale` = `pen + (width+kerning)*xScale`. So ink starts at `pen`. yOffset and per-glyph height are **not** used by Draw; all glyph tiles share one y (vertical alignment is baked into the tile art). Vertical centre of the quad is `y - 0.5*charHeight*yScale` (world sign depends on the HUD transform: ESTIMATE, not resolved).
- Unknown glyph: advance only `(maxWidth + kerning)*xScale`, nothing drawn.
- Z: quad z offset const -7499.0 (exe read as data), i.e. HUD depth is part of the matrix.
- DrawJust 0x627c60: vertical flag bit0: `y += round(charHeight*yScale)`; bit1: `y -= baseline*yScale`; bit2: `y += round(charHeight*yScale)/2 - baseline*yScale`; horizontal bit 0x100: `x -= GetStrLen/2` (integer shift); 0x200: `x -= GetStrLen`.
- DrawBox 0x6288f0: temporarily sets xScale = (xMax-xMin)/GetStrLen(at scale 1), yScale = (yMax-yMin)/GetStrHeight(scale 1), draws at `(xMin, yMin + aboveBaseline*yScale)`.
- GetHeight 0x628a50: `height[c]*yScale` (unknown: `charHeight*yScale`; space: 0.0).
- GetStrHeight 0x628bf0 (second out param `aboveBaseline`): over non-space glyphs: `below = max(baseline - yOffset)` over glyphs with yOffset <= baseline; `above = max(height*yScale - (baseline - yOffset))` over glyphs with yOffset < baseline (else just `height*yScale`); result `round(above + below)`. Note the mixed scaled/unscaled terms. **Bug/UB**: a space (or any unknown char) takes charInfo = -1 and reads `mChar[-1]` (the allocator prefix): the result for strings containing a space is undefined in the original. I skip spaces; "New Game" height below is for the non-space glyphs only.

### 3.3 Orientation note (EXECUTED)
Decoding the TGAs by their descriptor (bottom-left origin => flip) gives upright text. The `.fnt` yOffset/baseline are consistent with the **file's row order (bottom-up)**, not the flipped one (EXECUTED: with the raw bottom-up rows 130 of 145 glyphs have first ink row == yOffset; with the upright (flipped) decode the offset is spread over -2..+3 and matches far fewer). This matches the exe flipping the quad (rotate-X by pi) instead of the texture. For the web port: if you upload TGA rows as stored (bottom-up) and flip the quad, or flip on decode and flip the quad back, the glyph tile is the only vertical authority; do not use yOffset to place glyphs.

### 3.4 The three menu strings (scale 1.0). EXECUTED by `fonts.py`, hand-checked
| string | BionicleFont width | BionicleFontHi width | BionicleFont height (no space) | Hi height (no space) |
|---|---|---|---|---|
| "New Game" | 84 | 165 | 10 | 15 |
| "Options" | 57 | 113 | 11 | 20 |
| "Quit" | 32 | 57 | 11 | 15 |

Hand check, BionicleFont: Q13 + u9 + i3 + t7 = 32; N11+e9+w13+space8+G12+a9+m13+e9 = 84; kerning 0. Hi (kerning 4, space 16): N17 e19 w28 sp20 G21 a19 m26 e19 = 169, minus final kerning 4 = 165.
Cross-check against pixels: the rendered hard-ink (alpha >= 128) horizontal extent of BionicleFont is 0..84, 0..57, 0..32 (exactly GetStrLen); Hi is 0..165, 1..113, 1..58 (the "ink = width-1" tool quirk; 1 px). Lowercase strings exist in both fonts (no missing glyphs); upper-case "NEW GAME/OPTIONS/QUIT" would be 88/67/38 (BionicleFont) and 167/123/66 (Hi).
The actual width on screen = value * xScale; the script-side scale (sctilefont natives) is **UNVERIFIED** here (only constants read from the exe: default 1.0). Which case the menu uses is also unverified (script strings come from `lang_eng.blk`).
Renders: `render_BionicleFont_*.png`, `render_BionicleFontHi_*.png` (4x nearest, on a dark background), contact sheet `render_all.png`. Viewed `render_all.png`: legible; BionicleFont white blocky face, Hi a wider yellow face. (The yellow/white colouring comes from the glyph pixels and the diffuse tint; the tint the game applies is script-set and not examined.)

## 4. Surprises
- `.fnt`, `.info`, `.bin`, `.tga` sheet, ReadMe, source `.tif/.psd` are all packed **inside** the .col; the shipped directory is a verbatim copy of the tool's output folder. Only the glyph TGAs and the `.fnt` matter at runtime.
- Endianness splits: `.col` header, directory and .db header are **big-endian** (an Xbox/console-era tool, source path `...\Xbox\SlTileFont.XBOX.cpp`); the `.fnt` and `.db` records are **little-endian**; the engine's swap flag is on for the .col stream and forced off for sub-files.
- Hashed names: SrHashString (x0x83 + c, no case folding); names are stored without `.\`; .db gives the plaintext.
- Compression: LZSS (nibble 0x10) per entry; `.fnt/.info/.bin` are stored (0x01) -- 7 non-glyph entries are stored, 146 TGAs compressed.
- TGA: descriptor 0x00 but 32 bpp with real alpha: Pillow ignores the alpha channel for that combination (EXECUTED: all alpha 255 with Pillow, correct with my decoder). The repo's C++ `decode_tga` (engine/src/sage/image.cpp) accepts types 2/10 at 24/32 bpp, so it should be fine, but check its handling of the descriptor-0 + extension-area case on these files before trusting it (UNVERIFIED, not run).
- On-disk `.info` is stale vs the packed one and the `.fnt` (kerning -4 vs 0, space 12 vs 8).
- Multiple pages: none; rows is 1 in every font (one row of tiles, one texture per glyph), so there is no atlas page logic in the engine; an atlas for the web port is a free choice.
- tech_font_01*: 79 records with only 50 distinct ascii (duplicates; lookup is first-match).
- GetStrHeight reads `mChar[-1]` for spaces (see 3.2).

## 5. Open verification gaps
- UNVERIFIED: the actual in-game pixel width on the menu (script scale, case, any DrawJust/DrawBox use). No game run (forbidden).
- UNVERIFIED: vertical placement sign and exact vertical origin of the glyph quad on screen (HUD transform not read).
- UNVERIFIED: which of the lookups in `ScCollectiveVolume::Open` normalise the requested name (I only verified the hash of volume-relative, case-sensitive names matches the directory).
- UNVERIFIED: the repo's C++ TGA decoder on these files.
- ESTIMATE: `.db`, `.col.t`, `.bin` are not read at runtime.
