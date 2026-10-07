// M1a-3 on Dawn: the fixed-function uber-shader (lomn/d3d/ffp.h) against its CPU reference of the D3D8 formulas.
// Each case builds a d3d::State, compiles it (pipeline_key / pipeline_desc / sampler_desc / pack_uniforms), draws one
// triangle covering the 8x8 target's centre with constant per-vertex inputs (the three vertices are equidistant from
// the point light, so the per-vertex lighting is the same at every vertex), and compares the centre pixel with
// reference_vertex_color + reference_fragment (+-2 per channel; blending against the clear colour computed here).
// The cases live in tests/ffp_cases.h; tools/research/oracle/ffp_oracle.py checks the CPU reference itself on the same
// cases plus seeded random samples (ctest ffp_oracle).
// Exits 77 (ctest SKIP) without an adapter.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "lomn/d3d/device8.h"
#include "lomn/d3d/ffp.h"
#include "lomn/rhi.h"
#include "ffp_cases.h"

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
  // Equilateral triangle (circumradius 3) around the origin in the z = 0.5 plane, normal (0,0,-1) (tests/ffp_cases.h).
  const float* vc = test::kFfpVcol;
  float verts[3][12];
  test::ffp_triangle(verts);
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

  for (const test::FfpCase& c : test::gpu_ffp_cases()) run(c.what, c.s, c.has_vcol);
  std::printf("adapter=%s\n", dev->adapter().c_str());
  return ok ? 0 : 1;
}
