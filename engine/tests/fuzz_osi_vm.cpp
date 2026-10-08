// M2a-1 T5: deterministic mutation-fuzz SEED harness for the OSI script VM (lomn::osi::Vm), built with ASan + UBSan
// (-fno-sanitize-recover=all) by `make fuzz-osi-vm` (on demand, not part of check; the full campaign is M2a-4). Seeds:
// every assembled conformance program (*.osi under SEED_DIR, written by tools/osi/conform_assemble.py) and, optionally, the
// real Base.osi (user data, never committed). Each seed runs once unmutated, then mutated inputs follow; an input that
// parses is run through the engine calls main, main2, Setup, Process(t, dt, 0) and Input(cnt1, VK_DOWN, 2, 0.0, 0.0) with
// every native a typed stub from data/osi (D11). A step cap bounds each call: the trace sink counts `I` records and throws
// the VM's own error type once past the cap, so the VM unwinds exactly as on an instruction error.
// Invariants after every call (and reset_after_error on an error): stack depth 0, context depth 0; after every input: the
// script image is unchanged (D4). The sanitizers catch the rest (an out-of-bounds slot, a dangling handle dereference, UB).
// Usage: fuzz_osi_vm ITERATIONS SEED DATA_DIR SEED_DIR [BASE_OSI] [--steps N]
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "lomn/osi/heap.h"
#include "lomn/osi/registry.h"
#include "lomn/osi/trace.h"
#include "lomn/osi/vm.h"
#include "lomn/sage/osi.h"

using namespace lomn;
using namespace lomn::osi;

namespace {
uint64_t rng = 1;
uint32_t rnd(uint32_t n) {
  rng = rng * 6364136223846793005ull + 1442695040888963407ull;
  return n ? static_cast<uint32_t>((rng >> 33) % n) : 0;
}
void die(const char* what, uint64_t it, const std::string& seed) {
  std::printf("INVARIANT FAILED at iteration %llu (seed %s): %s\n", static_cast<unsigned long long>(it), seed.c_str(), what);
  std::exit(1);
}
bool slurp(const std::filesystem::path& p, std::string& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

// The step cap, and the opcodes that actually executed (from the `I` records: "I %08x %02x ...").
struct StepCap : TraceSink {
  uint64_t cap = 100000, steps = 0, total = 0;
  bool tripped = false;
  std::set<int> ops;
  void line(std::string_view s) override {
    if (s.size() < 13 || s[0] != 'I') return;
    ++total;
    ops.insert(static_cast<int>(std::strtol(std::string(s.substr(11, 2)).c_str(), nullptr, 16)));
    if (!tripped && ++steps > cap) {
      tripped = true;
      throw VmError("Undefined: fuzz step cap");
    }
  }
};

struct Seed {
  std::string name;
  std::vector<uint8_t> bytes;
  uint32_t code_lo = 0;  // the lowest function/method offset: most mutations land in the bytecode
  bool real = false;
};
}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  std::vector<std::string> pos;
  uint64_t steps = 100000;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--steps" && i + 1 < argc) steps = std::strtoull(argv[++i], nullptr, 10);
    else pos.push_back(a);
  }
  if (pos.size() < 4) {
    std::fprintf(stderr, "usage: fuzz_osi_vm ITERATIONS SEED DATA_DIR SEED_DIR [BASE_OSI] [--steps N]\n");
    return 2;
  }
  const uint64_t iters = std::strtoull(pos[0].c_str(), nullptr, 10);
  rng = std::strtoull(pos[1].c_str(), nullptr, 10);
  const std::filesystem::path data = pos[2];
  std::string tj, nj, mj;
  if (!slurp(data / "types.json", tj) || !slurp(data / "natives.json", nj) || !slurp(data / "natives_beta150.json", mj)) {
    std::fprintf(stderr, "fuzz_osi_vm: cannot read the tables in %s\n", data.string().c_str());
    return 2;
  }
  const TypeTable types = TypeTable::load(tj);
  NativeRegistry reg = NativeRegistry::load(nj, mj, types);

  std::vector<Seed> seeds;
  std::vector<std::filesystem::path> files;
  std::error_code ec;
  for (std::filesystem::recursive_directory_iterator it(pos[3], ec), end; !ec && it != end; it.increment(ec))
    if (it->path().extension() == ".osi") files.push_back(it->path());
  std::sort(files.begin(), files.end());
  if (pos.size() > 4) files.push_back(pos[4]);
  for (size_t k = 0; k < files.size(); ++k) {
    std::string b;
    if (!slurp(files[k], b)) {
      std::fprintf(stderr, "fuzz_osi_vm: cannot read %s\n", files[k].string().c_str());
      return 2;
    }
    Seed s;
    s.name = files[k].filename().string();
    s.bytes.assign(b.begin(), b.end());
    s.real = pos.size() > 4 && k + 1 == files.size();
    sage::OsiFile o;
    std::string err;
    if (o.parse(std::vector<uint8_t>(s.bytes), &err)) {
      uint32_t lo = static_cast<uint32_t>(s.bytes.size());
      for (const auto& f : o.functions()) lo = std::min(lo, f.offset);
      for (const auto& c : o.classes())
        for (const auto& m : c.methods) lo = std::min(lo, m.offset);
      s.code_lo = lo < s.bytes.size() ? lo : 0;
    }
    seeds.push_back(std::move(s));
  }
  if (seeds.empty()) {
    std::fprintf(stderr, "fuzz_osi_vm: no seeds under %s\n", pos[3].c_str());
    return 2;
  }

  // the named opcodes (opcodes.md) and a few boundary bytes
  const uint8_t interesting[] = {0x00, 0x01, 0x7f, 0x80, 0xff, 0x10, 0x11, 0x12, 0x13, 0x14, 0x18, 0x19, 0x1a, 0x21, 0x22,
                                 0x23, 0x24, 0x25, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x40, 0x42, 0x43, 0x44, 0x45, 0x46,
                                 0x47, 0x48, 0x50, 0x51, 0x52, 0x53, 0x54, 0x60, 0x70, 0x73, 0x74, 0x75, 0x7a, 0x82, 0x83,
                                 0x84, 0x90, 0x91, 0x93, 0x94, 0x95, 0x96, 0xa0, 0xb0, 0xb1, 0xb2, 0xbf};
  // the 96 named handlers (opcodes.md / the dispatch table): coverage is reported over these, other bytes separately
  const std::set<int> named = {0x00, 0x02, 0x03, 0x04, 0x05, 0x06, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19,
                               0x1a, 0x21, 0x22, 0x23, 0x24, 0x25, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x40, 0x41, 0x42, 0x43,
                               0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x50, 0x51, 0x52, 0x53, 0x54, 0x60, 0x61, 0x62, 0x63, 0x64,
                               0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x7a,
                               0x7b, 0x80, 0x81, 0x82, 0x83, 0x84, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0xa0, 0xa1, 0xa2,
                               0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xb0, 0xb1, 0xb2, 0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbf, 0xff};
  const std::vector<std::pair<std::string, std::vector<Variant>>> entries = {
      {"main", {}},
      {"main2", {}},
      {"Setup", {}},
      {"Process", {make_float(0.0166f), make_float(0.0166f), make_int(0)}},
      {"Input", {make_int(0x636e7431), make_int(0x28), make_int(2), make_float_bits(0), make_float_bits(0)}},
  };
  uint64_t accepted = 0, calls = 0, ok = 0, errors = 0, capped = 0, not_found = 0, internal = 0;
  std::set<std::string> messages;
  StepCap cap;
  cap.cap = steps;
  for (uint64_t it = 0; it < iters; ++it) {
    size_t si;
    if (it < seeds.size()) {
      si = static_cast<size_t>(it);  // every seed once, unmutated
    } else {
      const bool pick_real = seeds.back().real && rnd(8) == 0;
      si = pick_real ? seeds.size() - 1 : rnd(static_cast<uint32_t>(seeds.size() - (seeds.back().real ? 1 : 0)));
    }
    const Seed& seed = seeds[si];
    std::vector<uint8_t> b = seed.bytes;
    if (it >= seeds.size()) {
      const uint32_t nmut = 1 + rnd(seed.real ? 4 : 8);
      for (uint32_t m = 0; m < nmut && !b.empty(); ++m) {
        const uint32_t lo = seed.code_lo < b.size() && rnd(4) != 0 ? seed.code_lo : 0;
        const uint32_t at = lo + rnd(static_cast<uint32_t>(b.size() - lo));
        switch (rnd(8)) {
          case 0: b[at] ^= static_cast<uint8_t>(1u << rnd(8)); break;
          case 1: b[at] = interesting[rnd(sizeof interesting)]; break;
          case 2: b[at] = 0xff; if (at + 1 < b.size()) b[at + 1] = 0xff; break;  // u16 operand to 0xffff
          case 3: b.insert(b.begin() + at, static_cast<uint8_t>(rnd(256))); break;
          case 4: b.erase(b.begin() + at, b.begin() + std::min<size_t>(b.size(), at + 1 + rnd(16))); break;
          case 5: { const uint32_t len = std::min<uint32_t>(1 + rnd(32), static_cast<uint32_t>(b.size() - at));
                    std::vector<uint8_t> chunk(b.begin() + at, b.begin() + at + len);
                    for (uint32_t k = rnd(8); k > 0; --k) b.insert(b.begin() + at, chunk.begin(), chunk.end()); break; }
          case 6: b[at] = static_cast<uint8_t>(rnd(256)); break;
          case 7: if (at + 1 < b.size()) b[at + 1] = static_cast<uint8_t>(rnd(256)); break;  // an operand byte
        }
      }
    }
    sage::OsiFile file;
    std::string err;
    const std::vector<uint8_t> image = b;
    if (!file.parse(std::move(b), &err)) continue;
    ++accepted;
    Heap heap(nullptr);
    Vm vm(file, reg, heap, &cap);
    for (const auto& [name, args] : entries) {
      cap.steps = 0;
      cap.tripped = false;
      ++calls;
      const CallResult r = vm.call(name, args);
      if (!r.found) ++not_found;
      if (r.ok) {
        ++ok;
      } else {
        const VmError* e = vm.last_error();
        if (!e) die("a failed call without last_error", it, seed.name);
        if (cap.tripped) ++capped;
        else ++errors;
        if (messages.size() < 4096) messages.insert(e->message.substr(0, 60));
        vm.reset_after_error();
      }
      const VmState st = vm.snapshot();
      if (st.depth != 0) die("stack depth != 0 after a call (and reset)", it, seed.name);
      if (st.context_depth != 0) die("context depth != 0 after a call (and reset)", it, seed.name);
    }
    internal += vm.internal_errors().size();
    if (file.bytes() != image) die("the script image changed (D4)", it, seed.name);
  }
  std::printf("fuzz_osi_vm: %llu inputs (%zu seeds%s), %llu accepted, %llu calls: %llu ok, %llu errors, %llu step-capped "
              "(cap %llu), %llu entry not found; %llu instructions, %zu/%zu named opcodes fetched (+%zu unused bytes), "
              "%zu distinct error messages, %llu internal_errors; 0 invariant failures\n",
              static_cast<unsigned long long>(iters), seeds.size(), seeds.back().real ? " incl. Base.osi" : "",
              static_cast<unsigned long long>(accepted), static_cast<unsigned long long>(calls),
              static_cast<unsigned long long>(ok), static_cast<unsigned long long>(errors), static_cast<unsigned long long>(capped),
              static_cast<unsigned long long>(steps), static_cast<unsigned long long>(not_found),
              static_cast<unsigned long long>(cap.total),
              static_cast<size_t>(std::count_if(named.begin(), named.end(), [&](int o) { return cap.ops.count(o) != 0; })), named.size(),
              static_cast<size_t>(std::count_if(cap.ops.begin(), cap.ops.end(), [&](int o) { return named.count(o) == 0; })),
              messages.size(), static_cast<unsigned long long>(internal));
  return 0;
}
