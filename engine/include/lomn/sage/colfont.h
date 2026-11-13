// Tile fonts: the collective file (.col) and the binary font table (.fnt) inside it. M2a-3 Task 7a, written from
// docs/knowledge/research/2026-10-08-m2a0-probe2-fonts.md (the contract) and docs/osi/hud.md §3.6 / §4.4 / §7.2.
// Labels: EXECUTED = measured on the install; INFERRED = read from the decompile only (never run).
//
// .col (ScCollectiveFile::ReadHeader 0x4cf390, Open 0x4cf150, ScHashEntries::ReadEntries 0x4cfa50, GetInfo 0x4cfc20),
// all integers BIG-endian: u32 version, u32 count, u32 flags (bit 0 cleared on load; `flags & 0xe` == 4 = hash mode, the
// only mode shipped), u32 total size, 0x14 raw bytes never read, then `count` 16-byte entries {hash, flags, offset,
// usize} sorted ascending by hash. hash = SrHashString 0x4f6af0: h = h * 0x83 + byte (u32, case-sensitive, name
// relative to the volume). Stored size = next entry's offset - offset (the last: total - offset). `flags & 0xf0`
// selects the codec: 0x00 stored, 0x10 LZSS (the blockfile's decode_lzss). Data starts at 0x24 + 16 * count.
//
// .fnt (ScTileFont::ScTileFont 0x626b40; the entry "<Name>.fnt" of "<Name>.col", LITTLE-endian): 32-byte header of
// eight i32 {rows, columns, numberOfCharacters, charWidth, charHeight, kerning, baseline, spaceWidth}, then
// numberOfCharacters records of 18 bytes {u16 ascii, i32 width, i32 height, i32 xOffset, i32 yOffset}. Record i
// (i = row * columns + col) owns the glyph texture "<Name><row>_<col>.tga" of the same collective.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "lomn/fs.h"
#include "lomn/sage/image.h"

namespace lomn::sage {

constexpr size_t kColHeaderSize = 0x24;
constexpr size_t kColEntrySize = 16;
// Caps (spec §5). Measured over the 8 shipped fonts (2026-11-13): <= 170 entries, <= 0x6c000 bytes per file, <= 145
// glyphs, tiles <= 32 x 32, |every .fnt field| <= 32. Each cap leaves >= 6x headroom.
constexpr uint32_t kMaxColEntries = 4096;
constexpr uint64_t kMaxColFileSize = 64u << 20;   // same as kMaxEntrySize
constexpr uint64_t kMaxColEntrySize = 64u << 20;  // a decoded entry
constexpr uint32_t kMaxFontGlyphs = 1024;
constexpr int32_t kMaxFontTile = 128;       // charWidth / charHeight
constexpr int32_t kMaxFntValue = 1 << 16;   // |every other i32|, so no sum over a string can leave a float's exact range

// SrHashString 0x4f6af0.
uint32_t col_hash(std::string_view name);

struct ColEntry {
  uint32_t hash = 0;
  uint32_t flags = 0;
  uint32_t offset = 0;
  uint32_t size = 0;    // usize: bytes after decoding
  uint32_t stored = 0;  // next.offset - offset
};

class ColFile {
 public:
  // Validates the header and the directory; any failure leaves the object empty and sets *err.
  // Rejected: bytes < 0x24, bytes > kMaxColFileSize, count > kMaxColEntries, flags & 0xe != 4, total != bytes,
  // 0x24 + 16 * count > total, an entry offset below the data start or past total, offsets that decrease along the
  // directory, hashes that do not ascend, a codec nibble other than 0x00/0x10, size > kMaxColEntrySize, a stored
  // entry with size > stored.
  bool open_bytes(std::vector<uint8_t> bytes, std::string* err);
  bool is_open() const { return open_; }
  uint32_t version() const { return version_; }
  uint32_t flags() const { return flags_; }
  const std::vector<ColEntry>& entries() const { return entries_; }
  // ScHashEntries::GetInfo: binary search by col_hash(name). -1 if absent.
  int find(std::string_view name) const;
  // The decoded bytes of entry `index`; on failure `out` is left empty (M1a-4: never a partial decode).
  bool read(size_t index, std::vector<uint8_t>& out, std::string* err) const;
  bool read_named(std::string_view name, std::vector<uint8_t>& out, std::string* err) const;

 private:
  bool open_ = false;
  uint32_t version_ = 0, flags_ = 0;
  std::vector<uint8_t> bytes_;
  std::vector<ColEntry> entries_;
};

constexpr size_t kFntHeaderSize = 32;
constexpr size_t kFntRecordSize = 18;

struct FontGlyph {
  uint16_t ascii = 0;
  int32_t width = 0, height = 0, x_offset = 0, y_offset = 0;
};

struct TileFont {
  int32_t rows = 0, columns = 0, count = 0, char_width = 0, char_height = 0, kerning = 0, baseline = 0, space_width = 0;
  int32_t max_width = 0, min_width = 0;  // over all records (ctor)
  std::vector<FontGlyph> glyphs;

  // Parses the `.fnt` bytes. Rejected: shorter than 32 + count * 18, count < 1 or > kMaxFontGlyphs, rows or columns < 1,
  // rows * columns != count, a tile size outside 1..kMaxFontTile, kerning / baseline / space / glyph fields with
  // |value| > kMaxFntValue. Trailing bytes are allowed. max_width / min_width are the ctor's loop (0x626d71, 0x626d7e:
  // initial 0 and 0xffffffff, UNSIGNED compares at 0x6277e0 / 0x627810). On failure *this is left default.
  bool parse(const uint8_t* data, size_t size, std::string* err);

  // The linear lookup of the original: the first record with this ascii, -1 if none. (The shipped tech_font_01* have
  // records with duplicate ascii: the first wins.)
  int find(uint32_t code) const;

  // GetWidth 0x6289a0 at x scale `xscale` (f32 arithmetic, EXECUTED asm read): ' ' (0x20) -> float(i32(space + kerning)) * s; a
  // known glyph -> float(u64(u32(kerning + width))) * s; anything else -> float(u64(u32(maxWidth + kerning))) * s. The
  // unsigned conversions are the original's (`fild qword` over a zero-extended dword): a negative sum is a huge width.
  float get_width(uint32_t code, float xscale = 1.0f) const;
  // GetStrLen 0x628ae0. `s` is length-delimited (an ScString), a NUL is an ordinary character. Per character: a lead
  // byte 0xF0..0xFE takes the next byte, code = lead * 256 + next (0 when past the end); then, looking at the byte at the
  // CURRENT position (the second byte of a 2-byte code, if there was one): 0xFF skips itself and the following character
  // (a second 0xF0..0xFE lead skips its trailing byte too) with no width; '\n' ends the string; anything else adds
  // get_width(code), each add rounded to f32. Result: TRUNCATE toward zero (x87 RC = 0xc00; probe 2 said
  // round-to-nearest-even, the asm says chop) of sum - kerning * xscale, as the low 32 bits of the fistp qword.
  int get_str_len(std::string_view s, float xscale = 1.0f) const;
  // GetStrHeight 0x628bf0 (and GetHeight 0x628a50). Same string walk as get_str_len ('\n' ends it early). Per character:
  // h = height * yscale of its record, d = baseline - yOffset (u32); `below` = max d over records with baseline >= yOffset
  // (unsigned); the candidate `above` is h - d when baseline > yOffset, else h; *above_baseline = max candidate. Result:
  // trunc(*above_baseline + below). A character with no record (a space included) is SKIPPED, contributing nothing: the original reads record -1 there (heap before the
  // array), hud.md §4.9 rules the skip, INFERRED. Always returns true.
  bool get_str_height(std::string_view s, float yscale, int* height, float* above_baseline = nullptr) const;
};

// A font loaded from the VFS: the .col, the parsed .fnt and every glyph texture (decoded through decode_tga).
struct LoadedFont {
  std::string name;  // the stem of the path, as the .col entries spell it ("BionicleFont")
  ColFile col;
  TileFont font;
  std::vector<Image> glyph_images;  // record order; each charWidth x charHeight
  std::vector<TgaInfo> glyph_tga;   // per image
};

// "<name>/<name>.fnt" of "<path>.col" (what sctilefont.create mounts, hud.md §7.2), then "<name><row>_<col>.tga" per
// record. `path` is a game path without the extension ("Root/Data/Art/Fonts/BionicleFont"). A missing .col, .fnt or
// glyph texture, a TGA that does not decode, or one that is not charWidth x charHeight is an error (hud.md §7.2: the
// missing file is a VM error). On failure `out` is left default.
bool load_tile_font(const fs::FileSystem& vfs, std::string_view path, LoadedFont& out, std::string* err);

}  // namespace lomn::sage
