// M2a-2 T3: ScriptHost. The frame loop is M2a-1 T4's (lomn_osi_run.cpp), moved here without a change in behaviour; the
// guard is the native A6 leg's `D` lines (Task 3 brief). Rules taken from the asm (M2a-1 T4 controller addendum,
// ScProcessManager::Process 0x4a9170, ScSLOSIProcess::Process 0x43b250):
//  - fake time: PM+8 (a double, 0 at start, INFERRED) += the double at 0x712d68 by `fld qword; fadd qword; fstp qword`
//    (0x4a92c8-0x4a92e3): the sum is rounded to the precision-control state (24 or 64 bits, D8'), then stored as a double;
//    the script receives it as f32. dt is the same constant as f32. The counter is PM+0x44 (0 at start, INFERRED),
//    incremented at frame end (0x4a95b6).
//  - Process re-runs while its result is int 0, at most 10 extra passes, with the same arguments (0x43b33f-0x43b378,
//    0x4a930d / 0x4a955b / 0x4a957d-0x4a9584). After 11 int-0 passes the process skips the next frame (PM+0x40 parity
//    0x4a92fd-0x4a930a, eligibility byte 0x4a937d, set at 0x4a93b7-0x4a93c1 / 0x4a94a6-0x4a94bb).
//  - natives.log v2 (D14): `K %6d` at the start of every frame, before anything else of it; Setup has no `K`.
#include "lomn/osi/host.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <stdexcept>

#include "lomn/osi/json.h"
#include "lomn/osi/services.h"

namespace lomn::osi {

// ---- fake time: fadd qword at the precision-control state, then fstp qword (round to 53) ----
// Exact: both operands as integer * 2^e, summed in 128 bits (a tiny partner collapses to a same-signed epsilon far below
// half an ulp of the larger at 64 bits, which keeps the rounding direction and never creates a tie).
namespace {
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
}  // namespace

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

namespace {
// GetTime(1) 0x4fa460: timeGetTime() (a DWORD of milliseconds, 0x4fa475) * [0x728038] = 0.001 (0x4fa48d)
double steady_seconds() {
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  return static_cast<double>(static_cast<uint32_t>(ms)) * 0.001;
}
constexpr int32_t kController1 = 0x636e7431;  // 'cnt1'
// the double at 0x712d68 (read from the exe as data: 0x3f7b4e81c0000000)
double time_step() {
  const uint64_t b = 0x3f7b4e81c0000000ull;
  double d;
  std::memcpy(&d, &b, 8);
  return d;
}
}  // namespace

namespace {
// keys.py's frame grammar (`f.lstrip("-").isdigit()`, one optional '-'), parsed without undefined behaviour (review M-6:
// the KEYSPEC reaches the browser): checked strtoll, then the int range less one at the top, because the release frame
// is f + 1.
bool parse_frame(const std::string& t, int& out) {
  size_t i = t.size() > 0 && t[0] == '-' ? 1 : 0;
  if (i == t.size()) return false;
  for (size_t k = i; k < t.size(); ++k)
    if (t[k] < '0' || t[k] > '9') return false;
  errno = 0;
  char* end = nullptr;
  const long long v = std::strtoll(t.c_str(), &end, 10);
  if (errno == ERANGE || !end || *end != 0) return false;
  if (v < INT32_MIN || v > static_cast<long long>(INT32_MAX) - 1) return false;
  out = static_cast<int>(v);
  return true;
}
}  // namespace

bool expand_keys(const std::string& spec, std::vector<KeyCall>& out, std::string* err) {
  std::istringstream ss(spec);
  std::string tok;
  while (ss >> tok) {
    const size_t at = tok.find('@');
    if (at == std::string::npos) return *err = "bad key token " + tok, false;
    const std::string name = tok.substr(0, at);
    int f = 0;
    if (!parse_frame(tok.substr(at + 1), f)) return *err = "bad key token " + tok, false;
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

namespace {
// natives.md §7.2: every override names a winner key, and the merged registry row carries the same tier.
void check_overrides(std::string_view json, const NativeRegistry& reg) {
  if (json.empty()) return;
  Json j;
  std::string err;
  if (!parse_json(json, j, &err)) throw std::runtime_error("native_overrides: " + err);
  if (j.type != Json::Array) throw std::runtime_error("native_overrides: not a list");
  for (const Json& r : j.arr) {
    const Json* k = r.get("key");
    const Json* t = r.get("tier");
    if (!k || k->type != Json::String || !t || t->type != Json::String)
      throw std::runtime_error("native_overrides: a row without key/tier");
    const size_t dot = k->str.find('.');
    const NativeEntry* e = dot == std::string::npos ? nullptr : reg.find(k->str.substr(0, dot), k->str.substr(dot + 1));
    if (!e) throw std::runtime_error("native_overrides: unknown key " + k->str);
    if (t->str != tier_name(e->tier))
      throw std::runtime_error("native_overrides: " + k->str + " is " + t->str + " in the overrides but " +
                               tier_name(e->tier) + " in the merged registry (run make osi-tables)");
  }
}
}  // namespace

ScriptHost::ScriptHost(const sage::OsiFile& script, std::string_view natives_json, std::string_view mods_json,
                       std::string_view types_json, std::string_view overrides_json, const fs::FileSystem* vfs,
                       TraceSink* trace, ReturnLog* log, Options opt)
    : script_(script), trace_(trace), log_(log), opt_(std::move(opt)) {
  std::string err;
  types_ = TypeTable::load(types_json, &err);
  if (!err.empty()) throw std::runtime_error("types: " + err);
  reg_ = NativeRegistry::load(natives_json, mods_json, types_);
  check_overrides(overrides_json, reg_);
  if (!expand_keys(opt_.keys, keys_, &err)) throw std::runtime_error(err);
  reg_.set_return_log(log_);
  heap_ = std::make_unique<Heap>(trace_);
  vm_ = std::make_unique<Vm>(script_, reg_, *heap_, trace_);
  ctx_ = std::make_unique<HostContext>(*vm_, *heap_, vfs, opt_.flags, log_);
  if (opt_.real_natives) {  // fails closed (review I-1): never a silent stub run
    const BindReport rep = bind_natives(reg_, *ctx_);
    if (rep.game_bodies == 0)  // R-1 (M2a-2 T7a): the selftest group's body does not satisfy the check
      throw std::runtime_error("real natives: no native group binds a body (" + std::to_string(native_group_count()) +
                               " game-native groups linked, " + std::to_string(rep.groups) + " groups run)");
    std::string missing;
    for (size_t i = 0; i < reg_.size(); ++i)
      if (reg_.entry(i).tier == Tier::Implemented && !reg_.entry(i).fn) missing += " " + reg_.entry(i).key;
    if (!missing.empty()) throw std::runtime_error("real natives: tier1 without a bound body:" + missing);
  }
  vm_->set_native_observer([this](const NativeEntry& e, uint64_t seq) {
    ++stats_.natives;
    auto it = stats_.hits.find(e.key);
    if (it == stats_.hits.end()) {
      NativeHit h;
      h.first_frame = setup_done_ ? current_frame_ : -1;
      h.first_seq = seq;
      h.tier = e.tier;
      h.bound = e.fn != nullptr;
      it = stats_.hits.emplace(e.key, h).first;
    }
    ++it->second.calls;
  });
}

ScriptHost::~ScriptHost() = default;

const HostStats& ScriptHost::stats() const {
  stats_.instructions = vm_->instructions();
  stats_.max_nesting = vm_->max_nesting();
  stats_.stack_high_water = vm_->stack_high_water();
  return stats_;
}

CallResult ScriptHost::setup() {
  CallResult r = vm_->call("Setup", {});
  setup_done_ = true;
  stats_.setup_instructions = vm_->instructions();
  if (!r.ok) stats_.stop = "error \"" + fmt_message(vm_->last_error()->message) + "\" in Setup";
  return r;
}

bool ScriptHost::frame() {
  if (stats_.stop != "none") return false;
  const long k = frame_;
  current_frame_ = k;
  static const double C = time_step();
  double dt_d = C;
  if (!opt_.flags.deterministic) {
    // Outside --deterministic (D3) ScProcessManager::Process runs GetTime(1) and the blend and clamp on EVERY pass
    // (0x4a91fe-0x4a929f); the byte [0x712d78] that UseRealDeltaTime sets is read once, at 0x4a92a5, and only selects which dt
    // and process time are used (0x4a92ae-0x4a92e3): the smoothed dt and `now` (fstp [pm+8]), or the fake step.
    RealDeltaTime& rt = ctx_->subsystem<RealDeltaTime>();
    const double now = opt_.clock ? opt_.clock() : steady_seconds();
    const double smoothed = rt.step(now);
    if (rt.effective) {
      dt_d = smoothed;
      time_ = now;
    } else {
      time_ = fadd_pc(time_, C, vm_->fpu_pc());  // the original's fake-time path: time += 0.0066667 (0x4a92c8-0x4a92e3)
    }
  } else {
    time_ = fadd_pc(time_, C, vm_->fpu_pc());  // the original's fake-time path: time += 0.0066667 (0x4a92c8-0x4a92e3)
  }
  const float dt = static_cast<float>(dt_d);
  const float t = static_cast<float>(time_);
  char kb[64];
  std::snprintf(kb, sizeof kb, "K %6ld %08x %08x %08x", k, f32_bits(t), f32_bits(dt), counter_);
  if (trace_) trace_->line(kb);
  if (log_) {
    std::snprintf(kb, sizeof kb, "K %6ld", k);
    log_->line(kb);
  }
  for (; kc_ < keys_.size() && keys_[kc_].frame < k; ++kc_) {}
  for (; kc_ < keys_.size() && keys_[kc_].frame == k && stats_.stop == "none"; ++kc_) {
    CallResult kr = vm_->call("Input", keys_[kc_].args);
    if (!kr.ok) stats_.stop = "error \"" + fmt_message(vm_->last_error()->message) + "\" at frame " + std::to_string(k);
  }
  if (stats_.stop == "none") {
    if (k == skip_frame_) {
      ++stats_.frames_skipped;
      if (stats_.first_skips.size() < 5) stats_.first_skips.push_back(std::to_string(k));
    } else {
      uint64_t passes = 0;
      bool capped = true;
      for (int p = 0; p < 11; ++p) {
        ++passes;
        CallResult pr = vm_->call("Process", {make_float(t), make_float(dt), make_int(static_cast<int32_t>(counter_))});
        if (!pr.ok) {
          stats_.stop = "error \"" + fmt_message(vm_->last_error()->message) + "\" at frame " + std::to_string(k);
          capped = false;
          break;
        }
        if (!(pr.result.type == kInt && pr.result.value == 0)) {
          capped = false;
          break;
        }
      }
      stats_.passes_total += passes;
      stats_.max_passes = std::max(stats_.max_passes, passes);
      if (passes > 1) {
        ++stats_.frames_rerun;
        if (stats_.first_reruns.size() < 5) stats_.first_reruns.push_back(std::to_string(k) + ":" + std::to_string(passes));
      }
      if (capped) skip_frame_ = k + 1;
    }
  }
  ++counter_;  // 0x4a95b6
  ++frame_;
  stats_.frames = frame_;
  return stats_.stop == "none";
}

}  // namespace lomn::osi
