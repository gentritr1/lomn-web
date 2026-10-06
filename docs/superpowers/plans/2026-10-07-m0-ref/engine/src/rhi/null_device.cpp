#include <cstdio>

#include "lomn/rhi.h"

namespace lomn::rhi {
namespace {

class NullDevice final : public Device {
 public:
  explicit NullDevice(std::vector<std::string>* log) : log_(log) {}
  std::string backend() const override { return "null"; }
  std::string adapter() const override { return "none"; }
  TextureId create_render_target(uint32_t w, uint32_t h) override {
    rec("create_render_target %ux%u -> %u", w, h, ++next_tex_);
    return next_tex_;
  }
  BufferId create_uniform_buffer(uint32_t bytes) override {
    rec("create_uniform_buffer %u -> %u", bytes, ++next_buf_);
    return next_buf_;
  }
  void write_buffer(BufferId b, uint32_t off, const void* data, uint32_t size) override {
    uint32_t sum = 0;  // simple checksum so the log captures content, not just size
    for (uint32_t i = 0; i < size; ++i) sum = sum * 31u + static_cast<const uint8_t*>(data)[i];
    rec("write_buffer %u +%u %u bytes sum=%08x", b, off, size, sum);
  }
  PipelineId create_pipeline(const PipelineDesc& d) override {
    ++stats_.pipelines_total;
    rec("create_pipeline wgsl=%zu bytes vs=%s fs=%s uniforms=%u -> %u", d.wgsl.size(), d.vs_entry.c_str(),
        d.fs_entry.c_str(), d.uniform_bytes, ++next_pipe_);
    return next_pipe_;
  }
  void begin_frame(TextureId t, const float c[4]) override {
    stats_.draws = 0;
    rec("begin_frame target=%u clear=%.3f,%.3f,%.3f,%.3f", t, c[0], c[1], c[2], c[3]);
  }
  void draw(PipelineId p, uint32_t n, BufferId u, uint32_t off) override {
    ++stats_.draws;
    rec("draw pipeline=%u vertices=%u uniforms=%u+%u", p, n, u, off);
  }
  void end_frame() override { rec("end_frame draws=%u", stats_.draws); }
  bool read_pixels(TextureId, std::vector<uint8_t>&) override { return false; }
  FrameStats last_frame_stats() const override { return stats_; }

 private:
  template <typename... A>
  void rec(const char* fmt, A... a) {
    if (!log_) return;
    char buf[256];
    std::snprintf(buf, sizeof buf, fmt, a...);
    log_->emplace_back(buf);
  }
  std::vector<std::string>* log_;
  uint32_t next_tex_ = 0, next_buf_ = 0, next_pipe_ = 0;
  FrameStats stats_;
};

}  // namespace

std::unique_ptr<Device> create_null_device(std::vector<std::string>* call_log) {
  return std::make_unique<NullDevice>(call_log);
}

}  // namespace lomn::rhi
