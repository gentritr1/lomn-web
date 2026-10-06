// Native WebGPU (Dawn) headless: render the scene and check pixels. Exits 77 (ctest SKIP) without an adapter.
#include <cstdio>
#include <cstdlib>

#include "lomn/loop.h"
#include "lomn/rhi.h"

int main() {
  std::unique_ptr<lomn::rhi::Device> dev;
  std::string err;
  lomn::rhi::create_webgpu_device(nullptr, 0, 0, [&](std::unique_ptr<lomn::rhi::Device> d, std::string e) {
    dev = std::move(d);
    err = std::move(e);
  });
  if (!dev) {
    std::printf("SKIPPED (no WebGPU adapter: %s)\n", err.c_str());
    return 77;
  }
  lomn::TriangleScene scene;
  scene.init(*dev);
  auto rt = dev->create_render_target(64, 64);
  scene.render(*dev, rt, lomn::kFakeDeltaTime);
  std::vector<uint8_t> px;
  if (!dev->read_pixels(rt, px) || px.size() != 64 * 64 * 4) return 1;
  const uint8_t* c = &px[(32 * 64 + 32) * 4];
  const uint8_t* k = &px[(1 * 64 + 1) * 4];
  std::printf("adapter=%s centre=%u,%u,%u corner=%u,%u,%u\n", dev->adapter().c_str(), c[0], c[1], c[2], k[0], k[1], k[2]);
  auto near = [](int v, int want) { return std::abs(v - want) <= 2; };
  bool ok = near(c[0], 255) && near(c[1], 128) && near(c[2], 0) && near(k[0], 0) && near(k[1], 0) && near(k[2], 64);
  return ok ? 0 : 1;
}
