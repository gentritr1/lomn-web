// Render interface (frozen in M0; additive changes only, reviewed). Two backends: "null" (records a
// deterministic call log, no GPU) and "webgpu" (Dawn natively, emdawnwebgpu in the browser).
// M0 surface: render targets, one uniform block per pipeline (dynamic offset, 256-byte aligned), non-indexed draws.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace lomn::rhi {

using TextureId = uint32_t;   // 0 = the presentation surface (browser canvas); never valid headless
using BufferId = uint32_t;    // 0 = none
using PipelineId = uint32_t;
constexpr uint32_t kUniformAlign = 256;

struct PipelineDesc {
  std::string wgsl;               // must define the vertex/fragment entry points below
  std::string vs_entry = "vs";
  std::string fs_entry = "fs";
  uint32_t uniform_bytes = 0;     // 0 = no bind group; else @group(0) @binding(0) var<uniform>, dynamic offset
};

struct FrameStats {
  uint32_t draws = 0;
  uint32_t pipelines_total = 0;
};

class Device {
 public:
  virtual ~Device() = default;
  virtual std::string backend() const = 0;   // "null" | "webgpu"
  virtual std::string adapter() const = 0;   // adapter description, or "none"
  virtual TextureId create_render_target(uint32_t width, uint32_t height) = 0;  // RGBA8, readable
  virtual BufferId create_uniform_buffer(uint32_t bytes) = 0;
  virtual void write_buffer(BufferId buffer, uint32_t offset, const void* data, uint32_t size) = 0;
  virtual PipelineId create_pipeline(const PipelineDesc& desc) = 0;
  virtual void begin_frame(TextureId target, const float clear_rgba[4]) = 0;
  virtual void draw(PipelineId pipeline, uint32_t vertex_count, BufferId uniforms, uint32_t uniform_offset) = 0;
  virtual void end_frame() = 0;
  // Synchronous readback of a render target into tightly packed RGBA8. False if unsupported (null, browser).
  virtual bool read_pixels(TextureId target, std::vector<uint8_t>& rgba) = 0;
  virtual FrameStats last_frame_stats() const = 0;
};

// Null backend: appends one line per call to *call_log (if non-null). Lines are stable text, so two runs
// (or native vs wasm32) can be compared byte for byte.
std::unique_ptr<Device> create_null_device(std::vector<std::string>* call_log);

#if defined(LOMN_HAS_WEBGPU)
using DeviceCallback = std::function<void(std::unique_ptr<Device> device, std::string error)>;
// Asynchronous creation (the only form available in the browser). canvas_selector: CSS selector of the
// canvas to present to, or nullptr for headless. Natively the callback runs before this function returns.
void create_webgpu_device(const char* canvas_selector, uint32_t width, uint32_t height, DeviceCallback done);
#endif

}  // namespace lomn::rhi
