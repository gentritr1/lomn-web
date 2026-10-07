#include "lomn/loop.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

#if !defined(__EMSCRIPTEN__)
#include "stb_image_write.h"
#endif

namespace lomn {

namespace {
constexpr uint32_t kSize = 64;
const char* kWgsl = R"(
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

double percentile(std::vector<double> v, double q) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[static_cast<size_t>(q * (v.size() - 1) + 0.5)];
}
}  // namespace

float TriangleScene::angle_at(double sim_time) {
  constexpr double kTwoPi = 6.283185307179586;
  return static_cast<float>(std::fmod(sim_time * 0.25 * kTwoPi, kTwoPi));
}

void TriangleScene::init(rhi::Device& dev) {
  rhi::PipelineDesc d;
  d.wgsl = kWgsl;
  d.uniform_bytes = 16;
  pipeline_ = dev.create_pipeline(d);
  uniforms_ = dev.create_uniform_buffer(rhi::kUniformAlign);
}

void TriangleScene::render(rhi::Device& dev, rhi::TextureId target, double sim_time) {
  float u[4] = {angle_at(sim_time), 0, 0, 0};
  dev.write_buffer(uniforms_, 0, u, sizeof u);
  const float clear[4] = {0.0f, 0.0f, 0.25f, 1.0f};
  dev.begin_frame(target, clear);
  dev.draw(pipeline_, 3, uniforms_, 0);
  dev.end_frame();
}

bool run_deterministic(rhi::Device& dev, const LoopOptions& opts, std::vector<FrameRecord>* records) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::create_directories(opts.out_dir, ec);
  if (ec) {
    std::fprintf(stderr, "cannot create %s: %s\n", opts.out_dir.c_str(), ec.message().c_str());
    return false;
  }
  std::ofstream trace(fs::path(opts.out_dir) / "trace.jsonl");
  if (!trace) {
    std::fprintf(stderr, "cannot write %s/trace.jsonl\n", opts.out_dir.c_str());
    return false;
  }
  TriangleScene scene;
  scene.init(dev);
  rhi::TextureId rt = dev.create_render_target(kSize, kSize);
  std::vector<double> frame_ms;
  double sim_time = 0;
  uint32_t draws = 0, shots_written = 0;
  auto t_start = std::chrono::steady_clock::now();
  for (uint32_t f = 0; f < opts.frames; ++f) {
    auto t0 = std::chrono::steady_clock::now();
    sim_time += kFakeDeltaTime;
    scene.render(dev, rt, sim_time);
    draws += dev.last_frame_stats().draws;
    if (std::find(opts.shots.begin(), opts.shots.end(), f) != opts.shots.end()) {
      std::vector<uint8_t> px;
      if (dev.read_pixels(rt, px)) {
#if !defined(__EMSCRIPTEN__)
        std::string name = (fs::path(opts.out_dir) / ("shot_" + std::to_string(f) + ".png")).string();
        if (stbi_write_png(name.c_str(), kSize, kSize, 4, px.data(), kSize * 4)) ++shots_written;
#endif
      }
    }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    frame_ms.push_back(ms);
    float angle = TriangleScene::angle_at(sim_time);
    char line[160];
    std::snprintf(line, sizeof line, "{\"frame\":%u,\"sim_time\":%.17g,\"angle\":%.9g}\n", f, sim_time, angle);
    trace << line;
    if (records) records->push_back({f, sim_time, angle, ms});
  }
  double wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_start).count();
  trace.close();
  if (!trace) {
    std::fprintf(stderr, "failed writing %s/trace.jsonl\n", opts.out_dir.c_str());
    return false;
  }
  std::ofstream m(fs::path(opts.out_dir) / "metrics.json");
  m << "{\n \"frames\": " << frame_ms.size() << ",\n \"expected_frames\": " << opts.frames
    << ",\n \"backend\": \"" << dev.backend() << "\",\n \"adapter\": \"" << dev.adapter()
    << "\",\n \"wall_ms\": " << wall << ",\n \"frame_ms_p50\": " << percentile(frame_ms, 0.5)
    << ",\n \"frame_ms_p95\": " << percentile(frame_ms, 0.95) << ",\n \"draws_per_frame\": "
    << (opts.frames ? double(draws) / opts.frames : 0.0) << ",\n \"pipelines\": " << dev.last_frame_stats().pipelines_total
    << ",\n \"shots_written\": " << shots_written << "\n}\n";
  m.close();
  if (!m) {
    std::fprintf(stderr, "failed writing %s/metrics.json\n", opts.out_dir.c_str());
    return false;
  }
  return true;
}

}  // namespace lomn
