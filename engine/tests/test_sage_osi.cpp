// M1a-3: the bounded OSI reader (lomn/sage/osi.h) on the synthetic Base.osi program (tests/osi_builder.h; the A9 scene
// carries the same bytes) and hostile variants of it: every truncation, bit flips, a bad version, an unknown opcode,
// every per-file / per-method cap (REF3 revision 2: values measured on the real Base.osi, see osi.h).
// The time/memory side of the caps is probed by probe_osi.cpp (ctest osi_probe, native and wasm32).
#include <cstdint>
#include <string>
#include <vector>

#include "doctest.h"
#include "lomn/sage/osi.h"
#include "osi_builder.h"

using namespace lomn;

namespace {
std::vector<uint8_t> synthetic_osi() { return test::statt_osi(); }
std::vector<uint8_t> with_code(const std::vector<uint8_t>& code) {
  test::OsiSpec s;
  s.code = code;
  return test::build_osi(s);
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
  REQUIRE_MESSAGE(o.eval_calls("lego_area_statt", "InitFull", calls, &err), err);
  REQUIRE(calls.size() == 2);
  CHECK(calls[0].fn == "setfog");
  REQUIRE(calls[0].args.size() == 4);
  double v = 0;
  // PushConstanti32 (0x40) is a SIGNED 32-bit operand (Task 8A C1): 0xff808080 is -8,355,712, as osi.py decodes it.
  REQUIRE(calls[0].args[0].kind == sage::OsiValue::Int);
  CHECK(calls[0].args[0].i == int64_t(int32_t(0xff808080u)));
  REQUIRE(calls[0].args[1].number(v));
  CHECK(v == -5.0);  // 0 - 5 folded
  REQUIRE(calls[0].args[2].number(v));
  CHECK(v == -50.0);
  CHECK(calls[0].args[3].kind == sage::OsiValue::Float);
  CHECK(calls[1].fn == "setcavelighting");
  REQUIRE(calls[1].args[0].number(v));
  CHECK(v == 1.0);
  CHECK_FALSE(o.eval_calls("lego_area_none", "initfull", calls, &err));
  CHECK_FALSE(o.eval_calls("lego_area_statt", "process", calls, &err));
}

TEST_CASE("osi: signed 32-bit pushes and the integer/float folds (Task 8A C1)") {
  // push i32 -5, push i8 3, Add -> int -2; push i32 INT_MIN, push i32 -1, Divide -> float 2147483648 (Divide always
  // folds as a float); push i32 -1 -> int -1; push i32 INT_MIN, push i32 -1, Multiply -> int 2147483648 (folds are in
  // 64 bits: no 32-bit wrap, as osi.py's Python ints); push u32 0xffffffff (0x48) -> int 4294967295 (unsigned).
  std::vector<uint8_t> c;
  auto i32 = [&](int32_t v) {
    c.push_back(0x40);
    test::osi_put32(c, static_cast<uint32_t>(v));
  };
  i32(-5);
  c.insert(c.end(), {0x43, 3, 0x70});
  i32(INT32_MIN);
  i32(-1);
  c.push_back(0x73);
  i32(-1);
  i32(INT32_MIN);
  i32(-1);
  c.push_back(0x72);
  c.push_back(0x48);
  test::osi_put32(c, 0xffffffffu);
  c.insert(c.end(), {0x82, 0, 0, 1, 0, 5, 0x30, 0x52});
  sage::OsiFile o;
  std::string err;
  REQUIRE_MESSAGE(o.parse(with_code(c), &err), err);
  std::vector<sage::OsiCall> calls;
  REQUIRE_MESSAGE(o.eval_calls("lego_area_statt", "initfull", calls, &err), err);
  REQUIRE(calls.size() == 1);
  REQUIRE(calls[0].args.size() == 5);
  const auto& a = calls[0].args;
  CHECK(a[0].kind == sage::OsiValue::Int);
  CHECK(a[0].i == -2);
  CHECK(a[1].kind == sage::OsiValue::Float);
  CHECK(a[1].f == 2147483648.0f);
  CHECK(a[2].kind == sage::OsiValue::Int);
  CHECK(a[2].i == -1);
  CHECK(a[3].kind == sage::OsiValue::Int);
  CHECK(a[3].i == 2147483648ll);
  CHECK(a[4].kind == sage::OsiValue::Int);
  CHECK(a[4].i == 4294967295ll);
}

TEST_CASE("osi: every truncation and 2000 bit flips fail cleanly or parse; bad version and opcodes are rejected") {
  const std::vector<uint8_t> good = synthetic_osi();
  for (size_t len = 0; len < good.size(); ++len) {
    sage::OsiFile o;
    std::string err;
    const bool ok = o.parse(std::vector<uint8_t>(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(len)), &err);
    if (ok) {  // a truncated bytecode tail can still parse its tables: the walk must then fail
      std::vector<sage::OsiCall> calls;
      CHECK_FALSE(o.eval_calls("lego_area_statt", "initfull", calls, &err));
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
      o.eval_calls("lego_area_statt", "initfull", calls, &err);
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
  CHECK_FALSE(o.eval_calls("lego_area_statt", "initfull", calls, &err));
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
  CHECK_FALSE(o.eval_calls("lego_area_statt", "initfull", calls, &err));
  CHECK(err.find("kMaxOsiInstructions") != std::string::npos);
}

TEST_CASE("osi: per-method caps (calls, argc, args, string bytes, literal stack) are errors with no partial result") {
  sage::OsiFile o;
  std::string err;
  std::vector<sage::OsiCall> calls;
  auto run = [&](const std::vector<uint8_t>& code) {
    REQUIRE_MESSAGE(o.parse(with_code(code), &err), err);
    calls.assign(3, sage::OsiCall());  // stale content must be cleared on success and on failure
    return o.eval_calls("lego_area_statt", "initfull", calls, &err);
  };
  auto rep = [](std::vector<uint8_t> unit, uint32_t n) {
    std::vector<uint8_t> c;
    for (uint32_t i = 0; i < n; ++i) c.insert(c.end(), unit.begin(), unit.end());
    c.push_back(0x52);
    return c;
  };
  const std::vector<uint8_t> call0 = {0x82, 0, 0, 1, 0, 0, 0x30};  // call, Pop its result (as compiled scripts do)
  CHECK(run(rep(call0, sage::kMaxOsiCallsPerMethod)));  // exactly at the cap: accepted
  CHECK(calls.size() == sage::kMaxOsiCallsPerMethod);
  CHECK_FALSE(run(rep(call0, sage::kMaxOsiCallsPerMethod + 1)));
  CHECK(err.find("kMaxOsiCallsPerMethod") != std::string::npos);
  CHECK(calls.empty());
  std::vector<uint8_t> argc127 = {0x82, 0, 0, 1, 0, 127, 0x30};
  CHECK(run(rep(argc127, sage::kMaxOsiArgsPerMethod / 127)));                  // 32 x 127 = 4064 args
  CHECK_FALSE(run(rep(argc127, sage::kMaxOsiArgsPerMethod / 127 + 1)));        // 4191 > 4096
  CHECK(err.find("kMaxOsiArgsPerMethod") != std::string::npos);
  CHECK(calls.empty());
  CHECK_FALSE(run(rep({0x82, 0, 0, 1, 0, 0x80}, 1)));  // argc -128 (s8): rejected, never clamped
  CHECK(err.find("argc") != std::string::npos);
  // String bytes copied into arguments: 255-char string pushed and passed one by one.
  test::OsiSpec s;
  s.strings.push_back(std::string(255, 'x'));
  std::vector<uint8_t> unit = {0x46, 4, 0, 0x82, 0, 0, 1, 0, 1, 0x30};
  s.code = rep(unit, sage::kMaxOsiStringArgBytes / 255);
  REQUIRE(o.parse(test::build_osi(s), &err));
  CHECK(o.eval_calls("lego_area_statt", "initfull", calls, &err));
  s.code = rep(unit, sage::kMaxOsiStringArgBytes / 255 + 1);
  REQUIRE(o.parse(test::build_osi(s), &err));
  CHECK_FALSE(o.eval_calls("lego_area_statt", "initfull", calls, &err));
  CHECK(err.find("kMaxOsiStringArgBytes") != std::string::npos);
  CHECK(calls.empty());
  // Literal stack depth.
  CHECK(run(rep({0x45}, sage::kMaxOsiStackDepth)));
  CHECK_FALSE(run(rep({0x45}, sage::kMaxOsiStackDepth + 1)));
  CHECK(err.find("kMaxOsiStackDepth") != std::string::npos);
}

TEST_CASE("osi: check_all walks shared bytecode once and caps total instructions and calls per file") {
  sage::OsiFile o;
  std::string err;
  uint64_t n = 0, nc = 0;
  test::OsiSpec s;
  s.code = test::osi_statt_program();
  s.functions = 1000;  // all share the method's bytecode: walked once
  REQUIRE(o.parse(test::build_osi(s), &err));
  REQUIRE_MESSAGE(o.check_all(&n, &err, &nc), err);
  CHECK(n == 15);
  CHECK(nc == 2);
  // Functions starting inside one long Nop run at distinct offsets: each walk is legal, the total is capped.
  s.code.assign(sage::kMaxOsiInstructions - 1, 0x00);
  s.code.push_back(0x52);
  s.functions = 200;
  s.function_stride = 1;
  REQUIRE(o.parse(test::build_osi(s), &err));
  CHECK_FALSE(o.check_all(&n, &err));
  CHECK(err.find("kMaxOsiTotalInstructions") != std::string::npos);
  // Calls per file: methods may each stay under kMaxOsiCallsPerMethod while the file exceeds kMaxOsiTotalCalls.
  s.code.clear();
  for (uint32_t i = 0; i < 5000; ++i) s.code.insert(s.code.end(), {0x82, 0, 0, 1, 0, 0});
  s.code.push_back(0x52);
  s.functions = 30;
  s.function_stride = 6;  // 30 walks of ~5000 calls each = 150,000 > 131,072
  REQUIRE(o.parse(test::build_osi(s), &err));
  CHECK_FALSE(o.check_all(&n, &err));
  CHECK(err.find("kMaxOsiTotalCalls") != std::string::npos);
}

TEST_CASE("osi: a failed parse leaves no partial tables; oversized files are rejected before reading") {
  std::vector<uint8_t> b = synthetic_osi();
  sage::OsiFile o;
  std::string err;
  REQUIRE(o.parse(b, &err));
  REQUIRE(o.classes().size() == 1);
  // Cut inside the source table's count: the string, class and symbol tables were already read when parse fails.
  std::vector<uint8_t> t(b.begin(), b.end() - static_cast<std::ptrdiff_t>(test::osi_statt_program().size()) - 1);
  CHECK_FALSE(o.parse(t, &err));
  CHECK(err.find("symbol/source") != std::string::npos);
  CHECK(o.classes().empty());
  CHECK(o.strings().empty());
  CHECK(o.major() == 0);
  std::vector<uint8_t> big(static_cast<size_t>(sage::kMaxOsiBytes) + 1, 0);
  CHECK_FALSE(o.parse(std::move(big), &err));
  CHECK(err.find("too large") != std::string::npos);
}
