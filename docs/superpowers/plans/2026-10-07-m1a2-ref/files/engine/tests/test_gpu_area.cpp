// A7 on Dawn (headless): the synthetic scene rendered through load_area + AreaRenderer and checked at known pixels.
// Exits 77 (ctest SKIP) without an adapter. Pixel centres below are computed from the scene geometry in
// lomn/view/synthetic.h: 64x64, fov 90, eye (0,0,10) looking along -Z, so a point at world z = 0 has x_ndc = x / 10
// and at z = -2 x_ndc = x / 12; pixel (px, py) has ndc ((px + 0.5) / 32 - 1, 1 - (py + 0.5) / 32).
#include <cstdio>
#include <cstdlib>

#include "lomn/fs.h"
#include "lomn/rhi.h"
#include "lomn/view/area.h"
#include "lomn/view/synthetic.h"

using namespace lomn;

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
  fs::MemoryFileSystem mfs;
  view::AreaData area;
  if (!view::mount_synthetic_install(mfs) || !view::load_area(mfs, view::kSyntheticLevel, view::kSyntheticArea, view::AreaOptions(), area, &err)) {
    std::printf("load failed: %s\n", err.c_str());
    return 1;
  }
  rhi::RenderTargetDesc rd{64, 64, true, 1.0f};
  rhi::TextureId rt = dev->create_render_target_ex(rd);
  view::AreaRenderer r;
  if (!rt || !r.upload(*dev, area, view::RenderOptions(), &err)) {
    std::printf("upload failed: %s\n", err.c_str());
    return 1;
  }
  r.render(*dev, rt, area, view::make_camera(area, view::synthetic_camera(), 1.0f));
  std::vector<uint8_t> px;
  if (!dev->read_pixels(rt, px) || px.size() != 64 * 64 * 4) return 1;
  struct Probe {
    int x, y, r, g, b;
    const char* what;
  };
  const Probe probes[] = {
      {17, 32, 255, 0, 0, "red DXT1 quad (opaque) in front of the blue quad: depth test LESSEQUAL"},
      {41, 32, 0, 128, 0, "cut quad, opaque texels: green x vertex colour (1,0.5,1)"},
      {52, 25, 0, 0, 255, "cut quad, alpha-0 texels discarded (no depth write): blue quad behind shows"},
      {32, 16, 0, 0, 255, "blue quad: material diffuse, no vertex colours, no texture (white)"},
      {58, 32, 255, 128, 255, "trans quad @-60, missing texture: white fallback x vertex colour"},
      {1, 1, 0, 0, 0, "clear colour 0xff000000"},
  };
  bool ok = true;
  for (const Probe& p : probes) {
    const uint8_t* c = &px[(static_cast<size_t>(p.y) * 64 + p.x) * 4];
    const bool hit = std::abs(c[0] - p.r) <= 2 && std::abs(c[1] - p.g) <= 2 && std::abs(c[2] - p.b) <= 2 && c[3] == 255;
    std::printf("%s (%d,%d) = %u,%u,%u,%u want %d,%d,%d: %s\n", hit ? "ok  " : "FAIL", p.x, p.y, c[0], c[1], c[2], c[3], p.r, p.g, p.b, p.what);
    ok = ok && hit;
  }
  const uint32_t a7_draws = dev->last_frame_stats().draws;
  std::printf("adapter=%s draws=%u\n", dev->adapter().c_str(), a7_draws);
  ok = ok && a7_draws == 10;

  // Blend state on Dawn, straight through rhi with the area shader: a full-target opaque quad (vertex colour blue,
  // white texture), then an alpha-pipeline quad (vertex colour red, texture alpha 128) over it. SRCALPHA/INVSRCALPHA
  // gives red * 128/255 + blue * 127/255 = (128, 0, 127); the target's alpha stays 255.
  rhi::MeshPipelineDesc pd;
  pd.wgsl = view::area_wgsl();
  pd.uniform_bytes = view::kAreaUniformBytes;
  pd.vertex_stride = 36;
  pd.attributes = {{rhi::VertexFormat::Float32x3, 0, 0}, {rhi::VertexFormat::Float32x2, 12, 1}, {rhi::VertexFormat::Float32x4, 20, 2}};
  pd.fs_entry = "fs_opaque";
  rhi::PipelineId opaque = dev->create_mesh_pipeline(pd);
  pd.fs_entry = "fs_alpha";
  pd.blend = rhi::BlendMode::AlphaBlend;
  rhi::PipelineId alpha = dev->create_mesh_pipeline(pd);
  const float verts[2][4][9] = {
      {{-1, -1, 0.5f, 0, 0, 0, 0, 1, 1}, {1, -1, 0.5f, 0, 0, 0, 0, 1, 1}, {1, 1, 0.5f, 0, 0, 0, 0, 1, 1}, {-1, 1, 0.5f, 0, 0, 0, 0, 1, 1}},
      {{-1, -1, 0.25f, 0, 0, 1, 0, 0, 1}, {1, -1, 0.25f, 0, 0, 1, 0, 0, 1}, {1, 1, 0.25f, 0, 0, 1, 0, 0, 1}, {-1, 1, 0.25f, 0, 0, 1, 0, 0, 1}}};
  const uint32_t idx[12] = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
  rhi::BufferId vb = dev->create_vertex_buffer(sizeof verts), ib = dev->create_index_buffer(sizeof idx);
  rhi::BufferId ub = dev->create_uniform_buffer(2 * rhi::kUniformAlign);
  dev->write_buffer(vb, 0, verts, sizeof verts);
  dev->write_buffer(ib, 0, idx, sizeof idx);
  float u[2][64] = {};
  for (int k = 0; k < 2; ++k) {
    u[k][0] = u[k][5] = u[k][10] = u[k][15] = 1.0f;  // wvp = identity (positions already in clip space)
    u[k][20] = 1.0f;                                   // use vertex colour
    u[k][21] = 1.0f / 255.0f;                          // alpha ref 1
  }
  dev->write_buffer(ub, 0, u, sizeof u);
  const uint8_t white[4] = {255, 255, 255, 255}, half[4] = {255, 255, 255, 128};
  rhi::TextureId tw = dev->create_texture(1, 1, 1), th = dev->create_texture(1, 1, 1);
  dev->write_texture(tw, 0, white, 4);
  dev->write_texture(th, 0, half, 4);
  rhi::SamplerId s = dev->create_sampler(rhi::SamplerDesc());
  rhi::BindGroupId bw = dev->create_bind_group(ub, view::kAreaUniformBytes, tw, s), bh = dev->create_bind_group(ub, view::kAreaUniformBytes, th, s);
  rhi::TextureId rt2 = dev->create_render_target_ex(rhi::RenderTargetDesc{8, 8, true, 1.0f});
  const float clear[4] = {0, 0, 0, 1};
  dev->begin_frame(rt2, clear);
  dev->draw_indexed({opaque, vb, ib, 0, 6, bw, 0});
  dev->draw_indexed({alpha, vb, ib, 6, 6, bh, rhi::kUniformAlign});
  dev->end_frame();
  std::vector<uint8_t> px2;
  if (!dev->read_pixels(rt2, px2) || px2.size() != 8 * 8 * 4) return 1;
  const uint8_t* c = &px2[(4 * 8 + 4) * 4];
  const bool blend_ok = std::abs(c[0] - 128) <= 2 && c[1] <= 2 && std::abs(c[2] - 127) <= 2 && c[3] == 255;
  std::printf("%s blend (4,4) = %u,%u,%u,%u want 128,0,127,255: SRCALPHA/INVSRCALPHA, target alpha kept\n", blend_ok ? "ok  " : "FAIL", c[0],
              c[1], c[2], c[3]);
  ok = ok && blend_ok && dev->last_frame_stats().draws == 2;
  return ok ? 0 : 1;
}
