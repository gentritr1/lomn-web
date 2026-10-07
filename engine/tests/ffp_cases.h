// The fixed-function cases gpu_ffp draws (test_gpu_ffp.cpp), shared with lomn_ffp_cases (ffp_cases.cpp), which dumps
// them - plus seeded random samples - with the CPU reference's results for the independent Python check
// tools/research/oracle/ffp_oracle.py (REF3 revision 2, item 3).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "lomn/d3d/device8.h"
#include "lomn/view/math.h"

namespace lomn::test {

struct FfpCase {
  const char* what;
  d3d::State s;
  bool has_vcol;
  bool tex2x2 = false;  // sample kFfpTex2x2 (point) instead of the 1x1 white texture (texture-transform cases)
};

// 2x2 RGBA8 texture, row 0 = v in [0, 0.5): red, green; row 1: blue, "mixed" (128, 64, 192: Task 8B B4, a texel whose
// channels are not 0/1, so a modulate is a real product). Texel centres at u, v = 0.25 / 0.75.
constexpr uint8_t kFfpTex2x2[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 128, 64, 192, 255};
// Point sample of kFfpTex2x2 at uv (wrap addressing, the D3D8 default; nearest texel = floor(frac(uv) * 2)).
inline d3d::Color4 ffp_tex2x2_texel(const float uv[2]) {
  int t[2];
  for (int k = 0; k < 2; ++k) t[k] = std::min(1, static_cast<int>(std::floor((uv[k] - std::floor(uv[k])) * 2.0f)));
  const uint8_t* p = &kFfpTex2x2[(t[1] * 2 + t[0]) * 4];
  return d3d::Color4{p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f};
}

// One triangle (circumradius 3) around the origin in the z = 0.5 plane, normal (0,0,-1), vertex colour kFfpVcol.
constexpr float kFfpVcol[4] = {0.8f, 0.6f, 0.4f, 0.5f};
inline void ffp_triangle(float verts[3][12]) {
  for (int i = 0; i < 3; ++i) {
    const double a = 1.5707963 + i * 2.0943951;
    const float v[12] = {static_cast<float>(3 * std::cos(a)), static_cast<float>(3 * std::sin(a)), 0.5f, 0, 0, -1, 0.5f, 0.5f,
                         kFfpVcol[0], kFfpVcol[1], kFfpVcol[2], kFfpVcol[3]};
    for (int k = 0; k < 12; ++k) verts[i][k] = v[k];
  }
}

// Eye depth the fragment sees (clip w = (p * world * view * proj).w) at the triangle's vertex p. Every case keeps w the
// same at the three vertices (the triangle lies in a plane of constant view depth), so this is also the w at the centre
// pixel (Task 8B B4: the reference used to assume w = 1).
inline float ffp_eye_w(const d3d::State& s, const float p[3]) {
  float c[4];
  view::transform_point(view::mat_mul(view::mat_mul(s.world, s.view), s.proj), p, c);
  return c[3];
}

inline std::vector<FfpCase> gpu_ffp_cases() {
  using namespace d3d;
  std::vector<FfpCase> out;
  State base = default_state();
  base.rs[rs::ZENABLE] = kZbTrue;
  base.rs[rs::CULLMODE] = kCullNone;
  base.rs[rs::SRCBLEND] = kBlendSrcAlpha;
  base.rs[rs::DESTBLEND] = kBlendInvSrcAlpha;
  base.tss[0][tss::COLORARG2] = kTaDiffuse;
  base.tss[0][tss::MAGFILTER] = base.tss[0][tss::MINFILTER] = kTexfLinear;
  base.tss[0][tss::MIPFILTER] = kTexfLinear;

  State lit = base;  // LIGHTING on: emissive + ambient (global + lights) + diffuse (point light + directional)
  lit.rs[rs::LIGHTING] = 1;
  lit.rs[rs::AMBIENT] = 0xff1a1a26u;
  lit.material.ambient = {0.7f, 0.7f, 0.7f, 1};
  lit.material.emissive = {0.05f, 0.0f, 0.1f, 1};
  lit.material.diffuse = {0.2f, 0.9f, 0.3f, 1};
  Light8 pt;
  pt.type = kLightPoint;
  pt.position[2] = -1.5f;  // on the axis, |P - L| = sqrt(9 + 4) for every vertex
  pt.range = 10;
  pt.attenuation1 = 1.0f / (0.15f * 10.0f);
  pt.diffuse = {0.75f, 0.56f, 0.16f, 0};
  pt.ambient = {0.1f, 0.1f, 0.1f, 1};
  Light8 dl;
  dl.type = kLightDirectional;
  dl.direction[2] = 2;  // L = -dir = (0,0,-1) = the normal: N.L = 1
  dl.diffuse = {0.3f, 0.3f, 0.3f, 1};
  lit.lights[0] = pt;
  lit.lights[2] = dl;
  lit.light_on[0] = lit.light_on[2] = true;
  out.push_back({"lit: vertex colour as COLOR1 diffuse, point (attenuated) + directional + ambients", lit, true});
  out.push_back({"lit: no vertex colour -> material diffuse", lit, false});
  State lit_far = lit;
  lit_far.lights[0].range = 3.0f;  // every vertex is sqrt(13) > 3 away: the point light contributes nothing
  out.push_back({"lit: point light out of range", lit_far, true});
  State back = lit_far;  // a directional light from behind the surface: N.L = -1 is clamped to 0
  back.lights[2].direction[2] = -2;
  out.push_back({"lit: directional light behind the surface (N.L clamped to 0)", back, true});
  State unlit = base;
  unlit.rs[rs::LIGHTING] = 0;
  out.push_back({"LIGHTING off: vertex colour", unlit, true});
  out.push_back({"LIGHTING off, no vertex colour: opaque white", unlit, false});
  State fog = unlit;
  fog.rs[rs::FOGENABLE] = 1;
  fog.rs[rs::FOGTABLEMODE] = kFogLinear;
  fog.rs[rs::FOGSTART] = float_to_bits(0.0f);
  fog.rs[rs::FOGEND] = float_to_bits(4.0f);
  fog.rs[rs::FOGCOLOR] = 0xffff0000u;
  out.push_back({"fog LINEAR at w 1 (start 0, end 4): f 0.75", fog, true});
  fog.rs[rs::FOGTABLEMODE] = kFogExp;
  fog.rs[rs::FOGDENSITY] = float_to_bits(0.7f);
  out.push_back({"fog EXP density 0.7 at w 1", fog, true});
  fog.rs[rs::FOGTABLEMODE] = kFogExp2;
  out.push_back({"fog EXP2 density 0.7 at w 1", fog, true});
  State test = unlit;  // alpha test only (GcArea::Render's plnt/tre1 context flag: DrawX turns blending off), TFACTOR alpha
  test.rs[rs::ALPHABLENDENABLE] = 0;
  test.rs[rs::ALPHATESTENABLE] = 1;
  test.rs[rs::ALPHAFUNC] = kCmpGreater;
  test.rs[rs::ALPHAREF] = 1;
  test.tss[0][tss::ALPHAOP] = kTopSelectArg2;
  test.tss[0][tss::ALPHAARG2] = kTaTFactor;
  test.rs[rs::TEXTUREFACTOR] = 0x01000000u;  // alpha 1: 1 > 1 fails -> discarded
  out.push_back({"alpha test GREATER ref 1, alpha 1: discarded", test, true});
  test.rs[rs::TEXTUREFACTOR] = 0x02000000u;
  out.push_back({"alpha test GREATER ref 1, alpha 2: kept (unblended)", test, true});
  test.rs[rs::ALPHAREF] = 0;
  test.rs[rs::TEXTUREFACTOR] = 0x01000000u;
  out.push_back({"alpha test GREATER ref 0, alpha 1: kept (R13: ALPHAREF 0 keeps alpha 1/255)", test, true});
  test.rs[rs::ALPHABLENDENABLE] = 1;
  test.rs[rs::ALPHATESTENABLE] = 0;
  test.rs[rs::TEXTUREFACTOR] = 0xb2000000u;  // 178 = round(255 * 0.7): the vllg river's textures.slb trans
  out.push_back({"TFACTOR alpha 178 blended", test, true});
  State mod = unlit;  // vertex alpha through ALPHAOP MODULATE(TFACTOR, TEXTURE) (DrawX's mTransparency < 1 path)
  mod.rs[rs::ALPHABLENDENABLE] = 1;
  mod.tss[0][tss::ALPHAOP] = kTopModulate;
  mod.tss[0][tss::ALPHAARG1] = kTaTFactor;
  mod.tss[0][tss::ALPHAARG2] = kTaTexture;
  mod.rs[rs::TEXTUREFACTOR] = 0x80000000u;
  out.push_back({"ALPHAOP MODULATE(TFACTOR 128, TEXTURE 255)", mod, true});
  // Task 8A B1: TEXTURETRANSFORMFLAGS COUNT2 on the vertices' 2-D uv (0.5, 0.5), 2x2 point-sampled texture. D3D8 computes
  // (u, v, 1, 0) * TEXTURE0: scale 0.5 -> (0.25, 0.25) = red; a translation in _31/_32 moves the texel, the same value in
  // _41/_42 (row 4) must not (a (u, v, 1, 1) pad would move it).
  State tt = unlit;
  tt.tss[0][tss::MAGFILTER] = tt.tss[0][tss::MINFILTER] = kTexfPoint;
  tt.tss[0][tss::MIPFILTER] = kTexfPoint;  // one level: no mip effect (MIPFILTER NONE is flagged unsupported)
  tt.tss[0][tss::TEXTURETRANSFORMFLAGS] = kTtfCount2;
  tt.tex[0] = view::mat_identity();
  tt.tex[0].m[0] = tt.tex[0].m[5] = 0.5f;
  out.push_back({"COUNT2 scale 0.5: uv (0.25, 0.25) -> red", tt, false, true});
  State t31 = tt;
  t31.tex[0].m[8] = 0.5f;  // _31
  out.push_back({"COUNT2 + _31 0.5: u 0.75 -> green", t31, false, true});
  State t32 = tt;
  t32.tex[0].m[9] = 0.5f;  // _32
  out.push_back({"COUNT2 + _32 0.5: v 0.75 -> blue", t32, false, true});
  State t41 = tt;
  t41.tex[0].m[12] = 0.5f;  // _41: ignored for 2-D coordinates
  out.push_back({"COUNT2 + _41 0.5: ignored -> red", t41, false, true});
  State t42 = tt;
  t42.tex[0].m[13] = 0.5f;  // _42: ignored
  out.push_back({"COUNT2 + _42 0.5: ignored -> red", t42, false, true});
  // Task 8B B4: fold probes that a symmetric identity setup cannot tell apart.
  // (A) A non-identity, asymmetric world: RotX(0.5) (row-vector rows (1,0,0), (0,c,s), (0,-s,c)) + a translation. The
  //     normal (0,0,-1) becomes (0, s, -c); against a directional light along (0,-1,1) N.L = (s + c)/sqrt2 = 0.96. A
  //     transposed world (rows/columns swapped in the shader) gives (c - s)/sqrt2 = 0.28 and moves w off 1.
  State wr = base;
  wr.rs[rs::LIGHTING] = 1;
  wr.material.diffuse = {0.2f, 0.9f, 0.3f, 1};
  wr.material.ambient = {0, 0, 0, 1};
  {
    const float c = std::cos(0.5f), sn = std::sin(0.5f);
    wr.world = view::mat_identity();
    wr.world.m[5] = c;
    wr.world.m[6] = sn;
    wr.world.m[9] = -sn;
    wr.world.m[10] = c;
    wr.world.m[12] = 0.1f;   // _41
    wr.world.m[13] = -0.05f; // _42
    wr.world.m[14] = 0.02f;  // _43
  }
  Light8 wl;
  wl.type = kLightDirectional;
  wl.direction[1] = -1;
  wl.direction[2] = 1;
  wl.diffuse = {1, 1, 1, 1};
  wr.lights[0] = wl;
  wr.light_on[0] = true;
  out.push_back({"world RotX(0.5) + translation, directional (0,-1,1): N.L 0.96 (transposed: 0.28)", wr, false});
  // (B) Fog at an eye depth w != 1: a perspective projection (_33 0.5, _34 1, _44 0) puts the z = 0.5 plane at w = 0.5.
  //     LINEAR start 0, end 1: f = 0.5 (w = 1 would give 0, the clip z 0.25 would give 0.75).
  State fw = unlit;
  fw.proj = view::mat_identity();
  fw.proj.m[10] = 0.5f;  // _33
  fw.proj.m[11] = 1.0f;  // _34
  fw.proj.m[15] = 0.0f;  // _44
  fw.rs[rs::FOGENABLE] = 1;
  fw.rs[rs::FOGTABLEMODE] = kFogLinear;
  fw.rs[rs::FOGSTART] = float_to_bits(0.0f);
  fw.rs[rs::FOGEND] = float_to_bits(1.0f);
  fw.rs[rs::FOGCOLOR] = 0xffff0000u;
  out.push_back({"fog LINEAR (start 0, end 1) at w 0.5 (perspective proj): f 0.5", fw, true});
  // (C) A non-white texel under MODULATE(TEXTURE, DIFFUSE): kFfpTex2x2's mixed texel (COUNT2 scale 0.5 + _31 0.5 + _32
  //     0.5 -> uv (0.75, 0.75)) times the vertex colour (0.8, 0.6, 0.4) = (0.40, 0.15, 0.30). Every earlier case had a
  //     white texture or a white diffuse with 0/1 texels, where TEXTURE and DIFFUSE (or a product and a min) agree.
  State md = t31;
  md.tex[0].m[9] = 0.5f;  // _32
  md.tss[0][tss::COLOROP] = kTopModulate;
  md.tss[0][tss::COLORARG1] = kTaTexture;
  md.tss[0][tss::COLORARG2] = kTaDiffuse;
  out.push_back({"MODULATE(TEXTURE mixed texel, DIFFUSE vertex colour) -> (0.40, 0.15, 0.30)", md, true, true});
  return out;
}

}  // namespace lomn::test
