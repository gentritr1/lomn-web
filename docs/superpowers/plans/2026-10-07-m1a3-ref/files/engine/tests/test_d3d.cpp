// M1a-3: the D3D8 state shim (lomn/d3d/device8.h), its compiler (lomn/d3d/ffp.h) and the SAGE frame replay
// (lomn/view/sage_frame.h). Expected values come from the decompiled functions named in each case.
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "doctest.h"
#include "lomn/d3d/device8.h"
#include "lomn/d3d/ffp.h"
#include "lomn/fs.h"
#include "lomn/rhi.h"
#include "lomn/view/area.h"
#include "lomn/view/sage_frame.h"
#include "lomn/view/synthetic.h"

using namespace lomn;
using namespace lomn::d3d;

namespace {

std::vector<std::string> state_lines(const std::vector<std::string>& log) {
  std::vector<std::string> o;
  for (const auto& l : log)
    if (l.rfind("# ", 0) != 0) o.push_back(l);
  return o;
}

view::AreaData synthetic_area() {
  fs::MemoryFileSystem mfs;
  REQUIRE(view::mount_synthetic_install(mfs));
  view::AreaData a;
  std::string err;
  REQUIRE_MESSAGE(view::load_area(mfs, view::kSyntheticLevel, view::kSyntheticStateArea, view::AreaOptions(), a, &err), err);
  return a;
}

}  // namespace

TEST_CASE("d3d: defaults are the D3D8 ones the area path reads") {
  const State s = default_state();
  CHECK(s.rs[rs::LIGHTING] == 1);
  CHECK(s.rs[rs::CULLMODE] == kCullCCW);
  CHECK(s.rs[rs::ZFUNC] == kCmpLessEqual);
  CHECK(s.rs[rs::ALPHAREF] == 0);
  CHECK(s.rs[rs::ALPHAFUNC] == kCmpAlways);
  CHECK(s.rs[rs::SRCBLEND] == kBlendOne);
  CHECK(s.rs[rs::DESTBLEND] == kBlendZero);
  CHECK(s.rs[rs::TEXTUREFACTOR] == 0xffffffffu);
  CHECK(s.rs[rs::DIFFUSEMATERIALSOURCE] == kMcsColor1);
  CHECK(s.rs[rs::COLORVERTEX] == 1);
  CHECK(bits_to_float(s.rs[rs::FOGEND]) == 1.0f);
  CHECK(s.tss[0][tss::MAXANISOTROPY] == 1);  // never set by the exe
  CHECK(s.tss[0][tss::COLOROP] == kTopModulate);
  CHECK(s.tss[1][tss::COLOROP] == kTopDisable);
}

TEST_CASE("d3d: ScDxStates forwards only changes; direct calls are always logged") {
  std::vector<std::string> log;
  Device8 d(&log);
  d.dx_set_rs(rs::ZBIAS, 0);  // already 0: filtered
  d.dx_set_rs(rs::ZBIAS, 16);
  d.dx_set_rs(rs::ZBIAS, 16);
  d.set_tss(0, tss::MAGFILTER, kTexfLinear);
  d.set_tss(0, tss::MAGFILTER, kTexfLinear);  // GcArea::Render's direct call: logged every time
  d.dx_set_tss(0, tss::MAGFILTER, kTexfLinear);
  d.set_rs(rs::FOGSTART, float_to_bits(40.0f));
  CHECK(log == std::vector<std::string>{"RS ZBIAS 16", "TSS0 MAGFILTER 2", "TSS0 MAGFILTER 2", "RS FOGSTART 0x42200000 (40)"});
  CHECK(d.state_calls() == 4);
}

TEST_CASE("d3d: pipeline keys map D3D cull/z/blend/zbias; samplers map ANISOTROPIC x1 to linear") {
  State s = default_state();
  uint32_t u = 0;
  s.rs[rs::CULLMODE] = kCullCW;
  CHECK(pipeline_key(s, &u).cull == rhi::CullMode::Front);  // front face CW (rhi), so D3DCULL_CW culls front faces
  s.rs[rs::CULLMODE] = kCullCCW;
  CHECK(pipeline_key(s, &u).cull == rhi::CullMode::Back);
  s.rs[rs::CULLMODE] = kCullNone;
  CHECK(pipeline_key(s, &u).cull == rhi::CullMode::None);
  s.rs[rs::ZENABLE] = kZbFalse;
  CHECK(pipeline_key(s, &u).depth_compare == rhi::CompareFunc::Always);
  CHECK_FALSE(pipeline_key(s, &u).depth_write);
  s.rs[rs::ZENABLE] = kZbUseW;
  s.rs[rs::ZFUNC] = kCmpLessEqual;
  CHECK(pipeline_key(s, &u).depth_compare == rhi::CompareFunc::LessEqual);
  s.rs[rs::ZBIAS] = 16;
  CHECK(pipeline_key(s, &u).depth_bias == -16);
  CHECK(u == 0);
  s.rs[rs::ALPHABLENDENABLE] = 1;  // with the D3D8 default ONE/ZERO factors: not reproducible
  pipeline_key(s, &u);
  CHECK((u & kUnsupBlendFactors) != 0);
  s.tss[0][tss::MAGFILTER] = s.tss[0][tss::MINFILTER] = kTexfAnisotropic;
  s.tss[0][tss::MIPFILTER] = kTexfLinear;
  rhi::SamplerDesc sd = sampler_desc(s, &u);
  CHECK(sd.mag == rhi::FilterMode::Linear);
  CHECK(sd.min == rhi::FilterMode::Linear);
  CHECK(sd.max_anisotropy == 1);  // D3DTSS_MAXANISOTROPY 1: the exe never sets it
  s.tss[0][tss::MAGFILTER] = kTexfLinear;  // GcArea::Render's MAG LINEAR + MIN ANISOTROPIC
  s.tss[0][tss::MAXANISOTROPY] = 4;
  CHECK(sampler_desc(s, &u).max_anisotropy == 1);  // WebGPU has one anisotropy for min and mag
  const rhi::MeshPipelineDesc pd = pipeline_desc(pipeline_key(s, &u));
  CHECK(pd.front_face == rhi::FrontFace::CW);
  CHECK(pd.uniform_bytes == kFfpUniformBytes);
  CHECK(pd.fs_entry == "fs");
}

TEST_CASE("d3d: per-draw uniform block layout (A10: the lit colour source is the vertex DIFFUSE)") {
  State s = default_state();
  s.rs[rs::LIGHTING] = 0;
  float u[kFfpUniformBytes / 4];
  DrawInputs in;
  in.vertex_has_diffuse = true;
  in.texture_bound = true;
  pack_uniforms(s, in, u);
  CHECK(u[76] == 0.0f);  // lighting off
  CHECK(u[77] == 1.0f);  // vertex has DIFFUSE -> used as the colour
  CHECK(u[78] == 1.0f);  // COLORVERTEX + DIFFUSEMATERIALSOURCE COLOR1
  CHECK(u[90] == 1.0f);  // texture bound
  in.vertex_has_diffuse = false;
  pack_uniforms(s, in, u);
  CHECK(u[77] == 0.0f);
  CHECK(u[78] == 0.0f);
  s.rs[rs::LIGHTING] = 1;
  s.lights[3].type = kLightPoint;
  s.lights[3].range = 7;
  s.light_on[3] = true;
  pack_uniforms(s, in, u);
  CHECK(u[76] == 1.0f);
  CHECK(u[91] == 1.0f);   // one enabled light, packed first
  CHECK(u[96 + 11] == float(kLightPoint));
  CHECK(u[96 + 15] == 7.0f);
}

TEST_CASE("d3d: reference lighting = D3D8 fixed-function formulas") {
  State s = default_state();
  s.rs[rs::LIGHTING] = 1;
  s.rs[rs::AMBIENT] = 0xff202020u;  // 32/255
  s.material.ambient = {0.5f, 0.5f, 0.5f, 1};
  s.material.diffuse = {0.25f, 0.5f, 1.0f, 0.75f};
  s.material.emissive = {0.1f, 0, 0, 1};
  Light8 dir;
  dir.type = kLightDirectional;
  dir.direction[1] = -2;  // pointing down; L = +y
  dir.diffuse = {1, 1, 1, 1};
  s.lights[0] = dir;
  s.light_on[0] = true;
  const float p[3] = {0, 0, 0}, n[3] = {0, 1, 0};
  Color4 c = reference_vertex_color(s, false, Color4{}, p, n);
  const float ga = 32.0f / 255.0f;
  CHECK(c.r == doctest::Approx(0.1f + 0.5f * ga + 0.25f));
  CHECK(c.g == doctest::Approx(0.5f * ga + 0.5f));
  CHECK(c.b == doctest::Approx(std::min(1.0f, 0.5f * ga + 1.0f)));
  CHECK(c.a == 0.75f);  // alpha = diffuse alpha
  // Vertex colour replaces the material diffuse (COLOR1).
  c = reference_vertex_color(s, true, Color4{1, 0, 0, 0.5f}, p, n);
  CHECK(c.g == doctest::Approx(0.5f * ga));
  CHECK(c.a == 0.5f);
  // Point light: attenuation 1/(a0 + a1 d + a2 d^2), nothing beyond Range.
  s.light_on[0] = false;
  Light8 pt;
  pt.type = kLightPoint;
  pt.position[1] = 4;
  pt.range = 5;
  pt.attenuation1 = 0.5f;  // atten = 1 / (0.5 * 4) = 0.5
  pt.diffuse = {1, 1, 1, 1};
  pt.ambient = {0.2f, 0.2f, 0.2f, 1};
  s.lights[1] = pt;
  s.light_on[1] = true;
  s.material.emissive = {0, 0, 0, 1};
  c = reference_vertex_color(s, false, Color4{}, p, n);
  CHECK(c.r == doctest::Approx(0.5f * (ga + 0.2f * 0.5f) + 0.25f * 0.5f));
  s.lights[1].range = 3.9f;  // out of range: only the global ambient
  c = reference_vertex_color(s, false, Color4{}, p, n);
  CHECK(c.r == doctest::Approx(0.5f * ga));
  // LIGHTING off: vertex colour, else opaque white.
  s.rs[rs::LIGHTING] = 0;
  c = reference_vertex_color(s, false, Color4{}, p, n);
  CHECK((c.r == 1 && c.g == 1 && c.b == 1 && c.a == 1));
}

TEST_CASE("d3d: reference fragment: stage 0 ops, ALPHAREF GREATER, linear W fog") {
  State s = default_state();
  s.tss[0][tss::ALPHAOP] = kTopSelectArg2;
  s.tss[0][tss::ALPHAARG2] = kTaTexture;
  s.rs[rs::ALPHATESTENABLE] = 1;
  s.rs[rs::ALPHAFUNC] = kCmpGreater;
  s.rs[rs::ALPHAREF] = 1;
  s.rs[rs::ALPHABLENDENABLE] = 1;
  Color4 out;
  CHECK_FALSE(reference_fragment(s, Color4{1, 1, 1, 1}, Color4{1, 1, 1, 1.0f / 255.0f}, true, 1, out));  // 1 > 1 fails
  CHECK(reference_fragment(s, Color4{1, 1, 1, 1}, Color4{1, 1, 1, 2.0f / 255.0f}, true, 1, out));
  s.rs[rs::ALPHAREF] = 0;
  CHECK(reference_fragment(s, Color4{1, 1, 1, 1}, Color4{1, 1, 1, 1.0f / 255.0f}, true, 1, out));  // R13: ref 0 passes
  s.rs[rs::ALPHATESTENABLE] = 0;
  s.rs[rs::FOGENABLE] = 1;
  s.rs[rs::FOGTABLEMODE] = kFogLinear;
  s.rs[rs::FOGSTART] = float_to_bits(10);
  s.rs[rs::FOGEND] = float_to_bits(20);
  s.rs[rs::FOGCOLOR] = 0xff0000ffu;
  REQUIRE(reference_fragment(s, Color4{1, 1, 1, 1}, Color4{1, 0, 0, 1}, true, 15, out));
  CHECK(out.r == doctest::Approx(0.5f));
  CHECK(out.b == doctest::Approx(0.5f));
  REQUIRE(reference_fragment(s, Color4{1, 1, 1, 1}, Color4{1, 0, 0, 1}, true, 25, out));
  CHECK(out.r == doctest::Approx(0.0f));  // beyond FOGEND: all fog
}

TEST_CASE("sage: SrPushRenderMode carries ALPHAREF across the HUD's 0x80000 mode (R13 history)") {
  std::vector<std::string> log;
  Device8 d(&log);
  view::SageFrame f(d, view::SageCaps(), view::SageFrameOptions());
  f.push_render_mode(view::kModeMainView);  // from gLastRenderMode 0: nothing about ALPHAREF
  CHECK(d.get_rs(rs::ALPHAREF) == 0);
  CHECK(d.get_rs(rs::LIGHTING) == 1);
  CHECK(d.get_rs(rs::CULLMODE) == kCullCW);
  f.push_render_mode(view::kModeHud);  // entering 0x80000: ALPHAREF 0, blend + test on, z write off
  CHECK(d.get_rs(rs::ALPHAREF) == 0);
  CHECK(d.get_rs(rs::ALPHABLENDENABLE) == 1);
  CHECK(d.get_rs(rs::ZWRITEENABLE) == 0);
  f.push_render_mode(view::kModeArea);  // leaving it: ALPHAREF 1
  CHECK(d.get_rs(rs::ALPHAREF) == 1);
  CHECK(d.get_rs(rs::ALPHABLENDENABLE) == 0);
  CHECK(d.get_rs(rs::ZWRITEENABLE) == 1);
  CHECK(d.get_rs(rs::LIGHTING) == 0);  // 0x14634 bit 0 clear: the area nodes are unlit
  CHECK(d.get_rs(rs::ZENABLE) == kZbUseW);
  CHECK(d.get_rs(rs::FILLMODE) == kFillSolid);
  CHECK(d.get_rs(rs::FOGENABLE) == 1);  // 0x200
  f.push_render_mode(view::kModeTrans);  // 0x10034: no 0x200 -> fog off, cull none
  CHECK(d.get_rs(rs::FOGENABLE) == 0);
  CHECK(d.get_rs(rs::CULLMODE) == kCullNone);
}

TEST_CASE("sage: SetFog writes the statics' bit patterns once, then only toggles FOGENABLE") {
  std::vector<std::string> log;
  Device8 d(&log);
  view::SageFrame f(d, view::SageCaps(), view::SageFrameOptions());
  f.set_fog_value(0xfff9dcf9u, 40.0f, 1200.0f, 0.05f);
  f.set_fog(true);
  f.set_fog(true);  // mFogOn already set: returns early
  f.set_fog(false);
  f.set_fog(true);
  // FOGTABLEMODE: the D3D8 default is NONE; SetFog writes LINEAR (AppMain did too, on the real device).
  const std::vector<std::string> want = {"RS FOGENABLE 1",       "RS FOGTABLEMODE 3", "RS FOGCOLOR 0xfff9dcf9",         "RS FOGSTART 0x42200000 (40)",
                                         "RS FOGEND 0x44960000 (1200)", "RS FOGDENSITY 0x3d4ccccd (0.05)", "RS FOGENABLE 0",
                                         "RS FOGENABLE 1"};
  CHECK(state_lines(log) == want);
}

TEST_CASE("sage: DrawX two passes; plnt sets the context flag (alpha test only) and turns fog off afterwards (R14)") {
  view::AreaData a = synthetic_area();
  std::vector<std::string> log;
  Device8 d(&log);
  view::SageFrame f(d, view::SageCaps(), view::SageFrameOptions());
  f.push_render_mode(view::kModeArea);
  uint32_t quad = 0, plnt = 0;
  for (uint32_t i = 0; i < a.placements.size(); ++i) {
    const std::string& id = a.models[a.placements[i].model].id;
    if (id == "quad" && a.placements[i].route == sage::Route::Main) quad = i;
    if (id == "plnt") plnt = i;
  }
  log.clear();
  f.area_render(a, quad);
  std::vector<std::string> l = state_lines(log);
  // quad: red (opaque), nothere (opaque, NULL texture), then cut in pass 2 with blend + GREATER test.
  auto at = [&](const std::string& s) { return std::find(l.begin(), l.end(), s) - l.begin(); };
  REQUIRE(at("DRAW quad#0 attrib=0") < at("DRAW quad#0 attrib=2"));
  REQUIRE(at("DRAW quad#0 attrib=2") < at("RS ALPHATESTENABLE 1"));
  CHECK(at("RS ALPHABLENDENABLE 1") < at("DRAW quad#0 attrib=1"));
  CHECK(at("RS ALPHAFUNC 5") < at("DRAW quad#0 attrib=1"));
  CHECK(at("RS ZBIAS 16") > at("DRAW quad#0 attrib=1"));  // left at 16 after the alpha pass
  CHECK(l.back() == "TEXTURE 1 NULL");
  CHECK(d.get_rs(rs::FOGENABLE) == 1);
  log.clear();
  f.area_render(a, plnt);
  l = state_lines(log);
  // ALPHABLENDENABLE goes 1 then back to 0 before the alpha subset is drawn; fog is off after the model.
  const auto draw = at("DRAW plnt#6 attrib=0");
  REQUIRE(draw < static_cast<std::ptrdiff_t>(l.size()));
  std::string last_blend;
  for (std::ptrdiff_t k = 0; k < draw; ++k)
    if (l[k].rfind("RS ALPHABLENDENABLE", 0) == 0) last_blend = l[k];
  CHECK(last_blend == "RS ALPHABLENDENABLE 0");
  CHECK(l.back() == "RS FOGENABLE 0");
  CHECK(d.get_rs(rs::FOGENABLE) == 0);
  CHECK_FALSE(f.context_flag());
}

TEST_CASE("sage: a model with mTransparency < 1 gets ZBIAS 16 and a TFACTOR alpha (DrawX prologue)") {
  view::AreaData a = synthetic_area();
  std::vector<std::string> log;
  Device8 d(&log);
  view::SageFrame f(d, view::SageCaps(), view::SageFrameOptions());
  f.draw_x(a, 0, 0.5f);
  const std::vector<std::string> l = state_lines(log);
  REQUIRE(l.size() > 6);
  CHECK(l[0].rfind("XFORM WORLD", 0) == 0);  // SOFTWAREVERTEXPROCESSING 0 is the default: ScDxStates filters it
  CHECK(l[1] == "RS ZBIAS 16");
  CHECK(l[2] == "RS ALPHABLENDENABLE 1");
  CHECK(l[3] == "TSS0 ALPHAOP 4");
  CHECK(l[4] == "TSS0 ALPHAARG1 3");
  CHECK(l[5] == "TSS0 ALPHAARG2 2");  // D3D8 default CURRENT -> TEXTURE
  CHECK(l[6] == "RS TEXTUREFACTOR 0x80000000");
}

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

TEST_CASE("area: A10 - every drawn subset of a coloured model feeds its vertex DIFFUSE to the shader") {
  view::AreaData a = synthetic_area();
  std::vector<std::string> calls;
  auto dev = rhi::create_null_device(&calls);
  rhi::TextureId rt = dev->create_render_target_ex(rhi::RenderTargetDesc{16, 16, true, 1.0f});
  view::AreaRenderer r;
  std::string err;
  REQUIRE_MESSAGE(r.upload(*dev, a, view::RenderOptions(), &err), err);
  REQUIRE(r.render(*dev, rt, a, view::make_camera(a, view::synthetic_camera(), 1.0f)));
  // The vertex layout carries the colour at @location(3) (48-byte stride, offset 32).
  bool layout = false;
  for (const auto& l : calls)
    if (l.rfind("create_mesh_pipeline", 0) == 0) layout = l.find("3:f32x4@32") != std::string::npos;
  CHECK(layout);
  CHECK(std::string(view::area_wgsl()).find("@location(3) c: vec4f") != std::string::npos);
  CHECK(std::string(view::area_wgsl()).find("select(vec4f(1.0), c, u.a.y > 0.5)") != std::string::npos);
  CHECK(r.last_stats().rhi_draws == 19);
}

TEST_CASE("d3d: hostile floats never reach an undefined float->int conversion (UBSan finding)") {
  CHECK(quantize_unorm8(std::nanf("")) == 0.0f);
  CHECK(quantize_unorm8(std::numeric_limits<float>::infinity()) == 1.0f);
  CHECK(quantize_unorm8(-std::numeric_limits<float>::infinity()) == 0.0f);
  CHECK(quantize_unorm8(0.5f) == 128.0f / 255.0f);
}
