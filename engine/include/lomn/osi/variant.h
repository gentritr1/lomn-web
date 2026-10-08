// M2a-1 T1: the OSI script VM's value type and its arithmetic (docs/osi/opcodes.md "Conventions", D7-D10, D8').
// A Variant is the original's 8-byte ScOSIVariant {u16 type, u32 value}. `value` holds int bits, f32 bits, a
// string-table index, a colour, a return/marker PC (a FILE OFFSET) or a heap handle (D7) - never a host pointer.
#pragma once
#include <cstdint>
#include <string>

namespace lomn::osi {

struct Variant {
  uint16_t type = 0x0f;
  uint32_t value = 0;
};
inline bool operator==(Variant a, Variant b) { return a.type == b.type && a.value == b.value; }
inline bool operator!=(Variant a, Variant b) { return !(a == b); }

enum : uint16_t {
  kNone = 0x00,         // result of an unsupported conversion (ConvertVariant 0x60b9d0)
  kObject = 0x01,       // script object (handle)
  kRetMarker = 0x02,    // script->script return marker (value = return PC, a file offset)
  kArray = 0x05,        // array (handle)
  kNothing = 0x0f,
  kInt = 0x10,
  kStringId = 0x11,     // string-table index
  kFloat = 0x12,
  kStringTable = 0x13,  // never created by any handler or CreateVariant overload (opcodes.md); not modelled
  kColour16 = 0x14,
  kColour = 0x15,
  kString = 0x16,       // dynamic string (handle)
  kFrameMarker = 0xff,  // engine->script frame-base marker (value = the caller's PC)
  kByRef = 0x200,       // by-reference box, only around a native call
};
inline bool is_game_type(uint16_t t) { return t >= 0x20 && t <= 0xfe; }

Variant make_int(int32_t i);
Variant make_float(float f);
Variant make_float_bits(uint32_t bits);
Variant make_colour(uint32_t rgba);
Variant nothing();
float as_float(Variant v);    // bit cast of value, no conversion
int32_t as_int(Variant v);    // bit cast of value, no conversion
uint32_t f32_bits(float f);
float bits_f32(uint32_t b);

// An error inside the VM (D10). Thrown by handlers, the stack, the heap and natives; the OUTERMOST Vm::call catches it,
// fills pc/op/depth from the VM state at the throw (nothing changes the state while unwinding) and records it.
// Texts are the binary's, typos included; our own errors: "Stack overflow (1000 slots)", "Stack underflow",
// "Direct call without a memoised site", and guards for behaviour the table marks undefined ("Undefined: ...").
// The non-OSI ScErrors keep their enum name as the text ("kLenTooLong", "kOutOfRange").
struct VmError {
  std::string message;
  uint32_t pc = 0;
  uint8_t op = 0;
  uint32_t depth = 0;
  VmError() = default;
  explicit VmError(std::string m) : message(std::move(m)) {}
};

// ---- Arithmetic (D8': x87 precision control is VM state, fpu_pc in {24, 64}) ----
enum class Op : uint8_t { Add, Sub, Mul, Div, Eq, Lt, Gt, Le, Ge };

// One operand of a numeric row: an int32 (loaded exactly by fild) or an f32 (fld dword / dword memory operand).
struct Num {
  bool is_int = true;
  int32_t i = 0;
  uint32_t f = 0;  // f32 bits
  static Num of_int(int32_t v) { Num n; n.is_int = true; n.i = v; return n; }
  static Num of_float_bits(uint32_t b) { Num n; n.is_int = false; n.f = b; return n; }
};

// Rows 0x70-0x73 (and 0x74's float*int step): every f32 result = round_f32(round_p(exact a OP b)), p = fpu_pc
// significant bits, nearest-even, unbounded exponent at the round_p step, then the final `fstp dword` (subnormals,
// overflow to +-inf). Computed exactly with integer arithmetic (no host FPU state), so native == wasm32. A NaN result is
// stored as 0xffc00000 (D9). Zero divisors are the caller's error (the handler tests them first).
uint32_t x87_arith(Op op, Num a, Num b, int fpu_pc);

// The consult's helper names (D8 interface), now with the D8' precision state.
float mixed_add(int32_t a, float b, int fpu_pc = 24);
float mixed_sub_if(int32_t a, float b, int fpu_pc = 24);
float mixed_sub_fi(float a, int32_t b, int fpu_pc = 24);
float mixed_mul(int32_t a, float b, int fpu_pc = 24);
float mixed_div_if(int32_t a, float b, int fpu_pc = 24);
float mixed_div_fi(float a, int32_t b, int fpu_pc = 24);

constexpr uint32_t kCanonicalNaN = 0xffc00000u;  // D9: the x87 real indefinite
float canon_nan(float f);
uint32_t canon_nan_bits(uint32_t bits);

// Rows 0x60-0x64: exact compare of the widened operands; unordered (any NaN) -> 0; +0.0 == -0.0.
int cmp_exact(double a, double b, Op op);

// ConvertVariant's numeric/text conversions (opcodes.md "ConvertVariant").
int32_t chop_to_int(uint32_t f32bits);           // fistp qword with chop, low 32 bits; NaN / |x| >= 2^63 -> 0
uint32_t int_to_f32_bits(int32_t i);             // fild + one fstp dword rounding
int32_t osi_atoi(const std::string& text);       // C atoi on the bytes up to the first NUL (32-bit wrap)
// atof up to the first NUL, rounded once to f32 (`fstp dword`). *switches_pc is false only when ___strtold would take
// its exponent-overflow path (|decimal exponent| > 32767), which skips ___dec2num (D8').
uint32_t osi_atof_bits(const std::string& text, bool* switches_pc);
std::string format_d(int32_t i);                 // "%d"
std::string format_f(uint32_t f32bits);          // "%f" of the f32 widened exactly to double

}  // namespace lomn::osi
