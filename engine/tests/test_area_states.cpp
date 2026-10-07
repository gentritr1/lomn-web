// M1a-3 area-loader additions (lomn/view/area.h): LIGHT.slb / default lights (GcLightLoader::LoadLights 0x00470010,
// GrSetDefaultLighting 0x00470560), the Base.osi fog / cave-lighting literals and textures.slb, on the synthetic state scene.
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "doctest.h"
#include "lomn/d3d/device8.h"
#include "lomn/fs.h"
#include "lomn/sage/blockfile.h"
#include "lomn/view/area.h"
#include "lomn/view/synthetic.h"
#include "osi_builder.h"
#include "sage_test_util.h"

using namespace lomn;
using namespace lomn::d3d;

namespace {

view::AreaData synthetic_area() {
  fs::MemoryFileSystem mfs;
  REQUIRE(view::mount_synthetic_install(mfs));
  view::AreaData a;
  std::string err;
  REQUIRE_MESSAGE(view::load_area(mfs, view::kSyntheticLevel, view::kSyntheticStateArea, view::AreaOptions(), a, &err), err);
  return a;
}

}  // namespace

TEST_CASE("area: lights, fog and special textures come from LIGHT.slb, Base.osi and textures.slb") {
  view::AreaData a = synthetic_area();
  // LIGHT.slb: a point light and a directional one (+ the opposite "Shad" fill for the first directional).
  REQUIRE(a.lights.size() == 3);
  CHECK(a.lights_source == "light_slb");
  const Light8& pt = a.lights[0].light;
  CHECK(pt.type == kLightPoint);
  CHECK(pt.range == 20.0f);
  CHECK(pt.attenuation1 == doctest::Approx(1.0f / (0.15f * 20.0f)));
  // intensity 1.0 > 0.9 -> x 1.2; then SetCaveLighting(1) from the script: style 0 -> SetIntensity(0.25) on the base.
  CHECK(pt.diffuse.g == doctest::Approx(0.5f * 0.25f));
  CHECK(a.lights[1].name == "sun1");
  CHECK(a.lights[1].light.direction[1] == -1.0f);
  CHECK(a.lights[2].name == "Shad");
  CHECK(a.lights[2].light.direction[0] == -0.5f);
  CHECK(a.cave_lighting);
  CHECK(a.fog.source == "osi");
  CHECK(a.fog.color == 0xff808080u);
  CHECK(a.fog.start == 5.0f);  // GcAreaDirector::SetFog negates the script's -5 / -50
  CHECK(a.fog.end == 50.0f);
  CHECK(a.fog.density == doctest::Approx(0.1f));
  CHECK(a.script_class == "lego_area_statt");
  CHECK(a.special_textures == 3);
  CHECK(a.special_matches == 2);  // "tfx" (trans) and "cut" (scroll); "red" is (1, 0, 0) and skipped
  CHECK(a.level_type == uint8_t('t' - '0'));
}

TEST_CASE("area: default lighting by level type (GrSetDefaultLighting)") {
  fs::MemoryFileSystem mfs;
  REQUIRE(view::mount_synthetic_install(mfs));
  std::vector<uint8_t> blk;
  REQUIRE(mfs.read("Root/Data/Levels/levt/levt.blk", blk) == fs::Error::None);
  for (const char* lev : {"lev1", "lev4"}) {
    fs::MemoryFileSystem m2;
    REQUIRE(m2.mount(std::string("Root/Data/Levels/") + lev + "/" + lev + ".blk", blk));
    std::vector<uint8_t> area;
    REQUIRE(mfs.read("Root/Data/Levels/levt/test.blk", area) == fs::Error::None);
    REQUIRE(m2.mount(std::string("Root/Data/Levels/") + lev + "/quad.blk", area));  // no quad_light.slb in it
    view::AreaData a;
    std::string err;
    REQUIRE_MESSAGE(view::load_area(m2, lev, "quad", view::AreaOptions(), a, &err), err);
    CHECK(a.lights_source == "default");
    if (std::string(lev) == "lev1") {
      REQUIRE(a.lights.size() == 1);  // "LmbA": ambient 120/255 grey
      CHECK(a.lights[0].name == "LmbA");
      CHECK(a.lights[0].light.ambient.r == doctest::Approx(0.47058824f));
      CHECK(a.lights[0].light.diffuse.r == 0.0f);
    } else {
      REQUIRE(a.lights.size() == 2);  // SunL white x 1.2, Shad blue-grey (level type 4)
      CHECK(a.lights[0].name == "SunL");
      CHECK(a.lights[0].light.diffuse.r == doctest::Approx(1.2f));
      CHECK(a.lights[0].light.direction[0] == doctest::Approx(0.85570401f));
      CHECK(a.lights[1].light.diffuse.b == doctest::Approx(0.54901963f * 1.2f));
    }
  }
}


// ---- Task 8B D2-D6: pinning the loaded data (the review found 11 simultaneous loader mutations that no test saw). ----
namespace {

using sage_test::Bytes;

const view::Model& model(const view::AreaData& a, const std::string& id) {
  for (const view::Model& m : a.models)
    if (m.id == id) return m;
  FAIL("no model " << id);
  return a.models.front();
}
const view::SubsetDraw& subset(const view::Model& m, const std::string& texture) {
  for (const view::SubsetDraw& d : m.subsets)
    if (d.texture_name == texture) return d;
  FAIL("no subset with texture " << texture);
  return m.subsets.front();
}
void check4(const Color4& c, float r, float g, float b, float a) {
  CHECK(c.r == doctest::Approx(r));
  CHECK(c.g == doctest::Approx(g));
  CHECK(c.b == doctest::Approx(b));
  CHECK(c.a == doctest::Approx(a));
}
uint32_t id4(const char* s) {
  return (uint32_t(uint8_t(s[0])) << 24) | (uint32_t(uint8_t(s[1])) << 16) | (uint32_t(uint8_t(s[2])) << 8) | uint32_t(uint8_t(s[3]));
}
// <area>_LIGHT.slb {slot, n, ptr=12} + 44-byte records {id, pos/dir, intensity, range, rgba, flags} (as synthetic.cpp).
struct L {
  const char* id;
  float x, y, z, intensity, range, r, g, b, a;
  uint32_t flags;
};
Bytes light_slb(const std::vector<L>& ls) {
  Bytes b;
  sage_test::put32(b, 0);
  sage_test::put32(b, static_cast<uint32_t>(ls.size()));
  sage_test::put32(b, 12);
  for (const L& l : ls) {
    sage_test::put32(b, id4(l.id));
    for (float v : {l.x, l.y, l.z, l.intensity, l.range, l.r, l.g, l.b, l.a}) sage_test::putf(b, v);
    sage_test::put32(b, l.flags);
  }
  sage_test::put32(b, 8);
  sage_test::put32(b, 1);
  sage_test::put32(b, 0x00C0FFEEu);
  return b;
}
// textures.slb {n, n, ptr=12} + 20-byte records + length-prefixed names (as synthetic.cpp).
struct T {
  std::string name;
  float trans, u, v;
};
Bytes textures_slb(const std::vector<T>& ts) {
  Bytes b;
  sage_test::put32(b, static_cast<uint32_t>(ts.size()));
  sage_test::put32(b, static_cast<uint32_t>(ts.size()));
  sage_test::put32(b, 12);
  uint32_t str = static_cast<uint32_t>(12 + 20 * ts.size());
  std::vector<uint32_t> slots = {8};
  for (const T& t : ts) {
    slots.push_back(static_cast<uint32_t>(b.size()));
    sage_test::put32(b, str);
    sage_test::putf(b, t.trans);
    sage_test::putf(b, t.u);
    sage_test::putf(b, t.v);
    sage_test::put32(b, 0);
    str += static_cast<uint32_t>(t.name.size()) + 2;
  }
  for (const T& t : ts) {
    b.push_back(static_cast<uint8_t>(t.name.size()));
    sage_test::putstr(b, t.name);
    b.push_back(0);
  }
  while (b.size() % 4) b.push_back(0);
  for (size_t i = slots.size(); i-- > 0;) sage_test::put32(b, slots[i]);
  sage_test::put32(b, static_cast<uint32_t>(slots.size()));
  sage_test::put32(b, 0x00C0FFEEu);
  return b;
}
// Area "lite" of `level`: the synthetic level blockfile mounted as <level>.blk (textures, textures.slb) plus an area
// blockfile holding only the given entries (names sorted); optional Base.osi.
view::AreaData lite(const std::string& level, std::vector<std::pair<std::string, Bytes>> entries, const view::AreaOptions& opt = {},
                    const Bytes* osi = nullptr) {
  fs::MemoryFileSystem syn;
  REQUIRE(view::mount_synthetic_install(syn));
  std::vector<uint8_t> lev;
  REQUIRE(syn.read("Root/Data/Levels/levt/levt.blk", lev) == fs::Error::None);
  fs::MemoryFileSystem m;
  REQUIRE(m.mount("Root/Data/Levels/" + level + "/" + level + ".blk", lev));
  std::sort(entries.begin(), entries.end());
  std::vector<sage_test::BlkEntrySpec> es;
  for (auto& e : entries) es.push_back({e.first, e.second, static_cast<uint32_t>(e.second.size()), 2});
  REQUIRE(m.mount("Root/Data/Levels/" + level + "/lite.blk", sage_test::make_blk(es)));
  if (osi) REQUIRE(m.mount("Root/Data/Base.osi", *osi));
  view::AreaData a;
  std::string err;
  REQUIRE_MESSAGE(view::load_area(m, level, "lite", opt, a, &err), err);
  return a;
}
bool has_warning(const view::AreaData& a, const std::string& part) {
  for (const std::string& w : a.warnings)
    if (w.find(part) != std::string::npos) return true;
  return false;
}
// Base.osi whose class lego_area_lite<digit> InitFull runs `code`.
Bytes lite_osi(const std::string& cls, const std::vector<uint8_t>& code) {
  test::OsiSpec s;
  s.cls = cls;
  s.code = code;
  return test::build_osi(s);
}
void push_f32(std::vector<uint8_t>& c, float f) {
  uint32_t x;
  std::memcpy(&x, &f, 4);
  c.push_back(0x44);
  for (int i = 0; i < 4; ++i) c.push_back(static_cast<uint8_t>(x >> (8 * i)));
}
void push_i32(std::vector<uint8_t>& c, uint32_t v) {
  c.push_back(0x40);
  for (int i = 0; i < 4; ++i) c.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void set_fog(std::vector<uint8_t>& c, uint32_t colour, float start, float end, float density) {  // SetFog(colour, s, e, d)
  push_i32(c, colour);
  push_f32(c, start);
  push_f32(c, end);
  push_f32(c, density);
  c.insert(c.end(), {0x82, 0, 0, 1, 0, 4, 0x30});
}

}  // namespace

TEST_CASE("area: the stat scene's materials are pinned (ambient 0.7, _glass, eye emissive, textures.slb, untextured)") {
  const view::AreaData a = synthetic_area();
  // LoadIndexed forces ambient 0.7 on every material it builds.
  for (const view::Model& m : a.models)
    for (const view::SubsetDraw& d : m.subsets)
      if (d.has_material) check4(d.material.ambient, 0.7f, 0.7f, 0.7f, 1.0f);
  const view::Model& mixm = model(a, "mixm");
  const view::SubsetDraw& glass = subset(mixm, "x_glass");  // "_glass": diffuse 0, mTrans 0
  check4(glass.material.diffuse, 0, 0, 0, 0);
  CHECK(glass.trans == 0.0f);
  CHECK_FALSE(glass.special);
  const view::SubsetDraw& half = subset(mixm, "half");  // plain: the .x diffuse, mTrans 1
  check4(half.material.diffuse, 1, 1, 1, 1);
  check4(half.material.emissive, 0, 0, 0, 1);
  CHECK(half.trans == 1.0f);
  const view::SubsetDraw& tfx = subset(mixm, "tfx");  // textures.slb {tfx, 0.5, 0, 0}: SetTransparency, no scroll
  CHECK(tfx.trans == 0.5f);
  CHECK(tfx.special);
  CHECK(tfx.uv_offset[0] == 0.0f);
  CHECK(tfx.uv_offset[1] == 0.0f);
  const view::Model& quad = model(a, "quad");
  const view::SubsetDraw& cut = subset(quad, "cut");  // {cut, 1, 0.25, 0}: SetUVOffset only
  CHECK(cut.trans == 1.0f);
  CHECK(cut.uv_offset[0] == 0.25f);
  CHECK(cut.uv_offset[1] == 0.0f);
  CHECK(cut.special);
  CHECK_FALSE(subset(quad, "red").special);  // {red, 1, 0, 0} is (1, 0, 0): GcArea::SetSpecialTexture skips it
  CHECK(a.special_matches == 2);
  const view::SubsetDraw& eye = subset(model(a, "eyes"), "on_eye");  // "eye" + "on_": green glow, diffuse black
  check4(eye.material.emissive, 0, 1, 0, 1);
  check4(eye.material.diffuse, 0, 0, 0, 1);
  // GetGreatestZ: every vertex of these flat quads has y = 0, so the LAST vertex in buffer order wins the tie.
  CHECK(mixm.greatest_y[0] == 5.0f);  // quad 3 (5..9, -10..-5), corner 4 = (x0, 0, z1)
  CHECK(mixm.greatest_y[2] == -5.0f);
  CHECK(model(a, "blue").greatest_y[0] == -8.0f);
  CHECK(model(a, "blue").greatest_y[2] == 8.0f);
  CHECK(a.stats.untextured_materials == 3);  // blue, sky1, watr: their material has no TextureFilename
}

TEST_CASE("area: default lighting for level types 2, 3, 5 and 6 (GrSetDefaultLighting colours)") {
  struct Want {
    const char* lev;
    float r, g, b;
  };
  for (const Want w : {Want{"lev3", 1.0f, 0.98039216f, 0.83529413f}, Want{"lev5", 0.95686275f, 1.0f, 0.86666667f},
                       Want{"lev6", 1.0f, 1.0f, 1.0f}}) {
    CAPTURE(w.lev);
    const view::AreaData a = lite(w.lev, {});
    CHECK(a.lights_source == "default");
    REQUIRE(a.lights.size() == 2);
    CHECK(a.lights[0].name == "SunL");
    check4(a.lights[0].light.diffuse, w.r * 1.2f, w.g * 1.2f, w.b * 1.2f, 1.2f);
    CHECK(a.lights[1].name == "Shad");
    check4(a.lights[1].light.diffuse, 0.35294119f * 1.2f, 0.35294119f * 1.2f, 0.35294119f * 1.2f, 1.2f);
    CHECK(a.lights[1].light.direction[0] == doctest::Approx(-0.85570401f));
  }
  const view::AreaData a2 = lite("lev2", {});
  REQUIRE(a2.lights.size() == 1);  // type 2 < 3: "LmbA" ambient only
  CHECK(a2.lights[0].name == "LmbA");
  CHECK(a2.lights[0].light.range == 10000.0f);
  CHECK(a2.lights[0].light.attenuation0 == 1.0f);
}

TEST_CASE("area: LIGHT.slb records: type-4 white directional, one Shad, range <= 0, styles, light_slb+default") {
  // Level type 4: a directional record's colour is replaced by white; Shad is blue-grey; both scaled by the intensity.
  const view::AreaData a4 = lite("lev4", {{"lite_light.slb", light_slb({{"dir1", 0, -1, 0, 0.8f, 0, 0.9f, 0.5f, 0.25f, 1, 0x80}})}});
  REQUIRE(a4.lights.size() == 2);
  check4(a4.lights[0].light.diffuse, 0.8f, 0.8f, 0.8f, 0.8f);
  check4(a4.lights[1].light.diffuse, 0.35294119f * 0.8f, 0.47058824f * 0.8f, 0.54901963f * 0.8f, 0.8f);
  // Two directionals: Shad only after the FIRST (opposite direction, its intensity); level 1 keeps the record colours.
  const view::AreaData a1 = lite("lev1", {{"lite_light.slb", light_slb({{"dir1", 1, 0, 0, 0.5f, 0, 0.9f, 0.5f, 0.25f, 1, 0x80},
                                                                       {"dir2", 0, 0, 1, 0.5f, 0, 0.2f, 0.4f, 0.6f, 1, 0x80}})}});
  REQUIRE(a1.lights.size() == 3);
  CHECK(a1.lights[0].name == "dir1");
  check4(a1.lights[0].light.diffuse, 0.45f, 0.25f, 0.125f, 0.5f);
  CHECK(a1.lights[1].name == "Shad");
  CHECK(a1.lights[1].light.direction[0] == -1.0f);
  CHECK(a1.lights[2].name == "dir2");
  CHECK(a1.lights_source == "light_slb");
  // Point lights: range <= 0 -> attenuation0 1, attenuation1 0, Range 10000 (kMaxLightRange); the style comes from
  // the first set bit of 2 / 4 / 8 / 0x10 / 0x20 -> 1 / 3 / 4 / 5 / 2. Only point lights: the defaults are added after.
  const view::AreaData p3 = lite("lev3", {{"lite_light.slb", light_slb({{"pz00", 0, 0, 0, 0.5f, 0, 1, 1, 1, 1, 0x40},
                                                                       {"pn00", 0, 0, 0, 0.5f, -5, 1, 1, 1, 1, 0x40 | 2},
                                                                       {"ps03", 0, 0, 0, 0.5f, 4, 1, 1, 1, 1, 0x40 | 4},
                                                                       {"ps04", 0, 0, 0, 0.5f, 4, 1, 1, 1, 1, 0x40 | 8},
                                                                       {"ps05", 0, 0, 0, 0.5f, 4, 1, 1, 1, 1, 0x40 | 0x10},
                                                                       {"ps02", 0, 0, 0, 0.5f, 4, 1, 1, 1, 1, 0x40 | 0x20},
                                                                       {"ps12", 0, 0, 0, 0.5f, 4, 1, 1, 1, 1, 0x40 | 2 | 4}})}});
  REQUIRE(p3.lights.size() == 9);
  for (int i : {0, 1}) {
    CHECK(p3.lights[i].light.attenuation0 == 1.0f);
    CHECK(p3.lights[i].light.attenuation1 == 0.0f);
    CHECK(p3.lights[i].light.range == 10000.0f);
  }
  CHECK(p3.lights[2].light.attenuation0 == 0.0f);
  CHECK(p3.lights[2].light.attenuation1 == doctest::Approx(1.0f / (0.15f * 4.0f)));
  const uint8_t styles[7] = {0, 1, 3, 4, 5, 2, 1};
  for (int i = 0; i < 7; ++i) CHECK(p3.lights[i].style == styles[i]);
  CHECK(p3.lights_source == "light_slb+default");
  CHECK(p3.lights[7].name == "SunL");
  check4(p3.lights[7].light.diffuse, 1.2f, 0.98039216f * 1.2f, 0.83529413f * 1.2f, 1.2f);
  CHECK(p3.lights[8].name == "Shad");
}

TEST_CASE("area: SetFog is last-write-wins (the game's statics); cave lighting dims style-0 lights only (Task 8B D5)") {
  std::vector<uint8_t> c;
  set_fog(c, 0xff102030u, -1.0f, -10.0f, 0.25f);
  set_fog(c, 0xff405060u, -2.0f, -20.0f, 0.5f);
  c.push_back(0x47);
  c.push_back(0x52);
  const Bytes osi = lite_osi("lego_area_lite3", c);
  const view::AreaData a = lite("lev3", {}, {}, &osi);
  CHECK(a.script_class == "lego_area_lite3");
  CHECK(a.fog.source == "osi");
  CHECK(a.fog.color == 0xff405060u);
  CHECK(a.fog.start == 2.0f);
  CHECK(a.fog.end == 20.0f);
  CHECK(a.fog.density == 0.5f);
}

TEST_CASE("area: non-finite light, textures.slb and fog values are skipped with a warning (Task 8B D3/D6)") {
  const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
  const view::AreaData a = lite("lev1", {{"lite_light.slb", light_slb({{"good", 1, 2, 3, 0.5f, 10, 1, 1, 1, 1, 0x40},
                                                                      {"nanp", nan, 0, 0, 0.5f, 10, 1, 1, 1, 1, 0x40},
                                                                      {"infi", 0, 0, 0, inf, 10, 1, 1, 1, 1, 0x40},
                                                                      {"nanr", 0, 0, 0, 0.5f, nan, 1, 1, 1, 1, 0x40},
                                                                      {"nanc", 0, 0, -1, 0.5f, 0, 1, nan, 1, 1, 0x80}})},
                                         {"textures.slb", textures_slb({{"tfx", nan, 0, 0}, {"cut", 1, inf, 0}, {"red", 0.5f, 0, 0}})}});
  REQUIRE(a.lights.size() == 2);  // "good" + the level-1 default (the only directional record was skipped)
  CHECK(a.lights[0].name == "good");
  CHECK(a.lights_source == "light_slb+default");
  CHECK(has_warning(a, "light record 1: non-finite value (skipped)"));
  CHECK(has_warning(a, "light record 4: non-finite value (skipped)"));
  CHECK(a.special_textures == 1);  // the area blockfile's textures.slb (found before the level's): 2 records skipped
  CHECK(has_warning(a, "textures.slb record 0: non-finite value (skipped)"));
  CHECK(has_warning(a, "textures.slb record 1: non-finite value (skipped)"));
  // SetFog with a fold that overflows float32 (3e38 * 10 -> +inf): ignored with a warning, the default fog stays. The
  // fold itself used to be an undefined double -> float conversion (UBSan float-cast-overflow).
  std::vector<uint8_t> c;
  push_i32(c, 0xff102030u);
  push_f32(c, 3e38f);
  push_f32(c, 10.0f);
  c.push_back(0x72);  // Multiply
  push_f32(c, -10.0f);
  push_f32(c, 0.25f);
  c.insert(c.end(), {0x82, 0, 0, 1, 0, 4, 0x30, 0x47, 0x52});
  const Bytes osi = lite_osi("lego_area_lite", c);
  const view::AreaData f = lite("lev1", {}, {}, &osi);
  CHECK(f.fog.source == "default");
  CHECK(has_warning(f, "osi: SetFog with a non-finite or out-of-range value (ignored)"));
}

TEST_CASE("area: a point light's attenuation1 never divides by zero (denormal range -> FLT_MAX, Task 8B D3)") {
  // 0.15f * 1e-45 underflows to 0 in float: 1/0 was a float-divide-by-zero (UBSan). The game's x87 code keeps the
  // product in an 80-bit register (no underflow) and stores 1/x = +inf to the float; we store FLT_MAX (finite).
  const float denorm = std::numeric_limits<float>::denorm_min();
  const view::AreaData a = lite("lev1", {{"lite_light.slb", light_slb({{"tiny", 0, 0, 0, 0.5f, denorm, 1, 1, 1, 1, 0x40},
                                                                      {"norm", 0, 0, 0, 0.5f, 2, 1, 1, 1, 1, 0x40}})}});
  REQUIRE(a.lights.size() >= 2);
  CHECK(a.lights[0].light.attenuation1 == FLT_MAX);
  CHECK(a.lights[0].light.range == denorm);
  CHECK(a.lights[1].light.attenuation1 == 1.0f / (0.15f * 2.0f));  // normal ranges: the float formula, bit for bit
}

TEST_CASE("area: kMaxAreaLights is injectable (lower only), counts Shad/default lights and warns on a drop (Task 8B D4)") {
  view::AreaOptions o;
  o.max_lights = 2;
  const view::AreaData a = lite("lev1", {{"lite_light.slb", light_slb({{"pnt1", 0, 0, 0, 0.5f, 4, 1, 1, 1, 1, 0x40},
                                                                      {"dir1", 0, -1, 0, 0.5f, 0, 1, 1, 1, 1, 0x80}})}}, o);
  REQUIRE(a.lights.size() == 2);  // pnt1, dir1; the Shad fill is the third light
  CHECK(a.lights[1].name == "dir1");
  CHECK(has_warning(a, "lights: 1 over kMaxAreaLights dropped"));
  const view::AreaData d = lite("lev3", {{"lite_light.slb", light_slb({{"pnt1", 0, 0, 0, 0.5f, 4, 1, 1, 1, 1, 0x40}})}}, o);
  REQUIRE(d.lights.size() == 2);  // pnt1 + SunL; the default Shad crosses the cap
  CHECK(d.lights[1].name == "SunL");
  CHECK(has_warning(d, "lights: 1 over kMaxAreaLights dropped"));
  o.max_lights = view::kMaxAreaLights + 5;  // larger values are clamped to the default
  const view::AreaData e = lite("lev3", {}, o);
  CHECK(e.lights.size() == 2);
  CHECK_FALSE(has_warning(e, "kMaxAreaLights"));
}

TEST_CASE("area: textures.slb records are applied by name in table order, duplicates included (Task 8B D1)") {
  // The level table has {tfx 0.5}; this area's table (found first) has 1024 records = kMaxSpecTextures: filler names,
  // then two "tfx" records (the later one with trans < 1 wins; a (u, v) scroll from either applies).
  std::vector<T> ts;
  for (uint32_t i = 0; i < sage::kMaxSpecTextures - 2; ++i) ts.push_back({"f" + std::to_string(i), 0.5f, 0, 0});
  ts.push_back({"tfx", 0.75f, 0, 0.5f});
  ts.push_back({"tfx", 0.25f, 0, 0});
  fs::MemoryFileSystem syn;
  REQUIRE(view::mount_synthetic_install(syn));
  std::vector<uint8_t> state;
  REQUIRE(syn.read("Root/Data/Levels/levt/stat.blk", state) == fs::Error::None);
  // Re-mount the stat area with the big table added to its blockfile (entries kept sorted by name).
  sage::BlockFile bf;
  std::string err;
  REQUIRE_MESSAGE(bf.open_bytes(state, &err), err);
  std::vector<sage_test::BlkEntrySpec> es;
  bool added = false;
  for (size_t i = 0; i < bf.entries().size(); ++i) {
    const std::string n = bf.entries()[i].name;
    if (!added && n > "textures.slb") {
      const Bytes t = textures_slb(ts);
      es.push_back({"textures.slb", t, static_cast<uint32_t>(t.size()), 2});
      added = true;
    }
    std::vector<uint8_t> data;
    REQUIRE(bf.read(i, data, &err));
    es.push_back({n, data, static_cast<uint32_t>(data.size()), 2});
  }
  if (!added) {
    const Bytes t = textures_slb(ts);
    es.push_back({"textures.slb", t, static_cast<uint32_t>(t.size()), 2});
  }
  REQUIRE(syn.mount("Root/Data/Levels/levt/stat.blk", sage_test::make_blk(es)));
  view::AreaData a;
  REQUIRE_MESSAGE(view::load_area(syn, view::kSyntheticLevel, view::kSyntheticStateArea, view::AreaOptions(), a, &err), err);
  CHECK(a.special_textures == sage::kMaxSpecTextures);
  const view::SubsetDraw& tfx = subset(model(a, "mixm"), "tfx");
  CHECK(tfx.trans == 0.25f);
  CHECK(tfx.uv_offset[1] == 0.5f);
  CHECK(a.special_matches == 1);  // only "tfx": this table has no "cut"
}
