// M2a-1 T1: unit tests of lomn::osi (the script VM) - primitives first, then every handler family with hand-assembled
// bytes (a small in-test assembler over osi_builder.h's byte writers). Each case names the opcodes.md row it checks.
// NOT the conformance corpus (tests/osi/conformance, run by lomn_osi_conform). Numbers pinned here come from
// tools/osi/expect_calc.py (the T0 calculator; scratch pin_arith.py, see the commit body), `shasum -a 256`, the data
// tables (a python3 count over data/osi/*.json) or integer arithmetic stated next to them.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "doctest.h"
#include "lomn/osi/heap.h"
#include "lomn/osi/json.h"
#include "lomn/osi/registry.h"
#include "lomn/osi/stack.h"
#include "lomn/osi/trace.h"
#include "lomn/osi/variant.h"
#include "lomn/osi/vm.h"
#include "osi_builder.h"

using namespace lomn;
using namespace lomn::osi;

namespace {

uint32_t B(float f) { return f32_bits(f); }

// ---- a tiny OSI 4.1 assembler (layout as lomn/sage/osi.h reads it; byte writers from osi_builder.h) ----
struct Asm {
  struct Fn {
    std::string name;
    uint16_t argc;
    std::string label;
  };
  struct Method {
    std::string sym, label;
  };
  struct Cls {
    std::string name;
    std::vector<std::string> props;
    std::vector<Method> methods;
  };
  std::vector<std::string> strings, globals, symbols;
  std::vector<Fn> fns;
  std::vector<Cls> classes;
  std::vector<uint8_t> code;
  std::map<std::string, uint32_t> labels;
  struct Fix {
    size_t at;
    std::string label;
    bool rel16;
  };
  std::vector<Fix> fixes;
  uint32_t base = 0;

  static uint16_t index_of(std::vector<std::string>& v, const std::string& s) {
    for (size_t i = 0; i < v.size(); ++i)
      if (v[i] == s) return static_cast<uint16_t>(i);
    v.push_back(s);
    return static_cast<uint16_t>(v.size() - 1);
  }
  uint16_t str(const std::string& s) { return index_of(strings, s); }
  uint16_t sym(const std::string& s) { return index_of(symbols, s); }
  uint16_t glob(const std::string& s) { return static_cast<uint16_t>(0x8000 | index_of(globals, s)); }
  Asm& fn(const std::string& name, uint16_t argc) {
    fns.push_back({name, argc, "fn:" + name});
    return label("fn:" + name);
  }
  Asm& cls(const std::string& name, std::vector<std::string> props) {
    for (auto& p : props) sym(p);
    classes.push_back({name, props, {}});
    return *this;
  }
  Asm& method(const std::string& s) {  // the method's code follows
    sym(s);
    classes.back().methods.push_back({s, "m:" + classes.back().name + "." + s});
    return label("m:" + classes.back().name + "." + s);
  }
  Asm& label(const std::string& n) {
    labels[n] = static_cast<uint32_t>(code.size());
    return *this;
  }
  Asm& op(uint8_t o) {
    code.push_back(o);
    return *this;
  }
  Asm& u8(uint8_t v) { return op(v); }
  Asm& u16(uint32_t v) {
    test::osi_put16(code, v);
    return *this;
  }
  Asm& u32(uint32_t v) {
    test::osi_put32(code, v);
    return *this;
  }
  Asm& i32(int32_t v) { return op(0x40).u32(static_cast<uint32_t>(v)); }
  Asm& i8(int8_t v) { return op(0x43).u8(static_cast<uint8_t>(v)); }
  Asm& f(float v) { return op(0x44).u32(B(v)); }
  Asm& fbits(uint32_t b) { return op(0x44).u32(b); }
  Asm& s(const std::string& t) { return op(0x46).u16(str(t)); }
  Asm& nothing() { return op(0x47); }
  Asm& ret() { return op(0x52); }
  Asm& pop() { return op(0x30); }
  Asm& call(const std::string& c, const std::string& f, uint8_t n) {
    uint16_t ci = str(c), fi = str(f);
    return op(0x82).u16(ci).u16(fi).u8(n);
  }
  Asm& rel(uint8_t o, const std::string& l) {  // 0x53 / 0x54
    op(o);
    fixes.push_back({code.size(), l, true});
    return u16(0);
  }
  Asm& addr(const std::string& l) {  // 0x40 with a label's file offset (0x50's target)
    op(0x40);
    fixes.push_back({code.size(), l, false});
    return u32(0);
  }
  uint32_t at(const std::string& l) const { return base + labels.at(l); }

  std::vector<uint8_t> build() {
    std::vector<uint8_t> b = {'O', 'S', 'I', 0};
    test::osi_put16(b, 4);
    test::osi_put16(b, 1);
    auto table = [&](const std::vector<std::string>& v) {
      test::osi_put16(b, static_cast<uint32_t>(v.size()));
      for (const auto& x : v) {
        b.push_back(static_cast<uint8_t>(x.size()));
        b.insert(b.end(), x.begin(), x.end());
        b.push_back(0);
      }
    };
    table(strings);
    table(globals);
    test::osi_put16(b, static_cast<uint32_t>(fns.size()));
    std::vector<std::pair<size_t, std::string>> patch;
    for (const auto& f : fns) {
      b.push_back(static_cast<uint8_t>(f.name.size()));
      b.insert(b.end(), f.name.begin(), f.name.end());
      patch.push_back({b.size(), f.label});
      test::osi_put32(b, 0);
      test::osi_put16(b, f.argc);
    }
    test::osi_put16(b, static_cast<uint32_t>(classes.size()));
    for (const auto& c : classes) {
      b.push_back(static_cast<uint8_t>(c.props.size()));
      for (const auto& p : c.props) test::osi_put16(b, sym(p));
      b.push_back(static_cast<uint8_t>(c.methods.size()));
      for (const auto& m : c.methods) {
        test::osi_put16(b, sym(m.sym));
        patch.push_back({b.size(), m.label});
        test::osi_put32(b, 0);
      }
    }
    for (const auto& c : classes) {
      b.push_back(static_cast<uint8_t>(c.name.size()));
      b.insert(b.end(), c.name.begin(), c.name.end());
      b.push_back(0);
    }
    table(symbols);
    table({});
    base = static_cast<uint32_t>(b.size());
    std::vector<uint8_t> c = code;
    for (const auto& fx : fixes) {
      uint32_t t = base + labels.at(fx.label);
      if (fx.rel16) {
        uint32_t after = base + static_cast<uint32_t>(fx.at) + 2;
        uint16_t d = static_cast<uint16_t>(t - after);
        c[fx.at] = static_cast<uint8_t>(d);
        c[fx.at + 1] = static_cast<uint8_t>(d >> 8);
      } else {
        for (int i = 0; i < 4; ++i) c[fx.at + i] = static_cast<uint8_t>(t >> (8 * i));
      }
    }
    for (const auto& p : patch) {
      uint32_t t = base + labels.at(p.second);
      for (int i = 0; i < 4; ++i) b[p.first + i] = static_cast<uint8_t>(t >> (8 * i));
    }
    b.insert(b.end(), c.begin(), c.end());
    return b;
  }
};

// ---- test natives (declared like the corpus's declarative natives) ----
Variant nat_echo_arg0(NativeCall& c, void*) { return c.vm.heap().duplicate(*c.args.at(0)); }
Variant nat_seven(NativeCall&, void*) { return make_int(7); }
Variant nat_text_abc(NativeCall& c, void*) { return Variant{kString, c.vm.heap().new_string("abc")}; }
Variant nat_byref_write5(NativeCall& c, void*) {
  *c.args.at(0) = make_int(5);
  return nothing();
}
Variant nat_reenter(NativeCall& c, void*) {
  c.vm.call("inner", {});
  return make_int(1);
}

NativeEntry entry(const std::string& cls, const std::string& name, uint8_t mn, uint8_t mx, std::vector<uint16_t> types,
                  NativeFn fn) {
  NativeEntry e;
  e.cls = cls;
  e.name = name;
  e.key = NativeRegistry::fold(cls + "." + name);
  e.min = mn;
  e.max = mx;
  e.types.fill(kNothing);
  for (size_t i = 0; i < types.size(); ++i) e.types[i] = types[i];
  e.fn = fn;
  e.tier = Tier::Implemented;
  return e;
}

struct T {
  sage::OsiFile file;
  NativeRegistry reg;
  StringTrace tr;
  Heap heap{&tr};
  std::unique_ptr<Vm> vm;
  void load(Asm& a) {
    std::string err;
    REQUIRE_MESSAGE(file.parse(a.build(), &err), err);
    vm = std::make_unique<Vm>(file, reg, heap, &tr);
  }
  CallResult run(Asm& a, const std::string& entry_name = "main", std::vector<Variant> args = {}) {
    load(a);
    return vm->call(entry_name, args);
  }
  std::string err() const {
    std::string e = vm->last_error() ? vm->last_error()->message : std::string("<no error>");
    if (std::getenv("OSI_DUMP")) e += "\n" + tr.text();
    return e;
  }
  bool has(const std::string& l) const {
    for (const auto& x : tr.lines)
      if (x == l) return true;
    return false;
  }
  std::vector<std::string> starting(const std::string& p) const {
    std::vector<std::string> out;
    for (const auto& x : tr.lines)
      if (x.rfind(p, 0) == 0) out.push_back(x);
    return out;
  }
};

Variant I(int32_t v) { return make_int(v); }
Variant F(float v) { return make_float(v); }

}  // namespace

// ============================== primitives ==============================

TEST_CASE("osi vm primitives: x87_arith D8' (0x70-0x73 Floats convention, falsifiers 0x70#1-#4, 0x71-0x73, 0x72#2)") {
  auto fi = [](int32_t i) { return Num::of_int(i); };
  auto ff = [](uint32_t b) { return Num::of_float_bits(b); };
  // the D8' lines: 1.0e-15 = f32 0x26901d7d, 1.0e-10 = 0x2edbe6ff
  CHECK(x87_arith(Op::Add, fi(16777217), ff(0x26901d7d), 24) == 0x4b800001u);
  CHECK(x87_arith(Op::Add, fi(16777217), ff(0x26901d7d), 64) == 0x4b800000u);
  CHECK(x87_arith(Op::Add, fi(16777217), ff(0x2edbe6ff), 64) == 0x4b800001u);
  CHECK(x87_arith(Op::Add, fi(16777217), ff(0x2edbe6ff), 24) == 0x4b800001u);
  CHECK(x87_arith(Op::Mul, ff(0x24368a23), ff(0x1af99066), 24) == 0x002c7cd4u);
  CHECK(x87_arith(Op::Mul, ff(0x24368a23), ff(0x1af99066), 64) == 0x002c7cd5u);
  // the M2a-0 falsifiers (same at 24 and 64)
  for (int pc : {24, 64}) {
    CHECK(x87_arith(Op::Add, fi(16777217), ff(B(1.0f)), pc) == 0x4b800001u);
    CHECK(x87_arith(Op::Sub, fi(16777217), ff(B(1.0f)), pc) == 0x4b800000u);
    CHECK(x87_arith(Op::Mul, fi(16777217), ff(B(3.0f)), pc) == 0x4c400001u);
    CHECK(x87_arith(Op::Div, fi(50331651), ff(B(3.0f)), pc) == 0x4b800000u);
    CHECK(x87_arith(Op::Div, ff(B(1.0e30f)), ff(B(1.0e-30f)), pc) == 0x7f800000u);
    CHECK(x87_arith(Op::Sub, ff(B(1.0f)), ff(B(1.0f)), pc) == 0x00000000u);
    CHECK(x87_arith(Op::Div, ff(0x00000001), ff(B(2.0f)), pc) == 0x00000000u);  // tie to even at the subnormal end
    CHECK(x87_arith(Op::Div, ff(0x00000003), ff(B(4.0f)), pc) == 0x00000001u);
  }
  // six int +- float double-rounding cases where the 53-bit formula differs (round-to-odd cases, consult M23 kind)
  struct R {
    Op op;
    int32_t a;
    uint32_t b, want, f53;
  };
  const R rs[] = {{Op::Sub, -240135720, 0x30cad190, 0xcd6502e3, 0xcd6502e2}, {Op::Add, -107217628, 0x307624b3, 0xcccc805b, 0xcccc805c},
                  {Op::Sub, 522063344, 0x31f2dc0b, 0x4df8f06f, 0x4df8f070},  {Op::Sub, 20110827, 0x305c1cda, 0x4b996ef5, 0x4b996ef6},
                  {Op::Sub, -160926040, 0xb0597e4d, 0xcd197895, 0xcd197896}, {Op::Add, 355023760, 0x32ccf93c, 0x4da949dd, 0x4da949dc}};
  for (const R& r : rs) {
    CHECK(x87_arith(r.op, fi(r.a), ff(r.b), 24) == r.want);
    CHECK(x87_arith(r.op, fi(r.a), ff(r.b), 64) == r.want);
    CHECK(r.want != r.f53);
  }
  // signed zeros (IEEE / x87): (-1.0) * 0 -> -0.0; -0.0 + -0.0 -> -0.0; -0.0 - +0 -> -0.0; 1 - 1 -> +0
  CHECK(x87_arith(Op::Mul, ff(B(-1.0f)), fi(0), 24) == 0x80000000u);
  CHECK(x87_arith(Op::Add, ff(0x80000000), ff(0x80000000), 24) == 0x80000000u);
  CHECK(x87_arith(Op::Sub, ff(0x80000000), fi(0), 24) == 0x80000000u);
  // the consult helpers
  CHECK(B(mixed_add(16777217, bits_f32(0x26901d7d))) == 0x4b800001u);
  CHECK(B(mixed_add(16777217, bits_f32(0x26901d7d), 64)) == 0x4b800000u);
  CHECK(B(mixed_sub_fi(1.0f, 16777217)) == B(-16777216.0f));
  CHECK(B(mixed_div_fi(3.0f, 2)) == B(1.5f));
}

TEST_CASE("osi vm primitives: canon_nan (D9) and cmp_exact (0x60-0x64)") {
  const uint32_t inf = 0x7f800000u;
  CHECK(x87_arith(Op::Sub, Num::of_float_bits(inf), Num::of_float_bits(inf), 24) == kCanonicalNaN);
  CHECK(x87_arith(Op::Mul, Num::of_float_bits(inf), Num::of_int(0), 64) == kCanonicalNaN);
  CHECK(canon_nan_bits(0x7fc00001u) == kCanonicalNaN);
  CHECK(canon_nan_bits(0x3f800000u) == 0x3f800000u);
  CHECK(B(canon_nan(std::numeric_limits<float>::quiet_NaN())) == kCanonicalNaN);
  const double nan = std::numeric_limits<double>::quiet_NaN();
  CHECK(cmp_exact(16777217.0, 16777216.0, Op::Eq) == 0);  // 0x60#1
  CHECK(cmp_exact(16777219.0, 16777220.0, Op::Lt) == 1);  // 0x61#1
  CHECK(cmp_exact(16777217.0, 16777216.0, Op::Gt) == 1);  // 0x62#1
  CHECK(cmp_exact(16777217.0, 16777216.0, Op::Le) == 0);  // 0x63#1
  CHECK(cmp_exact(16777219.0, 16777220.0, Op::Ge) == 0);  // 0x64#1
  CHECK(cmp_exact(0.0, -0.0, Op::Eq) == 1);
  for (Op o : {Op::Eq, Op::Lt, Op::Gt, Op::Le, Op::Ge}) {
    CHECK(cmp_exact(nan, 1.0, o) == 0);
    CHECK(cmp_exact(1.0, nan, o) == 0);
  }
}

TEST_CASE("osi vm primitives: ConvertVariant arithmetic (0xb0-0xb2 rows, guards guard:0xb1 guard:0xb2)") {
  CHECK(chop_to_int(B(2.7f)) == 2);
  CHECK(chop_to_int(B(-2.7f)) == -2);
  CHECK(chop_to_int(B(3.0e9f)) == -1294967296);   // 3000000000 - 2^32
  CHECK(chop_to_int(B(-3.0e9f)) == 1294967296);
  CHECK(chop_to_int(kCanonicalNaN) == 0);
  CHECK(chop_to_int(B(1.0e19f)) == 0);             // |x| >= 2^63: integer indefinite, low dword 0
  CHECK(int_to_f32_bits(16777217) == 0x4b800000u); // guard:0xb1
  CHECK(int_to_f32_bits(16777219) == 0x4b800002u);
  CHECK(format_d(-5) == "-5");
  CHECK(format_f(B(1.5f)) == "1.500000");
  CHECK(format_f(B(-2.25f)) == "-2.250000");
  CHECK(osi_atoi(" -12x") == -12);
  CHECK(osi_atoi("abc") == 0);
  bool sw = false;
  CHECK(osi_atof_bits("0.5", &sw) == 0x3f000000u);
  CHECK(sw);
  CHECK(osi_atof_bits("-2.25", &sw) == B(-2.25f));
  CHECK(osi_atof_bits("abc", &sw) == 0u);
  CHECK(sw);  // ___dec2num runs even when nothing parses (D8')
  CHECK(osi_atof_bits("1e99999", &sw) == 0x7f800000u);
  CHECK_FALSE(sw);  // ___strtold's exponent-overflow path skips ___dec2num
  CHECK(osi_atof_bits("0x10", &sw) == 0u);  // hex is not decimal text: "0" then stop
}

TEST_CASE("osi vm primitives: Heap handles (D7), Duplicate/Dispose and the A/F records") {
  StringTrace tr;
  Heap h(&tr);
  sage::OsiFile none;
  uint32_t o = h.new_object(3, 2);
  uint32_t a = h.new_array();
  uint32_t s1 = h.new_string("hi");
  uint32_t g = h.new_game(0x27);
  CHECK(o == 1);
  CHECK(a == 1);
  CHECK(s1 == 1);
  CHECK(g == 1);
  // the object owns its properties; the array its elements
  uint32_t s2 = h.new_string("p0");
  uint32_t s3 = h.new_string("p1");
  h.elements(a).push_back(Variant{kString, h.new_string("e0")});
  h.elements(a).push_back(Variant{kString, h.new_string("e1")});
  Variant ov{kObject, o};
  Variant copy = h.duplicate(ov);  // +1 reference
  CHECK(copy == ov);
  Variant sv{kString, s1};
  Variant scopy = h.duplicate(sv);  // deep copy: a new handle
  CHECK(scopy.value == 6);
  CHECK(h.text(scopy.value) == "hi");
  // put the two strings in the object's props via the heap's object storage (as 0x10 would)
  tr.lines.clear();
  h.dispose(ov);
  CHECK(tr.lines.empty());  // refcount 2 -> 1
  h.dispose(Variant{kArray, a});
  REQUIRE(tr.lines.size() == 3);
  CHECK(tr.lines[0] == "F a 00000001");
  CHECK(tr.lines[1] == "F s 00000005");  // back to front: e1 (handle 5) before e0 (handle 4)
  CHECK(tr.lines[2] == "F s 00000004");
  CHECK(h.stats().allocs[kKindString] == 6);
  CHECK(h.stats().live[kKindArray] == 0);
  tr.lines.clear();
  h.dispose(Variant{0x27, g});
  CHECK(tr.lines == std::vector<std::string>{"F g 00000001"});
  (void)s2;
  (void)s3;
  // A record formats
  StringTrace t2;
  Heap h2(&t2);
  h2.new_object(0x12, 0);
  h2.new_array(3);
  h2.new_string("abc");
  h2.new_game(0x42);
  CHECK(t2.lines == std::vector<std::string>{"A o 00000001 0012", "A a 00000001 00000003", "A s 00000001   3", "A g 00000001 42"});
}

TEST_CASE("osi vm primitives: Stack caps (D10) and frames") {
  Stack s;
  for (uint32_t i = 0; i < kStackSlots; ++i) s.push(make_int(static_cast<int32_t>(i)));
  CHECK(s.depth() == 1000);
  bool threw = false;
  try {
    s.push(make_int(1));
  } catch (const VmError& e) {
    threw = true;
    CHECK(e.message == "Stack overflow (1000 slots)");
  }
  CHECK(threw);
  CHECK(s.depth() == 1000);
  Stack e;
  threw = false;
  try {
    e.pop();
  } catch (const VmError& er) {
    threw = true;
    CHECK(er.message == "Stack underflow");
  }
  CHECK(threw);
  Stack f;
  CHECK(f.find_frame() == 0);
  f.push(Variant{kFrameMarker, 0});
  f.push(make_int(1));
  f.push(make_int(2));
  CHECK(f.find_frame() == 1);
  f.insert_below(2, Variant{kRetMarker, 9});
  CHECK(f.depth() == 4);
  CHECK(f.at(1) == Variant{kRetMarker, 9});
  CHECK(f.at(2) == make_int(1));
  CHECK(f.find_frame() == 2);
}

TEST_CASE("osi vm primitives: trace field formats, FNV-1a, SHA-256, digest") {
  CHECK(fmt_variant(make_int(-1)) == "0010:ffffffff");
  CHECK(fmt_variant(make_float(-0.0f)) == "0012:80000000");
  CHECK(std::string(kEmptySlot()) == "----:--------");
  CHECK(fmt_name(std::string("a b\\\x80z", 6)) == "a\\x20b\\\\\\x80z");
  CHECK(fmt_message(std::string("say \"hi\"\\ \x01", 11)) == "say \\\"hi\\\"\\\\ \\x01");
  CHECK(fmt_hex(std::string("\x00\xff" "a", 3)) == "00ff61");
  const char* abc = "abc";
  CHECK(sha256_hex(reinterpret_cast<const uint8_t*>(abc), 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(fnv1a64(0xcbf29ce484222325ull, "") == 0xcbf29ce484222325ull);
  StringTrace out;
  DigestTrace d(&out);
  d.line("# hdr");
  d.line("K      0 00000000 00000000 00000000");
  d.line("I 00000000 00 0 0 ----:--------");
  d.finish();
  REQUIRE(out.lines.size() == 3);
  CHECK(out.lines[0] == "# hdr");
  CHECK(out.lines[1].rfind("D      0        2 ", 0) == 0);
  CHECK(out.lines[2].rfind("T 3 ", 0) == 0);
}

TEST_CASE("osi vm primitives: JSON reader and TypeTable / NativeRegistry over data/osi") {
  Json j;
  std::string err;
  REQUIRE(parse_json(R"({"a": [1, -2, "x\"y"], "b": null, "c": true, "d": 4294967295})", j, &err));
  int64_t v = 0;
  REQUIRE(j.get("d"));
  CHECK(j.get("d")->as_i64(v));
  CHECK(v == 4294967295LL);
  CHECK(j.get("a")->arr.at(2).str == "x\"y");
  CHECK(j.get("b")->is_null());
  auto slurp = [](const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
  };
  const std::string root = std::string(LOMN_TESTS_DIR) + "/../../data/osi/";
  TypeTable types = TypeTable::load(slurp(root + "types.json"));
  CHECK(types.id("type:GcRectangle") == 0x20);  // rank 0
  CHECK(types.id("GcTimer") == 0x27);           // rank 7
  CHECK(types.id("type:ScTileFont") == 0x42);   // rank 34
  CHECK(types.id("byref") == 0x200);
  NativeRegistry reg = NativeRegistry::load(slurp(root + "natives.json"), slurp(root + "natives_beta150.json"), types);
  CHECK(reg.size() == 1049 + 19);  // natives.json winners + the mod rows (python3 count over data/osi, 2026-10-08)
  const NativeEntry* sc = reg.find("ScTileFont", "SETCOLOR");
  REQUIRE(sc);
  CHECK(sc->types[0] == 0x42);
  CHECK(sc->min == 5);
  const NativeEntry* stf = reg.find("gcscripttools", "stringtofloat");
  REQUIRE(stf);
  CHECK(stf->sets_fpu_pc64);
  const NativeEntry* gb = reg.find("bettersaver", "getbooleanvalue");
  REQUIRE(gb);
  CHECK(gb->tier == Tier::Mod);
  CHECK(reg.find("gclegosounddirector", "play") == nullptr);
  bool threw = false;
  try {
    reg.bind("no.such", nat_seven, nullptr, Tier::Implemented);
  } catch (const std::logic_error&) {
    threw = true;
  }
  CHECK(threw);
}

// trace.md §4, the worked example, byte for byte (the header line is the host's).
TEST_CASE("osi vm trace: the trace.md worked example (0x46, 0x82, 0x30, 0x44, 0x53 error, P after E)") {
  T t;
  Asm a;
  a.str("t");
  a.str("echo");
  a.fn("main", 0).s("abc").call("t", "echo", 1).pop().f(1.0f).rel(0x53, "end").label("end").ret();
  t.reg.add(entry("t", "echo", 1, 1, {kString}, nat_text_abc));
  CallResult r = t.run(a);
  CHECK_FALSE(r.ok);
  const std::vector<std::string> want = {
      "C main 0 000f:00000000 000f:00000000 000f:00000000 000f:00000000 000f:00000000 000f:00000000 000f:00000000 "
      "000f:00000000 000f:00000000 000f:00000000",
      "I 0000002d 46    1    1 00ff:00000000",
      "I 00000030 82    2    1 0011:00000002",
      "A s 00000001   3",
      "A s 00000002   3",
      "S 00000001   3 616263",
      "S 00000002   3 616263",
      "N 00000030 t.echo 1 0016:00000001 -> 0016:00000002",
      "F s 00000001",
      "I 00000036 30    2    1 0016:00000002",
      "F s 00000002",
      "I 00000037 44    1    1 00ff:00000000",
      "I 0000003c 53    2    1 0012:3f800000",
      "E 0000003c 53    1 \"Results of comparison expression is not integral\"",
      "P 0000003c 64"};
  CHECK(t.tr.lines == want);
  CHECK(t.vm->snapshot().fpu_pc == 64);
}

// ============================== handler families ==============================

TEST_CASE("osi vm 0x00 0x04 0x05 0x06 nop/line: no stack change, PC +1/+5; 0x02/0x03 always error") {
  T t;
  Asm a;
  a.fn("main", 0).op(0x00).op(0x04).u16(1).u16(2).op(0x05).u16(0).u16(0).op(0x06).u16(0).u16(0).i8(3).ret();
  CallResult r = t.run(a);
  CHECK(r.ok);
  CHECK(r.result == I(3));
  CHECK(t.vm->snapshot().depth == 0);
  for (uint8_t o : {0x02, 0x03}) {
    T u;
    Asm b;
    b.fn("main", 0).i8(1).label("x").op(o).ret();
    CallResult q = u.run(b);
    CHECK_FALSE(q.ok);
    CHECK(u.err() == (o == 0x02 ? "Debugger not enabled! but encountered DebugOn" : "Debugger not enabled! but encountered DebugOff"));
    CHECK(u.vm->last_error()->pc == b.at("x"));
    CHECK(u.vm->last_error()->depth == 2);
    CHECK(u.vm->snapshot().pc == b.at("x") + 1);  // 0x02/0x03 advance PC before the error (0x6038a6, 0x6038d6)
  }
}

TEST_CASE("osi vm 0x13 0x10 0x11 members: CreateObject, Set/GetMemberValue, the object owns its props") {
  T t;
  Asm a;
  a.cls("thing", {"x", "y"});
  a.fn("main", 0).op(0x13).u16(0).op(0x35)          // obj obj
      .i8(5).op(0x32)                                 // obj 5 obj  (Swap so o is on top of v)
      .op(0x10).u16(a.sym("y"))                       // obj.y = 5 -> obj
      .op(0x11).u16(a.sym("y")).ret();
  CallResult r = t.run(a);
  CHECK(r.ok);
  CHECK(r.result == I(5));
  CHECK(t.heap.stats().live[kKindObject] == 0);
  CHECK(t.has("A o 00000001 0000"));
  CHECK(t.has("F o 00000001"));
  // errors: absent property (%s = the symbol name), non-member variable, unregistered game variable
  {
    T u;
    Asm b;
    b.cls("thing", {"x"});
    b.sym("zz");
    b.fn("main", 0).i8(1).op(0x13).u16(0).op(0x10).u16(b.sym("zz")).ret();
    CHECK_FALSE(u.run(b).ok);
    CHECK(u.err() == "Reference to non-existant member property \"zz\"");
    CHECK(u.vm->last_error()->depth == 1);  // o and v popped
  }
  {
    T u;
    Asm b;
    b.sym("x");
    b.fn("main", 0).i8(1).i8(2).op(0x10).u16(0).ret();
    CHECK_FALSE(u.run(b).ok);
    CHECK(u.err() == "Member property of non member variable");
    CHECK(u.vm->last_error()->depth == 2);  // only o popped
  }
  {
    T u;
    Asm b;
    b.sym("x");
    b.fn("main", 0).call("t", "obj", 0).op(0x11).u16(0).ret();
    u.reg.add(entry("t", "obj", 0, 0, {}, [](NativeCall& c, void*) { return Variant{0x27, c.vm.heap().new_game(0x27)}; }));
    CHECK_FALSE(u.run(b).ok);
    CHECK(u.err() == "Reference to unregistered Game Variable");
  }
}

TEST_CASE("osi vm 0x12 0x51 0x14 0x15 0x16 0x17 methods: call through a tagged file offset, this, arg check") {
  T t;
  Asm a;
  a.cls("thing", {"v"});
  // main: o = new thing; o.set(7); return o.get()
  a.fn("main", 0).op(0x13).u16(0).op(0x35).i8(7).op(0x33).u8(2)   // o o 7 o
      .op(0x12).u16(a.sym("set")).op(0x51).u8(2).pop()            // o.set(7) -> result popped; stack: o
      .op(0x35).op(0x35).op(0x12).u16(a.sym("get")).op(0x51).u8(1).op(0x32).pop().ret();
  a.method("set").op(0x14).u8(2).op(0x21).u16(1).op(0x15).u16(a.sym("v")).nothing().ret();
  a.method("get").op(0x14).u8(1).op(0x16).u16(a.sym("v")).ret();
  CallResult r = t.run(a);
  CHECK_MESSAGE(r.ok, t.err());
  CHECK(r.result == I(7));
  CHECK(t.heap.stats().live[kKindObject] == 0);
  // 0x14 pads with nothings up to n (this counts): a 3-slot method called with 1 slot
  {
    T u;
    Asm b;
    b.cls("thing", {});
    b.fn("main", 0).op(0x13).u16(0).op(0x35).op(0x35).op(0x12).u16(b.sym("m")).op(0x51).u8(1).op(0x32).pop().ret();
    b.method("m").op(0x14).u8(3).op(0x21).u16(2).ret();
    CallResult q = u.run(b);
    CHECK(q.ok);
    CHECK(q.result == nothing());
  }
  {  // "Too many arguments to method"
    T u;
    Asm b;
    b.cls("thing", {});
    b.fn("main", 0).op(0x13).u16(0).op(0x35).i8(1).op(0x32).op(0x12).u16(b.sym("m")).op(0x51).u8(2).ret();
    b.method("m").label("chk").op(0x14).u8(1).ret();
    CHECK_FALSE(u.run(b).ok);
    CHECK(u.err() == "Too many arguments to method");
    CHECK(u.vm->last_error()->pc == b.at("chk"));
  }
  {  // 0x17 GetThisMemberFunction, and 0x12 "Call to undefined member method"
    T u;
    Asm b;
    b.cls("thing", {});
    b.sym("nope");
    b.fn("main", 0).op(0x13).u16(0).op(0x35).op(0x35).op(0x12).u16(b.sym("m")).op(0x51).u8(1).op(0x32).pop().ret();
    b.method("m").op(0x14).u8(1).op(0x17).u16(b.sym("k")).op(0x21).u16(0).op(0x32).op(0x51).u8(1).ret();
    b.method("k").op(0x14).u8(1).i8(42).ret();
    CallResult q = u.run(b);
    CHECK_MESSAGE(q.ok, u.err());
    CHECK(q.result == I(42));
    T w;
    Asm c;
    c.cls("thing", {});
    c.sym("nope");
    c.fn("main", 0).op(0x13).u16(0).op(0x12).u16(c.sym("nope")).ret();
    CHECK_FALSE(w.run(c).ok);
    CHECK(w.err() == "Call to undefined member method");
  }
  {  // 0x15/0x16/0x17 on a non-object this
    for (uint8_t o : {0x15, 0x16, 0x17}) {
      T u;
      Asm b;
      b.sym("v");
      b.fn("main", 1).op(0x21).u16(0).op(o).u16(0).ret();
      CHECK_FALSE(u.run(b, "main", {I(1)}).ok);
      CHECK(u.err() == (o == 0x17 ? "Member method of non member variable" : "Member property of non member variable"));
    }
    T u;
    Asm b;
    b.cls("thing", {});
    b.sym("q");
    b.fn("main", 0).op(0x13).u16(0).op(0x35).op(0x12).u16(b.sym("m")).op(0x51).u8(1).ret();
    b.method("m").op(0x14).u8(1).op(0x16).u16(b.sym("q")).ret();
    CHECK_FALSE(u.run(b).ok);
    CHECK(u.err() == "Reference to non-existant member property \"q\"");
  }
}

TEST_CASE("osi vm 0x18 0x19 0x1a members by string: case-folded FindMemberID, the four error texts") {
  T t;
  Asm a;
  a.cls("thing", {"hp"});
  a.fn("main", 0).op(0x13).u16(0).op(0x35)        // o o
      .i8(9).op(0x32).s("HP").op(0x1a)            // o.hp = 9 (v o s) -> o
      .op(0x35).s("hp").op(0x18)                  // o o.hp
      .op(0x32).pop().ret();
  CallResult r = t.run(a);
  CHECK_MESSAGE(r.ok, t.err());
  CHECK(r.result == I(9));
  CHECK(t.heap.stats().live[kKindObject] == 0);
  {
    T u;
    Asm b;
    b.cls("thing", {"hp"});
    b.fn("main", 0).op(0x13).u16(0).i8(3).op(0x18).ret();
    CHECK_FALSE(u.run(b).ok);
    CHECK(u.err() == "Invalid type for member access (must be a string)");
    CHECK(u.vm->last_error()->depth == 1);  // both popped before the check: the marker remains
  }
  {
    T u;
    Asm b;
    b.cls("thing", {"hp"});
    b.fn("main", 0).op(0x13).u16(0).s("Mana").op(0x18).ret();
    CHECK_FALSE(u.run(b).ok);
    CHECK(u.err() == "Reference to non-existant member property \"Mana\"");  // %s = the name as given
  }
  {
    T u;
    Asm b;
    b.fn("main", 0).i8(1).s("hp").op(0x18).ret();
    CHECK_FALSE(u.run(b).ok);
    CHECK(u.err() == "Member property of non member variable");
  }
  {
    T u;
    Asm b;
    b.fn("main", 0).i8(1).i8(1).s("hp").op(0x1a).ret();
    CHECK_FALSE(u.run(b).ok);
    CHECK(u.err() == "Member property of non object variable");
  }
  {
    T u;
    Asm b;
    b.fn("main", 0).i8(1).i8(1).i8(2).op(0x1a).ret();
    CHECK_FALSE(u.run(b).ok);
    CHECK(u.err() == "Invalid type for member set (must be a string)");
  }
  {  // 0x19: method by string, then 0x51
    T u;
    Asm b;
    b.cls("thing", {});
    b.fn("main", 0).op(0x13).u16(0).op(0x35).op(0x35).s("GO").op(0x19).op(0x51).u8(1).op(0x32).pop().ret();
    b.method("go").op(0x14).u8(1).i8(11).ret();
    CallResult q = u.run(b);
    CHECK_MESSAGE(q.ok, u.err());
    CHECK(q.result == I(11));
    T w;
    Asm c;
    c.fn("main", 0).i8(1).s("go").op(0x19).ret();
    CHECK_FALSE(w.run(c).ok);
    CHECK(w.err() == "Member method of non member variable");
    T x;
    Asm d;
    d.cls("thing", {});
    d.fn("main", 0).op(0x13).u16(0).i8(1).op(0x19).ret();
    CHECK_FALSE(x.run(d).ok);
    CHECK(x.err() == "Invalid method type for function call (must be a string)");
  }
}

TEST_CASE("osi vm 0x21 0x22 0x23 0x24 0x25 variables: frame slots, globals by name (bit 15), inc/dec texts") {
  T t;
  Asm a;
  uint16_t g = a.glob("counter");
  a.fn("main", 2).op(0x23).u8(1)                              // args a0 a1, local l2
      .op(0x21).u16(0).op(0x21).u16(1).op(0x70).op(0x22).u16(2)  // l2 = a0 + a1
      .op(0x24).u16(2).op(0x24).u16(2).op(0x25).u16(2)           // l2 += 1, += 1, -= 1
      .op(0x21).u16(2).op(0x22).u16(g)                           // counter = l2
      .op(0x24).u16(g).op(0x21).u16(g).ret();
  CallResult r = t.run(a, "main", {I(3), I(4)});
  CHECK_MESSAGE(r.ok, t.err());
  CHECK(r.result == I(9));
  REQUIRE(t.vm->find_global("COUNTER"));
  CHECK(*t.vm->find_global("counter") == I(9));
  {
    T u;
    Asm b;
    b.fn("main", 1).op(0x25).u16(0).ret();
    CHECK_FALSE(u.run(b, "main", {F(1.0f)}).ok);
    CHECK(u.err() == "decrement of non-integral type");
  }
  {
    T u;
    Asm b;
    uint16_t gg = b.glob("g");
    b.fn("main", 0).op(0x25).u16(gg).ret();  // an unknown global is created as nothing: not int
    CHECK_FALSE(u.run(b).ok);
    CHECK(u.err() == "increment of non-integral type");  // the original's copy-paste text on the global path
  }
  {
    T u;
    Asm b;
    b.fn("main", 1).op(0x24).u16(0).ret();
    CHECK_FALSE(u.run(b, "main", {nothing()}).ok);
    CHECK(u.err() == "increment of non-integral type");
  }
}

TEST_CASE("osi vm 0x30 0x31 0x32 0x33 0x34 0x35 stack ops") {
  T t;
  Asm a;
  a.fn("main", 0).i8(1).i8(2).i8(3).op(0x33).u8(3)  // 1 2 3 1
      .op(0x34).u8(2)                                 // 1 2 3 1 3 1
      .op(0x31).u8(2)                                 // 1 2 3 1
      .op(0x32)                                       // 1 2 1 3
      .op(0x70).op(0x70).op(0x70).ret();              // 1+2+1+3 = 7
  CallResult r = t.run(a);
  CHECK_MESSAGE(r.ok, t.err());
  CHECK(r.result == I(7));
  T u;
  Asm b;
  b.fn("main", 0).s("ab").op(0x35).op(0xb0).op(0x35).pop().pop().pop().nothing().ret();  // Dup of a 0x16 deep-copies
  CHECK(u.run(b).ok);
  CHECK(u.has("A s 00000001   2"));
  CHECK(u.has("A s 00000002   2"));
  CHECK(u.heap.stats().live[kKindString] == 0);
}

TEST_CASE("osi vm 0x40-0x49 push constants (0x41 always errors, PC not advanced)") {
  T t;
  Asm a;
  a.fn("main", 0).i32(-5).op(0x42).u16(0xfffe).op(0x70).op(0x43).u8(0xff).op(0x70).op(0x45).op(0x70).ret();
  CallResult r = t.run(a);
  CHECK(r.ok);
  CHECK(r.result == I(-8));
  T u;
  Asm b;
  b.fn("main", 0).f(2.5f).op(0x48).u32(0x11223344).op(0x49).u16(0x7fff).s("q").nothing().op(0x31).u8(4).ret();
  CallResult q = u.run(b);
  CHECK(q.ok);
  CHECK(q.result == F(2.5f));
  CHECK(u.starting("I ").at(2).find(" 0015:11223344") != std::string::npos);  // 0x49's I line sees 0x48's colour on top
  CHECK(u.starting("I ").at(3).find(" 0014:00007fff") != std::string::npos);
  T w;
  Asm c;
  c.fn("main", 0).i8(1).label("x").op(0x41).u8(0).u8(0).u8(0).ret();
  CHECK_FALSE(w.run(c).ok);
  CHECK(w.err() == "unwritten instruction BCL_PushConstanti24 called");
  CHECK(w.vm->last_error()->pc == c.at("x"));
  CHECK(w.vm->last_error()->depth == 2);
}

TEST_CASE("osi vm 0x50 0x52 0x53 0x54 calls and branches (file-relative target, markers, float condition)") {
  T t;
  Asm a;
  // main: r = sub(20, 22); if (r) r = r + 0 else r = 99
  a.fn("main", 0).i8(20).i8(22).addr("sub").op(0x50).u8(2).op(0x35).rel(0x53, "else").rel(0x54, "end")
      .label("else").pop().i8(99).label("end").ret();
  a.label("sub").op(0x21).u16(0).op(0x21).u16(1).op(0x70).ret();
  CallResult r = t.run(a);
  CHECK_MESSAGE(r.ok, t.err());
  CHECK(r.result == I(42));
  CHECK(t.vm->snapshot().depth == 0);
  // nothing is false (branch), int 0 false, int 1 true; a float condition is an error (0x53 falsifier)
  T u;
  Asm b;
  b.fn("main", 0).nothing().rel(0x53, "a").i8(1).ret().label("a").i8(0).rel(0x53, "b").i8(2).ret().label("b").i8(1)
      .rel(0x53, "c").i8(3).ret().label("c").i8(4).ret();
  CallResult q = u.run(b);
  CHECK(q.ok);
  CHECK(q.result == I(3));
  T w;
  Asm c;
  c.fn("main", 0).f(0.0f).label("x").rel(0x53, "x").ret();
  CHECK_FALSE(w.run(c).ok);
  CHECK(w.err() == "Results of comparison expression is not integral");
  CHECK(w.vm->last_error()->depth == 1);
  T x;
  Asm d;
  d.fn("main", 0).f(1.0f).op(0x50).u8(0).ret();
  CHECK_FALSE(x.run(d).ok);
  CHECK(x.err() == "Internal error: Jump to non-integral address");
  T y;
  Asm e;
  e.fn("main", 0).f(1.0f).op(0x51).u8(0).ret();
  CHECK_FALSE(y.run(e).ok);
  CHECK(y.err() == "Internal error: Jump to non-integral address");
}

TEST_CASE("osi vm 0x52 return disposes locals top-down; 0xff Halt leaves {0xff, nothing}") {
  T t;
  Asm a;
  a.fn("main", 0).s("a").op(0xb0).s("b").op(0xb0).i8(5).ret();
  CallResult r = t.run(a);
  CHECK(r.ok);
  CHECK(r.result == I(5));
  auto f = t.starting("F ");
  REQUIRE(f.size() == 2);
  CHECK(f[0] == "F s 00000002");
  CHECK(f[1] == "F s 00000001");
  T u;
  Asm b;
  b.fn("main", 0).i8(1).op(0xff).i8(2).ret();
  CallResult q = u.run(b);
  CHECK(q.ok);
  CHECK(q.result == nothing());
  CHECK(u.vm->snapshot().depth == 0);  // Halt does not unwind, but CallWithReturn pops (with dispose) to its marker
}

TEST_CASE("osi vm 0x60 EqualTo: nothing cases, identity, string fold, exact mixed compare, error texts") {
  auto eval = [](std::function<void(Asm&)> body) {
    auto t = std::make_unique<T>();
    Asm a;
    a.fn("main", 0);
    body(a);
    a.ret();
    CallResult r = t->run(a);
    return std::make_pair(r, std::move(t));
  };
  CHECK(eval([](Asm& a) { a.nothing().nothing().op(0x60); }).first.result == I(1));
  CHECK(eval([](Asm& a) { a.nothing().i8(0).op(0x60); }).first.result == I(0));  // nothing = 0 -> 0
  CHECK(eval([](Asm& a) { a.i8(0).nothing().op(0x60); }).first.result == I(0));
  CHECK(eval([](Asm& a) { a.s("ABC").s("abc").op(0x60); }).first.result == I(1));
  CHECK(eval([](Asm& a) { a.s("ab\xe9").s("AB\xc9").op(0x60); }).first.result == I(0));  // only a-z fold
  CHECK(eval([](Asm& a) { a.i32(16777217).f(16777216.0f).op(0x60); }).first.result == I(0));  // 0x60#1
  CHECK(eval([](Asm& a) { a.f(0.0f).fbits(0x80000000).op(0x60); }).first.result == I(1));
  CHECK(eval([](Asm& a) { a.i8(7).i8(7).op(0x60); }).first.result == I(1));
  CHECK(eval([](Asm& a) { a.cls("k", {}); a.op(0x13).u16(0).op(0x35).op(0x60); }).first.result == I(1));
  auto e = eval([](Asm& a) { a.i8(1).s("1").op(0x60); });
  CHECK_FALSE(e.first.ok);
  CHECK(e.second->err() == "Invalid operand to operator '=' (rhs) (type:16)");
  CHECK(e.second->has("A s 00000001   1"));  // the 0x11 operand became a dynamic string first (0x605397)
  auto e2 = eval([](Asm& a) { a.op(0x48).u32(1).i8(1).op(0x60); });
  CHECK(e2.second->err() == "Invalid operand to operator '=' (lhs) (type:15)");
}

TEST_CASE("osi vm 0x61 0x62 0x63 0x64 compares (falsifiers 0x61#1-0x64#2, error texts)") {
  struct C {
    uint8_t op;
    bool ia;
    double a;
    bool ib;
    double b;
    int want;
  };
  const C cs[] = {{0x61, true, 16777219, false, 16777220.0, 1}, {0x62, true, 16777217, false, 16777216.0, 1},
                  {0x63, true, 16777217, false, 16777216.0, 0}, {0x64, true, 16777219, false, 16777220.0, 0},
                  {0x64, false, 16777216.0, true, 16777217, 0},  {0x61, true, -3, true, 2, 1},
                  {0x62, false, 2.5, false, 2.5, 0},             {0x63, false, 2.5, false, 2.5, 1}};
  for (const C& c : cs) {
    T t;
    Asm a;
    a.fn("main", 0);
    if (c.ia) a.i32(static_cast<int32_t>(c.a)); else a.f(static_cast<float>(c.a));
    if (c.ib) a.i32(static_cast<int32_t>(c.b)); else a.f(static_cast<float>(c.b));
    a.op(c.op).ret();
    CallResult r = t.run(a);
    CHECK_MESSAGE(r.ok, t.err());
    CHECK_MESSAGE(r.result == I(c.want), "op " << int(c.op));
  }
  const char* lhs[] = {"Invalid operand to operator '<' (lhs) (type:16)", "Invalid operand to operator '>' (lhs) (type:16)",
                       "Invalid operand to operator '<=' (lhs) (type:16)", "Invalid operand to operator '>=' (lhs) (type:16)"};
  uint8_t ops[] = {0x61, 0x62, 0x63, 0x64};
  for (int k = 0; k < 4; ++k) {
    T t;
    Asm a;
    a.fn("main", 0).s("x").op(0xb0).i8(1).op(ops[k]).ret();
    CHECK_FALSE(t.run(a).ok);
    CHECK(t.err() == lhs[k]);
    T u;
    Asm b;
    b.fn("main", 0).f(1.0f).nothing().op(ops[k]).ret();
    CHECK_FALSE(u.run(b).ok);
    CHECK(u.err() == std::string(lhs[k]).replace(std::string(lhs[k]).find("(lhs) (type:16)"), 15, "(lhs) (type:12)"));
  }
}

TEST_CASE("osi vm 0x6a-0x6f 0x76-0x78 logic and bitwise (0x78 logical shift falsifier)") {
  auto bin = [](uint8_t op, Variant a, Variant b) {
    T t;
    Asm s;
    s.fn("main", 2).op(0x21).u16(0).op(0x21).u16(1).op(op).ret();
    CallResult r = t.run(s, "main", {a, b});
    return std::make_pair(r, r.ok ? std::string() : t.err());
  };
  CHECK(bin(0x6a, I(2), I(3)).first.result == I(1));
  CHECK(bin(0x6a, I(2), I(0)).first.result == I(0));
  CHECK(bin(0x6b, I(0), I(0)).first.result == I(0));
  CHECK(bin(0x6b, I(0), I(-1)).first.result == I(1));
  CHECK(bin(0x6d, I(12), I(10)).first.result == I(8));
  CHECK(bin(0x6e, I(12), I(10)).first.result == I(14));
  CHECK(bin(0x6f, I(12), I(10)).first.result == I(6));
  CHECK(bin(0x77, I(1), I(33)).first.result == I(2));     // b & 31
  CHECK(bin(0x78, I(-8), I(1)).first.result == I(0x7ffffffc));
  CHECK(bin(0x6a, nothing(), I(1)).second == "Invalid operand to operator 'and' (lhs) (type:0F)");
  CHECK(bin(0x6b, I(1), F(1.0f)).second == "Invalid operand to operator 'or' (rhs) (type:12)");
  CHECK(bin(0x6d, F(1.0f), I(1)).second == "Invalid operand to operator '&' (lhs) (type:12)");
  CHECK(bin(0x6e, I(1), nothing()).second == "Invalid operand to operator '|' (rhs) (type:0F)");
  CHECK(bin(0x6f, nothing(), I(1)).second == "Invalid operand to operator '@' (lhs) (type:0F)");
  CHECK(bin(0x77, I(1), F(1.0f)).second == "Invalid operand to operator '<<' (rhs) (type:12)");
  CHECK(bin(0x78, F(1.0f), I(1)).second == "Invalid operand to operator '>>' (lhs) (type:12)");
  auto un = [](uint8_t op, Variant a) {
    T t;
    Asm s;
    s.fn("main", 1).op(0x21).u16(0).op(op).ret();
    CallResult r = t.run(s, "main", {a});
    return std::make_pair(r, r.ok ? std::string() : t.err());
  };
  CHECK(un(0x6c, I(0)).first.result == I(1));
  CHECK(un(0x6c, I(5)).first.result == I(0));
  CHECK(un(0x6c, nothing()).first.result == I(1));
  CHECK(un(0x76, I(0)).first.result == I(-1));
  CHECK(un(0x76, nothing()).first.result == I(1));
  CHECK(un(0x6c, F(0.0f)).second == "Invalid operand to operator 'not' (type:12)");
  CHECK(un(0x76, F(0.0f)).second == "Invalid operand to operator '~' (type:12)");
  CHECK(un(0x7a, F(1.0f)).first.result == make_float_bits(0x3f800001));  // raw bits + 1
  CHECK(un(0x7b, I(0)).first.result == I(-1));
}

TEST_CASE("osi vm 0x70 0x71 0x72 0x73 0x75 arithmetic: wrap, D8' via 0xb1, strings, kLenTooLong, Divide by 0") {
  auto bin = [](uint8_t op, Variant a, Variant b) {
    T t;
    Asm s;
    s.fn("main", 2).op(0x21).u16(0).op(0x21).u16(1).op(op).ret();
    CallResult r = t.run(s, "main", {a, b});
    return std::make_pair(r, r.ok ? std::string() : t.err());
  };
  CHECK(bin(0x70, I(0x7fffffff), I(1)).first.result == I(INT32_MIN));
  CHECK(bin(0x71, I(16777217), F(1.0f)).first.result == make_float_bits(0x4b800000));  // 0x71#1
  CHECK(bin(0x72, I(0x10000), I(0x10000)).first.result == I(0));
  CHECK(bin(0x72, I(16777217), F(3.0f)).first.result == make_float_bits(0x4c400001));  // 0x72#1
  CHECK(bin(0x73, I(-7), I(2)).first.result == I(-3));
  CHECK(bin(0x73, I(50331651), F(3.0f)).first.result == make_float_bits(0x4b800000));  // 0x73#1
  CHECK(bin(0x73, F(1.0e30f), F(1.0e-30f)).first.result == make_float_bits(0x7f800000));  // 0x73#2
  CHECK(bin(0x73, F(1.0f), make_float_bits(0x80000000)).second == "Divide by 0");
  CHECK(bin(0x73, I(1), I(0)).second == "Divide by 0");
  CHECK(bin(0x73, F(1.0f), I(0)).second == "Divide by 0");
  CHECK(bin(0x75, I(-1), I(3)).first.result == I(0));  // unsigned: 0xffffffff % 3 = 0
  CHECK(bin(0x75, I(7), I(0)).second == "Divide by 0");
  CHECK(bin(0x75, F(7.0f), I(2)).second == "Invalid operand to operator '%' (lhs) (type:12)");
  CHECK(bin(0x72, nothing(), I(2)).second == "Invalid left-hand-side operand to operator '*' (type:0F)");
  CHECK(bin(0x72, I(2), nothing()).second == "Invalid operand to operator '*' (rhs) (type:0F)");
  CHECK(bin(0x71, I(2), nothing()).second == "Invalid operand to operator '-' (rhs) (type:0F)");
  CHECK(bin(0x73, nothing(), I(2)).second == "Invalid operand to operator '/' (lhs) (type:0F)");
  CHECK(bin(0x70, nothing(), I(2)).second == "Invalid operand to operator '+' (lhs) (type:0F)");
  // falsifiers 0x70#2 - 0x70#4: 1e-15 at 24, then "0.5" 0xb1 switches to 64 (P record), then both additions
  T t;
  Asm a;
  a.fn("main", 0).i32(16777217).fbits(0x26901d7d).op(0x70).op(0x22).u16(0).s("0.5").op(0xb1).pop()
      .i32(16777217).fbits(0x26901d7d).op(0x70).i32(16777217).fbits(0x2edbe6ff).op(0x70).op(0x21).u16(0).ret();
  a.code.insert(a.code.begin(), {0x23, 1});  // a local for the first result
  a.labels["fn:main"] = 0;
  CallResult r = t.run(a);
  CHECK_MESSAGE(r.ok, t.err());
  CHECK(r.result == make_float_bits(0x4b800001));
  REQUIRE(t.vm->snapshot().depth == 0);
  auto ps = t.starting("P ");
  REQUIRE(ps.size() == 1);
  // the stack before Return held 4b800000 (1e-15 at 64) and 4b800001 (1e-10 at 64); check via the I lines
  auto is = t.starting("I ");
  bool saw00 = false, saw01 = false;
  for (const auto& l : is) {
    if (l.find(" 0012:4b800000") != std::string::npos) saw00 = true;
    if (l.find(" 0012:4b800001") != std::string::npos) saw01 = true;
  }
  CHECK(saw00);
  CHECK(saw01);
  // string + string, and the 255-character cap (0x70 falsifier: 200 + 60 throws, 200 + 54 succeeds)
  T u;
  Asm b;
  b.fn("main", 0).s(std::string(200, 'a')).s(std::string(54, 'b')).op(0x70).op(0xb0).ret();
  CallResult q = u.run(b);
  CHECK(q.ok);
  CHECK(q.result_text.size() == 254);
  T w;
  Asm c;
  c.fn("main", 0).i8(1).s(std::string(200, 'a')).s(std::string(60, 'b')).label("add").op(0x70).ret();
  CHECK_FALSE(w.run(c).ok);
  CHECK(w.err() == "kLenTooLong");
  CHECK(w.vm->last_error()->pc == c.at("add"));
  CHECK(w.vm->last_error()->depth == 2);  // before - 2: both operands popped, nothing pushed
  CHECK(bin(0x70, I(1), make_float_bits(0)).first.ok);
  T x;
  Asm d;
  d.fn("main", 0).s("x").i8(1).op(0x70).ret();
  CHECK_FALSE(x.run(d).ok);
  CHECK(x.err() == "Invalid operand to string operator '+' (rhs) (type:10)");
  T y;
  Asm e;
  e.fn("main", 0).i8(1).s("x").op(0x70).ret();
  CHECK_FALSE(y.run(e).ok);
  CHECK(y.err() == "Invalid operand to string operator '+' (lhs) (type:10)");
}

TEST_CASE("osi vm 0x74 Power: the four cases (falsifiers 2.0 1 ^ and 3 2 ^)") {
  auto bin = [](Variant a, Variant b) {
    T t;
    Asm s;
    s.fn("main", 2).op(0x21).u16(0).op(0x21).u16(1).op(0x74).ret();
    CallResult r = t.run(s, "main", {a, b});
    return std::make_pair(r, r.ok ? std::string() : t.err());
  };
  CHECK(bin(F(2.0f), I(1)).first.result == make_float_bits(0x4f000000));  // (float)(int bits of 2.0) * 2.0
  CHECK(bin(I(3), I(2)).first.result == I(81));
  CHECK(bin(I(3), I(0)).first.result == I(3));
  CHECK(bin(F(2.0f), F(3.0f)).first.result == F(8.0f));
  CHECK(bin(I(2), F(10.0f)).first.result == F(1024.0f));
  CHECK(bin(I(3), I(-1)).first.ok);  // 2^32 - 1 iterations end at a fixed point
  CHECK(bin(nothing(), I(1)).second == "Invalid operand to operator '^' (lhs) (type:0F)");
  CHECK(bin(I(1), nothing()).second == "Invalid operand to operator '^' (rhs) (type:0F)");
}

TEST_CASE("osi vm 0x80 0x81 game-variable opcodes change nothing") {
  T t;
  Asm a;
  a.fn("main", 0).i8(4).op(0x80).u16(0).u16(0).op(0x81).u16(0).u16(0).ret();
  CallResult r = t.run(a);
  CHECK(r.ok);
  CHECK(r.result == I(4));
}

TEST_CASE("osi vm 0x82 CallGameFunction: conversion in place, pops with dispose, memoised (D4), errors") {
  T t;
  Asm a;
  a.fn("main", 0).label("loop").i8(3).call("t", "echo", 1).op(0x22).u16(0x8000 | 0);  // g = echo(3) as string
  a.globals.push_back("g");
  a.fn("again", 0).s("12").call("t", "num", 1).ret();
  t.reg.add(entry("t", "echo", 1, 1, {kString}, nat_echo_arg0));
  t.reg.add(entry("t", "num", 1, 1, {kInt}, nat_echo_arg0));
  a.code.push_back(0x47);
  a.code.push_back(0x52);
  // main: g = t.echo(3) -> "3"; return nothing
  t.load(a);
  // move the Return of main right after the global store
  CallResult r = t.vm->call("main", {});
  CHECK_MESSAGE(r.ok, t.err());
  REQUIRE(t.vm->find_global("g"));
  CHECK(t.vm->find_global("g")->type == kString);
  CHECK(t.heap.text(t.vm->find_global("g")->value) == "3");
  CallResult q = t.vm->call("again", {});
  CHECK_MESSAGE(q.ok, t.err());
  CHECK(q.result == I(12));  // "12" (0x11) -> int: atoi, the in-place dynamic string disposed
  // memoised second execution traces 82 both times
  T u;
  Asm b;
  b.fn("main", 0).i8(2).op(0x23).u8(0).label("top").call("t", "seven", 0).pop().op(0x7b).op(0x35).rel(0x53, "done")
      .rel(0x54, "top").label("done").ret();
  u.reg.add(entry("t", "seven", 0, 0, {}, nat_seven));
  CallResult m = u.run(b);
  CHECK(m.ok);
  CHECK(u.vm->memo(b.at("top")) != nullptr);
  int n82 = 0;
  for (const auto& l : u.starting("I "))
    if (l.substr(11, 2) == "82") ++n82;
  CHECK(n82 == 2);
  CHECK(u.starting("N ").size() == 2);
  CHECK(u.file.bytes()[b.at("top")] == 0x82);  // the image is never written
  // errors
  {
    T w;
    Asm c;
    c.fn("main", 0).label("c").call("t", "none", 0).ret();
    CHECK_FALSE(w.run(c).ok);
    CHECK(w.err() == "Call to unregistered Game Function");
    CHECK(w.vm->last_error()->pc == c.at("c"));
  }
  {
    T w;
    Asm c;
    c.fn("main", 0).i8(1).i8(2).call("t", "seven", 2).ret();
    w.reg.add(entry("t", "seven", 0, 1, {kInt}, nat_seven));
    CHECK_FALSE(w.run(c).ok);
    CHECK(w.err() == "Script is calling t.seven");  // "Too many parameters ..." is overwritten
    CHECK(w.vm->last_error()->depth == 3);          // arguments stay
  }
  {  // a conversion to a game type is the "unimplimented" error, reported as Script is calling
    T w;
    Asm c;
    c.fn("main", 0).i8(1).call("t", "obj", 1).ret();
    w.reg.add(entry("t", "obj", 1, 1, {0x27}, nat_seven));
    CHECK_FALSE(w.run(c).ok);
    CHECK(w.err() == "Script is calling t.obj");
  }
  {  // an unsupported source becomes {0, 0} silently
    T w;
    Asm c;
    c.fn("main", 0).nothing().call("t", "echo", 1).ret();
    w.reg.add(entry("t", "echo", 1, 1, {kInt}, nat_echo_arg0));
    CallResult z = w.run(c);
    CHECK(z.ok);
    CHECK(z.result == Variant{kNone, 0});
  }
  {  // by-reference: the native writes the slot, the pop disposes it, N prints the slot index
    T w;
    Asm c;
    c.fn("main", 0).i8(9).i8(1).call("t", "byref", 1).ret();
    w.reg.add(entry("t", "byref", 1, 1, {kByRef}, nat_byref_write5));
    CallResult z = w.run(c);
    CHECK(z.ok);
    CHECK(w.has("N 00000000 t.byref 1 0200:00000002 -> 000f:00000000") == false);  // pc below is the real one
    auto ns = w.starting("N ");
    REQUIRE(ns.size() == 1);
    CHECK(ns[0].find(" t.byref 1 0200:00000002 -> 000f:00000000") != std::string::npos);
  }
  {  // a literal 0x84 is our D10 error
    T w;
    Asm c;
    c.fn("main", 0).label("d").op(0x84).u32(0).u8(0).ret();
    CHECK_FALSE(w.run(c).ok);
    CHECK(w.err() == "Direct call without a memoised site");
  }
}

TEST_CASE("osi vm 0x83 CallGameFunctionFromString: name from the stack, its two escaping errors") {
  T t;
  Asm a;
  a.str("t");
  a.fn("main", 0).i8(5).s("ECHO").op(0x83).u16(0).u8(1).ret();
  t.reg.add(entry("t", "echo", 1, 1, {kInt}, nat_echo_arg0));
  CallResult r = t.run(a);
  CHECK_MESSAGE(r.ok, t.err());
  CHECK(r.result == I(5));
  CHECK(t.starting("N ").at(0).find(" t.ECHO 1 0010:00000005 -> 0010:00000005") != std::string::npos);
  T u;
  Asm b;
  b.str("t");
  b.fn("main", 0).i8(5).i8(1).op(0x83).u16(0).u8(1).ret();
  CHECK_FALSE(u.run(b).ok);
  CHECK(u.err() == "Invalid member type for function call (must be a string)");
  T w;
  Asm c;
  c.str("t");
  c.fn("main", 0).s("nope").op(0x83).u16(0).u8(0).ret();
  CHECK_FALSE(w.run(c).ok);
  CHECK(w.err() == "Script is calling t.nope");
}

TEST_CASE("osi vm 0x90-0x96 arrays: shared by reference, bounds texts, insert at count is an error") {
  T t;
  Asm a;
  a.fn("main", 0).op(0x90).op(0x35).i8(10).op(0x94)  // a=[10]
      .op(0x35).i8(20).op(0x94)                        // [10,20]
      .op(0x35).i8(0).i8(5).op(0x96)                   // [5,10,20]
      .op(0x35).i8(1).op(0x95)                         // [5,20]
      .op(0x35).i8(1).i8(7).op(0x93)                   // [5,7]
      .op(0x35).i8(1).op(0x91).op(0x32).op(0x92).op(0x70).ret();  // 7 + 2
  CallResult r = t.run(a);
  CHECK_MESSAGE(r.ok, t.err());
  CHECK(r.result == I(9));
  CHECK(t.heap.stats().live[kKindArray] == 0);
  CHECK(t.heap.stats().allocs[kKindArray] == 1);
  struct E {
    uint8_t op;
    const char* msg;
  };
  const E es[] = {{0x91, "Reference beyond start/end of array"}, {0x93, "Access beyond end of array"},
                  {0x95, "removal outside of array bounds"},     {0x96, "insertion outside of array bounds"}};
  for (const E& e : es) {
    T u;
    Asm b;
    b.fn("main", 0).op(0x90).op(0x35).i8(1).op(0x94).i8(1);
    if (e.op == 0x93 || e.op == 0x96) b.i8(3);
    b.op(e.op).ret();
    CHECK_FALSE(u.run(b).ok);
    CHECK(u.err() == e.msg);
  }
  T w;
  Asm c;
  c.fn("main", 0).i8(1).op(0x92).ret();
  CHECK_FALSE(w.run(c).ok);
  CHECK(w.err() == "array reference on non-array");
  T x;
  Asm d;
  d.fn("main", 0).op(0x90).i8(-1).op(0x91).ret();  // a negative index counts as huge
  CHECK_FALSE(x.run(d).ok);
  CHECK(x.err() == "Reference beyond start/end of array");
}

TEST_CASE("osi vm 0xa0-0xa7 colour channels: clamp to 255, 0x14 rejected") {
  T t;
  Asm a;
  a.fn("main", 0).op(0x48).u32(0x00000000).i8(-1).op(0xa0).i32(0x12).op(0xa1).i32(300).op(0xa2).i8(7).op(0xa3)
      .op(0x35).op(0xa4).op(0x32).op(0x35).op(0xa5).op(0x32).op(0x35).op(0xa6).op(0x32).op(0xa7)
      .op(0x70).op(0x70).op(0x70).ret();  // 255 + 0x12 + 255 + 7 = 535
  CallResult r = t.run(a);
  CHECK_MESSAGE(r.ok, t.err());
  CHECK(r.result == I(535));
  T u;
  Asm b;
  b.fn("main", 0).op(0x49).u16(1).op(0xa4).ret();
  CHECK_FALSE(u.run(b).ok);
  CHECK(u.err() == "cant access red value of anything but a color");
  T w;
  Asm c;
  c.fn("main", 0).op(0x48).u32(0).f(1.0f).op(0xa3).ret();
  CHECK_FALSE(w.run(c).ok);
  CHECK(w.err() == "cant set alpha to a non-integral value");
  T x;
  Asm d;
  d.fn("main", 0).i8(1).f(1.0f).op(0xa1).ret();
  CHECK_FALSE(x.run(d).ok);
  CHECK(x.err() == "cant access green value of anything but a color");  // c is checked before v
}

TEST_CASE("osi vm 0xb0 0xb1 0xb2 conversions (0xb2 falsifiers, %f, type 0)") {
  auto un = [](uint8_t op, Variant a) {
    T t;
    Asm s;
    s.fn("main", 1).op(0x21).u16(0).op(op).ret();
    return std::make_pair(t.run(s, "main", {a}), std::string());
  };
  CHECK(un(0xb2, F(2.7f)).first.result == I(2));
  CHECK(un(0xb2, F(-2.7f)).first.result == I(-2));
  CHECK(un(0xb2, F(3.0e9f)).first.result == I(-1294967296));
  CHECK(un(0xb1, I(16777217)).first.result == make_float_bits(0x4b800000));
  CHECK(un(0xb0, nothing()).first.result == Variant{kNone, 0});
  CHECK(un(0xb0, F(0.5f)).first.result_text == "0.500000");
  CHECK(un(0xb0, I(-42)).first.result_text == "-42");
  T t;
  Asm a;
  a.fn("main", 0).s("-2.25").op(0xb1).ret();
  CallResult r = t.run(a);
  CHECK(r.result == F(-2.25f));
  // trace order for 0xb1 on a 0x11 (trace.md §3): A s, P, F s
  std::vector<std::string> ev;
  for (const auto& l : t.tr.lines)
    if (l[0] == 'A' || l[0] == 'P' || l[0] == 'F') ev.push_back(l.substr(0, 1));
  CHECK(ev == std::vector<std::string>{"A", "P", "F"});
  CHECK(t.vm->fpu_pc() == 64);
}

TEST_CASE("osi vm 0xb8-0xbd 0xbf type tests (0xbf on a game handle = type + 0x10000)") {
  auto un = [](uint8_t op, std::function<void(Asm&)> push) {
    T t;
    Asm s;
    s.fn("main", 0);
    push(s);
    s.op(op).ret();
    t.reg.add(entry("t", "obj", 0, 0, {}, [](NativeCall& c, void*) { return Variant{0x27, c.vm.heap().new_game(0x27)}; }));
    CallResult r = t.run(s);
    return std::make_pair(r, r.ok ? std::string() : t.err());
  };
  CHECK(un(0xb8, [](Asm& a) { a.i8(1); }).first.result == I(1));
  CHECK(un(0xb9, [](Asm& a) { a.i8(1); }).first.result == I(0));
  CHECK(un(0xba, [](Asm& a) { a.s("x"); }).first.result == I(1));
  CHECK(un(0xbb, [](Asm& a) { a.cls("k", {}); a.op(0x13).u16(0); }).first.result == I(1));
  CHECK(un(0xbc, [](Asm& a) { a.call("t", "obj", 0); }).first.result == I(1));
  CHECK(un(0xbd, [](Asm& a) { a.op(0x90); }).first.result == I(1));
  CHECK(un(0xbf, [](Asm& a) { a.cls("k", {}); a.cls("j", {}); a.op(0x13).u16(1); }).first.result == I(1));
  CHECK(un(0xbf, [](Asm& a) { a.call("t", "obj", 0); }).first.result == I(0x10027));
  CHECK(un(0xbf, [](Asm& a) { a.i8(1); }).second == "Cant get classID of non object");
}

TEST_CASE("osi vm BCLUnused: Call of undefined instruction, PC not advanced") {
  for (uint8_t o : {0x01, 0x20, 0xfe}) {
    T t;
    Asm a;
    a.fn("main", 0).i8(1).label("u").op(o).ret();
    CHECK_FALSE(t.run(a).ok);
    CHECK(t.err() == "Call of undefined instruction");
    CHECK(t.vm->last_error()->pc == a.at("u"));
    CHECK(t.vm->last_error()->op == o);
  }
}

TEST_CASE("osi vm engine calls: prefix FindFunction, M line, exact argc, surplus disposed in index order, argc > 10") {
  T t;
  Asm a;
  a.fn("setup", 0).i8(1).ret();
  a.fn("two", 2).op(0x21).u16(1).ret();
  t.load(a);
  CallResult r = t.vm->call("SetupLevel_1", {});
  CHECK(r.found);
  CHECK(r.result == I(1));
  CallResult m = t.vm->call("set", {});
  CHECK_FALSE(m.found);
  CHECK(t.has("M set"));
  std::vector<Variant> args;
  for (int i = 0; i < 4; ++i) args.push_back(Variant{kString, t.heap.new_string("s")});
  t.tr.lines.clear();
  CallResult q = t.vm->call("two", args);
  CHECK(q.ok);
  CHECK(q.result.type == kString);
  auto f = t.starting("F ");
  // surplus a2, a3 before the first I (index order); 0x21 deep-copies a1 (handle 5); Return disposes a1 then a0 (top
  // down); the host disposes the result after R
  CHECK(f == std::vector<std::string>{"F s 00000003", "F s 00000004", "F s 00000002", "F s 00000001", "F s 00000005"});
  CHECK(t.tr.lines[1] == "F s 00000003");
  CHECK(t.heap.stats().live[kKindString] == 0);
  T u;
  Asm b;
  b.fn("main", 11).label("x").i8(1).ret();
  CHECK_FALSE(u.run(b).ok);
  CHECK(u.err() == "Too many args required (max 10) to call requested entry point.");
  CHECK(u.vm->last_error()->pc == b.at("x"));
  CHECK(u.vm->last_error()->op == 0x43);
  CHECK(u.vm->last_error()->depth == 1);
}

TEST_CASE("osi vm D10: error snapshot before reset, reset_after_error, fpu 64; stack cap by recursion") {
  T t;
  Asm a;
  a.fn("main2", 0).i8(2).ret();
  a.fn("main", 0).i8(1).i8(2).f(1.0f).label("x").rel(0x53, "x").ret();
  t.load(a);
  CallResult r = t.vm->call("main", {});
  CHECK_FALSE(r.ok);
  VmState s = t.vm->snapshot();
  CHECK(s.depth == 3);  // marker, 1, 2 (the float was popped)
  CHECK(s.context_depth == 1);
  CHECK(s.top == I(2));
  CHECK(s.fpu_pc == 64);
  t.vm->reset_after_error();
  s = t.vm->snapshot();
  CHECK(s.depth == 0);
  CHECK(s.context_depth == 0);
  CallResult q = t.vm->call("main2", {});
  CHECK(q.ok);
  CHECK(q.result == I(2));
  CHECK(t.vm->fpu_pc() == 64);
  // recursion through 0x50 until the cap: the error at depth exactly 1000
  T u;
  Asm b;
  b.fn("main", 0).label("rec").addr("rec").op(0x50).u8(0).ret();
  CHECK_FALSE(u.run(b).ok);
  CHECK(u.err() == "Stack overflow (1000 slots)");
  CHECK(u.vm->last_error()->depth == 1000);
  u.vm->reset_after_error();
  CHECK(u.vm->snapshot().depth == 0);
  // a Pop on an empty stack: the marker is popped by the first Pop, the second underflows
  T w;
  Asm c;
  c.fn("main", 0).pop().pop().ret();
  CHECK_FALSE(w.run(c).ok);
  CHECK(w.err() == "Stack underflow");
}

TEST_CASE("osi vm re-entrancy: a native calls vm.call; nested C..R; an inner error is re-reported by 0x82") {
  T t;
  Asm a;
  a.fn("inner", 0).i8(3).ret();
  a.fn("main", 0).op(0x23).u8(1).i8(44).op(0x22).u16(0).call("t", "reenter", 0).op(0x21).u16(0).op(0x70).ret();
  t.reg.add(entry("t", "reenter", 0, 0, {}, nat_reenter));
  CallResult r = t.run(a);
  CHECK_MESSAGE(r.ok, t.err());
  CHECK(r.result == I(45));  // the outer local survived
  CHECK(t.starting("C ").size() == 2);
  CHECK(t.starting("R ").size() == 2);
  CHECK(t.vm->snapshot().context_depth == 0);
  T u;
  Asm b;
  b.fn("inner", 0).f(1.0f).rel(0x53, "z").label("z").ret();
  b.fn("main", 0).label("c").call("t", "reenter", 0).ret();
  u.reg.add(entry("t", "reenter", 0, 0, {}, nat_reenter));
  CallResult q = u.run(b);
  CHECK_FALSE(q.ok);
  CHECK(u.err() == "Script is calling t.reenter");
  CHECK(u.vm->last_error()->pc == b.at("c"));
  CHECK(u.vm->last_error()->op == 0x82);
  CHECK(u.vm->snapshot().context_depth == 2);
  CHECK(u.starting("E ").size() == 1);
  u.vm->reset_after_error();
  CHECK(u.vm->snapshot().depth == 0);
  CHECK(u.vm->snapshot().context_depth == 0);
}

TEST_CASE("osi vm D11 typed stubs from the registry and sets_fpu_pc64") {
  NativeRegistry reg;
  NativeEntry e = entry("x", "mk", 0, 0, {}, nullptr);
  e.ret = RetKind::Game;
  e.ret_type = 0x42;
  e.tier = Tier::Stub;
  reg.add(e);
  NativeEntry s = entry("x", "str", 0, 0, {}, nullptr);
  s.ret = RetKind::String;
  s.tier = Tier::Stub;
  s.sets_fpu_pc64 = true;
  reg.add(s);
  NativeEntry c = entry("x", "cp", 1, 1, {kInt}, nullptr);
  c.ret = RetKind::Copy;
  reg.add(c);
  NativeEntry d = entry("x", "def", 0, 0, {}, nullptr);
  d.ret = RetKind::Int;
  d.default_ret = make_int(-1);
  reg.add(d);
  T t;
  t.reg = reg;
  Asm a;
  a.fn("main", 0).call("x", "mk", 0).op(0xbf).call("x", "str", 0).op(0xba).op(0x70).i8(6).call("x", "cp", 1).op(0x70)
      .call("x", "def", 0).op(0x70).ret();
  CallResult r = t.run(a);
  CHECK_MESSAGE(r.ok, t.err());
  CHECK(r.result == I(0x10042 + 1 + 6 - 1));
  CHECK(t.has("A g 00000001 42"));
  CHECK(t.vm->fpu_pc() == 64);
  CHECK(t.starting("P ").size() == 1);
}

// ============================== fix round 1 (blind review I1-I3, the FindMemberID ruling) ==============================

TEST_CASE("osi vm fix1 I1: atoi is _strtol(s, 0, 10) and SATURATES (0x65cde0 -> 0x65cd40, clamp 0x65cd95-0x65cdbb)") {
  CHECK(osi_atoi("3000000000") == 2147483647);
  CHECK(osi_atoi("-2147483649") == INT32_MIN);
  CHECK(osi_atoi("2147483648") == 2147483647);
  CHECK(osi_atoi("-2147483648") == INT32_MIN);
  CHECK(osi_atoi("2147483647") == 2147483647);
  CHECK(osi_atoi("99999999999999999999999") == 2147483647);
  CHECK(osi_atoi("  -99999999999999999999x") == INT32_MIN);
  // through the VM: 0xb2 on a string (row 0xb2: 0x11/0x16 -> atoi)
  T t;
  Asm a;
  a.fn("main", 0).s("3000000000").op(0xb2).ret();
  CallResult r = t.run(a);
  CHECK(r.ok);
  CHECK(r.result == I(2147483647));
}

TEST_CASE("osi vm fix1 I2: object dtor frees props back to front (0x6085cd-0x6085e3), exact F sequence") {
  T t;
  Asm a;
  a.cls("k", {"p0", "p1"});
  // o = new k; o.p0 = "a" (string 1 after 0xb0 copy...); keep it simple: dynamic strings via 0xb0 of 0x11
  a.fn("main", 0).op(0x13).u16(0)                                  // o                       (A o 1)
      .s("x").op(0xb0).op(0x33).u8(2).op(0x10).u16(a.sym("p0"))     // o.p0 = "x" (s1)         -> o
      .s("y").op(0xb0).op(0x33).u8(2).op(0x10).u16(a.sym("p1"))     // o.p1 = "y" (s2)         -> o
      .pop().nothing().ret();                                        // Pop disposes o
  CallResult r = t.run(a);
  REQUIRE_MESSAGE(r.ok, t.err());
  CHECK(t.starting("F ") == std::vector<std::string>{"F o 00000001", "F s 00000002", "F s 00000001"});
}

TEST_CASE("osi vm fix1 I2: a nested engine call's marker is the PC after the invoking 0x82 (site+6, 0x6015c6-0x601780)") {
  T t;
  Asm a;
  a.fn("inner", 0).label("in").i8(3).ret();
  a.fn("main", 0).label("site").call("t", "reenter", 0).ret();
  t.reg.add(entry("t", "reenter", 0, 0, {}, nat_reenter));
  CallResult r = t.run(a);
  REQUIRE_MESSAGE(r.ok, t.err());
  char want[64];
  std::snprintf(want, sizeof want, "I %08x 43    2    2 00ff:%08x", a.at("in"), a.at("site") + 6);
  CHECK(t.has(want));
}

TEST_CASE("osi vm fix1 I2: 0x60 on two strings disposes a then b (0x6052ae/0x6052b9 order, string path 0x6053f6/0x605401)") {
  T t;
  Asm a;
  a.fn("main", 0).s("ABC").s("abc").op(0x60).ret();
  CallResult r = t.run(a);
  REQUIRE(r.ok);
  CHECK(r.result == I(1));
  CHECK(t.starting("F ") == std::vector<std::string>{"F s 00000001", "F s 00000002"});
  T u;
  Asm b;
  b.cls("k", {});
  b.fn("main", 0).op(0x13).u16(0).op(0x13).u16(0).op(0x60).ret();  // two distinct objects: a (o1) then b (o2)
  CallResult q = u.run(b);
  REQUIRE(q.ok);
  CHECK(q.result == I(0));
  CHECK(u.starting("F ") == std::vector<std::string>{"F o 00000001", "F o 00000002"});
}

TEST_CASE("osi vm fix1 I3: heap storage is bounded by the live values, ids stay monotonic (D7)") {
  StringTrace tr;
  Heap h(&tr);
  for (int i = 0; i < 10000; ++i) {
    uint32_t s = h.new_string("x");
    uint32_t o = h.new_object(0, 1);
    uint32_t a = h.new_array(2);
    uint32_t g = h.new_game(0x27);
    h.dispose(Variant{kString, s});
    h.dispose(Variant{kObject, o});
    h.dispose(Variant{kArray, a});
    h.dispose(Variant{0x27, g});
  }
  CHECK(h.stored() == 0);
  CHECK(h.new_string("y") == 10001);  // never reused
  CHECK(h.stored() == 1);
  CHECK(h.stats().allocs[kKindString] == 10001);
}

TEST_CASE("osi vm fix1 M8: dispose of a deep chain is iterative (no recursion) and keeps the back-to-front order") {
  Heap h;
  uint32_t head = h.new_array(0);
  uint32_t cur = head;
  for (int i = 0; i < 200000; ++i) {  // a 200,000-deep chain of arrays: recursion would overflow the native stack
    uint32_t nx = h.new_array(0);
    h.elements(cur).push_back(Variant{kArray, nx});
    cur = nx;
  }
  h.dispose(Variant{kArray, head});
  CHECK(h.stats().live[kKindArray] == 0);
  CHECK(h.stored() == 0);
}

TEST_CASE("osi vm fix1: FindMemberID miss-cache poisoning (0x609ca7-0x609cc8) and the 51st-insert overflow") {
  T t;
  Asm a;
  a.cls("k", {"hp", "mp"});  // symbols: hp (0), mp (1); a miss compares both, so it caches {"mp", 0xffff}
  a.fn("okhp", 0).op(0x13).u16(0).s("HP").op(0x18).ret();
  a.fn("okmp", 0).op(0x13).u16(0).s("MP").op(0x18).ret();
  a.fn("miss", 0).op(0x13).u16(0).s("nope").op(0x18).ret();
  t.load(a);
  // a fresh VM: "mp" resolves
  CallResult r0 = t.vm->call("okmp", {});
  CHECK(r0.ok);
  // a new VM on the same script: miss first, then "mp" hits the poisoned entry
  T u;
  u.load(a);
  CHECK_FALSE(u.vm->call("miss", {}).ok);
  CHECK(u.err() == "Reference to non-existant member property \"nope\"");
  u.vm->reset_after_error();
  CHECK_FALSE(u.vm->call("okmp", {}).ok);
  CHECK(u.err() == "Reference to non-existant member property \"MP\"");
  u.vm->reset_after_error();
  CallResult r2 = u.vm->call("okhp", {});  // hp was never poisoned
  CHECK(r2.ok);
  // inserts so far: miss (1), okmp hit the cache (0), okhp (1) = 2; 48 more misses fill 50 entries, the next overflows
  for (int i = 0; i < 48; ++i) {
    CHECK_FALSE(u.vm->call("miss", {}).ok);
    CHECK(u.err() == "Reference to non-existant member property \"nope\"");
    u.vm->reset_after_error();
  }
  CHECK_FALSE(u.vm->call("miss", {}).ok);
  CHECK(u.err() == "Undefined behaviour: FindMemberID cache overflow (51st miss)");
}

TEST_CASE("osi vm fix1 M5/M7: JSON text is latin-1; reset_after_error never throws") {
  Json j;
  std::string err;
  REQUIRE(parse_json("\"a\\u00e9\\u0000\"", j, &err));
  CHECK(j.str == std::string("a\xe9\0", 3));
  CHECK_FALSE(parse_json("\"\\u0100\"", j, &err));
  REQUIRE(parse_json("\"\xff\x80\"", j, &err));
  CHECK(j.str == "\xff\x80");
  // M7: a fabricated (dangling) handle left on the stack by an error is dropped by the reset, not thrown
  T t;
  Asm a;
  a.fn("main", 1).f(1.0f).rel(0x53, "z").label("z").ret();
  t.load(a);
  CHECK_FALSE(t.vm->call("main", {Variant{kString, 999}}).ok);
  bool threw = false;
  try {
    t.vm->reset_after_error();
  } catch (...) {
    threw = true;
  }
  CHECK_FALSE(threw);
  CHECK(t.vm->snapshot().depth == 0);
  REQUIRE(t.vm->internal_errors().size() == 1);
  CHECK(t.vm->internal_errors()[0] == "reset_after_error: Undefined: dangling string handle");
}
