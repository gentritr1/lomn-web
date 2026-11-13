#include "lomn/sage/colfont.h"

#include <cmath>

#include "lomn/sage/blockfile.h"
#include "bytes.h"

// M2a-3 T7a. See colfont.h for the layouts and the asm addresses; tests/test_sage_colfont.cpp pins each by its bytes.
namespace lomn::sage {
using detail::fail;
using detail::le16;
using detail::le32;

static uint32_t be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

// SrHashString 0x4f6af0: h = h * 0x83 + byte over the bytes, u32.
uint32_t col_hash(std::string_view name) {
  uint32_t h = 0;
  for (unsigned char c : name) h = h * 0x83u + c;
  return h;
}

// ---------------------------------------------------------------------------------------------------------- ColFile
bool ColFile::open_bytes(std::vector<uint8_t> bytes, std::string* err) {
  *this = ColFile();
  if (bytes.size() > kMaxColFileSize) return fail(err, "col: file larger than the " + std::to_string(kMaxColFileSize) + "-byte cap");
  if (bytes.size() < kColHeaderSize) return fail(err, "col: shorter than the 0x24-byte header");
  const uint8_t* d = bytes.data();
  const uint32_t version = be32(d), count = be32(d + 4), flags = be32(d + 8), total = be32(d + 12);
  if ((flags & 0xe) != 4) return fail(err, "col: flags & 0xe is not 4 (only the hash-entries layout is read)");
  if (total != bytes.size()) return fail(err, "col: total size field " + std::to_string(total) + " differs from the file size " + std::to_string(bytes.size()));
  if (count > kMaxColEntries) return fail(err, "col: " + std::to_string(count) + " entries is above the cap of " + std::to_string(kMaxColEntries));
  const uint64_t data_start = kColHeaderSize + static_cast<uint64_t>(kColEntrySize) * count;
  if (data_start > total) return fail(err, "col: directory runs past the end of the file");
  std::vector<ColEntry> es(count);
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* e = d + kColHeaderSize + static_cast<size_t>(kColEntrySize) * i;
    ColEntry& c = es[i];
    c.hash = be32(e);
    c.flags = be32(e + 4);
    c.offset = be32(e + 8);
    c.size = be32(e + 12);
    if (i > 0 && c.hash < es[i - 1].hash) return fail(err, "col: directory hashes do not ascend at entry " + std::to_string(i));
    if (c.offset < data_start || c.offset > total) return fail(err, "col: entry " + std::to_string(i) + " offset is outside the data area");
    if (i > 0 && c.offset < es[i - 1].offset) return fail(err, "col: entry offsets decrease at entry " + std::to_string(i));
    if ((c.flags & 0xf0) != 0x00 && (c.flags & 0xf0) != 0x10) return fail(err, "col: entry " + std::to_string(i) + " has an unknown codec nibble");
    if (c.size > kMaxColEntrySize) return fail(err, "col: entry " + std::to_string(i) + " decodes to more than the cap");
  }
  for (uint32_t i = 0; i < count; ++i) {  // GetInfo: the stored size is the next entry's offset (the last: the total) - offset
    ColEntry& c = es[i];
    c.stored = (i + 1 < count ? es[i + 1].offset : total) - c.offset;
    if ((c.flags & 0xf0) == 0 && c.size > c.stored) return fail(err, "col: stored entry " + std::to_string(i) + " is larger than its span");
  }
  version_ = version;
  flags_ = flags;
  bytes_ = std::move(bytes);
  entries_ = std::move(es);
  open_ = true;
  return true;
}

// ScHashEntries::GetInfo 0x4cfc20: binary search over the hash-sorted directory; the lowest of equal hashes.
int ColFile::find(std::string_view name) const {
  const uint32_t h = col_hash(name);
  size_t lo = 0, hi = entries_.size();
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (entries_[mid].hash < h) lo = mid + 1;
    else hi = mid;
  }
  return lo < entries_.size() && entries_[lo].hash == h ? static_cast<int>(lo) : -1;
}

bool ColFile::read(size_t index, std::vector<uint8_t>& out, std::string* err) const {
  out.clear();
  if (!open_) return fail(err, "col: not open");
  if (index >= entries_.size()) return fail(err, "col: entry index " + std::to_string(index) + " out of range");
  const ColEntry& e = entries_[index];
  const uint8_t* src = bytes_.data() + e.offset;
  if ((e.flags & 0xf0) == 0) {
    out.assign(src, src + e.size);
    return true;
  }
  return decode_lzss(src, e.stored, e.size, out, err);  // clears `out` on any failure
}

bool ColFile::read_named(std::string_view name, std::vector<uint8_t>& out, std::string* err) const {
  out.clear();
  const int i = find(name);
  if (i < 0) return fail(err, "col: no entry named " + std::string(name));
  return read(static_cast<size_t>(i), out, err);
}

// -------------------------------------------------------------------------------------------------------- TileFont
static int64_t abs64(int32_t v) { return v < 0 ? -static_cast<int64_t>(v) : v; }

bool TileFont::parse(const uint8_t* data, size_t size, std::string* err) {
  *this = TileFont();
  if (size < kFntHeaderSize) return fail(err, "fnt: shorter than the 32-byte header");
  TileFont f;
  int32_t* fields[8] = {&f.rows, &f.columns, &f.count, &f.char_width, &f.char_height, &f.kerning, &f.baseline, &f.space_width};
  for (int i = 0; i < 8; ++i) *fields[i] = static_cast<int32_t>(le32(data + 4 * i));
  if (f.count < 1 || f.count > static_cast<int32_t>(kMaxFontGlyphs)) return fail(err, "fnt: glyph count " + std::to_string(f.count) + " outside 1.." + std::to_string(kMaxFontGlyphs));
  if (size - kFntHeaderSize < static_cast<uint64_t>(f.count) * kFntRecordSize) return fail(err, "fnt: records run past the end of the data");
  if (f.rows < 1 || f.columns < 1 || static_cast<int64_t>(f.rows) * f.columns != f.count) return fail(err, "fnt: rows * columns differs from the glyph count");
  if (f.char_width < 1 || f.char_width > kMaxFontTile || f.char_height < 1 || f.char_height > kMaxFontTile) return fail(err, "fnt: tile size outside 1.." + std::to_string(kMaxFontTile));
  if (abs64(f.kerning) > kMaxFntValue || abs64(f.baseline) > kMaxFntValue || abs64(f.space_width) > kMaxFntValue) return fail(err, "fnt: a header field is beyond the cap");
  f.glyphs.resize(static_cast<size_t>(f.count));
  uint32_t mx = 0, mn = 0xffffffffu;  // ctor 0x626d71 / 0x626d7e; unsigned compares at 0x6277e0 / 0x627810
  for (size_t i = 0; i < f.glyphs.size(); ++i) {
    const uint8_t* r = data + kFntHeaderSize + i * kFntRecordSize;
    FontGlyph& g = f.glyphs[i];
    g.ascii = le16(r);
    g.width = static_cast<int32_t>(le32(r + 2));
    g.height = static_cast<int32_t>(le32(r + 6));
    g.x_offset = static_cast<int32_t>(le32(r + 10));
    g.y_offset = static_cast<int32_t>(le32(r + 14));
    if (abs64(g.width) > kMaxFntValue || abs64(g.height) > kMaxFntValue || abs64(g.x_offset) > kMaxFntValue || abs64(g.y_offset) > kMaxFntValue)
      return fail(err, "fnt: glyph " + std::to_string(i) + " has a field beyond the cap");
    const uint32_t w = static_cast<uint32_t>(g.width);
    if (w > mx) mx = w;
    if (w < mn) mn = w;
  }
  f.max_width = static_cast<int32_t>(mx);
  f.min_width = static_cast<int32_t>(mn);
  *this = std::move(f);
  return true;
}

// GetWidth 0x6289a0 / GetHeight 0x628a50 search: 16-bit compare, first match.
int TileFont::find(uint32_t code) const {
  const uint16_t c = static_cast<uint16_t>(code);
  for (size_t i = 0; i < glyphs.size(); ++i)
    if (glyphs[i].ascii == c) return static_cast<int>(i);
  return -1;
}

// The x87 does these in extended precision (the products and the f32 stores per add are the observable part); double is
// the closest the host has. Integers convert as the asm converts them: the space's `fild dword` is signed, every other
// width is a zero-extended `fild qword`.
static double width_d(const TileFont& f, uint32_t code, double xs) {
  if ((code & 0xffff) == 0x20) return static_cast<double>(static_cast<int32_t>(static_cast<uint32_t>(f.space_width) + static_cast<uint32_t>(f.kerning))) * xs;
  const int i = f.find(code);
  const uint32_t w = i >= 0 ? static_cast<uint32_t>(f.glyphs[static_cast<size_t>(i)].width) : static_cast<uint32_t>(f.max_width);
  return static_cast<double>(static_cast<uint64_t>(static_cast<uint32_t>(f.kerning) + w)) * xs;
}

float TileFont::get_width(uint32_t code, float xscale) const { return static_cast<float>(width_d(*this, code, xscale)); }

// fistp qword under RC = chop, then `mov eax, [qword]`: the low dword of the truncated value (an overflowing fistp stores
// the "integer indefinite" 0x8000000000000000, low dword 0).
static int chop_low32(double v) {
  const double t = std::trunc(v);
  if (!(std::fabs(t) < 9.2e18)) return 0;
  return static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(static_cast<int64_t>(t))));
}

namespace {
enum class Tok { Char, Skip, Newline };
uint32_t at(std::string_view s, size_t i) { return i < s.size() ? static_cast<uint8_t>(s[i]) : 0u; }  // the original reads the terminator
// One step of the string walk shared by GetStrLen 0x628b03.. and GetStrHeight 0x628c30..: the code (a lead byte
// 0xF0..0xFE takes the next byte: lead * 256 + next), then the 0xFF and '\n' tests on the byte at the CURRENT position.
// `i` ends one past what the step consumed (the loop's own increment included).
Tok next_tok(std::string_view s, size_t& i, uint32_t& code) {
  const uint32_t b = at(s, i);
  code = b;
  if (b >= 0xf0 && b <= 0xfe) {
    ++i;
    code = (b << 8) + at(s, i);
  }
  if (at(s, i) == 0xff) {  // the callback escape: skips itself and the next character, no width
    ++i;
    const uint32_t b2 = at(s, i);
    if (b2 >= 0xf0 && b2 <= 0xfe) ++i;
    ++i;
    return Tok::Skip;
  }
  if (at(s, i) == 0x0a) return Tok::Newline;
  ++i;
  return Tok::Char;
}
}  // namespace

int TileFont::get_str_len(std::string_view s, float xscale) const {
  float sum = 0.0f;  // [0x75822c] = 0.0, stored as f32 after every add
  size_t i = 0;
  uint32_t code = 0;
  while (i < s.size()) {
    const Tok t = next_tok(s, i, code);
    if (t == Tok::Newline) break;
    if (t == Tok::Char) sum = static_cast<float>(static_cast<double>(sum) + width_d(*this, code, xscale));
  }
  return chop_low32(static_cast<double>(sum) - static_cast<double>(kerning) * static_cast<double>(xscale));
}

bool TileFont::get_str_height(std::string_view s, float yscale, int* height, float* above_baseline) const {
  float above = 0.0f, below = 0.0f;  // [0x75823c] and [0x758238]
  size_t i = 0;
  uint32_t code = 0;
  while (i < s.size()) {
    const Tok t = next_tok(s, i, code);
    if (t == Tok::Newline) break;
    if (t == Tok::Skip) continue;
    const int idx = find(code);
    if (idx < 0) continue;  // hud.md §4.9 (INFERRED): the original reads record -1, heap before the array; both engines skip the character
    const FontGlyph& g = glyphs[static_cast<size_t>(idx)];
    // GetHeight 0x628a50: ' ' = 0.0 [0x758228], else height * yScale (height zero-extended), stored as f32
    const float a = (code & 0xffff) == 0x20 ? 0.0f : static_cast<float>(static_cast<double>(static_cast<uint32_t>(g.height)) * yscale);
    const uint32_t base = static_cast<uint32_t>(baseline), yo = static_cast<uint32_t>(g.y_offset);
    const uint32_t d = base - yo;
    if (static_cast<double>(d) > static_cast<double>(below) && base >= yo) below = static_cast<float>(d);
    const float cand = base > yo ? static_cast<float>(static_cast<double>(a) - static_cast<double>(d)) : a;
    if (cand > above) above = cand;
  }
  if (height) *height = chop_low32(static_cast<double>(above) + static_cast<double>(below));
  if (above_baseline) *above_baseline = above;
  return true;
}

// ------------------------------------------------------------------------------------------------------ the loader
bool load_tile_font(const fs::FileSystem& vfs, std::string_view path, LoadedFont& out, std::string* err) {
  out = LoadedFont();
  LoadedFont lf;
  const size_t cut = path.find_last_of("/\\");
  lf.name = std::string(cut == std::string_view::npos ? path : path.substr(cut + 1));
  const std::string col_path = std::string(path) + ".col";
  uint64_t sz = 0;
  if (vfs.size(col_path, sz) != fs::Error::None) return fail(err, "tile font " + std::string(path) + ": cannot open " + col_path + " (file not found)");
  if (sz > kMaxColFileSize) return fail(err, "tile font " + std::string(path) + ": " + col_path + " is larger than the cap");
  std::vector<uint8_t> bytes;
  if (vfs.read(col_path, bytes) != fs::Error::None) return fail(err, "tile font " + std::string(path) + ": cannot read " + col_path);
  std::string e2;
  if (!lf.col.open_bytes(std::move(bytes), &e2)) return fail(err, "tile font " + std::string(path) + ": " + e2);
  std::vector<uint8_t> fnt;
  if (!lf.col.read_named(lf.name + ".fnt", fnt, &e2)) return fail(err, "tile font " + std::string(path) + ": " + e2);
  if (!lf.font.parse(fnt.data(), fnt.size(), &e2)) return fail(err, "tile font " + std::string(path) + ": " + e2);
  const TileFont& f = lf.font;
  lf.glyph_images.resize(f.glyphs.size());
  lf.glyph_tga.resize(f.glyphs.size());
  for (size_t i = 0; i < f.glyphs.size(); ++i) {
    const std::string tga = lf.name + std::to_string(i / static_cast<size_t>(f.columns)) + "_" + std::to_string(i % static_cast<size_t>(f.columns)) + ".tga";
    std::vector<uint8_t> t;
    if (!lf.col.read_named(tga, t, &e2)) return fail(err, "tile font " + std::string(path) + ": glyph texture " + tga + ": " + e2);
    if (!decode_tga(t.data(), t.size(), lf.glyph_images[i], &lf.glyph_tga[i], &e2)) return fail(err, "tile font " + std::string(path) + ": " + tga + ": " + e2);
    if (lf.glyph_images[i].width != static_cast<uint32_t>(f.char_width) || lf.glyph_images[i].height != static_cast<uint32_t>(f.char_height))
      return fail(err, "tile font " + std::string(path) + ": " + tga + " is not charWidth x charHeight");
  }
  out = std::move(lf);
  return true;
}

}  // namespace lomn::sage
