// M1b rhi additions on the null backend: destroy_* (dependency rule: a referenced object is rejected, never cascaded),
// live_objects() and the surface calls on a headless device (AreaRenderer::release follows in T2).
// The log lines are exact: this suite runs natively AND as wasm32 under Node, so both produce the same text.
#include <string>
#include <vector>

#include "doctest.h"
#include "lomn/fs.h"
#include "lomn/rhi.h"
#include "lomn/view/area.h"
#include "lomn/view/synthetic.h"

using namespace lomn;
using namespace lomn::rhi;

namespace {

MeshPipelineDesc mesh_desc() {
  MeshPipelineDesc d;
  d.wgsl = "w";
  d.uniform_bytes = 96;
  d.vertex_stride = 36;
  d.attributes = {{VertexFormat::Float32x3, 0, 0}, {VertexFormat::Float32x2, 12, 1}, {VertexFormat::Float32x4, 20, 2}};
  return d;
}

Device::LiveObjects counts(uint32_t t, uint32_t b, uint32_t p, uint32_t s, uint32_t g) {
  Device::LiveObjects o;
  o.textures = t;
  o.buffers = b;
  o.pipelines = p;
  o.samplers = s;
  o.bind_groups = g;
  return o;
}

// Forwards to a null device but rejects the n-th create_index_buffer: makes AreaRenderer::upload fail half-way.
class FailingDevice final : public Device {
 public:
  FailingDevice(std::unique_ptr<Device> d, int fail_index_buffer_at) : d_(std::move(d)), fail_at_(fail_index_buffer_at) {}
  std::string backend() const override { return d_->backend(); }
  std::string adapter() const override { return d_->adapter(); }
  TextureId create_render_target(uint32_t w, uint32_t h) override { return d_->create_render_target(w, h); }
  BufferId create_uniform_buffer(uint32_t b) override { return d_->create_uniform_buffer(b); }
  void write_buffer(BufferId b, uint32_t o, const void* p, uint32_t n) override { d_->write_buffer(b, o, p, n); }
  PipelineId create_pipeline(const PipelineDesc& x) override { return d_->create_pipeline(x); }
  void begin_frame(TextureId t, const float c[4]) override { d_->begin_frame(t, c); }
  void draw(PipelineId p, uint32_t n, BufferId u, uint32_t o) override { d_->draw(p, n, u, o); }
  void end_frame() override { d_->end_frame(); }
  bool read_pixels(TextureId t, std::vector<uint8_t>& v) override { return d_->read_pixels(t, v); }
  FrameStats last_frame_stats() const override { return d_->last_frame_stats(); }
  TextureId create_render_target_ex(const RenderTargetDesc& x) override { return d_->create_render_target_ex(x); }
  BufferId create_vertex_buffer(uint32_t n) override { return d_->create_vertex_buffer(n); }
  BufferId create_index_buffer(uint32_t n) override { return ++ib_ == fail_at_ ? 0 : d_->create_index_buffer(n); }
  TextureId create_texture(uint32_t w, uint32_t h, uint32_t m) override { return d_->create_texture(w, h, m); }
  bool write_texture(TextureId t, uint32_t m, const void* p, std::size_t n) override { return d_->write_texture(t, m, p, n); }
  SamplerId create_sampler(const SamplerDesc& x) override { return d_->create_sampler(x); }
  PipelineId create_mesh_pipeline(const MeshPipelineDesc& x) override { return d_->create_mesh_pipeline(x); }
  BindGroupId create_bind_group(const BindGroupDesc& x) override { return d_->create_bind_group(x); }
  void draw_indexed(const DrawIndexedDesc& x) override { d_->draw_indexed(x); }
  bool destroy_texture(TextureId t) override { return d_->destroy_texture(t); }
  bool destroy_buffer(BufferId b) override { return d_->destroy_buffer(b); }
  bool destroy_pipeline(PipelineId p) override { return d_->destroy_pipeline(p); }
  bool destroy_sampler(SamplerId s) override { return d_->destroy_sampler(s); }
  bool destroy_bind_group(BindGroupId g) override { return d_->destroy_bind_group(g); }
  LiveObjects live_objects() const override { return d_->live_objects(); }

 private:
  std::unique_ptr<Device> d_;
  int fail_at_, ib_ = 0;
};

}  // namespace

TEST_CASE("rhi destroy: a referenced texture/sampler/uniform buffer is rejected, never cascaded; ids are not reused") {
  std::vector<std::string> log;
  auto d = create_null_device(&log);
  const BufferId ub = d->create_uniform_buffer(512);
  const TextureId tex = d->create_texture(2, 2, 1);
  const SamplerId s = d->create_sampler(SamplerDesc());
  const PipelineId p = d->create_mesh_pipeline(mesh_desc());
  const BindGroupId bg = d->create_bind_group({ub, 96, tex, s});
  REQUIRE(bg == 1);
  CHECK(d->live_objects() == counts(1, 1, 1, 1, 1));
  log.clear();
  CHECK_FALSE(d->destroy_texture(tex));
  CHECK_FALSE(d->destroy_sampler(s));
  CHECK_FALSE(d->destroy_buffer(ub));
  CHECK(d->last_frame_stats().rejected == 3);
  CHECK(d->live_objects() == counts(1, 1, 1, 1, 1));  // nothing freed by a rejected destroy
  CHECK(d->destroy_pipeline(p));                       // pipelines are referenced by no rhi object
  CHECK(d->destroy_bind_group(bg));
  CHECK(d->destroy_texture(tex));
  CHECK(d->destroy_sampler(s));
  CHECK(d->destroy_buffer(ub));
  CHECK_FALSE(d->destroy_buffer(ub));  // already destroyed
  CHECK_FALSE(d->destroy_texture(0));  // 0 = the presentation surface: not a destroyable texture
  CHECK(d->live_objects() == counts(0, 0, 0, 0, 0));
  const std::vector<std::string> want = {
      "destroy_texture 1 rejected: in use by bind group 1",
      "destroy_sampler 1 rejected: in use by bind group 1",
      "destroy_buffer 1 rejected: in use by bind group 1",
      "destroy_pipeline 1",
      "destroy_bind_group 1",
      "destroy_texture 1",
      "destroy_sampler 1",
      "destroy_buffer 1",
      "destroy_buffer 1 rejected: unknown buffer",
      "destroy_texture 0 rejected: unknown texture",
  };
  CHECK(log == want);
  // Ids continue: never reused after a destroy.
  CHECK(d->create_texture(2, 2, 1) == 2);
  CHECK(d->create_uniform_buffer(256) == 2);
  CHECK(d->create_bind_group({1, 96, 2, 1}) == 0);  // the destroyed buffer/sampler ids are unknown now
}

TEST_CASE("rhi destroy: draws naming a destroyed object are rejected; a render target with depth is destroyable") {
  std::vector<std::string> log;
  auto d = create_null_device(&log);
  const TextureId rt = d->create_render_target_ex(RenderTargetDesc{16, 16, true, 1.0f});
  const BufferId ub = d->create_uniform_buffer(512), vb = d->create_vertex_buffer(144), ib = d->create_index_buffer(24);
  const TextureId tex = d->create_texture(2, 2, 1);
  const SamplerId s = d->create_sampler(SamplerDesc());
  const PipelineId p = d->create_mesh_pipeline(mesh_desc());
  const BindGroupId bg = d->create_bind_group({ub, 96, tex, s});
  const float c[4] = {0, 0, 0, 1};
  d->begin_frame(rt, c);
  d->draw_indexed({p, vb, ib, 0, 6, bg, 0});
  CHECK(d->destroy_pipeline(p));  // allowed inside a frame: the recorded draw keeps it (WebGPU refcounting)
  d->draw_indexed({p, vb, ib, 0, 6, bg, 0});
  d->end_frame();
  CHECK(d->last_frame_stats().draws == 1);
  CHECK(d->last_frame_stats().rejected == 1);
  CHECK(log[log.size() - 2] == "draw_indexed pipeline=1 vb=2 ib=3 first=0 count=6 bind_group=1+0 rejected: unknown mesh pipeline");
  CHECK(d->destroy_texture(rt));
  log.clear();
  d->begin_frame(rt, c);
  d->end_frame();
  CHECK(log.front() == "begin_frame target=1 clear=0.000,0.000,0.000,1.000 rejected: unknown render target");
}

TEST_CASE("rhi surface: set_surface_depth and resize are counted rejections on a device without a surface") {
  std::vector<std::string> log;
  auto d = create_null_device(&log);
  CHECK_FALSE(d->set_surface_depth(true, 1.0f));
  CHECK_FALSE(d->resize(640, 480));
  CHECK(d->last_frame_stats().rejected == 2);
  CHECK(log == std::vector<std::string>{"set_surface_depth 1 clear_depth=1.000 rejected: no presentation surface (headless device)",
                                        "resize 640x480 rejected: no presentation surface (headless device)"});
}

TEST_CASE("rhi lost handler (REF4 r2): the null device logs the call and never invokes the handler") {
  std::vector<std::string> log;
  auto d = create_null_device(&log);
  int calls = 0;
  d->set_lost_handler([&](const std::string&) { ++calls; });
  d->set_lost_handler(nullptr);
  CHECK(log == std::vector<std::string>{"set_lost_handler set", "set_lost_handler cleared"});
  CHECK(calls == 0);
  CHECK(d->last_frame_stats().rejected == 0);
}

