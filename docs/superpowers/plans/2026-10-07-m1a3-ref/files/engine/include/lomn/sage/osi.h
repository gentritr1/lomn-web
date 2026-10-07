// OSI (compiled SAGE script, data/Base.osi) - a bounded TABLE reader plus a straight-line literal finder, enough to
// recover the constant arguments that area scripts pass to engine functions (M1a-3: GcAreaDirector::SetFog,
// GcLightDirector::SetCaveLighting). It is not a VM (M2).
//
// Layout (version 4.1; field order cross-checked against the public SAGESharp reader, see REPORT for the URL):
//   "OSI\0", u16 major, u16 minor, string table, global table, function table {u8 len, name, u32 bytecode offset,
//   u16 params}, class table {u8 nprops, nprops x u16, u8 nmethods, nmethods x {u16 symbol, u32 bytecode offset}},
//   class names, symbol table, source table (version 4). String-like tables: u16 count, then {u8 len, chars, NUL}.
// Bytecode: u8 opcode + fixed operands (see osi.cpp). A subroutine is walked linearly until a Return that lies past the
// furthest forward branch target.
// Literal evaluation (find_calls): a stack of literal values; push-constant opcodes push, Add/Subtract/Multiply/Divide
// fold numbers, Pop/PopN drop, CallGameFunction pops its argc arguments (recording them) and pushes an unknown result.
// Every other opcode, every branch and every branch target CLEARS the stack (values become unknown), so a reported
// literal is always one computed in straight-line code right before the call.
// Untrusted input (spec §5): every read bounds-checked; table counts bounded by the remaining bytes; file <= 64 MiB;
// at most kMaxOsiInstructions per subroutine.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace lomn::sage {

constexpr uint32_t kMaxOsiInstructions = 1u << 20;
constexpr uint64_t kMaxOsiBytes = 64ull << 20;

struct OsiValue {
  enum Kind { Unknown, Int, Float, String } kind = Unknown;
  int64_t i = 0;
  float f = 0;
  std::string s;
  bool number(double& out) const;
};

struct OsiCall {
  std::string cls, fn;          // as stored (the game lower-cases names)
  std::vector<OsiValue> args;   // argc values, in source order
  uint32_t offset = 0;          // file offset of the CallGameFunction instruction
};

struct OsiMethod {
  std::string name;
  uint32_t offset = 0;
};
struct OsiClass {
  std::string name;
  std::vector<OsiMethod> methods;
};

class OsiFile {
 public:
  bool parse(std::vector<uint8_t> bytes, std::string* err);
  uint16_t major() const { return major_; }
  uint16_t minor() const { return minor_; }
  const std::vector<std::string>& strings() const { return strings_; }
  const std::vector<OsiClass>& classes() const { return classes_; }
  const OsiClass* find_class(const std::string& name) const;  // case-insensitive
  // Every CallGameFunction in class.method (case-insensitive names), with literal arguments where recoverable.
  bool find_calls(const std::string& cls, const std::string& method, std::vector<OsiCall>& out, std::string* err) const;
  // Disassembles every function and method (bounds/opcode check of the whole file); returns the instruction count.
  bool check_all(uint64_t* instructions, std::string* err) const;

 private:
  struct Insn {
    uint32_t at = 0;
    uint8_t op = 0;
    int64_t a = 0, b = 0, c = 0;
    float f = 0;
  };
  bool disasm(uint32_t start, std::vector<Insn>& out, std::string* err) const;
  std::vector<uint8_t> d_;
  uint16_t major_ = 0, minor_ = 0;
  std::vector<std::string> strings_, globals_, symbols_, sources_;
  std::vector<std::pair<std::string, uint32_t>> functions_;
  std::vector<OsiClass> classes_;
};

}  // namespace lomn::sage
