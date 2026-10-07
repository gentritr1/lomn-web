// M1a-3: the bounded OSI reader (lomn/sage/osi.h) on the synthetic Base.osi (lomn/view/synthetic.cpp) and hostile
// variants of it: every truncation, bit flips, a bad version, an unknown opcode, an instruction-cap overflow.
#include <string>
#include <vector>

#include "doctest.h"
#include "lomn/fs.h"
#include "lomn/sage/osi.h"
#include "lomn/view/synthetic.h"

using namespace lomn;

namespace {
std::vector<uint8_t> synthetic_osi() {
  fs::MemoryFileSystem mfs;
  REQUIRE(view::mount_synthetic_install(mfs));
  std::vector<uint8_t> b;
  REQUIRE(mfs.read("Root/Data/Base.osi", b) == fs::Error::None);
  return b;
}
}  // namespace

TEST_CASE("osi: tables and the straight-line literal finder") {
  sage::OsiFile o;
  std::string err;
  REQUIRE_MESSAGE(o.parse(synthetic_osi(), &err), err);
  CHECK(o.major() == 4);
  CHECK(o.minor() == 1);
  REQUIRE(o.classes().size() == 1);
  CHECK(o.find_class("LEGO_AREA_STATT") != nullptr);  // case-insensitive
  uint64_t n = 0;
  REQUIRE_MESSAGE(o.check_all(&n, &err), err);
  CHECK(n == 15);  // push i32, 0, f32, sub, 0, f32, sub, f32, call, pop, i8, call, pop, nothing, return
  std::vector<sage::OsiCall> calls;
  REQUIRE_MESSAGE(o.find_calls("lego_area_statt", "InitFull", calls, &err), err);
  REQUIRE(calls.size() == 2);
  CHECK(calls[0].fn == "setfog");
  REQUIRE(calls[0].args.size() == 4);
  double v = 0;
  REQUIRE(calls[0].args[0].number(v));
  CHECK(static_cast<uint32_t>(static_cast<int64_t>(v)) == 0xff808080u);
  REQUIRE(calls[0].args[1].number(v));
  CHECK(v == -5.0);  // 0 - 5 folded
  REQUIRE(calls[0].args[2].number(v));
  CHECK(v == -50.0);
  CHECK(calls[0].args[3].kind == sage::OsiValue::Float);
  CHECK(calls[1].fn == "setcavelighting");
  REQUIRE(calls[1].args[0].number(v));
  CHECK(v == 1.0);
  CHECK_FALSE(o.find_calls("lego_area_none", "initfull", calls, &err));
  CHECK_FALSE(o.find_calls("lego_area_statt", "process", calls, &err));
}

TEST_CASE("osi: every truncation and 2000 bit flips fail cleanly or parse; bad version and opcodes are rejected") {
  const std::vector<uint8_t> good = synthetic_osi();
  for (size_t len = 0; len < good.size(); ++len) {
    sage::OsiFile o;
    std::string err;
    const bool ok = o.parse(std::vector<uint8_t>(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(len)), &err);
    if (ok) {  // a truncated bytecode tail can still parse its tables: the walk must then fail
      std::vector<sage::OsiCall> calls;
      CHECK_FALSE(o.find_calls("lego_area_statt", "initfull", calls, &err));
    }
  }
  uint32_t seed = 12345, parsed = 0;
  for (int i = 0; i < 2000; ++i) {
    std::vector<uint8_t> b = good;
    seed = seed * 1664525u + 1013904223u;
    b[(seed >> 8) % b.size()] ^= static_cast<uint8_t>(1u << ((seed >> 4) & 7));
    sage::OsiFile o;
    std::string err;
    if (o.parse(b, &err)) {
      ++parsed;
      uint64_t n = 0;
      o.check_all(&n, &err);
      std::vector<sage::OsiCall> calls;
      o.find_calls("lego_area_statt", "initfull", calls, &err);
    }
  }
  CHECK(parsed > 0);
  std::vector<uint8_t> v = good;
  v[4] = 6;  // version 6.1: another table layout
  sage::OsiFile o;
  std::string err;
  CHECK_FALSE(o.parse(v, &err));
  CHECK(err.find("unsupported version") != std::string::npos);
  std::vector<uint8_t> bad = good;
  bad[bad.size() - 1] = 0x01;  // the final Return becomes an undefined opcode
  REQUIRE(o.parse(bad, &err));
  std::vector<sage::OsiCall> calls;
  CHECK_FALSE(o.find_calls("lego_area_statt", "initfull", calls, &err));
  CHECK(err.find("unknown opcode") != std::string::npos);
}

TEST_CASE("osi: a subroutine longer than kMaxOsiInstructions is rejected") {
  std::vector<uint8_t> b = synthetic_osi();
  b.pop_back();  // drop the Return: a run of Nops follows, then the Return
  b.insert(b.end(), sage::kMaxOsiInstructions, 0x00);
  b.push_back(0x52);
  sage::OsiFile o;
  std::string err;
  REQUIRE(o.parse(b, &err));
  std::vector<sage::OsiCall> calls;
  CHECK_FALSE(o.find_calls("lego_area_statt", "initfull", calls, &err));
  CHECK(err.find("kMaxOsiInstructions") != std::string::npos);
}
