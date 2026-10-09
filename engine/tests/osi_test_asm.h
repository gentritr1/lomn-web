// Test-only OSI 4.1 assembler shared by test_osi_vm.cpp (M2a-1 T1, moved here unchanged in M2a-2 T3) and
// test_osi_natives.cpp: labels, functions, classes, symbols and globals, with the layout lomn/sage/osi.h reads.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "lomn/osi/variant.h"
#include "osi_builder.h"

namespace lomn::test {

inline uint32_t B(float f) { return osi::f32_bits(f); }

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

}  // namespace lomn::test
