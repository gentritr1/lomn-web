// lomn_view: render one area of a LOMN install from the POS start, replaying the game's D3D8 states (M1a-3).
//   lomn_view --install <dir> --level lev1 --area vllg --out <file.png> [options]
//   lomn_view --synthetic 1|2 --out <file> [options]        the A7 contract scene / the A9 state scene (no game data)
//   lomn_view --install <dir> --survey 1 --out <file.json>  CPU-load every area (no GPU), per-area counts + maxima
//   lomn_view --install <dir> --manifest 1 --out <file.json>  M1b: every area's reads at (path, offset, length)
//                           granularity + the boot set (format: docs/knowledge/web-porting-playbook.md "Prefetch manifest")
//   lomn_view --install <dir> --script 1 [--frames N] [--keys "down@100 ..."] [--trace F [--trace-frames a:b]]
//             [--digest F] [--natives-log F] [--natives real|stub] [--deterministic on|off] [--skip-video on|off]
//             [--out F.json]   M2a-2: run Base.osi (Root/Data/base.osi) Setup + N Process frames headless; prints
//                              the A8 coverage report, nesting and stack high-water, and the run summary.
//   --script 1 (M2a-2 T6; natives.md Decision 2): lomn::osi::ScriptHost over the install's VFS (fs::open_disk), the
//   frame loop of lomn_osi_run. Always headless; it takes only the options above (any other option, --script other than
//   1, a missing --install, or a script option without --script is exit 2). --frames 1..100000 (default 1: Setup + one
//   Process frame, the A8 run; there is no Setup-only run). --keys is ScriptHost's KEYSPEC (expand_keys, Task 3).
//   --trace-frames a:b keeps the header and frames a..b, -1 <= a <= b (frame -1 = Setup); it needs --trace. --natives
//   real (default; A9) binds every registered native group and fails closed (exit 4, no report) while none binds;
//   stub = all typed stubs (D11) + the mod rows, the A6 run. --deterministic and --skip-video (default on: D1, D3) reach
//   the natives as HostFlags and are reported in the JSON. --out F.json writes the run metrics to F.json itself (no
//   second .json suffix); without --out no JSON is written. The tables are data/osi (natives.json, natives_beta150.json,
//   types.json, native_overrides.json). stdout, in order: the A8 coverage report (tools/osi/coverage.py's grammar, byte
//   for byte), `nesting high-water %d`, `stack high-water %d`, then lomn_osi_run's summary lines without wall-clock
//   fields (so native and wasm32 stdout are byte-identical; the timings are in the JSON). Exit codes: 0 ok, 2 bad
//   arguments, 3 install / Base.osi / tables unreadable, 4 the host cannot start, a VM error stopped the run, or a write
//   failed.
// Options (every option takes a value):
//   --backend webgpu|null   webgpu (default) writes <out> as PNG; null writes the rhi call log to <out>
//   --width 1280 --height 720 --frames 1
//   --fov 72 --near 0.01 --far 4700 (the exe's camera; "exe" source) --eye x,y,z --yaw deg --pitch deg --eye-height 3
//   --vertex-colors on|off  --routes main,trans,back,moveable,nolight
//   --state-log FILE        write the D3D8 state log (A9): device init + frame 1 (+ frame 2 when --frames >= 2); must
//                           not be --out or --out.json (compared canonically)
//   --lighting faithful|forced   forced = GcRenderNodeGroup::SetLighting(true) on MainArea/MainTrans (half of the night-vision state: the special emissive colour is not modelled)
//   --fog on|off            off drops fog from every draw (debug; the state log is unchanged)
//   --write-install DIR     (--synthetic only, native; else exit 2) also write the synthetic install to DIR/data/...
//   --keep-cpu on|off       keep the CPU vertex/index/mip copies after upload (default off: released)
//   --texture-cache on|off  M1a-4 Task 3 (default off): a decode cache across area loads (lomn/view/texture_cache.h,
//                           256 MiB, LRU, keyed by path + SHA-256 of the bytes). Off constructs no cache and leaves every
//                           output byte-identical; on adds "texture_cache":{hits,misses,bytes,evictions} to the metrics
//                           (and, with --survey, to every area row, failed rows included: this load's hits/misses/
//                           evictions and the bytes resident after it, plus the run's totals at the top level; the rows
//                           sum to the totals). Without --survey it can never hit (one area per process) and only costs
//                           time and heap: lomn_view warns on stderr. Native arm64 only pays off (ARMv8 SHA-256); on
//                           x86-64 and wasm32 the portable SHA-256 makes it slower (docs/knowledge/measurements.md).
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
#include <iterator>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "json_escape.h"
#include "lomn/fs.h"
#include "lomn/osi/host.h"
#include "lomn/osi/json.h"
#include "lomn/osi/registry.h"
#include "lomn/osi/trace.h"
#include "lomn/rhi.h"
#include "lomn/sage/osi.h"
#include "lomn/view/area.h"
#include "lomn/view/prefetch.h"
#include "lomn/view/synthetic.h"
#include "lomn/view/texture_cache.h"

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
               "                 [--lighting faithful|forced] [--fog on|off] [--texture-cache on|off]\n"
               "       lomn_view --synthetic 1 --out FILE [...]   |   lomn_view --install DIR --survey 1 --out FILE.json\n"
               "       lomn_view --install DIR --script 1 [--frames N] [--keys SPEC] [--trace F [--trace-frames a:b]]\n"
               "                 [--digest F] [--natives-log F] [--natives real|stub] [--deterministic on|off]\n"
               "                 [--skip-video on|off] [--out F.json]\n");
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

// M1a-4 Task 3: {"hits","misses","bytes","evictions"} (+ any extra members, already JSON).
std::string texture_cache_json(uint64_t hits, uint64_t misses, uint64_t bytes, uint64_t evictions, const std::string& extra = "") {
  return "{\"hits\":" + std::to_string(hits) + ",\"misses\":" + std::to_string(misses) + ",\"bytes\":" + std::to_string(bytes) +
         ",\"evictions\":" + std::to_string(evictions) + extra + "}";
}

std::string warnings_json(const std::vector<std::string>& w) {
  std::string o = "[";
  for (size_t i = 0; i < w.size(); ++i) o += std::string(i ? "," : "") + js(w[i]);
  return o + "]";
}

#if !defined(__EMSCRIPTEN__) || defined(LOMN_NODE_DISK_FS)
// The disk file system's user directory (native, or wasm32 under Node with NODERAWFS). Task 8B F2: the one definition
// (survey() used a duplicate, user_dir()).
std::string disk_user_dir() {
  std::error_code ec;
  const std::filesystem::path t = std::filesystem::temp_directory_path(ec);
  return ((ec ? std::filesystem::path("/tmp") : t) / "lomn_view_user").string();
}
// Task 8B E2: two output paths name the same file when their canonical forms (absolute, "." / ".." and symlinked
// directories resolved; the file itself need not exist) are equal. Never throws; on an error the spellings compare.
bool same_file(const std::string& a, const std::string& b) {
  namespace sfs = std::filesystem;
  std::error_code ea, eb;
  const sfs::path ca = sfs::weakly_canonical(sfs::absolute(a, ea), ea), cb = sfs::weakly_canonical(sfs::absolute(b, eb), eb);
  return (ea || eb) ? a == b : ca == cb;
}
#else
bool same_file(const std::string& a, const std::string& b) { return a == b; }
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
// Every <lev>/<area>.blk under data/levels (sorted, lower-case), the level blockfile itself excluded. Empty `levels`
// when the install has no data/levels directory.
std::vector<std::pair<std::string, std::string>> list_install_areas(const std::string& install, std::filesystem::path& levels) {
  namespace sfs = std::filesystem;
  levels.clear();
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
  std::vector<std::pair<std::string, std::string>> areas;
  if (levels.empty()) return areas;
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
  return areas;
}
// `cache` (M1a-4 Task 3): nullptr = off (no cache key anywhere in the JSON); else shared by every area's load.
int survey(const std::string& install, const std::string& out_path, view::TextureCache* cache) {
  namespace sfs = std::filesystem;
  auto fsp = fs::open_disk(install, disk_user_dir());
  if (!fsp) return std::fprintf(stderr, "cannot open install %s\n", install.c_str()), 3;
  sfs::path levels;
  const std::vector<std::pair<std::string, std::string>> areas = list_install_areas(install, levels);
  if (levels.empty()) return std::fprintf(stderr, "no data/levels in %s\n", install.c_str()), 3;
  if (areas.empty()) return std::fprintf(stderr, "survey: no area blockfile under %s\n", levels.string().c_str()), 3;
  // REF4 r2 (M6): the survey keeps every <lev>/<x>.blk (123 on Beta 1.5.0, the M1a counts) and marks the ones the game
  // cannot address (view::list_areas rule, the browser's list: 122).
  std::vector<view::AreaRef> addressable;
  {
    std::vector<std::string> paths;
    for (const auto& la : areas) paths.push_back("root/data/levels/" + la.first + "/" + la.second + ".blk");
    addressable = view::list_areas(paths);
  }
  uint64_t n_addressable = 0;
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
    view::AreaOptions ao;
    ao.texture_cache = cache;
    // fix1 M-4: the row's block is the cache's own counter deltas over this load, so it exists on a FAILED row too (a
    // load can look up, store and evict before it fails; AreaData is reset on failure) and the rows sum to the totals.
    const view::TextureCacheStats before = cache ? cache->stats() : view::TextureCacheStats();
    bool ok = view::load_area(*fsp, la.first, la.second, ao, a, &err);
    const double ms = ms_since(t0);
    total_ms += ms;
    const std::string cache_row =
        cache ? ",\"texture_cache\":" + texture_cache_json(cache->stats().hits - before.hits, cache->stats().misses - before.misses,
                                                            cache->stats().bytes, cache->stats().evictions - before.evictions)
              : std::string();
    const std::string where = la.first + "/" + la.second;
    const bool addr = std::find(addressable.begin(), addressable.end(), view::AreaRef{la.first, la.second}) != addressable.end();
    n_addressable += addr;
    rows += std::string(rows.empty() ? "" : ",\n") + "{\"area\":" + js(where) + ",\"addressable\":" + (addr ? "true" : "false") +
            ",\"ok\":" + (ok ? "true" : "false");
    if (!ok) {
      ++failures;
      rows += ",\"error\":" + js(err) + cache_row + "}";
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
            js(a.script_class) + ",\"special_matches\":" + std::to_string(a.special_matches) + "}" + cache_row + "}";
    max_records = std::max<uint64_t>(max_records, a.stats.obj_records);
    track(a.stats.placements, max_place, max_place_at, where);
    track(a.stats.draws, max_draws, max_draws_at, where);
    track(a.stats.textures, max_tex, max_tex_at, where);
    track(a.stats.texture_bytes, max_tex_bytes, max_tex_bytes_at, where);
    track(a.stats.geometry_bytes, max_geo, max_geo_at, where);
  }
  std::ofstream o(out_path);
  o << "{\n\"areas_total\":" << areas.size() << ",\"areas_addressable\":" << n_addressable << ",\"failures\":" << failures << ",\"total_load_ms\":" << num(total_ms) << ",\n\"max\":{\"obj_records\":" << max_records
    << ",\"placements\":" << max_place << ",\"placements_at\":" << js(max_place_at) << ",\"draws\":" << max_draws << ",\"draws_at\":"
    << js(max_draws_at) << ",\"textures\":" << max_tex << ",\"textures_at\":" << js(max_tex_at) << ",\"texture_bytes_with_mips\":"
    << max_tex_bytes << ",\"texture_bytes_at\":" << js(max_tex_bytes_at) << ",\"geometry_bytes\":" << max_geo << ",\"geometry_bytes_at\":"
    << js(max_geo_at) << ",\"pipelines\":" << max_pipes << ",\"pipelines_at\":" << js(max_pipes_at) << ",\"state_calls\":" << max_states
    << ",\"state_calls_at\":" << js(max_states_at) << ",\"lights\":" << max_lights << ",\"lights_at\":" << js(max_lights_at) << ",\"shader_variants\":" << max_variants
    << ",\"shader_variants_at\":" << js(max_variants_at)
    << "},\n\"frames\":{\"failures\":" << frame_failures << ",\"frames_with_rejects\":" << frame_rejects << ",\"unsupported_bits\":"
    << unsupported << ",\"areas_with_osi_fog\":" << fog_osi << "},\n"
    << (cache ? "\"texture_cache\":" + texture_cache_json(cache->stats().hits, cache->stats().misses, cache->stats().bytes, cache->stats().evictions,
                                                          ",\"entries\":" + std::to_string(cache->stats().entries) + ",\"cap_bytes\":" +
                                                              std::to_string(cache->cap_bytes())) + ",\n"
              : std::string())
    << "\"areas\":[\n"
    << rows << "]}\n";
  o.close();  // a late write error (e.g. ENOSPC) only shows after the final flush
  if (o.fail()) return std::fprintf(stderr, "cannot write %s\n", out_path.c_str()), 4;
  std::printf("survey: %zu areas, %llu failures -> %s\n", areas.size(), static_cast<unsigned long long>(failures), out_path.c_str());
  return failures || frame_failures || frame_rejects ? 4 : 0;
}

// M1b prefetch manifest (REF4 item 2). Format "lomn-prefetch-manifest" version 1, one JSON object:
//   files: [{path, size}]            every file any range below reads (normalised path, size in bytes)
//   boot:  {ranges: [[file, offset, length], ...], bytes}   view::boot_reads (Art.blk header+table, Base.osi)
//   areas: [{area: "lev1/vllg", ok, reads, bytes, bytes_beyond_boot, files, ranges: [[file, offset, length], ...]}]
//   summary: area count, failures, max bytes (+ where), union bytes, the ranges common to every area vs the boot set,
//            and the census boot set (whole files) for comparison.
// `file` indexes `files`; ranges are distinct and sorted. Derived from the install's structure (names, offsets, sizes):
// written under build/ only, never committed and never shipped (REF4 decision D2: the browser derives the same sets
// itself from the engine's own reads).
int manifest(const std::string& install, const std::string& out_path) {
  namespace sfs = std::filesystem;
  auto fsp = fs::open_disk(install, disk_user_dir());
  if (!fsp) return std::fprintf(stderr, "cannot open install %s\n", install.c_str()), 3;
  sfs::path levels;
  // REF4 r2 (M6): the rows are the areas the game can address, by the browser's own rule (view::list_areas: ids of 1..4
  // characters, the level blockfile excluded), so both list 122 on Beta 1.5.0; any other <lev>/<x>.blk (lev4c) is
  // reported under not_addressable instead of being "loaded" as its truncated id (the level blockfile).
  std::vector<std::pair<std::string, std::string>> areas;
  std::vector<std::string> not_addressable;
  {
    const std::vector<std::pair<std::string, std::string>> all = list_install_areas(install, levels);
    std::vector<std::string> paths;
    for (const auto& la : all) paths.push_back("root/data/levels/" + la.first + "/" + la.second + ".blk");
    const std::vector<view::AreaRef> ok = view::list_areas(paths);
    for (const auto& la : all) {
      if (std::find(ok.begin(), ok.end(), view::AreaRef{la.first, la.second}) != ok.end()) areas.push_back(la);
      else not_addressable.push_back(la.first + "/" + la.second);
    }
  }
  if (areas.empty()) return std::fprintf(stderr, "manifest: no area blockfile under %s\n", install.c_str()), 3;
  std::map<std::string, uint32_t> file_ids;
  std::vector<std::string> files;
  auto fid = [&](const std::string& p) {
    auto it = file_ids.find(p);
    if (it != file_ids.end()) return it->second;
    const uint32_t id = static_cast<uint32_t>(files.size());
    file_ids[p] = id;
    files.push_back(p);
    return id;
  };
  auto bytes_of = [](const std::vector<fs::ByteRange>& v) {
    uint64_t n = 0;
    for (const auto& r : v) n += r.length;
    return n;
  };
  auto ranges_json = [&](const std::vector<fs::ByteRange>& v) {
    std::string o = "[";
    for (size_t i = 0; i < v.size(); ++i)
      o += std::string(i ? "," : "") + "[" + std::to_string(fid(v[i].path)) + "," + std::to_string(v[i].offset) + "," +
           std::to_string(v[i].length) + "]";
    return o + "]";
  };
  const std::vector<fs::ByteRange> boot = view::record_reads(*fsp, [&] { view::boot_reads(*fsp); });
  const std::set<fs::ByteRange> boot_set(boot.begin(), boot.end());
  std::string rows;
  uint64_t failures = 0, max_bytes = 0, max_beyond = 0, max_reads = 0, sum_beyond = 0;
  std::string max_at, max_beyond_at, max_reads_at;
  std::set<fs::ByteRange> uni;
  std::set<fs::ByteRange> common;
  bool first = true;
  const auto t0 = Clock::now();
  for (const auto& la : areas) {
    view::AreaData a;
    std::string err;
    bool ok = false;
    const std::vector<fs::ByteRange> r =
        view::record_reads(*fsp, [&] { ok = view::load_area(*fsp, la.first, la.second, view::AreaOptions(), a, &err); });
    const std::string where = la.first + "/" + la.second;
    if (!ok) ++failures;
    std::set<std::string> fset;
    uint64_t beyond = 0;
    for (const auto& x : r) {
      fset.insert(x.path);
      uni.insert(x);
      if (!boot_set.count(x)) beyond += x.length;
    }
    const std::set<fs::ByteRange> rs(r.begin(), r.end());
    if (first) {
      common = rs;
      first = false;
    } else {
      std::set<fs::ByteRange> keep;
      std::set_intersection(common.begin(), common.end(), rs.begin(), rs.end(), std::inserter(keep, keep.begin()));
      common.swap(keep);
    }
    const uint64_t b = bytes_of(r);
    sum_beyond += beyond;
    if (b > max_bytes) max_bytes = b, max_at = where;
    if (beyond > max_beyond) max_beyond = beyond, max_beyond_at = where;
    if (r.size() > max_reads) max_reads = r.size(), max_reads_at = where;
    rows += std::string(rows.empty() ? "" : ",\n") + "{\"area\":" + js(where) + ",\"ok\":" + (ok ? "true" : "false") +
            (ok ? std::string() : ",\"error\":" + js(err)) + ",\"reads\":" + std::to_string(r.size()) + ",\"bytes\":" +
            std::to_string(b) + ",\"bytes_beyond_boot\":" + std::to_string(beyond) + ",\"files\":" + std::to_string(fset.size()) +
            ",\"ranges\":" + ranges_json(r) + "}";
  }
  const double ms = ms_since(t0);
  const std::vector<fs::ByteRange> common_v(common.begin(), common.end()), uni_v(uni.begin(), uni.end());
  // The census boot set (docs/census.md, M0a 0.6): whole files.
  std::string census;
  uint64_t census_bytes = 0;
  for (const char* p : {"Root/Data/Main.blk", "Root/Data/Base.osi", "Root/Data/Languages/lang_eng.blk", "Root/Data/Art/Art.blk"}) {
    uint64_t sz = 0;
    const bool there = fsp->size(p, sz) == fs::Error::None;
    const std::string n = fs::normalize(p);
    uint64_t read_by_boot = 0;
    for (const auto& x : boot)
      if (x.path == n) read_by_boot += x.length;
    census_bytes += there ? sz : 0;
    census += std::string(census.empty() ? "" : ",") + "{\"path\":" + js(n) + ",\"present\":" + (there ? "true" : "false") +
              ",\"size\":" + std::to_string(sz) + ",\"boot_bytes_read\":" + std::to_string(read_by_boot) + "}";
  }
  const std::string boot_json = ranges_json(boot), common_json = ranges_json(common_v);
  std::string files_json;  // complete only after every ranges_json call (each may add a file id)
  for (size_t i = 0; i < files.size(); ++i) {
    uint64_t sz = 0;
    fsp->size(files[i], sz);
    files_json += std::string(i ? "," : "") + "{\"path\":" + js(files[i]) + ",\"size\":" + std::to_string(sz) + "}";
  }
  std::ofstream o(out_path);
  o << "{\n\"format\":\"lomn-prefetch-manifest\",\"version\":1,\n\"files\":[" << files_json << "],\n\"boot\":{\"ranges\":" << boot_json
    << ",\"reads\":" << boot.size() << ",\"bytes\":" << bytes_of(boot) << "},\n\"summary\":{\"areas\":" << areas.size()
    << ",\"failures\":" << failures << ",\"wall_ms\":" << num(ms) << ",\"max_bytes\":" << max_bytes << ",\"max_bytes_at\":" << js(max_at)
    << ",\"max_bytes_beyond_boot\":" << max_beyond << ",\"max_bytes_beyond_boot_at\":" << js(max_beyond_at) << ",\"max_reads\":" << max_reads
    << ",\"max_reads_at\":" << js(max_reads_at) << ",\"mean_bytes_beyond_boot\":" << (areas.empty() ? 0 : sum_beyond / areas.size())
    << ",\"union_ranges\":" << uni_v.size() << ",\"union_bytes\":" << bytes_of(uni_v) << ",\"files_read\":" << files.size()
    << ",\"common_ranges\":" << common_json << ",\"common_bytes\":" << bytes_of(common_v)
    << ",\"boot_equals_common\":" << (common == boot_set ? "true" : "false") << ",\"not_addressable\":[" << [&] {
         std::string o;
         for (size_t i = 0; i < not_addressable.size(); ++i) o += std::string(i ? "," : "") + js(not_addressable[i]);
         return o;
       }() << "],\"census_boot\":[" << census
    << "],\"census_boot_bytes\":" << census_bytes << "},\n\"areas\":[\n" << rows << "]}\n";
  o.close();
  if (o.fail()) return std::fprintf(stderr, "cannot write %s\n", out_path.c_str()), 4;
  std::printf("manifest: %zu areas, %llu failures, boot %llu bytes, max %llu bytes (%s) -> %s\n", areas.size(),
              static_cast<unsigned long long>(failures), static_cast<unsigned long long>(bytes_of(boot)),
              static_cast<unsigned long long>(max_bytes), max_at.c_str(), out_path.c_str());
  return failures ? 4 : 0;
}
#endif

// ---- M2a-2 T6: --script 1, the game's own script headless (lomn::osi::ScriptHost) ----
#ifndef LOMN_OSI_DATA_DIR
#define LOMN_OSI_DATA_DIR "data/osi"
#endif

bool slurp(const std::string& p, std::string& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return !f.bad();
}

// The trace sinks of lomn_osi_run (same shapes): a tee, the frame window, and per-block counts from the records.
struct Tee : osi::TraceSink {
  std::vector<osi::TraceSink*> outs;
  void line(std::string_view s) override {
    for (auto* o : outs) o->line(s);
  }
};
struct Window : osi::TraceSink {  // header + frames [a, b]
  osi::TraceSink* out = nullptr;
  long a = -1, b = 0, frame = -1;
  void line(std::string_view s) override {
    if (!s.empty() && s[0] == '#') return out->line(s);
    if (!s.empty() && s[0] == 'K') frame = std::strtol(std::string(s.substr(1)).c_str(), nullptr, 10);
    if (frame >= a && frame <= b) out->line(s);
  }
};
struct ScriptStats : osi::TraceSink {
  uint64_t natives = 0, unregistered = 0;  // N records; E records of an unregistered game function (coverage.py --trace)
  std::set<uint32_t> pcs;
  void line(std::string_view s) override {
    if (s.size() > 10 && s[0] == 'I') pcs.insert(static_cast<uint32_t>(std::strtoul(std::string(s.substr(2, 8)).c_str(), nullptr, 16)));
    else if (!s.empty() && s[0] == 'N') ++natives;
    else if (s.size() > 2 && s[0] == 'E' && s[1] == ' ' && s.find("Call to unregistered Game Function") != std::string_view::npos)
      ++unregistered;
  }
};

uint64_t pct_u(std::vector<uint64_t> v, double p) {  // nearest rank (lomn_osi_run's)
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  size_t r = static_cast<size_t>(std::ceil(p * v.size()));
  if (r < 1) r = 1;
  return v[r - 1];
}
double pct_d(std::vector<double> v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  size_t r = static_cast<size_t>(std::ceil(p * v.size()));
  if (r < 1) r = 1;
  return v[r - 1];
}

// The `body` column of the coverage report, built the way tools/osi/coverage.py builds its registry: natives.json rows
// (winners only) first, then the mod rows; the first registration of a key wins; a row without `body` reports "-".
bool native_bodies(std::string_view natives_json, std::string_view mods_json, std::map<std::string, std::string>& out,
                   std::string* err) {
  for (std::string_view text : {natives_json, mods_json}) {
    osi::Json j;
    if (!osi::parse_json(text, j, err)) return false;
    if (j.type != osi::Json::Array) return *err = "a native table is not a list", false;
    for (const osi::Json& r : j.arr) {
      const osi::Json* w = r.get("winner");
      if (w && w->type == osi::Json::Bool && !w->b) continue;
      std::string key;
      if (const osi::Json* k = r.get("key"); k && k->type == osi::Json::String && !k->str.empty()) {
        key = k->str;
      } else {
        const osi::Json* c = r.get("cls");
        const osi::Json* n = r.get("name");
        if (!c || !n || c->type != osi::Json::String || n->type != osi::Json::String) return *err = "a native row without key", false;
        key = c->str + "." + n->str;
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
      }
      const osi::Json* b = r.get("body");
      out.emplace(key, !b ? "-" : b->type == osi::Json::String ? b->str : b->type == osi::Json::Null ? "None" : "-");
    }
  }
  return true;
}

// The A8 coverage report (tools/osi/coverage.py's grammar, byte for byte): one row per distinct registered native hit,
// sorted by (first-hit frame, first-hit sequence). `calls` counts every native call, `unregistered` the calls to a name
// the registry does not have. From ScriptHost's own hit table (the VM's native observer), not from the natives.log.
std::vector<std::string> coverage_report(const osi::HostStats& hs, const std::map<std::string, std::string>& bodies,
                                         uint64_t unregistered) {
  std::vector<std::pair<std::string, osi::NativeHit>> hits(hs.hits.begin(), hs.hits.end());
  std::sort(hits.begin(), hits.end(), [](const auto& x, const auto& y) {
    return x.second.first_frame != y.second.first_frame ? x.second.first_frame < y.second.first_frame
                                                        : x.second.first_seq < y.second.first_seq;
  });
  std::map<std::string, size_t> tiers;
  for (const auto& h : hits) ++tiers[osi::tier_name(h.second.tier)];
  std::vector<std::string> out;
  char b[512];
  std::snprintf(b, sizeof b, "coverage: natives distinct %zu (tier1 %zu, tier2 %zu, stub %zu, mod %zu, tier3 %zu), calls %llu, unregistered %llu",
                hits.size(), tiers["tier1"], tiers["tier2"], tiers["stub"], tiers["mod"], tiers["tier3"],
                static_cast<unsigned long long>(hs.natives), static_cast<unsigned long long>(unregistered));
  out.push_back(b);
  for (const auto& h : hits) {
    const auto it = bodies.find(h.first);
    std::snprintf(b, sizeof b, "native %-48s tier %-5s first %6ld calls %6llu body %s", h.first.c_str(), osi::tier_name(h.second.tier),
                  h.second.first_frame, static_cast<unsigned long long>(h.second.calls), it == bodies.end() ? "-" : it->second.c_str());
    out.push_back(b);
  }
  return out;
}

// --trace-frames bound: -1 (Setup) or a frame number 0..100000.
bool parse_frame_bound(const std::string& s, long& out) {
  if (s == "-1") return out = -1, true;
  uint32_t v = 0;
  if (!parse_u32(s, 0, 100000, v)) return false;
  out = static_cast<long>(v);
  return true;
}

int script_mode(std::map<std::string, std::string>& a) {
  auto has = [&](const char* k) { return a.count(k) != 0; };
  static const char* const allowed[] = {"--install", "--script", "--frames", "--keys", "--trace", "--trace-frames", "--digest",
                                        "--natives-log", "--natives", "--deterministic", "--skip-video", "--out"};
  for (const auto& kv : a)
    if (std::find_if(std::begin(allowed), std::end(allowed), [&](const char* x) { return kv.first == x; }) == std::end(allowed))
      return usage(("--script cannot be combined with " + kv.first + " (the script mode is headless)").c_str());
  if (a["--script"] != "1") return usage("--script takes exactly 1");
  if (!has("--install") || a["--install"].empty()) return usage("--script needs --install");
  uint32_t frames = 1;
  if (has("--frames") && !parse_u32(a["--frames"], 1, 100000, frames)) return usage("bad --frames (1..100000; 1 = Setup + one frame)");
  const std::string keys = has("--keys") ? a["--keys"] : "";
  {
    std::vector<osi::KeyCall> kc;
    std::string kerr;
    if (!osi::expand_keys(keys, kc, &kerr)) return usage(("bad --keys: " + kerr).c_str());
  }
  long wa = -1, wb = 1L << 30;
  if (has("--trace-frames")) {
    if (!has("--trace")) return usage("--trace-frames needs --trace");
    const std::string& w = a["--trace-frames"];
    const size_t c = w.find(':');
    if (c == std::string::npos || !parse_frame_bound(w.substr(0, c), wa) || !parse_frame_bound(w.substr(c + 1), wb) || wa > wb)
      return usage("bad --trace-frames (a:b, -1 <= a <= b <= 100000)");
  }
  const std::string natives = has("--natives") ? a["--natives"] : "real";
  if (natives != "real" && natives != "stub") return usage("bad --natives (real|stub)");
  osi::HostFlags flags;
  for (const char* k : {"--deterministic", "--skip-video"})
    if (has(k) && a[k] != "on" && a[k] != "off") return usage((std::string("bad ") + k + " (on|off)").c_str());
  flags.deterministic = !has("--deterministic") || a["--deterministic"] == "on";
  flags.skip_video = !has("--skip-video") || a["--skip-video"] == "on";
  std::vector<std::pair<std::string, std::string>> outs;  // (option, path)
  for (const char* k : {"--trace", "--digest", "--natives-log", "--out"})
    if (has(k)) {
      if (a[k].empty()) return usage((std::string("bad ") + k + " (a file)").c_str());
      outs.push_back({k, a[k]});
    }
  for (size_t i = 0; i < outs.size(); ++i)
    for (size_t j = i + 1; j < outs.size(); ++j)
      if (same_file(outs[i].second, outs[j].second))
        return usage((outs[i].first + " and " + outs[j].first + " name the same file").c_str());

  // Arguments are valid: only now drop whatever an earlier run left (a usage error leaves existing outputs untouched).
  for (const auto& o : outs)
    if (!remove_stale(o.second)) return std::fprintf(stderr, "cannot remove stale output %s: %s\n", o.second.c_str(), std::strerror(errno)), 4;
#if defined(__EMSCRIPTEN__) && !defined(LOMN_NODE_DISK_FS)
  return usage("--script needs the disk file system (native, or wasm32 under node)");
#else
  // Decision 2: Base.osi through the VFS at Root/Data/base.osi, the literal GcGame::SetUpScripting passes to CreateVM.
  const std::string install = a["--install"];
  std::unique_ptr<fs::FileSystem> fsp = fs::open_disk(install, disk_user_dir());
  if (!fsp) return std::fprintf(stderr, "cannot open install %s\n", install.c_str()), 3;
  std::vector<uint8_t> image;
  if (fsp->read("Root/Data/base.osi", image) != fs::Error::None)
    return std::fprintf(stderr, "cannot read Root/Data/base.osi in %s\n", install.c_str()), 3;
  const std::string data_dir = LOMN_OSI_DATA_DIR;
  std::string tj, nj, mj, ov;
  if (!slurp(data_dir + "/types.json", tj) || !slurp(data_dir + "/natives.json", nj) || !slurp(data_dir + "/natives_beta150.json", mj))
    return std::fprintf(stderr, "cannot read the OSI tables in %s\n", data_dir.c_str()), 3;
  slurp(data_dir + "/native_overrides.json", ov);  // optional (natives.md §7.2), as in lomn_osi_run
  auto sha = [](const std::string& s) { return osi::sha256_hex(reinterpret_cast<const uint8_t*>(s.data()), s.size()); };
  const std::string sha_before = osi::sha256_hex(image.data(), image.size());
  sage::OsiFile file;
  std::string err;
  if (!file.parse(image, &err)) return std::fprintf(stderr, "Root/Data/base.osi: %s\n", err.c_str()), 3;
  std::map<std::string, std::string> bodies;
  if (!native_bodies(nj, mj, bodies, &err)) return std::fprintf(stderr, "native tables: %s\n", err.c_str()), 4;

  // Sinks and header exactly as lomn_osi_run writes them (so the digest equals its digest on the same script and tables).
  std::string keys_field = keys;
  std::replace(keys_field.begin(), keys_field.end(), ' ', ',');
  std::string header = osi::header_line(sha_before, sha(nj), sha(mj), sha(tj));
  if (!keys.empty()) header += " keys=" + keys_field;
  Tee tee;
  ScriptStats stats;
  tee.outs.push_back(&stats);
  std::unique_ptr<osi::FileTrace> trace_file, digest_file, log_file;
  std::unique_ptr<Window> window;
  std::unique_ptr<osi::DigestTrace> digest;
  auto drop_outputs = [&] {  // the host never started: no partial output stays behind
    trace_file.reset();
    digest_file.reset();
    log_file.reset();
    for (const auto& o : outs) remove_stale(o.second);
  };
  if (has("--trace")) {
    trace_file = std::make_unique<osi::FileTrace>(a["--trace"]);
    if (!trace_file->ok()) return drop_outputs(), std::fprintf(stderr, "cannot write %s\n", a["--trace"].c_str()), 4;
    window = std::make_unique<Window>();
    window->out = trace_file.get();
    window->a = wa;
    window->b = wb;
    tee.outs.push_back(window.get());
  }
  if (has("--digest")) {
    digest_file = std::make_unique<osi::FileTrace>(a["--digest"]);
    if (!digest_file->ok()) return drop_outputs(), std::fprintf(stderr, "cannot write %s\n", a["--digest"].c_str()), 4;
    digest = std::make_unique<osi::DigestTrace>(digest_file.get());
    tee.outs.push_back(digest.get());
  }
  if (has("--natives-log")) {
    log_file = std::make_unique<osi::FileTrace>(a["--natives-log"]);
    if (!log_file->ok()) return drop_outputs(), std::fprintf(stderr, "cannot write %s\n", a["--natives-log"].c_str()), 4;
  }
  tee.line(header);
  osi::ScriptHost::Options opt;
  opt.flags = flags;
  opt.keys = keys;
  opt.real_natives = natives == "real";
  std::unique_ptr<osi::ScriptHost> host;
  try {
    host = std::make_unique<osi::ScriptHost>(file, nj, mj, tj, ov, fsp.get(), &tee, log_file.get(), opt);
  } catch (const std::exception& e) {
    drop_outputs();
    return std::fprintf(stderr, "lomn_view --script: %s\n", e.what()), 4;
  }
  osi::Vm& vm = host->vm();

  // The run: lomn_osi_run's loop and statistics.
  auto t0 = Clock::now();
  host->setup();
  const double setup_ms = ms_since(t0);
  const uint64_t setup_insns = vm.instructions(), setup_natives = stats.natives, setup_pcs = stats.pcs.size();
  std::vector<uint64_t> insns_per_frame, natives_per_frame, pcs_per_frame;
  std::vector<double> ms_per_frame;
  std::set<uint32_t> seen = stats.pcs;
  long flatten = -1;
  for (long k = 0; k < static_cast<long>(frames) && host->stats().stop == "none"; ++k) {
    stats.pcs.clear();
    const uint64_t i0 = vm.instructions(), n0 = stats.natives;
    const auto f0 = Clock::now();
    host->frame();
    ms_per_frame.push_back(ms_since(f0));
    insns_per_frame.push_back(vm.instructions() - i0);
    natives_per_frame.push_back(stats.natives - n0);
    pcs_per_frame.push_back(stats.pcs.size());
    for (uint32_t pc : stats.pcs)
      if (seen.insert(pc).second) flatten = k;
  }
  const osi::HostStats& hs = host->stats();
  if (digest) digest->finish();
  trace_file.reset();
  digest_file.reset();
  const long done = static_cast<long>(insns_per_frame.size());
  const bool unchanged = osi::sha256_hex(file.bytes().data(), file.bytes().size()) == sha_before;

  std::string o;
  for (const std::string& l : coverage_report(hs, bodies, stats.unregistered)) o += l + "\n";
  char b[512];
  auto add = [&](const char* fmt, auto... v) {
    std::snprintf(b, sizeof b, fmt, v...);
    o += b;
  };
  add("nesting high-water %d\n", hs.max_nesting);
  add("stack high-water %u\n", hs.stack_high_water);
  add("setup: instructions %llu, natives %llu, distinct_pcs %llu\n", (unsigned long long)setup_insns, (unsigned long long)setup_natives,
      (unsigned long long)setup_pcs);
  add("frames %ld (of %ld requested), instructions %llu, natives %llu, insns/frame p50 %llu p95 %llu (n=%ld)\n", done,
      static_cast<long>(frames), (unsigned long long)vm.instructions(), (unsigned long long)stats.natives,
      (unsigned long long)pct_u(insns_per_frame, 0.5), (unsigned long long)pct_u(insns_per_frame, 0.95), done);
  o += "distinct_pcs_by_frame (first 5, last):";
  for (long i = 0; i < std::min<long>(5, done); ++i) add(" %llu", (unsigned long long)pcs_per_frame[i]);
  if (done > 5) add(" ... %llu", (unsigned long long)pcs_per_frame.back());
  o += "\n";
  add("process passes: total %llu over %ld frames, max %llu, frames with re-runs %llu", (unsigned long long)hs.passes_total, done,
      (unsigned long long)hs.max_passes, (unsigned long long)hs.frames_rerun);
  if (!hs.first_reruns.empty()) {
    o += " (first:";
    for (const auto& s : hs.first_reruns) o += " " + s;
    o += ")";
  }
  add(", frames skipped %llu", (unsigned long long)hs.frames_skipped);
  if (!hs.first_skips.empty()) {
    o += " (first:";
    for (const auto& s : hs.first_skips) o += " " + s;
    o += ")";
  }
  o += "\n";
  o += "stop: " + hs.stop + "\n";
  o += std::string("script sha256 unchanged: ") + (unchanged ? "True" : "False") + "\n";
  add("flattening: the last frame that executed a new PC is %ld; distinct PCs overall %zu; natives/frame p50 %llu p95 %llu\n",
      flatten, seen.size(), (unsigned long long)pct_u(natives_per_frame, 0.5), (unsigned long long)pct_u(natives_per_frame, 0.95));
  std::fwrite(o.data(), 1, o.size(), stdout);
  std::fflush(stdout);

  if (has("--out")) {  // D1: the --skip-video and --deterministic states are reported in the metrics
    std::ofstream m(a["--out"]);
    m << "{\n\"tool\":\"lomn_view\",\"mode\":\"script\",\"script\":\"Root/Data/base.osi\",\"script_sha256\":" << js(sha_before)
      << ",\"natives\":" << js(natives) << ",\n\"skip_video\":" << (flags.skip_video ? "true" : "false")
      << ",\"deterministic\":" << (flags.deterministic ? "true" : "false") << ",\"keys\":" << js(keys) << ",\n\"frames\":" << frames
      << ",\"frames_run\":" << done << ",\"stop\":" << js(hs.stop) << ",\"script_unchanged\":" << (unchanged ? "true" : "false")
      << ",\n\"setup_instructions\":" << setup_insns << ",\"instructions\":" << vm.instructions() << ",\"natives_calls\":" << hs.natives
      << ",\"natives_distinct\":" << hs.hits.size() << ",\"unregistered\":" << stats.unregistered
      << ",\n\"nesting_high_water\":" << hs.max_nesting << ",\"stack_high_water\":" << hs.stack_high_water
      << ",\"passes_total\":" << hs.passes_total << ",\"frames_rerun\":" << hs.frames_rerun << ",\"frames_skipped\":" << hs.frames_skipped
      << ",\"flattening_frame\":" << flatten << ",\n\"insns_per_frame_p50\":" << pct_u(insns_per_frame, 0.5)
      << ",\"insns_per_frame_p95\":" << pct_u(insns_per_frame, 0.95) << ",\"insns_samples\":" << insns_per_frame.size()
      << ",\"natives_per_frame_p50\":" << pct_u(natives_per_frame, 0.5) << ",\"natives_per_frame_p95\":" << pct_u(natives_per_frame, 0.95)
      << ",\n\"script_ms_per_frame_p50\":" << num(pct_d(ms_per_frame, 0.5)) << ",\"script_ms_per_frame_p95\":" << num(pct_d(ms_per_frame, 0.95))
      << ",\"setup_ms\":" << num(setup_ms) << ",\"rss_peak_bytes\":" << rss_peak() << "\n}\n";
    m.close();
    if (m.fail()) return std::fprintf(stderr, "cannot write %s\n", a["--out"].c_str()), 4;
  }
  return hs.stop == "none" && unchanged ? 0 : 4;
#endif
}

}  // namespace

int main(int argc, char** argv) {
  if ((argc - 1) % 2 != 0) return usage((std::string("option ") + argv[argc - 1] + " needs a value").c_str());
  std::map<std::string, std::string> a;
  static const char* known[] = {"--install", "--level", "--area", "--out", "--backend", "--width", "--height", "--frames", "--fov", "--near",
                                "--far", "--eye", "--yaw", "--pitch", "--eye-height", "--vertex-colors", "--routes",
                                "--synthetic", "--survey", "--keep-cpu", "--state-log", "--lighting", "--fog", "--write-install",
                                "--texture-cache", "--manifest",
                                // M2a-2 T6 (I10): the --script mode's options
                                "--script", "--keys", "--trace", "--trace-frames", "--digest", "--natives-log", "--natives",
                                "--deterministic", "--skip-video"};
  for (int i = 1; i + 1 < argc; i += 2) {
    std::string k = argv[i];
    if (std::find_if(std::begin(known), std::end(known), [&](const char* x) { return k == x; }) == std::end(known))
      return usage(("unknown option " + k).c_str());
    a[k] = argv[i + 1];
  }
  auto has = [&](const char* k) { return a.count(k) != 0; };
  if (has("--script")) return script_mode(a);  // exempt from --out (written only when given) and from the area options
  for (const char* k : {"--keys", "--trace", "--trace-frames", "--digest", "--natives-log", "--natives", "--deterministic", "--skip-video"})
    if (has(k)) return usage((std::string(k) + " needs --script 1").c_str());
  const std::string out = has("--out") ? a["--out"] : "";
  if (out.empty()) return usage("--out is required");
  if (has("--synthetic") && a["--synthetic"] != "1" && a["--synthetic"] != "2") return usage("--synthetic takes 1 (A7 scene) or 2 (A9 state scene)");
  if (has("--survey") && a["--survey"] != "1") return usage("--survey takes exactly 1");
  const bool synthetic = has("--synthetic");
  if (synthetic && (has("--install") || has("--level") || has("--area") || has("--survey")))
    return usage("--synthetic cannot be combined with --install, --level, --area or --survey");
  if (has("--survey") && (has("--level") || has("--area"))) return usage("--survey cannot be combined with --level or --area");
  if (has("--manifest")) {  // M1b
    if (a["--manifest"] != "1") return usage("--manifest takes exactly 1");
    // C-f6: --texture-cache (any value) is a combination error too: the manifest pass has no decode cache to configure.
    if (synthetic || has("--survey") || has("--level") || has("--area") || has("--texture-cache")) return usage("--manifest takes only --install and --out");
#if defined(__EMSCRIPTEN__)
    return usage("--manifest needs the native disk file system");
#else
    if (!has("--install")) return usage("--manifest needs --install");
    if (int rc = remove_stale_outputs(out)) return rc;
    return manifest(a["--install"], out);
#endif
  }
  // M1a-4 Task 3: the cache object exists only when the flag is on; off (the default) never constructs one.
  if (has("--texture-cache") && a["--texture-cache"] != "on" && a["--texture-cache"] != "off") return usage("bad --texture-cache (on|off)");
  const bool texture_cache_on = has("--texture-cache") && a["--texture-cache"] == "on";
  std::unique_ptr<view::TextureCache> texture_cache;
  if (has("--survey")) {
#if defined(__EMSCRIPTEN__)
    return usage("--survey needs the native disk file system");
#else
    if (!has("--install")) return usage("--survey needs --install");
    if (int rc = remove_stale_outputs(out)) return rc;
    if (texture_cache_on) texture_cache = std::make_unique<view::TextureCache>();
    return survey(a["--install"], out, texture_cache.get());
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
  // values count as fallback: they are not data). Task 8B F1: clip = "cli" when near AND far are both given,
  // "cli_partial" when only one is (the other keeps its exe value, or its fallback on the synthetic scene); else exe /
  // fallback. (Every other camera flag sets its whole field, so clip is the only partial override.)
  const bool pitch_cli = has("--pitch"), fov_cli = has("--fov"), clip_cli = has("--near") && has("--far");
  const bool clip_partial = !clip_cli && (has("--near") || has("--far"));
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
  // Task 8B E2: compared canonically with BOTH outputs (--out and --out.json), so "./x" or "d/../x.json" is caught too.
  if (has("--state-log") && (state_log_path.empty() || same_file(state_log_path, out) || same_file(state_log_path, out + ".json")))
    return usage("bad --state-log (a file other than --out and --out.json)");
  // Task 8B E2: --write-install writes the synthetic install only (native); empty is rejected (it would write into the
  // working directory), and under Node it is a usage error instead of being silently ignored.
  if (has("--write-install")) {
#if defined(__EMSCRIPTEN__)
    return usage("--write-install needs the native build");
#else
    if (!synthetic) return usage("--write-install needs --synthetic");
    if (a["--write-install"].empty()) return usage("bad --write-install (a directory)");
#endif
  }
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

  if (texture_cache_on) {
    // fix1 M-5: one area per process can never hit; the cache only hashes, copies and holds a second copy.
    std::fprintf(stderr, "lomn_view: warning: --texture-cache on without --survey loads one area and can never hit (cost only)\n");
    texture_cache = std::make_unique<view::TextureCache>();
    ao.texture_cache = texture_cache.get();
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
  std::vector<double> frame_starts_ms;  // M1a-4 A12: wall-clock start of each frame; the dt samples are the n-1 differences
  const auto t_loop = Clock::now();
  size_t first_frame_lines = 0, state_lines = 0;
  for (uint32_t f = 0; f < frames; ++f) {
    const auto t0 = Clock::now();
    frame_starts_ms.push_back(std::chrono::duration<double, std::milli>(t0 - t_loop).count());
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
  // M1a-4 A12: nearest-rank p95 (ceil(0.95 n) - 1), and the frame-to-frame dt samples (n - 1 of them) so the perf history can
  // reconcile samples against frames before it records any percentile.
  const double p95 = sorted[(sorted.size() * 95 + 99) / 100 - 1];
  std::vector<double> dts;
  for (size_t i = 1; i < frame_starts_ms.size(); ++i) dts.push_back(frame_starts_ms[i] - frame_starts_ms[i - 1]);
  std::sort(dts.begin(), dts.end());
  const double dt_p50 = dts.empty() ? 0.0 : dts[dts.size() / 2];
  const char* const pitch_src = pitch_cli ? "cli" : "fallback";
  const char* const fov_src = fov_cli ? "cli" : synthetic ? "fallback" : "exe";
  const char* const clip_src = clip_cli ? "cli" : clip_partial ? "cli_partial" : synthetic ? "fallback" : "exe";
  // A partial clip override on the synthetic scene still leaves one fallback plane.
  const bool clip_fallback = std::string(clip_src) == "fallback" || (clip_partial && synthetic);
  const bool camera_fallback = cam.eye_source == "bounds_centre" || cam.eye_source == "origin" || (cam.yaw_source != "cli") ||
                               !pitch_cli || std::string(fov_src) == "fallback" || clip_fallback;
  std::ofstream m(out + ".json");
  m << "{\n\"tool\":\"lomn_view\",\"level\":" << js(data.level) << ",\"area\":" << js(data.area) << ",\"synthetic\":"
    << (synthetic ? "true" : "false") << ",\n\"backend\":" << js(dev->backend()) << ",\"adapter\":" << js(dev->adapter())
    << ",\"width\":" << width << ",\"height\":" << height << ",\n\"frames\":" << frames << ",\"frames_rendered\":" << frame_ms.size()
    << ",\"load_ms\":" << num(data.stats.load_ms) << ",\"decode_ms\":" << num(data.stats.decode_ms) << ",\"upload_ms\":" << num(upload_ms)
    << ",\"frame_submit_ms_first\":" << num(frame_ms.front()) << ",\"frame_submit_ms_p50\":" << num(p50) << ",\"frame_submit_ms_p95\":" << num(p95)
    << ",\"frame_dt_samples\":" << dts.size() << ",\"frame_dt_ms_p50\":" << num(dt_p50) << ",\"readback_ms\":" << num(readback_ms)
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
    << (cam.warning.empty() ? std::string() : ",\"warning\":" + js(cam.warning))  // M1a-4 I-2: only when it fell back
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
    << (texture_cache ? ",\n\"texture_cache\":" + texture_cache_json(texture_cache->stats().hits, texture_cache->stats().misses,
                                                                     texture_cache->stats().bytes, texture_cache->stats().evictions)
                      : std::string())
    << ",\n\"warnings_dropped\":" << data.warnings_dropped
    << ",\"warnings\":" << warnings_json(data.warnings) << "\n}\n";
  m.close();
  if (m.fail()) return std::fprintf(stderr, "cannot write %s.json\n", out.c_str()), 4;
  std::printf("ok %s/%s backend=%s draws=%u textures=%u out=%s\n", data.level.c_str(), data.area.c_str(), dev->backend().c_str(),
              fstats.draws, data.stats.textures, out.c_str());
  return 0;
}
