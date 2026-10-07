// lomn_view: render one area of a LOMN install, unlit, from the POS start (M1a-2 "first pixels").
//   lomn_view --install <dir> --level lev1 --area vllg --out <file.png> [options]
//   lomn_view --synthetic 1 --out <file> [options]          the A7 contract scene (no game data)
//   lomn_view --install <dir> --survey 1 --out <file.json>  CPU-load every area (no GPU), per-area counts + maxima
// Options (every option takes a value):
//   --backend webgpu|null   webgpu (default) writes <out> as PNG; null writes the rhi call log to <out>
//   --width 1280 --height 720 --frames 1
//   --fov 60 --near 0.5 --far 4000 --eye x,y,z --yaw deg --pitch deg --eye-height 3   (camera fallback overrides)
//   --vertex-colors on|off  --alpha-ref 1  --routes main,trans,back,moveable,nolight
// Always writes <out>.json metrics (except --survey, whose output IS the JSON).
// Exit codes (as lomn_native): 0 ok, 2 bad args, 3 no WebGPU adapter / install or area unreadable, 4 failure.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "json_escape.h"
#include "lomn/fs.h"
#include "lomn/rhi.h"
#include "lomn/view/area.h"
#include "lomn/view/synthetic.h"

#if !defined(__EMSCRIPTEN__)
#include <filesystem>

#include "stb_image_write.h"
#endif

using namespace lomn;
using Clock = std::chrono::steady_clock;

namespace {

double ms_since(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

int usage(const char* why) {
  std::fprintf(stderr, "%s\n", why);
  std::fprintf(stderr,
               "usage: lomn_view --install DIR --level LEV --area AREA --out FILE [--backend webgpu|null] [--width W --height H]\n"
               "                 [--fov DEG --near N --far F --eye x,y,z --yaw DEG --pitch DEG --eye-height H] [--frames N]\n"
               "                 [--vertex-colors on|off] [--alpha-ref 0..255] [--routes main,trans,back,moveable,nolight]\n"
               "       lomn_view --synthetic 1 --out FILE [...]   |   lomn_view --install DIR --survey 1 --out FILE.json\n");
  return 2;
}

bool parse_float(const std::string& s, float& out) {
  char* end = nullptr;
  out = std::strtof(s.c_str(), &end);
  return !s.empty() && end && *end == '\0';
}
bool parse_u32(const std::string& s, uint32_t lo, uint32_t hi, uint32_t& out) {
  char* end = nullptr;
  unsigned long v = std::strtoul(s.c_str(), &end, 10);
  if (s.empty() || !end || *end != '\0' || v < lo || v > hi) return false;
  out = static_cast<uint32_t>(v);
  return true;
}
std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  for (std::string t; std::getline(ss, t, sep);) out.push_back(t);
  return out;
}

std::string num(double v) {
  char b[64];
  std::snprintf(b, sizeof b, "%.3f", v);
  return b;
}
std::string vec3(const float* v) { return "[" + num(v[0]) + "," + num(v[1]) + "," + num(v[2]) + "]"; }
std::string js(const std::string& s) { return "\"" + json_escape(s) + "\""; }

std::string routes_json(const view::AreaStats& st) {
  static const sage::Route rs[] = {sage::Route::Main, sage::Route::Trans, sage::Route::Back, sage::Route::Moveable, sage::Route::NoLightMoveable};
  std::string o = "{";
  for (size_t i = 0; i < 5; ++i)
    o += std::string(i ? "," : "") + "\"" + sage::route_name(rs[i]) + "\":" + std::to_string(st.routes[static_cast<int>(rs[i])]);
  return o + "}";
}

std::string area_counts_json(const view::AreaData& a) {
  const view::AreaStats& s = a.stats;
  return "\"art_blk\":" + std::string(a.art_blk ? "true" : "false") + ",\"obj_records\":" + std::to_string(s.obj_records) +
         ",\"placements\":" + std::to_string(s.placements) + ",\"routes\":" + routes_json(s) + ",\"ignored_records\":" +
         std::to_string(s.ignored_records) + ",\"disabled_records\":" + std::to_string(s.disabled_records) + ",\"missing_models\":" +
         std::to_string(s.missing_models) + ",\"placements_with_orientation\":" + std::to_string(s.placements_with_orientation) +
         ",\"models\":" + std::to_string(s.models) + ",\"models_without_colors\":" + std::to_string(s.models_without_colors) +
         ",\"subsets\":" + std::to_string(s.subsets) + ",\"alpha_subsets\":" + std::to_string(s.alpha_subsets) + ",\"untextured_subsets\":" +
         std::to_string(s.untextured_subsets) + ",\"draws\":" + std::to_string(s.draws) + ",\"vertices\":" + std::to_string(s.vertices) +
         ",\"triangles\":" + std::to_string(s.triangles) + ",\"geometry_bytes\":" + std::to_string(s.geometry_bytes) + ",\"textures\":" +
         std::to_string(s.textures) + ",\"textures_alpha\":" + std::to_string(s.textures_alpha) + ",\"textures_missing\":" +
         std::to_string(s.textures_missing) + ",\"textures_failed\":" + std::to_string(s.textures_failed) + ",\"texture_bytes_with_mips\":" +
         std::to_string(s.texture_bytes) + ",\"pos_records\":" + std::to_string(a.pos_records) + ",\"start\":" +
         (a.has_start ? "{\"record\":" + std::to_string(a.start_record) + ",\"pos\":" + vec3(a.start) + "}" : std::string("null")) +
         ",\"bounds\":" + (a.has_bounds ? "{\"min\":" + vec3(a.bounds_min) + ",\"max\":" + vec3(a.bounds_max) + "}" : std::string("null"));
}

std::string models_json(const view::AreaData& a) {
  std::vector<std::string> ids;
  for (const view::Model& m : a.models) ids.push_back(m.id);
  std::sort(ids.begin(), ids.end());
  std::string o = "[";
  for (size_t i = 0; i < ids.size(); ++i) o += std::string(i ? "," : "") + js(ids[i]);
  return o + "]";
}

std::string warnings_json(const std::vector<std::string>& w) {
  std::string o = "[";
  for (size_t i = 0; i < w.size(); ++i) o += std::string(i ? "," : "") + js(w[i]);
  return o + "]";
}

#if !defined(__EMSCRIPTEN__)
// Every <lev>/<area>.blk under data/levels (sorted), CPU-loaded with load_area. Measures the caps' real maxima.
int survey(const std::string& install, const std::string& out_path) {
  namespace sfs = std::filesystem;
  auto fsp = fs::open_disk(install, (sfs::temp_directory_path() / "lomn_view_user").string());
  if (!fsp) return std::fprintf(stderr, "cannot open install %s\n", install.c_str()), 3;
  std::error_code ec;
  sfs::path levels;
  for (const auto& d : sfs::directory_iterator(install, ec))
    if (d.is_directory()) {
      std::string n = d.path().filename().string();
      std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (n == "data")
        for (const auto& l : sfs::directory_iterator(d.path(), ec)) {
          std::string ln = l.path().filename().string();
          std::transform(ln.begin(), ln.end(), ln.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
          if (ln == "levels") levels = l.path();
        }
    }
  if (levels.empty()) return std::fprintf(stderr, "no data/levels in %s\n", install.c_str()), 3;
  std::vector<std::pair<std::string, std::string>> areas;
  for (const auto& l : sfs::directory_iterator(levels, ec)) {
    if (!l.is_directory()) continue;
    std::string lev = l.path().filename().string();
    for (const auto& f : sfs::directory_iterator(l.path(), ec)) {
      std::string n = f.path().filename().string();
      std::string ln = n;
      std::transform(ln.begin(), ln.end(), ln.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (ln.size() < 5 || ln.compare(ln.size() - 4, 4, ".blk") != 0) continue;
      std::string stem = ln.substr(0, ln.size() - 4);
      std::string llev = lev;
      std::transform(llev.begin(), llev.end(), llev.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (stem == llev) continue;  // the level blockfile itself
      areas.push_back({llev, stem});
    }
  }
  std::sort(areas.begin(), areas.end());
  std::string rows;
  uint64_t max_place = 0, max_draws = 0, max_tex = 0, max_tex_bytes = 0, max_geo = 0, max_records = 0, failures = 0;
  double total_ms = 0;
  std::string max_place_at, max_draws_at, max_tex_at, max_tex_bytes_at, max_geo_at;
  auto track = [](uint64_t v, uint64_t& m, std::string& at, const std::string& where) {
    if (v > m) {
      m = v;
      at = where;
    }
  };
  for (const auto& la : areas) {
    view::AreaData a;
    std::string err;
    const auto t0 = Clock::now();
    bool ok = view::load_area(*fsp, la.first, la.second, view::AreaOptions(), a, &err);
    const double ms = ms_since(t0);
    total_ms += ms;
    const std::string where = la.first + "/" + la.second;
    rows += std::string(rows.empty() ? "" : ",\n") + "{\"area\":" + js(where) + ",\"ok\":" + (ok ? "true" : "false");
    if (!ok) {
      ++failures;
      rows += ",\"error\":" + js(err) + "}";
      continue;
    }
    rows += "," + area_counts_json(a) + ",\"load_ms\":" + num(a.stats.load_ms) + ",\"decode_ms\":" + num(a.stats.decode_ms) +
            ",\"warnings\":" + std::to_string(a.warnings.size()) + "}";
    max_records = std::max<uint64_t>(max_records, a.stats.obj_records);
    track(a.stats.placements, max_place, max_place_at, where);
    track(a.stats.draws, max_draws, max_draws_at, where);
    track(a.stats.textures, max_tex, max_tex_at, where);
    track(a.stats.texture_bytes, max_tex_bytes, max_tex_bytes_at, where);
    track(a.stats.geometry_bytes, max_geo, max_geo_at, where);
  }
  std::ofstream o(out_path);
  o << "{\n\"areas_total\":" << areas.size() << ",\"failures\":" << failures << ",\"total_load_ms\":" << num(total_ms) << ",\n\"max\":{\"obj_records\":" << max_records
    << ",\"placements\":" << max_place << ",\"placements_at\":" << js(max_place_at) << ",\"draws\":" << max_draws << ",\"draws_at\":"
    << js(max_draws_at) << ",\"textures\":" << max_tex << ",\"textures_at\":" << js(max_tex_at) << ",\"texture_bytes_with_mips\":"
    << max_tex_bytes << ",\"texture_bytes_at\":" << js(max_tex_bytes_at) << ",\"geometry_bytes\":" << max_geo << ",\"geometry_bytes_at\":"
    << js(max_geo_at) << "},\n\"areas\":[\n"
    << rows << "]}\n";
  if (!o) return std::fprintf(stderr, "cannot write %s\n", out_path.c_str()), 4;
  std::printf("survey: %zu areas, %llu failures -> %s\n", areas.size(), static_cast<unsigned long long>(failures), out_path.c_str());
  return failures ? 4 : 0;
}
#endif

}  // namespace

int main(int argc, char** argv) {
  if ((argc - 1) % 2 != 0) return usage((std::string("option ") + argv[argc - 1] + " needs a value").c_str());
  std::map<std::string, std::string> a;
  static const char* known[] = {"--install", "--level", "--area", "--out", "--backend", "--width", "--height", "--frames", "--fov", "--near",
                                "--far", "--eye", "--yaw", "--pitch", "--eye-height", "--vertex-colors", "--alpha-ref", "--routes",
                                "--synthetic", "--survey"};
  for (int i = 1; i + 1 < argc; i += 2) {
    std::string k = argv[i];
    if (std::find_if(std::begin(known), std::end(known), [&](const char* x) { return k == x; }) == std::end(known))
      return usage(("unknown option " + k).c_str());
    a[k] = argv[i + 1];
  }
  auto has = [&](const char* k) { return a.count(k) != 0; };
  const std::string out = has("--out") ? a["--out"] : "";
  if (out.empty()) return usage("--out is required");
  const bool synthetic = has("--synthetic") && a["--synthetic"] == "1";
  if (has("--survey")) {
#if defined(__EMSCRIPTEN__)
    return usage("--survey needs the native disk file system");
#else
    if (!has("--install")) return usage("--survey needs --install");
    return survey(a["--install"], out);
#endif
  }
  std::string level = synthetic ? view::kSyntheticLevel : a["--level"], area = synthetic ? view::kSyntheticArea : a["--area"];
  if (!synthetic && (!has("--install") || level.empty() || area.empty())) return usage("--install, --level and --area are required");
  const std::string backend = has("--backend") ? a["--backend"] : "webgpu";
  if (backend != "webgpu" && backend != "null") return usage(("unknown backend " + backend).c_str());

  uint32_t width = 1280, height = 720, frames = 1;
  if (synthetic) width = height = 64;
  view::CameraSettings cs = synthetic ? view::synthetic_camera() : view::CameraSettings();
  view::RenderOptions ro;
  view::AreaOptions ao;
  if (has("--width") && !parse_u32(a["--width"], 1, rhi::kMaxRenderTargetSize, width)) return usage("bad --width");
  if (has("--height") && !parse_u32(a["--height"], 1, rhi::kMaxRenderTargetSize, height)) return usage("bad --height");
  if (has("--frames") && !parse_u32(a["--frames"], 1, 100000, frames)) return usage("bad --frames");
  if (has("--fov") && (!parse_float(a["--fov"], cs.fov_deg) || !(cs.fov_deg > 0.0f && cs.fov_deg < 180.0f))) return usage("bad --fov");
  if (has("--near") && (!parse_float(a["--near"], cs.near_z) || !(cs.near_z > 0.0f))) return usage("bad --near");
  if (has("--far") && (!parse_float(a["--far"], cs.far_z) || !(cs.far_z > cs.near_z))) return usage("bad --far");
  if (!(cs.far_z > cs.near_z)) return usage("--far must be greater than --near");
  if (has("--eye")) {
    auto p = split(a["--eye"], ',');
    if (p.size() != 3) return usage("bad --eye (x,y,z)");
    for (int k = 0; k < 3; ++k)
      if (!parse_float(p[k], cs.eye[k])) return usage("bad --eye (x,y,z)");
    cs.eye_set = true;
  }
  if (has("--yaw")) {
    if (!parse_float(a["--yaw"], cs.yaw_deg)) return usage("bad --yaw");
    cs.yaw_set = true;
  }
  if (has("--pitch") && (!parse_float(a["--pitch"], cs.pitch_deg) || !(cs.pitch_deg > -90.0f && cs.pitch_deg < 90.0f))) return usage("bad --pitch");
  if (has("--eye-height") && !parse_float(a["--eye-height"], cs.eye_height)) return usage("bad --eye-height");
  if (has("--vertex-colors")) {
    if (a["--vertex-colors"] != "on" && a["--vertex-colors"] != "off") return usage("bad --vertex-colors (on|off)");
    ro.vertex_colors = a["--vertex-colors"] == "on";
  }
  if (has("--alpha-ref") && (!parse_float(a["--alpha-ref"], ro.alpha_ref) || !(ro.alpha_ref >= 0.0f && ro.alpha_ref <= 255.0f))) return usage("bad --alpha-ref");
  if (has("--routes")) {
    ao.draw_main = ao.draw_trans = ao.draw_back = ao.draw_moveable = ao.draw_nolight = false;
    for (const std::string& r : split(a["--routes"], ',')) {
      if (r == "main") ao.draw_main = true;
      else if (r == "trans") ao.draw_trans = true;
      else if (r == "back") ao.draw_back = true;
      else if (r == "moveable") ao.draw_moveable = true;
      else if (r == "nolight") ao.draw_nolight = true;
      else return usage(("bad --routes entry " + r).c_str());
    }
  }

  // File system: the synthetic scene in memory, or the install on disk (case-insensitive VFS).
  std::unique_ptr<fs::FileSystem> fsp;
  if (synthetic) {
    auto mem = std::make_unique<fs::MemoryFileSystem>();
    if (!view::mount_synthetic_install(*mem)) return std::fprintf(stderr, "synthetic install failed\n"), 4;
    fsp = std::move(mem);
  } else {
#if defined(__EMSCRIPTEN__)
    return usage("--install needs the native build (the browser path is M1b)");
#else
    fsp = fs::open_disk(a["--install"], (std::filesystem::temp_directory_path() / "lomn_view_user").string());
    if (!fsp) return std::fprintf(stderr, "cannot open install %s\n", a["--install"].c_str()), 3;
#endif
  }

  std::unique_ptr<rhi::Device> dev;
  std::vector<std::string> calls;
  if (backend == "null") {
    dev = rhi::create_null_device(&calls);
  } else {
#if defined(LOMN_HAS_WEBGPU)
    std::string err;
    rhi::create_webgpu_device(nullptr, 0, 0, [&](std::unique_ptr<rhi::Device> d, std::string e) {
      dev = std::move(d);
      err = std::move(e);
    });
    if (!dev) return std::fprintf(stderr, "webgpu unavailable: %s\n", err.c_str()), 3;
#else
    return std::fprintf(stderr, "webgpu unavailable: built without the WebGPU backend\n"), 3;
#endif
  }

  view::AreaData data;
  std::string err;
  if (!view::load_area(*fsp, level, area, ao, data, &err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return err.rfind("area: cannot", 0) == 0 || err.find("blockchain: cannot open") != std::string::npos ? 3 : 4;
  }
  const auto t_up = Clock::now();
  rhi::RenderTargetDesc rd;
  rd.width = width;
  rd.height = height;
  rd.depth = true;
  rhi::TextureId rt = dev->create_render_target_ex(rd);
  view::AreaRenderer renderer;
  if (!rt || !renderer.upload(*dev, data, ro, &err)) {
    std::fprintf(stderr, "upload failed: %s\n", rt ? err.c_str() : "render target rejected");
    return 4;
  }
  const double upload_ms = ms_since(t_up);
  const view::Camera cam = view::make_camera(data, cs, static_cast<float>(width) / static_cast<float>(height));
  std::vector<double> frame_ms;
  for (uint32_t f = 0; f < frames; ++f) {
    const auto t0 = Clock::now();
    renderer.render(*dev, rt, data, cam);
    frame_ms.push_back(ms_since(t0));
  }
  const rhi::FrameStats fstats = dev->last_frame_stats();
  double readback_ms = 0;
  bool wrote = false;
  if (backend == "null") {
    std::ofstream f(out);
    for (const auto& c : calls) f << c << '\n';
    wrote = static_cast<bool>(f);
  } else {
#if !defined(__EMSCRIPTEN__)
    std::vector<uint8_t> px;
    const auto t0 = Clock::now();
    if (!dev->read_pixels(rt, px)) return std::fprintf(stderr, "readback failed\n"), 4;
    readback_ms = ms_since(t0);
    wrote = stbi_write_png(out.c_str(), static_cast<int>(width), static_cast<int>(height), 4, px.data(), static_cast<int>(width) * 4) != 0;
#endif
  }
  if (!wrote) return std::fprintf(stderr, "cannot write %s\n", out.c_str()), 4;

  std::vector<double> sorted = frame_ms;
  std::sort(sorted.begin(), sorted.end());
  const double p50 = sorted[sorted.size() / 2];
  std::ofstream m(out + ".json");
  m << "{\n\"tool\":\"lomn_view\",\"level\":" << js(data.level) << ",\"area\":" << js(data.area) << ",\"synthetic\":"
    << (synthetic ? "true" : "false") << ",\n\"backend\":" << js(dev->backend()) << ",\"adapter\":" << js(dev->adapter())
    << ",\"width\":" << width << ",\"height\":" << height << ",\n\"frames\":" << frames << ",\"frames_rendered\":" << frame_ms.size()
    << ",\"load_ms\":" << num(data.stats.load_ms) << ",\"decode_ms\":" << num(data.stats.decode_ms) << ",\"upload_ms\":" << num(upload_ms)
    << ",\"frame_submit_ms_first\":" << num(frame_ms.front()) << ",\"frame_submit_ms_p50\":" << num(p50) << ",\"readback_ms\":" << num(readback_ms)
    << ",\n\"draws_last_frame\":" << fstats.draws << ",\"indices_last_frame\":" << fstats.indices << ",\"pipelines\":" << fstats.pipelines_total
    << ",\"gpu_textures\":" << renderer.textures_created() << ",\"texture_bytes_uploaded\":" << renderer.uploaded_texture_bytes() << ",\n"
    << area_counts_json(data) << ",\n\"model_ids\":" << models_json(data) << ",\n\"camera\":{\"fallback\":true,\"eye\":" << vec3(cam.eye) << ",\"at\":" << vec3(cam.at)
    << ",\"eye_source\":" << js(cam.eye_source) << ",\"yaw_deg\":" << num(cam.yaw_deg) << ",\"yaw_source\":" << js(cam.yaw_source)
    << ",\"pitch_deg\":" << num(cam.pitch_deg) << ",\"fov_deg\":" << num(cam.fov_deg) << ",\"near\":" << num(cam.near_z) << ",\"far\":"
    << num(cam.far_z) << ",\"aspect\":" << num(cam.aspect) << "},\n\"render\":{\"vertex_colors\":" << (ro.vertex_colors ? "true" : "false")
    << ",\"alpha_ref\":" << num(ro.alpha_ref) << "},\n\"warnings\":" << warnings_json(data.warnings) << "\n}\n";
  if (!m) return std::fprintf(stderr, "cannot write %s.json\n", out.c_str()), 4;
  std::printf("ok %s/%s backend=%s draws=%u textures=%u out=%s\n", data.level.c_str(), data.area.c_str(), dev->backend().c_str(),
              fstats.draws, data.stats.textures, out.c_str());
  return 0;
}
