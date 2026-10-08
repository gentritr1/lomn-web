// M2a-1 T1: the native registry (data/osi/natives.json winners + data/osi/natives_beta150.json mod rows, D2/D12) and the
// game-type table (data/osi/types.json). Natives are found by their case-folded (A-Z) token pair, as RegisterFunction
// (0x5fab30) keys them. A native that no binding replaces is a typed stub (D11).
#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lomn/osi/variant.h"

namespace lomn::osi {

class Vm;
class Heap;
struct NativeEntry;
struct TraceSink;

// What a native sees. `args` holds exactly argc slot pointers, deepest first, already converted in place; a by-reference
// (0x200) parameter gets the SAME pointer (the original passes the stack slot's address, never the box, 0x60198c-0x601abc).
struct NativeCall {
  Vm& vm;
  const NativeEntry& entry;
  uint32_t site_pc;
  std::vector<Variant*> args;
};
using NativeFn = Variant (*)(NativeCall&, void* user);
enum class Tier : uint8_t { Stub, Implemented, Mod, Unknown };
enum class RetKind : uint8_t { None, Int, Float, String, Colour, Game, Copy };

struct NativeEntry {
  std::string cls, name, key;          // names as registered; key = case-folded "cls.name"
  uint8_t min = 0, max = 0;
  std::array<uint16_t, 10> types{};    // declared argument types (0x0f for unused slots)
  RetKind ret = RetKind::None;
  uint16_t ret_type = 0;               // the game type id for RetKind::Game
  std::optional<Variant> default_ret;  // the per-row `default` (D11) - beats the kind default
  NativeFn fn = nullptr;               // nullptr = the typed stub
  void* user = nullptr;
  Tier tier = Tier::Stub;
  bool sets_fpu_pc64 = false;          // D8': gcscripttools.stringtofloat (registry flag), or a test native
};

class TypeTable {
 public:
  static TypeTable load(std::string_view json, std::string* err = nullptr);
  // The id of a registry type name: "int", "float", "string", "colour", "colour16", "string-id", "byref", "nothing",
  // "unknown:0x0", "type:<Class>"; a bare "<Class>" is looked up as "type:<Class>". 0xffff when unknown.
  uint16_t id(std::string_view name) const;
  const std::string& name(uint16_t id) const;  // "" when unknown

 private:
  std::map<std::string, uint16_t, std::less<>> ids_;
  std::map<uint16_t, std::string> names_;
};

// natives.log (trace.md §6) receives one line per native call; it is a line sink like the trace.
using ReturnLog = TraceSink;

class NativeRegistry {
 public:
  NativeRegistry() = default;
  // natives.json winners, then the mod rows (an exe key wins over a mod row with the same key). Throws std::runtime_error
  // on a malformed table.
  static NativeRegistry load(std::string_view natives_json, std::string_view mods_json, const TypeTable& types);
  const NativeEntry* find(std::string_view cls, std::string_view fn) const;  // case-folded token pair (A-Z only)
  // key "cls.name" (split at the first '.'); unknown key -> std::logic_error
  void bind(std::string_view key, NativeFn fn, void* user, Tier tier);
  void add(NativeEntry e);                                                    // tests, the conformance runner (replaces)
  // D11: default_ret if set; else none -> nothing, int -> 0, float -> 0.0, string -> "" (0x16), colour -> 0,
  // game -> a fresh game handle of ret_type, copy -> Duplicate of argument 0.
  Variant stub_return(const NativeEntry& e, Heap& heap, NativeCall& call);
  void set_return_log(ReturnLog* log) { log_ = log; }
  ReturnLog* return_log() const { return log_; }
  size_t size() const { return entries_.size(); }

  static std::string fold(std::string_view s);  // A-Z -> a-z, every other byte unchanged

 private:
  // A deque: references to entries (the VM's memo, a NativeCall's `entry`) stay valid when later entries are added.
  std::deque<NativeEntry> entries_;
  // keyed on the folded (class, name) token pair, as RegisterFunction's ScOSIDualToken map is
  std::map<std::pair<std::string, std::string>, size_t> index_;
  ReturnLog* log_ = nullptr;
};

// The registry vocabulary of a declared type name (types.md) -> type id; 0xffff when unknown.
uint16_t type_id_of(std::string_view name, const TypeTable& types);

}  // namespace lomn::osi
