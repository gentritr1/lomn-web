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
// Literal evaluation (eval_calls; the Python oracle's osi.eval_calls is the same evaluator): a stack of literal values;
// push-constant opcodes push, Add/Subtract/Multiply/Divide fold numbers, Pop/PopN drop, CallGameFunction pops its argc
// arguments (recording them) and pushes an unknown result. Every other opcode, every branch and every branch target
// CLEARS the stack (values become unknown), so a reported literal is always one computed in straight-line code right
// before the call. (Not to be confused with osi.py's find_plain_push_calls / --plain-calls, which only decodes the argc
// instructions right before a call when they are plain pushes: a computed argument such as 0 - 40 reads as None there.)
// Untrusted input (spec section 5, M1a-1 audit rules): every read bounds-checked; table counts bounded by the remaining
// bytes; every attacker-controlled count below is capped from a value MEASURED on the real Base.osi (Beta 1.5.0, 486,274
// bytes; tools/research/osi_maxima.py) with >= 16x headroom where the format allows it. A cap hit is an error: parse() leaves
// an empty OsiFile, eval_calls() an empty list.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace lomn::sage {

// Caps (measured maximum on the real Base.osi -> cap, headroom).
constexpr uint64_t kMaxOsiBytes = 8ull << 20;               // file: 486,274 bytes -> 8 MiB (17x)
constexpr uint32_t kMaxOsiInstructions = 1u << 15;          // per subroutine: 1,707 -> 32,768 (19x)
constexpr uint64_t kMaxOsiTotalInstructions = 1ull << 22;   // check_all, unique subroutines: 151,522 -> 4,194,304 (27x)
constexpr uint64_t kMaxOsiTotalCalls = 1ull << 17;          // check_all, CallGameFunction per file: 4,402 -> 131,072 (29x)
constexpr uint32_t kMaxOsiCallsPerMethod = 2048;            // eval_calls: 78 -> 2,048 (26x)
constexpr uint32_t kMaxOsiArgc = 127;                       // per call: 9 -> 127 (14x; the s8 operand's own bound)
constexpr uint32_t kMaxOsiArgsPerMethod = 4096;             // eval_calls, sum of argc: 128 -> 4,096 (32x)
constexpr uint32_t kMaxOsiStringArgBytes = 16384;           // eval_calls, string bytes copied into args: 570 -> 16,384 (28x)
constexpr uint32_t kMaxOsiStackDepth = 256;                 // eval_calls literal stack: 16 -> 256 (16x)

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
  bool eval_calls(const std::string& cls, const std::string& method, std::vector<OsiCall>& out, std::string* err) const;
  // Disassembles every distinct function and method subroutine (bounds/opcode check of the whole file) within
  // kMaxOsiTotalInstructions / kMaxOsiTotalCalls; returns the instruction and CallGameFunction counts.
  bool check_all(uint64_t* instructions, std::string* err, uint64_t* calls = nullptr) const;

 private:
  struct Insn {
    uint32_t at = 0;
    uint8_t op = 0;
    int64_t a = 0, b = 0, c = 0;
    float f = 0;
  };
  bool parse_tables(std::vector<uint8_t> bytes, std::string* err);
  bool disasm(uint32_t start, std::vector<Insn>& out, std::string* err) const;
  bool eval_impl(const std::string& cls, const std::string& method, std::vector<OsiCall>& out, std::string* err) const;
  std::vector<uint8_t> d_;
  uint16_t major_ = 0, minor_ = 0;
  std::vector<std::string> strings_, globals_, symbols_, sources_;
  std::vector<std::pair<std::string, uint32_t>> functions_;
  std::vector<OsiClass> classes_;
};

}  // namespace lomn::sage
