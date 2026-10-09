// M2a-2 T3: the native framework (docs/osi/natives.md §1.2 "The port's native ABI", §2 game objects = D13, §3
// natives.log v2 = D14, §5.4 the swallowed nested error = D16). A native body is a NativeFn whose `user` is the
// HostContext; it reads converted arguments from NativeCall, returns one Variant, and touches the VM only through the
// calls below (never the stack, pc, frame_ptr, context_depth or fpu_pc; never a host pointer in a variant, D7).
#pragma once
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "lomn/osi/heap.h"
#include "lomn/osi/registry.h"
#include "lomn/osi/variant.h"
#include "lomn/osi/vm.h"

namespace lomn::fs {
class FileSystem;
}

namespace lomn::osi {

struct HostFlags {
  bool deterministic = true;  // D3: scprocessmanager.userealdeltatime is a logged no-op; the fake-time path stays on
  bool skip_video = true;     // D1: playcinematic completes instantly
};

// One engine object the script can hold (D13). `engine_refs` is the number of references the engine holds when the
// object is constructed (cited per class in natives.md §2.4: 0 for an object the native constructs, the holders alive
// at the call for an engine-owned one). The handle is minted on the first create_variant, never at construction.
class GameObject {
 public:
  GameObject(uint16_t type, int32_t engine_refs) : type_(type), engine_refs_(engine_refs) {}
  virtual ~GameObject() = default;
  GameObject(const GameObject&) = delete;
  GameObject& operator=(const GameObject&) = delete;
  uint16_t type() const { return type_; }      // the game type the handle is minted under (its `A g` line)
  uint32_t handle() const { return handle_; }  // 0 until the first create_variant
  int32_t engine_refs() const { return engine_refs_; }  // the engine's references now (natives.log's ` G`)

 private:
  friend class HostContext;
  uint16_t type_;
  uint32_t handle_ = 0;
  int32_t engine_refs_;
};

class HostContext {
 public:
  HostContext(Vm& vm, Heap& heap, const fs::FileSystem* vfs, HostFlags flags, ReturnLog* log);
  ~HostContext();
  HostContext(const HostContext&) = delete;
  HostContext& operator=(const HostContext&) = delete;

  // Creates a host object owned by this context. It is deleted when its total count reaches 0 after minting (`F g`), or
  // when the engine releases its last reference before any handle was minted, or with the context.
  template <class T, class... A>
  T* make(A&&... a) {
    auto p = std::make_unique<T>(std::forward<A>(a)...);
    T* raw = p.get();
    owned_.emplace(static_cast<GameObject*>(raw), std::move(p));
    return raw;
  }

  // CreateVariant(T*) (natives.md §2.1): nullptr -> nothing; otherwise +1 reference (the overload's `inc [edi+4]`) and
  // the handle is minted on first use (`A g`, total = the engine's references + 1). One handle per live object; the
  // variant's type tag is `as_type` (the native's declared return, natives.md §2.3), the object's type by default.
  Variant create_variant(GameObject* o);
  Variant create_variant(GameObject* o, uint16_t as_type);

  // Argument i as a host object (natives.md §1.2): nothing -> nullptr. Unreachable after 0x82's ConvertVariant (a wrong
  // game type never reaches the native): asserts in debug, VmError("Undefined: object_arg type") in release (m3). A game
  // handle with no host object (a typed stub's fresh handle, D11) is VmError("Undefined: object_arg without a host
  // object").
  template <class T>
  T* object_arg(NativeCall& c, size_t i) {
    if (i >= c.args.size()) throw VmError("Undefined: object_arg index past argc");
    const Variant v = *c.args[i];
    if (v.type == kNothing) return nullptr;
    GameObject* o = is_game_type(v.type) ? object(v.value) : nullptr;
    if (!o && is_game_type(v.type)) throw VmError("Undefined: object_arg without a host object");
    T* t = dynamic_cast<T*>(o);
    if (!t) {
      assert(!"object_arg type");
      throw VmError("Undefined: object_arg type");
    }
    return t;
  }
  GameObject* object(uint32_t handle) const;  // the live host object of a game handle, else nullptr

  // The engine's own references (D13). Each change on an object that has a handle writes `H <type>:<handle> %+d` to
  // natives.log at that event (natives.md §3.1: one line per AddRef/Release, always +1 or -1). release() at a total of 0
  // frees the handle (`F g`) and deletes the object; before minting, the last engine release deletes it.
  void add_ref(GameObject* o);
  void release(GameObject* o);

  // Call(name, a0..a9) from inside a native (natives.md §1.2): writes `C <vm.native_seq()> <name> <a0>..<a9>` (the query
  // name; 10 arguments, nothing-padded; ` S` after a 0x16, ` G` after a game value), runs the call and disposes its
  // result. An error propagates out of the native.
  CallResult engine_call(std::string_view name, const std::vector<Variant>& args);
  // The same call made where the original catches (D16; only the three GcAreaLoader::LoadArea blocks, natives.md §5.2):
  // an error is swallowed (Vm::swallow: the `E` line at this point, the state left as at the throw), `X <seq>` is
  // written, and the result is {found = true, ok = false}. `*swallowed` says whether that happened.
  CallResult engine_call_swallowing(std::string_view name, const std::vector<Variant>& args, bool* swallowed = nullptr);

  // One instance per context and type, default-constructed on first use as T(HostContext&).
  template <class T>
  T& subsystem() {
    const size_t id = subsystem_id<T>();
    if (id >= subsystems_.size()) subsystems_.resize(id + 1);
    if (!subsystems_[id]) subsystems_[id] = std::make_shared<T>(*this);
    return *static_cast<T*>(subsystems_[id].get());
  }

  const HostFlags& flags() const { return flags_; }
  const fs::FileSystem* vfs() const { return vfs_; }
  Vm& vm() { return vm_; }
  Heap& heap() { return heap_; }
  ReturnLog* log() const { return log_; }

 private:
  static size_t next_subsystem_id();
  template <class T>
  static size_t subsystem_id() {
    static const size_t id = next_subsystem_id();
    return id;
  }
  void on_free(uint32_t handle);  // the heap's free hook
  void log_line(const std::string& s);
  void destroy(GameObject* o);

  Vm& vm_;
  Heap& heap_;
  const fs::FileSystem* vfs_;
  HostFlags flags_;
  ReturnLog* log_;
  std::unordered_map<GameObject*, std::unique_ptr<GameObject>> owned_;
  std::unordered_map<uint32_t, GameObject*> by_handle_;
  std::vector<std::shared_ptr<void>> subsystems_;
};

// Native groups (Tasks 7a/8a/9a) self-register at static initialisation; bind_natives runs every registered group
// against the registry (each binds its natives with `user` = &ctx) and sets Tier::Implemented on every row bound.
using BindFn = void (*)(NativeRegistry&, HostContext&);
void register_native_group(const char* name, BindFn fn);
void bind_natives(NativeRegistry& reg, HostContext& ctx);
size_t native_group_count();

// The `<v>` notation of natives.log for one value: the variant, then ` S %d <hex>` for a live 0x16 and ` G %d` for a live
// game handle (natives.md §3.1). Shared by the VM's result lines and the host's `C` lines.
std::string log_value(Heap& heap, Variant v);

}  // namespace lomn::osi
