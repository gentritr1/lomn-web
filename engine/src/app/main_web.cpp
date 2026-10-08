// Browser entry (M1b viewer). Linked with INVOKE_RUN=0: web/app/engine.js instantiates the module, registers the
// install's file index (lomn_fs_add_file), prefetches the boot set into the wasm heap (lomn_boot_probe rounds, see
// below), and only then calls main(). main() acquires the WebGPU device asynchronously (no Asyncify) and starts the
// frame loop from the device callback.
//
// File bridge (lomn/prefetch_fs.h): reads come from a PrefetchFileSystem. A load runs in ROUNDS: each round starts a
// touch generation and runs the real load; every read whose bytes are not resident fails as a recorded MISS (the
// loader treats it like a missing file and continues). JavaScript fetches the misses (lomn_fs_misses) straight into
// heap buffers (lomn_fs_reserve -> HEAPU8.set per chunk -> lomn_fs_commit) and runs the next round, until a round has
// no miss. That round's reads are exactly the native access log of the same load (the Playwright smoke compares them
// with `lomn_view --manifest`). Bounded: JS stops after kMaxRounds rounds or a round that fetched nothing new.
//
// Viewer: one area at a time. lomn_area_show() releases the previous area's GPU objects (AreaRenderer::release), uploads
// the new one, frees its CPU copies and releases the previous area's prefetched ranges that the new load did not read
// (files shared by both stay resident). Free-fly camera: view::FlyCamera from the POS-start / exe-default camera.
// Metrics: lomn_metrics_json() (window.__lomn.metrics and the ?hud=1 overlay read it).
// Triangle mode (lomn_configure(..., 1)): the M0 scene, unchanged, for web/tests/smoke.mjs.
#include <emscripten/emscripten.h>
#include <emscripten/heap.h>
#include <malloc.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "json_escape.h"
#include "lomn/loop.h"
#include "lomn/prefetch_fs.h"
#include "lomn/rhi.h"
#include "lomn/view/area.h"
#include "lomn/view/fly_camera.h"
#include "lomn/view/prefetch.h"

using namespace lomn;

namespace {

constexpr uint32_t kDtRing = 1200;  // last 1200 frame intervals (20 s at 60 Hz) for fps / percentiles

struct State {
  fs::PrefetchFileSystem fs;
  std::vector<std::string> paths;  // normalised index paths (the area list)
  std::unique_ptr<rhi::Device> dev;
  std::string error, text;
  bool triangle = false;
  TriangleScene tri;
  double sim_time = 0;
  uint32_t width = 1280, height = 720;
  // area loading
  std::string probe_level, probe_area;
  view::AreaData probe;  // the last round's result
  bool probe_ok = false;
  uint32_t probe_rounds = 0;
  double probe_ms = 0;
  // shown area
  view::AreaData area;
  bool has_area = false;
  std::string probe_reads;  // lomn_fs_reads: the last area probe round's exact reads
  view::AreaRenderer ren;
  view::RenderOptions render_opts;  // lomn_set_fog (the pixel gate's positive control: ?fog=off)
  uint32_t debug_leaked = 0;        // lomn_debug_leak_texture (the leak detector's positive control)
  double upload_ms = 0, show_ms = 0;
  uint64_t cpu_released = 0, fs_released = 0;
  uint32_t areas_shown = 0, release_failures = 0, objects_released = 0;
  // camera / input
  view::FlyCamera fly;
  view::FlyInput in;
  // frame metrics
  uint64_t frames = 0, frames_since_show = 0;
  double last_ms = 0;
  double dt[kDtRing] = {};
  uint32_t dt_count = 0, dt_head = 0;
  double replay_ms[kDtRing] = {}, submit_ms[kDtRing] = {}, cpu_ms[kDtRing] = {};
  uint32_t rejected_last = 0, draws_last = 0;
  // REF4 r2 (I2): the display rate, measured once from the empty-canvas frames before the first area (the reference the
  // fps is reconciled against; deriving it from the measured frames themselves would call any steady rate "no drops").
  // Review fix 2: the MEDIAN of 120 intervals (after 10 skipped), snapped to a standard rate only within +-3 %.
  static constexpr uint32_t kDisplaySkip = 10, kDisplaySamples = 120;
  double display_dt[kDisplaySamples] = {};
  uint32_t display_seen = 0, display_n = 0, display_runs = 0;
  bool remeasure = false;  // debug (lomn_debug_remeasure): measure again, with empty frames, on a page that already shows an area
  double display_hz = 0, display_raw_hz = 0, display_median_ms = 0;
  bool display_snapped = false;
  double spin_ms = 0;  // debug: busy-wait per frame (?debug=1&debug_spin=MS), to show the DROPPING flag fires
  // REF4 r2 (C1)
  bool lost = false;
  std::string lost_reason;
};
State* g = nullptr;  // created in lomn_configure (or main) so static init stays trivial

State& st() {
  if (!g) g = new State();
  return *g;
}

const char* keep(std::string s) {
  st().text = std::move(s);
  return st().text.c_str();
}
// JS numbers: finite, integral, 0..2^53.
bool to_u64(double d, uint64_t& out) {
  if (!std::isfinite(d) || d < 0 || d > 9007199254740991.0 || std::floor(d) != d) return false;
  out = static_cast<uint64_t>(d);
  return true;
}
void set_state(const char* state, const std::string& detail) {
  EM_ASM({ if (Module.onLomnState) Module.onLomnState(UTF8ToString($0), UTF8ToString($1)); }, state, detail.c_str());
}

double pct(const double* v, uint32_t n, double p) {
  if (!n) return 0;
  std::vector<double> s(v, v + n);
  std::sort(s.begin(), s.end());
  return s[std::min<size_t>(n - 1, static_cast<size_t>(p * (n - 1) + 0.5))];
}
std::string num(double v) {
  if (!std::isfinite(v)) return "null";
  char b[64];
  std::snprintf(b, sizeof b, "%.3f", v);
  return b;
}
std::string js(const std::string& s) { return "\"" + json_escape(s) + "\""; }
// The display rate the frame intervals imply: 1000 / p50, snapped to the nearest common refresh rate.
double nominal_hz(double p50_ms) {
  static const double rates[] = {24, 30, 48, 50, 60, 72, 75, 90, 100, 120, 144, 165, 240};
  if (!(p50_ms > 0)) return 0;
  const double hz = 1000.0 / p50_ms;
  double best = rates[0];
  for (double r : rates)
    if (std::fabs(r - hz) < std::fabs(best - hz)) best = r;
  return best;
}

// The display rate implied by the median frame interval of the empty frames. The median, not a percentile: a percentile
// of ~100 samples wanders by a millisecond from run to run (the old p10 sat on the 60/72 Hz snap boundary and read 72 Hz on
// a 60 Hz display about one run in seven under load). The result is a standard refresh rate only when the raw rate is
// within +-3 % of one (a resolution allowance for timer jitter); otherwise the raw rate is kept and marked unsnapped,
// never forced onto a neighbour.
struct DisplayRate {
  double median_ms, raw_hz, hz;
  bool snapped;
};
DisplayRate classify_display(double median_ms) {
  static const double standard[] = {30, 48, 50, 60, 72, 75, 90, 100, 120, 144, 165, 240};
  DisplayRate r{median_ms, 0, 0, false};
  if (!(median_ms > 0) || !std::isfinite(median_ms)) return r;
  r.raw_hz = 1000.0 / median_ms;
  r.hz = r.raw_hz;
  double best = standard[0];
  for (double c : standard)
    if (std::fabs(c - r.raw_hz) < std::fabs(best - r.raw_hz)) best = c;
  if (std::fabs(best - r.raw_hz) <= 0.03 * best) {
    r.hz = best;
    r.snapped = true;
  }
  return r;
}
DisplayRate display_rate(const double* dt, uint32_t n) { return classify_display(pct(dt, n, 0.5)); }

view::Camera current_camera() {
  State& s = st();
  const float aspect = static_cast<float>(s.width) / static_cast<float>(s.height ? s.height : 1);
  return view::make_camera(s.area, s.fly.settings(view::CameraSettings()), aspect);
}

void frame() {
  State& s = st();
  const double t0 = emscripten_get_now();
  const double dt = s.last_ms ? t0 - s.last_ms : 0;
  s.last_ms = t0;
  ++s.frames;
  if (s.triangle) {
    s.sim_time += kFakeDeltaTime;
    s.tri.render(*s.dev, 0, s.sim_time);
    if (dt > 0) {
      s.dt[s.dt_head] = dt;
      s.dt_head = (s.dt_head + 1) % kDtRing;
      s.dt_count = std::min(s.dt_count + 1, kDtRing);
    }
    return;
  }
  double replay = 0, submit = 0;
  if (s.has_area && !s.remeasure) {
    s.fly.update(s.in, dt / 1000.0);
    s.in.look_dx = s.in.look_dy = 0;
    s.ren.render(*s.dev, 0, s.area, current_camera());
    replay = s.ren.last_stats().replay_ms;
    submit = s.ren.last_stats().submit_ms;
    ++s.frames_since_show;
  } else {
    const float black[4] = {0, 0, 0, 1};
    s.dev->begin_frame(0, black);
    s.dev->end_frame();
    // I2: before the first area, the empty frames measure the display (the first kDisplaySkip are startup noise).
    if (dt > 0 && (s.areas_shown == 0 || s.remeasure) && s.display_n < State::kDisplaySamples && ++s.display_seen > State::kDisplaySkip) {
      s.display_dt[s.display_n++] = dt;
      if (s.display_n == State::kDisplaySamples) {
        const DisplayRate r = display_rate(s.display_dt, s.display_n);
        s.display_median_ms = r.median_ms;
        s.display_raw_hz = r.raw_hz;
        s.display_hz = r.hz;
        s.display_snapped = r.snapped;
        s.remeasure = false;
        ++s.display_runs;
      }
    }
  }
  if (s.spin_ms > 0 && s.has_area)  // never during the display measurement before the first area
    while (emscripten_get_now() - t0 < s.spin_ms) {
    }
  const rhi::FrameStats fs = s.dev->last_frame_stats();
  s.rejected_last = fs.rejected;
  s.draws_last = fs.draws;
  // dt samples only between two frames of the same shown area: the reconciliation window restarts at every show.
  if (dt > 0 && s.frames_since_show >= 2) {
    s.dt[s.dt_head] = dt;
    s.replay_ms[s.dt_head] = replay;
    s.submit_ms[s.dt_head] = submit;
    s.cpu_ms[s.dt_head] = emscripten_get_now() - t0;
    s.dt_head = (s.dt_head + 1) % kDtRing;
    s.dt_count = std::min(s.dt_count + 1, kDtRing);
  }
}

}  // namespace

extern "C" {

// Before main(): canvas drawing-buffer size and mode (triangle = the M0 smoke scene).
EMSCRIPTEN_KEEPALIVE void lomn_configure(int width, int height, int triangle) {
  State& s = st();
  if (width > 0 && height > 0 && width <= int(rhi::kMaxRenderTargetSize) && height <= int(rhi::kMaxRenderTargetSize)) {
    s.width = static_cast<uint32_t>(width);
    s.height = static_cast<uint32_t>(height);
  }
  s.triangle = triangle != 0;
}

EMSCRIPTEN_KEEPALIVE const char* lomn_last_error() { return st().error.c_str(); }

// ---- file bridge ----
EMSCRIPTEN_KEEPALIVE int lomn_fs_add_file(const char* path, double size) {
  uint64_t n = 0;
  if (!path || !to_u64(size, n)) return st().error = "lomn_fs_add_file: bad arguments", 0;
  if (!st().fs.add_file(path, n, &st().error)) return 0;
  st().paths.push_back(fs::normalize(path));
  return 1;
}
// Returns the heap address to fill (also the handle), or 0 (lomn_last_error says why).
EMSCRIPTEN_KEEPALIVE uintptr_t lomn_fs_reserve(const char* path, double offset, double length) {
  uint64_t o = 0, l = 0;
  if (!path || !to_u64(offset, o) || !to_u64(length, l)) return st().error = "lomn_fs_reserve: bad arguments", 0;
  return reinterpret_cast<uintptr_t>(st().fs.reserve(path, o, l, &st().error));
}
EMSCRIPTEN_KEEPALIVE int lomn_fs_commit(uintptr_t handle) { return st().fs.commit(reinterpret_cast<const uint8_t*>(handle)) ? 1 : 0; }
EMSCRIPTEN_KEEPALIVE int lomn_fs_abort(uintptr_t handle) { return st().fs.abort(reinterpret_cast<const uint8_t*>(handle)) ? 1 : 0; }
EMSCRIPTEN_KEEPALIVE int lomn_fs_is_resident(const char* path, double offset, double length) {
  uint64_t o = 0, l = 0;
  return path && to_u64(offset, o) && to_u64(length, l) && st().fs.is_resident(path, o, l) ? 1 : 0;
}
// "path\toffset\tlength\n" lines (lomn::view::ranges_text).
EMSCRIPTEN_KEEPALIVE const char* lomn_fs_misses() { return keep(view::ranges_text(st().fs.misses())); }
EMSCRIPTEN_KEEPALIVE const char* lomn_fs_touched() { return keep(view::ranges_text(st().fs.touched())); }
// M1 close-out C-f2: the ranges the last lomn_area_probe round actually read (exact, distinct, sorted, no zero-length reads). Unlike
// lomn_fs_touched (the RESIDENT ranges a read hit, which can be larger than the read) this is what a cached prefetch set must hold.
EMSCRIPTEN_KEEPALIVE const char* lomn_fs_reads() { return keep(st().probe_reads); }
EMSCRIPTEN_KEEPALIVE double lomn_fs_misses_dropped() { return static_cast<double>(st().fs.misses_dropped()); }
// Area list from the index (levels/<lev>/<area>.blk): "lev\tarea\n" lines.
EMSCRIPTEN_KEEPALIVE const char* lomn_list_areas() {
  std::string o;
  for (const view::AreaRef& a : view::list_areas(st().paths)) o += a.level + "\t" + a.area + "\n";
  return keep(o);
}

// ---- boot (before main) ----
// One round: returns the number of misses (> 0: fetch them and call again), 0 when the boot set is resident.
EMSCRIPTEN_KEEPALIVE int lomn_boot_probe() {
  State& s = st();
  s.fs.next_generation();
  view::boot_reads(s.fs);
  return static_cast<int>(s.fs.misses().size() + s.fs.misses_dropped());
}
// Pins the last boot round's ranges (never released); returns the pinned byte count.
EMSCRIPTEN_KEEPALIVE double lomn_boot_done() {
  State& s = st();
  s.fs.pin_touched();
  return static_cast<double>(s.fs.pinned_bytes());
}

// ---- areas ----
EMSCRIPTEN_KEEPALIVE void lomn_area_begin(const char* level, const char* area) {
  State& s = st();
  s.probe_level = level ? level : "";
  s.probe_area = area ? area : "";
  s.probe_rounds = 0;
  s.probe_ms = 0;
  s.probe_ok = false;
}
// One load round. Returns the miss count (> 0: fetch and call again); 0 = loaded without a miss (lomn_area_show next);
// -1 = the load failed without a miss (a real error: lomn_last_error).
EMSCRIPTEN_KEEPALIVE int lomn_area_probe() {
  State& s = st();
  s.fs.next_generation();
  const double t0 = emscripten_get_now();
  std::string err;
  // M1 close-out C-f2: the exact (path, offset, length) of every non-empty read of this round, for lomn_fs_reads.
  s.probe_reads = view::ranges_text(view::record_reads(s.fs, [&] {
    s.probe_ok = view::load_area(s.fs, s.probe_level, s.probe_area, view::AreaOptions(), s.probe, &err);
  }));
  s.probe_ms += emscripten_get_now() - t0;
  ++s.probe_rounds;
  const size_t misses = s.fs.misses().size() + s.fs.misses_dropped();
  if (misses) return static_cast<int>(misses);
  if (!s.probe_ok) {
    s.error = "area " + s.probe_level + "/" + s.probe_area + ": " + err;
    return -1;
  }
  return 0;
}
// Shows the area the last probe loaded. 1 on success; 0 on failure (lomn_last_error).
EMSCRIPTEN_KEEPALIVE int lomn_area_show() {
  State& s = st();
  if (!s.dev) return s.error = "no device yet", 0;
  // Final review I-1: on a lost device every Dawn create call still returns a handle, so upload() would "succeed" and re-arm
  // has_area on a loop that is cancelled for good. Refuse, with the loss as the reason the page reports.
  if (s.lost) return s.error = "GPU device lost: reload the page", 0;
  if (!s.probe_ok || !s.fs.misses().empty()) return s.error = "no completed probe", 0;
  const double t0 = emscripten_get_now();
  uint32_t failures = 0;
  s.objects_released += s.ren.release(*s.dev, &failures);
  s.release_failures += failures;
  s.area = std::move(s.probe);
  s.probe = view::AreaData();
  s.probe_ok = false;
  s.has_area = false;
  const double tu = emscripten_get_now();
  std::string err;
  if (!s.ren.upload(*s.dev, s.area, s.render_opts, &err)) {
    s.error = "upload: " + err;
    s.ren.release(*s.dev, &failures);
    return 0;
  }
  s.upload_ms = emscripten_get_now() - tu;
  s.cpu_released = view::release_cpu_copies(s.area);
  // The touch generation is still the successful probe's: drop every prefetched range it did not read.
  s.fs_released = s.fs.release_untouched();
  const float aspect = static_cast<float>(s.width) / static_cast<float>(s.height ? s.height : 1);
  s.fly.reset(view::make_camera(s.area, view::CameraSettings(), aspect));
  s.has_area = true;
  ++s.areas_shown;
  s.frames_since_show = 0;
  s.dt_count = s.dt_head = 0;
  s.show_ms = emscripten_get_now() - t0;
  return 1;
}

// ---- input / surface ----
// keys bit mask: 1 forward, 2 back, 4 left, 8 right, 16 up, 32 down, 64 fast.
EMSCRIPTEN_KEEPALIVE void lomn_input(int keys, double look_dx, double look_dy) {
  State& s = st();
  s.in.forward = keys & 1;
  s.in.back = keys & 2;
  s.in.left = keys & 4;
  s.in.right = keys & 8;
  s.in.up = keys & 16;
  s.in.down = keys & 32;
  s.in.fast = keys & 64;
  if (std::isfinite(look_dx) && std::isfinite(look_dy) && std::fabs(look_dx) < 1e5 && std::fabs(look_dy) < 1e5) {
    s.in.look_dx += static_cast<float>(look_dx);
    s.in.look_dy += static_cast<float>(look_dy);
  }
}
// Debug switch (RenderOptions::fog): 0 drops fog from every draw of areas shown afterwards. The browser tests use it as
// the pixel gate's positive control (a change the tolerance must see). Default on (faithful).
EMSCRIPTEN_KEEPALIVE void lomn_set_fog(int on) { st().render_opts.fog = on != 0; }
// Debug: creates one 1x1 texture that nothing owns or destroys (a deliberate leak). The browser tests call it once to
// prove the live-object leak check fires. Returns the texture id (0 without a device).
EMSCRIPTEN_KEEPALIVE int lomn_debug_leak_texture() {
  State& s = st();
  if (!s.dev) return 0;
  ++s.debug_leaked;
  return static_cast<int>(s.dev->create_texture(1, 1, 1));
}
// The build flavour compiled into this module (CMake: LOMN_BUILD_TYPE / LOMN_BUILD_FLAGS); the page adds the SHA-256 of
// the wasm bytes it instantiated.
EMSCRIPTEN_KEEPALIVE const char* lomn_build_info() {
#if !defined(LOMN_BUILD_TYPE)
#define LOMN_BUILD_TYPE "unknown"
#define LOMN_BUILD_FLAGS ""
#endif
  return keep(std::string("{\"type\":") + js(LOMN_BUILD_TYPE) + ",\"cxx_flags\":" + js(LOMN_BUILD_FLAGS) + ",\"emscripten\":\"" +
              std::to_string(__EMSCRIPTEN_MAJOR__) + "." + std::to_string(__EMSCRIPTEN_MINOR__) + "." + std::to_string(__EMSCRIPTEN_TINY__) +
              "\",\"compiler\":" + js(__VERSION__) + "}");
}
// Debug (REF4 r2, I2): busy-wait this long in every frame (0..200 ms), to show that the DROPPING flag fires.
// Debug (review fix 2): measure the display rate again with empty frames (about 130 frames), on a page that shows an area; the
// result replaces display_hz and bumps display_runs. The tests run it 10 times on one page to show the rate does not wander.
EMSCRIPTEN_KEEPALIVE void lomn_debug_remeasure() {
  State& s = st();
  s.remeasure = true;
  s.display_seen = s.display_n = 0;
}
// Debug (review fix 2): the pure classification of a median frame interval, for deterministic tests of the snap rule.
EMSCRIPTEN_KEEPALIVE const char* lomn_debug_classify_display(double interval_ms) {
  const DisplayRate r = classify_display(interval_ms);
  return keep("{\"interval_ms\":" + num(interval_ms) + ",\"raw_hz\":" + num(r.raw_hz) + ",\"hz\":" + num(r.hz) + ",\"snapped\":" +
              (r.snapped ? "true" : "false") + "}");
}
EMSCRIPTEN_KEEPALIVE void lomn_debug_spin(double ms) { st().spin_ms = std::isfinite(ms) ? std::max(0.0, std::min(200.0, ms)) : 0; }
EMSCRIPTEN_KEEPALIVE void lomn_camera_reset() {
  State& s = st();
  if (!s.has_area) return;
  const float aspect = static_cast<float>(s.width) / static_cast<float>(s.height ? s.height : 1);
  s.fly.reset(view::make_camera(s.area, view::CameraSettings(), aspect));
}
EMSCRIPTEN_KEEPALIVE int lomn_resize(int w, int h) {
  State& s = st();
  if (!s.dev || w <= 0 || h <= 0) return 0;
  if (!s.dev->resize(static_cast<uint32_t>(w), static_cast<uint32_t>(h))) return 0;
  s.width = static_cast<uint32_t>(w);
  s.height = static_cast<uint32_t>(h);
  return 1;
}

// ---- metrics ----
EMSCRIPTEN_KEEPALIVE const char* lomn_metrics_json() {
  State& s = st();
  const uint32_t n = s.dt_count;
  double window = 0;
  std::vector<double> dts, rep, sub, cpu;
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t k = (s.dt_head + kDtRing - n + i) % kDtRing;
    window += s.dt[k];
    dts.push_back(s.dt[k]);
    rep.push_back(s.replay_ms[k]);
    sub.push_back(s.submit_ms[k]);
    cpu.push_back(s.cpu_ms[k]);
  }
  const double p50 = pct(dts.data(), n, 0.5), p95 = pct(dts.data(), n, 0.95);
  // Reconciliation (REF4 r2, I2): a window of n intervals spans n + 1 frames; at the DISPLAY rate measured before the
  // first area it should hold window * display_hz / 1000 intervals. DROPPING = measured fps < 0.95 x the display rate
  // (the same as counted / expected < 0.95). Without a display measurement the snapped p50 is used and labelled as such
  // (it cannot detect a steady lower rate), and nothing is flagged.
  const bool measured = s.display_hz > 0;
  const double hz = measured ? s.display_hz : nominal_hz(p50);
  const double expected = window * hz / 1000.0;
  const double fps = window > 0 ? n * 1000.0 / window : 0;
  const bool dropping = measured && n >= 30 && window > 0 && fps < 0.95 * s.display_hz;
  const rhi::Device::LiveObjects lo = s.dev ? s.dev->live_objects() : rhi::Device::LiveObjects();
  const view::RenderStats& rs = s.ren.last_stats();
  const struct mallinfo mi = mallinfo();
  std::string o = "{";
  o += "\"frames\":" + std::to_string(s.frames) + ",\"frames_since_show\":" + std::to_string(s.frames_since_show);
  o += ",\"fps\":{\"dt_samples\":" + std::to_string(n) + ",\"frames_in_window\":" + std::to_string(n ? n + 1 : 0) +
       ",\"window_ms\":" + num(window) + ",\"p50_ms\":" + num(p50) + ",\"p95_ms\":" + num(p95) + ",\"nominal_hz\":" + num(hz) +
       ",\"display_hz\":" + num(s.display_hz) + ",\"display_raw_hz\":" + num(s.display_raw_hz) + ",\"display_snapped\":" +
       (s.display_snapped ? "true" : "false") + ",\"display_median_ms\":" + num(s.display_median_ms) + ",\"display_samples\":" +
       std::to_string(s.display_n) + ",\"display_runs\":" + std::to_string(s.display_runs) + ",\"hz_source\":" + js(measured ? "display measured before the first area" : "snapped p50 (display not measured)") +
       ",\"expected_intervals\":" + num(expected) + ",\"fps\":" + num(fps) + ",\"dropping\":" + (dropping ? "true" : "false") +
       ",\"spin_ms\":" + num(s.spin_ms) + "}";
  o += ",\"breakdown_ms\":{\"script\":0,\"physics\":0,\"replay_p50\":" + num(pct(rep.data(), n, 0.5)) + ",\"replay_p95\":" +
       num(pct(rep.data(), n, 0.95)) + ",\"submit_p50\":" + num(pct(sub.data(), n, 0.5)) + ",\"submit_p95\":" +
       num(pct(sub.data(), n, 0.95)) + ",\"frame_cpu_p50\":" + num(pct(cpu.data(), n, 0.5)) + ",\"frame_cpu_p95\":" +
       num(pct(cpu.data(), n, 0.95)) + "}";
  o += ",\"draws\":" + std::to_string(s.draws_last) + ",\"rejected_last_frame\":" + std::to_string(s.rejected_last) +
       ",\"pipelines\":" + std::to_string(s.ren.pipelines()) + ",\"textures\":" + std::to_string(s.ren.textures_created()) +
       ",\"texture_bytes\":" + std::to_string(s.ren.uploaded_texture_bytes()) + ",\"state_calls\":" + std::to_string(rs.state_calls);
  o += ",\"live_objects\":{\"textures\":" + std::to_string(lo.textures) + ",\"buffers\":" + std::to_string(lo.buffers) +
       ",\"pipelines\":" + std::to_string(lo.pipelines) + ",\"samplers\":" + std::to_string(lo.samplers) +
       ",\"bind_groups\":" + std::to_string(lo.bind_groups) + "}";
  o += ",\"heap\":{\"wasm_memory_bytes\":" + std::to_string(emscripten_get_heap_size()) + ",\"malloc_in_use_bytes\":" +
       std::to_string(static_cast<uint64_t>(mi.uordblks)) + ",\"malloc_arena_bytes\":" + std::to_string(static_cast<uint64_t>(mi.arena)) +
       ",\"fs_resident_bytes\":" + std::to_string(s.fs.resident_bytes()) + ",\"fs_pinned_bytes\":" + std::to_string(s.fs.pinned_bytes()) +
       ",\"fs_peak_bytes\":" + std::to_string(s.fs.peak_bytes()) + ",\"fs_ranges\":" + std::to_string(s.fs.resident_ranges()) + "}";
  o += ",\"fs\":{\"files\":" + std::to_string(s.fs.file_count()) + ",\"misses_total\":" + std::to_string(s.fs.misses_total()) +
       ",\"generation\":" + std::to_string(s.fs.generation()) + "}";
  o += ",\"area\":";
  if (s.has_area) {
    const view::Camera c = current_camera();
    o += "{\"level\":" + js(s.area.level) + ",\"area\":" + js(s.area.area) + ",\"rounds\":" + std::to_string(s.probe_rounds) +
         ",\"probe_ms\":" + num(s.probe_ms) + ",\"load_ms\":" + num(s.area.stats.load_ms) + ",\"decode_ms\":" +
         num(s.area.stats.decode_ms) + ",\"upload_ms\":" + num(s.upload_ms) + ",\"show_ms\":" + num(s.show_ms) +
         ",\"cpu_bytes_released\":" + std::to_string(s.cpu_released) + ",\"fs_bytes_released\":" + std::to_string(s.fs_released) +
         ",\"draws_expected\":" + std::to_string(s.area.stats.draws) + ",\"warnings\":" + std::to_string(s.area.warnings.size()) +
         ",\"camera\":{\"moved\":" + (s.fly.moved() ? "true" : "false") + ",\"eye\":[" + num(c.eye[0]) + "," + num(c.eye[1]) + "," +
         num(c.eye[2]) + "],\"yaw_deg\":" + num(c.yaw_deg) + ",\"pitch_deg\":" + num(c.pitch_deg) + ",\"fov_deg\":" + num(c.fov_deg) +
         ",\"eye_source\":" + js(c.eye_source) + "}}";
  } else {
    o += "null";
  }
  o += ",\"areas_shown\":" + std::to_string(s.areas_shown) + ",\"objects_released\":" + std::to_string(s.objects_released) +
       ",\"release_failures\":" + std::to_string(s.release_failures);
  o += ",\"surface\":{\"width\":" + std::to_string(s.width) + ",\"height\":" + std::to_string(s.height) + "}";
  o += ",\"render\":{\"fog\":" + std::string(s.render_opts.fog ? "true" : "false") + ",\"debug_leaked_textures\":" +
       std::to_string(s.debug_leaked) + "}";
  o += ",\"lost\":" + std::string(s.lost ? "true" : "false") + ",\"lost_reason\":" + js(s.lost_reason);
  o += ",\"adapter\":" + js(s.dev ? s.dev->adapter() : std::string()) + "}";
  return keep(o);
}

}  // extern "C"

int main() {
  State& s = st();
  set_state("starting", "");
  rhi::create_webgpu_device("#canvas", s.width, s.height, [](std::unique_ptr<rhi::Device> d, std::string err) {
    State& s2 = st();
    if (!d) {
      set_state("error", err);
      return;
    }
    s2.dev = std::move(d);
    // REF4 r2 (C1): a lost device (or the first uncaptured GPU error) stops the loop and tells the page.
    s2.dev->set_lost_handler([](const std::string& why) {
      State& s3 = st();
      if (s3.lost) return;
      s3.lost = true;
      s3.lost_reason = why;
      s3.has_area = false;
      emscripten_cancel_main_loop();
      set_state("error", why);
    });
    if (s2.triangle) {
      s2.tri.init(*s2.dev);
    } else if (!s2.dev->set_surface_depth(true, 1.0f)) {
      set_state("error", "surface depth buffer rejected");
      return;
    }
    set_state("ready", s2.dev->adapter());
    emscripten_set_main_loop(frame, 0, false);
  });
  return 0;
}
