// rhi WebGPU backend on Dawn (headless), without lomn_area: local WGSL only. Exits 77 (ctest SKIP) without an adapter.
//  1. Blend: an opaque quad (blue), then an AlphaBlend quad (red, texture alpha 128) over it -> SRCALPHA/INVSRCALPHA
//     = (128, 0, 127) and the target's alpha stays 255.
//  2. An M0 pipeline drawn into a depth-enabled target keeps its pixels: byte-identical to the same draw into a target
//     without depth, and it is not depth-tested against a mesh quad drawn first nearer the camera (compare Always).
//  3. Invalid M0 draws (unknown pipeline, a mesh pipeline, unknown uniform buffer) are rejected and counted, never
//     reach Dawn and never throw; the frame's pixels are unchanged.
#include <cstdio>
#include <cstdlib>

#include "lomn/rhi.h"

using namespace lomn::rhi;

namespace {

const char* kMeshWgsl = R"(
struct U { tint: vec4f }
@group(0) @binding(0) var<uniform> u: U;
@group(0) @binding(1) var tex: texture_2d<f32>;
@group(0) @binding(2) var samp: sampler;
struct VOut { @builtin(position) pos: vec4f, @location(0) color: vec4f }
@vertex fn vs(@location(0) p: vec3f, @location(1) c: vec4f) -> VOut {
  var o: VOut;
  o.pos = vec4f(p, 1.0);
  o.color = c * u.tint;
  return o;
}
@fragment fn fs_opaque(i: VOut) -> @location(0) vec4f {
  return vec4f(textureSample(tex, samp, vec2f(0.5, 0.5)).rgb * i.color.rgb, 1.0);
}
@fragment fn fs_alpha(i: VOut) -> @location(0) vec4f {
  let t = textureSample(tex, samp, vec2f(0.5, 0.5));
  if (t.a <= 1.0 / 255.0) { discard; }
  return vec4f(t.rgb * i.color.rgb, t.a);
}
)";

// The M0 triangle (as lomn::TriangleScene, angle 0): orange on the clear colour.
const char* kM0Wgsl = R"(
struct U { angle: f32 }
@group(0) @binding(0) var<uniform> u: U;
@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
  var p = array<vec2f, 3>(vec2f(0.0, 0.8), vec2f(-0.8, -0.8), vec2f(0.8, -0.8));
  let c = cos(u.angle);
  let s = sin(u.angle);
  let v = p[i];
  return vec4f(c * v.x - s * v.y, s * v.x + c * v.y, 0.0, 1.0);
}
@fragment fn fs() -> @location(0) vec4f { return vec4f(1.0, 0.5, 0.0, 1.0); }
)";

bool near(const uint8_t* c, int r, int g, int b, int a) {
  return std::abs(c[0] - r) <= 2 && std::abs(c[1] - g) <= 2 && std::abs(c[2] - b) <= 2 && c[3] == a;
}

}  // namespace

int main() {
  std::unique_ptr<Device> dev;
  std::string err;
  create_webgpu_device(nullptr, 0, 0, [&](std::unique_ptr<Device> d, std::string e) {
    dev = std::move(d);
    err = std::move(e);
  });
  if (!dev) {
    std::printf("SKIPPED (no WebGPU adapter: %s)\n", err.c_str());
    return 77;
  }
  bool ok = true;
  auto report = [&](bool hit, const char* what, const uint8_t* c) {
    std::printf("%s %s = %u,%u,%u,%u\n", hit ? "ok  " : "FAIL", what, c[0], c[1], c[2], c[3]);
    ok = ok && hit;
  };

  // Shared mesh objects: two full-target quads (z 0.5 blue, z 0.25 red), 1x1 textures (alpha 255 and 128).
  MeshPipelineDesc pd;
  pd.wgsl = kMeshWgsl;
  pd.uniform_bytes = 16;
  pd.vertex_stride = 28;
  pd.attributes = {{VertexFormat::Float32x3, 0, 0}, {VertexFormat::Float32x4, 12, 1}};
  pd.fs_entry = "fs_opaque";
  PipelineId opaque = dev->create_mesh_pipeline(pd);
  pd.fs_entry = "fs_alpha";
  pd.blend = BlendMode::AlphaBlend;
  PipelineId alpha = dev->create_mesh_pipeline(pd);
  const float verts[2][4][7] = {
      {{-1, -1, 0.5f, 0, 0, 1, 1}, {1, -1, 0.5f, 0, 0, 1, 1}, {1, 1, 0.5f, 0, 0, 1, 1}, {-1, 1, 0.5f, 0, 0, 1, 1}},
      {{-1, -1, 0.25f, 1, 0, 0, 1}, {1, -1, 0.25f, 1, 0, 0, 1}, {1, 1, 0.25f, 1, 0, 0, 1}, {-1, 1, 0.25f, 1, 0, 0, 1}}};
  const uint32_t idx[12] = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
  BufferId vb = dev->create_vertex_buffer(sizeof verts), ib = dev->create_index_buffer(sizeof idx);
  BufferId ub = dev->create_uniform_buffer(kUniformAlign);
  dev->write_buffer(vb, 0, verts, sizeof verts);
  dev->write_buffer(ib, 0, idx, sizeof idx);
  const float tint[4] = {1, 1, 1, 1};
  dev->write_buffer(ub, 0, tint, sizeof tint);
  const uint8_t white[4] = {255, 255, 255, 255}, half[4] = {255, 255, 255, 128};
  TextureId tw = dev->create_texture(1, 1, 1), th = dev->create_texture(1, 1, 1);
  dev->write_texture(tw, 0, white, 4);
  dev->write_texture(th, 0, half, 4);
  SamplerId s = dev->create_sampler(SamplerDesc());
  BindGroupId bw = dev->create_bind_group({ub, 16, tw, s}), bh = dev->create_bind_group({ub, 16, th, s});
  if (!opaque || !alpha || !vb || !ib || !ub || !tw || !th || !s || !bw || !bh) return std::printf("FAIL setup\n"), 1;
  const float clear[4] = {0, 0, 0.25f, 1};

  // 1. Blend.
  TextureId rt = dev->create_render_target_ex(RenderTargetDesc{8, 8, true, 1.0f});
  dev->begin_frame(rt, clear);
  dev->draw_indexed({opaque, vb, ib, 0, 6, bw, 0});
  dev->draw_indexed({alpha, vb, ib, 6, 6, bh, 0});
  dev->end_frame();
  std::vector<uint8_t> px;
  if (!dev->read_pixels(rt, px)) return 1;
  report(near(&px[(4 * 8 + 4) * 4], 128, 0, 127, 255) && dev->last_frame_stats().draws == 2,
         "blend (4,4) want 128,0,127,255: SRCALPHA/INVSRCALPHA, target alpha kept", &px[(4 * 8 + 4) * 4]);

  // 2. M0 pipeline into a depth target.
  PipelineDesc m0;
  m0.wgsl = kM0Wgsl;
  m0.uniform_bytes = 16;
  PipelineId tri = dev->create_pipeline(m0);
  BufferId tu = dev->create_uniform_buffer(kUniformAlign);
  const float angle[4] = {0, 0, 0, 0};
  dev->write_buffer(tu, 0, angle, sizeof angle);
  TextureId plain = dev->create_render_target(64, 64);
  TextureId depth = dev->create_render_target_ex(RenderTargetDesc{64, 64, true, 1.0f});
  std::vector<uint8_t> a, b;
  for (TextureId t : {plain, depth}) {
    dev->begin_frame(t, clear);
    dev->draw(tri, 3, tu, 0);
    dev->end_frame();
    if (!dev->read_pixels(t, t == plain ? a : b)) return 1;
  }
  report(a == b && near(&b[(32 * 64 + 32) * 4], 255, 128, 0, 255), "M0 triangle in a depth target == without depth (centre)",
         &b[(32 * 64 + 32) * 4]);
  // A mesh quad written to depth 0.25 first; the M0 triangle (z 0, compare Always) still covers it.
  dev->begin_frame(depth, clear);
  dev->draw_indexed({opaque, vb, ib, 6, 6, bw, 0});
  dev->draw(tri, 3, tu, 0);
  dev->end_frame();
  if (!dev->read_pixels(depth, b)) return 1;
  report(near(&b[(32 * 64 + 32) * 4], 255, 128, 0, 255), "M0 triangle over a nearer mesh quad (not depth-tested)", &b[(32 * 64 + 32) * 4]);
  report(near(&b[(2 * 64 + 2) * 4], 255, 0, 0, 255), "mesh quad outside the triangle", &b[(2 * 64 + 2) * 4]);

  // 3. Invalid M0 draws are rejected: same pixels as a frame with only the valid draw.
  dev->begin_frame(depth, clear);
  dev->draw(99, 3, tu, 0);
  dev->draw(opaque, 3, tu, 0);
  dev->draw(tri, 3, 99, 0);
  dev->draw(tri, 3, tu, 0);
  dev->end_frame();
  const FrameStats st = dev->last_frame_stats();
  std::vector<uint8_t> c;
  if (!dev->read_pixels(depth, c)) return 1;
  report(st.rejected == 3 && st.draws == 1 && c == a, "3 invalid M0 draws rejected, 1 drawn, pixels == the valid frame", &c[(32 * 64 + 32) * 4]);
  std::printf("adapter=%s\n", dev->adapter().c_str());
  return ok ? 0 : 1;
}
