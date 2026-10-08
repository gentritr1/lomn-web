// M2a-1 T4: lomn_osi_run, the A6 differential run of the real Base.osi (consult §4.1, Task 6 brief + controller addendum).
//
//   lomn_osi_run BASE.osi [--data-dir data/osi] [--frames 600] [--keys "down@100 down@160 enter@220 esc@280"]
//                [--trace FILE [--trace-frames a:b]] [--digest FILE] [--natives-log FILE] [--perf-out FILE --commit C]
//                [--print-keys]
//
// The run: Setup(); then for frame k = 0..frames-1: the fake time advances, the `K` record, that frame's key Input calls
// (KEYSPEC, tools/osi/keys.py has the same expansion), then Process(time, dt, counter) with the re-run rule. All natives are
// the registry's typed stubs (D11) plus the Beta 1.5.0 mod rows (D12). The run stops at the first VM error.
//
// Rules taken from the asm (controller addendum, ScProcessManager::Process 0x4a9170, ScSLOSIProcess::Process 0x43b250):
//  - fake time: PM+8 (a double, 0 at start, INFERRED) += the double at 0x712d68 by `fld qword; fadd qword; fstp qword`
//    (0x4a92c8-0x4a92e3): the sum is rounded to the precision-control state (24 or 64 bits, D8'), then stored as a double;
//    the script receives it as f32. dt is the same constant as f32. The counter is PM+0x44 (0 at start, INFERRED),
//    incremented at frame end (0x4a95b6).
//  - Process re-runs while its result is int 0, at most 10 extra passes, with the same arguments (0x43b33f-0x43b378,
//    0x4a930d / 0x4a955b / 0x4a957d-0x4a9584). After 11 int-0 passes the process skips the next frame (PM+0x40 parity
//    0x4a92fd-0x4a930a, eligibility byte 0x4a937d, set at 0x4a93b7-0x4a93c1 / 0x4a94a6-0x4a94bb).
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

// ---- fake time: fadd qword at the precision-control state, then fstp qword (round to 53) ----
// Exact: both operands as integer * 2^e, summed in 128 bits (a tiny partner collapses to a same-signed epsilon far below
// half an ulp of the larger at 64 bits, which keeps the rounding direction and never creates a tie).
struct Dbl {
  bool neg;
  uint64_t m;  // integer significand
  int e;       // value = m * 2^e
};
Dbl split(double d) {
  uint64_t b;
  std::memcpy(&b, &d, 8);
  Dbl r;
  r.neg = (b >> 63) != 0;
  const int ex = static_cast<int>((b >> 52) & 0x7ff);
  const uint64_t fr = b & ((uint64_t(1) << 52) - 1);
  if (ex == 0) {
    r.m = fr;
    r.e = -1074;
  } else {
    r.m = fr | (uint64_t(1) << 52);
    r.e = ex - 1075;
  }
  return r;
}
int bitlen(unsigned __int128 v) {
  int n = 0;
  while (v) v >>= 1, ++n;
  return n;
}
// round v * 2^e (+ a sticky fraction) to q significant bits, nearest-even; returns the new significand, adjusts e
unsigned __int128 round_bits(unsigned __int128 v, int& e, bool sticky, int q) {
  const int L = bitlen(v);
  if (L <= q) return v;
  const int s = L - q;
  unsigned __int128 m = v >> s;
  const bool r = (v >> (s - 1)) & 1;
  const bool rest = sticky || (v & ((static_cast<unsigned __int128>(1) << (s - 1)) - 1)) != 0;
  if (r && (rest || (m & 1))) ++m;
  e += s;
  return m;
}
double fadd_pc(double a, double b, int pc) {
  if (!std::isfinite(a) || !std::isfinite(b)) return a + b;
  Dbl x = split(a), y = split(b);
  if (x.m == 0 && y.m == 0) return a + b;  // signed zeros as IEEE
  // put the larger magnitude first (a zero partner contributes nothing; the sum is still rounded to pc below)
  if (x.m == 0 || (y.m != 0 && y.e + bitlen(y.m) > x.e + bitlen(x.m))) std::swap(x, y);
  if (y.m == 0) y.e = x.e;
  const int top = x.e + bitlen(x.m);
  if (y.m != 0 && top - (y.e + bitlen(y.m)) > 70) {  // y below 2^(top-70): collapse it to an epsilon of the same sign
    y.m = 1;
    y.e = top - 128;
  }
  const int e0 = std::min(x.e, y.e);
  if (x.e - e0 > 70 || y.e - e0 > 70) {  // cannot happen after the collapse with normal operands; keep it exact anyway
    return a + b;
  }
  const unsigned __int128 X = static_cast<unsigned __int128>(x.m) << (x.e - e0);
  const unsigned __int128 Y = static_cast<unsigned __int128>(y.m) << (y.e - e0);
  bool neg;
  unsigned __int128 N;
  if (x.neg == y.neg) {
    N = X + Y;
    neg = x.neg;
  } else if (X >= Y) {
    N = X - Y;
    neg = x.neg;
  } else {
    N = Y - X;
    neg = y.neg;
  }
  if (N == 0) return 0.0;
  int e = e0;
  unsigned __int128 M = round_bits(N, e, false, pc == 64 ? 64 : 24);  // the fadd at the precision-control state
  M = round_bits(M, e, false, 53);                                    // fstp qword
  const double r = std::ldexp(static_cast<double>(static_cast<uint64_t>(M)), e);
  return neg ? -r : r;
}

// ---- KEYSPEC (consult §4.1): a tap is two frames: state 2 on frame f, state 1 on frame f+1 ----
struct KeyCall {
  int frame;
  std::vector<Variant> args;
};
constexpr int32_t kController1 = 0x636e7431;  // 'cnt1'
bool expand_keys(const std::string& spec, std::vector<KeyCall>& out, std::string* err) {
  std::istringstream ss(spec);
  std::string tok;
  while (ss >> tok) {
    const size_t at = tok.find('@');
    if (at == std::string::npos) return *err = "bad key token " + tok, false;
    const std::string name = tok.substr(0, at);
    const int f = std::atoi(tok.c_str() + at + 1);
    for (int st : {2, 1}) {
      KeyCall k;
      k.frame = st == 2 ? f : f + 1;
      if (name == "down" || name == "enter") {  // analog-trigger site S3: 5 args (C1, VK, state, 0.0, 0.0)
        k.args = {make_int(kController1), make_int(name == "down" ? 0x28 : 0x0d), make_int(st), make_float_bits(0),
                  make_float_bits(0)};
      } else if (name == "esc") {  // key-mapper site S4: 3 args (C1, 251, state)
        k.args = {make_int(kController1), make_int(251), make_int(st)};
      } else {
        return *err = "unknown key " + name, false;
      }
      out.push_back(k);
    }
  }
  std::stable_sort(out.begin(), out.end(), [](const KeyCall& a, const KeyCall& b) { return a.frame < b.frame; });
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
  bool print_keys = false;
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
  TypeTable types = TypeTable::load(tj);
  NativeRegistry reg;
  try {
    reg = NativeRegistry::load(nj, mj, types);
  } catch (const std::exception& e) {
    return std::fprintf(stderr, "lomn_osi_run: %s\n", e.what()), 2;
  }
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
    reg.set_return_log(log_file.get());
  }
  tee.line(header);
  Heap heap(&tee);
  Vm vm(file, reg, heap, &tee);

  std::string stop = "none";
  auto t0 = std::chrono::steady_clock::now();
  CallResult r = vm.call("Setup", {});
  const double setup_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  const uint64_t setup_insns = vm.instructions(), setup_natives = stats.natives, setup_pcs = stats.pcs.size();
  if (!r.ok) stop = "error \"" + fmt_message(vm.last_error()->message) + "\" in Setup";

  const double C = [] {  // the double at 0x712d68 (read from the exe as data: 0x3f7b4e81c0000000)
    const uint64_t b = 0x3f7b4e81c0000000ull;
    double d;
    std::memcpy(&d, &b, 8);
    return d;
  }();
  const float dt = static_cast<float>(C);
  double time = 0.0;  // PM+8
  uint32_t counter = 0;  // PM+0x44
  std::vector<uint64_t> insns_per_frame, natives_per_frame, pcs_per_frame;
  std::vector<double> ms_per_frame;
  std::set<uint32_t> seen = stats.pcs;
  long flatten = -1;
  uint64_t passes_total = 0, max_passes = 0, frames_rerun = 0, frames_skipped = 0;
  std::vector<std::string> first_reruns, first_skips;
  long skip_frame = -2;
  size_t kc = 0;
  for (long k = 0; k < frames && stop == "none"; ++k) {
    stats.pcs.clear();
    const uint64_t i0 = vm.instructions(), n0 = stats.natives;
    auto f0 = std::chrono::steady_clock::now();
    time = fadd_pc(time, C, vm.fpu_pc());
    const float t = static_cast<float>(time);
    char kb[64];
    std::snprintf(kb, sizeof kb, "K %6ld %08x %08x %08x", k, f32_bits(t), f32_bits(dt), counter);
    tee.line(kb);
    for (; kc < key_calls.size() && key_calls[kc].frame < k; ++kc) {}
    for (; kc < key_calls.size() && key_calls[kc].frame == k && stop == "none"; ++kc) {
      CallResult kr = vm.call("Input", key_calls[kc].args);
      if (!kr.ok) stop = "error \"" + fmt_message(vm.last_error()->message) + "\" at frame " + std::to_string(k);
    }
    if (stop == "none") {
      if (k == skip_frame) {
        ++frames_skipped;
        if (first_skips.size() < 5) first_skips.push_back(std::to_string(k));
      } else {
        uint64_t passes = 0;
        bool capped = true;
        for (int p = 0; p < 11; ++p) {
          ++passes;
          CallResult pr = vm.call("Process", {make_float(t), make_float(dt), make_int(static_cast<int32_t>(counter))});
          if (!pr.ok) {
            stop = "error \"" + fmt_message(vm.last_error()->message) + "\" at frame " + std::to_string(k);
            capped = false;
            break;
          }
          if (!(pr.result.type == kInt && pr.result.value == 0)) {
            capped = false;
            break;
          }
        }
        passes_total += passes;
        max_passes = std::max(max_passes, passes);
        if (passes > 1) {
          ++frames_rerun;
          if (first_reruns.size() < 5) first_reruns.push_back(std::to_string(k) + ":" + std::to_string(passes));
        }
        if (capped) skip_frame = k + 1;
      }
    }
    ++counter;  // 0x4a95b6
    ms_per_frame.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - f0).count());
    insns_per_frame.push_back(vm.instructions() - i0);
    natives_per_frame.push_back(stats.natives - n0);
    pcs_per_frame.push_back(stats.pcs.size());
    for (uint32_t pc : stats.pcs)
      if (seen.insert(pc).second) flatten = k;
  }
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
  std::printf("process passes: total %llu over %ld frames, max %llu, frames with re-runs %llu", (unsigned long long)passes_total,
              done, (unsigned long long)max_passes, (unsigned long long)frames_rerun);
  if (!first_reruns.empty()) {
    std::printf(" (first:");
    for (const auto& s : first_reruns) std::printf(" %s", s.c_str());
    std::printf(")");
  }
  std::printf(", frames skipped %llu", (unsigned long long)frames_skipped);
  if (!first_skips.empty()) {
    std::printf(" (first:");
    for (const auto& s : first_skips) std::printf(" %s", s.c_str());
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
                   "\"setup_instructions\": %llu, \"instructions\": %llu, \"flattening_frame\": %ld, \"stop\": \"%s\"}\n",
                   commit.c_str(), done, frames, (unsigned long long)pct(insns_per_frame, 0.5),
                   (unsigned long long)pct(insns_per_frame, 0.95), insns_per_frame.size(),
                   (unsigned long long)pct(natives_per_frame, 0.5), (unsigned long long)pct(natives_per_frame, 0.95),
                   pctd(ms_per_frame, 0.5), pctd(ms_per_frame, 0.95), vm.stack_high_water(), (unsigned long long)setup_insns,
                   (unsigned long long)vm.instructions(), flatten, stop == "none" ? "none" : "error");
      std::fclose(pf);
    }
  }
  return stop == "none" && sha_after == sha_before ? 0 : 1;
}
