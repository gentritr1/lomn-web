// M1a-2 area renderer: D3D-convention matrices, the synthetic A7 scene through load_area / AreaRenderer on the null
// backend, failure paths and caps.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "doctest.h"
#include "lomn/fs.h"
#include "lomn/rhi.h"
#include "lomn/sage/blockfile.h"
#include "lomn/view/area.h"
#include "lomn/view/synthetic.h"
#include "sage_test_util.h"

using namespace lomn;
using namespace lomn::view;

namespace {

void xf(const Mat4& m, float x, float y, float z, float out[4]) {
  const float p[3] = {x, y, z};
  transform_point(m, p, out);
}

AreaData load_synthetic(std::string* err = nullptr) {
  fs::MemoryFileSystem mfs;
  REQUIRE(mount_synthetic_install(mfs));
  AreaData a;
  std::string e;
  REQUIRE_MESSAGE(load_area(mfs, kSyntheticLevel, kSyntheticArea, AreaOptions(), a, &e), e);
  if (err) *err = e;
  return a;
}

bool has_warning(const AreaData& a, const std::string& needle) {
  return std::any_of(a.warnings.begin(), a.warnings.end(), [&](const std::string& w) { return w.find(needle) != std::string::npos; });
}

}  // namespace

TEST_CASE("view math: RotX(-1.5707982) maps model (x, y, z) to world (x, z, -y); translation is _41.._43") {
  const Mat4 w = mat_mul(mat_rotation_x(kWorldRotX), mat_translation(10, 20, 30));
  float o[4];
  xf(w, 1, 2, 3, o);
  CHECK(o[0] == doctest::Approx(11).epsilon(1e-6));
  CHECK(o[1] == doctest::Approx(23).epsilon(1e-5));  // z -> y
  CHECK(o[2] == doctest::Approx(28).epsilon(1e-5));  // -y -> z
  CHECK(o[3] == 1.0f);
  CHECK(w.m[12] == 10.0f);
  CHECK(w.m[13] == 20.0f);
  CHECK(w.m[14] == 30.0f);
  // The game's constant is not exactly -pi/2: cos is ~ -1.7e-6, not 0.
  const Mat4 r = mat_rotation_x(kWorldRotX);
  CHECK(r.m[6] == doctest::Approx(-1.0f));
  CHECK(std::fabs(r.m[5]) < 1e-5f);
  CHECK(r.m[5] != 0.0f);
}

TEST_CASE("view math: PerspectiveFovRH maps view z = -near to depth 0 and z = -far to depth 1 (WebGPU range)") {
  const Mat4 p = perspective_fov_rh(1.5707964f, 2.0f, 0.5f, 100.0f);
  CHECK(p.m[0] == doctest::Approx(0.5f));  // cot(45 deg) / aspect
  CHECK(p.m[5] == doctest::Approx(1.0f));
  CHECK(p.m[11] == -1.0f);
  CHECK(p.m[15] == 0.0f);
  float o[4];
  xf(p, 0, 0, -0.5f, o);
  CHECK(o[2] / o[3] == doctest::Approx(0.0).epsilon(1e-6));
  xf(p, 0, 0, -100.0f, o);
  CHECK(o[2] / o[3] == doctest::Approx(1.0).epsilon(1e-6));
  xf(p, 0, 0, -10.0f, o);
  CHECK(o[2] / o[3] > 0.0f);
  CHECK(o[2] / o[3] < 1.0f);
  xf(p, 10, 0, -10.0f, o);  // 45 deg right at aspect 2 -> x_ndc 0.5
  CHECK(o[0] / o[3] == doctest::Approx(0.5f));
}

TEST_CASE("view math: LookAtRH puts the target on the -Z axis of view space") {
  const float eye[3] = {1, 2, 3}, at[3] = {1, 2, -7}, up[3] = {0, 1, 0};
  const Mat4 v = look_at_rh(eye, at, up);
  float o[4];
  xf(v, 1, 2, -7, o);
  CHECK(o[0] == doctest::Approx(0).epsilon(1e-6));
  CHECK(o[1] == doctest::Approx(0).epsilon(1e-6));
  CHECK(o[2] == doctest::Approx(-10));
  xf(v, 2, 2, 3, o);  // +X world is +X view (right) when looking along -Z
  CHECK(o[0] == doctest::Approx(1));
}

TEST_CASE("area: the synthetic scene loads with routing, skips and texture resolution as the game would") {
  AreaData a = load_synthetic();
  const AreaStats& s = a.stats;
  CHECK(a.level == "levt");
  CHECK(a.area == "test");
  CHECK_FALSE(a.art_blk);
  CHECK(has_warning(a, "art.blk not opened"));
  CHECK(s.obj_records == 8);
  CHECK(s.placements == 6);
  CHECK(s.ignored_records == 1);
  CHECK(s.missing_models == 1);
  CHECK(s.routes[static_cast<int>(sage::Route::Main)] == 2);
  CHECK(s.routes[static_cast<int>(sage::Route::Back)] == 1);
  CHECK(s.routes[static_cast<int>(sage::Route::Trans)] == 2);
  CHECK(s.routes[static_cast<int>(sage::Route::NoLightMoveable)] == 1);
  CHECK(s.routes[static_cast<int>(sage::Route::Moveable)] == 0);
  CHECK(has_warning(a, "model miss"));
  CHECK_FALSE(has_warning(a, "nope"));  // A6: an ignored record's model is never loaded
  REQUIRE(a.models.size() == 2);        // quad, blue: loaded once each (cached by id)
  CHECK(a.models[0].id == "quad");
  CHECK(a.models[0].has_colors);
  CHECK(a.models[0].vertex_count == 12);
  CHECK_FALSE(a.models[1].has_colors);
  REQUIRE(a.models[0].subsets.size() == 3);
  CHECK_FALSE(a.models[0].subsets[0].alpha);
  CHECK(a.models[0].subsets[1].alpha);
  CHECK(a.models[0].subsets[2].texture == kNoTexture);
  CHECK(a.models[1].subsets[0].texture == kNoTexture);
  CHECK(a.models[1].subsets[0].diffuse[2] == 1.0f);
  CHECK(s.subsets == 4);
  CHECK(s.draws == 3 + 1 + 1 + 3 + 1 + 1);
  CHECK(s.alpha_subsets == 1);
  CHECK(s.untextured_subsets == 2);
  CHECK(s.models_without_colors == 1);
  REQUIRE(a.textures.size() == 2);
  CHECK(s.textures_missing == 1);
  CHECK(has_warning(a, "texture nothere: not found"));
  CHECK(a.textures[0].request == "Root/Data/Levels/levt/textures/red.dds");
  CHECK(a.textures[0].format == "DXT1");
  CHECK_FALSE(a.textures[0].alpha_on);
  CHECK(a.textures[0].mips.size() == 3);
  CHECK(a.textures[1].format == "A8R8G8B8");
  CHECK(a.textures[1].alpha_on);
  CHECK(a.textures[1].mips.size() == 4);
  CHECK(s.texture_bytes == (16 + 4 + 1) * 4 + (64 + 16 + 4 + 1) * 4);
  CHECK(a.pos_records == 2);
  REQUIRE(a.has_start);
  CHECK(a.start_record == 1);
  CHECK(a.start[2] == 10.0f);
  // Placement world matrices: RotX then the record position.
  CHECK(a.placements[1].pos[2] == -2.0f);
  CHECK(a.placements[1].world.m[14] == -2.0f);
  REQUIRE(a.has_bounds);  // every route except back: z from -80 (nolight) to 0
  CHECK(a.bounds_min[2] == doctest::Approx(-80.0f));
  CHECK(a.bounds_max[1] == doctest::Approx(8.0f));
}

TEST_CASE("area: route switches leave the disabled records out") {
  fs::MemoryFileSystem mfs;
  REQUIRE(mount_synthetic_install(mfs));
  AreaOptions o;
  o.draw_trans = o.draw_back = o.draw_nolight = false;
  AreaData a;
  std::string e;
  REQUIRE(load_area(mfs, "levt", "test", o, a, &e));
  CHECK(a.stats.placements == 2);
  CHECK(a.stats.disabled_records == 4);
  CHECK(a.stats.draws == 4);
}

TEST_CASE("area: camera fallback from the POS start, yaw towards the bounds centre by default") {
  AreaData a = load_synthetic();
  Camera c = make_camera(a, synthetic_camera(), 1.0f);
  CHECK(c.eye_source == "pos_start");
  CHECK(c.yaw_source == "cli");
  CHECK(c.eye[2] == 10.0f);
  CHECK(c.at[2] == doctest::Approx(9.0f));
  CameraSettings s;  // defaults: eye height 3, pitch -10, yaw towards the bounds centre
  c = make_camera(a, s, 16.0f / 9.0f);
  CHECK(c.eye[1] == 3.0f);
  CHECK(c.yaw_source == "towards_bounds_centre");
  // Bounds centre: x (-8 + 60) / 2 = 26 (the off-screen quad reaches x 60), z (-80 + 0) / 2 = -40.
  const double cx = (a.bounds_min[0] + a.bounds_max[0]) / 2, cz = (a.bounds_min[2] + a.bounds_max[2]) / 2;
  CHECK(cx == doctest::Approx(26.0));
  CHECK(c.yaw_deg == doctest::Approx(std::atan2(cx, -(cz - 10.0)) * 180.0 / 3.14159265358979).epsilon(1e-4));
  CHECK(c.pitch_deg == -10.0f);
  s.eye_set = true;
  s.eye[0] = 5;
  c = make_camera(a, s, 1.0f);
  CHECK(c.eye_source == "cli");
  CHECK(c.eye[0] == 5.0f);
  CHECK(c.eye[1] == 0.0f);  // --eye is used as given
}

TEST_CASE("area: null-backend frame follows the SAGE node order, DrawX's two passes and the trans SortIt order") {
  AreaData a = load_synthetic();
  std::vector<std::string> log;
  auto dev = rhi::create_null_device(&log);
  rhi::RenderTargetDesc rd{64, 64, true, 1.0f};
  rhi::TextureId rt = dev->create_render_target_ex(rd);
  AreaRenderer r;
  std::string err;
  REQUIRE_MESSAGE(r.upload(*dev, a, RenderOptions(), &err), err);
  CHECK(r.pipelines() == 0);         // M1a-3: pipelines are created per d3d::PipelineKey on first use
  CHECK(r.textures_created() == 3);  // white + 2
  log.clear();
  r.render(*dev, rt, a, make_camera(a, synthetic_camera(), 1.0f));
  CHECK(dev->last_frame_stats().draws == 10);
  // Keys: area/nolight nodes cull CW -> 1 opaque, 2 alpha (cut, DrawX pass 2); back/trans nodes cull NONE -> 3 opaque,
  // 4 alpha.
  CHECK(r.pipelines() == 4);
  std::vector<std::string> draws;
  for (const auto& l : log)
    if (l.rfind("draw_indexed", 0) == 0) draws.push_back(l);
  REQUIRE(draws.size() == 10);
  for (const auto& d : draws) CHECK(d.find("rejected") == std::string::npos);
  // Buffer ids: uniforms 1, quad vb/ib 2/3, blue vb/ib 4/5. MainArea: quad (red opaque, missing texture -> NULL ->
  // white, then cut in pass 2), blue; NoLight: blue; Back: blue; Trans (SortIt, farthest greatest-Y point first): quad
  // (|(50, -56) - (0, 10)| = 82.8) before blue (|(-8, -62) - (0, 10)| = 72.4).
  CHECK(draws[0].rfind("draw_indexed pipeline=1 vb=2 ib=3 first=0 count=6 ", 0) == 0);
  CHECK(draws[1].rfind("draw_indexed pipeline=1 vb=2 ib=3 first=12 count=6 ", 0) == 0);
  CHECK(draws[2].rfind("draw_indexed pipeline=2 vb=2 ib=3 first=6 count=6 ", 0) == 0);
  CHECK(draws[3].rfind("draw_indexed pipeline=1 vb=4 ", 0) == 0);  // main blue
  CHECK(draws[4].rfind("draw_indexed pipeline=1 vb=4 ", 0) == 0);  // nolight blue (cull CW)
  CHECK(draws[5].rfind("draw_indexed pipeline=3 vb=4 ", 0) == 0);  // back blue (cull NONE)
  CHECK(draws[6].rfind("draw_indexed pipeline=3 vb=2 ib=3 first=0 ", 0) == 0);  // trans quad
  CHECK(draws[8].rfind("draw_indexed pipeline=4 vb=2 ib=3 first=6 ", 0) == 0);  // its alpha subset in pass 2
  CHECK(draws[9].rfind("draw_indexed pipeline=3 vb=4 ", 0) == 0);  // trans blue last
  std::string wb;
  for (const auto& l : log)
    if (l.rfind("write_buffer 1 +0 ", 0) == 0) wb = l;
  CHECK(wb.rfind("write_buffer 1 +0 10240 bytes sum=", 0) == 0);  // one 1024-byte slot per draw, one upload per frame
  CHECK(log.back() == "end_frame draws=10");
}

TEST_CASE("area: a missing area leaves no partial state; a missing OBJ table places nothing") {
  fs::MemoryFileSystem mfs;
  REQUIRE(mount_synthetic_install(mfs));
  AreaData a = load_synthetic();
  std::string err;
  CHECK_FALSE(load_area(mfs, "levt", "none", AreaOptions(), a, &err));
  CHECK(err.find("cannot open") != std::string::npos);
  CHECK(a.models.empty());
  CHECK(a.placements.empty());
  // An area blockfile without tables: entered, nothing placed, warnings only (the game skips missing loaders).
  REQUIRE(mfs.mount("Root/Data/Levels/levt/empt.blk", sage_test::make_blk({sage_test::stored("readme.txt", "x")})));
  REQUIRE(load_area(mfs, "levt", "empt", AreaOptions(), a, &err));
  CHECK(a.placements.empty());
  CHECK_FALSE(a.has_start);
  CHECK(has_warning(a, "no obj table"));
  CHECK(has_warning(a, "no pos table"));
}

namespace {
sage_test::Bytes blue_x_bytes() {
  fs::MemoryFileSystem mfs;
  REQUIRE(mount_synthetic_install(mfs));
  std::vector<uint8_t> blk;
  REQUIRE(mfs.read("Root/Data/Levels/levt/test.blk", blk) == fs::Error::None);
  sage::BlockFile bf;
  std::string err;
  REQUIRE(bf.open_bytes(blk, &err));
  std::vector<uint8_t> x;
  REQUIRE(bf.read(static_cast<size_t>(bf.find("blue.x")), x, &err));
  return x;
}
sage_test::Bytes obj_records(uint32_t n, uint32_t flags) {
  using namespace sage_test;
  Bytes b;
  put32(b, 0);
  put32(b, n);
  put32(b, n);
  put32(b, 16);
  for (uint32_t i = 0; i < n; ++i) {
    putstr(b, "eulb");  // LE u32 of "blue"
    for (int k = 0; k < 13; ++k) putf(b, 0.0f);
    put32(b, flags);
  }
  put32(b, 12);
  put32(b, 1);
  put32(b, 0x00C0FFEEu);
  return b;
}
}  // namespace

TEST_CASE("area: placement cap (untrusted OBJ counts) fails the load cleanly") {
  fs::MemoryFileSystem mfs;
  REQUIRE(mount_synthetic_install(mfs));
  sage_test::Bytes x = blue_x_bytes();
  sage_test::Bytes ok = obj_records(kMaxPlacements, 0x10), over = obj_records(kMaxPlacements + 1, 0x10);
  REQUIRE(mfs.mount("Root/Data/Levels/levt/capa.blk",
                    sage_test::make_blk({{"blue.x", x, static_cast<uint32_t>(x.size()), 2},
                                         {"capa_obj.slb", ok, static_cast<uint32_t>(ok.size()), 2}})));
  REQUIRE(mfs.mount("Root/Data/Levels/levt/capb.blk",
                    sage_test::make_blk({{"blue.x", x, static_cast<uint32_t>(x.size()), 2},
                                         {"capb_obj.slb", over, static_cast<uint32_t>(over.size()), 2}})));
  AreaData a;
  std::string err;
  REQUIRE_MESSAGE(load_area(mfs, "levt", "capa", AreaOptions(), a, &err), err);
  CHECK(a.stats.placements == kMaxPlacements);
  CHECK(a.models.size() == 1);
  CHECK_FALSE(load_area(mfs, "levt", "capb", AreaOptions(), a, &err));
  CHECK(err.find("kMaxPlacements") != std::string::npos);
  CHECK(a.placements.empty());
  // Ignored records do not count against the cap (they are never drawn).
  sage_test::Bytes ignored = obj_records(kMaxPlacements * 2, 0x4);
  REQUIRE(mfs.mount("Root/Data/Levels/levt/capc.blk",
                    sage_test::make_blk({{"capc_obj.slb", ignored, static_cast<uint32_t>(ignored.size()), 2}})));
  REQUIRE(load_area(mfs, "levt", "capc", AreaOptions(), a, &err));
  CHECK(a.stats.ignored_records == kMaxPlacements * 2);
}

namespace {
// Binary .x with one triangle per material: `n` materials, each with its own texture name tex<i> (or none). `pos` are
// the triangle's three vertex positions (M1a-4 a: non-finite ones).
sage_test::Bytes x_with_subsets(uint32_t n, bool textured, const std::vector<float>& pos = {0, 0, 0, 1, 0, 0, 0, 0, 1}) {
  using namespace sage_test;
  Bytes b;
  putstr(b, "xof 0303bin 0032");
  auto name = [&](const std::string& s) {
    put16(b, 1);
    put32(b, static_cast<uint32_t>(s.size()));
    putstr(b, s);
  };
  auto ints = [&](const std::vector<uint32_t>& v) {
    put16(b, 6);
    put32(b, static_cast<uint32_t>(v.size()));
    for (uint32_t x : v) put32(b, x);
  };
  auto floats = [&](const std::vector<float>& v) {
    put16(b, 7);
    put32(b, static_cast<uint32_t>(v.size()));
    for (float x : v) putf(b, x);
  };
  name("Mesh");
  put16(b, 10);
  ints({3});
  REQUIRE(pos.size() == 9);
  floats(pos);
  std::vector<uint32_t> faces = {n};
  for (uint32_t i = 0; i < n; ++i) faces.insert(faces.end(), {3, 0, 1, 2});
  ints(faces);
  name("MeshMaterialList");
  put16(b, 10);
  std::vector<uint32_t> ml = {n, n};
  for (uint32_t i = 0; i < n; ++i) ml.push_back(i);
  ints(ml);
  for (uint32_t i = 0; i < n; ++i) {
    name("Material");
    put16(b, 10);
    floats({1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0});
    if (textured) {
      name("TextureFilename");
      put16(b, 10);
      std::string t = "tex" + std::to_string(i);
      put16(b, 2);
      put32(b, static_cast<uint32_t>(t.size()));
      putstr(b, t);
      put16(b, 20);
      put16(b, 11);
    }
    put16(b, 11);
  }
  put16(b, 11);
  put16(b, 11);
  return b;
}
sage_test::Bytes tiny_dxt1() {
  using namespace sage_test;
  Bytes b;
  putstr(b, "DDS ");
  put32(b, 124);
  put32(b, 0x1007);
  put32(b, 4);
  put32(b, 4);
  for (int i = 0; i < 14; ++i) put32(b, 0);
  put32(b, 32);
  put32(b, 0x4);
  putstr(b, "DXT1");
  for (int i = 0; i < 5; ++i) put32(b, 0);
  put32(b, 0x1000);
  for (int i = 0; i < 4; ++i) put32(b, 0);
  for (int i = 0; i < 8; ++i) put8(b, 0x11);
  return b;
}
sage_test::BlkEntrySpec entry(const std::string& n, const sage_test::Bytes& b) { return {n, b, static_cast<uint32_t>(b.size()), 2}; }
}  // namespace

TEST_CASE("area: draw cap (placements x subsets) fails the load cleanly") {
  fs::MemoryFileSystem mfs;
  REQUIRE(mount_synthetic_install(mfs));
  sage_test::Bytes x5 = x_with_subsets(5, false);
  // 819 x 5 = 4095 draws fit; 820 x 5 = 4100 > kMaxDraws (4096).
  sage_test::Bytes fit = obj_records(kMaxDraws / 5, 0x10), over = obj_records(kMaxDraws / 5 + 1, 0x10);
  REQUIRE(mfs.mount("Root/Data/Levels/levt/drwa.blk", sage_test::make_blk({entry("blue.x", x5), entry("drwa_obj.slb", fit)})));
  REQUIRE(mfs.mount("Root/Data/Levels/levt/drwb.blk", sage_test::make_blk({entry("blue.x", x5), entry("drwb_obj.slb", over)})));
  AreaData a;
  std::string err;
  REQUIRE_MESSAGE(load_area(mfs, "levt", "drwa", AreaOptions(), a, &err), err);
  CHECK(a.stats.draws == (kMaxDraws / 5) * 5);
  CHECK_FALSE(load_area(mfs, "levt", "drwb", AreaOptions(), a, &err));
  CHECK(err.find("kMaxDraws") != std::string::npos);
}

TEST_CASE("area: texture-count cap fails the load cleanly") {
  // One model whose kMaxAreaTextures + 1 subsets each name a different, existing texture.
  const uint32_t n = kMaxAreaTextures + 1;
  sage_test::Bytes dds = tiny_dxt1();
  std::vector<sage_test::BlkEntrySpec> level;
  for (uint32_t i = 0; i < n; ++i) level.push_back(entry("tex" + std::to_string(i) + ".dds", dds));
  std::sort(level.begin(), level.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
  fs::MemoryFileSystem mfs;
  REQUIRE(mfs.mount("Root/Data/Levels/levx/levx.blk", sage_test::make_blk(level)));
  sage_test::Bytes xa = x_with_subsets(kMaxAreaTextures, true), xb = x_with_subsets(n, true), recs = obj_records(1, 0x10);
  REQUIRE(mfs.mount("Root/Data/Levels/levx/texa.blk", sage_test::make_blk({entry("blue.x", xa), entry("texa_obj.slb", recs)})));
  REQUIRE(mfs.mount("Root/Data/Levels/levx/texb.blk", sage_test::make_blk({entry("blue.x", xb), entry("texb_obj.slb", recs)})));
  AreaData a;
  std::string err;
  REQUIRE_MESSAGE(load_area(mfs, "levx", "texa", AreaOptions(), a, &err), err);
  CHECK(a.textures.size() == kMaxAreaTextures);
  CHECK_FALSE(load_area(mfs, "levx", "texb", AreaOptions(), a, &err));
  CHECK(err.find("kMaxAreaTextures") != std::string::npos);
  CHECK(a.textures.empty());
}

TEST_CASE("area: mutated synthetic blockfiles never crash and never leave a partial area (deterministic fuzz)") {
  fs::MemoryFileSystem base;
  REQUIRE(mount_synthetic_install(base));
  std::vector<uint8_t> level, area;
  REQUIRE(base.read("Root/Data/Levels/levt/levt.blk", level) == fs::Error::None);
  REQUIRE(base.read("Root/Data/Levels/levt/test.blk", area) == fs::Error::None);
  uint32_t seed = 0x1234567u;
  auto rnd = [&]() {
    seed = seed * 1664525u + 1013904223u;
    return seed >> 8;
  };
  uint32_t loaded = 0;
  const int iterations = 1500;
  for (int it = 0; it < iterations; ++it) {
    std::vector<uint8_t> l = level, a = area;
    std::vector<uint8_t>& t = (it & 1) ? l : a;
    switch (rnd() % 4) {
      case 0: t.resize(rnd() % (t.size() + 1)); break;                                      // truncate
      case 1: for (int k = 0; k < 4; ++k) t[rnd() % t.size()] ^= uint8_t(1u << (rnd() % 8)); break;  // bit flips
      case 2: { size_t at = rnd() % (t.size() - 3); uint32_t v = rnd() % 3 ? 0xffffffffu : rnd(); std::memcpy(&t[at], &v, 4); break; }
      default: t[rnd() % t.size()] = uint8_t(rnd()); break;
    }
    fs::MemoryFileSystem mfs;
    REQUIRE(mfs.mount("Root/Data/Levels/levt/levt.blk", std::move(l)));
    REQUIRE(mfs.mount("Root/Data/Levels/levt/test.blk", std::move(a)));
    AreaData out;
    std::string err;
    if (!load_area(mfs, "levt", "test", AreaOptions(), out, &err)) {
      CHECK(out.placements.empty());
      CHECK(out.models.empty());
      continue;
    }
    ++loaded;
    // Every accepted area must upload and render without a rejected rhi call.
    std::vector<std::string> log;
    auto dev = rhi::create_null_device(&log);
    rhi::RenderTargetDesc rd{16, 16, true, 1.0f};
    rhi::TextureId rt = dev->create_render_target_ex(rd);
    AreaRenderer r;
    REQUIRE(r.upload(*dev, out, RenderOptions(), &err));
    r.render(*dev, rt, out, make_camera(out, synthetic_camera(), 1.0f));
    for (const auto& line : log) CHECK(line.find("rejected") == std::string::npos);
    CHECK(dev->last_frame_stats().draws == out.stats.draws);
  }
  CHECK(loaded > 0);
  CHECK(loaded < static_cast<uint32_t>(iterations));  // the mutations do reach the rejection paths
}

namespace {
// Uncompressed 32-bpp TGA (type 2), bottom-up, BGRA pixels all equal to `bgra`, declaring `alpha_bits`.
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
}  // namespace

TEST_CASE("area: TGA textures resolve after .dds misses; 0 declared alpha bits means opaque (spec R8)") {
  fs::MemoryFileSystem base;
  REQUIRE(mount_synthetic_install(base));
  std::vector<uint8_t> area;
  REQUIRE(base.read("Root/Data/Levels/levt/test.blk", area) == fs::Error::None);
  const uint8_t half[4] = {10, 20, 30, 0};  // alpha byte 0
  fs::MemoryFileSystem mfs;
  REQUIRE(mfs.mount("Root/Data/Levels/levg/levg.blk",
                    sage_test::make_blk({entry("cut.tga", tga32(4, 2, 8, half)), entry("red.tga", tga32(2, 2, 0, half))})));
  REQUIRE(mfs.mount("Root/Data/Levels/levg/test.blk", std::move(area)));
  AreaData a;
  std::string err;
  REQUIRE_MESSAGE(load_area(mfs, "levg", "test", AreaOptions(), a, &err), err);
  REQUIRE(a.textures.size() == 2);
  CHECK(a.textures[0].request == "Root/Data/Levels/levg/textures/red.tga");
  CHECK(a.textures[0].format == "TGA32");
  CHECK_FALSE(a.textures[0].alpha_on);
  CHECK(a.textures[0].mips[0].rgba == std::vector<uint8_t>{30, 20, 10, 255, 30, 20, 10, 255, 30, 20, 10, 255, 30, 20, 10, 255});
  CHECK(a.textures[1].format == "TGA32a");
  CHECK(a.textures[1].alpha_on);
  CHECK(a.textures[1].mips[0].rgba[3] == 0);
  CHECK(a.textures[1].mips.size() == 3);  // 4x2 -> 2x1 -> 1x1
  CHECK(a.models[0].subsets[0].alpha == false);
  CHECK(a.models[0].subsets[1].alpha == true);
}

TEST_CASE("area: interleaved vertex = position, normal, uv, colour (48-byte stride)") {
  AreaData a = load_synthetic();
  REQUIRE(kVertexFloats == 12);
  const Model& q = a.models[0];
  REQUIRE(q.vertices.size() == size_t(q.vertex_count) * kVertexFloats);
  const float* v1 = &q.vertices[1 * kVertexFloats];  // red quad corner (x1, 0, z0) = (-1, 0, -4), uv (1, 1)
  // M1a-3: vertex colours are stored as the D3DCOLOR the vertex buffer holds: 0.5 -> 128 -> 128/255.
  CHECK(std::vector<float>(v1, v1 + 12) == std::vector<float>{-1, 0, -4, 0, -1, 0, 1, 1, 1, 128.0f / 255.0f, 1, 1});
  const Model& b = a.models[1];  // no MeshNormals: zero normal; no MeshTextureCoords: uv 0; no colours: white
  CHECK(std::vector<float>(b.vertices.begin(), b.vertices.begin() + 12) == std::vector<float>{-8, 0, -2, 0, 0, 0, 0, 0, 1, 1, 1, 1});
}

TEST_CASE("area: release_cpu_copies frees vertex/index/mip copies; rendering is unchanged; re-upload is refused") {
  AreaData a = load_synthetic();
  auto frame = [](AreaData& area, bool release) {
    std::vector<std::string> log;
    auto dev = rhi::create_null_device(&log);
    rhi::TextureId rt = dev->create_render_target_ex(rhi::RenderTargetDesc{64, 64, true, 1.0f});
    AreaRenderer r;
    std::string err;
    REQUIRE(r.upload(*dev, area, RenderOptions(), &err));
    if (release) {
      CHECK(release_cpu_copies(area) >= 12 * 48 + 18 * 4 + 4 * 48 + 6 * 4 + 424);
      CHECK_FALSE(r.upload(*dev, area, RenderOptions(), &err));
      CHECK(err.find("already released") != std::string::npos);
    }
    log.clear();
    r.render(*dev, rt, area, make_camera(area, synthetic_camera(), 1.0f));
    return log;
  };
  const auto kept = frame(a, false);
  const auto released = frame(a, true);
  CHECK(a.cpu_released);
  CHECK(a.models[0].vertices.empty());
  CHECK(a.models[0].indices.empty());
  CHECK(a.textures[0].mips.empty());
  CHECK(a.models[0].subsets.size() == 3);
  CHECK(kept == released);
}

// ---- Task 6 cleanup (B1-B7) ----
namespace {

sage_test::Bytes synthetic_entry(const char* name) {
  fs::MemoryFileSystem mfs;
  REQUIRE(mount_synthetic_install(mfs));
  std::vector<uint8_t> blk;
  REQUIRE(mfs.read("Root/Data/Levels/levt/test.blk", blk) == fs::Error::None);
  sage::BlockFile bf;
  std::string err;
  REQUIRE(bf.open_bytes(blk, &err));
  std::vector<uint8_t> x;
  REQUIRE(bf.find(name) >= 0);
  REQUIRE(bf.read(static_cast<size_t>(bf.find(name)), x, &err));
  return x;
}

// One OBJ record: `id` is the 4CC as fourcc() prints it (stored byte-reversed, little-endian u32).
struct ObjSpec {
  std::string id;
  float pos[3];
  float orient[3];
  uint32_t flags;
};
sage_test::Bytes obj_table(const std::vector<ObjSpec>& recs) {
  using namespace sage_test;
  Bytes b;
  put32(b, 0);
  put32(b, static_cast<uint32_t>(recs.size()));
  put32(b, static_cast<uint32_t>(recs.size()));
  put32(b, 16);
  for (const ObjSpec& r : recs) {
    REQUIRE(r.id.size() == 4);
    for (int k = 3; k >= 0; --k) put8(b, static_cast<uint8_t>(r.id[k]));
    for (float v : r.pos) putf(b, v);
    for (float v : r.orient) putf(b, v);
    put32(b, 0);
    for (int k = 0; k < 6; ++k) putf(b, 0.0f);
    put32(b, r.flags);
  }
  put32(b, 12);
  put32(b, 1);
  put32(b, 0x00C0FFEEu);
  return b;
}

// The vb id of every draw_indexed line, consecutive repeats collapsed (one entry per drawn object).
std::vector<int> object_vbs(const std::vector<std::string>& log) {
  std::vector<int> out;
  for (const auto& l : log) {
    if (l.rfind("draw_indexed", 0) != 0) continue;
    const size_t at = l.find(" vb=");
    const int vb = std::atoi(l.c_str() + at + 4);
    if (out.empty() || out.back() != vb) out.push_back(vb);
  }
  return out;
}

std::vector<std::string> render_log(const AreaData& a, const Camera& cam) {
  std::vector<std::string> log;
  auto dev = rhi::create_null_device(&log);
  const rhi::TextureId rt = dev->create_render_target_ex(rhi::RenderTargetDesc{16, 16, true, 1.0f});
  AreaRenderer r;
  std::string err;
  REQUIRE_MESSAGE(r.upload(*dev, a, RenderOptions(), &err), err);
  log.clear();
  REQUIRE(r.render(*dev, rt, a, cam));
  return log;
}

// A rejected load leaves `out` equal to a default AreaData.
bool is_default(const AreaData& a) {
  return a.level.empty() && a.area.empty() && a.placements.empty() && a.models.empty() && a.textures.empty() && a.stats.draws == 0 &&
         a.stats.placements == 0 && a.warnings.empty() && a.warnings_dropped == 0 && !a.has_start && !a.has_bounds && !a.art_blk &&
         a.pos_records == 0;
}

std::string id4(uint32_t i) {  // distinct printable 4CCs
  std::string s(4, 'a');
  for (int k = 0; k < 4; ++k, i /= 26) s[k] = static_cast<char>('a' + i % 26);
  return s;
}

}  // namespace

TEST_CASE("area: non-finite OBJ positions/orientations are skipped at load; the trans sort is a total order (B1)") {
  const float nan = std::nanf(""), inf = std::numeric_limits<float>::infinity();
  fs::MemoryFileSystem mfs;
  REQUIRE(mount_synthetic_install(mfs));
  const sage_test::Bytes recs = obj_table({
      {"blue", {0, 0, -10}, {0, 0, 0}, 0x110},
      {"quad", {nan, 0, 0}, {0, 0, 0}, 0x110},
      {"quad", {0, 0, -30}, {0, 0, 0}, 0x110},
      {"blue", {0, inf, 0}, {0, 0, 0}, 0x110},
      {"trio", {0, 0, -20}, {0, 0, 0}, 0x110},
      {"blue", {0, 0, -5}, {nan, 0, 0}, 0x110},
      {"blue", {-inf, 0, 0}, {0, 0, 0}, 0x10},
  });
  REQUIRE(mfs.mount("Root/Data/Levels/levt/nanf.blk",
                    sage_test::make_blk({entry("blue.x", synthetic_entry("blue.x")), entry("nanf_obj.slb", recs),
                                         entry("quad.x", synthetic_entry("quad.x")), entry("trio.x", x_with_subsets(1, false))})));
  AreaData a;
  std::string err;
  REQUIRE_MESSAGE(load_area(mfs, "levt", "nanf", AreaOptions(), a, &err), err);
  CHECK(a.stats.obj_records == 7);
  CHECK(a.stats.placements == 3);
  CHECK(a.stats.nonfinite_records == 4);
  for (int r : {1, 3, 5, 6}) CHECK(has_warning(a, "obj record " + std::to_string(r) + ": non-finite position or orientation (skipped)"));
  REQUIRE(a.models.size() == 3);  // blue, quad, trio: a skipped record never loads its model first
  CHECK(a.models[1].id == "quad");
  for (int k = 0; k < 3; ++k) {
    CHECK(std::isfinite(a.bounds_min[k]));
    CHECK(std::isfinite(a.bounds_max[k]));
  }
  // Buffers: uniforms 1, blue vb 2, quad vb 4, trio vb 6. Trans order = GcTransDirector::SortIt from the eye (0,0,0): the
  // farthest greatest-Y point (model space + position, xz only) first: quad (50, -26), trio (0, -19), blue (-8, -2).
  CameraSettings cs = synthetic_camera();
  cs.eye_set = true;
  CHECK(object_vbs(render_log(a, make_camera(a, cs, 1.0f))) == std::vector<int>{4, 6, 2});
  // Non-finite distances (injected after load): an infinite one is the farthest; a NaN one never wins a comparison, so
  // that object is left over; the original would then keep a stale pick (no termination): we take the first remaining.
  AreaData b = a;
  b.placements[0].pos[0] = nan;  // blue
  b.placements[1].pos[2] = inf;  // quad
  CHECK(object_vbs(render_log(b, make_camera(b, cs, 1.0f))) == std::vector<int>{4, 6, 2});
  // A non-finite eye makes every distance NaN: each round takes the first remaining, i.e. record order.
  Camera c = make_camera(a, cs, 1.0f);
  c.eye[0] = nan;
  CHECK(object_vbs(render_log(a, c)) == std::vector<int>{2, 4, 6});
}

TEST_CASE("area: geometry-byte and texture-byte caps fail the load cleanly, leaving a default AreaData (B2)") {
  CHECK(AreaOptions().max_geometry_bytes == kMaxAreaGeometryBytes);  // injectable for tests, defaults unchanged
  CHECK(AreaOptions().max_texture_bytes == kMaxAreaTextureBytes);
  CHECK(AreaOptions().max_placements == kMaxPlacements);
  CHECK(AreaOptions().max_draws == kMaxDraws);
  CHECK(AreaOptions().max_textures == kMaxAreaTextures);
  const AreaData base = load_synthetic();
  const uint64_t geo = base.stats.geometry_bytes, tex = base.stats.texture_bytes;
  // Two models (quad 12 x 48 + 18 x 4, blue 4 x 48 + 6 x 4) and two textures (84 + 340 bytes with mips).
  CHECK(geo == (12 * 48 + 18 * 4) + (4 * 48 + 6 * 4));
  CHECK(tex == 424);
  fs::MemoryFileSystem mfs;
  REQUIRE(mount_synthetic_install(mfs));
  std::string err;
  AreaOptions o;
  AreaData a;
  o.max_geometry_bytes = geo;  // exactly the sum: fits
  REQUIRE_MESSAGE(load_area(mfs, "levt", "test", o, a, &err), err);
  o.max_geometry_bytes = geo - 1;  // the second model crosses it
  CHECK_FALSE(load_area(mfs, "levt", "test", o, a, &err));
  CHECK(err.find("kMaxAreaGeometryBytes") != std::string::npos);
  CHECK(is_default(a));
  o = AreaOptions();
  o.max_texture_bytes = tex;
  REQUIRE_MESSAGE(load_area(mfs, "levt", "test", o, a, &err), err);
  o.max_texture_bytes = tex - 1;  // the second texture crosses it
  CHECK_FALSE(load_area(mfs, "levt", "test", o, a, &err));
  CHECK(err.find("kMaxAreaTextureBytes") != std::string::npos);
  CHECK(is_default(a));
}

TEST_CASE("area: targeted OBJ / .x mutations reach the cap and mid-load rejection paths (deterministic fuzz, B3)") {
  const sage_test::Bytes quad = synthetic_entry("quad.x"), blue = synthetic_entry("blue.x"), obj = synthetic_entry("test_obj.slb"),
                         pos = synthetic_entry("test_pos.slb");
  fs::MemoryFileSystem base;
  REQUIRE(mount_synthetic_install(base));
  std::vector<uint8_t> level;
  REQUIRE(base.read("Root/Data/Levels/levt/levt.blk", level) == fs::Error::None);
  // Each iteration lowers ONE cap to a value in [0, the unmutated scene's total]: what a mutation adds (or the scene
  // itself) then crosses it mid-load, after models and textures were already loaded.
  const AreaData ref = load_synthetic();
  // quad.x's MeshMaterialList ints {3 materials, 3 faces, 0, 1, 2} (binary .x: token 6, count 5, five u32).
  const uint8_t mml[] = {6, 0, 5, 0, 0, 0, 3, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0};
  const auto mml_at = std::search(quad.begin(), quad.end(), std::begin(mml), std::end(mml));
  REQUIRE(mml_at != quad.end());
  const size_t mml_off = static_cast<size_t>(mml_at - quad.begin()) + 6;  // first of the five u32
  const uint32_t n_obj = 8, rec0 = 0x16;
  uint32_t seed = 0x5eedu;
  auto rnd = [&]() {
    seed = seed * 1664525u + 1013904223u;
    return seed >> 8;
  };
  std::map<std::string, int> reasons;
  const int iterations = 1200;
  for (int it = 0; it < iterations; ++it) {
    sage_test::Bytes q = quad, o = obj;
    for (int m = 0, n = 1 + rnd() % 3; m < n; ++m) {
      switch (rnd() % 4) {
        case 0: {  // record count (both copies of n)
          static const uint32_t counts[] = {0, 1, 7, 9, 0xffffffffu, 0x10000000u};
          const uint32_t v = counts[rnd() % 6];
          sage_test::set32(o, 4, v);
          if (rnd() % 2) sage_test::set32(o, 8, v);
          break;
        }
        case 1: {  // routing flags of one record
          static const uint32_t flags[] = {0x0, 0x04, 0x08, 0x10, 0x20, 0x30, 0x110, 0x01};
          sage_test::set32(o, rec0 + (rnd() % n_obj) * 60 + 0x38, rnd() % 4 ? flags[rnd() % 8] : rnd());
          break;
        }
        case 2: {  // model id of one record: an existing, a missing or a random model
          static const char* ids[] = {"dauq", "eulb", "ssim", "xxxx"};
          const char* id = ids[rnd() % 4];
          for (int k = 0; k < 4; ++k) o[rec0 + (rnd() % n_obj) * 60 + k] = static_cast<uint8_t>(id[k]);
          break;
        }
        default: {  // quad.x material count / per-face material index
          static const uint32_t vals[] = {0, 1, 2, 3, 4, 0xffffffffu};
          sage_test::set32(q, mml_off + 4 * (rnd() % 5), vals[rnd() % 6]);
          break;
        }
      }
    }
    AreaOptions caps;
    switch (rnd() % 5) {
      case 0: caps.max_placements = rnd() % (ref.stats.placements + 1); break;
      case 1: caps.max_draws = rnd() % (ref.stats.draws + 1); break;
      case 2: caps.max_textures = rnd() % static_cast<uint32_t>(ref.textures.size() + 1); break;
      case 3: caps.max_texture_bytes = rnd() % (ref.stats.texture_bytes + 1); break;
      default: caps.max_geometry_bytes = rnd() % (ref.stats.geometry_bytes + 1); break;
    }
    fs::MemoryFileSystem mfs;
    REQUIRE(mfs.mount("Root/Data/Levels/levt/levt.blk", std::vector<uint8_t>(level)));
    REQUIRE(mfs.mount("Root/Data/Levels/levt/test.blk",
                      sage_test::make_blk({entry("blue.x", blue), entry("quad.x", q), entry("test_obj.slb", o), entry("test_pos.slb", pos)})));
    AreaData out;
    std::string err;
    if (!load_area(mfs, "levt", "test", caps, out, &err)) {
      CHECK(is_default(out));
      ++reasons[err];
      continue;
    }
    ++reasons["(loaded)"];
    std::vector<std::string> log;
    auto dev = rhi::create_null_device(&log);
    const rhi::TextureId rt = dev->create_render_target_ex(rhi::RenderTargetDesc{16, 16, true, 1.0f});
    AreaRenderer r;
    REQUIRE(r.upload(*dev, out, RenderOptions(), &err));
    REQUIRE(r.render(*dev, rt, out, make_camera(out, synthetic_camera(), 1.0f)));
    for (const auto& line : log) CHECK(line.find("rejected") == std::string::npos);
    CHECK(dev->last_frame_stats().draws == out.stats.draws);
  }
  std::printf("B3 targeted fuzz, %d iterations, outcome histogram:\n", iterations);
  for (const auto& kv : reasons) std::printf("  %5d  %s\n", kv.second, kv.first.c_str());
  CHECK(reasons["(loaded)"] > 0);
  CHECK(reasons["area: more than kMaxPlacements drawn records"] > 0);  // mid-load: after models were loaded
  CHECK(reasons["area: more than kMaxDraws draws"] > 0);
  CHECK(reasons["area: geometry over kMaxAreaGeometryBytes"] > 0);
  CHECK(reasons["area: more than kMaxAreaTextures textures"] > 0);
  CHECK(reasons["area: textures over kMaxAreaTextureBytes"] > 0);
}

TEST_CASE("area: stored warnings are capped at 1024 (+ warnings_dropped); drawable records have a budget (B4)") {
  CHECK(kMaxStoredWarnings == 1024);
  CHECK(kMaxDrawableRecords == kMaxPlacements * 64);
  fs::MemoryFileSystem mfs;
  REQUIRE(mount_synthetic_install(mfs));
  std::vector<ObjSpec> few, many;
  for (uint32_t i = 0; i < 2000; ++i) few.push_back({id4(i), {0, 0, 0}, {0, 0, 0}, 0x10});
  for (uint32_t i = 0; i < 100000; ++i) many.push_back({id4(i), {0, 0, 0}, {0, 0, 0}, 0x10});
  REQUIRE(mfs.mount("Root/Data/Levels/levt/fewm.blk", sage_test::make_blk({entry("fewm_obj.slb", obj_table(few))})));
  REQUIRE(mfs.mount("Root/Data/Levels/levt/manm.blk", sage_test::make_blk({entry("manm_obj.slb", obj_table(many))})));
  AreaData a;
  std::string err;
  REQUIRE_MESSAGE(load_area(mfs, "levt", "fewm", AreaOptions(), a, &err), err);
  CHECK(a.stats.missing_models == 2000);
  CHECK(a.warnings.size() == kMaxStoredWarnings);
  CHECK(a.warnings_dropped == 2002 - kMaxStoredWarnings);  // + "art.blk not opened" and "no pos table"
  CHECK(std::is_sorted(a.warnings.begin(), a.warnings.end()));
  CHECK_FALSE(load_area(mfs, "levt", "manm", AreaOptions(), a, &err));
  CHECK(err.find("kMaxDrawableRecords") != std::string::npos);
  CHECK(is_default(a));
}

TEST_CASE("area: pitch is clamped to +-89.9 degrees, so straight up/down still gives a full view basis (B5)") {
  const AreaData a = load_synthetic();
  for (float pitch : {-90.0f, 90.0f, -1000.0f}) {
    CameraSettings s = synthetic_camera();
    s.pitch_deg = pitch;
    const Camera c = make_camera(a, s, 1.0f);
    CHECK(c.pitch_deg == (pitch > 0 ? 89.9f : -89.9f));
    for (int col = 0; col < 3; ++col) {  // the x, y, z axes of the view basis are columns 0..2 of rows 0..2
      const float* m = c.view.m;
      const float len2 = m[col] * m[col] + m[4 + col] * m[4 + col] + m[8 + col] * m[8 + col];
      CHECK(len2 == doctest::Approx(1.0f).epsilon(1e-4));
    }
  }
  CameraSettings s = synthetic_camera();
  s.pitch_deg = -10.0f;  // in range: unchanged
  CHECK(make_camera(a, s, 1.0f).pitch_deg == -10.0f);
}

TEST_CASE("area: render() needs a successful upload of the same AreaData; a repeat upload is rejected (B6)") {
  AreaData a = load_synthetic();
  std::vector<std::string> log;
  auto dev = rhi::create_null_device(&log);
  const rhi::TextureId rt = dev->create_render_target_ex(rhi::RenderTargetDesc{16, 16, true, 1.0f});
  const Camera cam = make_camera(a, synthetic_camera(), 1.0f);
  AreaRenderer fresh;
  log.clear();
  CHECK_FALSE(fresh.render(*dev, rt, a, cam));  // never uploaded
  CHECK_FALSE(fresh.render(*dev, rt, a, cam));  // (logged once)
  CHECK(log.empty());                           // no frame, no rhi call
  AreaRenderer r;
  std::string err;
  REQUIRE(r.upload(*dev, a, RenderOptions(), &err));
  const size_t after_upload = log.size();
  CHECK_FALSE(r.upload(*dev, a, RenderOptions(), &err));  // repeat: rejected, creates nothing (no leaked GPU objects)
  CHECK(err.find("already uploaded") != std::string::npos);
  CHECK(log.size() == after_upload);
  CHECK(r.render(*dev, rt, a, cam));  // the first upload still renders
  CHECK(dev->last_frame_stats().draws == 10);
  const AreaData other = a;  // same content, different object: not what was uploaded
  log.clear();
  CHECK_FALSE(r.render(*dev, rt, other, cam));
  CHECK(log.empty());
  // A refused upload leaves the renderer without anything to render.
  AreaData released = load_synthetic();
  release_cpu_copies(released);
  AreaRenderer failed;
  CHECK_FALSE(failed.upload(*dev, released, RenderOptions(), &err));
  CHECK_FALSE(failed.render(*dev, rt, released, cam));
}

TEST_CASE("area: warnings escape file bytes (non-printable, >= 0x80) as \\xHH (B7)") {
  fs::MemoryFileSystem mfs;
  REQUIRE(mount_synthetic_install(mfs));
  const std::string raw_id = std::string("\x01\xff", 2) + "ab";  // fourcc order
  REQUIRE(mfs.mount("Root/Data/Levels/levt/esc1.blk",
                    sage_test::make_blk({entry("esc1_obj.slb", obj_table({{raw_id, {0, 0, 0}, {0, 0, 0}, 0x10}}))})));
  AreaData a;
  std::string err;
  REQUIRE_MESSAGE(load_area(mfs, "levt", "esc1", AreaOptions(), a, &err), err);
  CHECK(a.stats.missing_models == 1);
  CHECK(has_warning(a, "model \\x01\\xffab: "));
  for (const std::string& w : a.warnings)
    for (char ch : w) CHECK((ch >= 0x20 && ch < 0x7f));
}

TEST_CASE("area: load_area reports a typed error kind, Environment (a blockfile is missing) vs Data (C7)") {
  fs::MemoryFileSystem mfs;
  REQUIRE(mount_synthetic_install(mfs));
  const sage_test::Bytes junk(64, 0x58);  // not a BLKF blockfile
  REQUIRE(mfs.mount("Root/Data/Levels/levt/bad1.blk", sage_test::Bytes(junk)));
  REQUIRE(mfs.mount("Root/Data/Levels/levj/levj.blk", sage_test::Bytes(junk)));
  REQUIRE(mfs.mount("Root/Data/Levels/levj/test.blk", sage_test::make_blk({sage_test::stored("readme.txt", "x")})));
  AreaData a;
  std::string err;
  LoadError kind = LoadError::Data;
  CHECK(load_area(mfs, "levt", "test", AreaOptions(), a, &err, &kind));
  CHECK(kind == LoadError::None);
  CHECK_FALSE(load_area(mfs, "levt", "none", AreaOptions(), a, &err, &kind));  // area blockfile missing
  CHECK(kind == LoadError::Environment);
  CHECK_FALSE(load_area(mfs, "levz", "test", AreaOptions(), a, &err, &kind));  // level blockfile missing
  CHECK(kind == LoadError::Environment);
  CHECK_FALSE(load_area(mfs, "levt", "bad1", AreaOptions(), a, &err, &kind));  // area blockfile present but corrupt
  CHECK(kind == LoadError::Data);
  CHECK_FALSE(load_area(mfs, "levj", "test", AreaOptions(), a, &err, &kind));  // level blockfile present but corrupt
  CHECK(kind == LoadError::Data);
  AreaOptions o;
  o.max_placements = 0;
  CHECK_FALSE(load_area(mfs, "levt", "test", o, a, &err, &kind));  // a cap
  CHECK(kind == LoadError::Data);
  CHECK(is_default(a));
  CHECK_FALSE(load_area(mfs, "levt", "none", AreaOptions(), a, &err));  // the kind is optional
}

// ---- M1a-4 (a): non-finite POS start positions and .x vertex positions ----
namespace {
struct PosSpec {
  std::string id;  // fourcc order
  float pos[3];
  uint32_t flags;
};
sage_test::Bytes pos_table(const std::vector<PosSpec>& recs) {  // {slot, n, ptr=12} + 20-byte records
  using namespace sage_test;
  Bytes b;
  put32(b, 0);
  put32(b, static_cast<uint32_t>(recs.size()));
  put32(b, 12);
  for (const PosSpec& r : recs) {
    REQUIRE(r.id.size() == 4);
    for (int k = 3; k >= 0; --k) put8(b, static_cast<uint8_t>(r.id[k]));
    for (float v : r.pos) putf(b, v);
    put32(b, r.flags);
  }
  put32(b, 8);
  put32(b, 1);
  put32(b, 0x00C0FFEEu);
  return b;
}
bool same_matrix(const Mat4& x, const Mat4& y) { return std::memcmp(x.m, y.m, sizeof x.m) == 0; }
}  // namespace

TEST_CASE("area: a POS start with a non-finite position is skipped with a warning; the camera falls back as with no POS (M1a-4 a)") {
  const float nan = std::nanf(""), inf = std::numeric_limits<float>::infinity();
  fs::MemoryFileSystem mfs;
  REQUIRE(mount_synthetic_install(mfs));
  const sage_test::Bytes blue = synthetic_entry("blue.x");
  const sage_test::Bytes obj = obj_table({{"blue", {2, 0, -10}, {0, 0, 0}, 0x10}, {"blue", {-6, 0, -30}, {0, 0, 0}, 0x10}});
  auto mount = [&](const char* area, const std::vector<PosSpec>* pos) {
    std::vector<sage_test::BlkEntrySpec> es = {entry("blue.x", blue), entry(std::string(area) + "_obj.slb", obj)};
    if (pos) es.push_back(entry(std::string(area) + "_pos.slb", pos_table(*pos)));
    REQUIRE(mfs.mount("Root/Data/Levels/levt/" + std::string(area) + ".blk", sage_test::make_blk(es)));
  };
  const std::vector<PosSpec> bad = {{"pkup", {1, 2, 3}, 0x60}, {"strt", {nan, 0, 10}, 0x1000}, {"strt", {0, inf, 10}, 0x1000},
                                    {"strt", {0, 0, -inf}, 0x1000}};
  const std::vector<PosSpec> later = {{"strt", {nan, nan, nan}, 0x1000}, {"strt", {4, 5, 6}, 0x1000}};
  mount("nps0", nullptr);  // the same area without a POS table: the reference fallback
  mount("nps1", &bad);
  mount("nps2", &later);
  AreaData ref, a, b;
  std::string err;
  REQUIRE_MESSAGE(load_area(mfs, "levt", "nps0", AreaOptions(), ref, &err), err);
  REQUIRE_MESSAGE(load_area(mfs, "levt", "nps1", AreaOptions(), a, &err), err);
  REQUIRE_MESSAGE(load_area(mfs, "levt", "nps2", AreaOptions(), b, &err), err);
  CHECK_FALSE(ref.has_start);
  // Every start record is non-finite: no start, one warning per skipped record, the camera exactly as without a POS table.
  CHECK(a.pos_records == 4);
  CHECK_FALSE(a.has_start);
  for (int r : {1, 2, 3}) CHECK(has_warning(a, "pos record " + std::to_string(r) + ": non-finite start position (skipped)"));
  CHECK_FALSE(has_warning(a, "pos record 0:"));  // a finite record without the start flag is not a start
  const Camera cr = make_camera(ref, CameraSettings(), 16.0f / 9.0f), ca = make_camera(a, CameraSettings(), 16.0f / 9.0f);
  CHECK(cr.eye_source == "bounds_centre");
  CHECK(ca.eye_source == "bounds_centre");
  CHECK(ca.yaw_source == cr.yaw_source);
  CHECK(std::memcmp(ca.eye, cr.eye, sizeof ca.eye) == 0);
  CHECK(ca.yaw_deg == cr.yaw_deg);
  CHECK(same_matrix(ca.view, cr.view));
  CHECK(same_matrix(ca.proj, cr.proj));
  for (float v : ca.eye) CHECK(std::isfinite(v));
  // A skipped record is not there: a later finite start record is the start.
  CHECK(b.has_start);
  CHECK(b.start_record == 1);
  CHECK(b.start[0] == 4.0f);
  CHECK(b.start[1] == 5.0f);
  CHECK(b.start[2] == 6.0f);
  CHECK(has_warning(b, "pos record 0: non-finite start position (skipped)"));
  CHECK(make_camera(b, CameraSettings(), 1.0f).eye_source == "pos_start");
}

TEST_CASE("area: a model with any non-finite vertex position is rejected at load with a warning (M1a-4 a)") {
  const float nan = std::nanf(""), inf = std::numeric_limits<float>::infinity();
  fs::MemoryFileSystem mfs;
  REQUIRE(mount_synthetic_install(mfs));
  const sage_test::Bytes blue = synthetic_entry("blue.x");
  const sage_test::Bytes obj = obj_table({{"nanx", {0, 0, -10}, {0, 0, 0}, 0x10},
                                          {"infx", {0, 0, -12}, {0, 0, 0}, 0x10},
                                          {"ninf", {0, 0, -14}, {0, 0, 0}, 0x10},
                                          {"blue", {0, 0, -20}, {0, 0, 0}, 0x10},
                                          {"nanx", {5, 0, -10}, {0, 0, 0}, 0x110}});
  REQUIRE(mfs.mount("Root/Data/Levels/levt/nfvx.blk",
                    sage_test::make_blk({entry("blue.x", blue),  // entries in name order (the blockfile directory is sorted)
                                         entry("infx.x", x_with_subsets(1, false, {inf, 0, 0, 1, 0, 0, 0, 0, 1})),
                                         entry("nanx.x", x_with_subsets(1, false, {0, 0, 0, 1, nan, 0, 0, 0, 1})),
                                         entry("nfvx_obj.slb", obj),
                                         entry("ninf.x", x_with_subsets(1, false, {0, 0, 0, 1, 0, 0, 0, 0, -inf}))})));
  AreaData a;
  std::string err;
  REQUIRE_MESSAGE(load_area(mfs, "levt", "nfvx", AreaOptions(), a, &err), err);
  for (const char* id : {"nanx", "infx", "ninf"}) CHECK(has_warning(a, std::string("model ") + id + ": non-finite vertex position (rejected)"));
  REQUIRE(a.models.size() == 1);  // only blue: a rejected model is cached as missing (one warning, every record skipped)
  CHECK(a.models[0].id == "blue");
  CHECK(a.stats.placements == 1);
  CHECK(a.stats.missing_models == 4);
  REQUIRE(a.has_bounds);
  for (int k = 0; k < 3; ++k) {
    CHECK(std::isfinite(a.bounds_min[k]));
    CHECK(std::isfinite(a.bounds_max[k]));
  }
  for (const Model& m : a.models)
    for (float v : m.vertices) CHECK(std::isfinite(v));
}
