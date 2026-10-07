#include <cstdarg>
#include <cstdio>

#include "lomn/rhi.h"
#include "registry.h"

namespace lomn::rhi {
namespace {

using detail::BufInfo;
using detail::BufKind;
using detail::Registry;
using detail::TexInfo;
using detail::TexKind;

class NullDevice final : public Device {
 public:
  explicit NullDevice(std::vector<std::string>* log) : log_(log) {}
  std::string backend() const override { return "null"; }
  std::string adapter() const override { return "none"; }
  TextureId create_render_target(uint32_t w, uint32_t h) override {
    TextureId id = reg_.add_texture({TexKind::RenderTarget, w, h, 1, false});
    rec("create_render_target %ux%u -> %u", w, h, id);
    return id;
  }
  BufferId create_uniform_buffer(uint32_t bytes) override {
    // Capacity as the WebGPU backend allocates it (rounded up to kUniformAlign, u64), so both validate writes alike. Over
    // kMaxBufferBytes: registered without storage (counted as a rejection, M0 log text kept; every later use is rejected).
    const uint64_t cap = detail::uniform_capacity(bytes);
    if (bytes && cap == 0) ++stats_.rejected;
    BufferId id = reg_.add_buffer({BufKind::Uniform, static_cast<uint32_t>(cap)});
    rec("create_uniform_buffer %u -> %u", bytes, id);
    return id;
  }
  void write_buffer(BufferId b, uint32_t off, const void* data, uint32_t size) override {
    std::string why = reg_.check_write_buffer(b, off, size, data == nullptr);
    if (!why.empty()) {
      ++stats_.rejected;
      rec("write_buffer %u +%u %u bytes rejected: %s", b, off, size, why.c_str());
      return;
    }
    rec("write_buffer %u +%u %u bytes sum=%08x", b, off, size, detail::checksum(data, size));
  }
  PipelineId create_pipeline(const PipelineDesc& d) override {
    ++stats_.pipelines_total;
    PipelineId id = reg_.add_pipeline({false, d.uniform_bytes, 0});
    rec("create_pipeline wgsl=%zu bytes vs=%s fs=%s uniforms=%u -> %u", d.wgsl.size(), d.vs_entry.c_str(), d.fs_entry.c_str(),
        d.uniform_bytes, id);
    return id;
  }
  void begin_frame(TextureId t, const float c[4]) override {
    stats_.draws = 0;
    stats_.indices = 0;
    stats_.rejected = 0;
    std::string why = reg_.check_begin_frame(t, false);  // the null device never has a presentation surface
    frame_rejected_ = !why.empty();
    frame_open_ = true;
    if (frame_rejected_) {
      ++stats_.rejected;
      rec("begin_frame target=%u clear=%.3f,%.3f,%.3f,%.3f rejected: %s", t, c[0], c[1], c[2], c[3], why.c_str());
      return;
    }
    rec("begin_frame target=%u clear=%.3f,%.3f,%.3f,%.3f", t, c[0], c[1], c[2], c[3]);
  }
  void draw(PipelineId p, uint32_t n, BufferId u, uint32_t off) override {
    if (!frame_open_) {  // M1a-3 parity with WebGPU: a draw outside begin_frame/end_frame is a counted rejection
      ++stats_.rejected;
      rec("draw pipeline=%u vertices=%u uniforms=%u+%u rejected: outside a frame", p, n, u, off);
      return;
    }
    if (frame_rejected_) {  // as WebGPU: a rejected frame's draws are not drawn and not counted (not rejected again)
      rec("draw pipeline=%u vertices=%u uniforms=%u+%u skipped: frame rejected", p, n, u, off);
      return;
    }
    std::string why = reg_.check_draw(p, u, off);
    if (!why.empty()) {
      ++stats_.rejected;
      rec("draw pipeline=%u vertices=%u uniforms=%u+%u rejected: %s", p, n, u, off, why.c_str());
      return;
    }
    ++stats_.draws;
    rec("draw pipeline=%u vertices=%u uniforms=%u+%u", p, n, u, off);
  }
  void end_frame() override {
    frame_rejected_ = false;
    frame_open_ = false;
    rec("end_frame draws=%u", stats_.draws);
  }
  bool read_pixels(TextureId, std::vector<uint8_t>&) override { return false; }
  FrameStats last_frame_stats() const override { return stats_; }

  // ---- M1a-2 ----
  TextureId create_render_target_ex(const RenderTargetDesc& d) override {
    std::string why = Registry::check_render_target(d);
    if (!why.empty()) {
      ++stats_.rejected;
      rec("create_render_target_ex %ux%u depth=%d rejected: %s -> 0", d.width, d.height, int(d.depth), why.c_str());
      return 0;
    }
    TextureId id = reg_.add_texture({TexKind::RenderTarget, d.width, d.height, 1, d.depth});
    rec("create_render_target_ex %ux%u depth=%d clear_depth=%.3f -> %u", d.width, d.height, int(d.depth), d.clear_depth, id);
    return id;
  }
  BufferId create_vertex_buffer(uint32_t bytes) override { return make_buffer("create_vertex_buffer", BufKind::Vertex, bytes); }
  BufferId create_index_buffer(uint32_t bytes) override { return make_buffer("create_index_buffer", BufKind::Index, bytes); }
  TextureId create_texture(uint32_t w, uint32_t h, uint32_t mips) override {
    std::string why = Registry::check_texture(w, h, mips);
    if (!why.empty()) {
      ++stats_.rejected;
      rec("create_texture %ux%u mips=%u rejected: %s -> 0", w, h, mips, why.c_str());
      return 0;
    }
    TextureId id = reg_.add_texture({TexKind::Sampled, w, h, mips, false});
    rec("create_texture %ux%u mips=%u -> %u", w, h, mips, id);
    return id;
  }
  bool write_texture(TextureId t, uint32_t mip, const void* rgba, size_t bytes) override {
    std::string why = reg_.check_write_texture(t, mip, bytes, rgba == nullptr);
    if (!why.empty()) {
      ++stats_.rejected;
      rec("write_texture %u mip=%u %llu bytes rejected: %s", t, mip, static_cast<unsigned long long>(bytes), why.c_str());
      return false;
    }
    const TexInfo& ti = reg_.textures[t];
    rec("write_texture %u mip=%u %ux%u %llu bytes sum=%08x", t, mip, detail::mip_dim(ti.width, mip), detail::mip_dim(ti.height, mip),
        static_cast<unsigned long long>(bytes), detail::checksum(rgba, bytes));
    return true;
  }
  SamplerId create_sampler(const SamplerDesc& s) override {
    std::string why = Registry::check_sampler(s);
    if (!why.empty()) {
      ++stats_.rejected;
      rec("create_sampler rejected: %s -> 0", why.c_str());
      return 0;
    }
    SamplerId id = reg_.add_sampler(s);
    rec("create_sampler mag=%s min=%s mip=%s u=%s v=%s aniso=%u -> %u", detail::filter_name(s.mag), detail::filter_name(s.min),
        detail::filter_name(s.mip), detail::address_name(s.u), detail::address_name(s.v), unsigned(s.max_anisotropy), id);
    return id;
  }
  PipelineId create_mesh_pipeline(const MeshPipelineDesc& d) override {
    std::string why = Registry::check_mesh_pipeline(d);
    if (!why.empty()) {
      ++stats_.rejected;
      rec("create_mesh_pipeline rejected: %s -> 0", why.c_str());
      return 0;
    }
    ++stats_.pipelines_total;
    PipelineId id = reg_.add_pipeline({true, d.uniform_bytes, detail::kMeshSlots});
    std::string attrs;
    for (const VertexAttribute& a : d.attributes) {
      char b[48];
      std::snprintf(b, sizeof b, "%s%u:%s@%u", attrs.empty() ? "" : ",", a.location, detail::vertex_format_name(a.format), a.offset);
      attrs += b;
    }
    // M1a-3 fields are appended only when not default, so M1a-2 pipeline lines keep their exact text.
    std::string extra;
    if (d.front_face != FrontFace::CCW) extra += " front=cw";
    if (d.depth_bias != 0) extra += " depth_bias=" + std::to_string(d.depth_bias);
    rec("create_mesh_pipeline wgsl=%zu bytes sum=%08x vs=%s fs=%s uniforms=%u stride=%u attrs=[%s] blend=%s depth_write=%d depth=%s cull=%s%s -> %u",
        d.wgsl.size(), detail::checksum(d.wgsl.data(), d.wgsl.size()), d.vs_entry.c_str(), d.fs_entry.c_str(), d.uniform_bytes,
        d.vertex_stride, attrs.c_str(), detail::blend_name(d.blend), int(d.depth_write), detail::compare_name(d.depth_compare),
        detail::cull_name(d.cull), extra.c_str(), id);
    return id;
  }
  BindGroupId create_bind_group(const BindGroupDesc& d) override {
    const BufferId u = d.uniforms;
    const uint32_t ubytes = d.uniform_bytes;
    const TextureId t = d.texture;
    const SamplerId s = d.sampler;
    std::string why = reg_.check_bind_group(d);
    if (!why.empty()) {
      ++stats_.rejected;
      rec("create_bind_group uniforms=%u bytes=%u texture=%u sampler=%u rejected: %s -> 0", u, ubytes, t, s, why.c_str());
      return 0;
    }
    BindGroupId id = reg_.add_bind_group({d, static_cast<uint32_t>(uniform_stride(ubytes)), detail::bind_slots(d)});
    rec("create_bind_group uniforms=%u bytes=%u texture=%u sampler=%u -> %u", u, ubytes, t, s, id);
    return id;
  }
  void draw_indexed(const DrawIndexedDesc& d) override {
    if (!frame_open_) {  // M1a-3 parity with WebGPU (see draw)
      ++stats_.rejected;
      rec("draw_indexed pipeline=%u vb=%u ib=%u first=%u count=%u bind_group=%u+%u rejected: outside a frame", d.pipeline, d.vertices,
          d.indices, d.first_index, d.index_count, d.bind_group, d.uniform_offset);
      return;
    }
    if (frame_rejected_) {
      rec("draw_indexed pipeline=%u vb=%u ib=%u first=%u count=%u bind_group=%u+%u skipped: frame rejected", d.pipeline, d.vertices,
          d.indices, d.first_index, d.index_count, d.bind_group, d.uniform_offset);
      return;
    }
    std::string why = reg_.check_draw_indexed(d);
    if (!why.empty()) {
      ++stats_.rejected;
      rec("draw_indexed pipeline=%u vb=%u ib=%u first=%u count=%u bind_group=%u+%u rejected: %s", d.pipeline, d.vertices, d.indices,
          d.first_index, d.index_count, d.bind_group, d.uniform_offset, why.c_str());
      return;
    }
    ++stats_.draws;
    stats_.indices += d.index_count;
    rec("draw_indexed pipeline=%u vb=%u ib=%u first=%u count=%u bind_group=%u+%u", d.pipeline, d.vertices, d.indices, d.first_index,
        d.index_count, d.bind_group, d.uniform_offset);
  }

  // ---- M1b ----
  bool destroy_texture(TextureId t) override { return destroy("destroy_texture", t, reg_.check_destroy_texture(t), reg_.textures); }
  bool destroy_buffer(BufferId b) override { return destroy("destroy_buffer", b, reg_.check_destroy_buffer(b), reg_.buffers); }
  bool destroy_pipeline(PipelineId p) override { return destroy("destroy_pipeline", p, reg_.check_destroy_pipeline(p), reg_.pipelines); }
  bool destroy_sampler(SamplerId s) override { return destroy("destroy_sampler", s, reg_.check_destroy_sampler(s), reg_.samplers); }
  bool destroy_bind_group(BindGroupId g) override {
    return destroy("destroy_bind_group", g, reg_.check_destroy_bind_group(g), reg_.bind_groups);
  }
  LiveObjects live_objects() const override { return reg_.live(); }
  // The null device never has a presentation surface: both are counted rejections with stable log lines.
  bool set_surface_depth(bool on, float clear_depth) override {
    ++stats_.rejected;
    rec("set_surface_depth %d clear_depth=%.3f rejected: no presentation surface (headless device)", int(on), clear_depth);
    return false;
  }
  bool resize(uint32_t w, uint32_t h) override {
    ++stats_.rejected;
    rec("resize %ux%u rejected: no presentation surface (headless device)", w, h);
    return false;
  }
  // REF4 r2: the null device is never lost; the call is logged (the handler is kept, never invoked).
  void set_lost_handler(std::function<void(const std::string&)> h) override {
    lost_handler_ = std::move(h);
    rec("set_lost_handler %s", lost_handler_ ? "set" : "cleared");
  }

 private:
  template <typename Map>
  bool destroy(const char* what, uint32_t id, const std::string& why, Map& table) {
    if (!why.empty()) {
      ++stats_.rejected;
      rec("%s %u rejected: %s", what, id, why.c_str());
      return false;
    }
    table.erase(id);
    rec("%s %u", what, id);
    return true;
  }
  BufferId make_buffer(const char* what, BufKind kind, uint32_t bytes) {
    std::string why = Registry::check_buffer_bytes(bytes);
    if (!why.empty()) {
      ++stats_.rejected;
      rec("%s %u rejected: %s -> 0", what, bytes, why.c_str());
      return 0;
    }
    BufferId id = reg_.add_buffer({kind, bytes});
    rec("%s %u -> %u", what, bytes, id);
    return id;
  }
  // Lines of any length (M1a-2 pipeline lines exceed M0's 256-byte buffer); identical text for M0 calls.
  void rec(const char* fmt, ...) {
    if (!log_) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (static_cast<size_t>(n) < sizeof buf) {
      log_->emplace_back(buf, static_cast<size_t>(n));
      return;
    }
    std::string s(static_cast<size_t>(n) + 1, '\0');
    va_start(ap, fmt);
    std::vsnprintf(&s[0], s.size(), fmt, ap);
    va_end(ap);
    s.resize(static_cast<size_t>(n));
    log_->push_back(std::move(s));
  }
  std::vector<std::string>* log_;
  Registry reg_;
  FrameStats stats_;
  bool frame_rejected_ = false;  // the current frame's begin_frame was rejected
  bool frame_open_ = false;      // between begin_frame and end_frame
  std::function<void(const std::string&)> lost_handler_;
};

}  // namespace

std::unique_ptr<Device> create_null_device(std::vector<std::string>* call_log) {
  return std::make_unique<NullDevice>(call_log);
}

}  // namespace lomn::rhi
