// M2a-1 T1: the VM heap (D7: handles, never pointers). Objects (0x01), arrays (0x05), dynamic strings (0x16) and game
// objects (0x20..0xfe) live here; a variant holds a uint32_t handle from a per-kind monotonic counter starting at 1,
// never reused. Reference counts are explicit (opcodes.md "Pop and ownership", "Objects, arrays and strings").
// Every allocation writes an `A` record and every free an `F` record to the trace (trace.md §2).
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "lomn/osi/variant.h"

namespace lomn::sage {
class OsiFile;
}

namespace lomn::osi {

struct TraceSink;

enum HeapKind : int { kKindObject = 0, kKindArray = 1, kKindString = 2, kKindGame = 3 };

struct HeapStats {
  std::array<uint64_t, 4> live{};    // o, a, s, g
  std::array<uint64_t, 4> allocs{};  // total allocations per kind
  std::array<uint64_t, 4> peak{};    // peak live per kind
};

class Heap {
 public:
  explicit Heap(TraceSink* t = nullptr) : trace_(t) {}
  void set_trace(TraceSink* t) { trace_ = t; }

  uint32_t new_object(uint16_t class_id, uint32_t nprops);  // refcount 1, every property nothing
  uint32_t new_array();                                     // empty, refcount 1 (ctor 0x5fc690)
  uint32_t new_array(uint32_t n_nothings);                  // n nothings, refcount 1 (ctor 0x5fc710)
  uint32_t new_string(std::string_view text);               // a 0x16 string (no length cap here; callers apply theirs)
  uint32_t new_game(uint16_t type);                         // a game handle (natives, stubs), refcount 1

  // Duplicate (0x60aa20): +1 reference for 0x01, 0x05, 0x20..0xfe; a deep copy (new handle) for 0x16; nothing else.
  Variant duplicate(Variant v);
  // Dispose (0x60a930): -1 reference (free at 0) for 0x01, 0x05, 0x20..0xfe; 0x16 deletes the string. An object frees
  // its properties and an array its elements back to front, after its own `F` record (depth first, as the recursive
  // destructors do), using an explicit worklist rather than recursion.
  void dispose(const Variant& v);

  // GetProperty (0x6086a0): the first property whose symbol is `symbol` in the object's class record, else nullptr.
  Variant* property(uint32_t obj, uint16_t symbol, const sage::OsiFile& script);
  std::vector<Variant>& elements(uint32_t arr);
  std::string& text(uint32_t str);
  uint16_t class_id(uint32_t obj) const;
  uint16_t game_type(uint32_t h) const;
  bool live(HeapKind k, uint32_t id) const;
  HeapStats stats() const { return stats_; }
  // Values held right now: live ones only (freed entries are erased; ids stay monotonic and are never reused, D7).
  size_t stored() const { return objs_.size() + arrs_.size() + strs_.size() + games_.size(); }

 private:
  struct Obj {
    int32_t refs = 0;
    uint16_t cls = 0;
    std::vector<Variant> props;
  };
  struct Arr {
    int32_t refs = 0;
    std::vector<Variant> elems;
  };
  struct Game {
    int32_t refs = 0;
    uint16_t type = 0;
  };
  Obj& obj(uint32_t id);
  Arr& arr(uint32_t id);
  std::string& str(uint32_t id);
  Game& game(uint32_t id);
  const Obj& obj(uint32_t id) const;
  const Game& game(uint32_t id) const;
  void alloc_(HeapKind k);
  void free_(HeapKind k, uint32_t id);
  void emit(const char* s);

  TraceSink* trace_ = nullptr;
  // live values only, keyed by handle (node-based: references stay valid while other handles come and go)
  std::unordered_map<uint32_t, Obj> objs_;
  std::unordered_map<uint32_t, Arr> arrs_;
  std::unordered_map<uint32_t, std::string> strs_;
  std::unordered_map<uint32_t, Game> games_;
  std::array<uint32_t, 4> next_{{1, 1, 1, 1}};  // per-kind monotonic ids (D7)
  HeapStats stats_;
};

}  // namespace lomn::osi
