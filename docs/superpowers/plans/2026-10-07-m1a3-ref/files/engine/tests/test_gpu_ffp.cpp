// M1a-3 on Dawn: the fixed-function uber-shader (lomn/d3d/ffp.h) against its CPU reference of the D3D8 formulas.
// Each case builds a d3d::State, compiles it (pipeline_key / pipeline_desc / sampler_desc / pack_uniforms), draws one
// triangle covering the 8x8 target's centre with constant per-vertex inputs (the three vertices are equidistant from
// the point light, so the per-vertex lighting is the same at every vertex), and compares the centre pixel with
// reference_vertex_color + reference_fragment (+-2 per channel; blending against the clear colour computed here).
// Exits 77 (ctest SKIP) without an adapter.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "lomn/d3d/device8.h"
#include "lomn/d3d/ffp.h"
#include "lomn/rhi.h"

using namespace lomn;
using namespace lomn::d3d;

int main() {
  std::unique_ptr<rhi::Device> dev;
  std::string err;
  rhi::create_webgpu_device(nullptr, 0, 0, [&](std::unique_ptr<rhi::Device> d, std::string e) {
    dev = std::move(d);
    err = std::move(e);
  });
  if (!dev) {
    std::printf("SKIPPED (no WebGPU adapter: %s)\n", err.c_str());
    return 77;
  }
  // Equilateral triangle (circumradius 3) around the origin in the z = 0.5 plane, normal (0,0,-1), vertex colour vc.
  const float vc[4] = {0.8f, 0.6f, 0.4f, 0.5f};
  float verts[3][12];
  for (int i = 0; i < 3; ++i) {
    const double a = 1.5707963 + i * 2.0943951;
    const float v[12] = {static_cast<float>(3 * std::cos(a)), static_cast<float>(3 * std::sin(a)), 0.5f, 0, 0, -1, 0.5f, 0.5f,
                         vc[0], vc[1], vc[2], vc[3]};
    for (int k = 0; k < 12; ++k) verts[i][k] = v[k];
  }
  const uint32_t idx[3] = {0, 1, 2};
  rhi::BufferId vb = dev->create_vertex_buffer(sizeof verts), ib = dev->create_index_buffer(sizeof idx);
  dev->write_buffer(vb, 0, verts, sizeof verts);
  dev->write_buffer(ib, 0, idx, sizeof idx);
  const uint8_t white[4] = {255, 255, 255, 255};
  rhi::TextureId tex = dev->create_texture(1, 1, 1);
  dev->write_texture(tex, 0, white, 4);
  rhi::TextureId rt = dev->create_render_target_ex(rhi::RenderTargetDesc{8, 8, true, 1.0f});
  if (!vb || !ib || !tex || !rt) return std::printf("FAIL setup\n"), 1;
  const float clear[4] = {0.0f, 0.25f, 0.0f, 1.0f};

  bool ok = true;
  auto run = [&](const char* what, const State& s, bool has_vcol) {
    uint32_t unsup = 0;
    const PipelineKey key = pipeline_key(s, &unsup);
    rhi::PipelineId p = dev->create_mesh_pipeline(pipeline_desc(key));
    rhi::SamplerId sm = dev->create_sampler(sampler_desc(s, &unsup));
    rhi::BufferId ub = dev->create_uniform_buffer(kFfpUniformBytes);
    rhi::BindGroupId bg = dev->create_bind_group({ub, kFfpUniformBytes, tex, sm});
    float u[kFfpUniformBytes / 4];
    DrawInputs in;
    in.vertex_has_diffuse = has_vcol;
    in.texture_bound = true;
    pack_uniforms(s, in, u);
    dev->write_buffer(ub, 0, u, kFfpUniformBytes);
    dev->begin_frame(rt, clear);
    dev->draw_indexed({p, vb, ib, 0, 3, bg, 0});
    dev->end_frame();
    std::vector<uint8_t> px;
    if (!p || !bg || !dev->read_pixels(rt, px)) {
      std::printf("FAIL %s: setup\n", what);
      ok = false;
      return;
    }
    // Reference: lit colour at a vertex (all equal), then stage 0 / test / fog at the centre (w = 1), then blend.
    const float pos[3] = {verts[0][0], verts[0][1], verts[0][2]}, nrm[3] = {0, 0, -1};
    const Color4 d = reference_vertex_color(s, has_vcol, Color4{vc[0], vc[1], vc[2], vc[3]}, pos, nrm);
    Color4 f;
    float want[3] = {clear[0], clear[1], clear[2]};
    if (reference_fragment(s, d, Color4{1, 1, 1, 1}, true, 1.0f, f)) {
      const float a = s.rs[rs::ALPHABLENDENABLE] ? f.a : 1.0f;
      want[0] = f.r * a + clear[0] * (1 - a);
      want[1] = f.g * a + clear[1] * (1 - a);
      want[2] = f.b * a + clear[2] * (1 - a);
    }
    const uint8_t* c = &px[(4 * 8 + 4) * 4];
    int w8[3];
    bool hit = c[3] == 255 && unsup == 0;
    for (int k = 0; k < 3; ++k) {
      w8[k] = static_cast<int>(std::lround(std::min(1.0f, std::max(0.0f, want[k])) * 255.0f));
      hit = hit && std::abs(int(c[k]) - w8[k]) <= 2;
    }
    std::printf("%s %s = %u,%u,%u,%u want %d,%d,%d\n", hit ? "ok  " : "FAIL", what, c[0], c[1], c[2], c[3], w8[0], w8[1], w8[2]);
    ok = ok && hit;
  };

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
  run("lit: vertex colour as COLOR1 diffuse, point (attenuated) + directional + ambients", lit, true);
  run("lit: no vertex colour -> material diffuse", lit, false);
  State lit_far = lit;
  lit_far.lights[0].range = 3.0f;  // every vertex is sqrt(13) > 3 away: the point light contributes nothing
  run("lit: point light out of range", lit_far, true);
  State back = lit_far;  // a directional light from behind the surface: N.L = -1 is clamped to 0
  back.lights[2].direction[2] = -2;
  run("lit: directional light behind the surface (N.L clamped to 0)", back, true);
  State unlit = base;
  unlit.rs[rs::LIGHTING] = 0;
  run("LIGHTING off: vertex colour", unlit, true);
  run("LIGHTING off, no vertex colour: opaque white", unlit, false);
  State fog = unlit;
  fog.rs[rs::FOGENABLE] = 1;
  fog.rs[rs::FOGTABLEMODE] = kFogLinear;
  fog.rs[rs::FOGSTART] = float_to_bits(0.0f);
  fog.rs[rs::FOGEND] = float_to_bits(4.0f);
  fog.rs[rs::FOGCOLOR] = 0xffff0000u;
  run("fog LINEAR at w 1 (start 0, end 4): f 0.75", fog, true);
  fog.rs[rs::FOGTABLEMODE] = kFogExp;
  fog.rs[rs::FOGDENSITY] = float_to_bits(0.7f);
  run("fog EXP density 0.7 at w 1", fog, true);
  fog.rs[rs::FOGTABLEMODE] = kFogExp2;
  run("fog EXP2 density 0.7 at w 1", fog, true);
  State test = unlit;  // alpha test only (GcArea::Render's plnt/tre1 context flag: DrawX turns blending off), TFACTOR alpha
  test.rs[rs::ALPHABLENDENABLE] = 0;
  test.rs[rs::ALPHATESTENABLE] = 1;
  test.rs[rs::ALPHAFUNC] = kCmpGreater;
  test.rs[rs::ALPHAREF] = 1;
  test.tss[0][tss::ALPHAOP] = kTopSelectArg2;
  test.tss[0][tss::ALPHAARG2] = kTaTFactor;
  test.rs[rs::TEXTUREFACTOR] = 0x01000000u;  // alpha 1: 1 > 1 fails -> discarded
  run("alpha test GREATER ref 1, alpha 1: discarded", test, true);
  test.rs[rs::TEXTUREFACTOR] = 0x02000000u;
  run("alpha test GREATER ref 1, alpha 2: kept (unblended)", test, true);
  test.rs[rs::ALPHAREF] = 0;
  test.rs[rs::TEXTUREFACTOR] = 0x01000000u;
  run("alpha test GREATER ref 0, alpha 1: kept (R13: ALPHAREF 0 keeps alpha 1/255)", test, true);
  test.rs[rs::ALPHABLENDENABLE] = 1;
  test.rs[rs::ALPHATESTENABLE] = 0;
  test.rs[rs::TEXTUREFACTOR] = 0xb2000000u;  // 178 = round(255 * 0.7): the vllg river's textures.slb trans
  run("TFACTOR alpha 178 blended", test, true);
  State mod = unlit;  // vertex alpha through ALPHAOP MODULATE(TFACTOR, TEXTURE) (DrawX's mTransparency < 1 path)
  mod.rs[rs::ALPHABLENDENABLE] = 1;
  mod.tss[0][tss::ALPHAOP] = kTopModulate;
  mod.tss[0][tss::ALPHAARG1] = kTaTFactor;
  mod.tss[0][tss::ALPHAARG2] = kTaTexture;
  mod.rs[rs::TEXTUREFACTOR] = 0x80000000u;
  run("ALPHAOP MODULATE(TFACTOR 128, TEXTURE 255)", mod, true);
  std::printf("adapter=%s\n", dev->adapter().c_str());
  return ok ? 0 : 1;
}
