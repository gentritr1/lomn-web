// Test-only OSI 4.1 writer (lomn/sage/osi.h layout): one class with one method "initfull", optional functions that
// all point at the method's bytecode (or at offsets inside it), and the program the synthetic Base.osi carries
// (lomn/view/synthetic.cpp writes the same bytes for the A9 scene). Shared by test_sage_osi.cpp and probe_osi.cpp so
// the OSI reader's tests need nothing above lomn_sage.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace lomn::test {

struct OsiSpec {
  std::vector<std::string> strings = {"gcareadirector", "setfog", "gclightdirector", "setcavelighting"};
  std::string cls = "lego_area_statt";
  std::vector<uint8_t> code;               // the method's bytecode
  uint32_t functions = 0;                  // function-table entries
  uint32_t function_stride = 0;            // function k starts at code + k * stride (0: all share the method's code)
  // Task 8B C2: extra classes after lego_area_statt (empty names, no properties), each with extra_methods methods whose
  // symbol is extra_symbol (symbol 1) and whose bytecode is the method's code. 0 = none.
  uint32_t extra_classes = 0;
  uint8_t extra_methods = 0;
  std::string extra_symbol = "m";
};

inline void osi_put16(std::vector<uint8_t>& b, uint32_t v) {
  b.push_back(static_cast<uint8_t>(v));
  b.push_back(static_cast<uint8_t>(v >> 8));
}
inline void osi_put32(std::vector<uint8_t>& b, uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

inline std::vector<uint8_t> build_osi(const OsiSpec& s) {
  std::vector<uint8_t> b = {'O', 'S', 'I', 0};
  osi_put16(b, 4);
  osi_put16(b, 1);
  auto table = [&](const std::vector<std::string>& v) {
    osi_put16(b, static_cast<uint32_t>(v.size()));
    for (const std::string& x : v) {
      b.push_back(static_cast<uint8_t>(x.size()));
      b.insert(b.end(), x.begin(), x.end());
      b.push_back(0);
    }
  };
  table(s.strings);
  table({});  // globals
  osi_put16(b, s.functions);
  std::vector<size_t> fpatch;
  for (uint32_t k = 0; k < s.functions; ++k) {
    b.push_back(1);
    b.push_back('f');
    fpatch.push_back(b.size());
    osi_put32(b, 0);
    osi_put16(b, 0);
  }
  osi_put16(b, 1 + s.extra_classes);  // class 0: no properties, one method: symbol 0 = "initfull"
  b.push_back(0);
  b.push_back(1);
  osi_put16(b, 0);
  const size_t mpatch = b.size();
  osi_put32(b, 0);
  std::vector<size_t> xpatch;  // extra classes' method offsets
  xpatch.reserve(size_t(s.extra_classes) * s.extra_methods);
  for (uint32_t k = 0; k < s.extra_classes; ++k) {
    b.push_back(0);
    b.push_back(s.extra_methods);
    for (uint32_t m = 0; m < s.extra_methods; ++m) {
      osi_put16(b, 1);
      xpatch.push_back(b.size());
      osi_put32(b, 0);
    }
  }
  b.push_back(static_cast<uint8_t>(s.cls.size()));
  b.insert(b.end(), s.cls.begin(), s.cls.end());
  b.push_back(0);
  for (uint32_t k = 0; k < s.extra_classes; ++k) {
    b.push_back(0);  // empty class name + NUL
    b.push_back(0);
  }
  table(s.extra_classes ? std::vector<std::string>{"initfull", s.extra_symbol} : std::vector<std::string>{"initfull"});
  table({});  // sources
  const uint32_t code = static_cast<uint32_t>(b.size());
  auto patch = [&](size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = static_cast<uint8_t>(v >> (8 * i));
  };
  patch(mpatch, code);
  for (size_t at : xpatch) patch(at, code);
  for (uint32_t k = 0; k < s.functions; ++k) patch(fpatch[k], code + k * s.function_stride);
  b.insert(b.end(), s.code.begin(), s.code.end());
  return b;
}

// The synthetic scene's InitFull: SetFog(0xff808080, 0 - 5, 0 - 50, 0.1f); SetCaveLighting(1).
inline std::vector<uint8_t> osi_statt_program() {
  std::vector<uint8_t> c;
  auto f32 = [&](float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    osi_put32(c, x);
  };
  c.push_back(0x40);
  osi_put32(c, 0xff808080u);  // PushConstanti32
  c.push_back(0x45);          // PushConstant0
  c.push_back(0x44);
  f32(5.0f);
  c.push_back(0x71);          // Subtract -> -5
  c.push_back(0x45);
  c.push_back(0x44);
  f32(50.0f);
  c.push_back(0x71);          // -> -50
  c.push_back(0x44);
  f32(0.1f);
  c.insert(c.end(), {0x82, 0, 0, 1, 0, 4});  // CallGameFunction gcareadirector.setfog/4
  c.push_back(0x30);                         // Pop
  c.insert(c.end(), {0x43, 1});              // PushConstanti8 1
  c.insert(c.end(), {0x82, 2, 0, 3, 0, 1});  // CallGameFunction gclightdirector.setcavelighting/1
  c.push_back(0x30);
  c.push_back(0x47);  // PushNothing
  c.push_back(0x52);  // Return
  return c;
}

inline std::vector<uint8_t> statt_osi() {
  OsiSpec s;
  s.code = osi_statt_program();
  return build_osi(s);
}

}  // namespace lomn::test
