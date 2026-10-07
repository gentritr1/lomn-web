// REF3 revision 2: crafted-input TIME and MEMORY probes for the OSI reader (lomn/sage/osi.h), one per loop whose count
// an attacker controls (M1a-1 audit rule). Each probe builds a hostile Base.osi in memory, runs parse + check_all +
// eval_calls, and must finish with the expected cap error in <= kMaxSeconds and grow the process peak memory by
// <= kMaxGrowthMB. Runs natively (peak RSS) and as wasm32 under Node (linear-memory size, 2 GB cap; it only grows when
// the allocator runs out of the initial 16 MiB, so +0.0 there means every probe fit in it): ctest osi_probe.
// Before the caps, "calls" (Fable's 600 KB call flood) took 486 MB natively.
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "lomn/sage/osi.h"
#include "osi_builder.h"
#include "probe_util.h"

static double peak_mb() { return probe_peak_mb(); }

using namespace lomn;

namespace {
constexpr double kMaxSeconds = 5.0, kMaxGrowthMB = 64.0;

std::vector<uint8_t> rep(std::vector<uint8_t> unit, uint32_t n, bool ret = true) {
  std::vector<uint8_t> c;
  c.reserve(unit.size() * n + 1);
  for (uint32_t i = 0; i < n; ++i) c.insert(c.end(), unit.begin(), unit.end());
  if (ret) c.push_back(0x52);
  return c;
}

bool probe(const char* name, std::vector<uint8_t> file, const char* want) {  // want == nullptr: must be accepted
  const double m0 = peak_mb();
  const auto t0 = std::chrono::steady_clock::now();
  const size_t bytes = file.size();
  std::string err, got;
  {
    sage::OsiFile o;
    if (!o.parse(std::move(file), &err)) {
      got = err;
    } else {
      uint64_t n = 0;
      if (!o.check_all(&n, &err)) got = err;
      std::vector<sage::OsiCall> calls;
      if (!o.eval_calls("lego_area_statt", "initfull", calls, &err) && got.empty()) got = err;
    }
  }
  const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const double grow = peak_mb() - m0;
  const bool want_ok = want == nullptr ? got.empty() : got.find(want) != std::string::npos;
  const bool ok = want_ok && s <= kMaxSeconds && grow <= kMaxGrowthMB;
  std::printf("%s %-14s %8zu bytes  %.3f s  peak +%.1f MB (%.1f MB)  -> %s\n", ok ? "ok  " : "FAIL", name, bytes, s, grow,
              peak_mb(), got.empty() ? "(accepted)" : got.c_str());
  return ok;
}
}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);  // every line survives a timeout kill
  bool ok = true;
  test::OsiSpec s;
  s.strings.push_back(std::string(255, 'x'));  // string 4: the longest a u8 length allows
  // 1. Fable's call flood: 100,000 x CallGameFunction argc 127 (600 KB): one subroutine past the instruction cap.
  s.code = rep({0x82, 0, 0, 1, 0, 127}, 100000);
  ok &= probe("calls", test::build_osi(s), "kMaxOsiInstructions");
  // 1b. The same flood inside the instruction cap: the per-method argument cap stops it.
  s.code = rep({0x82, 0, 0, 1, 0, 127}, 32000);
  ok &= probe("calls-in-cap", test::build_osi(s), "kMaxOsiArgsPerMethod");
  // 1c. argc-0 calls with their results popped: the per-method call cap.
  s.code = rep({0x82, 0, 0, 1, 0, 0, 0x30}, 16000);
  ok &= probe("calls-argc0", test::build_osi(s), "kMaxOsiCallsPerMethod");
  // 2. String-push flood (each push copied a 255-byte string onto the literal stack).
  s.code = rep({0x46, 4, 0}, 32000);
  ok &= probe("stack", test::build_osi(s), "kMaxOsiStackDepth");
  // 2b. 255-byte strings passed one per call: the string-bytes cap.
  s.code = rep({0x46, 4, 0, 0x82, 0, 0, 1, 0, 1, 0x30}, 3000);
  ok &= probe("string-args", test::build_osi(s), "kMaxOsiStringArgBytes");
  // 3. 65,535 functions sharing one 32,767-Nop run (walked once), then 32,000 functions starting at distinct offsets of
  // it (each walk legal, 32,000 x ~16,000 instructions in total): check_all's total-instruction cap.
  s.code = rep({0x00}, sage::kMaxOsiInstructions - 1);
  s.functions = 65535;
  s.function_stride = 0;
  ok &= probe("funcs-shared", test::build_osi(s), nullptr);  // shared bytecode: accepted (no cap hit), walked once
  s.functions = 32000;
  s.function_stride = 1;
  ok &= probe("funcs-distinct", test::build_osi(s), "kMaxOsiTotalInstructions");
  s.functions = 0;
  // 4. A subroutine as long as the file cap allows.
  s.code = rep({0x00}, static_cast<uint32_t>(sage::kMaxOsiBytes) - 1024);
  ok &= probe("nops", test::build_osi(s), "kMaxOsiInstructions");
  // 5. One byte over the file cap.
  ok &= probe("too-large", std::vector<uint8_t>(static_cast<size_t>(sage::kMaxOsiBytes) + 1, 0), "too large");
  // 6. (Task 8B C2) Methods sharing one 255-character symbol (each method used to copy the symbol name). Just inside
  // kMaxOsiMethods (1 + 1,028 x 255 = 262,141 methods): accepted, and bounded because a method keeps a symbol index,
  // not a copy of the name. (Run first: the native figure is the process PEAK, which only grows.)
  s.code = test::osi_statt_program();
  s.extra_symbol = std::string(255, 'm');
  s.extra_methods = 255;
  s.extra_classes = 1028;
  ok &= probe("methods-in-cap", test::build_osi(s), nullptr);
  // 6b. The method table as large as the file cap allows: 5,450 classes x 255 methods = 1.39M methods (365 MB natively
  // before the cap): rejected by kMaxOsiMethods before any method is stored.
  s.extra_classes = 5450;
  ok &= probe("methods-flood", test::build_osi(s), "kMaxOsiMethods");
  s.extra_classes = 0;
  std::printf("%s\n", ok ? "osi_probe: all probes bounded" : "osi_probe: FAILED");
  return ok ? 0 : 1;
}
