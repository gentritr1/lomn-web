// A7 on Dawn (headless): the synthetic scene rendered through load_area + AreaRenderer and checked at known pixels.
// Exits 77 (ctest SKIP) without an adapter. (The rhi-only blend and M0-in-depth probes live in test_gpu_mesh.cpp.) Pixel centres below are computed from the scene geometry in
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
  const rhi::FrameStats fs = dev->last_frame_stats();
  std::printf("adapter=%s draws=%u rejected=%u\n", dev->adapter().c_str(), fs.draws, fs.rejected);
  return ok && fs.draws == 10 && fs.rejected == 0 ? 0 : 1;
}
