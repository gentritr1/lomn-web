// M2a-1 T1: the 96 BCL handlers of ScOSIVirtualMachine, one per named row of docs/osi/opcodes.md, plus BCLUnused.
// Each row's code sits in a region opened by a `// ==== row 0xNN` line and holds that row's message literals verbatim
// (tools/osi/check_messages.py checks every row x message pair against its region). Shared bodies receive their message
// literals from the row that calls them. Undefined behaviour the table marks (a non-object in 0x11, `0x33 0`, a stale
// slot, an index past a table) raises our own "Undefined: ..." error instead of touching memory.
//
// Pop order and check order follow the asm (rows' cited addresses); cells settled from the asm in M2a-1 T1:
// - 0x10 (0x5fe664, 0x5fe7de, 0x5fe7fc): o popped, type checks, then v popped, then GetProperty.
// - 0x18 (0x5feec1/0x5feef1 then 0x5fef15): s and o both popped before the string check; the absent-property %s is the
//   name as given (the local copy, 0x5ff19f); disposal o then s (0x5ff1e2, 0x5ff1ed).
// - 0x19 (0x5ff284/0x5ff29f, 0x5ff2b4): both popped, then o's type, then s's; disposal o then s.
// - 0x1a (0x5ff421/0x5ff451, 0x5ff475; 0x5ff6c9, 0x5ff6f6): s and o popped, s checked, o checked, then v popped.
// - 0x60 (0x605369/0x605397): a 0x11 operand becomes a dynamic string whatever the other operand is (so `1 = "1"` allocates
//   one before its error); on the object/game/string paths the result is pushed, then a, then b disposed (0x6052a9-0x6052bf).
// - 0x6a etc. (0x6055f9, 0x605616), 0x74 (0x604984, 0x6049a3): lhs checked before rhs (strings at 0x74f8a7, 0x74f8dd).
// - 0xa0-0xa3 (0x5fff49/0x5fff64 then 0x5fff79, 0x5fff8f): both popped, then c's type, then v's.
// - 0x91-0x96 (e.g. 0x5fd3c5, 0x5fd3e3): all operands popped, then the 0x13 check, then 0x05, else non-array.
// - FindMemberID (0x609ba0): tolower (A-Z) of the name, then strcmp against each symbol; no match -> an unset id.
// - 0x82/0x83/0x84 advance PC past all operands before the native runs (0x6015c6-0x601780, 0x602178-0x60237c), so a
//   nested engine call's 0xff marker holds the address AFTER the call instruction.
#include <cmath>
#include <cstdio>
#include <exception>

#include "lomn/osi/natives.h"
#include "lomn/osi/vm.h"

namespace lomn::osi {

namespace {
bool is_str(Variant v) { return v.type == kStringId || v.type == kString; }
Num num_of(Variant v) { return v.type == kInt ? Num::of_int(static_cast<int32_t>(v.value)) : Num::of_float_bits(v.value); }
double widen(Variant v) { return v.type == kInt ? static_cast<double>(static_cast<int32_t>(v.value)) : static_cast<double>(bits_f32(v.value)); }
std::string upto_nul(std::string s) {
  const size_t z = s.find('\0');
  if (z != std::string::npos) s.resize(z);
  return s;
}
std::string fmt_s(const char* fmt, const std::string& a) {
  char b[600];
  std::snprintf(b, sizeof b, fmt, a.c_str());
  return b;
}
std::string fmt_ss(const char* fmt, const std::string& a, const std::string& c) {
  char b[700];
  std::snprintf(b, sizeof b, fmt, a.c_str(), c.c_str());
  return b;
}
uint32_t lower_ascii(uint32_t c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
}  // namespace

// ==== shared: the dispatch table (256 entries; every op without a named handler is BCLUnused)
bool Vm::dispatch(uint8_t op) {
  struct Table {
    Handler t[256];
    Table() {
      for (auto& h : t) h = &Vm::bcl_unused;
#define S(o) t[0x##o] = &Vm::bcl_##o;
      S(00) S(02) S(03) S(04) S(05) S(06) S(10) S(11) S(12) S(13) S(14) S(15) S(16) S(17) S(18) S(19) S(1a)
      S(21) S(22) S(23) S(24) S(25) S(30) S(31) S(32) S(33) S(34) S(35) S(40) S(41) S(42) S(43) S(44) S(45) S(46) S(47)
      S(48) S(49) S(50) S(51) S(52) S(53) S(54) S(60) S(61) S(62) S(63) S(64) S(6a) S(6b) S(6c) S(6d) S(6e) S(6f)
      S(70) S(71) S(72) S(73) S(74) S(75) S(76) S(77) S(78) S(7a) S(7b) S(80) S(81) S(82) S(83) S(84)
      S(90) S(91) S(92) S(93) S(94) S(95) S(96) S(a0) S(a1) S(a2) S(a3) S(a4) S(a5) S(a6) S(a7)
      S(b0) S(b1) S(b2) S(b8) S(b9) S(ba) S(bb) S(bc) S(bd) S(bf) S(ff)
#undef S
    }
  };
  static const Table table;
  return (this->*table.t[op])();
}

// ==== row 0x00
bool Vm::bcl_00() {  // BCLNop 0x603830
  pc_ = cur_pc_ + 1;
  return true;
}
// ==== row 0x02
bool Vm::bcl_02() {  // BCLDebugOn 0x6038a0: PC += 1 (0x6038a6), then the error
  pc_ = cur_pc_ + 1;
  throw VmError("Debugger not enabled! but encountered DebugOn");
}
// ==== row 0x03
bool Vm::bcl_03() {  // BCLDebugOff 0x6038d0: PC += 1 (0x6038d6), then the error
  pc_ = cur_pc_ + 1;
  throw VmError("Debugger not enabled! but encountered DebugOff");
}
// ==== row 0x04
bool Vm::bcl_04() {  // BCLLineNumber 0x603870: operands skipped
  pc_ = cur_pc_ + 5;
  return true;
}
// ==== row 0x05
bool Vm::bcl_05() {
  pc_ = cur_pc_ + 5;
  return true;
}
// ==== row 0x06
bool Vm::bcl_06() {
  pc_ = cur_pc_ + 5;
  return true;
}

// ==== row 0x10
bool Vm::bcl_10() {  // BCLSetMemberValue 0x5fe610
  pc_ = cur_pc_ + 3;  // T2a-pc: PC first (error-PC table)
  const uint16_t m = op_u16(1);
  const Variant o = pop();
  if (is_game_type(o.type)) throw VmError("Reference to unregistered Game Variable");  // map never populated
  if (o.type != kObject) throw VmError("Member property of non member variable");
  const Variant v = pop();
  Variant* prop = heap_.property(o.value, m, script_);
  if (!prop) throw VmError(fmt_s("Reference to non-existant member property \"%s\"", symbol_name(m)));
  heap_.dispose(*prop);
  *prop = v;
  heap_.dispose(o);
  return true;
}
// ==== row 0x11
bool Vm::bcl_11() {  // BCLGetMemberValue 0x5fe9a0
  pc_ = cur_pc_ + 3;  // T2a-pc: PC first (error-PC table)
  const uint16_t m = op_u16(1);
  const Variant o = pop();
  if (is_game_type(o.type)) throw VmError("Reference to unregistered Game Variable");
  if (o.type != kObject) throw VmError("Undefined: 0x11 on a non-object");
  Variant* prop = heap_.property(o.value, m, script_);
  if (!prop) throw VmError("Undefined: 0x11 on an absent property");
  push(heap_.duplicate(*prop));
  heap_.dispose(o);
  return true;
}
// ==== row 0x12
bool Vm::bcl_12() {  // BCLGetMemberFunction 0x5fe590: pushes {0x10, file offset} (D4: never a host address)
  pc_ = cur_pc_ + 3;  // T2a-pc: PC first (error-PC table)
  const uint16_t m = op_u16(1);
  const Variant o = pop();
  if (o.type != kObject) throw VmError("Undefined: 0x12 on a non-object");
  const uint32_t off = find_method(o.value, m, "Call to undefined member method");
  push(Variant{kInt, off});
  heap_.dispose(o);
  return true;
}
// ==== row 0x13
bool Vm::bcl_13() {  // BCLCreateObject 0x5fe530
  pc_ = cur_pc_ + 3;  // T2a-pc: PC first (error-PC table)
  const uint16_t c = op_u16(1);
  if (c >= script_.classes().size()) throw VmError("Undefined: class index out of range");
  push(Variant{kObject, heap_.new_object(c, static_cast<uint32_t>(script_.classes()[c].props.size()))});
  return true;
}
// ==== row 0x14
bool Vm::bcl_14() {  // BCLMemberFunctionArgumentCheck 0x5feb90
  pc_ = cur_pc_ + 2;  // T2a-pc: PC first (error-PC table)
  const uint32_t n = op_u8(1);
  uint32_t k = (stack_.depth() - frame_) & 0xff;
  if (n < k) throw VmError("Too many arguments to method");
  for (; k < n; ++k) push(nothing());
  return true;
}
// ==== row 0x15
bool Vm::bcl_15() {  // BCLSetThisMemberValue 0x5fec10: the type check precedes the pop
  pc_ = cur_pc_ + 3;  // T2a-pc: PC first (error-PC table)
  const uint16_t m = op_u16(1);
  const Variant self = frame_slot(0);
  if (self.type != kObject) throw VmError("Member property of non member variable");
  const Variant v = pop();
  Variant* prop = heap_.property(self.value, m, script_);
  if (!prop) throw VmError(fmt_s("Reference to non-existant member property \"%s\"", symbol_name(m)));
  heap_.dispose(*prop);
  *prop = v;
  return true;
}
// ==== row 0x16
bool Vm::bcl_16() {  // BCLGetThisMemberValue 0x5fecf0
  pc_ = cur_pc_ + 3;  // T2a-pc: PC first (error-PC table)
  const uint16_t m = op_u16(1);
  const Variant self = frame_slot(0);
  if (self.type != kObject) throw VmError("Member property of non member variable");
  Variant* prop = heap_.property(self.value, m, script_);
  if (!prop) throw VmError(fmt_s("Reference to non-existant member property \"%s\"", symbol_name(m)));
  push(heap_.duplicate(*prop));
  return true;
}
// ==== row 0x17
bool Vm::bcl_17() {  // BCLGetThisMemberFunction 0x5fedb0
  pc_ = cur_pc_ + 3;  // T2a-pc: PC first (error-PC table)
  const uint16_t m = op_u16(1);
  const Variant self = frame_slot(0);
  if (self.type != kObject) throw VmError("Member method of non member variable");
  push(Variant{kInt, find_method(self.value, m, "Call to undefined member method")});
  return true;
}
// ==== row 0x18
bool Vm::bcl_18() {  // BCLGetMemberValueFromString 0x5fee40
  pc_ = cur_pc_ + 1;  // T2a-pc: PC first (error-PC table)
  const Variant s = pop();
  const Variant o = pop();
  if (!is_str(s)) throw VmError("Invalid type for member access (must be a string)");
  const std::string name = name_copy(s);  // kOutOfRange above 255 (natives.md §8.3)
  if (is_game_type(o.type)) throw VmError("Reference to unregistered Game Variable");
  if (o.type != kObject) throw VmError("Member property of non member variable");
  Variant* prop = heap_.property(o.value, find_member_id(name), script_);
  if (!prop) throw VmError(fmt_s("Reference to non-existant member property \"%s\"", name));
  push(heap_.duplicate(*prop));
  heap_.dispose(o);
  heap_.dispose(s);
  return true;
}
// ==== row 0x19
bool Vm::bcl_19() {  // BCLGetMemberFunctionFromString 0x5ff210
  pc_ = cur_pc_ + 1;  // T2a-pc: PC first (error-PC table)
  const Variant s = pop();
  const Variant o = pop();
  if (o.type != kObject) throw VmError("Member method of non member variable");
  if (!is_str(s)) throw VmError("Invalid method type for function call (must be a string)");
  const std::string name = name_copy(s);  // kOutOfRange above 255 (natives.md §8.3)
  push(Variant{kInt, find_method(o.value, find_member_id(name), "Call to undefined member method")});
  heap_.dispose(o);
  heap_.dispose(s);
  return true;
}
// ==== row 0x1a
bool Vm::bcl_1a() {  // BCLSetMemberValueFromString 0x5ff3a0
  pc_ = cur_pc_ + 1;  // T2a-pc: PC first (error-PC table)
  const Variant s = pop();
  const Variant o = pop();
  if (!is_str(s)) throw VmError("Invalid type for member set (must be a string)");
  const std::string name = name_copy(s);  // kOutOfRange above 255 (natives.md §8.3)
  if (is_game_type(o.type)) throw VmError("Reference to unregistered Game Variable");
  if (o.type != kObject) throw VmError("Member property of non object variable");
  const Variant v = pop();
  Variant* prop = heap_.property(o.value, find_member_id(name), script_);
  if (!prop) throw VmError(fmt_s("Reference to non-existant member property \"%s\"", name));
  heap_.dispose(*prop);
  *prop = v;
  heap_.dispose(o);
  heap_.dispose(s);
  return true;
}

// ==== row 0x21
bool Vm::bcl_21() {  // BCLGetVariableValue 0x607c50
  pc_ = cur_pc_ + 3;  // T2a-pc: PC first (error-PC table)
  const Variant d = heap_.duplicate(var_ref(op_u16(1)));
  push(d);
  return true;
}
// ==== row 0x22
bool Vm::bcl_22() {  // BCLSetVariableValue 0x607b50
  pc_ = cur_pc_ + 3;  // T2a-pc: PC first (error-PC table)
  const uint16_t idx = op_u16(1);
  const Variant v = pop();
  Variant& t = var_ref(idx);
  heap_.dispose(t);
  t = v;
  return true;
}
// ==== row 0x23
bool Vm::bcl_23() {  // BCLCreateStackVariables 0x607b00
  pc_ = cur_pc_ + 2;  // T2a-pc: PC first (error-PC table)
  const uint32_t n = op_u8(1);
  for (uint32_t i = 0; i < n; ++i) push(nothing());
  return true;
}
// ==== row 0x24
bool Vm::bcl_24() {  // BCLIncrementVariable 0x607cf0
  pc_ = cur_pc_ + 3;  // T2a-pc: PC first (error-PC table)
  Variant& v = var_ref(op_u16(1));
  if (v.type != kInt) throw VmError("increment of non-integral type");
  v.value += 1;
  return true;
}
// ==== row 0x25
bool Vm::bcl_25() {  // BCLDecrementVariable 0x607d90: the global path reuses the "increment" text
  pc_ = cur_pc_ + 3;  // T2a-pc: PC first (error-PC table)
  const uint16_t idx = op_u16(1);
  Variant& v = var_ref(idx);
  if (v.type != kInt) {
    if (idx & 0x8000) throw VmError("increment of non-integral type");
    throw VmError("decrement of non-integral type");
  }
  v.value -= 1;
  return true;
}

// ==== row 0x30
bool Vm::bcl_30() {  // BCLPop 0x606310
  pc_ = cur_pc_ + 1;  // T2a-pc: PC first (error-PC table)
  pop_dispose();
  return true;
}
// ==== row 0x31
bool Vm::bcl_31() {  // BCLPopN 0x606340
  pc_ = cur_pc_ + 2;  // T2a-pc: PC first (error-PC table)
  const uint32_t n = op_u8(1);
  for (uint32_t i = 0; i < n; ++i) pop_dispose();
  return true;
}
// ==== row 0x32
bool Vm::bcl_32() {  // BCLSwap 0x606390
  pc_ = cur_pc_ + 1;  // T2a-pc: PC first (error-PC table)
  const Variant a = pop(), b = pop();
  push(a);
  push(b);
  return true;
}
// ==== row 0x33
bool Vm::bcl_33() {  // BCLPull 0x606420: copies top-n (n = 1 is Dup)
  pc_ = cur_pc_ + 2;  // T2a-pc: PC first (error-PC table)
  const uint32_t n = op_u8(1);
  if (n == 0 || n > stack_.depth()) throw VmError("Undefined: 0x33 operand outside the stack");
  const Variant d = heap_.duplicate(stack_.at(stack_.depth() - n));
  push(d);
  return true;
}
// ==== row 0x34
bool Vm::bcl_34() {  // BCLDupN 0x6064b0
  pc_ = cur_pc_ + 2;  // T2a-pc: PC first (error-PC table)
  const uint32_t n = op_u8(1);
  for (uint32_t i = 0; i < n; ++i) {
    if (n > stack_.depth()) throw VmError("Undefined: 0x34 operand outside the stack");
    const Variant d = heap_.duplicate(stack_.at(stack_.depth() - n));
    push(d);
  }
  return true;
}
// ==== row 0x35
bool Vm::bcl_35() {  // BCLDup 0x606470
  pc_ = cur_pc_ + 1;  // T2a-pc: PC first (error-PC table)
  const Variant d = heap_.duplicate(stack_.top());
  push(d);
  return true;
}

// ==== row 0x40
bool Vm::bcl_40() {
  pc_ = cur_pc_ + 5;  // T2a-pc: PC first (error-PC table)
  push(Variant{kInt, op_u32(1)});
  return true;
}
// ==== row 0x41
bool Vm::bcl_41() { throw VmError("unwritten instruction BCL_PushConstanti24 called"); }  // 0x6065e0, PC not advanced
// ==== row 0x42
bool Vm::bcl_42() {
  pc_ = cur_pc_ + 3;  // T2a-pc: PC first (error-PC table)
  push(make_int(static_cast<int16_t>(op_u16(1))));
  return true;
}
// ==== row 0x43
bool Vm::bcl_43() {
  pc_ = cur_pc_ + 2;  // T2a-pc: PC first (error-PC table)
  push(make_int(static_cast<int8_t>(op_u8(1))));
  return true;
}
// ==== row 0x44
bool Vm::bcl_44() {
  pc_ = cur_pc_ + 5;  // T2a-pc: PC first (error-PC table)
  push(Variant{kFloat, op_u32(1)});
  return true;
}
// ==== row 0x45
bool Vm::bcl_45() {
  pc_ = cur_pc_ + 1;  // T2a-pc: PC first (error-PC table)
  push(make_int(0));
  return true;
}
// ==== row 0x46
bool Vm::bcl_46() {
  pc_ = cur_pc_ + 3;  // T2a-pc: PC first (error-PC table)
  push(Variant{kStringId, op_u16(1)});
  return true;
}
// ==== row 0x47
bool Vm::bcl_47() {
  pc_ = cur_pc_ + 1;  // T2a-pc: PC first (error-PC table)
  push(nothing());
  return true;
}
// ==== row 0x48
bool Vm::bcl_48() {
  pc_ = cur_pc_ + 5;  // T2a-pc: PC first (error-PC table)
  push(Variant{kColour, op_u32(1)});
  return true;
}
// ==== row 0x49
bool Vm::bcl_49() {
  pc_ = cur_pc_ + 3;  // T2a-pc: PC first (error-PC table)
  push(Variant{kColour16, op_u16(1)});
  return true;
}

// ==== row 0x50
bool Vm::bcl_50() {  // BCLJumpRelative 0x600ae0: the target is a FILE offset (0 = 'OSI\0')
  pc_ = cur_pc_ + 2;  // T2a-pc: the error PC (+2); the jump target is written last
  const uint32_t n = op_u8(1);
  const Variant t = pop();
  if (t.type != kInt) throw VmError("Internal error: Jump to non-integral address");
  stack_.insert_below(n, Variant{kRetMarker, cur_pc_ + 2});
  if (stack_.depth() > high_water_) high_water_ = stack_.depth();
  frame_ = stack_.find_frame();
  pc_ = t.value;
  return true;
}
// ==== row 0x51
bool Vm::bcl_51() {  // BCLJumpAbsolute 0x600c30: the target is the tagged file offset from 0x12/0x17/0x19
  pc_ = cur_pc_ + 2;  // T2a-pc: the error PC (+2); the jump target is written last
  const uint32_t n = op_u8(1);
  const Variant t = pop();
  if (t.type != kInt) throw VmError("Internal error: Jump to non-integral address");
  stack_.insert_below(n, Variant{kRetMarker, cur_pc_ + 2});
  if (stack_.depth() > high_water_) high_water_ = stack_.depth();
  frame_ = stack_.find_frame();
  pc_ = t.value;
  return true;
}
// ==== row 0x52
bool Vm::bcl_52() {  // BCLReturn 0x600cf0
  const Variant r = pop();
  for (;;) {
    const Variant m = pop();
    heap_.dispose(m);
    if (m.type == kRetMarker) {
      push(r);
      pc_ = m.value;
      frame_ = stack_.find_frame();
      return true;
    }
    if (m.type == kFrameMarker) {
      push(m);
      push(r);
      return false;  // Run stops; PC unchanged
    }
  }
}
// ==== row 0x53
bool Vm::bcl_53() {  // BCLCompareAndBranchIfFalse 0x600df0
  const int16_t d = static_cast<int16_t>(op_u16(1));
  const Variant c = pop();
  if (c.type == kNothing || (c.type == kInt && c.value == 0)) {
    pc_ = cur_pc_ + 3 + d;
  } else if (c.type == kInt) {
    pc_ = cur_pc_ + 3;
  } else {
    throw VmError("Results of comparison expression is not integral");
  }
  return true;
}
// ==== row 0x54
bool Vm::bcl_54() {  // BCLBranchAlways 0x600e70
  pc_ = cur_pc_ + 3 + static_cast<int16_t>(op_u16(1));
  return true;
}

// ==== shared: compares 0x61-0x64 (each row passes its texts)
bool Vm::compare(Op op, const char* lhs_msg, const char* rhs_msg) {
  pc_ = cur_pc_ + 1;  // T2a-pc: PC first (error-PC table)
  const Variant b = pop(), a = pop();
  int r;
  if ((a.type == kFloat || a.type == kInt) && (b.type == kFloat || b.type == kInt) && (a.type == kFloat || b.type == kFloat)) {
    r = cmp_exact(widen(a), widen(b), op);
  } else {
    if (a.type != kInt) throw VmError(fmt_err(lhs_msg, a.type));
    if (b.type != kInt) throw VmError(fmt_err(rhs_msg, b.type));
    r = cmp_exact(static_cast<int32_t>(a.value), static_cast<int32_t>(b.value), op);
  }
  push(make_int(r));
  return true;
}
// ==== row 0x60
bool Vm::bcl_60() {  // BCLEqualTo 0x605180
  pc_ = cur_pc_ + 1;  // T2a-pc: PC first (error-PC table)
  Variant b = pop(), a = pop();
  if (a.type == kNothing) {
    push(make_int(b.type == kNothing ? 1 : 0));
    if (b.type != kNothing) heap_.dispose(b);
    return true;
  }
  if (b.type == kNothing) {
    push(make_int(0));
    heap_.dispose(a);
    return true;
  }
  if ((a.type == kObject && b.type == kObject) || (is_game_type(a.type) && is_game_type(b.type))) {
    push(make_int(a.type == b.type && a.value == b.value ? 1 : 0));
    heap_.dispose(a);
    heap_.dispose(b);
    return true;
  }
  if (a.type == kStringId) a = to_dynamic_string(a);
  if (b.type == kStringId) b = to_dynamic_string(b);
  if (a.type == kString && b.type == kString) {
    const std::string& x = heap_.text(a.value);
    const std::string& y = heap_.text(b.value);
    bool eq = x.size() == y.size();  // InsensitiveComparison 0x5faf80: only a-z/A-Z fold
    for (size_t i = 0; eq && i < x.size(); ++i)
      eq = lower_ascii(static_cast<unsigned char>(x[i])) == lower_ascii(static_cast<unsigned char>(y[i]));
    push(make_int(eq ? 1 : 0));
    heap_.dispose(a);
    heap_.dispose(b);
    return true;
  }
  if ((a.type == kFloat || a.type == kInt) && (b.type == kFloat || b.type == kInt) && (a.type == kFloat || b.type == kFloat)) {
    push(make_int(cmp_exact(widen(a), widen(b), Op::Eq)));
    return true;
  }
  if (a.type != kInt) throw VmError(fmt_err("Invalid operand to operator '=' (lhs) (type:%02.2X)", a.type));
  if (b.type != kInt) throw VmError(fmt_err("Invalid operand to operator '=' (rhs) (type:%02.2X)", b.type));
  push(make_int(a.value == b.value ? 1 : 0));
  return true;
}
// ==== row 0x61
bool Vm::bcl_61() {
  return compare(Op::Lt, "Invalid operand to operator '<' (lhs) (type:%02.2X)", "Invalid operand to operator '<' (rhs) (type:%02.2X)");
}
// ==== row 0x62
bool Vm::bcl_62() {
  return compare(Op::Gt, "Invalid operand to operator '>' (lhs) (type:%02.2X)", "Invalid operand to operator '>' (rhs) (type:%02.2X)");
}
// ==== row 0x63
bool Vm::bcl_63() {
  return compare(Op::Le, "Invalid operand to operator '<=' (lhs) (type:%02.2X)",
                 "Invalid operand to operator '<=' (rhs) (type:%02.2X)");
}
// ==== row 0x64
bool Vm::bcl_64() {
  return compare(Op::Ge, "Invalid operand to operator '>=' (lhs) (type:%02.2X)",
                 "Invalid operand to operator '>=' (rhs) (type:%02.2X)");
}

// ==== shared: int,int operators (and 0x6a 0x6b 0x6d 0x6e 0x6f 0x75 0x77 0x78; which: 0 and 1 or 2 & 3 | 4 ^ 5 % 6 << 7 >>)
bool Vm::int_binop(int which, const char* lhs_msg, const char* rhs_msg) {
  pc_ = cur_pc_ + 1;  // T2a-pc: PC first (error-PC table)
  const Variant b = pop(), a = pop();
  if (a.type != kInt) throw VmError(fmt_err(lhs_msg, a.type));
  if (b.type != kInt) throw VmError(fmt_err(rhs_msg, b.type));
  const uint32_t x = a.value, y = b.value;
  uint32_t r = 0;
  switch (which) {
    case 0: r = (x != 0 && y != 0); break;
    case 1: r = (x != 0 || y != 0); break;
    case 2: r = x & y; break;
    case 3: r = x | y; break;
    case 4: r = x ^ y; break;
    case 6: r = x << (y & 31); break;
    case 7: r = x >> (y & 31); break;  // logical (0x605a76)
    default: break;
  }
  push(Variant{a.type, r});
  return true;
}
// ==== row 0x6a
bool Vm::bcl_6a() {
  return int_binop(0, "Invalid operand to operator 'and' (lhs) (type:%02.2X)", "Invalid operand to operator 'and' (rhs) (type:%02.2X)");
}
// ==== row 0x6b
bool Vm::bcl_6b() {
  return int_binop(1, "Invalid operand to operator 'or' (lhs) (type:%02.2X)", "Invalid operand to operator 'or' (rhs) (type:%02.2X)");
}
// ==== row 0x6c
bool Vm::bcl_6c() {  // BCLNot 0x605b40
  pc_ = cur_pc_ + 1;  // T2a-pc: PC first (error-PC table)
  const Variant a = pop();
  if (a.type == kNothing) push(make_int(1));
  else if (a.type == kInt) push(make_int(a.value == 0 ? 1 : 0));
  else throw VmError(fmt_err("Invalid operand to operator 'not' (type:%02.2X)", a.type));
  return true;
}
// ==== row 0x6d
bool Vm::bcl_6d() {
  return int_binop(2, "Invalid operand to operator '&' (lhs) (type:%02.2X)", "Invalid operand to operator '&' (rhs) (type:%02.2X)");
}
// ==== row 0x6e
bool Vm::bcl_6e() {
  return int_binop(3, "Invalid operand to operator '|' (lhs) (type:%02.2X)", "Invalid operand to operator '|' (rhs) (type:%02.2X)");
}
// ==== row 0x6f
bool Vm::bcl_6f() {
  return int_binop(4, "Invalid operand to operator '@' (lhs) (type:%02.2X)", "Invalid operand to operator '@' (rhs) (type:%02.2X)");
}

// ==== shared: numeric 0x70-0x73 after 0x70's string cases (D8': every f32 result through x87_arith at fpu_pc_)
namespace {
bool float_zero(Variant v) { return (v.value & 0x7fffffffu) == 0; }  // ordered equality with 0.0: +-0 (not NaN)
Variant arith_result(Op op, Variant a, Variant b, int pc, const char* div0_msg, const std::string& lhs_text,
                     const std::string& rhs_text) {
  const bool af = a.type == kFloat, bf = b.type == kFloat, ai = a.type == kInt, bi = b.type == kInt;
  if ((af || ai) && (bf || bi) && (af || bf)) {
    if (op == Op::Div && ((bf && float_zero(b)) || (bi && b.value == 0))) throw VmError(div0_msg);
    return Variant{kFloat, x87_arith(op, num_of(a), num_of(b), pc)};
  }
  if (!ai) throw VmError(lhs_text);
  if (!bi) throw VmError(rhs_text);
  const int32_t x = static_cast<int32_t>(a.value), y = static_cast<int32_t>(b.value);
  uint32_t r = 0;
  switch (op) {
    case Op::Add: r = a.value + b.value; break;
    case Op::Sub: r = a.value - b.value; break;
    case Op::Mul: r = a.value * b.value; break;
    default:
      if (y == 0) throw VmError(div0_msg);
      if (x == INT32_MIN && y == -1) throw VmError("Undefined: INT_MIN / -1 (#DE in the original)");
      r = static_cast<uint32_t>(x / y);  // idiv: truncation toward zero
      break;
  }
  return Variant{a.type, r};
}
}  // namespace

// ==== row 0x70
bool Vm::bcl_70() {  // BCLAdd 0x603ff0
  pc_ = cur_pc_ + 1;  // T2a-pc: PC first (error-PC table)
  Variant b = pop(), a = pop();
  if (a.type == kStringId) a = to_dynamic_string(a);
  if (b.type == kStringId) b = to_dynamic_string(b);
  if (a.type == kString) {
    if (b.type != kString) throw VmError(fmt_err("Invalid operand to string operator '+' (rhs) (type:%02.2X)", b.type));
    std::string& x = heap_.text(a.value);
    const std::string y = heap_.text(b.value);
    if (x.size() + y.size() >= 255) throw VmError("kLenTooLong");  // ScFixedString::append 0x4f5c80, non-OSI ScError
    x += y;
    push(a);
    heap_.dispose(b);
  } else if (b.type == kString) {
    throw VmError(fmt_err("Invalid operand to string operator '+' (lhs) (type:%02.2X)", a.type));
  } else {
    push(arith_result(Op::Add, a, b, fpu_pc_, "",
                      fmt_err("Invalid operand to operator '+' (lhs) (type:%02.2X)", a.type),
                      fmt_err("Invalid operand to operator '+' (rhs) (type:%02.2X)", b.type)));
  }
  return true;
}
// ==== row 0x71
bool Vm::bcl_71() {  // BCLSubtract 0x6042b0
  pc_ = cur_pc_ + 1;  // T2a-pc: PC first (error-PC table)
  const Variant b = pop(), a = pop();
  push(arith_result(Op::Sub, a, b, fpu_pc_, "",
                    fmt_err("Invalid operand to operator '-' (lhs) (type:%02.2X)", a.type),
                    fmt_err("Invalid operand to operator '-' (rhs) (type:%02.2X)", b.type)));
  return true;
}
// ==== row 0x72
bool Vm::bcl_72() {  // BCLMultiply 0x604450 (its lhs text differs)
  pc_ = cur_pc_ + 1;  // T2a-pc: PC first (error-PC table)
  const Variant b = pop(), a = pop();
  push(arith_result(Op::Mul, a, b, fpu_pc_, "",
                    fmt_err("Invalid left-hand-side operand to operator '*' (type:%02.2X)", a.type),
                    fmt_err("Invalid operand to operator '*' (rhs) (type:%02.2X)", b.type)));
  return true;
}
// ==== row 0x73
bool Vm::bcl_73() {  // BCLDivide 0x6045f0
  pc_ = cur_pc_ + 1;  // T2a-pc: PC first (error-PC table)
  const Variant b = pop(), a = pop();
  push(arith_result(Op::Div, a, b, fpu_pc_, "Divide by 0",
                    fmt_err("Invalid operand to operator '/' (lhs) (type:%02.2X)", a.type),
                    fmt_err("Invalid operand to operator '/' (rhs) (type:%02.2X)", b.type)));
  return true;
}
// ==== row 0x74
bool Vm::bcl_74() {  // BCLPower 0x6047f0
  pc_ = cur_pc_ + 1;  // T2a-pc: PC first (error-PC table)
  const Variant b = pop(), a = pop();
  Variant r;
  if (a.type == kFloat && b.type == kFloat) {
    r = Variant{kFloat, canon_nan_bits(f32_bits(static_cast<float>(std::pow(widen(a), widen(b)))))};
  } else if (a.type == kInt && b.type == kFloat) {
    r = Variant{kFloat, canon_nan_bits(f32_bits(static_cast<float>(std::pow(widen(a), widen(b)))))};
  } else if (a.type == kFloat && b.type == kInt) {
    uint32_t x = a.value;  // b times: x <- (float)(int32 bits of x) * x  (fild of the raw bits, 0x604944-0x604953)
    for (uint32_t k = 0; k < b.value; ++k) {
      const uint32_t nx = x87_arith(Op::Mul, Num::of_int(static_cast<int32_t>(x)), Num::of_float_bits(x), fpu_pc_);
      if (nx == x) break;  // a fixed point: every later iteration gives the same value
      x = nx;
    }
    r = Variant{kFloat, x};
  } else {
    if (a.type != kInt) throw VmError(fmt_err("Invalid operand to operator '^' (lhs) (type:%02.2X)", a.type));
    if (b.type != kInt) throw VmError(fmt_err("Invalid operand to operator '^' (rhs) (type:%02.2X)", b.type));
    uint32_t x = a.value;  // b times: x <- x * x (wrapping imul, 0x6049c0-0x6049cf)
    for (uint32_t k = 0; k < b.value; ++k) {
      const uint32_t nx = x * x;
      if (nx == x) break;
      x = nx;
    }
    r = Variant{a.type, x};
  }
  push(r);
  return true;
}
// ==== row 0x75
bool Vm::bcl_75() {  // BCLModulus 0x604a00: unsigned div, no float path
  pc_ = cur_pc_ + 1;  // T2a-pc: PC first (error-PC table)
  const Variant b = pop(), a = pop();
  if (a.type != kInt) throw VmError(fmt_err("Invalid operand to operator '%' (lhs) (type:%02.2X)", a.type));
  if (b.type != kInt) throw VmError(fmt_err("Invalid operand to operator '%' (rhs) (type:%02.2X)", b.type));
  if (b.value == 0) throw VmError("Divide by 0");
  push(Variant{a.type, a.value % b.value});
  return true;
}
// ==== row 0x76
bool Vm::bcl_76() {  // BCLBitwiseNot 0x605aa0
  pc_ = cur_pc_ + 1;  // T2a-pc: PC first (error-PC table)
  const Variant a = pop();
  if (a.type == kNothing) push(make_int(1));
  else if (a.type == kInt) push(Variant{kInt, ~a.value});
  else throw VmError(fmt_err("Invalid operand to operator '~' (type:%02.2X)", a.type));
  return true;
}
// ==== row 0x77
bool Vm::bcl_77() {
  return int_binop(6, "Invalid operand to operator '<<' (lhs) (type:%02.2X)", "Invalid operand to operator '<<' (rhs) (type:%02.2X)");
}
// ==== row 0x78
bool Vm::bcl_78() {
  return int_binop(7, "Invalid operand to operator '>>' (lhs) (type:%02.2X)", "Invalid operand to operator '>>' (rhs) (type:%02.2X)");
}
// ==== row 0x7a
bool Vm::bcl_7a() {  // BCLIncrement 0x605be0: raw value + 1, no type check
  pc_ = cur_pc_ + 1;  // T2a-pc: PC first (error-PC table)
  stack_.top().value += 1;
  return true;
}
// ==== row 0x7b
bool Vm::bcl_7b() {
  pc_ = cur_pc_ + 1;  // T2a-pc: PC first (error-PC table)
  stack_.top().value -= 1;
  return true;
}
// ==== row 0x80
bool Vm::bcl_80() {  // BCLGetGameVariable 0x6020d0: tokens computed and discarded
  pc_ = cur_pc_ + 5;
  return true;
}
// ==== row 0x81
bool Vm::bcl_81() {
  pc_ = cur_pc_ + 5;
  return true;
}

// ==== shared: the native call machinery of 0x82/0x83/0x84 (trace.md §3 "A native call")
void Vm::invoke_native(const NativeEntry& e, uint32_t n, const std::string& cls, const std::string& fn, uint32_t next_pc,
                       bool check_argc, const NativeMsgs& msgs) {
  if (check_argc) {
    if (n > e.max) throw VmError(msgs.too_many);
    if (n < e.min) throw VmError(msgs.too_few);
  }
  if (n > stack_.depth()) throw VmError("Undefined: more native arguments than stack slots");
  const uint32_t base = stack_.depth() - n;
  std::vector<Variant*> args(n);
  std::vector<bool> byref(n, false);
  for (uint32_t i = 0; i < n; ++i) {
    Variant& slot = stack_.at(base + i);
    const uint16_t t = i < 10 ? e.types[i] : kNothing;
    if (t == kByRef) {
      byref[i] = true;
    } else if (slot.type != t) {
      try {
        Variant r = convert(slot, t);  // may first turn a 0x11 slot into a dynamic string in place
        heap_.dispose(slot);           // PushInto 0x601ec0 disposes the old value
        slot = r;
      } catch (...) {
        if (!msgs.mismatch) throw;
        char b[160];
        std::snprintf(b, sizeof b, msgs.mismatch, static_cast<int>(i));
        throw VmError(b);
      }
    }
    args[i] = &slot;
  }
  std::vector<Variant> before(n);
  for (uint32_t i = 0; i < n; ++i) before[i] = *args[i];
  const uint32_t site = cur_pc_;
  const uint8_t site_op = cur_op_;
  pc_ = next_pc;  // the handler has consumed its operands before the native runs
  // D14: the sequence number is assigned at native ENTRY (a nested native gets a higher one, its line comes first)
  const uint64_t seq = native_seq_++;
  if (observer_) observer_(e, seq);
  active_seq_.push_back(seq);
  NativeCall call{*this, e, site, args};
  Variant r;
  try {
    r = e.fn ? e.fn(call, e.user) : natives_.stub_return(e, heap_, call);
  } catch (...) {
    active_seq_.pop_back();
    throw;
  }
  active_seq_.pop_back();
  if (exit_observer_) exit_observer_(e, seq);
  cur_pc_ = site;
  cur_op_ = site_op;
  if (e.sets_fpu_pc64) set_pc64();
  auto s_line = [&](Variant v) {
    const std::string& t = heap_.text(v.value);
    char b[24];
    std::snprintf(b, sizeof b, "S %08x %3u", v.value, static_cast<unsigned>(t.size()));
    trace_line(t.empty() ? std::string(b) : std::string(b) + " " + fmt_hex(t));
  };
  if (trace_) {
    for (uint32_t i = 0; i < n; ++i)
      if (!byref[i] && args[i]->type == kString) s_line(*args[i]);
    if (r.type == kString) s_line(r);
    char b[24];
    std::snprintf(b, sizeof b, "N %08x ", site);
    std::string l = std::string(b) + fmt_name(cls) + "." + fmt_name(fn) + " " + std::to_string(n);
    for (uint32_t i = 0; i < n; ++i) {
      if (byref[i]) {
        char x[16];
        std::snprintf(x, sizeof x, " 0200:%08x", base + i);
        l += x;
      } else {
        l += " " + fmt_variant(*args[i]);
      }
    }
    trace_line(l + " -> " + fmt_variant(r));
  }
  if (ReturnLog* log = natives_.return_log()) {  // natives.log v2 (D14): ` S` after a 0x16, ` G` after a game value
    char b[24];
    std::snprintf(b, sizeof b, "%08llu ", static_cast<unsigned long long>(seq));
    std::string l = std::string(b) + fmt_name(cls) + "." + fmt_name(fn) + " " + std::to_string(n) + " -> " + log_value(heap_, r);
    for (uint32_t i = 0; i < n; ++i)
      if (byref[i] && *args[i] != before[i]) l += " W " + std::to_string(i) + " " + log_value(heap_, *args[i]);
    log->line(l);
  }
  for (uint32_t i = 0; i < n; ++i) pop_dispose();  // 0x601c5f: top first
  push(r);
}

// ==== row 0x82
// ConvertVariant (0x60b9d0), used by 0x82-0x84 and 0xb0-0xb2.
Variant Vm::convert(Variant& v, uint16_t target) {
  if (v.type == target) return heap_.duplicate(v);
  switch (target) {
    case kInt:
      if (v.type == kFloat) return make_int(chop_to_int(v.value));  // chop, low 32 bits (0x60bc43-0x60bc6f)
      if (v.type == kStringId) v = to_dynamic_string(v);             // in place (0x60bbf9-0x60bc02)
      if (v.type == kString) return make_int(osi_atoi(heap_.text(v.value)));
      return Variant{kNone, 0};
    case kFloat:
      if (v.type == kInt) return Variant{kFloat, int_to_f32_bits(static_cast<int32_t>(v.value))};
      if (v.type == kStringId) v = to_dynamic_string(v);  // in place (0x60bcf9-0x60bd02)
      if (v.type == kString) {
        bool sw = true;
        const uint32_t bits = osi_atof_bits(heap_.text(v.value), &sw);  // _atof 0x60bd0d, fstp dword 0x60bd13
        if (sw) set_pc64();                                              // ___dec2num 0x65f886 (D8')
        return Variant{kFloat, bits};
      }
      return Variant{kNone, 0};
    case kString:
      if (v.type == kInt) return Variant{kString, heap_.new_string(format_d(static_cast<int32_t>(v.value)))};
      if (v.type == kFloat) return Variant{kString, heap_.new_string(format_f(v.value))};
      if (v.type == kStringId) return to_dynamic_string(v);
      return Variant{kNone, 0};
    default:
      throw VmError("Requested variant conversion unimplimented.  Recompile to debug for types");
  }
}

bool Vm::bcl_82() {  // BCLCallGameFunction 0x6015a0 (self-patch -> memoised per site, D4)
  const uint32_t n = op_u8(5);
  const uint32_t next = cur_pc_ + 6;
  const uint32_t site = cur_pc_;
  const uint8_t op = cur_op_;
  pc_ = cur_pc_ + 5;  // T2a-pc: five incs before the map lookup (the map miss 0x601725 is at +5)
  const std::string cls = str_entry(op_u16(1)), fn = str_entry(op_u16(3));
  if (const NativeEntry* m = memo(site)) {  // the patched 0x84: no lookup, no min/max (0x602bb0)
    pc_ = cur_pc_ + 6;  // 0x84: +6 for the whole body
    try {
      invoke_native(*m, n, cls, fn, next, false, NativeMsgs{nullptr, nullptr, nullptr});
    } catch (...) {
      cur_pc_ = site;
      cur_op_ = op;
      throw VmError(kDirectCallMessage);
    }
    return true;
  }
  const NativeEntry* e = natives_.find(cls, fn);
  if (!e) throw VmError("Call to unregistered Game Function");  // SrOSIError 0x601725, outside the try
  pc_ = cur_pc_ + 6;  // the sixth inc (0x601780): every later error, and the re-raise 0x601dd3, at +6
  try {
    invoke_native(*e, n, cls, fn, next, true,
                  NativeMsgs{"Too many parameters passed to game function", "Too few parameters passed to game function", nullptr});
  } catch (...) {
    cur_pc_ = site;
    cur_op_ = op;
    throw VmError(fmt_ss("Script is calling %s.%s", upto_nul(cls), upto_nul(fn)));  // 0x601dd3
  }
  memo_[site] = e;
  return true;
}
// ==== row 0x83
bool Vm::bcl_83() {  // BCLCallGameFunctionFromString 0x602160 (not self-patching)
  const uint32_t n = op_u8(3);
  const uint32_t next = cur_pc_ + 4;
  const uint32_t site = cur_pc_;
  const uint8_t op = cur_op_;
  pc_ = cur_pc_ + 3;  // T2a-pc: three incs (0x602178, 0x60218a, 0x6021a5) before the name pop
  const std::string cls = str_entry(op_u16(1));
  const Variant s = pop();
  if (!is_str(s)) throw VmError("Invalid member type for function call (must be a string)");  // 0x6022c3
  const std::string fn = name_copy(s);  // kOutOfRange above 255 (0x60226a / 0x602299), before the try: not re-raised
  try {
    pc_ = cur_pc_ + 4;  // the n byte's inc (0x60237c) inside the try: every caught error is re-raised at +4
    const NativeEntry* e = natives_.find(cls, fn);
    if (!e) throw VmError("Call to unregistered Game Function");  // SrOSIError 0x602431, inside the try
    invoke_native(*e, n, cls, fn, next, true,
                  NativeMsgs{"Too many parameters passed to game function", "Too few parameters passed to game function",
                             "Argument type mismatch in call to game function (argument %d [zero based])"});
  } catch (...) {
    cur_pc_ = site;
    cur_op_ = op;
    throw VmError(fmt_ss("Script is calling %s.%s", upto_nul(cls), fn));  // 0x602b24
  }
  heap_.dispose(s);
  return true;
}
// ==== row 0x84
// Reached only through a memoised 0x82 site (D4); its failure text is the patched instruction's (0x60303c).
const char* const Vm::kDirectCallMessage = "Script is calling a game function directly.";
bool Vm::bcl_84() { throw VmError("Direct call without a memoised site"); }  // a literal 0x84 in the file (D10)

// ==== row 0x90
bool Vm::bcl_90() {
  pc_ = cur_pc_ + 1;  // T2a-pc: PC first (error-PC table)
  push(Variant{kArray, heap_.new_array()});
  return true;
}
// ==== row 0x91
bool Vm::bcl_91() {  // BCLGetArrayValue 0x5fd0f0: i's type is not checked, its value is compared unsigned
  pc_ = cur_pc_ + 1;  // T2a-pc: PC first (error-PC table)
  const Variant i = pop(), a = pop();
  if (a.type == kStringTable) throw VmError("Reference beyond end of string table");  // no 0x13 exists: modelled empty
  if (a.type != kArray) throw VmError("array reference on non-array");
  const std::vector<Variant>& el = heap_.elements(a.value);
  if (i.value >= el.size()) throw VmError("Reference beyond start/end of array");
  const Variant d = heap_.duplicate(el[i.value]);
  push(d);
  heap_.dispose(a);
  return true;
}
// ==== row 0x92
bool Vm::bcl_92() {  // BCLElementsInArray 0x5fd2d0
  pc_ = cur_pc_ + 1;  // T2a-pc: PC first (error-PC table)
  const Variant a = pop();
  uint32_t n = 0;
  if (a.type == kArray) n = static_cast<uint32_t>(heap_.elements(a.value).size());
  else if (a.type != kStringTable) throw VmError("array reference on non-array");
  push(make_int(static_cast<int32_t>(n)));
  heap_.dispose(a);
  return true;
}
// ==== row 0x93
bool Vm::bcl_93() {  // BCLSetArrayValue 0x5fd360
  pc_ = cur_pc_ + 1;  // T2a-pc: PC first (error-PC table)
  const Variant v = pop(), i = pop(), a = pop();
  if (a.type == kStringTable) throw VmError("Assigning into string table is illegal");
  if (a.type != kArray) throw VmError("array reference on non-array");
  std::vector<Variant>& el = heap_.elements(a.value);
  if (i.value >= el.size()) throw VmError("Access beyond end of array");
  const Variant old = el[i.value];
  heap_.elements(a.value)[i.value] = v;
  heap_.dispose(old);
  heap_.dispose(a);
  return true;
}
// ==== row 0x94
bool Vm::bcl_94() {  // BCLAppendToArray 0x5fd450
  pc_ = cur_pc_ + 1;  // T2a-pc: PC first (error-PC table)
  const Variant v = pop(), a = pop();
  if (a.type == kStringTable) throw VmError("Assigning into string table is illegal");
  if (a.type != kArray) throw VmError("array reference on non-array");
  heap_.elements(a.value).push_back(v);
  heap_.dispose(a);
  return true;
}
// ==== row 0x95
bool Vm::bcl_95() {  // BCLRemoveFromArray 0x5fd500
  pc_ = cur_pc_ + 1;  // T2a-pc: PC first (error-PC table)
  const Variant i = pop(), a = pop();
  if (a.type == kStringTable) throw VmError("Removal from string table is illegal");
  if (a.type != kArray) throw VmError("array reference on non-array");
  std::vector<Variant>& el = heap_.elements(a.value);
  if (i.value >= el.size()) throw VmError("removal outside of array bounds");
  const Variant old = el[i.value];
  el.erase(el.begin() + i.value);
  heap_.dispose(old);
  heap_.dispose(a);
  return true;
}
// ==== row 0x96
bool Vm::bcl_96() {  // BCLInsertIntoArray 0x5fd5d0: inserting at count is an error (SrOSIError)
  pc_ = cur_pc_ + 1;  // T2a-pc: PC first (error-PC table)
  const Variant v = pop(), i = pop(), a = pop();
  if (a.type == kStringTable) throw VmError("Assigning into string table is illegal");
  if (a.type != kArray) throw VmError("array reference on non-array");
  std::vector<Variant>& el = heap_.elements(a.value);
  if (i.value >= el.size()) throw VmError("insertion outside of array bounds");
  el.insert(el.begin() + i.value, v);
  heap_.dispose(a);
  return true;
}

// ==== shared: colour channels (each row passes its texts)
bool Vm::colour_set(int shift, const char* c_msg, const char* v_msg) {
  pc_ = cur_pc_ + 1;  // T2a-pc: PC first (error-PC table)
  const Variant v = pop(), c = pop();
  if (c.type != kColour) throw VmError(c_msg);
  if (v.type != kInt) throw VmError(v_msg);
  const uint32_t x = v.value > 255 ? 255 : v.value;  // unsigned clamp: negatives -> 255
  push(Variant{kColour, (c.value & ~(0xffu << shift)) | (x << shift)});
  return true;
}
bool Vm::colour_get(int shift, const char* c_msg) {
  pc_ = cur_pc_ + 1;  // T2a-pc: PC first (error-PC table)
  const Variant c = pop();
  if (c.type != kColour) throw VmError(c_msg);
  push(make_int(static_cast<int32_t>((c.value >> shift) & 0xff)));
  return true;
}
// ==== row 0xa0
bool Vm::bcl_a0() { return colour_set(24, "cant access red value of anything but a color", "cant set red to a non-integral value"); }
// ==== row 0xa1
bool Vm::bcl_a1() { return colour_set(16, "cant access green value of anything but a color", "cant set green to a non-integral value"); }
// ==== row 0xa2
bool Vm::bcl_a2() { return colour_set(8, "cant access blue value of anything but a color", "cant set blue to a non-integral value"); }
// ==== row 0xa3
bool Vm::bcl_a3() { return colour_set(0, "cant access alpha value of anything but a color", "cant set alpha to a non-integral value"); }
// ==== row 0xa4
bool Vm::bcl_a4() { return colour_get(24, "cant access red value of anything but a color"); }
// ==== row 0xa5
bool Vm::bcl_a5() { return colour_get(16, "cant access green value of anything but a color"); }
// ==== row 0xa6
bool Vm::bcl_a6() { return colour_get(8, "cant access blue value of anything but a color"); }
// ==== row 0xa7
bool Vm::bcl_a7() { return colour_get(0, "cant access alpha value of anything but a color"); }

// ==== shared: 0xb0-0xb2 (a popped without dispose, converted, then disposed) and the type tests
bool Vm::convert_op(uint16_t target) {
  pc_ = cur_pc_ + 1;  // T2a-pc: PC first (error-PC table)
  Variant a = pop();
  const Variant r = convert(a, target);
  heap_.dispose(a);
  push(r);
  return true;
}
bool Vm::type_test(int which) {
  pc_ = cur_pc_ + 1;  // T2a-pc: PC first (error-PC table)
  const Variant a = pop();
  bool r = false;
  switch (which) {
    case 0: r = a.type == kInt; break;
    case 1: r = a.type == kFloat; break;
    case 2: r = a.type == kStringId || a.type == kString; break;
    case 3: r = a.type == kObject; break;
    case 4: r = a.type >= 0x20 && a.type < 0xff; break;
    default: r = a.type == kArray; break;
  }
  heap_.dispose(a);
  push(make_int(r ? 1 : 0));
  return true;
}
// ==== row 0xb0
bool Vm::bcl_b0() { return convert_op(kString); }
// ==== row 0xb1
bool Vm::bcl_b1() { return convert_op(kFloat); }
// ==== row 0xb2
bool Vm::bcl_b2() { return convert_op(kInt); }
// ==== row 0xb8
bool Vm::bcl_b8() { return type_test(0); }
// ==== row 0xb9
bool Vm::bcl_b9() { return type_test(1); }
// ==== row 0xba
bool Vm::bcl_ba() { return type_test(2); }
// ==== row 0xbb
bool Vm::bcl_bb() { return type_test(3); }
// ==== row 0xbc
bool Vm::bcl_bc() { return type_test(4); }
// ==== row 0xbd
bool Vm::bcl_bd() { return type_test(5); }
// ==== row 0xbf
bool Vm::bcl_bf() {  // BCLGetObjectClassID 0x607350
  pc_ = cur_pc_ + 1;  // T2a-pc: PC first (error-PC table)
  const Variant o = pop();
  uint32_t r;
  if (o.type == kObject) r = heap_.class_id(o.value);
  else if (is_game_type(o.type)) r = o.type + 0x10000u;
  else throw VmError("Cant get classID of non object");
  heap_.dispose(o);
  push(Variant{kInt, r});
  return true;
}
// ==== row 0xff
bool Vm::bcl_ff() {  // BCLHalt 0x603840: pushes a nothing, Run stops
  pc_ = cur_pc_ + 1;  // T2a-pc: PC first (error-PC table)
  push(nothing());
  halted_ = true;
  return false;
}
// ==== unused
bool Vm::bcl_unused() { throw VmError("Call of undefined instruction"); }  // BCLUnused 0x60be60, PC not advanced

}  // namespace lomn::osi
