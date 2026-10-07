// M1a-4 Task 3: the texture decode cache across area loads (lomn/view/texture_cache.h) and its use by load_area.
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "doctest.h"
#include "lomn/fs.h"
#include "lomn/rhi.h"
#include "lomn/view/area.h"
#include "lomn/view/synthetic.h"
#include "lomn/view/texture_cache.h"
#include "sage_test_util.h"

using namespace lomn;
using namespace lomn::view;

namespace {

std::string hex_of(const std::string& s) {
  return sha256_hex(sha256(reinterpret_cast<const uint8_t*>(s.data()), s.size()));
}
Sha256Digest digest_of(const std::string& s) { return sha256(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }

// A decoded texture of `w` x `h` (single level) filled with `v`: bytes() == w*h*4.
std::shared_ptr<const DecodedTexture> decoded(uint32_t w, uint32_t h, uint8_t v) {
  auto d = std::make_shared<DecodedTexture>();
  d->format = "TGA32";
  MipLevel l;
  l.width = w;
  l.height = h;
  l.rgba.assign(size_t(w) * h * 4, v);
  d->mips.push_back(std::move(l));
  return d;
}

sage_test::BlkEntrySpec entry(const std::string& n, const sage_test::Bytes& b) { return {n, b, static_cast<uint32_t>(b.size()), 2}; }
// Uncompressed 32-bpp TGA (type 2), every pixel `bgra`, declaring `alpha_bits`.
sage_test::Bytes tga32(uint16_t w, uint16_t h, uint8_t alpha_bits, const uint8_t bgra[4]) {
  using namespace sage_test;
  Bytes b = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  put16(b, w);
  put16(b, h);
  put8(b, 32);
  put8(b, alpha_bits);
  for (uint32_t i = 0; i < uint32_t(w) * h; ++i) b.insert(b.end(), bgra, bgra + 4);
  return b;
}
// Uncompressed 24-bpp TGA (type 2), bottom-up (descriptor 0), every pixel `bgr`. The LAST file byte is the red channel
// of the last stored pixel, which a bottom-up image places at (x = w-1, y = 0).
sage_test::Bytes tga24(uint16_t w, uint16_t h, const uint8_t bgr[3]) {
  using namespace sage_test;
  Bytes b = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  put16(b, w);
  put16(b, h);
  put8(b, 24);
  put8(b, 0);
  for (uint32_t i = 0; i < uint32_t(w) * h; ++i) b.insert(b.end(), bgr, bgr + 3);
  return b;
}
// The synthetic area "test" in level "levg", whose level blockfile holds red.tga (2x2, opaque) and cut.tga (4x2,
// alpha): the model's two textured subsets resolve to them (after the .dds misses). `red` sets red.tga's pixel;
// mount_tga_level_bytes takes red.tga's whole file instead.
void mount_tga_level_bytes(fs::MemoryFileSystem& mfs, const sage_test::Bytes& red_tga) {
  fs::MemoryFileSystem base;
  REQUIRE(mount_synthetic_install(base));
  std::vector<uint8_t> area;
  REQUIRE(base.read("Root/Data/Levels/levt/test.blk", area) == fs::Error::None);
  const uint8_t cut[4] = {10, 20, 30, 0};
  REQUIRE(mfs.mount("Root/Data/Levels/levg/levg.blk", sage_test::make_blk({entry("cut.tga", tga32(4, 2, 8, cut)), entry("red.tga", red_tga)})));
  REQUIRE(mfs.mount("Root/Data/Levels/levg/test.blk", std::move(area)));
}
void mount_tga_level(fs::MemoryFileSystem& mfs, const uint8_t red[4]) { mount_tga_level_bytes(mfs, tga32(2, 2, 0, red)); }
AreaData load_tga_bytes(const sage_test::Bytes& red_tga, TextureCache* cache) {
  fs::MemoryFileSystem mfs;
  mount_tga_level_bytes(mfs, red_tga);
  AreaOptions o;
  o.texture_cache = cache;
  AreaData a;
  std::string err;
  REQUIRE_MESSAGE(load_area(mfs, "levg", "test", o, a, &err), err);
  return a;
}
AreaData load_tga(const uint8_t red[4], TextureCache* cache, uint64_t max_texture_bytes = kMaxAreaTextureBytes, bool* ok = nullptr,
                  std::string* err_out = nullptr) {
  fs::MemoryFileSystem mfs;
  mount_tga_level(mfs, red);
  AreaOptions o;
  o.texture_cache = cache;
  o.max_texture_bytes = max_texture_bytes;
  AreaData a;
  std::string err;
  const bool r = load_area(mfs, "levg", "test", o, a, &err);
  if (ok) *ok = r;
  else REQUIRE_MESSAGE(r, err);
  if (err_out) *err_out = err;
  return a;
}
void check_same_textures(const AreaData& a, const AreaData& b) {
  REQUIRE(a.textures.size() == b.textures.size());
  for (size_t i = 0; i < a.textures.size(); ++i) {
    CHECK(a.textures[i].request == b.textures[i].request);
    CHECK(a.textures[i].format == b.textures[i].format);
    CHECK(a.textures[i].alpha_on == b.textures[i].alpha_on);
    REQUIRE(a.textures[i].mips.size() == b.textures[i].mips.size());
    for (size_t m = 0; m < a.textures[i].mips.size(); ++m) {
      CHECK(a.textures[i].mips[m].width == b.textures[i].mips[m].width);
      CHECK(a.textures[i].mips[m].height == b.textures[i].mips[m].height);
      CHECK(a.textures[i].mips[m].rgba == b.textures[i].mips[m].rgba);
    }
  }
  CHECK(a.stats.textures == b.stats.textures);
  CHECK(a.stats.texture_bytes == b.stats.texture_bytes);
  CHECK(a.stats.textures_alpha == b.stats.textures_alpha);
  CHECK(a.stats.textures_failed == b.stats.textures_failed);
  CHECK(a.stats.textures_missing == b.stats.textures_missing);
  CHECK(a.warnings == b.warnings);
}
// Null-backend call log + D3D8 state log of two frames of the synthetic A9 state scene.
std::pair<std::vector<std::string>, std::vector<std::string>> synthetic_logs(TextureCache* cache) {
  fs::MemoryFileSystem mfs;
  REQUIRE(mount_synthetic_install(mfs));
  AreaOptions o;
  o.texture_cache = cache;
  AreaData a;
  std::string err;
  REQUIRE_MESSAGE(load_area(mfs, kSyntheticLevel, kSyntheticStateArea, o, a, &err), err);
  std::vector<std::string> calls, states;
  auto dev = rhi::create_null_device(&calls);
  const rhi::TextureId rt = dev->create_render_target_ex(rhi::RenderTargetDesc{64, 64, true, 1.0f});
  AreaRenderer r;
  RenderOptions ro;
  ro.state_log = &states;
  REQUIRE(r.upload(*dev, a, ro, &err));
  const Camera cam = make_camera(a, synthetic_camera(), 1.0f);
  REQUIRE(r.render(*dev, rt, a, cam));
  REQUIRE(r.render(*dev, rt, a, cam));
  return {calls, states};
}

}  // namespace

TEST_CASE("texture cache: SHA-256 matches FIPS 180-4 / shasum vectors, across the 55/56/64-byte padding boundaries") {
  CHECK(hex_of("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(hex_of("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(hex_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  CHECK(hex_of(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  // "0123456789012..." of length n (shasum -a 256, 2026-10-07).
  const std::pair<size_t, const char*> v[] = {
      {55, "f34d5a0f80c0cbf84c8c0b90218c22637abd199965249da736a20143c8c9c9d9"},
      {56, "83aa034bda83e458a0dc9cbce0d4e354716aa0ff770ed37ac0ed2b292052e4af"},
      {63, "074f6e9ac301d5d1b6df6f1dfb8c6f89c187ea945d352ce6a29279a9c630680b"},
      {64, "9674d9e078535b7cec43284387a6ee39956188e735a85452b0050b55341cda56"},
      {65, "52774b57c10e45040a61c14d35c1c8ebefe880082313aa0a21ebb077734cd067"},
      {119, "d0cb70d05ff14123f114c0cca360c62077379cf1ac90e1bfafa9e9e4d827596a"},
      {120, "08642f0525963875af954100280fe3009293fa7e19c273444f31464c9b089243"}};
  for (const auto& nv : v) {
    std::string s;
    for (size_t i = 0; i < nv.first; ++i) s.push_back(static_cast<char>('0' + i % 10));
    CHECK_MESSAGE(hex_of(s) == nv.second, "length ", nv.first);
  }
}

TEST_CASE("texture cache: the same path with different bytes misses; the same bytes under another path misses") {
  TextureCache c;
  const std::string p = "root/data/levels/lev1/textures/a.dds";
  c.insert(p, digest_of("bytes one"), decoded(2, 2, 1));
  CHECK(c.find(p, digest_of("bytes two")) == nullptr);                          // same path, different bytes
  CHECK(c.find("root/data/levels/lev2/textures/a.dds", digest_of("bytes one")) == nullptr);  // same bytes, other path
  const auto hit = c.find(p, digest_of("bytes one"));
  REQUIRE(hit != nullptr);
  CHECK(hit->mips[0].rgba[0] == 1);
  CHECK(c.stats().hits == 1);
  CHECK(c.stats().misses == 2);
  CHECK(c.stats().entries == 1);
  CHECK(c.stats().bytes == 16);
  // Both keys can live side by side.
  c.insert(p, digest_of("bytes two"), decoded(2, 2, 2));
  CHECK(c.stats().entries == 2);
  CHECK(c.find(p, digest_of("bytes two"))->mips[0].rgba[0] == 2);
  CHECK(c.find(p, digest_of("bytes one"))->mips[0].rgba[0] == 1);
}

TEST_CASE("texture cache: LRU eviction under a small injected cap (a hit refreshes recency); an over-cap entry is not stored") {
  TextureCache c(48);  // three 16-byte entries
  CHECK(c.cap_bytes() == 48);
  c.insert("a", digest_of("a"), decoded(2, 2, 'a'));
  c.insert("b", digest_of("b"), decoded(2, 2, 'b'));
  c.insert("c", digest_of("c"), decoded(2, 2, 'c'));
  CHECK(c.stats().bytes == 48);
  CHECK(c.stats().evictions == 0);
  REQUIRE(c.find("a", digest_of("a")) != nullptr);  // a becomes most recent: LRU order now b, c, a
  c.insert("d", digest_of("d"), decoded(2, 2, 'd'));  // evicts b
  CHECK(c.stats().evictions == 1);
  CHECK(c.stats().bytes == 48);
  CHECK(c.stats().entries == 3);
  CHECK(c.find("b", digest_of("b")) == nullptr);
  CHECK(c.find("a", digest_of("a")) != nullptr);
  CHECK(c.find("c", digest_of("c")) != nullptr);
  CHECK(c.find("d", digest_of("d")) != nullptr);
  // A 32-byte entry evicts the two least recent (a, c), never itself.
  c.insert("e", digest_of("e"), decoded(4, 2, 'e'));
  CHECK(c.stats().evictions == 3);
  CHECK(c.stats().entries == 2);
  CHECK(c.stats().bytes == 48);
  CHECK(c.find("a", digest_of("a")) == nullptr);
  CHECK(c.find("c", digest_of("c")) == nullptr);
  CHECK(c.find("d", digest_of("d")) != nullptr);
  CHECK(c.find("e", digest_of("e")) != nullptr);
  // Larger than the whole cap: not stored, nothing evicted.
  const uint64_t ev = c.stats().evictions;
  c.insert("big", digest_of("big"), decoded(4, 4, 'x'));  // 64 > 48
  CHECK(c.find("big", digest_of("big")) == nullptr);
  CHECK(c.stats().evictions == ev);
  CHECK(c.stats().entries == 2);
  // Re-inserting an existing key replaces it without double-counting bytes.
  c.insert("d", digest_of("d"), decoded(2, 2, 'D'));
  CHECK(c.stats().bytes == 48);
  CHECK(c.find("d", digest_of("d"))->mips[0].rgba[0] == 'D');
  // A value without mips (or null) is ignored: a hit always has mips.front().
  c.insert("empty", digest_of("empty"), std::make_shared<DecodedTexture>());
  c.insert("null", digest_of("null"), nullptr);
  CHECK(c.find("empty", digest_of("empty")) == nullptr);
  CHECK(c.find("null", digest_of("null")) == nullptr);
  CHECK(c.stats().entries == 2);
  // Cap 0 stores nothing.
  TextureCache z(0);
  z.insert("a", digest_of("a"), decoded(1, 1, 1));
  CHECK(z.stats().entries == 0);
  CHECK(z.stats().bytes == 0);
}

TEST_CASE("texture cache: an evicted entry stays valid for a consumer that still holds it") {
  TextureCache c(16);
  c.insert("a", digest_of("a"), decoded(2, 2, 0x5a));
  std::shared_ptr<const DecodedTexture> held = c.find("a", digest_of("a"));
  REQUIRE(held != nullptr);
  const uint8_t* data = held->mips[0].rgba.data();
  c.insert("b", digest_of("b"), decoded(2, 2, 0x11));  // evicts a: the cache drops its reference only
  CHECK(c.stats().evictions == 1);
  CHECK(c.find("a", digest_of("a")) == nullptr);
  CHECK(held.use_count() == 1);  // the consumer is now the sole owner
  REQUIRE(held->mips.size() == 1);
  CHECK(held->mips[0].rgba.data() == data);
  CHECK(held->mips[0].rgba == std::vector<uint8_t>(16, 0x5a));
  // Destroying the cache does not invalidate it either.
  {
    auto c2 = std::make_unique<TextureCache>(1024);
    c2->insert("x", digest_of("x"), decoded(1, 1, 7));
    held = c2->find("x", digest_of("x"));
  }
  REQUIRE(held != nullptr);
  CHECK(held->mips[0].rgba == std::vector<uint8_t>(4, 7));
}

TEST_CASE("texture cache: flag off (default AreaOptions) never constructs or touches a cache") {
  CHECK(AreaOptions().texture_cache == nullptr);
  const uint64_t before = TextureCache::instances_constructed();
  const uint8_t red[4] = {0, 0, 255, 255};
  AreaData off = load_tga(red, nullptr);
  CHECK(TextureCache::instances_constructed() == before);  // the loader built none
  CHECK(off.stats.texture_cache_hits == 0);
  CHECK(off.stats.texture_cache_misses == 0);
  REQUIRE(off.textures.size() == 2);
  // The hook is live: a cache the caller passes is constructed (counted) and used.
  TextureCache c;
  CHECK(TextureCache::instances_constructed() == before + 1);
  AreaData on = load_tga(red, &c);
  CHECK(on.stats.texture_cache_misses == 2);
  CHECK(c.stats().misses == 2);
  CHECK(c.stats().entries == 2);
  CHECK(TextureCache::instances_constructed() == before + 1);
}

TEST_CASE("texture cache: load_area with the cache gives the same textures as without, on a miss and on a hit") {
  const uint8_t red[4] = {0, 0, 255, 255};
  AreaData off = load_tga(red, nullptr);
  TextureCache c;
  AreaData first = load_tga(red, &c);   // fresh file system, empty cache: both decoded and stored
  AreaData second = load_tga(red, &c);  // fresh file system, same bytes: both served from the cache
  CHECK(first.stats.texture_cache_misses == 2);
  CHECK(first.stats.texture_cache_hits == 0);
  CHECK(second.stats.texture_cache_hits == 2);
  CHECK(second.stats.texture_cache_misses == 0);
  CHECK(c.stats().hits == 2);
  CHECK(c.stats().misses == 2);
  CHECK(c.stats().bytes == off.stats.texture_bytes);
  check_same_textures(off, first);
  check_same_textures(off, second);
  // The area's own copy is independent of the cache's: releasing it leaves the cached entry intact.
  release_cpu_copies(second);
  AreaData third = load_tga(red, &c);
  CHECK(third.stats.texture_cache_hits == 2);
  check_same_textures(off, third);
}

TEST_CASE("texture cache: load_area remount with the same path but different bytes misses and decodes the new bytes") {
  const uint8_t red[4] = {0, 0, 255, 255}, green[4] = {0, 255, 0, 255};
  TextureCache c;
  AreaData a = load_tga(red, &c);
  AreaData b = load_tga(green, &c);  // red.tga changed, cut.tga did not
  CHECK(b.stats.texture_cache_hits == 1);
  CHECK(b.stats.texture_cache_misses == 1);
  REQUIRE(b.textures.size() == 2);
  REQUIRE(b.textures[0].request == "Root/Data/Levels/levg/textures/red.tga");
  CHECK(b.textures[0].mips[0].rgba[1] == 255);  // green (RGBA), not the cached red
  CHECK(b.textures[0].mips[0].rgba[0] == 0);
  check_same_textures(load_tga(green, nullptr), b);
  CHECK(a.textures[0].mips[0].rgba[0] == 255);
}

TEST_CASE("texture cache: same path, same size (>= 4 KB), only the LAST byte differs -> miss, and the new last texel is decoded (fix1 I-1)") {
  // 64x64 24-bpp: 18 + 12,288 bytes. Both files have the same size and differ only in their final byte (the red channel
  // of texel (63, 0)), so a digest over a prefix, or a (path, size) shortcut, would serve the stale texel.
  const uint8_t bgr[3] = {10, 20, 200};
  const sage_test::Bytes v1 = tga24(64, 64, bgr);
  sage_test::Bytes v2 = v1;
  v2.back() = 17;
  REQUIRE(v1.size() == 18 + 64 * 64 * 3);
  REQUIRE(v1.size() >= 4096);
  REQUIRE(v1.size() == v2.size());
  REQUIRE(std::equal(v1.begin(), v1.end() - 1, v2.begin()));
  const size_t last_texel = (0 * 64 + 63) * 4;  // (x 63, y 0), RGBA
  TextureCache c;
  AreaData a = load_tga_bytes(v1, &c);
  CHECK(a.stats.texture_cache_misses == 2);
  REQUIRE(a.textures.size() == 2);
  REQUIRE(a.textures[0].request == "Root/Data/Levels/levg/textures/red.tga");
  CHECK(a.textures[0].format == "TGA24");
  CHECK(a.textures[0].mips[0].rgba[last_texel] == 200);
  AreaData b = load_tga_bytes(v2, &c);
  CHECK(b.stats.texture_cache_misses == 1);  // red.tga: new content
  CHECK(b.stats.texture_cache_hits == 1);    // cut.tga: unchanged
  REQUIRE(b.textures.size() == 2);
  CHECK(b.textures[0].mips[0].rgba[last_texel] == 17);  // the new last texel, not the cached 200
  CHECK(b.textures[0].mips[0].rgba[last_texel - 4] == 200);
  check_same_textures(load_tga_bytes(v2, nullptr), b);
  // Both versions now live side by side under the one path: each reload hits its own entry.
  AreaData a2 = load_tga_bytes(v1, &c);
  CHECK(a2.stats.texture_cache_hits == 2);
  CHECK(a2.textures[0].mips[0].rgba[last_texel] == 200);
  AreaData b2 = load_tga_bytes(v2, &c);
  CHECK(b2.stats.texture_cache_hits == 2);
  CHECK(b2.textures[0].mips[0].rgba[last_texel] == 17);
}

TEST_CASE("texture cache: a tiny cache evicts between loads and the pixels stay right; area caps still apply on a hit") {
  const uint8_t red[4] = {0, 0, 255, 255};
  AreaData off = load_tga(red, nullptr);
  REQUIRE(off.stats.texture_bytes == 20 + 44);  // red 2x2 (16+4), cut 4x2 (32+8+4)
  TextureCache c(50);                            // holds one of the two
  for (int i = 0; i < 3; ++i) {
    AreaData a = load_tga(red, &c);
    check_same_textures(off, a);
    CHECK(c.stats().bytes <= 50);
  }
  CHECK(c.stats().evictions >= 2);
  // A warm cache does not bypass the per-area byte cap: the load fails exactly as without the cache.
  TextureCache warm;
  load_tga(red, &warm);
  bool ok_off = true, ok_on = true;
  std::string err_off, err_on;
  AreaData f_off = load_tga(red, nullptr, 40, &ok_off, &err_off);
  AreaData f_on = load_tga(red, &warm, 40, &ok_on, &err_on);
  CHECK_FALSE(ok_off);
  CHECK_FALSE(ok_on);
  CHECK(err_on == err_off);
  CHECK(err_on.find("kMaxAreaTextureBytes") != std::string::npos);
  CHECK(f_on.textures.empty());
  CHECK(warm.stats().hits >= 1);  // the over-cap load did take the hit path
}

TEST_CASE("texture cache: the synthetic A9 scene's call log and state log are identical with the cache on, cold and warm") {
  const auto off = synthetic_logs(nullptr);
  TextureCache c;
  const auto cold = synthetic_logs(&c);
  const auto warm = synthetic_logs(&c);
  CHECK(c.stats().hits > 0);
  CHECK(off.first == cold.first);
  CHECK(off.second == cold.second);
  CHECK(off.first == warm.first);
  CHECK(off.second == warm.second);
}
