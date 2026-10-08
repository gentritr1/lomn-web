// M1b rhi on Dawn (headless). Exits 77 (ctest SKIP) without an adapter.
//  1. destroy_* with the dependency rule: a texture/sampler/uniform buffer referenced by a live bind group is rejected
//     (counted, nothing freed); after the bind group goes, all of them go; live_objects() follows.
//  2. Destroy inside an open frame: the bind group and texture of a draw already recorded are destroyed before
//     end_frame; the frame still renders that texture (WebGPU keeps recorded objects alive) with no Dawn error.
//  3. Leak loops: 300 x (texture + bind group) and 100 x (depth render target) created and destroyed: live counts flat.
//  4. Surface calls on a headless device are counted rejections.
//  5. AreaRenderer::release: the synthetic A7 scene uploaded, rendered, read back and released 8 times on one device:
//     pixels byte-identical every cycle, live counts back to the baseline after every release.
//  REF4 r2: destroy_texture/destroy_buffer now Destroy the GPU object (deferred to end_frame inside a frame): probes 2
//  and 5 cover both paths. 6. the lost handler receives the first uncaptured GPU error once.
//  T8 P5: 7. the surface depth buffer (release_surface_depth) really Destroys on every resize: Dawn has no native surface,
//  so the closest observable is the device's own destroy counter (gpu_destroys) and its undestroyed-surface-depth set,
//  driven through the headless test seam debug_cycle_surface_depth (the depth half of resize()). Runs before probe 6 so
//  that "no GPU error" can still be asserted. The same counter pins texture/buffer destroys (2 per depth render target).
//  T8 P4: the lost handler is installed before probe 1 and records every reason it receives, so probes 1-4 assert that
//  no GPU error happened (none reached it, after a queue wait), and probe 6 asserts WHICH error arrived: the provoked
//  WGSL compile error (a stale error left by an earlier probe would arrive first, and fail that check).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "lomn/fs.h"
#include "lomn/rhi.h"
#include "lomn/view/area.h"
#include "lomn/view/synthetic.h"

using namespace lomn;
using namespace lomn::rhi;

namespace {

const char* kWgsl = R"(
struct U { tint: vec4f }
@group(0) @binding(0) var<uniform> u: U;
@group(0) @binding(1) var tex: texture_2d<f32>;
@group(0) @binding(2) var samp: sampler;
struct VOut { @builtin(position) pos: vec4f }
@vertex fn vs(@location(0) p: vec3f) -> VOut { var o: VOut; o.pos = vec4f(p, 1.0); return o; }
@fragment fn fs(i: VOut) -> @location(0) vec4f { return vec4f(textureSample(tex, samp, vec2f(0.5, 0.5)).rgb * u.tint.rgb, 1.0); }
)";

bool same(const Device::LiveObjects& a, const Device::LiveObjects& b) { return a == b; }
void print(const char* what, const Device::LiveObjects& o) {
  std::printf("     %s: textures=%u buffers=%u pipelines=%u samplers=%u bind_groups=%u\n", what, o.textures, o.buffers, o.pipelines,
              o.samplers, o.bind_groups);
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
  auto check = [&](bool hit, const char* what) {
    std::printf("%s %s\n", hit ? "ok  " : "FAIL", what);
    ok = ok && hit;
  };
  // T8 P4: every reason the lost handler receives (the device forwards only the first uncaptured error / loss).
  std::vector<std::string> gpu_errors;
  dev->set_lost_handler([&](const std::string& why) { gpu_errors.push_back(why); });

  // 1. Dependency rule.
  MeshPipelineDesc pd;
  pd.wgsl = kWgsl;
  pd.uniform_bytes = 16;
  pd.vertex_stride = 12;
  pd.attributes = {{VertexFormat::Float32x3, 0, 0}};
  const PipelineId pipe = dev->create_mesh_pipeline(pd);
  const float quad[4][3] = {{-1, -1, 0.5f}, {1, -1, 0.5f}, {1, 1, 0.5f}, {-1, 1, 0.5f}};
  const uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
  const BufferId vb = dev->create_vertex_buffer(sizeof quad), ib = dev->create_index_buffer(sizeof idx);
  const BufferId ub = dev->create_uniform_buffer(kUniformAlign);
  dev->write_buffer(vb, 0, quad, sizeof quad);
  dev->write_buffer(ib, 0, idx, sizeof idx);
  const float tint[4] = {1, 1, 1, 1};
  dev->write_buffer(ub, 0, tint, sizeof tint);
  const SamplerId samp = dev->create_sampler(SamplerDesc());
  const Device::LiveObjects base = dev->live_objects();
  const TextureId tex = dev->create_texture(1, 1, 1);
  const uint8_t green[4] = {0, 255, 0, 255};
  dev->write_texture(tex, 0, green, 4);
  const BindGroupId bg = dev->create_bind_group({ub, 16, tex, samp});
  if (!pipe || !vb || !ib || !ub || !samp || !tex || !bg) return std::printf("FAIL setup\n"), 1;
  const uint32_t rej0 = dev->last_frame_stats().rejected;
  check(!dev->destroy_texture(tex) && !dev->destroy_sampler(samp) && !dev->destroy_buffer(ub) &&
            dev->last_frame_stats().rejected == rej0 + 3,
        "1. texture/sampler/uniform buffer referenced by a live bind group: 3 rejected destroys");
  check(dev->live_objects().textures == base.textures + 1 && dev->live_objects().bind_groups == 1, "1. nothing freed by the rejections");

  // 2. Destroy inside an open frame, after the draw was recorded.
  const TextureId rt = dev->create_render_target_ex(RenderTargetDesc{8, 8, true, 1.0f});
  const float clear[4] = {0, 0, 0.25f, 1};
  dev->begin_frame(rt, clear);
  dev->draw_indexed({pipe, vb, ib, 0, 6, bg, 0});
  const bool d1 = dev->destroy_bind_group(bg), d2 = dev->destroy_texture(tex);
  dev->end_frame();
  std::vector<uint8_t> px;
  const bool read = dev->read_pixels(rt, px);
  check(d1 && d2 && read && px.size() == 8 * 8 * 4 && px[(4 * 8 + 4) * 4 + 1] >= 253 && px[(4 * 8 + 4) * 4 + 0] <= 2,
        "2. bind group + texture destroyed after the draw was recorded: the frame still shows the texture (green)");
  check(dev->last_frame_stats().draws == 1 && dev->last_frame_stats().rejected == 0, "2. 1 draw, 0 rejected");
  check(dev->destroy_texture(rt), "2. the render target (with its depth buffer) is destroyable");
  check(same(dev->live_objects(), base), "1+2. live objects back to the baseline");

  // 3. Leak loops.
  for (int i = 0; i < 300; ++i) {
    const TextureId t = dev->create_texture(256, 256, 9);
    const BindGroupId g = dev->create_bind_group({ub, 16, t, samp});
    if (!t || !g || !dev->destroy_bind_group(g) || !dev->destroy_texture(t)) {
      ok = false;
      std::printf("FAIL 3. texture loop iteration %d\n", i);
      break;
    }
  }
  for (int i = 0; i < 100; ++i) {
    const TextureId t = dev->create_render_target_ex(RenderTargetDesc{640, 480, true, 1.0f});
    if (!t || !dev->destroy_texture(t)) {
      ok = false;
      std::printf("FAIL 3. render-target loop iteration %d\n", i);
      break;
    }
  }
  check(same(dev->live_objects(), base), "3. 300 texture+bind group and 100 depth-target create/destroy cycles: counts flat");

  // 4. Surface calls headless.
  const uint32_t rej1 = dev->last_frame_stats().rejected;
  check(!dev->set_surface_depth(true, 1.0f) && !dev->resize(64, 64) && dev->last_frame_stats().rejected == rej1 + 2,
        "4. set_surface_depth / resize without a surface: counted rejections");
  {
    // T8 P4: wait for the queue (a frame + read-back), so any asynchronous error from probes 1-4 has been reported.
    const TextureId ft = dev->create_render_target_ex(RenderTargetDesc{8, 8, true, 1.0f});
    dev->begin_frame(ft, clear);
    dev->end_frame();
    std::vector<uint8_t> fpx;
    const bool flushed = dev->read_pixels(ft, fpx) && dev->destroy_texture(ft);
    for (const std::string& e : gpu_errors) std::printf("     unexpected GPU error: %s\n", e.c_str());
    check(flushed && gpu_errors.empty(), "1-4. no GPU error (0 uncaptured errors reached the lost handler after a queue wait)");
  }

  // 5. AreaRenderer::release cycles on the synthetic A7 scene.
  fs::MemoryFileSystem mfs;
  view::AreaData area;
  if (!view::mount_synthetic_install(mfs) || !view::load_area(mfs, view::kSyntheticLevel, view::kSyntheticArea, view::AreaOptions(), area, &err))
    return std::printf("FAIL load: %s\n", err.c_str()), 1;
  const TextureId art = dev->create_render_target_ex(RenderTargetDesc{64, 64, true, 1.0f});
  const Device::LiveObjects before = dev->live_objects();
  view::AreaRenderer r;
  std::vector<uint8_t> first;
  Device::LiveObjects shown;
  for (int cycle = 0; cycle < 8; ++cycle) {
    if (!r.upload(*dev, area, view::RenderOptions(), &err)) {
      std::printf("FAIL 5. upload cycle %d: %s\n", cycle, err.c_str());
      ok = false;
      break;
    }
    r.render(*dev, art, area, view::make_camera(area, view::synthetic_camera(), 1.0f));
    r.render(*dev, art, area, view::make_camera(area, view::synthetic_camera(), 1.0f));
    std::vector<uint8_t> p;
    if (!dev->read_pixels(art, p)) {
      ok = false;
      break;
    }
    if (cycle == 0) {
      first = p;
      shown = dev->live_objects();
      print("area shown", shown);
    } else if (p != first || !same(dev->live_objects(), shown)) {
      std::printf("FAIL 5. cycle %d: pixels %s, live counts %s\n", cycle, p == first ? "same" : "DIFFER",
                  same(dev->live_objects(), shown) ? "same" : "DIFFER");
      ok = false;
    }
    uint32_t failures = 0;
    r.release(*dev, &failures);
    if (failures || !same(dev->live_objects(), before)) {
      std::printf("FAIL 5. cycle %d: release failures %u\n", cycle, failures);
      print("after release", dev->live_objects());
      ok = false;
    }
  }
  check(ok, "5. 8 upload/render/release cycles: identical pixels, flat live counts, 0 rejected destroys");
  check(dev->last_frame_stats().rejected == 0, "5. no rejected call in the last frame");
  // 7. (T8 P5) Surface depth lifecycle + destroy counters.
  {
    const uint64_t g0 = dev->gpu_destroys();
    const TextureId drt = dev->create_render_target_ex(RenderTargetDesc{16, 16, true, 1.0f});  // colour + depth
    const BufferId dbuf = dev->create_uniform_buffer(kUniformAlign);
    const bool dd = dev->destroy_texture(drt) && dev->destroy_buffer(dbuf);
    check(dd && dev->gpu_destroys() == g0 + 3, "7. destroy_texture of a depth render target + destroy_buffer: 3 explicit GPU Destroy calls");
    const uint64_t g1 = dev->gpu_destroys();
    check(dev->debug_undestroyed_surface_depth() == 0, "7. no surface depth buffer before the first cycle");
    constexpr int kResizes = 24;
    bool cycled = true;
    for (int i = 0; i <= kResizes; ++i) cycled = dev->debug_cycle_surface_depth(64 + 8 * i, 48 + 4 * i) && cycled;
    // 1 creation + 24 recreations: the first cycle destroys nothing, each later one destroys its predecessor.
    std::printf("     surface depth: %d cycles, undestroyed=%u, gpu_destroys +%llu\n", kResizes + 1, dev->debug_undestroyed_surface_depth(),
                static_cast<unsigned long long>(dev->gpu_destroys() - g1));
    check(cycled && dev->debug_undestroyed_surface_depth() == 1,
          "7. 25 surface depth create/resize cycles: exactly 1 live (undestroyed) depth texture, not 25");
    check(dev->gpu_destroys() == g1 + kResizes, "7. each resize destroyed its predecessor (24 explicit Destroy calls)");
    check(!dev->debug_cycle_surface_depth(0, 8) && !dev->debug_cycle_surface_depth(8, 1u << 30) && dev->debug_undestroyed_surface_depth() == 1,
          "7. an out-of-range size is refused and leaves the depth buffer alone");
    // Wait for the queue so an asynchronous validation error from the cycles would have been reported by now.
    const TextureId ft = dev->create_render_target_ex(RenderTargetDesc{8, 8, true, 1.0f});
    dev->begin_frame(ft, clear);
    dev->end_frame();
    std::vector<uint8_t> fpx;
    const bool flushed = dev->read_pixels(ft, fpx) && dev->destroy_texture(ft);
    for (const std::string& e : gpu_errors) std::printf("     unexpected GPU error: %s\n", e.c_str());
    check(flushed && gpu_errors.empty(), "7. no GPU error from the surface depth cycles");
  }
  // 6. (REF4 r2, C1) The lost handler: a pipeline whose WGSL does not compile passes the shared checks (rhi does not parse
  //    WGSL) and fails inside Dawn as an uncaptured validation error; that first error reaches the handler exactly once,
  //    with a readable reason.
  {
    MeshPipelineDesc bad = pd;
    bad.wgsl = "this is not WGSL";
    const PipelineId bp = dev->create_mesh_pipeline(bad);
    const TextureId t6 = dev->create_render_target_ex(RenderTargetDesc{8, 8, true, 1.0f});
    const TextureId tx = dev->create_texture(1, 1, 1);
    const BindGroupId g6 = dev->create_bind_group({ub, 16, tx, samp});
    dev->begin_frame(t6, clear);
    dev->draw_indexed({bp, vb, ib, 0, 6, g6, 0});
    dev->end_frame();
    dev->draw_indexed({bp, vb, ib, 0, 6, g6, 0});  // outside a frame: a counted rejection, not a second GPU error
    std::vector<uint8_t> px6;
    dev->read_pixels(t6, px6);  // waits for the queue: any asynchronous error has been reported by now
    const std::string why = gpu_errors.empty() ? std::string() : gpu_errors.front();
    std::printf("     lost handler: %zu call(s): %s\n", gpu_errors.size(), why.c_str());
    check(gpu_errors.size() == 1 && why.rfind("GPU error (validation)", 0) == 0, "6. the first uncaptured GPU error reaches the lost handler once");
    // T8 P4: and it is the error this probe provoked (Dawn's WGSL parse error quoting the bad source), not a stale one.
    check(why.find("Error while parsing WGSL") != std::string::npos && why.find("this is not WGSL") != std::string::npos,
          "6. the handler's reason is the provoked WGSL compile error");
  }
  std::printf("adapter=%s\n", dev->adapter().c_str());
  return ok ? 0 : 1;
}
