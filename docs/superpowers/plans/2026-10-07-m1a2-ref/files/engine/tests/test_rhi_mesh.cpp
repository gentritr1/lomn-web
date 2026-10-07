// M1a-2 rhi additions on the null backend: stable log text, shared validation (no UB on bad ids or sizes).
#include <string>
#include <vector>

#include "doctest.h"
#include "lomn/rhi.h"

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

bool has_line(const std::vector<std::string>& log, const std::string& prefix) {
  for (const auto& l : log)
    if (l.rfind(prefix, 0) == 0) return true;
  return false;
}

struct Scene {
  std::vector<std::string> log;
  std::unique_ptr<Device> d = create_null_device(&log);
  TextureId rt = 0, tex = 0;
  BufferId ub = 0, vb = 0, ib = 0;
  SamplerId s = 0;
  PipelineId p = 0;
  BindGroupId bg = 0;
  Scene() {
    RenderTargetDesc rd;
    rd.width = 64;
    rd.height = 32;
    rd.depth = true;
    rt = d->create_render_target_ex(rd);
    ub = d->create_uniform_buffer(512);
    vb = d->create_vertex_buffer(36 * 4);
    ib = d->create_index_buffer(6 * 4);
    tex = d->create_texture(2, 2, 2);
    s = d->create_sampler(SamplerDesc());
    p = d->create_mesh_pipeline(mesh_desc());
    bg = d->create_bind_group({ub, 96, tex, s});
  }
  DrawIndexedDesc draw() const {
    DrawIndexedDesc x;
    x.pipeline = p;
    x.vertices = vb;
    x.indices = ib;
    x.index_count = 6;
    x.bind_group = bg;
    return x;
  }
};

}  // namespace

TEST_CASE("rhi mesh: creation logs stable text, ids per kind shared with the M0 calls") {
  Scene sc;
  REQUIRE(sc.log.size() == 8);
  CHECK(sc.log[0] == "create_render_target_ex 64x32 depth=1 clear_depth=1.000 -> 1");
  CHECK(sc.log[1] == "create_uniform_buffer 512 -> 1");
  CHECK(sc.log[2] == "create_vertex_buffer 144 -> 2");
  CHECK(sc.log[3] == "create_index_buffer 24 -> 3");
  CHECK(sc.log[4] == "create_texture 2x2 mips=2 -> 2");  // texture ids continue after the render target
  CHECK(sc.log[5] == "create_sampler mag=linear min=linear mip=linear u=wrap v=wrap aniso=1 -> 1");
  CHECK(sc.log[6] ==
        "create_mesh_pipeline wgsl=1 bytes sum=00000077 vs=vs fs=fs uniforms=96 stride=36 attrs=[0:f32x3@0,1:f32x2@12,2:f32x4@20] "
        "blend=opaque depth_write=1 depth=lessequal cull=none -> 1");
  CHECK(sc.log[7] == "create_bind_group uniforms=1 bytes=96 texture=2 sampler=1 -> 1");
  CHECK(sc.d->create_render_target(8, 8) == 3);  // M0 call keeps working, next texture id
  CHECK(sc.d->last_frame_stats().pipelines_total == 1);
}

TEST_CASE("rhi mesh: write_texture checks the mip size and logs a content checksum") {
  Scene sc;
  const uint8_t px4[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  CHECK(sc.d->write_texture(sc.tex, 0, px4, 16));
  CHECK(sc.d->write_texture(sc.tex, 1, px4, 4));
  CHECK_FALSE(sc.d->write_texture(sc.tex, 1, px4, 16));  // mip 1 of 2x2 is 1x1
  CHECK_FALSE(sc.d->write_texture(sc.tex, 2, px4, 4));   // only 2 levels
  CHECK_FALSE(sc.d->write_texture(sc.rt, 0, px4, 16));   // a render target is not a sampled texture
  CHECK_FALSE(sc.d->write_texture(99, 0, px4, 16));
  CHECK_FALSE(sc.d->write_texture(sc.tex, 0, nullptr, 16));
  CHECK(has_line(sc.log, "write_texture 2 mip=0 2x2 16 bytes sum="));
  CHECK(has_line(sc.log, "write_texture 2 mip=1 1x1 4 bytes sum="));
  CHECK(has_line(sc.log, "write_texture 2 mip=1 16 bytes rejected: byte count does not match the mip size"));
  CHECK(has_line(sc.log, "write_texture 99 mip=0 16 bytes rejected: unknown sampled texture"));
}

TEST_CASE("rhi mesh: invalid creations return 0 and are logged") {
  std::vector<std::string> log;
  auto d = create_null_device(&log);
  RenderTargetDesc rd;
  CHECK(d->create_render_target_ex(rd) == 0);  // 0x0
  rd.width = rd.height = 8;
  rd.clear_depth = 2.0f;
  CHECK(d->create_render_target_ex(rd) == 0);
  CHECK(d->create_vertex_buffer(0) == 0);
  CHECK(d->create_index_buffer((256u << 20) + 1) == 0);
  CHECK(d->create_texture(0, 4, 1) == 0);
  CHECK(d->create_texture(4097, 4, 1) == 0);
  CHECK(d->create_texture(4, 4, 4) == 0);  // 4x4 has 3 levels
  SamplerDesc sd;
  sd.max_anisotropy = 3;
  sd.mip = FilterMode::Nearest;
  CHECK(d->create_sampler(sd) == 0);
  sd.max_anisotropy = 17;
  sd.mip = FilterMode::Linear;
  CHECK(d->create_sampler(sd) == 0);
  MeshPipelineDesc pd = mesh_desc();
  pd.attributes[2].offset = 24;  // 24 + 16 > 36
  CHECK(d->create_mesh_pipeline(pd) == 0);
  pd = mesh_desc();
  pd.attributes[1].location = 0;
  CHECK(d->create_mesh_pipeline(pd) == 0);
  pd = mesh_desc();
  pd.uniform_bytes = 0;
  CHECK(d->create_mesh_pipeline(pd) == 0);
  pd = mesh_desc();
  pd.vertex_stride = 34;
  CHECK(d->create_mesh_pipeline(pd) == 0);
  CHECK(d->last_frame_stats().pipelines_total == 0);
  CHECK(d->create_bind_group({1, 96, 1, 1}) == 0);  // nothing exists
  REQUIRE(log.size() == 14);
  for (const auto& l : log) CHECK(l.find("rejected: ") != std::string::npos);
  CHECK(log[6] == "create_texture 4x4 mips=4 rejected: mip count out of range -> 0");
}

TEST_CASE("rhi mesh: bind groups need a uniform buffer, a sampled texture and a sampler") {
  Scene sc;
  CHECK(sc.d->create_bind_group({sc.vb, 96, sc.tex, sc.s}) == 0);     // vertex buffer as uniforms
  CHECK(sc.d->create_bind_group({sc.ub, 96, sc.rt, sc.s}) == 0);      // render target as texture
  CHECK(sc.d->create_bind_group({sc.ub, 96, sc.tex, 7}) == 0);        // unknown sampler
  CHECK(sc.d->create_bind_group({sc.ub, 96, sc.tex, 0}) == 0);        // missing sampler (all 3 slots required in M1a-2)
  CHECK(sc.d->create_bind_group({sc.ub, 0, sc.tex, sc.s}) == 0);
  CHECK(sc.d->create_bind_group({sc.ub, 65537, sc.tex, sc.s}) == 0);  // over kMaxUniformBytes
  CHECK(sc.d->create_bind_group({sc.ub, 513, sc.tex, sc.s}) == 0);    // stride 768 > the 512-byte buffer
  CHECK(sc.d->create_bind_group({sc.ub, 96, sc.tex, sc.s}) == 2);
  CHECK(sc.d->create_bind_group({sc.ub, 257, sc.tex, sc.s}) == 3);    // stride 512 == the buffer
  CHECK(sc.d->last_frame_stats().rejected == 7);
  CHECK(sc.log.back() == "create_bind_group uniforms=1 bytes=257 texture=2 sampler=1 -> 3");
}

TEST_CASE("rhi mesh: uniform blocks up to 65536 bytes use a 256-aligned slot stride") {
  CHECK(uniform_stride(1) == 256);
  CHECK(uniform_stride(256) == 256);
  CHECK(uniform_stride(257) == 512);
  CHECK(uniform_stride(65536) == 65536);
  std::vector<std::string> log;
  auto d = create_null_device(&log);
  MeshPipelineDesc pd = mesh_desc();
  pd.uniform_bytes = 65537;
  CHECK(d->create_mesh_pipeline(pd) == 0);
  pd.uniform_bytes = 65536;
  CHECK(d->create_mesh_pipeline(pd) == 1);
  pd.uniform_bytes = 257;
  PipelineId p257 = d->create_mesh_pipeline(pd);
  REQUIRE(p257 == 2);
  RenderTargetDesc rd;
  rd.width = rd.height = 4;
  TextureId rt = d->create_render_target_ex(rd);
  BufferId ub = d->create_uniform_buffer(1024), vb = d->create_vertex_buffer(144), ib = d->create_index_buffer(24);
  TextureId tex = d->create_texture(1, 1, 1);
  SamplerId s = d->create_sampler(SamplerDesc());
  BindGroupId bg = d->create_bind_group({ub, 257, tex, s});
  REQUIRE(bg != 0);
  const float clear[4] = {0, 0, 0, 1};
  d->begin_frame(rt, clear);
  DrawIndexedDesc x{p257, vb, ib, 0, 6, bg, 0};
  d->draw_indexed(x);       // slot [0, 512)
  x.uniform_offset = 512;   // slot [512, 1024)
  d->draw_indexed(x);
  x.uniform_offset = 768;   // 256-aligned, but [768, 1280) leaves the buffer
  d->draw_indexed(x);
  x.uniform_offset = 256;   // dynamic offsets stay 256-aligned, not stride-aligned
  d->draw_indexed(x);
  pd.uniform_bytes = 96;    // stride 256 != 512: another layout
  x.pipeline = d->create_mesh_pipeline(pd);
  x.uniform_offset = 0;
  d->draw_indexed(x);
  d->end_frame();
  CHECK(d->last_frame_stats().draws == 3);
  CHECK(d->last_frame_stats().rejected == 2);
  CHECK(log[log.size() - 5].find("bind_group=1+768 rejected: uniform range outside the buffer") != std::string::npos);
  CHECK(log[log.size() - 2] == "draw_indexed pipeline=3 vb=2 ib=3 first=0 count=6 bind_group=1+0 rejected: bind group does not match the pipeline layout");
}

TEST_CASE("rhi mesh: M0 draw() is validated too (unknown ids, mesh pipelines, uniform range) and counts rejections") {
  std::vector<std::string> log;
  auto d = create_null_device(&log);
  PipelineDesc m0;
  m0.wgsl = "x";
  m0.uniform_bytes = 16;
  PipelineId p = d->create_pipeline(m0);
  BufferId ub = d->create_uniform_buffer(256);
  PipelineId mesh = d->create_mesh_pipeline(mesh_desc());
  TextureId rt = d->create_render_target(4, 4);
  const float clear[4] = {0, 0, 0, 1};
  d->begin_frame(rt, clear);
  d->draw(p, 3, ub, 0);
  d->draw(9, 3, ub, 0);
  d->draw(mesh, 3, ub, 0);
  d->draw(p, 3, 7, 0);
  d->draw(p, 3, ub, 256);
  d->draw(p, 3, ub, 16);
  d->end_frame();
  CHECK(d->last_frame_stats().draws == 1);
  CHECK(d->last_frame_stats().rejected == 5);
  CHECK(log[5] == "draw pipeline=1 vertices=3 uniforms=1+0");  // valid M0 text unchanged
  CHECK(log[6] == "draw pipeline=9 vertices=3 uniforms=1+0 rejected: unknown M0 pipeline");
  CHECK(log[7] == "draw pipeline=2 vertices=3 uniforms=1+0 rejected: unknown M0 pipeline");
  CHECK(log[8] == "draw pipeline=1 vertices=3 uniforms=7+0 rejected: unknown uniform buffer");
  CHECK(log[9] == "draw pipeline=1 vertices=3 uniforms=1+256 rejected: uniform range outside the buffer");
  CHECK(log[10] == "draw pipeline=1 vertices=3 uniforms=1+16 rejected: uniform_offset not aligned");
  d->begin_frame(rt, clear);
  CHECK(d->last_frame_stats().rejected == 0);  // per frame
}

TEST_CASE("rhi mesh: write_buffer validates ids, alignment and range (valid M0 writes unchanged)") {
  Scene sc;
  const uint32_t data[9] = {};
  sc.d->write_buffer(sc.vb, 0, data, 36);
  sc.d->write_buffer(sc.vb, 140, data, 8);  // past the 144-byte buffer
  sc.d->write_buffer(sc.vb, 2, data, 4);    // unaligned offset
  sc.d->write_buffer(sc.vb, 0, data, 6);    // unaligned size
  sc.d->write_buffer(42, 0, data, 4);
  sc.d->write_buffer(sc.ub, 0, nullptr, 4);
  CHECK(sc.log[8].rfind("write_buffer 2 +0 36 bytes sum=", 0) == 0);
  CHECK(sc.log[9] == "write_buffer 2 +140 8 bytes rejected: write outside the buffer");
  CHECK(sc.log[10] == "write_buffer 2 +2 4 bytes rejected: offset/size not a multiple of 4");
  CHECK(sc.log[11] == "write_buffer 2 +0 6 bytes rejected: offset/size not a multiple of 4");
  CHECK(sc.log[12] == "write_buffer 42 +0 4 bytes rejected: unknown buffer");
  CHECK(sc.log[13] == "write_buffer 1 +0 4 bytes rejected: null data");
}

TEST_CASE("rhi mesh: draw_indexed counts valid draws and rejects every bad id or range") {
  Scene sc;
  const float clear[4] = {0, 0, 0, 1};
  sc.d->begin_frame(sc.rt, clear);
  sc.d->draw_indexed(sc.draw());
  DrawIndexedDesc x = sc.draw();
  x.uniform_offset = 256;  // second slot of the 512-byte buffer
  sc.d->draw_indexed(x);
  auto rejected = [&](DrawIndexedDesc bad, const char* why) {
    size_t before = sc.log.size();
    sc.d->draw_indexed(bad);
    REQUIRE(sc.log.size() == before + 1);
    CHECK(sc.log.back().find(std::string("rejected: ") + why) != std::string::npos);
  };
  x = sc.draw(); x.uniform_offset = 512; rejected(x, "uniform range outside the buffer");
  x = sc.draw(); x.uniform_offset = 128; rejected(x, "uniform_offset not aligned");
  x = sc.draw(); x.first_index = 1; rejected(x, "index range outside the index buffer");
  x = sc.draw(); x.index_count = 4; rejected(x, "index_count not a positive multiple of 3");
  x = sc.draw(); x.index_count = 0; rejected(x, "index_count not a positive multiple of 3");
  x = sc.draw(); x.first_index = 0xffffffffu; x.index_count = 3; rejected(x, "index range outside the index buffer");
  x = sc.draw(); x.vertices = sc.ib; rejected(x, "unknown vertex buffer");
  x = sc.draw(); x.indices = sc.vb; rejected(x, "unknown index buffer");
  x = sc.draw(); x.pipeline = 9; rejected(x, "unknown mesh pipeline");
  x = sc.draw(); x.bind_group = 9; rejected(x, "unknown bind group");
  PipelineDesc m0;
  m0.wgsl = "x";
  m0.uniform_bytes = 16;
  x = sc.draw(); x.pipeline = sc.d->create_pipeline(m0); rejected(x, "unknown mesh pipeline");  // M0 pipeline
  MeshPipelineDesc other = mesh_desc();
  other.uniform_bytes = 300;  // stride 512 vs the bind group's 256 (64 would share the 256 stride: compatible)
  x = sc.draw(); x.pipeline = sc.d->create_mesh_pipeline(other); rejected(x, "bind group does not match the pipeline layout");
  sc.d->end_frame();
  CHECK(sc.d->last_frame_stats().draws == 2);
  CHECK(sc.d->last_frame_stats().indices == 12);
  CHECK(sc.d->last_frame_stats().rejected == 12);
  CHECK(sc.log.back() == "end_frame draws=2");
  CHECK(has_line(sc.log, "draw_indexed pipeline=1 vb=2 ib=3 first=0 count=6 bind_group=1+256"));
}

namespace {
// A Device written against the M0 surface only (like test_loop's HostileNameDevice): still compiles, and every M1a-2
// call reports "unsupported" through the base-class defaults.
class M0OnlyDevice : public Device {
 public:
  std::string backend() const override { return "m0"; }
  std::string adapter() const override { return "none"; }
  TextureId create_render_target(uint32_t, uint32_t) override { return 1; }
  BufferId create_uniform_buffer(uint32_t) override { return 1; }
  void write_buffer(BufferId, uint32_t, const void*, uint32_t) override {}
  PipelineId create_pipeline(const PipelineDesc&) override { return 1; }
  void begin_frame(TextureId, const float*) override {}
  void draw(PipelineId, uint32_t, BufferId, uint32_t) override {}
  void end_frame() override {}
  bool read_pixels(TextureId, std::vector<uint8_t>&) override { return false; }
  FrameStats last_frame_stats() const override { return {}; }
};
}  // namespace

TEST_CASE("rhi mesh: the additions are optional for M0-only devices (defaults report unsupported)") {
  M0OnlyDevice d;
  CHECK(d.create_render_target_ex(RenderTargetDesc{64, 64, true, 1.0f}) == 0);
  CHECK(d.create_vertex_buffer(64) == 0);
  CHECK(d.create_index_buffer(64) == 0);
  CHECK(d.create_texture(4, 4, 1) == 0);
  const uint8_t px[4] = {};
  CHECK_FALSE(d.write_texture(1, 0, px, 4));
  CHECK(d.create_sampler(SamplerDesc()) == 0);
  CHECK(d.create_mesh_pipeline(mesh_desc()) == 0);
  CHECK(d.create_bind_group({1, 96, 1, 1}) == 0);
  d.draw_indexed(DrawIndexedDesc());
}
