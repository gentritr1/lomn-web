// M2a-3 T7a: the tile fonts (.col / .fnt), their glyph TGAs, GetWidth / GetStrLen / GetStrHeight, and the HUD sprite
// textures, written from docs/knowledge/research/2026-10-08-m2a0-probe2-fonts.md and docs/osi/hud.md §3.6 / §4.4 / §7.2
// BEFORE the reader (RED, then GREEN). Where the asm disagrees with probe 2 the asm wins and the case says so:
// GetStrLen TRUNCATES (RC = 0xc00; probe 2: round-to-nearest-even), a 2-byte code is lead * 256 + next, GetStrHeight does
// not skip spaces (it reads record -1 there), the string is length-delimited.
// Asm pins read the canonical exe as DATA through $LOMN_DATA (reference/LEGOBionicle.exe); real-data cases read the
// install through the disk VFS. Without LOMN_DATA they report SKIPPED and pass (as test_osi_services.cpp). Every number
// pinned from the install was measured on 2026-11-13/14 with a throw-away Python script of this task's own (struct over
// the .col bytes, its own LZSS), not with this reader.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#ifndef __EMSCRIPTEN__
#include <filesystem>
#endif

#include "doctest.h"
#include "lomn/fs.h"
#include "lomn/sage/blockchain.h"
#include "lomn/sage/colfont.h"
#include "lomn/sage/image.h"
#include "sage_test_util.h"

using namespace lomn;
using namespace lomn::sage;
using namespace sage_test;

namespace {

// ---------------------------------------------------------------------------------------------- synthetic builders
void be32(Bytes& b, uint32_t v) {
  for (int i = 3; i >= 0; --i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void set_be32(Bytes& b, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i) b[at + static_cast<size_t>(i)] = static_cast<uint8_t>(v >> (8 * (3 - i)));
}
uint32_t test_hash(const std::string& s) {  // SrHashString, written again here so col_hash is not its own oracle
  uint32_t h = 0;
  for (unsigned char c : s) h = h * 0x83u + c;
  return h;
}
// LZSS stream of literals only: a flag byte 0xff per 8 bytes.
Bytes lzss_literals(const Bytes& in) {
  Bytes o;
  for (size_t i = 0; i < in.size(); i += 8) {
    const size_t k = std::min<size_t>(8, in.size() - i);
    o.push_back(static_cast<uint8_t>((1u << k) - 1));
    o.insert(o.end(), in.begin() + static_cast<std::ptrdiff_t>(i), in.begin() + static_cast<std::ptrdiff_t>(i + k));
  }
  return o;
}

struct ColSpec {
  std::string name;
  Bytes payload;   // the stored bytes (for flags 0x11 an LZSS stream)
  uint32_t flags;  // 0x01 stored, 0x11 LZSS
  uint32_t usize;
  uint32_t pad = 0;  // gap bytes after the payload (stored size = payload + pad)
};
// A collective file in hash mode: entries sorted by hash, data back to back in directory order.
Bytes make_col(std::vector<ColSpec> es) {
  std::sort(es.begin(), es.end(), [](const ColSpec& a, const ColSpec& b) { return test_hash(a.name) < test_hash(b.name); });
  Bytes b;
  be32(b, 2);
  be32(b, static_cast<uint32_t>(es.size()));
  be32(b, 4);
  be32(b, 0);  // total, patched
  for (int i = 0; i < 20; ++i) b.push_back(0xff);
  uint32_t off = static_cast<uint32_t>(kColHeaderSize + 16 * es.size());
  for (const auto& e : es) {
    be32(b, test_hash(e.name));
    be32(b, e.flags);
    be32(b, off);
    be32(b, e.usize);
    off += static_cast<uint32_t>(e.payload.size()) + e.pad;
  }
  for (const auto& e : es) {
    b.insert(b.end(), e.payload.begin(), e.payload.end());
    b.insert(b.end(), e.pad, 0);
  }
  set_be32(b, 12, static_cast<uint32_t>(b.size()));
  return b;
}

struct Rec {
  uint16_t ascii;
  int32_t w, h, xo, yo;
};
void put_i32(Bytes& b, int32_t v) { put32(b, static_cast<uint32_t>(v)); }
Bytes make_fnt(int32_t rows, int32_t cols, int32_t cw, int32_t ch, int32_t kern, int32_t base, int32_t space, const std::vector<Rec>& rs,
               int32_t count = -1) {
  Bytes b;
  put_i32(b, rows);
  put_i32(b, cols);
  put_i32(b, count < 0 ? static_cast<int32_t>(rs.size()) : count);
  put_i32(b, cw);
  put_i32(b, ch);
  put_i32(b, kern);
  put_i32(b, base);
  put_i32(b, space);
  for (const Rec& r : rs) {
    put16(b, r.ascii);
    put_i32(b, r.w);
    put_i32(b, r.h);
    put_i32(b, r.xo);
    put_i32(b, r.yo);
  }
  return b;
}

// The synthetic font of the width / height cases: 1 x 6 tiles of 8 x 10, kerning 1, baseline 7, space 4.
//   rec 0 'A'    w6 h8 yo0   d = 7, above candidate h - d
//   rec 1 'g'    w5 h9 yo3   d = 4
//   rec 2 'q'    w3 h2 yo7   yo == baseline: d = 0, candidate h (baseline > yo is false)
//   rec 3 'j'    w4 h5 yo9   yo > baseline: below untouched, candidate h
//   rec 4 'A'    w9 (a duplicate: the FIRST record of an ascii wins)
//   rec 5 0xF141 w7 h3 yo6   a 2-byte code, lead 0xF1 * 256 + 'A'
const std::vector<Rec> kSynth = {{'A', 6, 8, 0, 0}, {'g', 5, 9, 0, 3}, {'q', 3, 2, 0, 7}, {'j', 4, 5, 0, 9}, {'A', 9, 8, 0, 0}, {0xF141, 7, 3, 0, 6}};
TileFont synth_font() {
  TileFont f;
  const Bytes b = make_fnt(1, 6, 8, 10, 1, 7, 4, kSynth);
  std::string err;
  REQUIRE_MESSAGE(f.parse(b.data(), b.size(), &err), err);
  return f;
}
int slen(const TileFont& f, const std::string& s, float xs = 1.0f) { return f.get_str_len(s, xs); }

}  // namespace

// =============================================================================================== TGA, checked FIRST
// Probe 2 flagged it UNVERIFIED: the glyph TGAs are 32 bpp type 2 with descriptor 0x00 (bottom-left origin, alpha bits
// declared 0) and REAL alpha, plus a TGA 2.0 extension area and a "TRUEVISION-XFILE" footer after the pixels. Pillow
// drops the alpha of such a file; decode_tga must keep it (and must not choke on the trailing bytes).
TEST_CASE("colfont tga: descriptor 0, 32 bpp, real alpha, extension area and footer after the pixels") {
  Bytes t = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  put16(t, 2);
  put16(t, 2);
  put8(t, 32);
  put8(t, 0);  // descriptor 0: alpha bits 0, bottom-left origin
  // file rows are bottom first: row 0 = bottom. pixels B G R A
  for (uint8_t v : {1, 2, 3, 0x10, 4, 5, 6, 0x00, 7, 8, 9, 0xff, 10, 11, 12, 0x80}) t.push_back(v);
  for (int i = 0; i < 40; ++i) t.push_back(0xee);  // a TGA 2.0 extension area, never read
  putstr(t, "TRUEVISION-XFILE.");
  t.push_back(0);
  Image img;
  TgaInfo ti;
  std::string err;
  REQUIRE_MESSAGE(decode_tga(t.data(), t.size(), img, &ti, &err), err);
  CHECK(ti.bits == 32);
  CHECK(ti.alpha_bits == 0);  // declared 0 ...
  CHECK_FALSE(ti.top_down);
  REQUIRE(img.width == 2);
  REQUIRE(img.height == 2);
  // ... and the alpha byte is still the file's 4th byte; the image is flipped to top-down: the file's second row first.
  CHECK(img.rgba == Bytes{9, 8, 7, 0xff, 12, 11, 10, 0x80, 3, 2, 1, 0x10, 6, 5, 4, 0x00});
}

// =================================================================================================== SrHashString
TEST_CASE("colfont hash: h = h * 0x83 + byte over the bytes, u32, case-sensitive, '' = 0") {
  CHECK(col_hash("") == 0u);
  CHECK(col_hash("a") == 0x61u);
  CHECK(col_hash("ab") == 0x3205u);  // 0x61 * 0x83 + 0x62
  CHECK(col_hash("Zz") == 0x2e88u);  // 0x5a * 0x83 + 0x7a
  CHECK(col_hash("BionicleFont.fnt") == 0x376d143eu);  // the value in BionicleFont.col's directory (entry 35)
  CHECK(col_hash("BionicleFont0_0.tga") == 0x4d904441u);
  CHECK(col_hash("bionicleFont.fnt") != col_hash("BionicleFont.fnt"));
  // the u32 wraps: 12 characters of 'z' overflow 32 bits
  CHECK(col_hash("zzzzzzzzzzzz") == test_hash("zzzzzzzzzzzz"));
}

// ================================================================================================== ColFile (synthetic)
TEST_CASE("colfont col: stored and LZSS entries, sizes from the next offset, lookup by hash") {
  const Bytes a = {'h', 'e', 'l', 'l', 'o'};
  Bytes big;
  for (int i = 0; i < 100; ++i) big.push_back(static_cast<uint8_t>(i * 7));
  // a match against the ring's pre-filled 0x20 run (positions 0..0xfed; 0xfee.. start as zeros and the write cursor starts
  // there): flag bit 0 clear, pair (lo = pos & 0xff, hi = (pos >> 4) & 0xf0 | (len - 3))
  const Bytes spaces_lz = {0x00, 0x00, 0x00};  // pos 0, len 3 -> 3 spaces
  const Bytes mixed = {0x01, 'X', 0x00, 0x01};  // flag byte 0x01: item 0 a literal 'X', item 1 a pair (pos 0, len 4) = 4 spaces
  const Bytes mixed_out = {'X', ' ', ' ', ' ', ' '};
  const Bytes col = make_col({{"a.bin", a, 1, 5, 3}, {"big.bin", lzss_literals(big), 0x11, 100}, {"sp.bin", spaces_lz, 0x11, 3},
                              {"mix.bin", mixed, 0x11, 5}, {"empty.bin", {}, 1, 0}});
  ColFile c;
  std::string err;
  REQUIRE_MESSAGE(c.open_bytes(col, &err), err);
  CHECK(c.is_open());
  CHECK(c.version() == 2);
  CHECK(c.flags() == 4);
  REQUIRE(c.entries().size() == 5);
  for (size_t i = 1; i < c.entries().size(); ++i) CHECK(c.entries()[i - 1].hash <= c.entries()[i].hash);
  Bytes out;
  REQUIRE_MESSAGE(c.read_named("a.bin", out, &err), err);
  CHECK(out == a);  // a stored entry reads usize bytes, not the 3 pad bytes of its stored span
  const int ia = c.find("a.bin");
  REQUIRE(ia >= 0);
  CHECK(c.entries()[static_cast<size_t>(ia)].stored == 8);  // next.offset - offset, or total - offset for the last
  CHECK(c.entries()[static_cast<size_t>(ia)].size == 5);
  REQUIRE_MESSAGE(c.read_named("big.bin", out, &err), err);
  CHECK(out == big);
  REQUIRE_MESSAGE(c.read_named("sp.bin", out, &err), err);
  CHECK(out == Bytes{' ', ' ', ' '});
  REQUIRE_MESSAGE(c.read_named("mix.bin", out, &err), err);
  CHECK(out == mixed_out);
  REQUIRE_MESSAGE(c.read_named("empty.bin", out, &err), err);
  CHECK(out.empty());
  CHECK(c.find("A.bin") == -1);  // case-sensitive
  CHECK(c.find("nothing") == -1);
  CHECK_FALSE(c.read_named("nothing", out, &err));
  CHECK(out.empty());
  CHECK_FALSE(err.empty());
  // the last entry's size is total - offset
  const size_t il = c.entries().size() - 1;
  CHECK(c.entries()[il].offset + c.entries()[il].stored == col.size());
}

TEST_CASE("colfont col: a header-only collective (0 entries, 36 bytes) opens and finds nothing") {
  const Bytes col = make_col({});
  REQUIRE(col.size() == 36);
  ColFile c;
  std::string err;
  REQUIRE_MESSAGE(c.open_bytes(col, &err), err);
  CHECK(c.entries().empty());
  CHECK(c.find("x.fnt") == -1);
}

TEST_CASE("colfont col: rejections leave nothing behind and say why") {
  const Bytes good = make_col({{"a.bin", {'a', 'b', 'c'}, 1, 3}, {"b.bin", {'d'}, 1, 1}, {"c.bin", lzss_literals({1, 2, 3, 4}), 0x11, 4}});
  {
    ColFile c;
    std::string err;
    REQUIRE_MESSAGE(c.open_bytes(good, &err), err);
    REQUIRE(c.entries().size() == 3);
  }
  const auto ent = [](size_t i) { return kColHeaderSize + 16 * i; };  // directory entry i: hash, flags, offset, usize
  size_t stored_i = 99, lz_i = 99;
  for (size_t i = 0; i < 3; ++i) {
    const uint32_t fl = (good[ent(i) + 4] << 24) | (good[ent(i) + 5] << 16) | (good[ent(i) + 6] << 8) | good[ent(i) + 7];
    if (fl == 1 && stored_i == 99) stored_i = i;
    if (fl == 0x11) lz_i = i;
  }
  REQUIRE(stored_i != 99);
  REQUIRE(lz_i != 99);
  struct Case {
    const char* what;
    std::function<void(Bytes&)> mutate;
  };
  const std::vector<Case> cases = {
      {"empty", [](Bytes& b) { b.clear(); }},
      {"shorter than the 0x24 header", [](Bytes& b) { b.resize(0x23); }},
      {"flags 0 (not hash mode)", [](Bytes& b) { set_be32(b, 8, 0); }},
      {"flags 6 (the .db's)", [](Bytes& b) { set_be32(b, 8, 6); }},
      {"flags 0xc", [](Bytes& b) { set_be32(b, 8, 0xc); }},
      {"total size one more than the file", [](Bytes& b) { set_be32(b, 12, static_cast<uint32_t>(b.size() + 1)); }},
      {"total size one less than the file", [](Bytes& b) { set_be32(b, 12, static_cast<uint32_t>(b.size() - 1)); }},
      {"count above kMaxColEntries", [](Bytes& b) { set_be32(b, 4, kMaxColEntries + 1); }},
      {"count 0xffffffff", [](Bytes& b) { set_be32(b, 4, 0xffffffffu); }},
      {"directory past the end", [](Bytes& b) { set_be32(b, 4, 1000); }},
      {"entry offset inside the directory", [&](Bytes& b) { set_be32(b, ent(0) + 8, 0x24); }},
      {"entry offset past the end", [&](Bytes& b) { set_be32(b, ent(1) + 8, static_cast<uint32_t>(b.size() + 1)); }},
      {"offsets that decrease", [&](Bytes& b) { set_be32(b, ent(2) + 8, static_cast<uint32_t>(0x24 + 16 * 3)); set_be32(b, ent(1) + 8, static_cast<uint32_t>(b.size() - 1)); }},
      {"hashes out of order", [&](Bytes& b) { set_be32(b, ent(0), 0xffffffffu); }},
      {"codec nibble 0x20", [&](Bytes& b) { set_be32(b, ent(lz_i) + 4, 0x21); }},
      {"codec nibble 0xf0", [&](Bytes& b) { set_be32(b, ent(lz_i) + 4, 0xf1); }},
      {"a stored entry larger than its span", [&](Bytes& b) { set_be32(b, ent(stored_i) + 12, 0x1000); }},
      {"an entry above kMaxColEntrySize", [&](Bytes& b) { set_be32(b, ent(lz_i) + 12, static_cast<uint32_t>(kMaxColEntrySize + 1)); }},
  };
  for (const Case& k : cases) {
    Bytes b = good;
    k.mutate(b);
    ColFile c;
    std::string err;
    CHECK_MESSAGE(!c.open_bytes(std::move(b), &err), k.what);
    CHECK_MESSAGE(!err.empty(), k.what);
    CHECK_MESSAGE((!c.is_open() && c.entries().empty() && c.find("a.bin") == -1), k.what);
    Bytes out = {1, 2, 3};
    CHECK_MESSAGE(!c.read(0, out, &err), k.what);  // an unopened file reads nothing, and clears `out`
    CHECK_MESSAGE(out.empty(), k.what);
  }
  {  // above kMaxColFileSize: rejected from the size alone
    Bytes huge(static_cast<size_t>(kMaxColFileSize) + 1, 0);
    ColFile c;
    std::string err;
    CHECK_FALSE(c.open_bytes(std::move(huge), &err));
    CHECK_FALSE(err.empty());
  }
}

TEST_CASE("colfont col: a bad LZSS stream is a read error that leaves no partial decode (open still succeeds)") {
  // the stream ends after 1 of the promised 5 bytes; and a stream promising more than 9 x its size
  const Bytes col = make_col({{"short.bin", {0x01, 'x'}, 0x11, 5}, {"ratio.bin", {0xff, 'a'}, 0x11, 100}, {"ok.bin", lzss_literals({9, 8}), 0x11, 2}});
  ColFile c;
  std::string err;
  REQUIRE_MESSAGE(c.open_bytes(col, &err), err);
  Bytes out = {7};
  CHECK_FALSE(c.read_named("short.bin", out, &err));
  CHECK(out.empty());
  CHECK_FALSE(err.empty());
  out = {7};
  err.clear();
  CHECK_FALSE(c.read_named("ratio.bin", out, &err));
  CHECK(out.empty());
  CHECK_FALSE(err.empty());
  CHECK(c.read_named("ok.bin", out, &err));
  CHECK(out == Bytes{9, 8});
  CHECK_FALSE(c.read(99, out, &err));  // index out of range
  CHECK(out.empty());
}

// ===================================================================================================== .fnt (synthetic)
TEST_CASE("colfont fnt: header, records, max / min width, first-match lookup, trailing bytes allowed") {
  const TileFont f = synth_font();
  CHECK(f.rows == 1);
  CHECK(f.columns == 6);
  CHECK(f.count == 6);
  CHECK(f.char_width == 8);
  CHECK(f.char_height == 10);
  CHECK(f.kerning == 1);
  CHECK(f.baseline == 7);
  CHECK(f.space_width == 4);
  CHECK(f.max_width == 9);
  CHECK(f.min_width == 3);
  REQUIRE(f.glyphs.size() == 6);
  CHECK(f.glyphs[5].ascii == 0xF141);
  CHECK(f.glyphs[5].width == 7);
  CHECK(f.glyphs[5].y_offset == 6);
  CHECK(f.find('A') == 0);  // the duplicate record 4 never wins
  CHECK(f.find(0xF141) == 5);
  CHECK(f.find('z') == -1);
  CHECK(f.find(' ') == -1);
  Bytes b = make_fnt(1, 6, 8, 10, 1, 7, 4, kSynth);
  b.push_back(0x55);  // trailing bytes: the ctor reads count records and stops
  TileFont g;
  std::string err;
  CHECK_MESSAGE(g.parse(b.data(), b.size(), &err), err);
}

TEST_CASE("colfont fnt: max / min width compare as UNSIGNED (a negative width is the largest)") {
  // 0x6277e0 jbe / 0x627810 jae over u32; max starts at 0, min at 0xffffffff
  const Bytes b = make_fnt(1, 2, 8, 8, 0, 4, 4, {{'a', 5, 4, 0, 0}, {'b', -2, 4, 0, 0}});
  TileFont f;
  std::string err;
  REQUIRE_MESSAGE(f.parse(b.data(), b.size(), &err), err);
  CHECK(static_cast<uint32_t>(f.max_width) == 0xfffffffeu);
  CHECK(f.min_width == 5);
}

TEST_CASE("colfont fnt: rejections (caps) leave the font default and say why") {
  struct Case {
    const char* what;
    Bytes data;
  };
  const Rec r{'a', 3, 4, 0, 0};
  const std::vector<Case> cases = {
      {"empty", {}},
      {"shorter than the header", Bytes(31, 0)},
      {"count 0", make_fnt(1, 1, 8, 8, 0, 4, 4, {}, 0)},
      {"records truncated by one byte", [&] { Bytes b = make_fnt(1, 2, 8, 8, 0, 4, 4, {r, r}); b.pop_back(); return b; }()},
      {"count above kMaxFontGlyphs", make_fnt(1, 1025, 8, 8, 0, 4, 4, std::vector<Rec>(1025, r))},
      {"count 0x7fffffff over 3 records", make_fnt(1, 3, 8, 8, 0, 4, 4, {r, r, r}, 0x7fffffff)},
      {"count -1", [&] { Bytes b = make_fnt(1, 1, 8, 8, 0, 4, 4, {r}); b[8] = b[9] = b[10] = b[11] = 0xff; return b; }()},
      {"rows * columns != count", make_fnt(2, 2, 8, 8, 0, 4, 4, {r, r, r})},
      {"rows 0", make_fnt(0, 2, 8, 8, 0, 4, 4, {r, r})},
      {"columns negative", make_fnt(1, -2, 8, 8, 0, 4, 4, {r, r})},
      {"tile width 0", make_fnt(1, 1, 0, 8, 0, 4, 4, {r})},
      {"tile height above kMaxFontTile", make_fnt(1, 1, 8, 129, 0, 4, 4, {r})},
      {"tile width negative", make_fnt(1, 1, -8, 8, 0, 4, 4, {r})},
      {"kerning beyond kMaxFntValue", make_fnt(1, 1, 8, 8, 65537, 4, 4, {r})},
      {"baseline beyond kMaxFntValue", make_fnt(1, 1, 8, 8, 0, -65537, 4, {r})},
      {"space beyond kMaxFntValue", make_fnt(1, 1, 8, 8, 0, 4, 1 << 20, {r})},
      {"a glyph width beyond kMaxFntValue", make_fnt(1, 1, 8, 8, 0, 4, 4, {{'a', 1 << 20, 4, 0, 0}})},
      {"a glyph yOffset beyond kMaxFntValue", make_fnt(1, 1, 8, 8, 0, 4, 4, {{'a', 3, 4, 0, -(1 << 20)}})},
  };
  for (const Case& k : cases) {
    TileFont f;
    std::string err;
    CHECK_MESSAGE(!f.parse(k.data.data(), k.data.size(), &err), k.what);
    CHECK_MESSAGE(!err.empty(), k.what);
    CHECK_MESSAGE((f.rows == 0 && f.columns == 0 && f.count == 0 && f.glyphs.empty() && f.max_width == 0 && f.min_width == 0), k.what);
  }
  {  // the limits themselves are accepted
    TileFont f;
    std::string err;
    const Bytes ok = make_fnt(1, 1, kMaxFontTile, kMaxFontTile, kMaxFntValue, -kMaxFntValue, kMaxFntValue, {{'a', kMaxFntValue, -kMaxFntValue, kMaxFntValue, -kMaxFntValue}});
    CHECK_MESSAGE(f.parse(ok.data(), ok.size(), &err), err);
  }
}

// ============================================================================================ widths (synthetic)
// Hand-computed on kSynth (kerning 1, space 4, maxWidth 9): A 1+6 = 7, g 6, q 4, j 5, space 5, unknown 10, 0xF141 8.
TEST_CASE("colfont width: GetWidth cases (space, known, first match, unknown, 2-byte code)") {
  const TileFont f = synth_font();
  CHECK(f.get_width('A') == 7.0f);   // the first record's 6, not the duplicate's 9
  CHECK(f.get_width('g') == 6.0f);
  CHECK(f.get_width(' ') == 5.0f);   // (space + kerning)
  CHECK(f.get_width('z') == 10.0f);  // (maxWidth + kerning)
  CHECK(f.get_width(0xF141) == 8.0f);
  CHECK(f.get_width('A', 2.0f) == 14.0f);
  CHECK(f.get_width(' ', 0.5f) == 2.5f);
  CHECK(f.get_width('z', 1.5f) == 15.0f);
}

TEST_CASE("colfont width: GetStrLen = sum of widths minus the trailing kerning, truncated") {
  const TileFont f = synth_font();
  CHECK(slen(f, "") == -1);  // 0 - 1 * kerning: the formula has no special case for the empty string
  CHECK(slen(f, "A") == 6);
  CHECK(slen(f, "Ag") == 12);
  CHECK(slen(f, "A g") == 17);   // 7 + 5 + 6 - 1
  CHECK(slen(f, "Az") == 16);    // 7 + 10 - 1
  CHECK(slen(f, "zzz") == 29);
  // '\n' ends the string
  CHECK(slen(f, "A\nA") == 6);
  CHECK(slen(f, "\nA") == -1);
  // 0xFF skips itself and the next character, with no width
  CHECK(slen(f, std::string("A\xff") + "gq") == 10);       // 7 + 4 - 1
  CHECK(slen(f, std::string("\xff") + "A") == -1);
  // ... and a 0xF0..0xFE lead after it skips its trailing byte too
  CHECK(slen(f, std::string("\xff\xf1\x41q")) == 3);       // only q: 4 - 1
  // a 2-byte code is lead * 256 + next
  CHECK(slen(f, std::string("\xf1\x41")) == 7);            // 8 - 1
  CHECK(slen(f, std::string("A\xf1\x41g")) == 20);         // 7 + 8 + 6 - 1
  CHECK(slen(f, std::string("\xf2\x41")) == 9);            // 0xF241 has no record: unknown, 10 - 1
  // a lead byte as the last byte: the next byte reads as 0, the code is 0xF100 (unknown)
  CHECK(slen(f, "\xf1") == 9);
  // the string is length-delimited: an embedded NUL is an ordinary (unknown) character
  CHECK(slen(f, std::string("A\0A", 3)) == 7 + 10 + 7 - 1);
  // truncation toward zero at a fractional scale (a round-to-nearest would give 4 and 6)
  CHECK(slen(f, "q", 1.25f) == 3);    // 4 * 1.25 - 1.25 = 3.75
  CHECK(slen(f, "Ag", 1.5f) == 18);   // exactly 19.5 - 1.5
  CHECK(slen(f, "Ag", 1.25f) == 15);  // 8.75 + 7.5 - 1.25 = 15 exactly in binary
  CHECK(slen(f, "", 2.0f) == -2);
  CHECK(slen(f, "", 0.5f) == 0);      // -0.5 truncates to 0
}

TEST_CASE("colfont width: negative sums use the original's UNSIGNED conversions (INFERRED from the asm; never run)") {
  // kerning -4 and a glyph of width 3: kerning + width = -1 as u32 = 0xffffffff, converted by `fild qword` (high dword 0)
  const Bytes b = make_fnt(1, 1, 8, 8, -4, 4, 6, {{'x', 3, 4, 0, 0}});
  TileFont f;
  std::string err;
  REQUIRE_MESSAGE(f.parse(b.data(), b.size(), &err), err);
  CHECK(f.get_width('x') == 4294967296.0f);  // float(4294967295) rounds up to 2^32
  CHECK(f.get_width(' ') == 2.0f);           // the space is the signed `fild dword`: 6 - 4
  CHECK(f.get_width('y') == 4294967296.0f - 0.0f);  // unknown: maxWidth 3 + kerning -4, the same wrap
  // GetStrLen: (2^32 as f32) - (-4 * 1) = 4294967300; fistp qword, the low dword is returned
  CHECK(f.get_str_len("x") == 4);
}

// =========================================================================================== heights (synthetic)
// The asm gates are unsigned (0x628d76 jb, 0x628dae jbe): baseline 4 against yOffset -2 (0xfffffffe) is baseline < yOffset,
// so below stays 0 and above = h = 3. A signed compare would give below = 6 and height 9.
TEST_CASE("colfont height: GetStrHeight compares baseline and a negative yOffset unsigned") {
  TileFont f;
  const Bytes b = make_fnt(1, 1, 8, 10, 1, 4, 4, {{'A', 6, 3, 0, -2}});
  std::string err;
  REQUIRE_MESSAGE(f.parse(b.data(), b.size(), &err), err);
  int v = -999;
  CHECK(f.get_str_height("A", 1.0f, &v));
  CHECK(v == 3);
}

TEST_CASE("colfont height: GetStrHeight (below = max baseline - yOffset, above = height - d or height)") {
  const TileFont f = synth_font();
  auto h = [&](const std::string& s, float ys, bool* ok = nullptr, float* above = nullptr) {
    int v = -999;
    const bool r = f.get_str_height(s, ys, &v, above);
    if (ok) *ok = r;
    return v;
  };
  bool ok = false;
  float above = -1;
  CHECK(h("A", 1.0f, &ok, &above) == 8);  // d = 7 - 0 = 7, above = 8 - 7 = 1: 1 + 7
  CHECK(ok);
  CHECK(above == 1.0f);
  CHECK(h("g", 1.0f) == 9);               // d = 4, above = 9 - 4 = 5
  CHECK(h("Ag", 1.0f, &ok, &above) == 12);  // below max(7, 4) = 7, above max(1, 5) = 5
  CHECK(above == 5.0f);
  CHECK(h("q", 1.0f) == 2);               // yo == baseline: d = 0, baseline > yo is false: above = h = 2
  CHECK(h("j", 1.0f) == 5);               // yo > baseline: below untouched, above = h = 5
  CHECK(h(std::string("\xf1\x41"), 1.0f) == 3);  // d = 1, above = 3 - 1 = 2: 2 + 1
  CHECK(h("Aq", 1.0f) == 9);              // below 7, above max(1, 2) = 2
  // the unscaled d against a scaled height (the original's mix): A at 0.5: 4 - 7 < 0 never beats the initial 0
  CHECK(h("A", 0.5f, &ok, &above) == 7);
  CHECK(above == 0.0f);
  CHECK(h("g", 0.5f, &ok, &above) == 4);  // 4.5 - 4 = 0.5 above, below 4: 4.5 truncates to 4
  CHECK(above == 0.5f);
  // '\n' ends the walk; a glyph after it is never looked at
  CHECK(h("A\nz", 1.0f, &ok) == 8);
  CHECK(ok);
  // 0xFF skips the next character
  CHECK(h(std::string("A\xff") + "z", 1.0f, &ok) == 8);
  CHECK(ok);
  // hud.md §4.9 (INFERRED): a code with no record, a space included, is skipped; the other glyphs' height is unchanged
  CHECK(h("A g", 1.0f, &ok, &above) == h("Ag", 1.0f));
  CHECK(ok);
  CHECK(h("Az", 1.0f, &ok) == h("A", 1.0f));
  CHECK(h("zA z ", 1.0f) == 8);
  CHECK(h(" ", 1.0f, &ok, &above) == 0);  // nothing counted: trunc(0 + 0)
  CHECK(ok);
  CHECK(above == 0.0f);
  CHECK(h("", 1.0f) == 0);
}

// ================================================================================ real data (native; needs LOMN_DATA)
#ifndef __EMSCRIPTEN__

namespace {

#define NEED_DATA(x)                      \
  if (!(x)) {                             \
    MESSAGE("SKIPPED (no LOMN_DATA)");    \
    return;                               \
  }

std::string data_root() {
  const char* ld = std::getenv("LOMN_DATA");
  return ld ? ld : "";
}
std::unique_ptr<fs::FileSystem> real_vfs() {
  const std::string r = data_root();
  if (r.empty()) return nullptr;
  return fs::open_disk(r + "/install", "");
}

struct FontExpect {
  const char* name;
  uint32_t dir;  // .col directory entries
  int rows, cols, n, cw, ch, kern, base, space;
  uint32_t maxw, minw;
  uint64_t alpha_sum;  // sum of the alpha byte over every glyph TGA (this task's own Python over the raw TGA bytes)
  int len_new, len_opt, len_quit, len_play;  // GetStrLen "New Game" / "Options" / "Quit" / "Play Game", scale 1.0
  int h_new, h_opt, h_quit, h_play;          // GetStrHeight, glyphs with no record (spaces) skipped
};
// ASCII order of the names (what the printer sorts by). Measured 2026-11-13 with a Python reader of this task's own.
const FontExpect kFonts[] = {
    {"BionicleFont", 153, 1, 145, 145, 16, 16, 0, 12, 8, 15, 3, 2074656, 84, 57, 32, 82, 10, 11, 11, 11},
    {"BionicleFontHi", 86, 1, 79, 79, 32, 32, 4, 12, 16, 25, 2, 1682921, 165, 113, 57, 165, 15, 20, 15, 20},
    {"BionicleFontSh", 87, 1, 79, 79, 32, 32, 4, 12, 16, 25, 3, 1682921, 167, 115, 58, 170, 16, 20, 16, 20},
    {"BionicleFontshad", 87, 1, 79, 79, 32, 32, 4, 12, 16, 26, 3, 2941419, 170, 120, 61, 172, 17, 22, 17, 22},
    {"Tech_Demo_Font01", 102, 1, 46, 46, 32, 32, 8, 12, 16, 22, 2, 984059, 210, 195, 105, 239, 12, 11, 13, 11},
    {"Tech_Demo_Font_Highlighted", 104, 1, 46, 46, 32, 32, 8, 12, 16, 22, 2, 984059, 210, 195, 105, 239, 12, 11, 13, 11},
    {"tech_font_01", 167, 1, 79, 79, 32, 32, 8, 12, 16, 30, 2, 1763431, 250, 243, 129, 287, 12, 11, 13, 11},
    {"tech_font_01_hilight", 170, 1, 79, 79, 32, 32, 8, 12, 16, 30, 2, 1763431, 250, 243, 129, 287, 12, 11, 13, 11},
};

std::string fonts_dir() { return data_root() + "/install/data/Art/Fonts"; }

bool ends_with_ci(std::string s, const std::string& suf) {
  for (char& c : s) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
  return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

}  // namespace

TEST_CASE("colfont real: the install has exactly 8 tile fonts (probe 2's '9' is a miscount)") {
  const std::string d = fonts_dir();
  NEED_DATA(!data_root().empty() && std::filesystem::is_directory(d));
  std::vector<std::string> stems;
  for (const auto& e : std::filesystem::directory_iterator(d)) {
    const std::string n = e.path().filename().string();
    if (ends_with_ci(n, ".col")) stems.push_back(n.substr(0, n.size() - 4));
  }
  std::sort(stems.begin(), stems.end());  // byte order = ASCII order
  std::vector<std::string> want;
  for (const FontExpect& f : kFonts) want.push_back(f.name);
  CHECK(stems == want);
}

// A11: the glyph set of BionicleFont equals the .col directory and the charset of the packed .fnt
TEST_CASE("colfont real: every font loads: header fields, 32 bpp descriptor-0 glyph TGAs with REAL alpha, sizes") {
  auto vfs = real_vfs();
  NEED_DATA(vfs);
  for (const FontExpect& e : kFonts) {
    INFO("font " << e.name);
    LoadedFont lf;
    std::string err;
    REQUIRE_MESSAGE(load_tile_font(*vfs, std::string("Root/Data/Art/Fonts/") + e.name, lf, &err), err);
    CHECK(lf.name == e.name);
    CHECK(lf.col.entries().size() == e.dir);
    const TileFont& f = lf.font;
    CHECK(f.rows == e.rows);
    CHECK(f.columns == e.cols);
    CHECK(f.count == e.n);
    CHECK(f.char_width == e.cw);
    CHECK(f.char_height == e.ch);
    CHECK(f.kerning == e.kern);
    CHECK(f.baseline == e.base);
    CHECK(f.space_width == e.space);
    CHECK(static_cast<uint32_t>(f.max_width) == e.maxw);
    CHECK(static_cast<uint32_t>(f.min_width) == e.minw);
    REQUIRE(f.glyphs.size() == static_cast<size_t>(e.n));
    REQUIRE(lf.glyph_images.size() == static_cast<size_t>(e.n));
    REQUIRE(lf.glyph_tga.size() == static_cast<size_t>(e.n));
    uint64_t alpha = 0, zero = 0, nonzero = 0;
    for (size_t i = 0; i < lf.glyph_images.size(); ++i) {
      const Image& im = lf.glyph_images[i];
      const TgaInfo& ti = lf.glyph_tga[i];
      REQUIRE(im.width == static_cast<uint32_t>(e.cw));
      REQUIRE(im.height == static_cast<uint32_t>(e.ch));
      CHECK(ti.image_type == 2);
      CHECK(ti.bits == 32);
      CHECK(ti.alpha_bits == 0);  // descriptor 0x00: no alpha declared ...
      CHECK_FALSE(ti.top_down);
      for (size_t p = 3; p < im.rgba.size(); p += 4) {  // ... and the 4th byte is real
        alpha += im.rgba[p];
        (im.rgba[p] ? nonzero : zero) += 1;
      }
    }
    CHECK(alpha == e.alpha_sum);
    CHECK(zero > 0);
    CHECK(nonzero > 0);
    if (std::string(e.name) == "BionicleFont") {
      CHECK(nonzero == 11180);
      CHECK(zero == 25940);
    }
  }
}

TEST_CASE("colfont real: A11 glyph set: 145 glyphs = 145 .fnt records = 145 directory TGAs; charset = the packed .info = the .fnt ascii column") {
  auto vfs = real_vfs();
  NEED_DATA(vfs);
  LoadedFont lf;
  std::string err;
  REQUIRE_MESSAGE(load_tile_font(*vfs, "Root/Data/Art/Fonts/BionicleFont", lf, &err), err);
  CHECK(lf.font.glyphs.size() == 145);
  CHECK(lf.glyph_images.size() == 145);
  // the directory TGAs: the .col.db names the 153 entries (36-byte BE header; then LE u32 hash, u32 mtime, u8 len, name)
  std::vector<uint8_t> db;
  REQUIRE(vfs->read("Root/Data/Art/Fonts/BionicleFont.col.db", db) == fs::Error::None);
  REQUIRE(db.size() > 36);
  size_t p = 36;
  std::vector<std::string> names;
  std::set<uint32_t> db_hashes;
  while (p + 9 <= db.size()) {
    const uint32_t h = db[p] | db[p + 1] << 8 | db[p + 2] << 16 | static_cast<uint32_t>(db[p + 3]) << 24;
    const size_t len = db[p + 8];
    REQUIRE(p + 9 + len <= db.size());
    std::string n(db.begin() + static_cast<std::ptrdiff_t>(p + 9), db.begin() + static_cast<std::ptrdiff_t>(p + 9 + len));
    REQUIRE(n.rfind(".\\", 0) == 0);
    n = n.substr(2);
    CHECK(test_hash(n) == h);
    CHECK(lf.col.find(n) >= 0);
    db_hashes.insert(h);
    names.push_back(n);
    p += 9 + len;
  }
  CHECK(p == db.size());
  CHECK(names.size() == 153);
  size_t tga = 0, glyph_tga = 0;
  for (const std::string& n : names) {
    if (ends_with_ci(n, ".tga")) ++tga;
    for (int i = 0; i < 145; ++i)
      if (n == "BionicleFont0_" + std::to_string(i) + ".tga") ++glyph_tga;
  }
  CHECK(tga == 146);  // 145 glyphs + the whole-sheet BionicleFont.tga
  CHECK(glyph_tga == 145);
  // the charset line of the .info packed in the .col: `\xHH` escapes for bytes >= 0xA0, 7 lines after the `//` comments
  std::vector<uint8_t> info;
  REQUIRE_MESSAGE(lf.col.read_named("BionicleFont.info", info, &err), err);
  std::vector<std::string> lines;
  {
    std::string cur;
    for (uint8_t c : info) {
      if (c == '\n') {
        if (!cur.empty() && cur.back() == '\r') cur.pop_back();
        lines.push_back(cur);
        cur.clear();
      } else {
        cur += static_cast<char>(c);
      }
    }
    if (!cur.empty()) lines.push_back(cur);
  }
  std::vector<std::string> body;
  for (const auto& l : lines)
    if (l.rfind("//", 0) != 0 && !l.empty()) body.push_back(l);
  REQUIRE(body.size() == 7);
  CHECK(body[2] == "0");  // the packed .info's kerning (the on-disk copy says -4: stale, never read)
  std::vector<uint16_t> charset;
  for (size_t i = 0; i < body[6].size();) {
    if (body[6][i] == '\\' && i + 3 < body[6].size() + 0 && body[6][i + 1] == 'x') {
      charset.push_back(static_cast<uint16_t>(std::stoi(body[6].substr(i + 2, 2), nullptr, 16)));
      i += 4;
    } else {
      charset.push_back(static_cast<uint8_t>(body[6][i]));
      i += 1;
    }
  }
  REQUIRE(charset.size() == 145);
  for (size_t i = 0; i < 145; ++i) CHECK(charset[i] == lf.font.glyphs[i].ascii);
}

TEST_CASE("colfont real: the .col.db names equal the directory hashes of every font; the packed '.col' leftovers are empty header-only files") {
  auto vfs = real_vfs();
  NEED_DATA(vfs);
  for (const FontExpect& e : kFonts) {
    INFO("font " << e.name);
    LoadedFont lf;
    std::string err;
    REQUIRE_MESSAGE(load_tile_font(*vfs, std::string("Root/Data/Art/Fonts/") + e.name, lf, &err), err);
    std::vector<uint8_t> db;
    REQUIRE(vfs->read(std::string("Root/Data/Art/Fonts/") + e.name + ".col.db", db) == fs::Error::None);
    REQUIRE(db.size() > 36);
    std::set<uint32_t> want, got;
    for (const ColEntry& c : lf.col.entries()) want.insert(c.hash);
    size_t p = 36, cols = 0;
    while (p + 9 <= db.size()) {
      got.insert(db[p] | db[p + 1] << 8 | db[p + 2] << 16 | static_cast<uint32_t>(db[p + 3]) << 24);
      const size_t len = db[p + 8];
      REQUIRE(p + 9 + len <= db.size());
      std::string n(db.begin() + static_cast<std::ptrdiff_t>(p + 9), db.begin() + static_cast<std::ptrdiff_t>(p + 9 + len));
      n = n.substr(2);
      if (ends_with_ci(n, ".col")) {  // a collective packed INSIDE the collective
        ++cols;
        std::vector<uint8_t> inner;
        REQUIRE_MESSAGE(lf.col.read_named(n, inner, &err), err);
        CHECK(inner.size() == 36);  // a header and nothing else
        ColFile ic;
        REQUIRE_MESSAGE(ic.open_bytes(inner, &err), err);
        CHECK(ic.entries().empty());          // no glyphs, no .fnt: it is not a font
        CHECK(ic.find(n.substr(0, n.size() - 4) + ".fnt") == -1);
      }
      p += 9 + len;
    }
    CHECK(p == db.size());
    CHECK(got == want);
    // every font has its own build-leftover .col; Tech_Demo_Font_Highlighted has a second one, Tech_Demo_Font01.col
    CHECK(cols == (std::string(e.name) == "Tech_Demo_Font_Highlighted" ? 2u : 1u));
  }
}

TEST_CASE("colfont real: A11 menu string widths and heights, scale 1.0 (and probe 2's 84/57/32 and 165/113/57)") {
  auto vfs = real_vfs();
  NEED_DATA(vfs);
  for (const FontExpect& e : kFonts) {
    INFO("font " << e.name);
    LoadedFont lf;
    std::string err;
    REQUIRE_MESSAGE(load_tile_font(*vfs, std::string("Root/Data/Art/Fonts/") + e.name, lf, &err), err);
    const TileFont& f = lf.font;
    CHECK(f.get_str_len("New Game") == e.len_new);
    CHECK(f.get_str_len("Options") == e.len_opt);
    CHECK(f.get_str_len("Quit") == e.len_quit);
    CHECK(f.get_str_len("Play Game") == e.len_play);
    int h = -7;
    CHECK(f.get_str_height("New Game", 1.0f, &h));
    CHECK(h == e.h_new);
    CHECK(f.get_str_height("Options", 1.0f, &h));
    CHECK(h == e.h_opt);
    CHECK(f.get_str_height("Quit", 1.0f, &h));
    CHECK(h == e.h_quit);
    CHECK(f.get_str_height("Play Game", 1.0f, &h));
    CHECK(h == e.h_play);
  }
}

TEST_CASE("colfont real: a missing font is an error that names the file (hud.md 7.2: a VM error)") {
  auto vfs = real_vfs();
  NEED_DATA(vfs);
  LoadedFont lf;
  std::string err;
  CHECK_FALSE(load_tile_font(*vfs, "Root/Data/Art/Fonts/NoSuchFont", lf, &err));
  CHECK(err.find("NoSuchFont") != std::string::npos);
  CHECK(lf.glyph_images.empty());
  CHECK(lf.font.glyphs.empty());
  CHECK_FALSE(lf.col.is_open());
}

// ----------------------------------------------------------------------------------------- the HUD sprite textures (F7)
// F7: Setup's HUD set is 136 loadhudsprite calls over 124 distinct files. gcbigsprite.loadhudsprite reaches the pixels
// through ScDrawableTexture::Load: with no current blockfile SrBlockFileLoad takes Art.blk first, then the disk (hud.md
// §7.1; natives.md §1.3); the extension is appended ".dds" then ".tga" (the first that exists).
TEST_CASE("colfont real: every HUD sprite file of F7 loads through the VFS and the block files (123 of 124; turnhead0024 is absent)") {
  auto vfs = real_vfs();
  NEED_DATA(vfs);
  std::ifstream in(std::string(LOMN_TESTS_DIR) + "/data/hud_sprite_files.txt");
  REQUIRE(in.good());
  std::vector<std::string> list;
  for (std::string l; std::getline(in, l);)
    if (!l.empty()) list.push_back(l);
  REQUIRE(list.size() == 124);
  BlockChain chain(*vfs);
  std::string err;
  REQUIRE_MESSAGE(chain.open_art("Root/Data/Art/Art.blk", &err), err);
  size_t found = 0, raw = 0, rle = 0, desc0 = 0;
  uint64_t pixels = 0;
  std::vector<std::string> missing;
  for (const std::string& name : list) {
    std::string chosen;
    for (const char* ext : {".dds", ".tga"})
      if (chosen.empty() && chain.exists(name + ext)) chosen = name + ext;
    if (chosen.empty()) {
      missing.push_back(name);
      continue;
    }
    std::vector<uint8_t> bytes;
    Source from = Source::None;
    REQUIRE_MESSAGE(chain.load(chosen, bytes, &from, &err), name << ": " << err);
    CHECK(from == Source::Art);
    Image img;
    TgaInfo ti;
    REQUIRE_MESSAGE(decode_tga(bytes.data(), bytes.size(), img, &ti, &err), name << ": " << err);
    CHECK(ti.bits == 32);
    ++found;
    (ti.image_type == 2 ? raw : rle) += 1;
    if (ti.alpha_bits == 0) ++desc0;
    pixels += static_cast<uint64_t>(img.width) * img.height;
  }
  CHECK(found == 123);
  REQUIRE(missing.size() == 1);
  CHECK(missing[0] == "Root/data/art/hud/turnhead0024");  // Art.blk holds only turnhead.tga; hud.md 7.1: no error, a null texture
  CHECK(raw == 7);   // 6 type-2 with descriptor 0x08 and one with descriptor 0 (hud/Black, alpha 255 throughout)
  CHECK(rle == 116);
  CHECK(desc0 == 1);
  CHECK(pixels == 482304);
  // a missing sprite file is not an error of the file layer: nothing exists, nothing loads, no exception
  std::vector<uint8_t> none;
  Source from = Source::Art;
  CHECK_FALSE(chain.load("Root/data/art/hud/turnhead0024.tga", none, &from, &err));
  CHECK(from == Source::None);
}

// ================================================================================================================ asm pins
namespace {
struct PeImage {
  std::vector<uint8_t> d;
  uint32_t base = 0;
  struct Sec {
    uint32_t va, vsz, rsz, rptr;
  };
  std::vector<Sec> secs;
  uint32_t le32(size_t o) const { return d[o] | d[o + 1] << 8 | d[o + 2] << 16 | static_cast<uint32_t>(d[o + 3]) << 24; }
  uint32_t le16(size_t o) const { return d[o] | d[o + 1] << 8; }
  bool load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    d.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    if (d.size() < 0x100 || d[0] != 'M' || d[1] != 'Z') return false;
    const size_t pe = le32(0x3c);
    if (pe + 24 > d.size() || d[pe] != 'P') return false;
    const size_t n = le16(pe + 6), opt = le16(pe + 20);
    base = le32(pe + 24 + 28);
    for (size_t i = 0; i < n; ++i) {
      const size_t o = pe + 24 + opt + i * 40;
      if (o + 40 > d.size()) return false;
      secs.push_back({le32(o + 12), le32(o + 8), le32(o + 16), le32(o + 20)});
    }
    return true;
  }
  std::string hex(uint32_t va, size_t n) const {
    std::string s;
    char t[4];
    for (size_t i = 0; i < n; ++i) {
      const uint32_t rva = va + static_cast<uint32_t>(i) - base;
      bool hit = false;
      for (const Sec& sc : secs)
        if (rva >= sc.va && rva < sc.va + std::max(sc.vsz, sc.rsz)) {
          hit = true;
          const uint8_t b = (rva - sc.va < sc.rsz && sc.rptr + (rva - sc.va) < d.size()) ? d[sc.rptr + (rva - sc.va)] : 0;
          std::snprintf(t, sizeof t, "%02x", b);
          s += (i ? " " : "") + std::string(t);
          break;
        }
      if (!hit) return "<outside the image>";
    }
    return s;
  }
  uint32_t u32(uint32_t va) const {
    uint32_t v = 0;
    for (int i = 3; i >= 0; --i) {
      const std::string h = hex(va + static_cast<uint32_t>(i), 1);
      v = v << 8 | static_cast<uint32_t>(std::stoi(h, nullptr, 16));
    }
    return v;
  }
};
const PeImage* exe() {
  static std::unique_ptr<PeImage> im;
  static bool tried = false;
  if (!tried) {
    tried = true;
    const std::string r = data_root();
    if (!r.empty()) {
      auto p = std::make_unique<PeImage>();
      if (p->load(r + "/reference/LEGOBionicle.exe")) im = std::move(p);
    }
  }
  return im.get();
}
#define PIN(img, va, bytes) CHECK_MESSAGE((img)->hex((va), sizeof(bytes) / 3) == bytes, "pin at " #va)
}  // namespace

// Every pin names its instruction; read with `objdump -d --x86-asm-syntax=intel` of LEGOBionicle.exe on 2026-11-14.
TEST_CASE("colfont asm pins: .col header, directory, hash, codec (ScCollectiveFile 0x4cf390 / 0x4cf150 / 0x4cfa50 / 0x4cfc20, SrHashString 0x4f6af0)") {
  const PeImage* im = exe();
  NEED_DATA(im);
  PIN(im, 0x4f6b00, "69 c0 83 00 00 00 0f b6 13 01 d0 43 80 3b 00 75 ef");  // SrHashString: h = h * 0x83 + byte, stop at NUL
  PIN(im, 0x4cfc62, "c1 e3 04");  // GetInfo: 16-byte entries (shl 4)
  PIN(im, 0x4cfc6b, "3b 03 77 05");  // GetInfo: binary search compares the hash, unsigned (cmp eax,[ebx]; ja)
  PIN(im, 0x4cfca0, "8b 54 33 04");  // GetInfo: entry +4 is the flags (out+0)
  PIN(im, 0x4cfcb5, "8b 54 33 08");  // GetInfo: entry +8 is the offset (out+4)
  PIN(im, 0x4cfccb, "8b 54 33 0c");  // GetInfo: entry +0xc is the usize (out+0xc)
  PIN(im, 0x4cfceb, "8b 58 0c");  // GetInfo: last entry, size = total [obj+0xc] - offset
  PIN(im, 0x4cfcf1, "2b 5c 06 08");  // GetInfo: last entry, sub offset
  PIN(im, 0x4cfd15, "8b 5c 33 18");  // GetInfo: other entries, next offset [+0x18] ...
  PIN(im, 0x4cfd19, "2b 5c 17 08");  // GetInfo: ... minus this offset [+8]
  PIN(im, 0x4cf3a2, "6a 04");  // ReadHeader: 4 bytes into obj+8 (version)
  PIN(im, 0x4cf3a7, "05 08 00 00 00");  // ReadHeader: ... obj+8
  PIN(im, 0x4cf401, "05 0c 00 00 00");  // ReadHeader: obj+0xc (count)
  PIN(im, 0x4cf45b, "05 10 00 00 00");  // ReadHeader: obj+0x10 (flags)
  PIN(im, 0x4cf4b5, "05 14 00 00 00");  // ReadHeader: obj+0x14 (total size)
  PIN(im, 0x4cf50a, "6a 14");  // ReadHeader: then 0x14 raw bytes at obj+0x18
  PIN(im, 0x4cf50c, "83 45 d8 18");  // ReadHeader: ... pointer = obj+0x18
  PIN(im, 0x4cf1b5, "81 67 10 fe ff ff ff");  // Open: flags bit 0 cleared on load
  PIN(im, 0x4cf1f3, "25 0e 00 00 00 3d 04 00 00 00");  // Open: flags & 0xe selects the entry class ...
  PIN(im, 0x4cf619, "81 e2 f0 00 00 00");  // SubFileLoadEntry: the codec is flags & 0xf0 ...
  PIN(im, 0x4cf626, "e8 e5 3d 00 00");  // SubFileLoadEntry: ... SrGetCompressionCallback
  PIN(im, 0x4cfa62, "c1 e0 04");  // ReadEntries: 16 bytes per entry
  PIN(im, 0x4cfa8d, "6a 04");  // ReadEntries: four 4-byte reads
}

TEST_CASE("colfont asm pins: ScTileFont ctor 0x626b40: header fields, 18-byte records, unsigned max / min") {
  const PeImage* im = exe();
  NEED_DATA(im);
  PIN(im, 0x627460, "89 5a 38");  // ScTileFont ctor: numberOfCharacters -> +0x38
  PIN(im, 0x6274c7, "89 5a 3c");  // ScTileFont ctor: charWidth -> +0x3c
  PIN(im, 0x62752e, "89 5a 40");  // ScTileFont ctor: charHeight -> +0x40
  PIN(im, 0x627595, "89 5a 48");  // ScTileFont ctor: kerning -> +0x48
  PIN(im, 0x62760a, "89 5a 44");  // ScTileFont ctor: baseline -> +0x44
  PIN(im, 0x627671, "89 5a 4c");  // ScTileFont ctor: spaceWidth -> +0x4c
  PIN(im, 0x627717, "69 bd 04 f9 ff ff 20 01 00 00");  // ScTileFont ctor: records stride 0x120 in memory
  PIN(im, 0x62772a, "6a 02");  // ScTileFont ctor: ascii is a 2-byte read
  PIN(im, 0x627776, "83 c7 04 6a 04");  // ScTileFont ctor: width is the 4-byte read at record+4
  PIN(im, 0x62783a, "83 c7 08 6a 04");  // ScTileFont ctor: height read at record+8
  PIN(im, 0x626d71, "c7 42 54 00 00 00 00");  // ScTileFont ctor: maxWidth starts at 0
  PIN(im, 0x626d7e, "c7 42 50 ff ff ff ff");  // ScTileFont ctor: minWidth starts at 0xffffffff
  PIN(im, 0x6277e0, "39 d3 76 13");  // ScTileFont ctor: max update, unsigned (cmp; jbe)
  PIN(im, 0x627810, "39 d6 73 13");  // ScTileFont ctor: min update, unsigned (cmp; jae)
}

TEST_CASE("colfont asm pins: GetWidth 0x6289a0, GetHeight 0x628a50, GetStrLen 0x628ae0, GetStrHeight 0x628bf0") {
  const PeImage* im = exe();
  NEED_DATA(im);
  PIN(im, 0x6289b0, "3d 20 00 00 00");  // GetWidth: space test on the 16-bit code
  PIN(im, 0x6289b7, "8b 55 4c 03 55 48");  // GetWidth: space = (space + kerning) ...
  PIN(im, 0x6289c0, "db 04 24 d8 4d 08");  // GetWidth: ... fild dword (signed) * xScale
  PIN(im, 0x6289f9, "03 5c fe 04");  // GetWidth: known glyph = kerning + width ...
  PIN(im, 0x628a05, "c7 44 24 04 00 00 00 00 df 2c 24 d8");  // GetWidth: ... zero-extended to a qword (unsigned)
  PIN(im, 0x628a20, "8b 55 54 03 55 48");  // GetWidth: unknown = maxWidth + kerning
  PIN(im, 0x6289d4, "6b f7 24 8b 5d 58 66 3b 14 f3 74 0b");  // GetWidth: first match wins (linear search by ascii)
  PIN(im, 0x628b0b, "3c fe 77 18 3c f0 72 14");  // GetStrLen: lead byte test 0xF0..0xFE
  PIN(im, 0x628b20, "c1 e0 08 01 c2");  // GetStrLen: code = lead << 8 + next
  PIN(im, 0x628b2c, "80 3a ff 75");  // GetStrLen: 0xFF test on the byte at the current index
  PIN(im, 0x628b65, "80 3a 0a 75");  // GetStrLen: newline test, then fistp with RC = chop (0xc00)
  PIN(im, 0x628b76, "66 81 4d e2 00 0c");  // GetStrLen: or 0xc00 into the control word
  PIN(im, 0x628b7f, "df 7d ec");  // GetStrLen: fistp qword
  PIN(im, 0x628ba0, "d8 45 e4 d9 5d e4");  // GetStrLen: width added and stored as f32
  PIN(im, 0x628bb5, "db 47 48 d8 4f 08 d8 6d e4");  // GetStrLen: the end: kerning * xScale subtracted
  PIN(im, 0x628bc1, "66 81 4d e2 00 0c");  // GetStrLen: end chop
  PIN(im, 0x628bca, "df 7d ec");  // GetStrLen: end fistp qword, low dword returned
  PIN(im, 0x628bd6, "8b 45 ec");  // GetStrLen: returns eax = low dword
  PIN(im, 0x628c6c, "80 3a ff 75");  // GetStrHeight: 0xFF test
  PIN(im, 0x628cc0, "80 3a 0a 75");  // GetStrHeight: newline test
  PIN(im, 0x628d45, "2b 5c fe 10");  // GetStrHeight: d = baseline - yOffset (record + 0x10)
  PIN(im, 0x628d76, "39 d7 72 23");  // GetStrHeight: below update gate, baseline < yOffset unsigned skips
  PIN(im, 0x628dae, "39 d7 76 25");  // GetStrHeight: above candidate when baseline > yOffset (jbe skips)
  PIN(im, 0x628dd2, "d8 6d d4");  // GetStrHeight: candidate = height - d
  PIN(im, 0x628e0b, "d8 45 e4");  // GetStrHeight: result = above + below ...
  PIN(im, 0x628e11, "66 81 4d d2 00 0c");  // GetStrHeight: ... chop
  PIN(im, 0x628a60, "3d 20 00 00 00");  // GetHeight: space test
  PIN(im, 0x628a67, "d9 05 28 82 75 00");  // GetHeight: space = [0x758228]
  PIN(im, 0x628aa2, "8b 54 f3 08");  // GetHeight: glyph height at record + 8
  PIN(im, 0x628ac1, "8b 55 40");  // GetHeight: unknown = charHeight [+0x40]
  // the float constants GetHeight / GetStrLen / GetStrHeight start from are all +0.0
  for (uint32_t va : {0x758228u, 0x75822cu, 0x758230u, 0x758234u, 0x758238u, 0x75823cu}) CHECK_MESSAGE(im->u32(va) == 0u, "0x" << std::hex << va);
}
#endif  // !__EMSCRIPTEN__
