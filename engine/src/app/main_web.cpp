// Browser entry: acquire the WebGPU device asynchronously, then run the triangle scene on #canvas each
// animation frame with the deterministic fake-delta step. Publishes window.__lomn = {state, frames, metrics}.
#include <emscripten/emscripten.h>

#include <memory>

#include "lomn/loop.h"
#include "lomn/rhi.h"

namespace {
std::unique_ptr<lomn::rhi::Device> g_dev;
lomn::TriangleScene g_scene;
double g_sim_time = 0;
uint32_t g_frames = 0;
double g_last_ms = 0;

void frame() {
  double now = emscripten_get_now();
  double dt = g_last_ms ? now - g_last_ms : 0;
  g_last_ms = now;
  g_sim_time += lomn::kFakeDeltaTime;
  g_scene.render(*g_dev, 0, g_sim_time);
  ++g_frames;
  EM_ASM({
    const l = (window.__lomn = window.__lomn || {});
    l.frames = $0;
    l.metrics = l.metrics || {dts: []};
    if ($1 > 0) l.metrics.dts.push($1);
    if ($0 >= 3) l.state = 'running';
  }, g_frames, dt);
}
}  // namespace

int main() {
  EM_ASM({ (window.__lomn = window.__lomn || {}).state = 'starting'; });
  lomn::rhi::create_webgpu_device("#canvas", 64, 64, [](std::unique_ptr<lomn::rhi::Device> d, std::string err) {
    if (!d) {
      EM_ASM({ window.__lomn.state = 'error'; window.__lomn.error = UTF8ToString($0); }, err.c_str());
      return;
    }
    g_dev = std::move(d);
    g_scene.init(*g_dev);
    emscripten_set_main_loop(frame, 0, false);
  });
  return 0;
}
