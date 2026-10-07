// Deterministic frame loop (M0 skeleton). Reproduces the original ScProcessManager fake-delta mode:
// every frame advances simulated time by exactly kFakeDeltaTime, independent of wall-clock time.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "lomn/rhi.h"

namespace lomn {

// ScProcessManager::sFakeDeltaTime initial value, read from LEGOBionicle.exe .data @ 0x712d68
// (1/150 s rounded through float). Never change by hand: it is the original game's step.
constexpr double kFakeDeltaTime = 0.006666666828095913;

struct LoopOptions {
  uint32_t frames = 0;               // number of frames to run
  std::vector<uint32_t> shots;       // frame indices to read back as PNG (backends that support readback)
  std::string out_dir;               // where trace.jsonl, metrics.json, calls.txt, shot_<n>.png go
};

// Spinning-triangle scene used until real game rendering exists. Pure function of sim_time.
class TriangleScene {
 public:
  void init(rhi::Device& dev);
  void render(rhi::Device& dev, rhi::TextureId target, double sim_time);
  static float angle_at(double sim_time);   // radians; 0.25 turns per simulated second

 private:
  rhi::PipelineId pipeline_ = 0;
  rhi::BufferId uniforms_ = 0;
};

struct FrameRecord {
  uint32_t frame;
  double sim_time;
  float angle;
  double wall_ms;
};

// Runs opts.frames frames headless into a render target. Writes:
//   trace.jsonl   {"frame":N,"sim_time":...,"angle":...} per frame (%.17g / %.9g) -- byte-identical across runs
//   metrics.json  frames, expected_frames, backend, adapter, wall/frame-time p50/p95, draws/frame, pipelines
//   shot_<N>.png  for each requested frame the backend can read back
// Returns false (and prints why) on I/O failure.
bool run_deterministic(rhi::Device& dev, const LoopOptions& opts, std::vector<FrameRecord>* records);

}  // namespace lomn
