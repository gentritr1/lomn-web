// M1a-4 cleanup C3 (review M-6): the fuzz driver (fuzz_formats.cpp) decides "deep reach" by looking for fragments in the
// parsers' error strings. If a parser message is renamed the driver would silently report 0 % deep reach for that
// format. fuzz_fragments.h is the single table of those fragments (the driver matches only through it); this test
// produces every entry with a crafted input and checks the real parser's error contains / starts with it.
//   - Add a fragment to the table without a producer here: "no crafted producer" fails.
//   - Rename a parser message: that fragment's producer no longer matches and fails.
//   - Put a raw string literal back at a match site in fuzz_formats.cpp: the source scan fails (native only).
#include <fstream>
#include <functional>
#include <map>
#include <regex>
#include <sstream>

#include "doctest.h"
#include "fuzz_fragments.h"
#include "lomn/sage/blockfile.h"
#include "lomn/sage/image.h"
#include "lomn/sage/slb.h"
#include "lomn/sage/xfile.h"
#include "sage_test_util.h"

using namespace lomn::sage;
using namespace sage_test;
using fuzz_frag::Fragment;
using fuzz_frag::Match;

namespace {

// Minimal binary .x writer (the same token ids as test_sage_xfile.cpp's X; kept separate so this file stands alone).
struct XB {
  Bytes b;
  XB() { putstr(b, "xof 0303bin 0032"); }
  XB& obj(const std::string& type) {
    put16(b, 1);
    put32(b, static_cast<uint32_t>(type.size()));
    putstr(b, type);
    put16(b, 10);
    return *this;
  }
  XB& close() {
    put16(b, 11);
    return *this;
  }
  XB& ints(std::initializer_list<uint32_t> v) {
    put16(b, 6);
    put32(b, static_cast<uint32_t>(v.size()));
    for (uint32_t x : v) put32(b, x);
    return *this;
  }
  XB& floats(std::initializer_list<float> v) {
    put16(b, 7);
    put32(b, static_cast<uint32_t>(v.size()));
    for (float x : v) putf(b, x);
    return *this;
  }
  XB& str(const std::string& s) {
    put16(b, 2);
    put32(b, static_cast<uint32_t>(s.size()));
    putstr(b, s);
    put16(b, 20);
    return *this;
  }
};

// A 3-vertex, one-triangle Mesh body (open); the caller adds children and closes it.
XB& tri(XB& x) { return x.obj("Mesh").ints({3}).floats({0, 0, 0, 1, 0, 0, 1, 1, 0}).ints({1, 3, 0, 1, 2}); }

std::string x_err(const Bytes& b) {
  XFile xf;
  std::string err;
  CHECK_FALSE(parse_x(b.data(), b.size(), xf, &err));
  return err;
}

std::string blk_err(const Bytes& b) {
  BlockFile bf;
  std::string err;
  CHECK_FALSE(bf.open_bytes(b, &err));
  return err;
}

std::string tga_err(uint16_t w, uint16_t h, const Bytes& data) {
  Bytes t(18, 0);
  t[2] = 10;  // RLE true-colour
  t[12] = static_cast<uint8_t>(w);
  t[13] = static_cast<uint8_t>(w >> 8);
  t[14] = static_cast<uint8_t>(h);
  t[15] = static_cast<uint8_t>(h >> 8);
  t[16] = 24;
  t.insert(t.end(), data.begin(), data.end());
  Image img;
  TgaInfo ti;
  std::string err;
  CHECK_FALSE(decode_tga(t.data(), t.size(), img, &ti, &err));
  return err;
}

// textures.slb: one record whose slot is (or is not) a fix-up; `name_len` characters of name.
std::string slb_tex_err(bool record_slot_is_fixup, size_t name_len) {
  Bytes b;
  put32(b, 1);
  put32(b, 1);
  put32(b, 12);
  put32(b, 32);  // record 0: name pointer -> 32
  putf(b, 0.5f);
  putf(b, 0.25f);
  putf(b, 0.0f);
  put32(b, 3);
  put8(b, static_cast<uint8_t>(name_len));  // length byte (ignored), string read from +1
  putstr(b, std::string(name_len, 'n'));
  put8(b, 0);
  while (b.size() % 4) put8(b, 0);
  if (record_slot_is_fixup) put32(b, 12);
  put32(b, 8);
  put32(b, record_slot_is_fixup ? 2 : 1);  // slot count
  put32(b, 0x00C0FFEEu);
  Slb s;
  std::string err;
  REQUIRE_MESSAGE(s.parse(b, &err), err);
  std::vector<SpecTextureRecord> t;
  CHECK_FALSE(parse_spec_textures(s, t, &err));
  return err;
}

// Fragment key "fmt:text" (prefix entries: the prefix text) -> produces an error string.
using Producer = std::function<std::string()>;
std::map<std::string, Producer> producers() {
  namespace f = fuzz_frag;
  std::map<std::string, Producer> p;
  auto add = [&](const char* fmt, const char* text, Producer fn) { p[std::string(fmt) + ":" + text] = std::move(fn); };

  add("blk", f::kBlkNameRun, [] {  // 12 entries of all-'A' bytes: entry 0's unterminated name runs through 632 more
    Bytes b;
    putstr(b, "BLKF");
    put32(b, 1);
    put32(b, 12);
    b.insert(b.end(), 12 * 56, 'A');
    return blk_err(b);
  });
  add("lzss", f::kLzssRatio, [] {
    std::vector<uint8_t> out, in = {0};
    std::string err;
    CHECK_FALSE(decode_lzss(in.data(), in.size(), 0xffffffffu, out, &err));
    return err;
  });
  add("slb.tex", f::kSlbTexFixup, [] { return slb_tex_err(false, 3); });
  add("slb.tex", f::kSlbTexNameLong, [] { return slb_tex_err(true, kMaxSpecTextureName + 1); });
  add("tga", f::kTgaRleTruncated, [] { return tga_err(128, 1, {0x7f, 1, 2, 3}); });  // raw run of 128 px, 3 bytes of data
  add("tga", f::kTgaRleRunsPast, [] { return tga_err(2, 1, {0x7f, 1, 2, 3}); });     // run of 128 px in a 2-px image

  add("x", f::kXTooLarge, [] {
#ifdef __EMSCRIPTEN__
    return std::string("x: file too large");  // 64 MiB is not worth allocating under wasm32; the native run produces it for real
#else
    Bytes big(static_cast<size_t>(kMaxEntrySize) + 1, 0);
    return x_err(big);
#endif
  });
  add("x", f::kXNotX, [] { return x_err(Bytes(16, 'z')); });
  add("x", f::kXNotBinary, [] {
    Bytes b(16, ' ');
    std::copy_n("xof 0303txt 0032", 16, b.begin());
    return x_err(b);
  });
  add("x", f::kXFloatSize, [] {
    Bytes b(16, ' ');
    std::copy_n("xof 0303bin 0080", 16, b.begin());
    return x_err(b);
  });

  // decode_mesh prefixes, one crafted file each.
  add("x", "x: Mesh ", [] { XB x; x.obj("Mesh").close(); return x_err(x.b); });                                             // header truncated
  add("x", "x: MeshNormals ", [] { XB x; tri(x).obj("MeshNormals").ints({1}).floats({0, 0, 1}).close().close(); return x_err(x.b); });
  add("x", "x: MeshTextureCoords ", [] { XB x; tri(x).obj("MeshTextureCoords").ints({5}).floats({0, 0, 1, 0, 1, 1, 0, 1, 0, 0}).close().close(); return x_err(x.b); });
  add("x", "x: MeshVertexColors ", [] { XB x; tri(x).obj("MeshVertexColors").close().close(); return x_err(x.b); });         // header truncated
  add("x", "x: MeshMaterialList ", [] { XB x; tri(x).obj("MeshMaterialList").close().close(); return x_err(x.b); });         // header truncated
  add("x", "x: Material floats", [] {
    XB x;
    tri(x).obj("MeshMaterialList").ints({1, 1, 0}).obj("Material").floats({1, 1, 1}).close().close().close();
    return x_err(x.b);
  });
  add("x", "x: face ", [] { XB x; x.obj("Mesh").ints({3}).floats({0, 0, 0, 1, 0, 0, 1, 1, 0}).ints({1, 2, 0, 1, 0}).close(); return x_err(x.b); });  // 2-gon (+1 spare int so the face-count pre-check passes)
  add("x", "x: material ", [] {
    XB x;
    tri(x).obj("MeshMaterialList").ints({1, 1, 1}).obj("Material").floats({1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0}).close().close().close();
    return x_err(x.b);
  });
  add("x", "x: normal index", [] { XB x; tri(x).obj("MeshNormals").ints({1, 1, 3, 0, 0, 1}).floats({0, 0, 1}).close().close(); return x_err(x.b); });
  add("x", "x: texture name", [] {
    XB x;
    tri(x).obj("MeshMaterialList").ints({1, 1, 0}).obj("Material").floats({1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0});
    x.obj("TextureFilename").str(std::string(kXMaxTextureName + 1, 't')).close().close().close().close();
    return x_err(x.b);
  });
  add("x", "x: too many materials", [] { XB x; tri(x).obj("MeshMaterialList").ints({kXMaxMaterials + 1, 1, 0}).close().close(); return x_err(x.b); });
  add("x", "x: vertex colour", [] { XB x; tri(x).obj("MeshVertexColors").ints({1, 3}).floats({1, 1, 1, 1}).close().close(); return x_err(x.b); });
  return p;
}

}  // namespace

TEST_CASE("fuzz fragments: every error fragment the fuzz driver matches is produced by a crafted input (cleanup C3)") {
  const std::map<std::string, Producer> prod = producers();
  const std::vector<Fragment> frags = fuzz_frag::all_fragments();
  std::map<std::string, bool> used;
  for (const Fragment& fr : frags) {
    const std::string key = std::string(fr.fmt) + ":" + fr.text;
    INFO("fragment " << key);
    auto it = prod.find(key);
    REQUIRE_MESSAGE(it != prod.end(), "no crafted producer for fragment " << key);
    used[key] = true;
    const std::string err = it->second();
    CAPTURE(err);
    CHECK_MESSAGE(fuzz_frag::matches(err, fr.text, fr.how), "parser error " << err << " no longer matches the fuzz driver fragment '" << std::string(fr.text) << "'");
  }
  for (const auto& kv : prod) CHECK_MESSAGE(used.count(kv.first), "producer " << kv.first << " has no fragment in fuzz_fragments.h (stale)");
}

#ifdef LOMN_TESTS_DIR
TEST_CASE("fuzz fragments: every k* constant in fuzz_fragments.h is listed in all_fragments() (cleanup C3)") {
  std::ifstream in(std::string(LOMN_TESTS_DIR) + "/fuzz_fragments.h");
  REQUIRE(in.good());
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string text = ss.str();
  const std::regex decl("inline constexpr const char\\* (k[A-Za-z0-9]+)(\\[\\])? =");
  int n = 0;
  for (std::sregex_iterator it(text.begin(), text.end(), decl), end; it != end; ++it, ++n) {
    const std::string name = (*it)[1];
    size_t uses = 0;
    for (size_t at = text.find(name); at != std::string::npos; at = text.find(name, at + name.size())) ++uses;
    CHECK_MESSAGE(uses >= 2, name << " is declared in fuzz_fragments.h but never listed in all_fragments()");
  }
  CHECK(n >= 11);  // 10 string constants + the prefix array: the scan actually found the declarations
}

TEST_CASE("fuzz fragments: the driver has no raw string literal at an error-match site (cleanup C3)") {
  std::ifstream in(std::string(LOMN_TESTS_DIR) + "/fuzz_formats.cpp");
  REQUIRE(in.good());
  std::string line;
  int n = 0;
  while (std::getline(in, line)) {
    ++n;
    const bool match_site = line.find("has(err, \"") != std::string::npos || line.find("has(terr, \"") != std::string::npos ||
                            line.find("err.compare(0") != std::string::npos || line.find("err.find(\"") != std::string::npos;
    CHECK_MESSAGE(!match_site, "fuzz_formats.cpp:" << n << " matches an error string with a raw literal; add it to fuzz_fragments.h: " << line);
  }
}
#endif
