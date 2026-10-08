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
// C++ vs osi.py on CRAFTED input (Task 8B C3). Both agree on all 8,406 class.method pairs of the real Base.osi (ctest
// osi_all_methods); on hostile bytecode they differ in these documented ways (osi.py is a research oracle, this reader
// the engine's bounded one):
//  - float folds: each fold result is stored as float32 here (OsiValue::f); osi.py keeps float64 through a chain of
//    folds, so a chain can round differently (a single fold agrees after one double -> float rounding);
//  - integer folds are computed in double here and kept as Int only within +-9.0e18 (else the value becomes Unknown),
//    and lose exactness above 2^53; osi.py uses exact unbounded ints. NEITHER wraps at 32 bits: whether the game VM's
//    integers are 32-bit (and wrap) is UNVERIFIED (no real script folds out of the 32-bit range);
//  - a negative CallGameFunction argc is an error here; osi.py pops nothing (0 arguments);
//  - a string index outside the string table (PushString, call class/function names) gives Unknown / "?" here; osi.py
//    raises IndexError;
//  - ALIGNED (Task 8B C3): a branch target before the file start never extends the subroutine walk in either reader
//    (this one used to wrap it to a huge unsigned target and walk past every Return).
// Name lookups (find_class, eval_calls) compare ASCII case-insensitively without allocating; method names are symbol
// indices (OsiMethod::symbol, method_name()), never per-method copies.
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
constexpr uint32_t kMaxOsiClasses = 8192;                   // class table: 395 -> 8,192 (20.7x)
constexpr uint32_t kMaxOsiMethods = 1u << 18;               // methods summed over all classes: 8,406 -> 262,144 (31x)

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
  uint16_t symbol = 0;  // index into the symbol table (OsiFile::method_name); Task 8B C2: no per-method copy of the name
  uint32_t offset = 0;
};
struct OsiClass {
  std::string name;
  std::vector<OsiMethod> methods;
  std::vector<uint16_t> props;  // M2a-1: the class record's property symbols (GetProperty 0x6086a0 searches these)
};
// M2a-1: one global-function table entry {u8 len, name, u32 offset, u16 argc} (FindFunction 0x609ce0).
struct OsiFunction {
  std::string name;
  uint32_t offset = 0;
  uint16_t argc = 0;
};

class OsiFile {
 public:
  bool parse(std::vector<uint8_t> bytes, std::string* err);
  uint16_t major() const { return major_; }
  uint16_t minor() const { return minor_; }
  const std::vector<std::string>& strings() const { return strings_; }
  const std::vector<OsiClass>& classes() const { return classes_; }
  // M2a-1 T0 accessors for the script VM (additive; parse/eval_calls/check_all behave as before):
  const std::vector<OsiFunction>& functions() const { return functions_; }  // in file order, with declared argc
  const std::vector<std::string>& globals() const { return globals_; }       // the global-name table (0x21/0x22)
  const std::vector<std::string>& symbols() const { return symbols_; }       // member/method symbol names
  const std::vector<uint8_t>& bytes() const { return d_; }                   // the whole image (never written, D4)
  // File offset of string-table entry i's u8 length byte; its text (strings()[i], raw bytes) follows it.
  const std::vector<uint32_t>& string_offsets() const { return string_offsets_; }
  const OsiClass* find_class(const std::string& name) const;  // case-insensitive
  // The method's name (its symbol; parse() checked the index). Methods sharing a symbol share the one string.
  const std::string& method_name(const OsiMethod& m) const { return symbols_[m.symbol]; }
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
  std::vector<OsiFunction> functions_;
  std::vector<uint32_t> string_offsets_;
  std::vector<OsiClass> classes_;
};

}  // namespace lomn::sage
