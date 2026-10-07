// M1a-2 rhi additions on the null backend: stable log text, shared validation (no UB on bad ids or sizes).
#include <cmath>
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

// ---- Task 6 cleanup (A2-A5, A7) ----

TEST_CASE("rhi mesh: an oversized uniform buffer keeps its M0 log line but has no storage (every use rejected)") {
  std::vector<std::string> log;
  auto d = create_null_device(&log);
  const BufferId huge = d->create_uniform_buffer(0xFFFFFFFFu);       // rounds up to 4 GiB: wrapped to 0 before
  const BufferId over = d->create_uniform_buffer((256u << 20) + 1);  // rounds up past kMaxBufferBytes
  const BufferId fits = d->create_uniform_buffer(256u << 20);        // exactly kMaxBufferBytes: usable
  REQUIRE(huge != 0);
  REQUIRE(over != 0);
  REQUIRE(fits != 0);
  CHECK(log[0] == "create_uniform_buffer 4294967295 -> 1");  // M0 text unchanged
  CHECK(log[1] == "create_uniform_buffer 268435457 -> 2");
  CHECK(d->last_frame_stats().rejected == 2);
  const uint32_t data[4] = {};
  d->write_buffer(huge, 0, data, 16);
  d->write_buffer(over, 0, data, 16);
  d->write_buffer(over, 0, data, 0);  // even an empty write: there is no buffer behind the id
  d->write_buffer(fits, (256u << 20) - 16, data, 16);
  CHECK(log[3] == "write_buffer 1 +0 16 bytes rejected: buffer has no storage");
  CHECK(log[4] == "write_buffer 2 +0 16 bytes rejected: buffer has no storage");
  CHECK(log[5] == "write_buffer 2 +0 0 bytes rejected: buffer has no storage");
  CHECK(log[6].rfind("write_buffer 3 +268435440 16 bytes sum=", 0) == 0);
  const TextureId tex = d->create_texture(1, 1, 1);
  const SamplerId s = d->create_sampler(SamplerDesc());
  CHECK(d->create_bind_group({huge, 96, tex, s}) == 0);
  CHECK(d->create_bind_group({over, 96, tex, s}) == 0);
  CHECK(d->create_bind_group({fits, 96, tex, s}) != 0);
  // M0 draw with a uniform block from a storage-less buffer.
  PipelineDesc m0;
  m0.wgsl = "x";
  m0.uniform_bytes = 16;
  const PipelineId p = d->create_pipeline(m0);
  const TextureId rt = d->create_render_target(4, 4);
  const float clear[4] = {0, 0, 0, 1};
  d->begin_frame(rt, clear);
  d->draw(p, 3, over, 0);
  d->end_frame();
  CHECK(d->last_frame_stats().draws == 0);
  CHECK(d->last_frame_stats().rejected == 1);
}

TEST_CASE("rhi mesh: uniform_stride is computed in 64 bits and never wraps to 0") {
  static_assert(sizeof(uniform_stride(1)) == 8, "uniform_stride returns uint64_t");
  CHECK(uniform_stride(0xFFFFFFFFu) == 0x100000000ull);
  CHECK(uniform_stride(0xFFFFFF01u) == 0x100000000ull);
  CHECK(uniform_stride(0xFFFFFF00u) == 0xFFFFFF00ull);
  CHECK(uniform_stride(0) == 0);
}

TEST_CASE("rhi mesh: every enum in a desc is range-checked (one value past the last enumerator is rejected)") {
  std::vector<std::string> log;
  auto d = create_null_device(&log);
  SamplerDesc sd;
  sd.mag = static_cast<FilterMode>(2);
  CHECK(d->create_sampler(sd) == 0);
  sd = SamplerDesc();
  sd.min = static_cast<FilterMode>(2);
  CHECK(d->create_sampler(sd) == 0);
  sd = SamplerDesc();
  sd.mip = static_cast<FilterMode>(2);
  CHECK(d->create_sampler(sd) == 0);
  sd = SamplerDesc();
  sd.u = static_cast<AddressMode>(3);
  CHECK(d->create_sampler(sd) == 0);
  sd = SamplerDesc();
  sd.v = static_cast<AddressMode>(3);
  CHECK(d->create_sampler(sd) == 0);
  sd = SamplerDesc();
  sd.mag = FilterMode::Nearest;
  sd.u = AddressMode::Mirror;  // the last valid values still pass
  CHECK(d->create_sampler(sd) == 1);
  MeshPipelineDesc pd = mesh_desc();
  pd.attributes[0].format = static_cast<VertexFormat>(3);
  CHECK(d->create_mesh_pipeline(pd) == 0);
  pd = mesh_desc();
  pd.blend = static_cast<BlendMode>(2);
  CHECK(d->create_mesh_pipeline(pd) == 0);
  pd = mesh_desc();
  pd.cull = static_cast<CullMode>(3);
  CHECK(d->create_mesh_pipeline(pd) == 0);
  pd = mesh_desc();
  pd.depth_compare = static_cast<CompareFunc>(8);
  CHECK(d->create_mesh_pipeline(pd) == 0);
  pd = mesh_desc();
  pd.attributes[2].format = VertexFormat::Float32x4;
  pd.blend = BlendMode::AlphaBlend;
  pd.cull = CullMode::Back;
  pd.depth_compare = CompareFunc::Always;
  CHECK(d->create_mesh_pipeline(pd) == 1);
  CHECK(d->last_frame_stats().rejected == 9);
  CHECK(has_line(log, "create_sampler rejected: bad filter mode -> 0"));
  CHECK(has_line(log, "create_sampler rejected: bad address mode -> 0"));
  CHECK(has_line(log, "create_mesh_pipeline rejected: bad vertex format -> 0"));
  CHECK(has_line(log, "create_mesh_pipeline rejected: bad blend mode -> 0"));
  CHECK(has_line(log, "create_mesh_pipeline rejected: bad cull mode -> 0"));
  CHECK(has_line(log, "create_mesh_pipeline rejected: bad depth compare -> 0"));
}

TEST_CASE("rhi mesh: begin_frame on an unknown, sampled or (headless) 0 target is a counted rejection; its draws are skipped") {
  Scene sc;
  const float clear[4] = {0, 0, 0, 1};
  for (TextureId bad : {TextureId(99), sc.tex, TextureId(0)}) {
    sc.log.clear();
    sc.d->begin_frame(bad, clear);
    sc.d->draw_indexed(sc.draw());
    sc.d->end_frame();
    const FrameStats st = sc.d->last_frame_stats();
    CHECK(st.rejected == 1);
    CHECK(st.draws == 0);
    REQUIRE(sc.log.size() == 3);
    CHECK(sc.log[1].find("skipped: frame rejected") != std::string::npos);
    CHECK(sc.log[2] == "end_frame draws=0");
  }
  sc.log.clear();
  sc.d->begin_frame(99, clear);
  CHECK(sc.log[0] == "begin_frame target=99 clear=0.000,0.000,0.000,1.000 rejected: unknown render target");
  sc.d->begin_frame(sc.tex, clear);
  CHECK(sc.log[1] == "begin_frame target=2 clear=0.000,0.000,0.000,1.000 rejected: unknown render target");
  sc.d->begin_frame(0, clear);
  CHECK(sc.log[2] == "begin_frame target=0 clear=0.000,0.000,0.000,1.000 rejected: no presentation surface (headless device)");
  // A valid frame afterwards draws normally (M0 text unchanged).
  sc.d->begin_frame(sc.rt, clear);
  sc.d->draw_indexed(sc.draw());
  sc.d->end_frame();
  CHECK(sc.log[3] == "begin_frame target=1 clear=0.000,0.000,0.000,1.000");
  CHECK(sc.d->last_frame_stats().draws == 1);
  CHECK(sc.d->last_frame_stats().rejected == 0);
}

TEST_CASE("rhi mesh: boundaries are pinned (buffers, targets, textures, mips, writes, uniform blocks)") {
  std::vector<std::string> log;
  auto d = create_null_device(&log);
  CHECK(d->create_vertex_buffer(256u << 20) != 0);         // exactly kMaxBufferBytes
  CHECK(d->create_vertex_buffer((256u << 20) + 1) == 0);
  CHECK(d->create_index_buffer(256u << 20) != 0);
  RenderTargetDesc rd;
  rd.width = rd.height = 8192;
  CHECK(d->create_render_target_ex(rd) != 0);
  rd.width = 8193;
  CHECK(d->create_render_target_ex(rd) == 0);
  rd.width = 8192;
  rd.height = 8193;
  CHECK(d->create_render_target_ex(rd) == 0);
  rd.height = 8;
  rd.clear_depth = std::nanf("");
  CHECK(d->create_render_target_ex(rd) == 0);
  rd.clear_depth = 0.0f;
  CHECK(d->create_render_target_ex(rd) != 0);
  CHECK(d->create_texture(4096, 4096, 13) != 0);  // full chain 4096 -> 1
  CHECK(d->create_texture(4096, 4096, 14) == 0);
  CHECK(d->create_texture(4097, 1, 1) == 0);
  CHECK(d->create_texture(1, 4097, 1) == 0);
  // Non-square chain: 4x1 -> 2x1 -> 1x1 (3 levels).
  const TextureId t41 = d->create_texture(4, 1, 3);
  REQUIRE(t41 != 0);
  CHECK(d->create_texture(4, 1, 4) == 0);
  const uint8_t px[16] = {};
  CHECK(d->write_texture(t41, 0, px, 16));
  CHECK(d->write_texture(t41, 1, px, 8));
  CHECK(d->write_texture(t41, 2, px, 4));
  CHECK_FALSE(d->write_texture(t41, 1, px, 4));
  CHECK(has_line(log, "write_texture " + std::to_string(t41) + " mip=1 2x1 8 bytes sum="));
  CHECK(has_line(log, "write_texture " + std::to_string(t41) + " mip=2 1x1 4 bytes sum="));
  // write_buffer: exact fit at the end; an offset + size that would wrap in 32 bits.
  const BufferId vb = d->create_vertex_buffer(144);
  log.clear();
  d->write_buffer(vb, 136, px, 8);
  d->write_buffer(vb, 0xFFFFFFFCu, px, 8);
  d->write_buffer(vb, 140, px, 8);
  CHECK(log[0].rfind("write_buffer " + std::to_string(vb) + " +136 8 bytes sum=", 0) == 0);
  CHECK(log[1].find("rejected: write outside the buffer") != std::string::npos);
  CHECK(log[2].find("rejected: write outside the buffer") != std::string::npos);
  // Uniform blocks: 65536 bytes bind; 65537 does not (kMaxUniformBytes), even with room in the buffer.
  const BufferId ub = d->create_uniform_buffer(2 * 65536);
  const TextureId tex = d->create_texture(1, 1, 1);
  const SamplerId s = d->create_sampler(SamplerDesc());
  CHECK(d->create_bind_group({ub, 65536, tex, s}) != 0);
  CHECK(d->create_bind_group({ub, 65537, tex, s}) == 0);
}

TEST_CASE("rhi mesh: draws outside begin_frame/end_frame are counted rejections (M1a-3 null/WebGPU parity)") {
  std::vector<std::string> log;
  auto dev = lomn::rhi::create_null_device(&log);
  const float clear[4] = {0, 0, 0, 1};
  lomn::rhi::TextureId rt = dev->create_render_target(4, 4);
  lomn::rhi::PipelineDesc pd;
  pd.wgsl = "x";
  lomn::rhi::PipelineId p = dev->create_pipeline(pd);
  dev->begin_frame(rt, clear);
  dev->end_frame();
  log.clear();
  dev->draw(p, 3, 0, 0);
  dev->draw_indexed(lomn::rhi::DrawIndexedDesc{});
  CHECK(dev->last_frame_stats().rejected == 2);
  CHECK(dev->last_frame_stats().draws == 0);
  REQUIRE(log.size() == 2);
  CHECK(log[0] == "draw pipeline=1 vertices=3 uniforms=0+0 rejected: outside a frame");
  CHECK(log[1].find("rejected: outside a frame") != std::string::npos);
}

// Task 8B A1: the M1a-3 MeshPipelineDesc fields are pinned at their boundaries, with their exact log text.
TEST_CASE("rhi mesh: depth_bias is accepted up to +-kMaxDepthBias, front_face past CW is rejected, both logged") {
  std::vector<std::string> log;
  auto d = create_null_device(&log);
  MeshPipelineDesc pd = mesh_desc();
  pd.depth_bias = kMaxDepthBias;
  CHECK(d->create_mesh_pipeline(pd) == 1);
  pd.depth_bias = -kMaxDepthBias;
  CHECK(d->create_mesh_pipeline(pd) == 2);
  pd.depth_bias = kMaxDepthBias + 1;
  CHECK(d->create_mesh_pipeline(pd) == 0);
  pd.depth_bias = -(kMaxDepthBias + 1);
  CHECK(d->create_mesh_pipeline(pd) == 0);
  pd.depth_bias = 0;
  pd.front_face = static_cast<FrontFace>(2);  // one past the last enumerator
  CHECK(d->create_mesh_pipeline(pd) == 0);
  pd.front_face = FrontFace::CW;
  pd.depth_bias = -16;
  CHECK(d->create_mesh_pipeline(pd) == 3);
  CHECK(d->last_frame_stats().rejected == 3);
  REQUIRE(log.size() == 6);
  CHECK(log[0].find(" cull=none depth_bias=65536 -> 1") != std::string::npos);
  CHECK(log[1].find(" cull=none depth_bias=-65536 -> 2") != std::string::npos);
  CHECK(log[2] == "create_mesh_pipeline rejected: depth_bias out of range -> 0");
  CHECK(log[3] == "create_mesh_pipeline rejected: depth_bias out of range -> 0");
  CHECK(log[4] == "create_mesh_pipeline rejected: bad front face -> 0");
  CHECK(log[5].find(" cull=none front=cw depth_bias=-16 -> 3") != std::string::npos);
  // Defaults (CCW, bias 0) print neither field: the M1a-2 line is unchanged.
  log.clear();
  CHECK(d->create_mesh_pipeline(mesh_desc()) == 4);
  CHECK(log[0].find(" cull=none -> 4") != std::string::npos);
}

TEST_CASE("rhi mesh: draws before the first begin_frame are counted rejections too (null)") {
  std::vector<std::string> log;
  auto dev = create_null_device(&log);
  PipelineDesc pd;
  pd.wgsl = "x";
  const PipelineId p = dev->create_pipeline(pd);
  log.clear();
  dev->draw(p, 3, 0, 0);  // no begin_frame has ever run on this device
  dev->draw_indexed(DrawIndexedDesc{});
  CHECK(dev->last_frame_stats().rejected == 2);
  CHECK(dev->last_frame_stats().draws == 0);
  REQUIRE(log.size() == 2);
  CHECK(log[0] == "draw pipeline=1 vertices=3 uniforms=0+0 rejected: outside a frame");
  CHECK(log[1] == "draw_indexed pipeline=0 vb=0 ib=0 first=0 count=0 bind_group=0+0 rejected: outside a frame");
}
