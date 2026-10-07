// The fixed-function cases gpu_ffp draws (test_gpu_ffp.cpp), shared with lomn_ffp_cases (ffp_cases.cpp), which dumps
// them - plus seeded random samples - with the CPU reference's results for the independent Python check
// tools/research/oracle/ffp_oracle.py (REF3 revision 2, item 3).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "lomn/d3d/device8.h"

namespace lomn::test {

struct FfpCase {
  const char* what;
  d3d::State s;
  bool has_vcol;
  bool tex2x2 = false;  // sample kFfpTex2x2 (point) instead of the 1x1 white texture (texture-transform cases)
};

// 2x2 RGBA8 texture, row 0 = v in [0, 0.5): red, green; row 1: blue, white. Texel centres at u, v = 0.25 / 0.75.
constexpr uint8_t kFfpTex2x2[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
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
  return out;
}

}  // namespace lomn::test
