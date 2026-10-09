// M2a-1 T1: the VM heap (D7) and the stack (D10 caps).
#include "lomn/osi/heap.h"

#include <cstdint>
#include <cstdio>
#include <utility>

#include "lomn/osi/stack.h"
#include "lomn/osi/trace.h"
#include "lomn/sage/osi.h"

namespace lomn::osi {

namespace {
const char kKindChar[4] = {'o', 'a', 's', 'g'};
}

void Heap::emit(const char* s) {
  if (trace_) trace_->line(s);
}

void Heap::alloc_(HeapKind k) {
  ++stats_.allocs[k];
  if (++stats_.live[k] > stats_.peak[k]) stats_.peak[k] = stats_.live[k];
}

void Heap::free_(HeapKind k, uint32_t id) {
  --stats_.live[k];
  char b[32];
  std::snprintf(b, sizeof b, "F %c %08x", kKindChar[k], id);
  emit(b);
}

uint32_t Heap::new_object(uint16_t class_id, uint32_t nprops) {
  const uint32_t id = mint_(kKindObject);
  Obj& o = objs_[id];
  o.refs = 1;
  o.cls = class_id;
  o.props.assign(nprops, nothing());
  alloc_(kKindObject);
  char b[32];
  std::snprintf(b, sizeof b, "A o %08x %04x", id, class_id);
  emit(b);
  return id;
}

uint32_t Heap::new_array() { return new_array(0); }

uint32_t Heap::new_array(uint32_t n) {
  const uint32_t id = mint_(kKindArray);
  Arr& a = arrs_[id];
  a.refs = 1;
  a.elems.assign(n, nothing());
  alloc_(kKindArray);
  char b[40];
  std::snprintf(b, sizeof b, "A a %08x %08x", id, n);
  emit(b);
  return id;
}

uint32_t Heap::new_string(std::string_view text) {
  const uint32_t id = mint_(kKindString);
  strs_[id].assign(text.data(), text.size());
  alloc_(kKindString);
  char b[40];
  std::snprintf(b, sizeof b, "A s %08x %3u", id, static_cast<unsigned>(text.size()));
  emit(b);
  return id;
}

uint32_t Heap::new_game(uint16_t type, int32_t engine_refs) {
  const uint32_t id = mint_(kKindGame);
  Game& g = games_[id];
  g.refs = engine_refs + 1;  // the engine's references + the returned variant (D13)
  g.engine = engine_refs;
  g.type = type;
  alloc_(kKindGame);
  char b[32];
  std::snprintf(b, sizeof b, "A g %08x %02x", id, type);
  emit(b);
  return id;
}

// D13 engine references (natives.md §2.2): the engine's AddRef/Release move the total and the engine part together.
void Heap::add_ref(uint32_t h) {
  Game& g = game(h);
  ++g.refs;
  ++g.engine;
}

void Heap::release(uint32_t h) {
  Game& g = game(h);
  if (g.engine <= 0) throw VmError("Undefined: release of an engine reference the engine does not hold");
  --g.engine;
  if (--g.refs > 0) return;
  games_.erase(h);
  free_(kKindGame, h);
  if (free_hook_) free_hook_(h);
}

int32_t Heap::engine_refs(uint32_t h) const { return game(h).engine; }
int32_t Heap::refs(uint32_t h) const { return game(h).refs; }

// D7 handles never wrap (parked N4): the last one minted is UINT32_MAX - 1, so handle 0 never appears.
uint32_t Heap::mint_(HeapKind k) {
  if (next_[k] == UINT32_MAX) throw VmError("Undefined: handle space exhausted");
  return next_[k]++;
}

// A handle that is not live can only come from behaviour the table marks undefined (a stale slot, a fabricated engine
// argument); it is reported instead of touching memory.
Heap::Obj& Heap::obj(uint32_t id) {
  auto it = objs_.find(id);
  if (it == objs_.end()) throw VmError("Undefined: dangling object handle");
  return it->second;
}
const Heap::Obj& Heap::obj(uint32_t id) const {
  auto it = objs_.find(id);
  if (it == objs_.end()) throw VmError("Undefined: dangling object handle");
  return it->second;
}
Heap::Arr& Heap::arr(uint32_t id) {
  auto it = arrs_.find(id);
  if (it == arrs_.end()) throw VmError("Undefined: dangling array handle");
  return it->second;
}
std::string& Heap::str(uint32_t id) {
  auto it = strs_.find(id);
  if (it == strs_.end()) throw VmError("Undefined: dangling string handle");
  return it->second;
}
Heap::Game& Heap::game(uint32_t id) {
  auto it = games_.find(id);
  if (it == games_.end()) throw VmError("Undefined: dangling game handle");
  return it->second;
}
const Heap::Game& Heap::game(uint32_t id) const {
  auto it = games_.find(id);
  if (it == games_.end()) throw VmError("Undefined: dangling game handle");
  return it->second;
}

bool Heap::live(HeapKind k, uint32_t id) const {
  switch (k) {
    case kKindObject: return objs_.count(id) != 0;
    case kKindArray: return arrs_.count(id) != 0;
    case kKindString: return strs_.count(id) != 0;
    default: return games_.count(id) != 0;
  }
}

Variant Heap::duplicate(Variant v) {
  if (v.type == kObject) {
    ++obj(v.value).refs;
  } else if (v.type == kArray) {
    ++arr(v.value).refs;
  } else if (is_game_type(v.type)) {
    ++game(v.value).refs;
  } else if (v.type == kString) {
    std::string copy = str(v.value);
    return Variant{kString, new_string(copy)};
  }
  // 0x13 (a string table) is never created (opcodes.md variant types): nothing to count.
  return v;
}

void Heap::dispose(const Variant& root) {
  // Depth-first like the recursive destructors (0x6085c0, 0x5fc790): a freed object/array writes its F record, then its
  // children are disposed back to front, each completely before the next. LIFO: pushing children front to back makes the
  // LAST child the next one processed.
  std::vector<Variant> work{root};
  while (!work.empty()) {
    const Variant v = work.back();
    work.pop_back();
    if (v.type == kObject) {
      auto it = objs_.find(v.value);
      if (it == objs_.end()) throw VmError("Undefined: dangling object handle");
      if (--it->second.refs > 0) continue;
      std::vector<Variant> props = std::move(it->second.props);
      objs_.erase(it);
      free_(kKindObject, v.value);
      work.insert(work.end(), props.begin(), props.end());
    } else if (v.type == kArray) {
      auto it = arrs_.find(v.value);
      if (it == arrs_.end()) throw VmError("Undefined: dangling array handle");
      if (--it->second.refs > 0) continue;
      std::vector<Variant> el = std::move(it->second.elems);
      arrs_.erase(it);
      free_(kKindArray, v.value);
      work.insert(work.end(), el.begin(), el.end());
    } else if (v.type == kString) {
      auto it = strs_.find(v.value);
      if (it == strs_.end()) throw VmError("Undefined: dangling string handle");
      strs_.erase(it);
      free_(kKindString, v.value);
    } else if (is_game_type(v.type)) {
      auto it = games_.find(v.value);
      if (it == games_.end()) throw VmError("Undefined: dangling game handle");
      if (--it->second.refs > 0) continue;
      games_.erase(it);
      free_(kKindGame, v.value);
      if (free_hook_) free_hook_(v.value);  // the host deletes its object after the F record (D13)
    }
  }
}

Variant* Heap::property(uint32_t id, uint16_t symbol, const sage::OsiFile& script) {
  Obj& o = obj(id);
  if (o.cls >= script.classes().size()) return nullptr;
  const auto& props = script.classes()[o.cls].props;
  for (size_t i = 0; i < props.size() && i < o.props.size(); ++i)
    if (props[i] == symbol) return &o.props[i];
  return nullptr;
}

std::vector<Variant>& Heap::elements(uint32_t id) { return arr(id).elems; }
std::string& Heap::text(uint32_t id) { return str(id); }
uint16_t Heap::class_id(uint32_t id) const { return obj(id).cls; }
uint16_t Heap::game_type(uint32_t id) const { return game(id).type; }

// ---- Stack ----
void Stack::push(Variant v) {
  if (top_ >= kStackSlots) throw VmError("Stack overflow (1000 slots)");
  s_[top_++] = v;
}

Variant Stack::pop() {
  if (top_ == 0) throw VmError("Stack underflow");
  Variant v = s_[--top_];
  s_[top_] = nothing();  // popped slots read as nothing (the original keeps stale bits; reading them is undefined)
  return v;
}

void Stack::pop_dispose(Heap& heap) {
  Variant v = pop();
  heap.dispose(v);
}

void Stack::insert_below(uint32_t n, Variant marker) {
  if (n > top_) throw VmError("Stack underflow");
  if (top_ >= kStackSlots) throw VmError("Stack overflow (1000 slots)");
  for (uint32_t i = top_; i > top_ - n; --i) s_[i] = s_[i - 1];
  s_[top_ - n] = marker;
  ++top_;
}

Variant& Stack::at(uint32_t i) {
  if (i >= top_) throw VmError("Undefined: stack slot above the top");
  return s_[i];
}

Variant& Stack::top() {
  if (top_ == 0) throw VmError("Stack underflow");
  return s_[top_ - 1];
}
const Variant& Stack::top() const {
  if (top_ == 0) throw VmError("Stack underflow");
  return s_[top_ - 1];
}

uint32_t Stack::find_frame() const {
  for (uint32_t i = top_; i-- > 0;)
    if (s_[i].type == kRetMarker || s_[i].type == kFrameMarker) return i + 1;
  return 0;
}

}  // namespace lomn::osi
