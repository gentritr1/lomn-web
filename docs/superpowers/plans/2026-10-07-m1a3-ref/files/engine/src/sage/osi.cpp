#include "lomn/sage/osi.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

#include "bytes.h"

namespace lomn::sage {
namespace {

using detail::Cursor;
using detail::le16;
using detail::le32;

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  return s;
}
bool fail(std::string* err, const std::string& m) {
  if (err) *err = "osi: " + m;
  return false;
}

// Operand encodings: 'B' u8, 'b' s8, 'H' u16, 'h' s16, 'T' s24, 'i' s32, 'I' u32, 'f' f32. Unlisted opcodes take
// none; opcodes in neither table are rejected.
const char* operands(uint8_t op) {
  switch (op) {
    case 0x04: case 0x05: case 0x06: return "HH";  // LineNumber variants
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x15: case 0x16: case 0x17: return "H";
    case 0x14: return "b";
    case 0x21: case 0x22: case 0x24: case 0x25: return "H";
    case 0x23: case 0x31: case 0x33: case 0x34: return "b";
    case 0x40: return "i";
    case 0x41: return "T";
    case 0x42: return "h";
    case 0x43: return "b";
    case 0x44: return "f";
    case 0x46: return "H";
    case 0x48: return "I";
    case 0x49: return "H";
    case 0x50: case 0x51: return "b";
    case 0x53: case 0x54: return "h";
    case 0x80: case 0x81: return "HH";
    case 0x82: return "HHb";
    case 0x83: return "Hb";
    case 0x84: return "ib";
    default: return nullptr;
  }
}
bool no_operand(uint8_t op) {
  static const std::set<uint8_t> k = {0x00, 0x02, 0x03, 0x18, 0x19, 0x1A, 0x30, 0x32, 0x35, 0x45, 0x47, 0x52, 0x60, 0x61, 0x62,
                                      0x63, 0x64, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x6F, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76,
                                      0x77, 0x78, 0x7A, 0x7B, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0xA0, 0xA1, 0xA2, 0xA3,
                                      0xA4, 0xA5, 0xA6, 0xA7, 0xB0, 0xB1, 0xB2, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBF, 0xFF};
  return k.count(op) != 0;
}

bool read_strings(Cursor& c, std::vector<std::string>& out) {
  uint16_t n = 0;
  if (!c.u16(n) || uint64_t(n) * 2 > c.remaining()) return false;
  out.clear();
  for (uint32_t i = 0; i < n; ++i) {
    uint8_t len = 0;
    std::string s;
    if (!c.u8(len) || !c.str(len, s) || !c.skip(1)) return false;
    out.push_back(std::move(s));
  }
  return true;
}

}  // namespace

bool OsiValue::number(double& out) const {
  if (kind == Int) out = static_cast<double>(i);
  else if (kind == Float) out = f;
  else return false;
  return true;
}

bool OsiFile::parse(std::vector<uint8_t> bytes, std::string* err) {
  *this = OsiFile();
  if (parse_tables(std::move(bytes), err)) return true;
  *this = OsiFile();  // no partial state after an error
  return false;
}

bool OsiFile::parse_tables(std::vector<uint8_t> bytes, std::string* err) {
  if (bytes.size() > kMaxOsiBytes) return fail(err, "file too large");
  d_ = std::move(bytes);
  Cursor c(d_.data(), d_.size());
  std::string magic;
  if (!c.str(4, magic) || magic != std::string("OSI\0", 4)) return fail(err, "bad magic");
  if (!c.u16(major_) || !c.u16(minor_)) return fail(err, "truncated header");
  if (major_ != 4) return fail(err, "unsupported version " + std::to_string(major_) + "." + std::to_string(minor_));
  if (!read_strings(c, strings_) || !read_strings(c, globals_)) return fail(err, "truncated string/global table");
  uint16_t nf = 0;
  if (!c.u16(nf) || uint64_t(nf) * 7 > c.remaining()) return fail(err, "bad function table");
  for (uint32_t i = 0; i < nf; ++i) {
    uint8_t len = 0;
    std::string name;
    uint32_t off = 0;
    uint16_t np = 0;
    if (!c.u8(len) || !c.str(len, name) || !c.u32(off) || !c.u16(np)) return fail(err, "truncated function table");
    functions_.push_back({name, off});
  }
  uint16_t nc = 0;
  if (!c.u16(nc) || uint64_t(nc) * 2 > c.remaining()) return fail(err, "bad class table");
  classes_.resize(nc);
  std::vector<uint16_t> method_sym;  // symbol index per method, in class order; resolved below
  for (uint32_t i = 0; i < nc; ++i) {
    uint8_t np = 0, nm = 0;
    if (!c.u8(np) || !c.skip(uint64_t(np) * 2) || !c.u8(nm) || uint64_t(nm) * 6 > c.remaining()) return fail(err, "truncated class");
    for (uint32_t m = 0; m < nm; ++m) {
      uint16_t sym = 0;
      uint32_t off = 0;
      if (!c.u16(sym) || !c.u32(off)) return fail(err, "truncated method");
      classes_[i].methods.push_back({std::string(), off});
      method_sym.push_back(sym);
    }
  }
  for (uint32_t i = 0; i < nc; ++i) {
    uint8_t len = 0;
    if (!c.u8(len) || !c.str(len, classes_[i].name) || !c.skip(1)) return fail(err, "truncated class names");
  }
  if (!read_strings(c, symbols_) || !read_strings(c, sources_)) return fail(err, "truncated symbol/source table");
  size_t mi = 0;
  for (OsiClass& k : classes_)
    for (OsiMethod& m : k.methods) {
      const uint16_t sym = method_sym[mi++];
      if (sym >= symbols_.size()) return fail(err, "method symbol out of range");
      m.name = symbols_[sym];
      if (m.offset >= d_.size()) return fail(err, "method offset outside the file");
    }
  for (const auto& f : functions_)
    if (f.second >= d_.size()) return fail(err, "function offset outside the file");
  return true;
}

const OsiClass* OsiFile::find_class(const std::string& name) const {
  const std::string n = lower(name);
  for (const OsiClass& c : classes_)
    if (lower(c.name) == n) return &c;
  return nullptr;
}

bool OsiFile::disasm(uint32_t start, std::vector<Insn>& out, std::string* err) const {
  out.clear();
  Cursor c(d_.data(), d_.size(), start);
  uint64_t last_target = 0;
  for (uint32_t n = 0; n < kMaxOsiInstructions; ++n) {
    Insn in;
    in.at = static_cast<uint32_t>(c.pos());
    if (!c.u8(in.op)) return fail(err, "bytecode runs past the end at " + std::to_string(in.at));
    const char* ops = operands(in.op);
    if (!ops && !no_operand(in.op)) return fail(err, "unknown opcode " + std::to_string(in.op) + " at " + std::to_string(in.at));
    int64_t* slot[3] = {&in.a, &in.b, &in.c};
    for (int k = 0; ops && ops[k]; ++k) {
      int64_t v = 0;
      switch (ops[k]) {
        case 'b': {
          uint8_t x;
          if (!c.u8(x)) return fail(err, "truncated operand");
          v = static_cast<int8_t>(x);
          break;
        }
        case 'H': case 'h': {
          uint16_t x;
          if (!c.u16(x)) return fail(err, "truncated operand");
          v = ops[k] == 'h' ? static_cast<int16_t>(x) : x;
          break;
        }
        case 'T': {
          uint8_t b0, b1, b2;
          if (!c.u8(b0) || !c.u8(b1) || !c.u8(b2)) return fail(err, "truncated operand");
          v = b0 | (b1 << 8) | (b2 << 16);
          if (v & 0x800000) v -= 0x1000000;
          break;
        }
        case 'i': case 'I': {
          uint32_t x;
          if (!c.u32(x)) return fail(err, "truncated operand");
          v = ops[k] == 'i' ? static_cast<int32_t>(x) : x;
          break;
        }
        case 'f': {
          uint32_t x;
          if (!c.u32(x)) return fail(err, "truncated operand");
          std::memcpy(&in.f, &x, 4);
          break;
        }
      }
      if (k < 3) *slot[k] = v;
    }
    out.push_back(in);
    if (in.op == 0x53 || in.op == 0x54) last_target = std::max<uint64_t>(last_target, uint64_t(int64_t(c.pos()) + in.a));
    else if (in.op == 0x52 && c.pos() > last_target) return true;
  }
  return fail(err, "subroutine exceeds kMaxOsiInstructions");
}

bool OsiFile::check_all(uint64_t* count, std::string* err, uint64_t* calls) const {
  uint64_t n = 0, nc = 0;
  std::vector<Insn> ins;
  std::set<uint32_t> seen;  // functions and methods that share bytecode are walked once
  auto walk = [&](uint32_t off) {
    if (!seen.insert(off).second) return true;
    if (!disasm(off, ins, err)) return false;
    n += ins.size();
    for (const Insn& in : ins) nc += in.op == 0x82;
    if (n > kMaxOsiTotalInstructions) return fail(err, "file exceeds kMaxOsiTotalInstructions");
    if (nc > kMaxOsiTotalCalls) return fail(err, "file exceeds kMaxOsiTotalCalls");
    return true;
  };
  for (const auto& f : functions_)
    if (!walk(f.second)) return false;
  for (const OsiClass& k : classes_)
    for (const OsiMethod& m : k.methods)
      if (!walk(m.offset)) return false;
  if (count) *count = n;
  if (calls) *calls = nc;
  return true;
}

bool OsiFile::eval_calls(const std::string& cls, const std::string& method, std::vector<OsiCall>& out, std::string* err) const {
  out.clear();
  if (eval_impl(cls, method, out, err)) return true;
  out.clear();  // no partial list after an error
  return false;
}

bool OsiFile::eval_impl(const std::string& cls, const std::string& method, std::vector<OsiCall>& out, std::string* err) const {
  const OsiClass* k = find_class(cls);
  if (!k) return fail(err, "no class " + cls);
  const OsiMethod* m = nullptr;
  for (const OsiMethod& x : k->methods)
    if (lower(x.name) == lower(method)) m = &x;
  if (!m) return fail(err, "no method " + cls + "." + method);
  std::vector<Insn> ins;
  if (!disasm(m->offset, ins, err)) return false;
  std::set<uint64_t> targets;
  for (size_t i = 0; i < ins.size(); ++i)
    if (ins[i].op == 0x53 || ins[i].op == 0x54) {
      const uint64_t next = i + 1 < ins.size() ? ins[i + 1].at : ins[i].at + 3;
      targets.insert(uint64_t(int64_t(next) + ins[i].a));
    }
  std::vector<OsiValue> st;
  uint64_t args_total = 0, string_bytes = 0;
  auto pop = [&]() {
    if (st.empty()) return OsiValue();
    OsiValue v = st.back();
    st.pop_back();
    return v;
  };
  auto str = [&](int64_t idx) {
    OsiValue v;
    if (idx >= 0 && static_cast<size_t>(idx) < strings_.size()) {
      v.kind = OsiValue::String;
      v.s = strings_[static_cast<size_t>(idx)];
    }
    return v;
  };
  for (const Insn& in : ins) {
    if (targets.count(in.at)) st.clear();
    OsiValue v;
    switch (in.op) {
      case 0x40: case 0x41: case 0x42: case 0x43: v.kind = OsiValue::Int; v.i = in.a; st.push_back(v); break;
      case 0x45: v.kind = OsiValue::Int; v.i = 0; st.push_back(v); break;
      case 0x44: v.kind = OsiValue::Float; v.f = in.f; st.push_back(v); break;
      case 0x46: st.push_back(str(in.a)); break;
      case 0x48: v.kind = OsiValue::Int; v.i = in.a; st.push_back(v); break;
      case 0x47: st.push_back(OsiValue()); break;
      case 0x04: case 0x05: case 0x06: case 0x00: break;  // line numbers, nop
      case 0x30: pop(); break;
      case 0x31: for (int64_t j = 0; j < in.a; ++j) pop(); break;
      case 0x70: case 0x71: case 0x72: case 0x73: {
        const OsiValue b = pop(), a = pop();
        double x, y;
        OsiValue r;
        if (a.number(x) && b.number(y)) {
          const bool fl = a.kind == OsiValue::Float || b.kind == OsiValue::Float || in.op == 0x73;
          double z = in.op == 0x70 ? x + y : in.op == 0x71 ? x - y : in.op == 0x72 ? x * y : (y != 0 ? x / y : 0);
          if (fl) {
            r.kind = OsiValue::Float;
            r.f = static_cast<float>(z);
          } else if (std::isfinite(z) && z >= -9.0e18 && z <= 9.0e18) {  // int64 range: the conversion stays defined
            r.kind = OsiValue::Int;
            r.i = static_cast<int64_t>(z);
          }
        }
        st.push_back(r);
        break;
      }
      case 0x82: {
        if (in.c < 0 || in.c > static_cast<int64_t>(kMaxOsiArgc)) return fail(err, "CallGameFunction argc out of range");
        if (out.size() >= kMaxOsiCallsPerMethod) return fail(err, "method exceeds kMaxOsiCallsPerMethod");
        args_total += static_cast<uint64_t>(in.c);
        if (args_total > kMaxOsiArgsPerMethod) return fail(err, "method exceeds kMaxOsiArgsPerMethod");
        OsiCall call;
        call.cls = in.a >= 0 && static_cast<size_t>(in.a) < strings_.size() ? strings_[static_cast<size_t>(in.a)] : "?";
        call.fn = in.b >= 0 && static_cast<size_t>(in.b) < strings_.size() ? strings_[static_cast<size_t>(in.b)] : "?";
        call.offset = in.at;
        call.args.resize(static_cast<size_t>(in.c));
        for (int64_t j = in.c - 1; j >= 0; --j) {
          call.args[static_cast<size_t>(j)] = pop();
          string_bytes += call.args[static_cast<size_t>(j)].s.size();
        }
        if (string_bytes > kMaxOsiStringArgBytes) return fail(err, "method exceeds kMaxOsiStringArgBytes");
        out.push_back(std::move(call));
        st.push_back(OsiValue());  // the call's result
        break;
      }
      default: st.clear(); break;  // any other effect is not modelled: forget every value
    }
    if (st.size() > kMaxOsiStackDepth) return fail(err, "literal stack exceeds kMaxOsiStackDepth");
  }
  return true;
}

}  // namespace lomn::sage
