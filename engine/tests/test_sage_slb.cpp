#include "doctest.h"
#include "lomn/sage/slb.h"
#include "sage_test_util.h"

using namespace lomn::sage;
using namespace sage_test;

namespace {

uint32_t id4(const char* s) {  // "vllw" -> LE u32 whose file bytes read "wllv"
  return (static_cast<uint32_t>(static_cast<uint8_t>(s[0])) << 24) | (static_cast<uint32_t>(static_cast<uint8_t>(s[1])) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(s[2])) << 8) | static_cast<uint32_t>(static_cast<uint8_t>(s[3]));
}

// The shape of every Beta <area>_OBJ.slb: {slot, n, n, ptr=0x16} + 60-byte records at the unaligned 0x16,
// 2 bytes of padding, one fix-up slot (12), count, 0x00C0FFEE.
Bytes make_obj(const std::vector<std::pair<const char*, uint32_t>>& recs, uint32_t slot_value = 12) {
  Bytes b;
  put32(b, id4("????"));
  put32(b, static_cast<uint32_t>(recs.size()));
  put32(b, static_cast<uint32_t>(recs.size()));
  put32(b, 0x16);
  put16(b, 0);  // 0x10..0x15: the records start at 0x16
  put32(b, 0);
  for (size_t i = 0; i < recs.size(); ++i) {
    put32(b, id4(recs[i].first));
    putf(b, 1.0f + static_cast<float>(i));  // pos
    putf(b, 2.0f);
    putf(b, 3.0f);
    putf(b, 0.5f);       // orient
    putf(b, 0.25f);
    putf(b, 0.125f);
    put32(b, 0xdeadbeefu);  // +0x1c (absent from spec 2.1's field list)
    for (int k = 0; k < 6; ++k) putf(b, 10.0f + k);  // ext0, ext1
    put32(b, recs[i].second);
  }
  put16(b, 0);  // padding to 4-byte alignment for the trailer
  put32(b, slot_value);
  put32(b, 1);
  put32(b, kSlbMagic);
  return b;
}

Slb parse_ok(Bytes b) {
  Slb s;
  std::string err;
  REQUIRE_MESSAGE(s.parse(std::move(b), &err), err);
  return s;
}

}  // namespace

TEST_CASE("slb: OBJ view decodes field by field from an unaligned record array") {
  Slb s = parse_ok(make_obj({{"main", 0x10}, {"wllv", 0x110}, {"dor2", 0x131}}));
  CHECK(s.has_trailer());
  CHECK(s.pointer_fields() == std::vector<uint32_t>{12});
  uint32_t target = 0;
  REQUIRE(s.read_ptr(12, target));
  CHECK(target == 0x16);
  CHECK_FALSE(s.read_ptr(8, target));  // not a fix-up slot
  std::vector<ObjRecord> recs;
  std::string err;
  REQUIRE(parse_obj(s, recs, &err));
  REQUIRE(recs.size() == 3);
  CHECK(fourcc(recs[0].id) == "main");
  CHECK(fourcc(recs[1].id) == "wllv");
  CHECK(recs[1].pos[0] == 2.0f);
  CHECK(recs[2].pos[2] == 3.0f);
  CHECK(recs[2].orient[1] == 0.25f);
  CHECK(recs[2].unk1c == 0xdeadbeefu);
  CHECK(recs[2].ext0[0] == 10.0f);
  CHECK(recs[2].ext1[2] == 15.0f);
  CHECK(recs[2].flags == 0x131);
}

TEST_CASE("slb: the fix-up slot offset is masked with ~3 (GetSLKFile), the target is not") {
  Slb s = parse_ok(make_obj({{"main", 0x10}}, 15));  // 15 & ~3 == 12
  CHECK(s.pointer_fields() == std::vector<uint32_t>{12});
  std::vector<ObjRecord> recs;
  std::string err;
  REQUIRE(parse_obj(s, recs, &err));
  CHECK(recs.size() == 1);
}

TEST_CASE("slb: trailer found at (len-4)&~3 even with trailing bytes") {
  Bytes b = make_obj({{"main", 0x10}});
  b.push_back(0x77);  // len % 4 == 1: (len-4)&~3 still lands on the magic
  Slb s = parse_ok(b);
  CHECK(s.has_trailer());
  b.push_back(0x77);
  b.push_back(0x77);
  b.push_back(0x77);
  b.push_back(0x77);  // now the magic is not at (len-4)&~3: no fix-ups, like the original
  Slb t = parse_ok(b);
  CHECK_FALSE(t.has_trailer());
  std::vector<ObjRecord> recs;
  std::string err;
  CHECK_FALSE(parse_obj(t, recs, &err));  // the records pointer was never fixed up
}

TEST_CASE("slb: malformed fix-up tables are rejected") {
  std::string err;
  Bytes good = make_obj({{"main", 0x10}});
  size_t m = good.size() - 4;
  {
    Bytes b = good;
    set32(b, m - 4, 0x40000000u);  // count*4 overflows 32 bits
    Slb s;
    CHECK_FALSE(s.parse(b, &err));
  }
  {
    Bytes b = good;
    set32(b, m - 4, static_cast<uint32_t>(m / 4));  // count runs past the start of the buffer
    Slb s;
    CHECK_FALSE(s.parse(b, &err));
  }
  {
    Bytes b = good;
    set32(b, m - 8, static_cast<uint32_t>(m - 4));  // slot points into the trailer itself
    Slb s;
    CHECK_FALSE(s.parse(b, &err));
  }
  {
    Bytes b = good;
    set32(b, m - 8, 0xfffffffcu);  // slot far outside
    Slb s;
    CHECK_FALSE(s.parse(b, &err));
  }
  {
    Bytes b = good;
    set32(b, 12, static_cast<uint32_t>(b.size()));  // fixed-up target outside the buffer
    Slb s;
    CHECK_FALSE(s.parse(b, &err));
  }
  {
    Bytes b = good;  // the same field listed twice: the original would add the base twice
    b.resize(m - 4);
    put32(b, 12);
    put32(b, 2);
    put32(b, kSlbMagic);
    Slb s;
    CHECK_FALSE(s.parse(b, &err));
  }
  Slb tiny;
  CHECK(tiny.parse(Bytes{1, 2, 3}, &err));  // too short for a trailer: raw image, no fix-ups
  CHECK_FALSE(tiny.has_trailer());
  CHECK(tiny.parse(Bytes{}, &err));
}

TEST_CASE("slb: record arrays must fit before the fix-up table") {
  std::string err;
  std::vector<ObjRecord> recs;
  for (uint32_t n : {2u, 0x04444445u, 0xffffffffu}) {
    Bytes b = make_obj({{"main", 0x10}});
    set32(b, 8, n);  // LoadObjects iterates the count at +8
    Slb s = parse_ok(b);
    CHECK_FALSE(parse_obj(s, recs, &err));
  }
  // Truncations of a valid file never crash; each either parses to a smaller view or is rejected.
  Bytes good = make_obj({{"main", 0x10}, {"rock", 0x8}});
  for (size_t n = 0; n < good.size(); ++n) {
    Slb s;
    if (s.parse(Bytes(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(n)), &err)) (void)parse_obj(s, recs, &err);
  }
}

TEST_CASE("slb: POS and LIGHT views") {
  std::string err;
  Bytes p;
  put32(p, 0);
  put32(p, 2);
  put32(p, 12);  // ptr
  for (uint32_t i = 0; i < 2; ++i) {
    put32(p, id4(i ? "strt" : "pkup"));
    putf(p, 1.5f);
    putf(p, -2.5f);
    putf(p, 3.5f);
    put32(p, i ? 0x1000u : 0x60u);
  }
  put32(p, 8);
  put32(p, 1);
  put32(p, kSlbMagic);
  Slb ps = parse_ok(p);
  std::vector<PosRecord> pos;
  REQUIRE(parse_pos(ps, pos, &err));
  REQUIRE(pos.size() == 2);
  CHECK(fourcc(pos[1].id) == "strt");
  CHECK(pos[1].flags == 0x1000);
  CHECK(pos[0].pos[1] == -2.5f);

  Bytes l;
  put32(l, 0);
  put32(l, 1);
  put32(l, 12);
  put32(l, id4("sun0"));
  for (float f : {0.f, -1.f, 0.f, 500.f, 2.f, 0.9f, 0.8f, 0.7f, 1.f}) putf(l, f);
  put32(l, 0x80);
  put32(l, 8);
  put32(l, 1);
  put32(l, kSlbMagic);
  Slb ls = parse_ok(l);
  std::vector<LightRecord> lights;
  REQUIRE(parse_light(ls, lights, &err));
  REQUIRE(lights.size() == 1);
  CHECK(lights[0].pos[1] == -1.f);
  CHECK(lights[0].range == 500.f);
  CHECK(lights[0].param == 2.f);
  CHECK(lights[0].color[0] == doctest::Approx(0.9f));
  CHECK(lights[0].color[3] == 1.f);
  CHECK(lights[0].flags == 0x80);
}

TEST_CASE("slb: routing follows the decompiled LoadObjects test order") {
  CHECK(route(0x10) == Route::Main);
  CHECK(route(0x110) == Route::Trans);
  CHECK(route(0x8) == Route::Back);
  CHECK(route(0x31) == Route::Moveable);
  CHECK(route(0x32) == Route::Moveable);
  CHECK(route(0x432) == Route::Moveable);
  CHECK(route(0x131) == Route::Moveable);  // 0x20 is tested before 0x100
  CHECK(route(0x132) == Route::Moveable);
  CHECK(route(0x20) == Route::NoLightMoveable);
  CHECK(route(0x0) == Route::Ignored);
  CHECK(route(0x1000) == Route::Ignored);
  // Combinations absent from the Beta data, where spec §2.1's table (0x8 first) would say "back":
  CHECK(route(0x18) == Route::Main);
  CHECK(route(0x28) == Route::NoLightMoveable);
  CHECK(route(0x118) == Route::Trans);
}

TEST_CASE("slb: 4CC strings and cinematic table names") {
  CHECK(fourcc(id4("vllw")) == "vllw");
  CHECK(fourcc(0x61620000u) == "ab");
  CHECK(is_cinematic_obj("cin1_obj.slb"));
  CHECK(is_cinematic_obj("CIN6_OBJ.SLB"));
  CHECK_FALSE(is_cinematic_obj("vllg_OBJ.slb"));
  CHECK_FALSE(is_cinematic_obj("cin1_cam.slb"));
}

// M1a-3: <lev>/textures.slb (GcSpecTextureLoader::LoadTextures 0x0046f270): {n, n, ptr} + 20-byte records whose name
// pointer targets a length-prefixed string read from name + 1; then the fix-up slots, their count and 0x00C0FFEE.
namespace {
Bytes make_textures(const std::vector<std::pair<std::string, float>>& recs, size_t name_len_override = 0) {
  Bytes b;
  put32(b, static_cast<uint32_t>(recs.size()));
  put32(b, static_cast<uint32_t>(recs.size()));
  put32(b, 12);
  uint32_t str = static_cast<uint32_t>(12 + 20 * recs.size());
  std::vector<uint32_t> slots = {8};
  for (const auto& r : recs) {
    slots.push_back(static_cast<uint32_t>(b.size()));
    put32(b, str);
    putf(b, r.second);
    putf(b, 0.25f);
    putf(b, 0.0f);
    put32(b, 3);
    str += static_cast<uint32_t>(r.first.size()) + 2;
  }
  for (const auto& r : recs) {
    put8(b, static_cast<uint8_t>(name_len_override ? name_len_override : r.first.size()));  // ignored by the game
    putstr(b, r.first);
    put8(b, 0);
  }
  while (b.size() % 4) put8(b, 0);
  for (size_t i = slots.size(); i-- > 0;) put32(b, slots[i]);
  put32(b, static_cast<uint32_t>(slots.size()));
  put32(b, 0x00C0FFEEu);
  return b;
}
}  // namespace

TEST_CASE("slb: textures.slb records (name read at ptr + 1, trans, U/V speeds, frames); long names are rejected") {
  std::vector<SpecTextureRecord> t;
  std::string err;
  Slb s = parse_ok(make_textures({{"tfx", 0.5f}, {"a_longer_name.dds", 1.0f}}, 200));
  REQUIRE_MESSAGE(parse_spec_textures(s, t, &err), err);
  REQUIRE(t.size() == 2);
  CHECK(t[0].name == "tfx");  // the length byte (200 here) is ignored: the C string at ptr + 1
  CHECK(t[0].trans == 0.5f);
  CHECK(t[0].u == 0.25f);
  CHECK(t[0].v == 0.0f);
  CHECK(t[0].frames == 3);
  CHECK(t[1].name == "a_longer_name.dds");
  Slb big = parse_ok(make_textures({{std::string(kMaxSpecTextureName + 1, 'n'), 1.0f}}));
  CHECK_FALSE(parse_spec_textures(big, t, &err));
  CHECK(err.find("kMaxSpecTextureName") != std::string::npos);
  Slb ok = parse_ok(make_textures({{std::string(kMaxSpecTextureName, 'n'), 1.0f}}));
  CHECK(parse_spec_textures(ok, t, &err));
  // Every truncation of a valid file fails cleanly (or parses as a different valid table).
  const Bytes good = make_textures({{"tfx", 0.5f}, {"cut", 1.0f}});
  for (size_t len = 0; len < good.size(); ++len) {
    Slb p;
    if (p.parse(Bytes(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(len)), &err)) parse_spec_textures(p, t, &err);
  }
}

// Task 8B C1: caps from the measured real maxima (tools/research/spec_textures_maxima.py: 18 tables, at most 15 records,
// names at most 35 chars) and the exe's own name bound; a rejected table leaves no partial record list.
TEST_CASE("slb: textures.slb caps: records <= kMaxSpecTextures, names < 64 chars (exe ScFixedString<0x40>), no partial state") {
  std::vector<SpecTextureRecord> t;
  std::string err;
  // LoadTextures copies name + 1 into a ScFixedString of mMaxSize 0x40; ScFixedString::Init (0x00401850) throws
  // kOutOfRange when strlen >= mMaxSize. So 63 characters is the longest name the game accepts.
  Slb n63 = parse_ok(make_textures({{std::string(63, 'n'), 0.5f}}));
  CHECK(parse_spec_textures(n63, t, &err));
  Slb n64 = parse_ok(make_textures({{std::string(64, 'n'), 0.5f}}));
  CHECK_FALSE(parse_spec_textures(n64, t, &err));
  CHECK(err.find("kMaxSpecTextureName") != std::string::npos);
  CHECK(t.empty());
  std::vector<std::pair<std::string, float>> many(kMaxSpecTextures, {"tfx", 0.5f});
  Slb at_cap = parse_ok(make_textures(many));
  REQUIRE_MESSAGE(parse_spec_textures(at_cap, t, &err), err);
  CHECK(t.size() == kMaxSpecTextures);
  many.push_back({"tfx", 0.5f});
  Slb over = parse_ok(make_textures(many));
  CHECK_FALSE(parse_spec_textures(over, t, &err));
  CHECK(err.find("kMaxSpecTextures") != std::string::npos);
  CHECK(t.empty());
  // A bad record after good ones: nothing of the table is kept (it used to keep the records before the bad one).
  t.assign(1, SpecTextureRecord{"stale", 0.5f, 0, 0, 0});
  Slb bad = parse_ok(make_textures({{"good", 0.5f}, {std::string(300, 'x'), 0.5f}}));  // over both the old and new name bound
  CHECK_FALSE(parse_spec_textures(bad, t, &err));
  CHECK(t.empty());
}
