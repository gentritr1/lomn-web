// M2a-2 T3: the native framework (docs/osi/natives.md §1.2, §2 = D13, §3 = D14, §5.4 = D16).
#include "lomn/osi/natives.h"

#include <cstdio>
#include <utility>

#include "lomn/osi/trace.h"

namespace lomn::osi {

namespace {
std::string seq8(uint64_t seq) {
  char b[24];
  std::snprintf(b, sizeof b, "%08llu", static_cast<unsigned long long>(seq));
  return b;
}
}  // namespace

std::string log_value(Heap& heap, Variant v) {
  std::string s = fmt_variant(v);
  if (v.type == kString && heap.live(kKindString, v.value)) {
    const std::string& t = heap.text(v.value);
    s += " S " + std::to_string(t.size()) + (t.empty() ? std::string() : " " + fmt_hex(t));
  } else if (is_game_type(v.type)) {
    // ` G`: the engine's references at this event, every variant reference excluded (natives.md §3.1)
    s += " G " + std::to_string(heap.live(kKindGame, v.value) ? heap.engine_refs(v.value) : 0);
  }
  return s;
}

HostContext::HostContext(Vm& vm, Heap& heap, const fs::FileSystem* vfs, HostFlags flags, ReturnLog* log)
    : vm_(vm), heap_(heap), vfs_(vfs), flags_(flags), log_(log) {
  heap_.set_free_hook([this](uint32_t h) { on_free(h); });
  vm_.set_native_exit_observer([this](const NativeEntry&, uint64_t) {
    if (by_handle_.empty()) return;  // a stub run mints no host object: nothing to compare
    const std::string m = check_engine_refs();
    if (!m.empty()) throw VmError("Undefined: engine reference count mismatch: " + m);
  });
}

HostContext::~HostContext() {
  heap_.set_free_hook(nullptr);  // the heap may outlive the context; its later frees no longer reach us
  vm_.set_native_exit_observer(nullptr);
}

size_t HostContext::next_subsystem_id() {
  static size_t n = 0;
  return n++;
}

void HostContext::log_line(const std::string& s) {
  if (log_) log_->line(s);
}

GameObject* HostContext::object(uint32_t handle) const {
  auto it = by_handle_.find(handle);
  return it == by_handle_.end() ? nullptr : it->second;
}

Variant HostContext::create_variant(GameObject* o) { return o ? create_variant(o, o->type()) : nothing(); }

// CreateVariant(T*) (e.g. GcTimer 0x445550): null -> {0x0f, 0} (0x445563); otherwise `inc dword [edi+4]` (0x445580),
// then {type, ptr}. The port mints the handle on the first pass (A g, total = engine + 1); later passes add one
// reference to the same handle (D13 identity per object).
Variant HostContext::create_variant(GameObject* o, uint16_t as_type) {
  if (!o) return nothing();
  if (o->handle_ == 0) {
    const uint32_t h = heap_.new_game(o->type_, o->engine_refs_);
    o->handle_ = h;
    by_handle_[h] = o;
  } else {
    heap_.duplicate(Variant{o->type_, o->handle_});  // +1 (a variant reference)
  }
  return Variant{as_type, o->handle_};
}

void HostContext::destroy(GameObject* o) {
  auto it = owned_.find(o);
  if (it != owned_.end()) owned_.erase(it);  // deletes it; an object the context does not own is only forgotten
}

// The heap freed a handle (its total reached 0, `F g` written): delete the host object (the original's Free 0x4f2bc0,
// vtable slot 1, natives.md §2.2). Must not throw: reset_after_error reaches it.
void HostContext::on_free(uint32_t handle) {
  auto it = by_handle_.find(handle);
  if (it == by_handle_.end()) return;  // a stub's handle: no host object
  GameObject* o = it->second;
  by_handle_.erase(it);
  o->handle_ = 0;
  destroy(o);
}

void HostContext::add_ref(GameObject* o) {
  if (!o) throw VmError("Undefined: add_ref on a null object");
  ++o->engine_refs_;
  if (o->handle_ == 0) return;  // not script-visible yet: the minting G carries it (natives.md §3.1)
  char b[40];
  std::snprintf(b, sizeof b, "H %04x:%08x %+d", heap_.game_type(o->handle_), o->handle_, 1);
  log_line(b);
  heap_.add_ref(o->handle_);
}

void HostContext::release(GameObject* o) {
  if (!o) throw VmError("Undefined: release on a null object");
  if (o->engine_refs_ <= 0) throw VmError("Undefined: release of an engine reference the engine does not hold");
  --o->engine_refs_;
  if (o->handle_ == 0) {
    if (o->engine_refs_ == 0) destroy(o);  // never minted, nothing holds it any more: no F g (not script-visible)
    return;
  }
  const uint32_t h = o->handle_;
  char b[40];
  std::snprintf(b, sizeof b, "H %04x:%08x %+d", heap_.game_type(h), h, -1);
  log_line(b);
  heap_.release(h);  // at a total of 0: F g, then on_free deletes `o`
}

CallResult HostContext::engine_call(std::string_view name, const std::vector<Variant>& args) {
  // outside any native the host's own sequence number is the literal -1 (`C -1`, natives.md §3.1); no M2a-2 path on the
  // A9 route makes such a call (Decision 8), but the record is fixed now
  std::string l = "C " + (vm_.in_native() ? seq8(vm_.native_seq()) : std::string("-1")) + " " + fmt_name(name);
  for (size_t i = 0; i < 10; ++i) l += " " + log_value(heap_, i < args.size() ? args[i] : nothing());
  log_line(l);
  return vm_.call(name, args);
}

CallResult HostContext::engine_call_swallowing(std::string_view name, const std::vector<Variant>& args, bool* swallowed) {
  if (swallowed) *swallowed = false;
  if (!vm_.in_native()) {
    // a host route (D16's process entries): the call is the outermost one, so Vm::call has already recorded the error
    // and written its E line; the swallow is the host's: `X -1` (natives.md §3.1, r-m2), nothing reset
    CallResult r = engine_call(name, args);
    if (r.found && !r.ok) {
      log_line("X -1");
      if (swallowed) *swallowed = true;
    }
    return r;
  }
  const uint64_t seq = vm_.native_seq();
  try {
    return engine_call(name, args);
  } catch (const VmError& e) {  // VmError only (review M-3): see natives.h
    vm_.swallow(e);  // D16 rules 1-2: the E line here, the state as at the throw
    log_line("X " + seq8(seq));
    if (swallowed) *swallowed = true;
    CallResult r;
    r.found = true;
    r.ok = false;
    return r;
  }
}

// ---- groups (the explicit list is natives/groups.cpp) ----
size_t native_group_count() {
  size_t n = 0;
  for (const NativeGroup& g : native_groups()) n += g.selftest ? 0 : 1;
  return n;
}

BindReport bind_natives(NativeRegistry& reg, HostContext& ctx) {
  BindReport rep;
  for (const NativeGroup& g : native_groups()) {
    std::vector<NativeFn> before(reg.size());
    for (size_t i = 0; i < reg.size(); ++i) before[i] = reg.entry(i).fn;
    g.bind(reg, ctx);
    ++rep.groups;
    size_t bound = 0;
    for (size_t i = 0; i < reg.size(); ++i)
      if (reg.entry(i).fn && reg.entry(i).fn != before[i]) {
        reg.entry(i).tier = Tier::Implemented;
        ++bound;
      }
    rep.bodies += bound;
    rep.groups_binding += bound ? 1 : 0;
    if (!g.selftest) {  // R-1: the framework's link probe is no game native
      rep.game_bodies += bound;
      rep.game_groups_binding += bound ? 1 : 0;
    }
  }
  return rep;
}

// M-7: the host object's count and the heap's engine part move together only through add_ref/release; a native that
// touches the heap directly would split them.
std::string HostContext::check_engine_refs() const {
  for (const auto& kv : by_handle_) {
    const int32_t heap_engine = heap_.live(kKindGame, kv.first) ? heap_.engine_refs(kv.first) : -1;
    if (kv.second->engine_refs_ != heap_engine) {
      char b[96];
      std::snprintf(b, sizeof b, "handle %08x: host engine count %d, heap engine count %d", kv.first,
                    kv.second->engine_refs_, heap_engine);
      return b;
    }
  }
  return "";
}

}  // namespace lomn::osi
