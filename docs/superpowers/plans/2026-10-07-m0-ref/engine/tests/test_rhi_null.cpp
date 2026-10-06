#include "doctest.h"
#include "lomn/rhi.h"

TEST_CASE("null device: call log is stable text and counts draws") {
  std::vector<std::string> log;
  auto d = lomn::rhi::create_null_device(&log);
  CHECK(d->backend() == "null");
  auto rt = d->create_render_target(64, 64);
  auto ub = d->create_uniform_buffer(256);
  lomn::rhi::PipelineDesc pd;
  pd.wgsl = "x";
  pd.uniform_bytes = 16;
  auto p = d->create_pipeline(pd);
  float u[4] = {1, 0, 0, 0};
  d->write_buffer(ub, 0, u, sizeof u);
  const float c[4] = {0, 0, 0.25f, 1};
  d->begin_frame(rt, c);
  d->draw(p, 3, ub, 0);
  d->end_frame();
  CHECK(d->last_frame_stats().draws == 1);
  CHECK(d->last_frame_stats().pipelines_total == 1);
  std::vector<uint8_t> px;
  CHECK_FALSE(d->read_pixels(rt, px));
  REQUIRE(log.size() == 7);
  CHECK(log[0] == "create_render_target 64x64 -> 1");
  CHECK(log[2] == "create_pipeline wgsl=1 bytes vs=vs fs=fs uniforms=16 -> 1");
  CHECK(log[5] == "draw pipeline=1 vertices=3 uniforms=1+0");
  CHECK(log[6] == "end_frame draws=1");
}
