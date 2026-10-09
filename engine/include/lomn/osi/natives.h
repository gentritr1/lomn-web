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
#include "lomn/sage/blockchain.h"

namespace lomn::fs {
class FileSystem;
}

namespace lomn::osi {

struct HostFlags {
  bool deterministic = true;  // D3: scprocessmanager.userealdeltatime is a logged no-op; the fake-time path stays on
  bool skip_video = true;     // D1: playcinematic completes instantly
};

class HostContext;

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
  // M2a-2 T9a: the engine references this object's deleting destructor drops (natives.md §2.2: Free 0x4f2bc0 calls
  // vtable[0]; e.g. ~ScTextObject 0x611070 releases its font). HostContext calls it once, after the object has left the
  // context and after its `F g`, just before deleting it. It must not throw. Default: none.
  virtual void release_held(HostContext&) {}

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
  std::string check_engine_refs() const;      // the M-7 invariant (see below); "" when it holds

  // The engine's own references (D13). Each change on an object that has a handle writes `H <type>:<handle> %+d` to
  // natives.log at that event (natives.md §3.1: one line per AddRef/Release, always +1 or -1). release() at a total of 0
  // frees the handle (`F g`) and deletes the object; before minting, the last engine release deletes it.
  void add_ref(GameObject* o);
  void release(GameObject* o);

  // Call(name, a0..a9) from inside a native (natives.md §1.2): writes `C <vm.native_seq()> <name> <a0>..<a9>` (the query
  // name; 10 arguments, nothing-padded; ` S` after a 0x16, ` G` after a game value), runs the call and disposes its
  // result. An error propagates out of the native. Outside any native (a host route) the record is `C -1 <name> …`, the
  // literal -1 (natives.md §3.1), and the call is the outermost one (an error is recorded, ok = false).
  CallResult engine_call(std::string_view name, const std::vector<Variant>& args);
  // The same call made where the original catches (D16; only the three GcAreaLoader::LoadArea blocks, natives.md §5.2):
  // an error is swallowed (Vm::swallow: the `E` line at this point, the state left as at the throw), `X <seq>` is
  // written, and the result is {found = true, ok = false}. `*swallowed` says whether that happened. On a host route
  // (no native in flight) the call is the outermost one: Vm::call records the error and writes its E line, and the
  // swallow writes `X -1` (natives.md §3.1, r-m2). Only VmError is swallowed (review M-3): the original's pads also
  // catch(...), but the only non-OSI exception in the port is its own "Undefined: host exception" (a std::exception from
  // host code, conformance rule 15(d): no original counterpart, never compared), which therefore propagates.
  CallResult engine_call_swallowing(std::string_view name, const std::vector<Variant>& args, bool* swallowed = nullptr);

  // One instance per context and type, default-constructed on first use as T(HostContext&).
  template <class T>
  T& subsystem() {
    const size_t id = subsystem_id<T>();
    if (id >= subsystems_.size()) subsystems_.resize(id + 1);
    if (!subsystems_[id]) subsystems_[id] = std::make_shared<T>(*this);
    return *static_cast<T*>(subsystems_[id].get());
  }

  // The host's block-file state (M2a-2 T7a fix 1, m-7): the original keeps the current/second block-file slots
  // ([0x736128] / [0x73612c]) and the Art block file (0x83b0b8) in process-global objects, so there is ONE sage::BlockChain per
  // context. Every native that saves, sets or restores the current block file (character/level/boss loads, ScTileFont,
  // LoadHudSprite) must use this chain, or scslosifileio.doesfileexist would not see its state. Built on first use over
  // the VFS, with Art.blk ("Root/Data/Art/Art.blk", GcGame::LoadArtBlockHeader 0x43b030) opened; nullptr without a VFS.
  sage::BlockChain* block_chain();

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
  std::unique_ptr<sage::BlockChain> blocks_;
  bool blocks_tried_ = false;
};

// Native groups (Tasks 7a/8a/9a). Fix 1 of Task 3 (review I-1): there is NO static-initialisation registration, because
// a self-registering translation unit inside the STATIC lomn_osi archive is dropped by the linker when nothing
// references it (the run would then silently use stubs). Every group is one line of the explicit list in
// src/osi/natives/groups.cpp; bind_natives() calls that list, so a group listed there always reaches the link.
using BindFn = void (*)(NativeRegistry&, HostContext&);
struct NativeGroup {
  const char* name;
  BindFn bind;
  bool selftest;  // the framework's own link probe (natives/selftest.cpp): binds only its test row, never a game native
};
const std::vector<NativeGroup>& native_groups();  // the explicit list, in bind order
size_t native_group_count();                      // the groups that are not selftest
struct BindReport {
  size_t groups = 0;          // groups run
  size_t groups_binding = 0;  // groups that bound at least one body (the selftest group included)
  size_t bodies = 0;          // rows whose body a group bound (the selftest group included)
  // M2a-2 T7a (R-1): the same two counts over the groups that are NOT the framework's selftest. The fail-closed check of
  // --real-natives reads these: the selftest body must never satisfy it.
  size_t game_groups_binding = 0;
  size_t game_bodies = 0;
};
// Runs every listed group against the registry (each binds its natives with `user` = &ctx) and sets Tier::Implemented
// on every row a group bound.
BindReport bind_natives(NativeRegistry& reg, HostContext& ctx);

// The HostContext invariant (review M-7): after every native, each minted host object's engine count equals the heap's
// engine part for its handle. Returns "" or a description of the first mismatch. HostContext checks it after every
// native return and throws VmError("Undefined: engine reference count mismatch: ...") on a mismatch.
// The `<v>` notation of natives.log for one value: the variant, then ` S %d <hex>` for a live 0x16 and ` G %d` for a live
// game handle (natives.md §3.1). Shared by the VM's result lines and the host's `C` lines.
std::string log_value(Heap& heap, Variant v);

}  // namespace lomn::osi
