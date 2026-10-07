// A7 on Dawn (headless): the synthetic scene rendered through load_area + AreaRenderer and checked at known pixels.
// Exits 77 (ctest SKIP) without an adapter. (The rhi-only blend and M0-in-depth probes live in test_gpu_mesh.cpp.) Pixel centres below are computed from the scene geometry in
// lomn/view/synthetic.h: 64x64, fov 90, eye (0,0,10) looking along -Z, so a point at world z = 0 has x_ndc = x / 10
// and at z = -2 x_ndc = x / 12; pixel (px, py) has ndc ((px + 0.5) / 32 - 1, 1 - (py + 0.5) / 32).
#include <cstdio>
#include <cstdlib>
#include <vector>

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
  struct Probe {
    int x, y, r, g, b;
    const char* what;
  };
  bool ok = true;
  // Renders one synthetic area into a fresh 64x64 target and checks probes (+-2 per channel, alpha 255).
  auto run = [&](const char* area_name, const std::vector<Probe>& probes, uint32_t want_draws) {
    fs::MemoryFileSystem mfs;
    view::AreaData area;
    if (!view::mount_synthetic_install(mfs) || !view::load_area(mfs, view::kSyntheticLevel, area_name, view::AreaOptions(), area, &err)) {
      std::printf("FAIL load %s: %s\n", area_name, err.c_str());
      ok = false;
      return;
    }
    rhi::TextureId rt = dev->create_render_target_ex(rhi::RenderTargetDesc{64, 64, true, 1.0f});
    view::AreaRenderer r;
    if (!rt || !r.upload(*dev, area, view::RenderOptions(), &err)) {
      std::printf("FAIL upload %s: %s\n", area_name, err.c_str());
      ok = false;
      return;
    }
    r.render(*dev, rt, area, view::make_camera(area, view::synthetic_camera(), 1.0f));
    std::vector<uint8_t> px;
    if (!dev->read_pixels(rt, px) || px.size() != 64 * 64 * 4) {
      ok = false;
      return;
    }
    for (const Probe& p : probes) {
      const uint8_t* c = &px[(static_cast<size_t>(p.y) * 64 + p.x) * 4];
      const bool hit = std::abs(c[0] - p.r) <= 2 && std::abs(c[1] - p.g) <= 2 && std::abs(c[2] - p.b) <= 2 && c[3] == 255;
      std::printf("%s %s (%d,%d) = %u,%u,%u,%u want %d,%d,%d: %s\n", hit ? "ok  " : "FAIL", area_name, p.x, p.y, c[0], c[1], c[2], c[3], p.r,
                  p.g, p.b, p.what);
      ok = ok && hit;
    }
    const rhi::FrameStats st = dev->last_frame_stats();
    std::printf("%s %s draws=%u rejected=%u (want %u, 0)\n", st.draws == want_draws && st.rejected == 0 ? "ok  " : "FAIL", area_name,
                st.draws, st.rejected, want_draws);
    ok = ok && st.draws == want_draws && st.rejected == 0;
  };
  // A7 scene. M1a-3 semantics: LIGHTING is off for the area nodes, so a vertex without DIFFUSE is opaque white (the
  // material is ignored); no area script -> the default fog (4701..4702) leaves these pixels untouched.
  run(view::kSyntheticArea,
      {{17, 32, 255, 0, 0, "red DXT1 quad (opaque) in front of the blue quad: depth test LESSEQUAL"},
       {41, 32, 0, 128, 0, "cut quad, opaque texels: green x vertex colour (1,0.5,1)"},
       {52, 25, 255, 255, 255, "cut quad, alpha-0 texels discarded (no depth write): the 'blue' quad behind shows"},
       {32, 16, 255, 255, 255, "'blue' quad: no vertex colours + LIGHTING off -> white (material ignored), NULL texture"},
       {58, 32, 255, 128, 255, "trans quad @-60, missing texture: white x vertex colour"},
       {1, 1, 0, 0, 0, "clear colour: the viewport ClearRect 0x00000000 (alpha forced 1: X8R8G8B8)"}},
      10);
  // A9 state scene: the script fog (0xff808080, 5..50, LINEAR on W = eye depth) is on in MainArea until "plnt" turns it
  // off, and off in the Trans node. f(w) = (50 - w) / 45: w 10 -> 0.8889 (+128 * 0.1111 = 14.2), w 12 -> 0.8444 (19.9).
  run(view::kSyntheticStateArea,
      {{17, 32, 241, 14, 14, "red quad, w 10, fogged"},
       {41, 32, 14, 128, 14, "cut quad, opaque texels, fogged"},
       {32, 16, 235, 235, 235, "'blue' quad, w 12, fogged"},
       {58, 32, 255, 128, 255, "trans quad (Trans node: no fog)"},
       {9, 55, 14, 241, 14, "plnt: alpha pass with the context flag -> alpha test only (alpha 128 unblended), fogged"},
       {25, 55, 0, 128, 0, "mixm 'half' after plnt: fog now off, alpha 128 blended over black"},
       {39, 55, 0, 0, 0, "mixm 'x_glass': mTrans 0 -> TFACTOR alpha 0, blended: invisible"},
       {54, 55, 0, 0, 128, "mixm 'tfx': textures.slb trans 0.5 -> TFACTOR alpha 128 blended over black"}},
      21);  // Task 8A E1: + "sky1" (Trans, hidden behind the 'blue' quad @-2; "watr" replaced "watw"); Task 8B D2: + "eyes" (hidden too)
  std::printf("adapter=%s\n", dev->adapter().c_str());
  return ok ? 0 : 1;
}
