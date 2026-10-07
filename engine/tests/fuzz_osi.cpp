// M1a-3 REF3 revision 2: deterministic mutation fuzzer for the OSI reader (lomn/sage/osi.h), built with ASan+UBSan
// (-fno-sanitize-recover=all) natively and as wasm32 (emcc, Node) by `make fuzz-osi` (not part of check). Seeds: the
// synthetic InitFull program, a small cap-edge program, and optionally a real Base.osi (argv[3]; user data, never
// committed). Every accepted input is checked against the caps; a failed parse / eval must leave no partial state.
// Usage: fuzz_osi ITERATIONS SEED [REAL_BASE_OSI]
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "lomn/sage/osi.h"
#include "osi_builder.h"

using namespace lomn;
static uint64_t rng = 1;
static uint32_t rnd(uint32_t n) {
  rng = rng * 6364136223846793005ull + 1442695040888963407ull;
  return n ? static_cast<uint32_t>((rng >> 33) % n) : 0;
}
static void die(const char* what, uint64_t it) {
  std::printf("INVARIANT FAILED at iteration %llu: %s\n", static_cast<unsigned long long>(it), what);
  std::exit(1);
}

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  const uint64_t iters = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 10000;
  rng = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1;
  std::vector<std::vector<uint8_t>> seeds = {test::statt_osi()};
  {
    test::OsiSpec s;
    s.strings.push_back(std::string(255, 'x'));
    s.code = {0x46, 4, 0, 0x46, 4, 0, 0x82, 0, 0, 1, 0, 2, 0x30, 0x53, 2, 0, 0x45, 0x45, 0x31, 2, 0x52};
    s.functions = 3;
    s.function_stride = 3;
    seeds.push_back(test::build_osi(s));
  }
  if (argc > 3) {
    std::ifstream f(argv[3], std::ios::binary);
    seeds.emplace_back(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  }
  const uint8_t interesting[] = {0, 1, 0x7f, 0x80, 0xff, 0x82, 0x46, 0x53, 0x54, 0x52, 0x31, 0x30, 0x45, 0x70, 0x73};
  uint64_t accepted = 0, evaluated = 0, calls_seen = 0;
  for (uint64_t it = 0; it < iters; ++it) {
    const size_t si = rnd(static_cast<uint32_t>(seeds.size()));
    std::vector<uint8_t> b = seeds[si];
    const bool real = si >= 2;
    const uint32_t nmut = 1 + rnd(real ? 4 : 8);
    for (uint32_t m = 0; m < nmut && !b.empty(); ++m) {
      const uint32_t at = rnd(static_cast<uint32_t>(b.size()));
      switch (rnd(7)) {
        case 0: b[at] ^= static_cast<uint8_t>(1u << rnd(8)); break;
        case 1: b[at] = interesting[rnd(sizeof interesting)]; break;
        case 2: b[at] = 0xff; if (at + 1 < b.size()) b[at + 1] = 0xff; break;   // u16 count/index to 0xffff
        case 3: b.insert(b.begin() + at, static_cast<uint8_t>(rnd(256))); break;
        case 4: b.erase(b.begin() + at, b.begin() + std::min<size_t>(b.size(), at + 1 + rnd(16))); break;
        case 5: { const uint32_t len = std::min<uint32_t>(1 + rnd(64), static_cast<uint32_t>(b.size() - at));
                  std::vector<uint8_t> chunk(b.begin() + at, b.begin() + at + len);
                  for (uint32_t k = rnd(64); k > 0; --k) b.insert(b.begin() + at, chunk.begin(), chunk.end()); break; }
        case 6: b.resize(at); break;
      }
    }
    sage::OsiFile o;
    std::string err;
    if (!o.parse(std::move(b), &err)) {
      if (!o.classes().empty() || !o.strings().empty()) die("failed parse left tables", it);
      continue;
    }
    ++accepted;
    uint64_t n = 0, nc = 0;
    if (o.check_all(&n, &err, &nc) && (n > sage::kMaxOsiTotalInstructions || nc > sage::kMaxOsiTotalCalls)) die("check_all over cap", it);
    uint32_t budget = real ? 16 : 1u << 30;  // the real seed has 8,406 methods: sample a few per input
    for (const sage::OsiClass& k : o.classes())
      for (const sage::OsiMethod& m : k.methods) {
        if (real && rnd(512) != 0) continue;
        if (budget-- == 0) break;
        std::vector<sage::OsiCall> calls(2);
        ++evaluated;
        if (!o.eval_calls(k.name, o.method_name(m), calls, &err)) {
          if (!calls.empty()) die("failed eval left calls", it);
          continue;
        }
        if (calls.size() > sage::kMaxOsiCallsPerMethod) die("calls over cap", it);
        uint64_t args = 0, sb = 0;
        for (const sage::OsiCall& c : calls) {
          if (c.args.size() > sage::kMaxOsiArgc) die("argc over cap", it);
          args += c.args.size();
          for (const sage::OsiValue& v : c.args) sb += v.s.size();
        }
        if (args > sage::kMaxOsiArgsPerMethod || sb > sage::kMaxOsiStringArgBytes) die("args/string bytes over cap", it);
        calls_seen += calls.size();
      }
  }
  std::printf("fuzz_osi: %llu inputs, %llu accepted, %llu method evaluations, %llu calls, 0 invariant failures\n",
              static_cast<unsigned long long>(iters), static_cast<unsigned long long>(accepted),
              static_cast<unsigned long long>(evaluated), static_cast<unsigned long long>(calls_seen));
  return 0;
}
