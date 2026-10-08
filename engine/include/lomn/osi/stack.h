// M2a-1 T1: the VM stack - 1,000 slots of 8 bytes, as the VM ctor 0x60b230 builds it. The original's Push (0x5fd0c0)
// and Pop (0x5fd260) check nothing; ours raise D10's two errors instead of writing outside the slots.
#pragma once
#include <array>
#include <cstdint>

#include "lomn/osi/variant.h"

namespace lomn::osi {

class Heap;

constexpr uint32_t kStackSlots = 1000;

class Stack {
 public:
  void push(Variant v);                          // "Stack overflow (1000 slots)" at depth 1000
  Variant pop();                                 // Pop(false): moves the slot out; "Stack underflow" at depth 0
  void pop_dispose(Heap& heap);                  // Pop(true)
  void insert_below(uint32_t n, Variant marker); // 0x50/0x51: the top n move up one slot, marker goes below them
  uint32_t depth() const { return top_; }
  Variant& at(uint32_t i);                       // slot i (0 = bottom), i < depth
  Variant& top();                                // the top slot; "Stack underflow" at depth 0
  const Variant& top() const;
  // The frame base: the first slot scanning down from the top whose type is 2 or 0xff, plus one (0 when none).
  uint32_t find_frame() const;

 private:
  std::array<Variant, kStackSlots> s_{};
  uint32_t top_ = 0;
};

}  // namespace lomn::osi
