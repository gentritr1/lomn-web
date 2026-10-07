// lomn_osi_dump (M1a-3 Task 8A C1): every class.method of a Base.osi through lomn::sage::OsiFile::eval_calls, one line
// per pair, in class-table order, as canonical text that tools/research/osi.py --compare-all checks against the
// independent Python evaluator (ctest osi_all_methods, native, LOMN_DATA only). The output is derived from game data:
// write it under build/, never commit it.
//
//   lomn_osi_dump Base.osi > dump.txt
//
// Line: "<class>.<method>\t<call> | <call> ..." (names lower-cased), call = "<cls>.<fn>(<arg>,<arg>...)" (lower-cased);
// arg = "i:<decimal int64>" | "f:<float32 bits, 8 lower-case hex>" | "f:nan" | "s:<bytes as hex>" | "?" (unknown);
// a method whose evaluation fails prints "ERROR". The same format is produced by osi.py _arg_text / dump_all.
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "lomn/sage/osi.h"

namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  return s;
}

std::string arg_text(const lomn::sage::OsiValue& v) {
  char b[32];
  switch (v.kind) {
    case lomn::sage::OsiValue::Int:
      std::snprintf(b, sizeof b, "i:%" PRId64, v.i);
      return b;
    case lomn::sage::OsiValue::Float: {
      if (v.f != v.f) return "f:nan";
      uint32_t bits;
      std::memcpy(&bits, &v.f, 4);
      std::snprintf(b, sizeof b, "f:%08x", bits);
      return b;
    }
    case lomn::sage::OsiValue::String: {
      std::string out = "s:";
      for (unsigned char c : v.s) {
        std::snprintf(b, sizeof b, "%02x", c);
        out += b;
      }
      return out;
    }
    default:
      return "?";
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: lomn_osi_dump Base.osi\n");
    return 2;
  }
  std::ifstream f(argv[1], std::ios::binary);
  if (!f) {
    std::fprintf(stderr, "lomn_osi_dump: cannot open %s\n", argv[1]);
    return 3;
  }
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  lomn::sage::OsiFile o;
  std::string err;
  if (!o.parse(std::move(bytes), &err)) {
    std::fprintf(stderr, "lomn_osi_dump: %s\n", err.c_str());
    return 3;
  }
  std::vector<lomn::sage::OsiCall> calls;
  for (const lomn::sage::OsiClass& k : o.classes())
    for (const lomn::sage::OsiMethod& m : k.methods) {
      std::string line = lower(k.name) + "." + lower(o.method_name(m)) + "\t";
      if (!o.eval_calls(k.name, o.method_name(m), calls, &err)) {
        line += "ERROR";
      } else {
        for (size_t c = 0; c < calls.size(); ++c) {
          if (c) line += " | ";
          line += lower(calls[c].cls) + "." + lower(calls[c].fn) + "(";
          for (size_t a = 0; a < calls[c].args.size(); ++a) line += (a ? "," : "") + arg_text(calls[c].args[a]);
          line += ")";
        }
      }
      std::fwrite(line.data(), 1, line.size(), stdout);
      std::fputc('\n', stdout);
    }
  return 0;
}
