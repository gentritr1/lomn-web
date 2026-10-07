// M1a-3: the D3D8 state shim (lomn/d3d/device8.h) and its compiler to rhi (lomn/d3d/ffp.h): defaults, the ScDxStates
// filter, pipeline keys, the uniform layout and the CPU reference of the fixed-function formulas. The SAGE frame replay
// is tested in test_sage_frame.cpp, the M1a-3 area-loader additions in test_area_states.cpp.
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "doctest.h"
#include "lomn/d3d/device8.h"
#include "lomn/d3d/ffp.h"

using namespace lomn;
using namespace lomn::d3d;

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

TEST_CASE("d3d: hostile floats never reach an undefined float->int conversion (UBSan finding)") {
  CHECK(quantize_unorm8(std::nanf("")) == 0.0f);
  CHECK(quantize_unorm8(std::numeric_limits<float>::infinity()) == 1.0f);
  CHECK(quantize_unorm8(-std::numeric_limits<float>::infinity()) == 0.0f);
  CHECK(quantize_unorm8(0.5f) == 128.0f / 255.0f);
}

TEST_CASE("d3d: the state log prints NaN and infinities identically natively and as wasm32 (Task 8A B2)") {
  // %.6g leaves NaN text to the C library: macOS prints "nan" for 0xffc00000 (the x87 default NaN), emscripten/musl
  // "-nan", which broke the native == wasm32 log contract on hostile floats. Rule: every NaN (any sign, any payload)
  // prints "nan"; +-infinity print "inf" / "-inf"; finite values keep %.6g (so valid logs are byte-identical).
  const float qnan_neg = bits_to_float(0xffc00000u), qnan_pos = bits_to_float(0x7fc00001u);
  const float inf = bits_to_float(0x7f800000u), ninf = bits_to_float(0xff800000u);
  std::vector<std::string> log;
  Device8 d(&log);
  Material8 m;
  m.diffuse = {qnan_neg, qnan_pos, inf, ninf};
  m.ambient = {0.7f, 0.7f, 0.7f, 1};
  m.specular = {-0.0f, 1e-7f, 123456789.0f, 0.5f};
  m.power = qnan_neg;
  d.set_material(m);
  d.set_rs(rs::FOGSTART, 0xffc00000u);
  d.set_rs(rs::FOGEND, 0xff800000u);
  Light8 l;
  l.type = kLightPoint;
  l.position[0] = qnan_neg;
  l.direction[1] = ninf;
  l.range = inf;
  l.attenuation1 = qnan_pos;
  d.set_light(0, l);
  d.clear(kClearTarget, 0, qnan_neg, false);
  CHECK(log == std::vector<std::string>{
                   "MATERIAL d=(nan,nan,inf,-inf) a=(0.7,0.7,0.7,1) s=(-0,1e-07,1.23457e+08,0.5) e=(0,0,0,0) p=nan",
                   "RS FOGSTART 0xffc00000 (nan)", "RS FOGEND 0xff800000 (-inf)",
                   "LIGHT 0 type=1 d=(0,0,0,0) s=(0,0,0,0) a=(0,0,0,0) pos=(nan,0,0) dir=(0,-inf,0) range=inf att=(0,nan,0)",
                   "CLEAR full flags=1 color=0x00000000 z=nan"});
}
