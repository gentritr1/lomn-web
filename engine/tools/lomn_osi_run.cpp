// M2a-1 T4: lomn_osi_run, the A6 differential run of the real Base.osi (consult §4.1, Task 6 brief + controller addendum).
//
//   lomn_osi_run BASE.osi [--data-dir data/osi] [--frames 600] [--keys "down@100 down@160 enter@220 esc@280"]
//                [--trace FILE [--trace-frames a:b]] [--digest FILE] [--natives-log FILE] [--perf-out FILE --commit C]
//                [--print-keys] [--real-natives]
//
// The run: Setup(); then for frame k = 0..frames-1: the fake time advances, the `K` record, that frame's key Input calls
// (KEYSPEC, tools/osi/keys.py has the same expansion), then Process(time, dt, counter) with the re-run rule. All natives are
// the registry's typed stubs (D11) plus the Beta 1.5.0 mod rows (D12), or with --real-natives every registered native
// group (M2a-2). The run stops at the first VM error. M2a-2 T3: Setup and the frame loop are lomn::osi::ScriptHost
// (engine/src/osi/host.cpp, which carries the asm citations of the loop); this tool adds the sinks and the statistics.
// natives.log is version 2 (D14: ` G`, `H`, `C`, `X`, `K` records; docs/osi/natives.md §3).
//
// Trace (docs/osi/trace.md): header + `keys=`; `--trace-frames a:b` keeps the header and the lines of frames a..b (frame
// -1 = everything before the first K, i.e. Setup). Digest: `D` per frame block (pre-K block = frame -1) and `T`.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "lomn/osi/heap.h"
#include "lomn/osi/host.h"
#include "lomn/osi/registry.h"
#include "lomn/osi/trace.h"
#include "lomn/osi/vm.h"
#include "lomn/sage/osi.h"

#ifndef LOMN_OSI_DATA_DIR
#define LOMN_OSI_DATA_DIR "data/osi"
#endif

using namespace lomn;
using namespace lomn::osi;

namespace {

bool slurp(const std::string& p, std::string& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

// ---- sinks ----
struct Tee : TraceSink {
  std::vector<TraceSink*> outs;
  void line(std::string_view s) override {
    for (auto* o : outs) o->line(s);
  }
};
struct Window : TraceSink {  // header + frames [a, b]
  TraceSink* out;
  long a, b, frame = -1;
  void line(std::string_view s) override {
    if (!s.empty() && s[0] == '#') return out->line(s);
    if (!s.empty() && s[0] == 'K') frame = std::strtol(std::string(s.substr(1)).c_str(), nullptr, 10);
    if (frame >= a && frame <= b) out->line(s);
  }
};
struct Stats : TraceSink {  // per-block counts from the records themselves
  uint64_t natives = 0;
  std::set<uint32_t> pcs;
  void line(std::string_view s) override {
    if (s.size() > 10 && s[0] == 'I') pcs.insert(static_cast<uint32_t>(std::strtoul(std::string(s.substr(2, 8)).c_str(), nullptr, 16)));
    else if (!s.empty() && s[0] == 'N') ++natives;
  }
};

uint64_t pct(std::vector<uint64_t> v, double p) {  // nearest rank
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  size_t r = static_cast<size_t>(std::ceil(p * v.size()));
  if (r < 1) r = 1;
  return v[r - 1];
}
double pctd(std::vector<double> v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  size_t r = static_cast<size_t>(std::ceil(p * v.size()));
  if (r < 1) r = 1;
  return v[r - 1];
}

}  // namespace

int main(int argc, char** argv) {
  std::string osi, data_dir = LOMN_OSI_DATA_DIR, keys, trace_path, digest_path, log_path, perf_path, commit = "unknown";
  long frames = 600, wa = -1, wb = 1L << 30;
  bool print_keys = false, real_natives = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "lomn_osi_run: %s needs a value\n", a.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--data-dir") data_dir = next();
    else if (a == "--frames") frames = std::strtol(next().c_str(), nullptr, 10);
    else if (a == "--keys") keys = next();
    else if (a == "--trace") trace_path = next();
    else if (a == "--trace-frames") {
      const std::string w = next();
      const size_t c = w.find(':');
      if (c == std::string::npos) return std::fprintf(stderr, "lomn_osi_run: --trace-frames a:b\n"), 2;
      wa = std::strtol(w.substr(0, c).c_str(), nullptr, 10);
      wb = std::strtol(w.substr(c + 1).c_str(), nullptr, 10);
    } else if (a == "--digest") digest_path = next();
    else if (a == "--natives-log") log_path = next();
    else if (a == "--perf-out") perf_path = next();
    else if (a == "--commit") commit = next();
    else if (a == "--print-keys") print_keys = true;
    else if (a == "--real-natives") real_natives = true;
    else if (!a.empty() && a[0] == '-') return std::fprintf(stderr, "lomn_osi_run: unknown option %s\n", a.c_str()), 2;
    else osi = a;
  }
  std::vector<KeyCall> key_calls;
  std::string err;
  if (!expand_keys(keys, key_calls, &err)) return std::fprintf(stderr, "lomn_osi_run: %s\n", err.c_str()), 2;
  if (print_keys) {  // the same listing tools/osi/keys.py prints
    for (const auto& k : key_calls) {
      std::printf("%d Input", k.frame);
      for (const auto& v : k.args) std::printf(" %s", fmt_variant(v).c_str());
      std::printf("\n");
    }
    return 0;
  }
  if (osi.empty()) return std::fprintf(stderr, "usage: lomn_osi_run BASE.osi [options]\n"), 2;
  std::string image, tj, nj, mj;
  if (!slurp(osi, image)) return std::fprintf(stderr, "lomn_osi_run: cannot read %s\n", osi.c_str()), 2;
  if (!slurp(data_dir + "/types.json", tj) || !slurp(data_dir + "/natives.json", nj) || !slurp(data_dir + "/natives_beta150.json", mj))
    return std::fprintf(stderr, "lomn_osi_run: cannot read the tables in %s\n", data_dir.c_str()), 2;
  const std::string sha_before = sha256_hex(reinterpret_cast<const uint8_t*>(image.data()), image.size());
  sage::OsiFile file;
  if (!file.parse(std::vector<uint8_t>(image.begin(), image.end()), &err)) return std::fprintf(stderr, "lomn_osi_run: %s\n", err.c_str()), 2;
  std::string ov;  // the overrides are cross-checked against the merged registry (natives.md §7.2); optional
  slurp(data_dir + "/native_overrides.json", ov);
  auto sha = [](const std::string& s) { return sha256_hex(reinterpret_cast<const uint8_t*>(s.data()), s.size()); };
  std::string keys_field = keys;
  std::replace(keys_field.begin(), keys_field.end(), ' ', ',');
  std::string header = header_line(sha_before, sha(nj), sha(mj), sha(tj));
  if (!keys.empty()) header += " keys=" + keys_field;

  Tee tee;
  Stats stats;
  tee.outs.push_back(&stats);
  std::unique_ptr<FileTrace> trace_file, digest_file, log_file;
  std::unique_ptr<Window> window;
  std::unique_ptr<DigestTrace> digest;
  if (!trace_path.empty()) {
    trace_file = std::make_unique<FileTrace>(trace_path);
    if (!trace_file->ok()) return std::fprintf(stderr, "lomn_osi_run: cannot write %s\n", trace_path.c_str()), 2;
    window = std::make_unique<Window>();
    window->out = trace_file.get();
    window->a = wa;
    window->b = wb;
    tee.outs.push_back(window.get());
  }
  if (!digest_path.empty()) {
    digest_file = std::make_unique<FileTrace>(digest_path);
    if (!digest_file->ok()) return std::fprintf(stderr, "lomn_osi_run: cannot write %s\n", digest_path.c_str()), 2;
    digest = std::make_unique<DigestTrace>(digest_file.get());
    tee.outs.push_back(digest.get());
  }
  if (!log_path.empty()) {
    log_file = std::make_unique<FileTrace>(log_path);
    if (!log_file->ok()) return std::fprintf(stderr, "lomn_osi_run: cannot write %s\n", log_path.c_str()), 2;
  }
  tee.line(header);
  ScriptHost::Options opt;
  opt.keys = keys;
  opt.real_natives = real_natives;
  std::unique_ptr<ScriptHost> host;
  try {
    host = std::make_unique<ScriptHost>(file, nj, mj, tj, ov, nullptr, &tee, log_file.get(), opt);
  } catch (const std::exception& e) {
    return std::fprintf(stderr, "lomn_osi_run: %s\n", e.what()), 2;
  }
  Vm& vm = host->vm();

  // T11: the 1-minute loadavg at the start and at the end of the run (perf_history.py reads both keys); -1 = unavailable
  auto loadavg1 = []() { double l[1] = {-1.0}; return ::getloadavg(l, 1) == 1 ? l[0] : -1.0; };
  const double loadavg_start = loadavg1();
  auto t0 = std::chrono::steady_clock::now();
  host->setup();
  const double setup_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  const uint64_t setup_insns = vm.instructions(), setup_natives = stats.natives, setup_pcs = stats.pcs.size();

  std::vector<uint64_t> insns_per_frame, natives_per_frame, pcs_per_frame;
  std::vector<double> ms_per_frame;
  std::set<uint32_t> seen = stats.pcs;
  long flatten = -1;
  for (long k = 0; k < frames && host->stats().stop == "none"; ++k) {
    stats.pcs.clear();
    const uint64_t i0 = vm.instructions(), n0 = stats.natives;
    auto f0 = std::chrono::steady_clock::now();
    host->frame();
    ms_per_frame.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - f0).count());
    insns_per_frame.push_back(vm.instructions() - i0);
    natives_per_frame.push_back(stats.natives - n0);
    pcs_per_frame.push_back(stats.pcs.size());
    for (uint32_t pc : stats.pcs)
      if (seen.insert(pc).second) flatten = k;
  }
  const HostStats& hs = host->stats();
  const std::string& stop = hs.stop;
  if (digest) digest->finish();
  const long done = static_cast<long>(insns_per_frame.size());
  const std::string sha_after = sha256_hex(file.bytes().data(), file.bytes().size());

  std::printf("setup: instructions %llu, natives %llu, distinct_pcs %llu\n", (unsigned long long)setup_insns,
              (unsigned long long)setup_natives, (unsigned long long)setup_pcs);
  std::printf("frames %ld (of %ld requested), instructions %llu, natives %llu, insns/frame p50 %llu p95 %llu (n=%ld)\n", done,
              frames, (unsigned long long)vm.instructions(), (unsigned long long)stats.natives,
              (unsigned long long)pct(insns_per_frame, 0.5), (unsigned long long)pct(insns_per_frame, 0.95), done);
  std::printf("distinct_pcs_by_frame (first 5, last):");
  for (long i = 0; i < std::min<long>(5, done); ++i) std::printf(" %llu", (unsigned long long)pcs_per_frame[i]);
  if (done > 5) std::printf(" ... %llu", (unsigned long long)pcs_per_frame.back());
  std::printf("\n");
  // the oracle's format: "over F frames" counts every frame of the run, skipped ones included
  std::printf("process passes: total %llu over %ld frames, max %llu, frames with re-runs %llu", (unsigned long long)hs.passes_total,
              done, (unsigned long long)hs.max_passes, (unsigned long long)hs.frames_rerun);
  if (!hs.first_reruns.empty()) {
    std::printf(" (first:");
    for (const auto& s : hs.first_reruns) std::printf(" %s", s.c_str());
    std::printf(")");
  }
  std::printf(", frames skipped %llu", (unsigned long long)hs.frames_skipped);
  if (!hs.first_skips.empty()) {
    std::printf(" (first:");
    for (const auto& s : hs.first_skips) std::printf(" %s", s.c_str());
    std::printf(")");
  }
  std::printf("\n");
  std::printf("stop: %s\n", stop.c_str());
  std::printf("script sha256 unchanged: %s\n", sha_after == sha_before ? "True" : "False");
  std::printf("flattening: the last frame that executed a new PC is %ld; distinct PCs overall %zu; stack high-water %u; "
              "natives/frame p50 %llu p95 %llu; script ms/frame p50 %.4f p95 %.4f (n=%ld of %ld frames); setup ms %.3f\n",
              flatten, seen.size(), vm.stack_high_water(), (unsigned long long)pct(natives_per_frame, 0.5),
              (unsigned long long)pct(natives_per_frame, 0.95), pctd(ms_per_frame, 0.5), pctd(ms_per_frame, 0.95), done,
              frames, setup_ms);
  if (!perf_path.empty()) {  // A15: one row per run, no thresholds
    std::FILE* pf = std::fopen(perf_path.c_str(), "ab");
    if (pf) {
      std::fprintf(pf,
                   "{\"scene\": \"osi-a6\", \"commit\": \"%s\", \"frames\": %ld, \"expected_frames\": %ld, \"insns_per_frame_p50\": %llu, "
                   "\"insns_per_frame_p95\": %llu, \"insns_samples\": %zu, \"natives_per_frame_p50\": %llu, \"natives_per_frame_p95\": %llu, "
                   "\"script_ms_per_frame_p50\": %.4f, \"script_ms_per_frame_p95\": %.4f, \"stack_high_water\": %u, "
                   "\"setup_instructions\": %llu, \"instructions\": %llu, \"flattening_frame\": %ld, \"stop\": \"%s\", "
                   "\"loadavg_start\": %.2f, \"loadavg_end\": %.2f}\n",
                   commit.c_str(), done, frames, (unsigned long long)pct(insns_per_frame, 0.5),
                   (unsigned long long)pct(insns_per_frame, 0.95), insns_per_frame.size(),
                   (unsigned long long)pct(natives_per_frame, 0.5), (unsigned long long)pct(natives_per_frame, 0.95),
                   pctd(ms_per_frame, 0.5), pctd(ms_per_frame, 0.95), vm.stack_high_water(), (unsigned long long)setup_insns,
                   (unsigned long long)vm.instructions(), flatten, stop == "none" ? "none" : "error", loadavg_start,
                   loadavg1());
      std::fclose(pf);
    }
  }
  return stop == "none" && sha_after == sha_before ? 0 : 1;
}
