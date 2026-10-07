// lomn_view: render one area of a LOMN install from the POS start, replaying the game's D3D8 states (M1a-3).
//   lomn_view --install <dir> --level lev1 --area vllg --out <file.png> [options]
//   lomn_view --synthetic 1|2 --out <file> [options]        the A7 contract scene / the A9 state scene (no game data)
//   lomn_view --install <dir> --survey 1 --out <file.json>  CPU-load every area (no GPU), per-area counts + maxima
// Options (every option takes a value):
//   --backend webgpu|null   webgpu (default) writes <out> as PNG; null writes the rhi call log to <out>
//   --width 1280 --height 720 --frames 1
//   --fov 72 --near 0.01 --far 4700 (the exe's camera; "exe" source) --eye x,y,z --yaw deg --pitch deg --eye-height 3
//   --vertex-colors on|off  --routes main,trans,back,moveable,nolight
//   --state-log FILE        write the D3D8 state log (A9): device init + frame 1 (+ frame 2 when --frames >= 2)
//   --lighting faithful|forced   forced = GcRenderNodeGroup::SetLighting(true) on MainArea/MainTrans (half of the night-vision state: the special emissive colour is not modelled)
//   --fog on|off            off drops fog from every draw (debug; the state log is unchanged)
//   --write-install DIR     (--synthetic only, native) also write the synthetic install to DIR/data/... (for oracles)
//   --keep-cpu on|off       keep the CPU vertex/index/mip copies after upload (default off: released)
// Always writes <out>.json metrics (except --survey, whose output IS the JSON).
// Exit codes (as lomn_native): 0 ok, 2 bad args, 3 no WebGPU adapter / install or area unreadable / survey found no
// area, 4 failure (bad area data, an exceeded cap, a write error).
// Arguments (Task 6): --synthetic and --survey take exactly 1 and exclude each other and --install/--level/--area
// (--synthetic) or --level/--area (--survey). Integers are digits only (no sign, space or hex), parsed in 64 bits, then
// range-checked. Floats must be finite: --fov (0,180), --near > 0 and < --far, --far <= 1e7, --eye |x,y,z| <= 1e6,
// --yaw any finite value (normalised to (-180,180]), --pitch [-89.9,89.9], --eye-height |h| <= 1e4.
// Outputs: <out> and <out>.json are deleted up front, so a failed run never leaves a stale output; every stream is
// checked after close(). The null backend with --frames N > 1 keeps only the first frame's call log (creation calls +
// frame 1; later frames are rendered but their identical lines are dropped) so memory stays bounded.
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
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

#include <unistd.h>

#if !defined(__EMSCRIPTEN__) || defined(LOMN_NODE_DISK_FS)
#include <filesystem>
#endif
#if !defined(__EMSCRIPTEN__)
#include <sys/resource.h>

#include "stb_image_write.h"
#endif
#if defined(__APPLE__)
#include <mach/mach.h>
#include <malloc/malloc.h>
#elif defined(__linux__)
#include <malloc.h>
#include <unistd.h>
#endif

using namespace lomn;
using Clock = std::chrono::steady_clock;

namespace {

double ms_since(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

// Resident set size now / peak so far, in bytes (0 where unsupported, e.g. wasm32).
uint64_t rss_now() {
#if defined(__APPLE__)
  mach_task_basic_info_data_t info;
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) return info.resident_size;
  return 0;
#elif defined(__linux__)
  long pages = 0, rss = 0;
  FILE* f = std::fopen("/proc/self/statm", "r");
  if (!f) return 0;
  const int n = std::fscanf(f, "%ld %ld", &pages, &rss);
  std::fclose(f);
  return n == 2 ? uint64_t(rss) * uint64_t(sysconf(_SC_PAGESIZE)) : 0;
#else
  return 0;
#endif
}
// macOS phys_footprint (what Activity Monitor calls Memory): unlike resident_size it excludes pages malloc has
// freed but kept mapped as reusable, so it shows a free() that resident_size hides. 0 elsewhere.
uint64_t footprint_now() {
#if defined(__APPLE__)
  task_vm_info_data_t info;
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) return info.phys_footprint;
#endif
  return 0;
}
// Bytes the C heap has handed out and not freed (all malloc zones on macOS, mallinfo2 on glibc). Unlike RSS and
// phys_footprint, this drops on free(): macOS keeps freed pages mapped for reuse (measured with a 27 MiB alloc/free
// probe: footprint 28.1 -> 28.1 MiB), as does wasm32, whose linear memory never shrinks.
uint64_t heap_in_use() {
#if defined(__APPLE__)
  malloc_statistics_t st{};
  malloc_zone_statistics(nullptr, &st);
  return st.size_in_use;
#elif defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
  return mallinfo2().uordblks + mallinfo2().hblkhd;
#else
  return 0;
#endif
}
uint64_t rss_peak() {
#if defined(__EMSCRIPTEN__)
  return 0;
#else
  struct rusage ru;
  if (getrusage(RUSAGE_SELF, &ru) != 0) return 0;
#if defined(__APPLE__)
  return uint64_t(ru.ru_maxrss);  // bytes on macOS
#else
  return uint64_t(ru.ru_maxrss) * 1024;  // KiB on Linux
#endif
#endif
}

int usage(const char* why) {
  std::fprintf(stderr, "%s\n", why);
  std::fprintf(stderr,
               "usage: lomn_view --install DIR --level LEV --area AREA --out FILE [--backend webgpu|null] [--width W --height H]\n"
               "                 [--fov DEG --near N --far F --eye x,y,z --yaw DEG --pitch DEG --eye-height H] [--frames N]\n"
               "                 [--vertex-colors on|off] [--routes main,trans,back,moveable,nolight] [--state-log FILE]\n"
               "                 [--lighting faithful|forced] [--fog on|off]\n"
               "       lomn_view --synthetic 1 --out FILE [...]   |   lomn_view --install DIR --survey 1 --out FILE.json\n");
  return 2;
}

// Finite floats only ("inf", "nan" and overflowing literals such as 1e39 are rejected).
bool parse_float(const std::string& s, float& out) {
  char* end = nullptr;
  out = std::strtof(s.c_str(), &end);
  return !s.empty() && end && *end == '\0' && std::isfinite(out);
}
bool parse_float_in(const std::string& s, float lo, float hi, float& out) { return parse_float(s, out) && out >= lo && out <= hi; }
// Digits only (a leading digit: no sign, space or 0x), accumulated in u64 with an overflow check (unsigned long is 32-bit
// on wasm32), then range-checked.
bool parse_u32(const std::string& s, uint32_t lo, uint32_t hi, uint32_t& out) {
  if (s.empty()) return false;
  uint64_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    const uint64_t d = static_cast<uint64_t>(c - '0');
    if (v > (UINT64_MAX - d) / 10) return false;
    v = v * 10 + d;
  }
  if (v < lo || v > hi) return false;
  out = static_cast<uint32_t>(v);
  return true;
}
// (-180, 180]; fmod in double, so the result is the same on every platform.
float normalise_yaw(float deg) {
  double y = std::fmod(static_cast<double>(deg), 360.0);
  if (y > 180.0) y -= 360.0;
  if (y <= -180.0) y += 360.0;
  return static_cast<float>(y);
}
// Removes a stale output file; a missing file is fine. False (with errno) when it exists but cannot be removed (e.g. a
// directory), which the caller reports as a write failure.
bool remove_stale(const std::string& path) { return ::unlink(path.c_str()) == 0 || errno == ENOENT; }
// 0 on success, else the exit code (4) after reporting which output could not be removed.
int remove_stale_outputs(const std::string& out) {
  for (const std::string& p : {out, out + ".json"})
    if (!remove_stale(p)) return std::fprintf(stderr, "cannot remove stale output %s: %s\n", p.c_str(), std::strerror(errno)), 4;
  return 0;
}
// Bytes outside printable ASCII (e.g. a 4CC model id from the file) as \xHH, so the JSON stays ASCII.
std::string ascii(const std::string& s) {
  std::string o;
  for (unsigned char c : s) {
    if (c >= 0x20 && c < 0x7f) {
      o.push_back(static_cast<char>(c));
    } else {
      char b[5];
      std::snprintf(b, sizeof b, "\\x%02x", c);
      o += b;
    }
  }
  return o;
}
std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  for (std::string t; std::getline(ss, t, sep);) out.push_back(t);
  return out;
}

std::string num(double v) {
  if (!std::isfinite(v)) return "null";  // backstop: JSON has no NaN/Infinity (the CLI already rejects non-finite input)
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
         ",\"placements\":" + std::to_string(s.placements) + ",\"routes\":" + routes_json(s) + ",\"nonfinite_records\":" +
         std::to_string(s.nonfinite_records) + ",\"ignored_records\":" +
         std::to_string(s.ignored_records) + ",\"disabled_records\":" + std::to_string(s.disabled_records) + ",\"missing_models\":" +
         std::to_string(s.missing_models) + ",\"placements_with_orientation\":" + std::to_string(s.placements_with_orientation) +
         ",\"models\":" + std::to_string(s.models) + ",\"models_without_colors\":" + std::to_string(s.models_without_colors) +
         ",\"subsets\":" + std::to_string(s.subsets) + ",\"alpha_subsets\":" + std::to_string(s.alpha_subsets) + ",\"untextured_subsets\":" +
         std::to_string(s.untextured_subsets) + ",\"untextured_materials\":" + std::to_string(s.untextured_materials) + ",\"draws\":" + std::to_string(s.draws) + ",\"vertices\":" + std::to_string(s.vertices) +
         ",\"triangles\":" + std::to_string(s.triangles) + ",\"geometry_bytes\":" + std::to_string(s.geometry_bytes) + ",\"textures\":" +
         std::to_string(s.textures) + ",\"textures_alpha\":" + std::to_string(s.textures_alpha) + ",\"textures_missing\":" +
         std::to_string(s.textures_missing) + ",\"textures_failed\":" + std::to_string(s.textures_failed) + ",\"texture_bytes_with_mips\":" +
         std::to_string(s.texture_bytes) + ",\"pos_records\":" + std::to_string(a.pos_records) + ",\"start\":" +
         (a.has_start ? "{\"record\":" + std::to_string(a.start_record) + ",\"pos\":" + vec3(a.start) + "}" : std::string("null")) +
         ",\"bounds\":" + (a.has_bounds ? "{\"min\":" + vec3(a.bounds_min) + ",\"max\":" + vec3(a.bounds_max) + "}" : std::string("null"));
}

std::string models_json(const view::AreaData& a) {
  std::vector<std::string> ids;
  for (const view::Model& m : a.models) ids.push_back(ascii(m.id));
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

#if !defined(__EMSCRIPTEN__) || defined(LOMN_NODE_DISK_FS)
// The disk file system's user directory (native, or wasm32 under Node with NODERAWFS).
std::string disk_user_dir() {
  std::error_code ec;
  const std::filesystem::path t = std::filesystem::temp_directory_path(ec);
  return ((ec ? std::filesystem::path("/tmp") : t) / "lomn_view_user").string();
}
#endif

#if !defined(__EMSCRIPTEN__)
// Every <lev>/<area>.blk under data/levels (sorted), CPU-loaded with load_area. Measures the caps' real maxima.
// Entries of a directory, sorted by name, without any throwing std::filesystem call (unreadable entries are skipped).
std::vector<std::filesystem::directory_entry> list_dir(const std::filesystem::path& dir) {
  std::vector<std::filesystem::directory_entry> out;
  std::error_code ec;
  std::filesystem::directory_iterator it(dir, ec), end;
  while (!ec && it != end) {
    out.push_back(*it);
    it.increment(ec);
  }
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.path().filename() < b.path().filename(); });
  return out;
}
bool is_dir(const std::filesystem::directory_entry& d) {
  std::error_code ec;
  return d.is_directory(ec);
}
std::string user_dir() {
  std::error_code ec;
  const std::filesystem::path t = std::filesystem::temp_directory_path(ec);
  return ((ec ? std::filesystem::path("/tmp") : t) / "lomn_view_user").string();
}

int survey(const std::string& install, const std::string& out_path) {
  namespace sfs = std::filesystem;
  auto fsp = fs::open_disk(install, user_dir());
  if (!fsp) return std::fprintf(stderr, "cannot open install %s\n", install.c_str()), 3;
  sfs::path levels;
  for (const auto& d : list_dir(install))
    if (is_dir(d)) {
      std::string n = d.path().filename().string();
      std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (n == "data")
        for (const auto& l : list_dir(d.path())) {
          std::string ln = l.path().filename().string();
          std::transform(ln.begin(), ln.end(), ln.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
          if (ln == "levels") levels = l.path();
        }
    }
  if (levels.empty()) return std::fprintf(stderr, "no data/levels in %s\n", install.c_str()), 3;
  std::vector<std::pair<std::string, std::string>> areas;
  for (const auto& l : list_dir(levels)) {
    if (!is_dir(l)) continue;
    std::string lev = l.path().filename().string();
    for (const auto& f : list_dir(l.path())) {
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
  if (areas.empty()) return std::fprintf(stderr, "survey: no area blockfile under %s\n", levels.string().c_str()), 3;
  std::string rows;
  uint64_t max_place = 0, max_draws = 0, max_tex = 0, max_tex_bytes = 0, max_geo = 0, max_records = 0, failures = 0;
  double total_ms = 0;
  std::string max_place_at, max_draws_at, max_tex_at, max_tex_bytes_at, max_geo_at;
  uint64_t max_pipes = 0, max_states = 0, max_lights = 0, frame_failures = 0, frame_rejects = 0, fog_osi = 0, max_variants = 0;
  uint32_t unsupported = 0;
  std::string max_pipes_at, max_states_at, max_lights_at, max_variants_at;
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
    // M1a-3: one null-backend frame through the D3D8 replay -> pipelines, state calls, draw kinds per area.
    std::string fr = ",\"frame\":null";
    {
      auto nd = rhi::create_null_device(nullptr);
      const rhi::TextureId rt = nd->create_render_target_ex(rhi::RenderTargetDesc{64, 64, true, 1.0f});
      view::AreaRenderer ren;
      std::string uerr;
      if (rt && ren.upload(*nd, a, view::RenderOptions(), &uerr) &&
          ren.render(*nd, rt, a, view::make_camera(a, view::CameraSettings(), 1.0f))) {
        const view::RenderStats rs = ren.last_stats();
        const rhi::FrameStats fs = nd->last_frame_stats();
        fr = ",\"frame\":{\"pipelines\":" + std::to_string(ren.pipelines()) + ",\"draws\":" + std::to_string(fs.draws) + ",\"rejected\":" +
             std::to_string(fs.rejected) + ",\"state_calls\":" + std::to_string(rs.state_calls) + ",\"lit\":" + std::to_string(rs.lit_draws) +
             ",\"fogged\":" + std::to_string(rs.fogged_draws) + ",\"blended\":" + std::to_string(rs.blended_draws) + ",\"alpha_tested\":" +
             std::to_string(rs.alpha_tested_draws) + ",\"null_texture\":" + std::to_string(rs.null_texture_draws) + ",\"shader_variants\":" + std::to_string(rs.shader_variants) + ",\"unsupported_bits\":" +
             std::to_string(rs.unsupported) + "}";
        track(ren.pipelines(), max_pipes, max_pipes_at, where);
        track(rs.state_calls, max_states, max_states_at, where);
        track(rs.shader_variants, max_variants, max_variants_at, where);
        if (fs.rejected) ++frame_rejects;
        unsupported |= rs.unsupported;
      } else {
        ++frame_failures;
      }
    }
    track(a.lights.size(), max_lights, max_lights_at, where);
    fog_osi += a.fog.source == "osi";
    rows += "," + area_counts_json(a) + ",\"load_ms\":" + num(a.stats.load_ms) + ",\"decode_ms\":" + num(a.stats.decode_ms) +
            ",\"warnings\":" + std::to_string(a.warnings.size()) + ",\"warnings_dropped\":" + std::to_string(a.warnings_dropped) + fr +
            ",\"scene\":{\"level_type\":" + std::to_string(a.level_type) + ",\"lights\":" + std::to_string(a.lights.size()) +
            ",\"lights_source\":" + js(a.lights_source) + ",\"fog_source\":" + js(a.fog.source) + ",\"fog_start\":" + num(a.fog.start) +
            ",\"fog_end\":" + num(a.fog.end) + ",\"cave_lighting\":" + (a.cave_lighting ? "true" : "false") + ",\"script_class\":" +
            js(a.script_class) + ",\"special_matches\":" + std::to_string(a.special_matches) + "}}";
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
    << js(max_geo_at) << ",\"pipelines\":" << max_pipes << ",\"pipelines_at\":" << js(max_pipes_at) << ",\"state_calls\":" << max_states
    << ",\"state_calls_at\":" << js(max_states_at) << ",\"lights\":" << max_lights << ",\"lights_at\":" << js(max_lights_at) << ",\"shader_variants\":" << max_variants
    << ",\"shader_variants_at\":" << js(max_variants_at)
    << "},\n\"frames\":{\"failures\":" << frame_failures << ",\"frames_with_rejects\":" << frame_rejects << ",\"unsupported_bits\":"
    << unsupported << ",\"areas_with_osi_fog\":" << fog_osi << "},\n\"areas\":[\n"
    << rows << "]}\n";
  o.close();  // a late write error (e.g. ENOSPC) only shows after the final flush
  if (o.fail()) return std::fprintf(stderr, "cannot write %s\n", out_path.c_str()), 4;
  std::printf("survey: %zu areas, %llu failures -> %s\n", areas.size(), static_cast<unsigned long long>(failures), out_path.c_str());
  return failures || frame_failures || frame_rejects ? 4 : 0;
}
#endif

}  // namespace

int main(int argc, char** argv) {
  if ((argc - 1) % 2 != 0) return usage((std::string("option ") + argv[argc - 1] + " needs a value").c_str());
  std::map<std::string, std::string> a;
  static const char* known[] = {"--install", "--level", "--area", "--out", "--backend", "--width", "--height", "--frames", "--fov", "--near",
                                "--far", "--eye", "--yaw", "--pitch", "--eye-height", "--vertex-colors", "--routes",
                                "--synthetic", "--survey", "--keep-cpu", "--state-log", "--lighting", "--fog", "--write-install"};
  for (int i = 1; i + 1 < argc; i += 2) {
    std::string k = argv[i];
    if (std::find_if(std::begin(known), std::end(known), [&](const char* x) { return k == x; }) == std::end(known))
      return usage(("unknown option " + k).c_str());
    a[k] = argv[i + 1];
  }
  auto has = [&](const char* k) { return a.count(k) != 0; };
  const std::string out = has("--out") ? a["--out"] : "";
  if (out.empty()) return usage("--out is required");
  if (has("--synthetic") && a["--synthetic"] != "1" && a["--synthetic"] != "2") return usage("--synthetic takes 1 (A7 scene) or 2 (A9 state scene)");
  if (has("--survey") && a["--survey"] != "1") return usage("--survey takes exactly 1");
  const bool synthetic = has("--synthetic");
  if (synthetic && (has("--install") || has("--level") || has("--area") || has("--survey")))
    return usage("--synthetic cannot be combined with --install, --level, --area or --survey");
  if (has("--survey") && (has("--level") || has("--area"))) return usage("--survey cannot be combined with --level or --area");
  if (has("--survey")) {
#if defined(__EMSCRIPTEN__)
    return usage("--survey needs the native disk file system");
#else
    if (!has("--install")) return usage("--survey needs --install");
    if (int rc = remove_stale_outputs(out)) return rc;
    return survey(a["--install"], out);
#endif
  }
  std::string level = synthetic ? view::kSyntheticLevel : a["--level"];
  std::string area = synthetic ? (a["--synthetic"] == "2" ? view::kSyntheticStateArea : view::kSyntheticArea) : a["--area"];
  if (!synthetic && (!has("--install") || level.empty() || area.empty())) return usage("--install, --level and --area are required");
  const std::string backend = has("--backend") ? a["--backend"] : "webgpu";
  if (backend != "webgpu" && backend != "null") return usage(("unknown backend " + backend).c_str());

  uint32_t width = 1280, height = 720, frames = 1;
  if (synthetic) width = height = 64;
  view::CameraSettings cs = synthetic ? view::synthetic_camera() : view::CameraSettings();
  // Per-field camera sources (spec M1 §2.3): "cli" when the flag was given; "exe" for FOV/near/far taken from the exe's
  // GcViewPort camera (M1a-3; Base.osi sets none of them); else the documented fallback (the synthetic scene's own
  // values count as fallback: they are not data). clip = near AND far both given.
  const bool pitch_cli = has("--pitch"), fov_cli = has("--fov"), clip_cli = has("--near") && has("--far");
  view::RenderOptions ro;
  view::AreaOptions ao;
  if (has("--width") && !parse_u32(a["--width"], 1, rhi::kMaxRenderTargetSize, width)) return usage("bad --width");
  if (has("--height") && !parse_u32(a["--height"], 1, rhi::kMaxRenderTargetSize, height)) return usage("bad --height");
  if (has("--frames") && !parse_u32(a["--frames"], 1, 100000, frames)) return usage("bad --frames");
  if (has("--fov") && (!parse_float(a["--fov"], cs.fov_deg) || !(cs.fov_deg > 0.0f && cs.fov_deg < 180.0f))) return usage("bad --fov (0,180)");
  if (has("--near") && (!parse_float(a["--near"], cs.near_z) || !(cs.near_z > 0.0f))) return usage("bad --near (> 0)");
  if (has("--far") && !parse_float_in(a["--far"], 0.0f, 1e7f, cs.far_z)) return usage("bad --far (<= 1e7)");
  if (!(cs.far_z > cs.near_z)) return usage("--far must be greater than --near");
  if (has("--eye")) {
    auto p = split(a["--eye"], ',');
    if (p.size() != 3) return usage("bad --eye (x,y,z)");
    for (int k = 0; k < 3; ++k)
      if (!parse_float_in(p[k], -1e6f, 1e6f, cs.eye[k])) return usage("bad --eye (x,y,z, each |v| <= 1e6)");
    cs.eye_set = true;
  }
  if (has("--yaw")) {
    if (!parse_float(a["--yaw"], cs.yaw_deg)) return usage("bad --yaw");
    cs.yaw_deg = normalise_yaw(cs.yaw_deg);
    cs.yaw_set = true;
  }
  if (has("--pitch") && !parse_float_in(a["--pitch"], -view::kMaxPitchDeg, view::kMaxPitchDeg, cs.pitch_deg)) return usage("bad --pitch [-89.9,89.9]");
  if (has("--eye-height") && !parse_float_in(a["--eye-height"], -1e4f, 1e4f, cs.eye_height)) return usage("bad --eye-height (|h| <= 1e4)");
  if (has("--vertex-colors")) {
    if (a["--vertex-colors"] != "on" && a["--vertex-colors"] != "off") return usage("bad --vertex-colors (on|off)");
    ro.vertex_colors = a["--vertex-colors"] == "on";
  }
  if (has("--lighting")) {
    if (a["--lighting"] != "faithful" && a["--lighting"] != "forced") return usage("bad --lighting (faithful|forced)");
    ro.force_area_lighting = a["--lighting"] == "forced";
  }
  if (has("--fog")) {
    if (a["--fog"] != "on" && a["--fog"] != "off") return usage("bad --fog (on|off)");
    ro.fog = a["--fog"] == "on";
  }
  const std::string state_log_path = has("--state-log") ? a["--state-log"] : "";
  if (has("--state-log") && (state_log_path.empty() || state_log_path == out)) return usage("bad --state-log (a file other than --out)");
  bool keep_cpu = false;
  if (has("--keep-cpu")) {
    if (a["--keep-cpu"] != "on" && a["--keep-cpu"] != "off") return usage("bad --keep-cpu (on|off)");
    keep_cpu = a["--keep-cpu"] == "on";
  }
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

  // Arguments are valid: only now drop whatever an earlier run left, so a usage error (exit 2) leaves existing outputs
  // untouched while any later failure (install, device, load, render) can never leave a stale one behind.
  if (int rc = remove_stale_outputs(out)) return rc;
  if (!state_log_path.empty() && !remove_stale(state_log_path))
    return std::fprintf(stderr, "cannot remove stale output %s: %s\n", state_log_path.c_str(), std::strerror(errno)), 4;

  // File system: the synthetic scene in memory, or the install on disk (case-insensitive VFS).
  std::unique_ptr<fs::FileSystem> fsp;
  if (synthetic) {
    auto mem = std::make_unique<fs::MemoryFileSystem>();
    if (!view::mount_synthetic_install(*mem)) return std::fprintf(stderr, "synthetic install failed\n"), 4;
#if !defined(__EMSCRIPTEN__)
    if (has("--write-install")) {  // the synthetic install as files, so independent oracles can read it like a real one
      namespace sfs = std::filesystem;
      const sfs::path root = a["--write-install"];
      std::error_code ec;
      sfs::create_directories(root / "data" / "levels" / "levt", ec);
      for (const char* f : {"Levels/levt/levt.blk", "Levels/levt/test.blk", "Levels/levt/stat.blk", "Base.osi"}) {
        std::vector<uint8_t> b;
        if (mem->read(std::string("Root/Data/") + f, b) != fs::Error::None) return std::fprintf(stderr, "synthetic file %s missing\n", f), 4;
        std::string rel = f;
        for (char& c : rel) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        if (rel == "base.osi") rel = "Base.osi";
        std::ofstream o(root / "data" / rel, std::ios::binary);
        o.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
        o.close();
        if (o.fail()) return std::fprintf(stderr, "cannot write %s\n", (root / "data" / rel).string().c_str()), 4;
      }
    }
#endif
    fsp = std::move(mem);
  } else {
#if defined(__EMSCRIPTEN__) && !defined(LOMN_NODE_DISK_FS)
    return usage("--install needs the native build (the browser path is M1b)");
#else
    fsp = fs::open_disk(a["--install"], disk_user_dir());
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
  view::LoadError load_kind = view::LoadError::None;
  if (!view::load_area(*fsp, level, area, ao, data, &err, &load_kind)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return load_kind == view::LoadError::Environment ? 3 : 4;  // typed (Task 6 C7): no message matching
  }
  const auto t_up = Clock::now();
  rhi::RenderTargetDesc rd;
  rd.width = width;
  rd.height = height;
  rd.depth = true;
  rhi::TextureId rt = dev->create_render_target_ex(rd);
  view::AreaRenderer renderer;
  std::vector<std::string> state_log;
  if (!state_log_path.empty()) ro.state_log = &state_log;
  if (!rt || !renderer.upload(*dev, data, ro, &err)) {
    std::fprintf(stderr, "upload failed: %s\n", rt ? err.c_str() : "render target rejected");
    return 4;
  }
  const double upload_ms = ms_since(t_up);
  // Upload copied every vertex/index array and mip chain to the device: free the CPU copies (unless --keep-cpu on).
  const uint64_t rss_after_upload = rss_now(), fp_after_upload = footprint_now(), heap_after_upload = heap_in_use();
  const uint64_t cpu_freed = keep_cpu ? 0 : view::release_cpu_copies(data);
  const uint64_t rss_after_release = rss_now(), fp_after_release = footprint_now(), heap_after_release = heap_in_use();
  const view::Camera cam = view::make_camera(data, cs, static_cast<float>(width) / static_cast<float>(height));
  std::vector<double> frame_ms;
  size_t first_frame_lines = 0, state_lines = 0;
  for (uint32_t f = 0; f < frames; ++f) {
    const auto t0 = Clock::now();
    if (!renderer.render(*dev, rt, data, cam)) return std::fprintf(stderr, "render refused\n"), 4;
    frame_ms.push_back(ms_since(t0));
    if (f == 0) first_frame_lines = calls.size();
    else calls.resize(first_frame_lines);  // null backend: keep only the first frame's log (bounded memory)
    if (f <= 1) state_lines = state_log.size();
    else state_log.resize(state_lines);  // state log: init + frames 1 and 2 (frame 2 shows the steady state)
  }
  const view::RenderStats rstats = renderer.last_stats();
  if (!state_log_path.empty()) {
    std::ofstream f(state_log_path);
    for (const auto& c : state_log) f << c << '\n';
    f.close();
    if (f.fail()) return std::fprintf(stderr, "cannot write %s\n", state_log_path.c_str()), 4;
  }
  const rhi::FrameStats fstats = dev->last_frame_stats();
  double readback_ms = 0;
  bool wrote = false;
  if (backend == "null") {
    std::ofstream f(out);
    for (const auto& c : calls) f << c << '\n';
    f.close();
    wrote = !f.fail();
  } else {
#if !defined(__EMSCRIPTEN__)
    std::vector<uint8_t> px;
    const auto t0 = Clock::now();
    if (!dev->read_pixels(rt, px)) return std::fprintf(stderr, "readback failed\n"), 4;
    if (px.size() != size_t(width) * height * 4)
      return std::fprintf(stderr, "readback returned %zu bytes, want %zu\n", px.size(), size_t(width) * height * 4), 4;
    readback_ms = ms_since(t0);
    wrote = stbi_write_png(out.c_str(), static_cast<int>(width), static_cast<int>(height), 4, px.data(), static_cast<int>(width) * 4) != 0;
#endif
  }
  if (!wrote) return std::fprintf(stderr, "cannot write %s\n", out.c_str()), 4;

  std::vector<double> sorted = frame_ms;
  std::sort(sorted.begin(), sorted.end());
  const double p50 = sorted[sorted.size() / 2];
  const char* const pitch_src = pitch_cli ? "cli" : "fallback";
  const char* const fov_src = fov_cli ? "cli" : synthetic ? "fallback" : "exe";
  const char* const clip_src = clip_cli ? "cli" : synthetic ? "fallback" : "exe";
  const bool camera_fallback = cam.eye_source == "bounds_centre" || cam.eye_source == "origin" || (cam.yaw_source != "cli") ||
                               !pitch_cli || std::string(fov_src) == "fallback" || std::string(clip_src) == "fallback";
  std::ofstream m(out + ".json");
  m << "{\n\"tool\":\"lomn_view\",\"level\":" << js(data.level) << ",\"area\":" << js(data.area) << ",\"synthetic\":"
    << (synthetic ? "true" : "false") << ",\n\"backend\":" << js(dev->backend()) << ",\"adapter\":" << js(dev->adapter())
    << ",\"width\":" << width << ",\"height\":" << height << ",\n\"frames\":" << frames << ",\"frames_rendered\":" << frame_ms.size()
    << ",\"load_ms\":" << num(data.stats.load_ms) << ",\"decode_ms\":" << num(data.stats.decode_ms) << ",\"upload_ms\":" << num(upload_ms)
    << ",\"frame_submit_ms_first\":" << num(frame_ms.front()) << ",\"frame_submit_ms_p50\":" << num(p50) << ",\"readback_ms\":" << num(readback_ms)
    << ",\n\"rss_after_upload_bytes\":" << rss_after_upload << ",\"cpu_bytes_released\":" << cpu_freed
    << ",\"rss_after_release_bytes\":" << rss_after_release << ",\"rss_peak_bytes\":" << rss_peak()
    << ",\"footprint_after_upload_bytes\":" << fp_after_upload << ",\"footprint_after_release_bytes\":" << fp_after_release
    << ",\"heap_in_use_after_upload_bytes\":" << heap_after_upload << ",\"heap_in_use_after_release_bytes\":" << heap_after_release
    << ",\"rejected_last_frame\":" << fstats.rejected
    << ",\n\"draws_last_frame\":" << fstats.draws << ",\"indices_last_frame\":" << fstats.indices << ",\"pipelines\":" << fstats.pipelines_total
    << ",\"gpu_textures\":" << renderer.textures_created() << ",\"texture_bytes_uploaded\":" << renderer.uploaded_texture_bytes() << ",\n"
    << area_counts_json(data) << ",\n\"model_ids\":" << models_json(data)
    // fallback: true while ANY of eye/yaw/pitch/fov/near/far is a documented fallback. The POS start is data (not a
    // fallback); a scene-set or --yaw is "cli"; the per-field *_source keys say which.
    << ",\n\"camera\":{\"fallback\":" << (camera_fallback ? "true" : "false")
    << ",\"eye\":" << vec3(cam.eye) << ",\"at\":" << vec3(cam.at)
    << ",\"eye_source\":" << js(cam.eye_source) << ",\"yaw_deg\":" << num(cam.yaw_deg) << ",\"yaw_source\":" << js(cam.yaw_source)
    << ",\"pitch_deg\":" << num(cam.pitch_deg) << ",\"pitch_source\":" << js(pitch_src)
    << ",\"fov_deg\":" << num(cam.fov_deg) << ",\"fov_source\":" << js(fov_src) << ",\"near\":" << num(cam.near_z) << ",\"far\":"
    << num(cam.far_z) << ",\"clip_source\":" << js(clip_src) << ",\"aspect\":" << num(cam.aspect)
    << "},\n\"render\":{\"vertex_colors\":" << (ro.vertex_colors ? "true" : "false")
    << ",\"lighting\":" << js(ro.force_area_lighting ? "forced" : "faithful") << ",\"fog\":" << (ro.fog ? "true" : "false") << "}"
    << ",\n\"states\":{\"state_calls_last_frame\":" << rstats.state_calls << ",\"draw_subsets_last_frame\":" << rstats.draw_subsets
    << ",\"empty_subsets_last_frame\":" << rstats.empty_subsets << ",\"rhi_draws_last_frame\":" << rstats.rhi_draws << ",\"lit_draws\":" << rstats.lit_draws << ",\"fogged_draws\":"
    << rstats.fogged_draws << ",\"blended_draws\":" << rstats.blended_draws << ",\"alpha_tested_draws\":" << rstats.alpha_tested_draws
    << ",\"null_texture_draws\":" << rstats.null_texture_draws << ",\"shader_variants\":" << rstats.shader_variants
    << ",\"unsupported_bits\":" << rstats.unsupported << "}"
    << ",\n\"scene\":{\"level_type\":" << unsigned(data.level_type) << ",\"lights\":" << data.lights.size() << ",\"lights_source\":"
    << js(data.lights_source) << ",\"fog\":{\"color\":" << data.fog.color << ",\"start\":" << num(data.fog.start) << ",\"end\":"
    << num(data.fog.end) << ",\"density\":" << num(data.fog.density) << ",\"source\":" << js(data.fog.source) << "}"
    << ",\"cave_lighting\":" << (data.cave_lighting ? "true" : "false") << ",\"script_class\":" << js(data.script_class)
    << ",\"special_textures\":" << data.special_textures << ",\"special_matches\":" << data.special_matches << "}"
    << ",\n\"warnings_dropped\":" << data.warnings_dropped
    << ",\"warnings\":" << warnings_json(data.warnings) << "\n}\n";
  m.close();
  if (m.fail()) return std::fprintf(stderr, "cannot write %s.json\n", out.c_str()), 4;
  std::printf("ok %s/%s backend=%s draws=%u textures=%u out=%s\n", data.level.c_str(), data.area.c_str(), dev->backend().c_str(),
              fstats.draws, data.stats.textures, out.c_str());
  return 0;
}
