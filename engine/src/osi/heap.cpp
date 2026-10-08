// M2a-1 T1: the VM heap (D7) and the stack (D10 caps).
#include "lomn/osi/heap.h"

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
  Obj o;
  o.live = true;
  o.refs = 1;
  o.cls = class_id;
  o.props.assign(nprops, nothing());
  objs_.push_back(std::move(o));
  const uint32_t id = static_cast<uint32_t>(objs_.size());
  alloc_(kKindObject);
  char b[32];
  std::snprintf(b, sizeof b, "A o %08x %04x", id, class_id);
  emit(b);
  return id;
}

uint32_t Heap::new_array() { return new_array(0); }

uint32_t Heap::new_array(uint32_t n) {
  Arr a;
  a.live = true;
  a.refs = 1;
  a.elems.assign(n, nothing());
  arrs_.push_back(std::move(a));
  const uint32_t id = static_cast<uint32_t>(arrs_.size());
  alloc_(kKindArray);
  char b[40];
  std::snprintf(b, sizeof b, "A a %08x %08x", id, n);
  emit(b);
  return id;
}

uint32_t Heap::new_string(std::string_view text) {
  Str s;
  s.live = true;
  s.text.assign(text.data(), text.size());
  strs_.push_back(std::move(s));
  const uint32_t id = static_cast<uint32_t>(strs_.size());
  alloc_(kKindString);
  char b[40];
  std::snprintf(b, sizeof b, "A s %08x %3u", id, static_cast<unsigned>(text.size()));
  emit(b);
  return id;
}

uint32_t Heap::new_game(uint16_t type) {
  Game g;
  g.live = true;
  g.refs = 1;
  g.type = type;
  games_.push_back(g);
  const uint32_t id = static_cast<uint32_t>(games_.size());
  alloc_(kKindGame);
  char b[32];
  std::snprintf(b, sizeof b, "A g %08x %02x", id, type);
  emit(b);
  return id;
}

// A handle that is not live can only come from behaviour the table marks undefined (a stale slot, a fabricated engine
// argument); it is reported instead of touching memory.
Heap::Obj& Heap::obj(uint32_t id) {
  if (id == 0 || id > objs_.size() || !objs_[id - 1].live) throw VmError("Undefined: dangling object handle");
  return objs_[id - 1];
}
Heap::Arr& Heap::arr(uint32_t id) {
  if (id == 0 || id > arrs_.size() || !arrs_[id - 1].live) throw VmError("Undefined: dangling array handle");
  return arrs_[id - 1];
}
Heap::Str& Heap::str(uint32_t id) {
  if (id == 0 || id > strs_.size() || !strs_[id - 1].live) throw VmError("Undefined: dangling string handle");
  return strs_[id - 1];
}
Heap::Game& Heap::game(uint32_t id) {
  if (id == 0 || id > games_.size() || !games_[id - 1].live) throw VmError("Undefined: dangling game handle");
  return games_[id - 1];
}

bool Heap::live(HeapKind k, uint32_t id) const {
  if (id == 0) return false;
  switch (k) {
    case kKindObject: return id <= objs_.size() && objs_[id - 1].live;
    case kKindArray: return id <= arrs_.size() && arrs_[id - 1].live;
    case kKindString: return id <= strs_.size() && strs_[id - 1].live;
    default: return id <= games_.size() && games_[id - 1].live;
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
    std::string copy = str(v.value).text;
    return Variant{kString, new_string(copy)};
  }
  // 0x13 (a string table) is never created (opcodes.md variant types): nothing to count.
  return v;
}

void Heap::dispose(const Variant& v) {
  if (v.type == kObject) {
    Obj& o = obj(v.value);
    if (--o.refs > 0) return;
    o.live = false;
    std::vector<Variant> props = std::move(o.props);
    o.props.clear();
    free_(kKindObject, v.value);
    for (size_t i = props.size(); i-- > 0;) dispose(props[i]);  // dtor 0x6085c0: back to front
  } else if (v.type == kArray) {
    Arr& a = arr(v.value);
    if (--a.refs > 0) return;
    a.live = false;
    std::vector<Variant> el = std::move(a.elems);
    a.elems.clear();
    free_(kKindArray, v.value);
    for (size_t i = el.size(); i-- > 0;) dispose(el[i]);  // dtor 0x5fc790: back to front
  } else if (v.type == kString) {
    Str& s = str(v.value);
    s.live = false;
    s.text.clear();
    s.text.shrink_to_fit();
    free_(kKindString, v.value);
  } else if (is_game_type(v.type)) {
    Game& g = game(v.value);
    if (--g.refs > 0) return;
    g.live = false;
    free_(kKindGame, v.value);
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
std::string& Heap::text(uint32_t id) { return str(id).text; }
uint16_t Heap::class_id(uint32_t id) const { return const_cast<Heap*>(this)->obj(id).cls; }
uint16_t Heap::game_type(uint32_t id) const { return const_cast<Heap*>(this)->game(id).type; }

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

uint32_t Stack::find_frame() const {
  for (uint32_t i = top_; i-- > 0;)
    if (s_[i].type == kRetMarker || s_[i].type == kFrameMarker) return i + 1;
  return 0;
}

}  // namespace lomn::osi
