// M2a-1 T1: lomn::osi::Vm, the OSI script VM (ScOSIVirtualMachine), written from docs/osi/opcodes.md (96 handlers,
// "Engine calls", "After an error"), docs/osi/trace.md and the M2 rulings D4 (memoised call sites, the image is never
// written), D7 (handles), D8' (precision state), D9 (canonical NaN), D10 (errors) and D11 (typed stubs).
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "lomn/osi/heap.h"
#include "lomn/osi/registry.h"
#include "lomn/osi/stack.h"
#include "lomn/osi/trace.h"
#include "lomn/osi/variant.h"
#include "lomn/sage/osi.h"

namespace lomn::osi {

struct VmState {
  uint32_t pc = 0, depth = 0, frame = 0, context_depth = 0;
  int refcount = 0;     // our own bookkeeping (+1 per active call); not compared (the original adds +2 per Call)
  Variant top;          // valid when depth > 0
  bool halted = false;  // the last Run ended on 0xff
  int fpu_pc = 24;      // D8'
};

// found=false: "Unable to locate function %s in scripts" (a Message, not an Error). ok=false: an error was recorded
// (last_error(); the VM state is left exactly as at the throw until reset_after_error()).
// The result is disposed by the host right after the R record (Call 0x60b320 disposes it), so `result` is only its type
// and value; `result_text` is the text of a 0x16 result, read before that disposal.
struct CallResult {
  bool found = false;
  bool ok = false;
  Variant result;
  std::string result_text;
};

class Vm {
 public:
  Vm(const sage::OsiFile& script, NativeRegistry& natives, Heap& heap, TraceSink* trace);

  // Engine -> script call (opcodes.md "Engine calls": Call 0x60b320 / CallWithReturn 0x60b450 / SetPCAndArgs 0x60b5e0).
  // At most 10 arguments; their ownership moves to the VM (pushed or disposed). Re-entrant: a native may call it.
  // Port cap (D10, not original): entering with kMaxEngineNesting calls already active throws
  // "Undefined: engine-call nesting limit (64)", which unwinds like any other native error.
  static constexpr int kMaxEngineNesting = 64;
  CallResult call(std::string_view name, const std::vector<Variant>& args = {});
  // 0x82 memoisation (D4): the native resolved at a call site (file offset), or nullptr.
  const NativeEntry* memo(uint32_t site_pc) const;
  // ConvertVariant (0x60b9d0). A 0x11 source with an int/float target is first replaced IN PLACE by a dynamic string
  // (so `v` may change); the converted value is returned. The caller disposes `v` afterwards when it owns it.
  Variant convert(Variant& v, uint16_t target);

  const VmError* last_error() const { return last_error_ ? &*last_error_ : nullptr; }
  VmState snapshot() const;
  void reset_after_error();  // our host behaviour (D10): pop to the outer call's marker, restore the four registers

  Heap& heap() { return heap_; }
  NativeRegistry& natives() { return natives_; }
  const sage::OsiFile& script() const { return script_; }
  TraceSink* trace() const { return trace_; }
  // A system-wide global by name (case-folded, as GetToken keys it), or nullptr when it does not exist yet.
  const Variant* find_global(std::string_view name) const;
  Variant& global(std::string_view name);  // creates it as nothing when absent (ScOSISystem::GetGlobalVariable 0x609e10)
  uint64_t instructions() const { return instructions_; }
  int fpu_pc() const { return fpu_pc_; }
  uint32_t stack_high_water() const { return high_water_; }
  // Host-side problems met where throwing is not allowed (reset_after_error); never traced.
  const std::vector<std::string>& internal_errors() const { return internal_errors_; }

  // ---- M2a-2 T3: the native framework's view of the VM ----
  // D15: the high-water of active engine calls (Vm::call frames in flight, the outermost included) over the VM's life.
  int max_nesting() const { return max_nesting_; }
  // natives.log sequence numbers (D14, natives.md §3.2) are assigned at native ENTRY. native_seq() is the number of the
  // innermost native executing now (the one a nested `C` record belongs to); in_native() says whether one is (m5).
  uint64_t native_seq() const;
  bool in_native() const { return !active_seq_.empty(); }
  uint64_t natives_called() const { return native_seq_; }  // the next sequence number = natives entered so far
  // FindMemberID 0x609ba0 for a native (parked N3: GetVariantFromBuffer's 0x616519): the same miss-poisoned cache as
  // 0x18/0x19/0x1a.
  uint16_t member_id(std::string_view text);
  // D16 rules 1-2 (natives.md §5.4): a swallowing native caught `e`, raised by its nested engine call. The VM state stays
  // exactly as at the throw; this writes the swallowed error's `E` line from that state (pc/op of the failing
  // instruction, depth at the throw), then `P <pc> 64` if fpu_pc was 24, and records it for swallowed(). It does not set
  // last_error(), does not reset and ends no engine call.
  void swallow(const VmError& e);
  struct Swallowed {
    VmError error;  // message, pc, op, depth as at the throw
    bool has_top = false;
    Variant top;
  };
  // The errors swallowed since the outermost call began (cleared at each outermost call's entry).
  const std::vector<Swallowed>& swallowed() const { return swallowed_; }
  // Called at each native's entry with its registry entry and sequence number (ScriptHost's coverage counts).
  void set_native_observer(std::function<void(const NativeEntry&, uint64_t)> f) { observer_ = std::move(f); }

 private:
  static const char* const kDirectCallMessage;  // 0x84's text, reported at a memoised 0x82 site
  using Handler = bool (Vm::*)();

  CallResult call_impl(std::string_view name, const std::vector<Variant>& args);
  const sage::OsiFunction* find_function(std::string_view name) const;
  void run();
  bool dispatch(uint8_t op);
  void record_error(const VmError& e);
  void set_pc64();  // D8': the switch to 64-bit precision, traced as `P` once
  void push(Variant v);
  Variant pop();
  void pop_dispose();
  void trace_line(const std::string& s);

  // operand reads at cur_pc_ + off (bounds-checked)
  uint8_t op_u8(uint32_t off) const;
  uint16_t op_u16(uint32_t off) const;
  uint32_t op_u32(uint32_t off) const;
  std::string str_entry(uint32_t idx) const;   // string-table text (raw bytes)
  std::string symbol_name(uint32_t idx) const; // symbol-table text
  std::string string_text(Variant s);          // text of a 0x11 or 0x16 variant
  // The name copies of 0x18/0x19/0x1a/0x83 into a local ScFixedString of capacity 0xff (natives.md §8.3): a 0x11 name is
  // read C-string style from the image (strlen from the entry + 1, operator= 0x4f5c00), a 0x16 one by its length
  // (operator= 0x4f5b00); either throws kOutOfRange when the length exceeds 255 (seta 0x4f5c27 / 0x4f5b16).
  std::string name_copy(Variant s);
  std::string error_line(uint32_t pc, uint8_t op, uint32_t depth, const std::string& msg) const;
  Variant to_dynamic_string(Variant v);        // ConvertStringIDToDynamicString (0x60b880), "kOutOfRange" at >= 255
  Variant& var_ref(uint16_t idx);              // 0x21/0x22 addressing
  Variant& frame_slot(uint32_t idx);
  const sage::OsiClass& class_of(uint32_t obj);
  uint32_t find_method(uint32_t obj, uint16_t sym, const char* undefined_msg);  // FindMethod 0x608600
  uint16_t find_member_id(const std::string& name);       // FindMemberID 0x609ba0 (with its miss-poisoned cache)
  static std::string upto_nul_copy(const std::string& s);
  std::string fmt_err(const char* fmt, uint32_t type) const;

  // shared handler bodies (each handler passes its own message literals)
  bool compare(Op op, const char* lhs_msg, const char* rhs_msg);
  bool int_binop(int which, const char* lhs_msg, const char* rhs_msg);
  bool colour_set(int shift, const char* c_msg, const char* v_msg);
  bool colour_get(int shift, const char* c_msg);
  bool type_test(int which);
  bool convert_op(uint16_t target);
  struct NativeMsgs {
    const char* too_many;
    const char* too_few;
    const char* mismatch;  // 0x83's inner "Argument type mismatch ..." (nullptr for 0x82)
  };
  void invoke_native(const NativeEntry& e, uint32_t n, const std::string& cls, const std::string& fn, uint32_t next_pc,
                     bool check_argc, const NativeMsgs& msgs);

#define LOMN_OSI_H(x) bool bcl_##x();
  LOMN_OSI_H(00) LOMN_OSI_H(02) LOMN_OSI_H(03) LOMN_OSI_H(04) LOMN_OSI_H(05) LOMN_OSI_H(06)
  LOMN_OSI_H(10) LOMN_OSI_H(11) LOMN_OSI_H(12) LOMN_OSI_H(13) LOMN_OSI_H(14) LOMN_OSI_H(15) LOMN_OSI_H(16) LOMN_OSI_H(17)
  LOMN_OSI_H(18) LOMN_OSI_H(19) LOMN_OSI_H(1a)
  LOMN_OSI_H(21) LOMN_OSI_H(22) LOMN_OSI_H(23) LOMN_OSI_H(24) LOMN_OSI_H(25)
  LOMN_OSI_H(30) LOMN_OSI_H(31) LOMN_OSI_H(32) LOMN_OSI_H(33) LOMN_OSI_H(34) LOMN_OSI_H(35)
  LOMN_OSI_H(40) LOMN_OSI_H(41) LOMN_OSI_H(42) LOMN_OSI_H(43) LOMN_OSI_H(44) LOMN_OSI_H(45) LOMN_OSI_H(46) LOMN_OSI_H(47)
  LOMN_OSI_H(48) LOMN_OSI_H(49)
  LOMN_OSI_H(50) LOMN_OSI_H(51) LOMN_OSI_H(52) LOMN_OSI_H(53) LOMN_OSI_H(54)
  LOMN_OSI_H(60) LOMN_OSI_H(61) LOMN_OSI_H(62) LOMN_OSI_H(63) LOMN_OSI_H(64)
  LOMN_OSI_H(6a) LOMN_OSI_H(6b) LOMN_OSI_H(6c) LOMN_OSI_H(6d) LOMN_OSI_H(6e) LOMN_OSI_H(6f)
  LOMN_OSI_H(70) LOMN_OSI_H(71) LOMN_OSI_H(72) LOMN_OSI_H(73) LOMN_OSI_H(74) LOMN_OSI_H(75) LOMN_OSI_H(76) LOMN_OSI_H(77)
  LOMN_OSI_H(78) LOMN_OSI_H(7a) LOMN_OSI_H(7b)
  LOMN_OSI_H(80) LOMN_OSI_H(81) LOMN_OSI_H(82) LOMN_OSI_H(83) LOMN_OSI_H(84)
  LOMN_OSI_H(90) LOMN_OSI_H(91) LOMN_OSI_H(92) LOMN_OSI_H(93) LOMN_OSI_H(94) LOMN_OSI_H(95) LOMN_OSI_H(96)
  LOMN_OSI_H(a0) LOMN_OSI_H(a1) LOMN_OSI_H(a2) LOMN_OSI_H(a3) LOMN_OSI_H(a4) LOMN_OSI_H(a5) LOMN_OSI_H(a6) LOMN_OSI_H(a7)
  LOMN_OSI_H(b0) LOMN_OSI_H(b1) LOMN_OSI_H(b2) LOMN_OSI_H(b8) LOMN_OSI_H(b9) LOMN_OSI_H(ba) LOMN_OSI_H(bb) LOMN_OSI_H(bc)
  LOMN_OSI_H(bd) LOMN_OSI_H(bf) LOMN_OSI_H(ff) LOMN_OSI_H(unused)
#undef LOMN_OSI_H

  const sage::OsiFile& script_;
  const std::vector<uint8_t>& img_;
  NativeRegistry& natives_;
  Heap& heap_;
  TraceSink* trace_;
  Stack stack_;
  uint32_t pc_ = 0;
  uint32_t frame_ = 0;
  uint16_t context_depth_ = 0;
  int refcount_ = 0;
  int fpu_pc_ = 24;
  bool halted_ = false;
  uint32_t cur_pc_ = 0;
  uint8_t cur_op_ = 0;
  uint64_t instructions_ = 0;
  uint32_t high_water_ = 0;
  uint64_t native_seq_ = 0;
  std::map<std::string, Variant, std::less<>> globals_;
  // D4 memo: site -> entry. NativeRegistry keeps entries in a deque, so the pointer survives later add() calls.
  std::unordered_map<uint32_t, const NativeEntry*> memo_;
  struct MemberCache {
    int symbol;   // symbol-table index of the cached text (-1: the null pointer of an empty symbol table)
    uint16_t id;  // the cached result (0xffff for a poisoned miss)
  };
  std::vector<MemberCache> member_cache_;
  std::vector<std::string> internal_errors_;
  std::optional<VmError> last_error_;
  int nesting_ = 0;
  int max_nesting_ = 0;
  std::vector<uint64_t> active_seq_;  // sequence numbers of the natives executing now, innermost last
  std::function<void(const NativeEntry&, uint64_t)> observer_;
  std::vector<Swallowed> swallowed_;
  struct Saved {
    uint32_t depth = 0, pc = 0, frame = 0;
    uint16_t context = 0;
    int refcount = 0;     // our own bookkeeping (+1 per active call); not compared (the original adds +2 per Call)
  } saved_;
};

}  // namespace lomn::osi
