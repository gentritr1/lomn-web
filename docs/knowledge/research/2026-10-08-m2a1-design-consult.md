# M2a-1 design consult — VM core, Python oracle, conformance, differential, mutation

Consultant: Fable 5.1 · 2026-10-08 · Repo `lomn-web-m2`, branch `m2-osi-vm` @ `dacc7b6` (read only; no product code, no commits).
Inputs: spec §5/§7 (A5–A7, D1–D6), `docs/osi/opcodes.md` (all 621 lines), `docs/osi/types.md`, `data/osi/natives.json`, the M2a-0 ledger, `engine/{include,src}/…/osi.*`, `engine/tests/osi_builder.h`, `fuzz_osi.cpp`, `tools/research/osi.py`, `osi_craft.py`, `oracle/mutate4.py`, `engine/CMakeLists.txt`, `Makefile`, `docs/knowledge/process-experiments.md`, probe 3, the consult.
Labels: **EXECUTED** = a command I ran in this session, **INFERRED** = read from code/docs/asm, not run, **ESTIMATE** = a guess with a stated basis.

## 0. Measured inputs (every number below that drives a threshold, with the command)

All EXECUTED on this host (load 8–13 during the session), Python 3.14.2, numpy 2.5.1, Apple clang 21, node 20.19.4, `em++` only via `.toolchain` (not on PATH).

| # | Fact | Value | Command (scratch, not committed) |
|---|---|---|---|
| M1 | `Base.osi` used | 486,274 B, sha256 `92a5cec5f35afd2e…`, at `../lomn-data/install/data/Base.osi` (`LOMN_DATA` unset in this shell) | `shasum -a 256` |
| M2 | opcode rows / named handlers / `BCLUnused` | 256 / 96 / 160 | regex over the `opcodes.md` table |
| M3 | named handlers with 0 uses in `Base.osi` (the "29 unused") | 29: `00 02 03 04 05 06 18 1a 24 25 31 32 34 41 49 6f 74 78 7a 7b 80 81 83 84 b8 b9 bc bf ff` | same |
| M4 | named rows with ≥1 error path / with none / always-error | 57 / 39 / 3 (`02 03 41`) | same |
| M5 | distinct quoted error messages over the 96 rows; row×message pairs | 81; 114 | same |
| M6 | `Base.osi`: strings / globals / functions / classes / symbols | 2,749 / 8 / 23 / 395 / 2,516 | `osi.py` parse |
| M7 | entry points (name, declared argc) | `setup` 0, `process` 3, `input` 5, `onevent` 10; the other 19 functions argc 0 | same |
| M8 | instructions (dedup by offset) / distinct opcodes / `0x82` sites / distinct keys | 151,522 / 67 / 4,402 / 728 (= spec §2) | `osi.disasm` over `subroutines()` |
| M9 | `0x82` followed directly by `0x53` / by `0x30` | 48 / 2,160 | same |
| M10 | max operand: `0x33` n 17, `0x23` n 20, `0x14` n 17, `0x50` n 0, `0x51` n 17 | — | same |
| M11 | longest string-table entry | 145 chars (0 entries ≥ 200) | same |
| M12 | `setup` / `process` / `input` / `onevent` bodies | 56 / 38 / 72 / 37 instructions | same |
| M13 | static over-approximation of code reachable from each entry (method symbol → every method with that symbol) | setup 2,295 subs / 112,844 insns / 3,776 call sites; process 2,607 / 121,915 / 3,882 | scratch reachability walk |
| M14 | `0x82` sites whose key declares a game-type parameter | 453 keys, 1,068 of 4,402 sites | registry × walk |
| M15 | first native on the static `initfull` path needing a game-object argument | `sctilefont.setcolor` in `global_class.initfull`, the **5th** native call of `Setup` (after 4× `sctilefont.create`) | linear walk of the consult's initfull chain |
| M16 | unregistered keys on that same static path | 2 (`bettersaver.getbooleanvalue`, `bettersaver.setbooleanvalue` in `gs_lego_screenfrontend.initfull`) | same |
| M17 | registry winners by **return kind** (the single `CreateVariant` overload the body calls, from `gen/decomp/calls.jsonl` + the 63 overload bodies) | 1,049 winners: none 621, int 223, float 129, string 16, colour 1, game-or-copy 59. Of the 650 called by `Base.osi`: none 408, int 127, float 60, string 12, game-or-copy 43 | scratch over `calls.jsonl`, `funcs/*.c` |
| M18 | per-frame natives in the front-end `process` methods | `lego_level_frontend.process`, `gs_lego_screenfrontend.process`, `base_screenlist.process`, `global_class.process`: **0** `0x82` each (they dispatch to sub-objects) | same |
| M19 | natives whose result feeds `0x53` directly | `gcsaver.findmaska` 12, `gcsaver.isconditionset` 10, `scslosistringfuncs.find` 6, `gcsaver.findglypha` 6, `finditema` 6, `bettersaver.getbooleanvalue` 5, `cheatmenu` 1, `scprinter.isdone` 1, `findcinema` 1 | same |
| M20 | Python dispatch rate of a tagged-variant stack loop | 5.42 M insn/s untraced, **1.62 M insn/s with a formatted trace line per instruction** (load 13) | `pybench.py` |
| M21 | `%f` of f32 widened to double: Python vs C `printf` on 10 values incl. half-way cases | identical, 10/10 | `pf.c` + Python |
| M22 | double rounding, int32 ⊕ f32: exact→f32 vs exact→f64→f32 | **+ 185, − 193, × 0 mismatches in 100,000 random pairs**; ÷ 0 in 300,000 | `fractions` probe |
| M23 | concrete falsifier | `16777217 1.0e-10 +` → f32 `4b800001` (16777218.0) exact-once; `4b800000` (16777216.0) via double | same + C validation (`rto.cpp`): TwoSum + round-to-odd reproduces the exact-once bits on 6/6 cases |
| M24 | corpus halves for the E2 pair (families, see §1.6) | A: 48 handlers / 41 messages / 20,474 authored chars / 33 used; B: 48 / 41 / 19,911 / 34 | exhaustive family partition, score 0.029 |
| M25 | M2a-0 agent time (ledger lines that carry minutes) | ≈ 280 min ≈ 4.7 agent-hours | sum over `progress.md` |

Two facts from M1 I rely on (INFERRED, cited): R23 of the M1 spec (`CreateDirectXWindow` 0x52e570 passes `CreateDevice` BehaviorFlags 0x80/0x20, no `D3DCREATE_FPU_PRESERVE`) and the boot order (`SetUpRenderer` before `SetUpScripting`), so the x87 precision-control field is **24 bits when the script runs**. This changes one convention in `opcodes.md` (§1.1, D8).

## 1. Task breakdown

Six tasks. T0 first; T1, T2, T3a, T3b in parallel from T0's commit; T4 after T1+T2; T5 last. **Independence rule:** the Python oracle (T2) and the two corpus halves (T3a/T3b) are written in worktrees cut from T0's commit, where `engine/src/osi/` does not exist; their briefs forbid reading any `engine/` path and the review package for them is diff-only. The C++ author (T1) never sees `tools/research/osi_vm.py` or `tests/osi/conformance/` before T4.

| Task | Agent (E2) | Depends on | Size (ESTIMATE, agent-hours incl. one fix round) |
|---|---|---|---|
| T0 data + reader prep + spec amendments | Opus default | — | 0.5–0.8 |
| T1 C++ VM core | Opus default (agent **V**) | T0 | 2.5–4 |
| T2 Python oracle | Opus default (agent **O** ≠ V) | T0 | 1.5–2.5 |
| T3a conformance corpus, group A | **Sonnet** | T0 | 0.8–1.2 |
| T3b conformance corpus, group B | **Opus-medium** | T0 | 0.8–1.2 |
| T4 differential run (CLI, replay, digest) | agent V | T1, T2, T3 | 1–1.5 |
| T5 mutation bite + fuzz seed | an agent ≠ V | T1–T4 | 0.8–1.2 |

### 1.0 Decisions this design adds (for the controller to ratify; each with cost-if-wrong)

- **D7 Handles, never pointers.** Every heap value a variant can hold (script object, array, dynamic string, game object) is a `uint32_t` handle from a **per-kind monotonic counter starting at 1, never reused**. Identity (`0x60` on type 1 and on game types) is handle equality. Traces print handles. This makes native, wasm32 and Python allocate identical ids in identical order and removes the 32-bit-pointer risk (§6 R5). Cost if wrong: none observable; handles are an implementation choice the original cannot distinguish.
- **D8 Mixed int/float arithmetic rounds the exact result once to f32** (x87 at PC=24, per M1 R23). `opcodes.md` currently says "port it as `(float)((double)a OP (double)b)`", which is the PC=53 reading; the two differ on double-rounding ties (M22: ~0.2 % of random int ± float pairs, 0 of 400k for × and ÷). T0 amends the Conventions paragraph: *port as the correctly rounded f32 of the exact result; C++ via TwoSum/TwoProduct + round-to-odd (validated M23), Python via `fractions`*; the `(double)` formula stays in the text as the PC=53 alternative and its falsifier (M23) joins the corpus. All 11 existing falsifiers hold under both (the table says so and the second reader recomputed them). Cost if wrong: one helper function per implementation and 1–3 expected values; `Base.osi` values are far from 2^24, so A6 is unaffected either way (M12/M11; ESTIMATE).
- **D9 NaN canonicalisation.** Any f32 result that is NaN is stored as `0xffc00000` (the x87 "real indefinite" an `fstp dword` writes; stable knowledge, UNVERIFIED on this exe). wasm32 NaN payloads are not deterministic across engines, so without this the trace could differ. Compares with a NaN operand yield 0 (table). Cost if wrong: one constant.
- **D10 Errors.** Inside the VM an error is a C++ exception (`osi::VmError`) that unwinds through handlers and through natives (natives are RAII-safe, as the original's are not); the outermost `Vm::call` catches it, records it, writes the `E` trace line and returns `ok=false` **without touching VM state**, so `snapshot()` shows exactly the state the table documents ("After an error"). The host decides: `reset_after_error()` performs the four resets (refcount, context depth, pop to and including this call's `0xff` marker, PC = marker value). Conformance asserts message + snapshot, never behaviour after the reset. Two errors the original does not have, with our own texts: `"Stack overflow (1000 slots)"` on a push at depth 1,000 and `"Stack underflow"` on a pop at depth 0 (spec M2a-1: "never a write past slot 1,000"). Cost if wrong: text only.
- **D11 Typed stubs for A6.** With every stub returning nothing, the A6 run dies at the 5th native of `Setup` (M15: `sctilefont.setcolor` needs a `type:ScTileFont`; `ConvertVariant` to a game type is the "unimplimented" error → "Script is calling sctilefont.setcolor" → kOSIError). So the registry carries a generated **`ret` column** (M17 kinds) and the default stub returns: none → nothing, int → 0, float → 0.0, string → "" (type 0x16), colour → 0, `game:<Class>` → a **fresh game handle of that class**, `copy` → `Duplicate` of argument 0. A per-row `default` override (D2's "one line in the registry") beats the kind default. Cost if wrong: a column regenerated by `make osi-tables`.
- **D12 The 7 Beta 1.5.0 mod natives are registered as stubs in M2a-1** (`data/osi/natives_beta150.json`, 7 rows), because `gs_lego_screenfrontend.initfull` calls two of them (M16) and the original with the mod installed does not error there. Their min/max/types are read from `BetterSaver.dll` / `OSIProject.DebugServer.dll` **as data** by the T3 asm registration reader (`tools/osi/registry.py` already decodes `push`→`call RegisterFunction` shapes); if the DLLs' shim (`OSIRegisterFunction`, per the final re-review) does not decode, the fallback is the type of the literal the script passes at every call site (`osi.py --eval`), which must be consistent across sites or the row is marked AMBIGUOUS. Bodies stay M2a-2. Cost if wrong: 7 rows edited in M2a-2.

### 1.1 T0 — data, reader and spec prep (files, acceptance, verification)

Files: `tools/osi/registry.py` (+`ret`, `default`, `ret_class` fields), `data/osi/natives.json` (regenerated), `data/osi/natives_beta150.json` (new, 7 rows, each citing the DLL offset or the call-site literal), `data/osi/types.json` (new: the 45 registered game types in first-use order with `id = 0x20 + rank`, plus the builtins; generated by `registry.py` from the same replay that writes `types.md`), `tools/osi/osiasm.py` (new, the conformance assembler), `engine/include/lomn/sage/osi.h` + `engine/src/sage/osi.cpp` (expose what the VM needs: per-class property symbols, function argc, the global-name table, the string table bytes; no behaviour change for `eval_calls`), `docs/osi/opcodes.md` (D8 amendment, D9/D10 notes under Conventions, a new section "Trace format" that links `docs/osi/trace.md`), `docs/osi/trace.md` (new, §2 of this document verbatim), `docs/osi/conformance.md` (new, §3), `tools/tests/test_osi_registry.py`, `tools/tests/test_osiasm.py`, `engine/tests/test_sage_osi.cpp`.

Assembler `osiasm.py`: text → OSI 4.1 bytes via a builder like `osi_craft.build` generalised: directives `.strings`, `.globals`, `.class NAME props(a,b) ; .method NAME`, `.function NAME argc`, mnemonics = the `BCL*` names from the table minus the prefix with operands in the table's units (`CallGameFunction cls.fn argc`, `PushConstantString "text"`, `JumpRelative @label`, labels for `CompareAndBranchIfFalse`/`BranchAlways`, `GetMemberFunction .sym`), and a raw escape `.byte` for deliberately bad bytecode. It is a serialiser, not an interpreter; both corpus authors and T2 use it. Round-trip test: `osiasm` output parsed by `osi.py` gives the same instruction list; `osi_statt_program()` reproduced byte for byte from its asm text.

Acceptance (observable): `make osi-tables` regenerates `natives.json` with `ret` for 1,049 winners and the kind counts of M17 exactly (pinned: none 621, int 223, float 129, string 16, colour 1, game 58 + copy 1 — the T0 implementer re-measures and the split of the 59 is its number, not mine); `types.json` has 45 game types in the `types.md` order; `natives_beta150.json` has 7 rows; `OsiFile` exposes `props`, `argc`, `globals`, and `ctest osi_all_methods` still prints 0 mismatches; `opcodes.md` regenerates byte-identically over the committed file.

Verification the reviewer re-runs: `make osi-tables && git diff --exit-code docs/osi/opcodes.md data/osi/natives.json docs/osi/types.md`; `python3 -m pytest -q tools/tests/test_osi_registry.py tools/tests/test_osiasm.py`; `ctest --test-dir build/native -R 'osi'`.

### 1.2 T1 — C++ VM core: interfaces

Files: `engine/include/lomn/osi/{variant.h,heap.h,stack.h,registry.h,trace.h,vm.h}`, `engine/src/osi/{heap.cpp,convert.cpp,registry.cpp,trace.cpp,vm.cpp,vm_handlers.cpp}`, `engine/tests/test_osi_vm.cpp` (unit tests of the primitives and of every handler family with hand-assembled bytes via `osi_builder.h`; **not** the corpus), CMake target `lomn_osi` (STATIC, links `lomn_sage`), added to `lomn_tests` and to `lomn_tests_asan`.

```cpp
namespace lomn::osi {

// 8-byte tagged variant, the ScOSIVariant {u16 type, u32 value} of the original. `value` holds int bits,
// f32 bits, a string-table index, a colour, a return/marker PC (file offset), or a handle (D7) — never a host pointer.
struct Variant { uint16_t type = kNothing; uint32_t value = 0; };
enum : uint16_t { kNone = 0x00, kObject = 0x01, kRetMarker = 0x02, kArray = 0x05, kNothing = 0x0f, kInt = 0x10,
                  kStringId = 0x11, kFloat = 0x12, kStringTable = 0x13, kColour16 = 0x14, kColour = 0x15,
                  kString = 0x16, kFrameMarker = 0xff, kByRef = 0x200 };            // game types: 0x20..0xfe
inline bool is_game_type(uint16_t t) { return t >= 0x20 && t <= 0xfe; }
Variant make_int(int32_t); Variant make_float(float); Variant make_colour(uint32_t); Variant nothing();
float as_float(Variant); int32_t as_int(Variant);                                   // bit casts, no conversion

// Exact-then-round-once mixed arithmetic (D8) and the x87 compare semantics (table 0x60–0x64).
float mixed_add(int32_t, float); float mixed_sub_if(int32_t, float); float mixed_sub_fi(float, int32_t);
float mixed_mul(int32_t, float); float mixed_div_if(int32_t, float); float mixed_div_fi(float, int32_t);
float canon_nan(float);                                                             // D9
int   cmp_exact(double a, double b, Op op);                                         // unordered -> 0

struct VmError { std::string message; uint32_t pc; uint8_t op; uint32_t depth; };  // thrown inside, reported outside

class Heap {  // owns objects, arrays, strings and game handles; refcounts explicit; emits A/F trace events
 public:
  explicit Heap(TraceSink* t);
  uint32_t new_object(uint16_t class_id, uint32_t nprops);                 // refcount 1, props nothing
  uint32_t new_array(); uint32_t new_array(uint32_t n_nothings);
  uint32_t new_string(std::string_view text);                             // throws kOutOfRange-equivalent at >= 255
  uint32_t new_game(uint16_t type);                                       // a game handle (natives + stubs)
  void duplicate(Variant& v);     // refcount +1 for 1, 5, 0x13, 0x20..0xfe; deep copy for 0x16 (table "Duplicate")
  void dispose(Variant& v);       // table "Dispose": refcount -1, free at 0; 0x16 deletes; props/elements back to front
  Variant* property(uint32_t obj, uint16_t symbol, const sage::OsiFile&);   // GetProperty: nullptr if absent
  std::vector<Variant>& elements(uint32_t arr); std::string& text(uint32_t str);
  uint16_t class_id(uint32_t obj) const; uint16_t game_type(uint32_t h) const;
  Stats stats() const;            // live counts per kind, peak, total allocs (for A15 and leak checks)
};

class Stack {  // 1,000 slots; our bounds checks (D10)
 public:
  void push(Variant v);             // throws VmError "Stack overflow (1000 slots)" at depth 1000
  Variant pop();                    // no dispose; throws "Stack underflow" at depth 0
  void pop_dispose(Heap&);          // Pop(true)
  void insert_below(uint32_t n, Variant marker);   // 0x50/0x51: memmove the top n up by one
  uint32_t depth() const; Variant& at(uint32_t i); Variant& top();
  uint32_t find_frame() const;      // first slot scanning down whose type is 2 or 0xff, plus one
 private: std::array<Variant, 1000> s_; uint32_t top_ = 0;
};

struct NativeCall {   // what a native sees
  Vm& vm; const NativeEntry& entry; uint32_t site_pc;
  std::vector<Variant*> args;        // exactly argc slot pointers, deepest first; a 0x200 parameter gets the SAME pointer
                                     // (the original passes the slot address, never the box), already converted in place
};
using NativeFn = Variant (*)(NativeCall&, void* user);
enum class Tier : uint8_t { Stub, Implemented, Mod, Unknown };
enum class RetKind : uint8_t { None, Int, Float, String, Colour, Game, Copy };
struct NativeEntry { std::string cls, name, key; uint8_t min, max; std::array<uint16_t, 10> types; RetKind ret;
                     uint16_t ret_type; std::optional<Variant> default_ret; NativeFn fn; void* user; Tier tier; };

class TypeTable {  // data/osi/types.json: name <-> id (0x20 + rank), builtins
 public: static TypeTable load(std::string_view json); uint16_t id(std::string_view name) const; const std::string& name(uint16_t) const; };

class NativeRegistry {
 public:
  static NativeRegistry load(std::string_view natives_json, std::string_view mods_json, const TypeTable&);  // winners only
  const NativeEntry* find(std::string_view cls, std::string_view fn) const;   // case-folded token pair (A–Z only)
  void bind(std::string_view key, NativeFn, void* user, Tier);                // unknown key -> std::logic_error
  void add(NativeEntry);                                                      // tests and the conformance runner
  Variant stub_return(const NativeEntry&, Heap&, NativeCall&);                // D11 policy, applied by the default fn
  void set_return_log(ReturnLog*);                                            // writes natives.log (§2.4)
};

struct TraceSink { virtual void line(std::string_view) = 0; virtual ~TraceSink() = default; };  // §2
class FileTrace : public TraceSink { … };   class DigestTrace : public TraceSink { … };   // per-frame FNV-1a 64

struct VmState { uint32_t pc, depth, frame, context_depth; int refcount; Variant top; bool halted; };
struct CallResult { bool found; bool ok; Variant result; };   // found=false: "Unable to locate function %s in scripts" (Message, not Error)

class Vm {
 public:
  Vm(const sage::OsiFile& script, NativeRegistry& natives, Heap& heap, TraceSink* trace);
  // Engine -> script, CallWithReturn protocol (table "Engine calls"): prefix-tolerant case-insensitive FindFunction,
  // exactly the script's argc pushed (ownership moves), the rest disposed, 0xff marker, Run, pop result, pop to marker.
  // Re-entrant: a native may call vm.call(); context depth is a u16 as in the original.
  CallResult call(std::string_view name, std::span<const Variant> args /* <= 10 */);
  // 0x82 memoisation (D4): per call site (file offset) -> NativeEntry*; the image is never written.
  const NativeEntry* memo(uint32_t site_pc) const;
  Variant convert(Variant v, uint16_t target);           // ConvertVariant, throws on an unsupported target
  const VmError* last_error() const; VmState snapshot() const; void reset_after_error();
  Heap& heap(); NativeRegistry& natives(); const sage::OsiFile& script() const;
  Variant& global(std::string_view name);                 // system-wide by name; creates nothing on first access
  uint64_t instructions() const;                          // executed count (A15)
 private:
  bool step();                                            // one instruction; false = Run stops (0x52 on 0xff, 0xff)
  … 96 handlers as member functions `bcl_xx()`, dispatched by a 256-entry table; unused entries -> bcl_unused()
};
}  // namespace lomn::osi
```

Notes binding the implementer to the table: `0x12/0x17/0x19` push `{0x10, file offset}` (a tagged offset, not a host address; `0x51` consumes it as a file offset); `0x50`'s target is file-relative (offset 0 = `'OSI\0'`); the u8 operands listed in "Operand-format notes" are read unsigned; `0x14` pads to n slots above the frame base and counts `this`; `0x21/0x22` bit 15 = global, index = `idx & 0xff`; `0x82` self-patch = memoise per site and trace `0x82`; a **literal `0x84`** in the file is our error `"Direct call without a memoised site"` (the original would call an arbitrary address). The byref box is modelled by passing the slot pointer (the native sees the slot; the pop-with-dispose afterwards disposes what it wrote).

Acceptance (observable): `lomn_tests` has ≥ 1 unit case per handler family naming the row; every one of the 81 messages (M5) appears verbatim in `vm_handlers.cpp` exactly once per row×message pair (114; a script greps them — a presence tripwire only, the semantic gate is T3); `lomn_tests_asan` runs the same cases clean; `Vm` compiles for wasm32 (`make wasm`) with no new link flags.

Verification: `ctest --test-dir build/native -R 'unit|osi'` and `ctest --test-dir build/wasm -R unit`; `python3 tools/osi/check_messages.py engine/src/osi/vm_handlers.cpp docs/osi/opcodes.md` (T1 writes it: prints `114/114 row-message pairs present`).

### 1.3 T2 — Python oracle: module API

Files: `tools/research/osi_vm.py` (single module, no third-party imports except `struct`, `fractions`, `json`; numpy optional and off by default so the result does not depend on it), `tools/tests/test_osi_vm.py` (its own unit tests, written from the table, using `osiasm.py`).

```python
# tools/research/osi_vm.py — written from docs/osi/opcodes.md, types.md, and the cited decomp/asm; NEVER from engine/.
class Variant(NamedTuple): type: int; value: int          # value is a u32 (ints masked two's complement; floats as f32 bits; handles)
class VmError(Exception): message: str; pc: int; op: int; depth: int
class Heap:         # objects/arrays/strings/game handles with explicit refcounts; per-kind monotonic ids from 1 (D7)
    def new_object(self, class_id: int, nprops: int) -> int; def new_array(self, n: int = 0) -> int
    def new_string(self, text: bytes) -> int; def new_game(self, type_id: int) -> int
    def duplicate(self, v: Variant) -> Variant; def dispose(self, v: Variant) -> None
class TypeTable: load(path) -> TypeTable; id(name) -> int; name(id) -> str
class NativeEntry(NamedTuple): cls, name, key, min, max, types: tuple[int, ...], ret: str, ret_type: int, default: Variant | None, fn, tier
class NativeRegistry:
    @classmethod load(cls, natives_json, mods_json, types: TypeTable) -> NativeRegistry
    def find(self, cls_name: str, fn_name: str) -> NativeEntry | None       # case-folded token pair
    def bind(self, key: str, fn) -> None;  def add(self, entry: NativeEntry) -> None
    def stub_return(self, entry, heap, call) -> Variant                       # the D11 policy
    def replay_from(self, log_path) -> None                                    # §2.4: returns come from the C++ log
class TraceSink: def line(self, s: str) -> None            # FileTrace(path), DigestTrace(path)
class CallResult(NamedTuple): found: bool; ok: bool; result: Variant
class VmState(NamedTuple): pc: int; depth: int; frame: int; context_depth: int; refcount: int; top: Variant | None; halted: bool
class Vm:
    def __init__(self, script: bytes, natives: NativeRegistry, heap: Heap, trace: TraceSink | None = None)
    def call(self, name: str, args: list[Variant]) -> CallResult            # CallWithReturn protocol; re-entrant
    def convert(self, v: Variant, target: int) -> Variant
    def snapshot(self) -> VmState; def reset_after_error(self) -> None
    @property last_error -> VmError | None; instructions: int
# helpers both the oracle and the corpus-expectation calculator use (pure arithmetic, no VM):
def f32(x: float) -> float; def f32_bits(x: float) -> int; def exact_round_f32(fr: Fraction) -> float   # D8
def convert_to_string(v, strings) -> bytes      # "%d" / "%f" (Python '%f' == libc on f32-widened doubles, M21)
# CLI
#   osi_vm.py --conform DIR [--trace-dir D]            runs every program, prints "conformance: N/N pass", exit 1 on any failure
#   osi_vm.py --run BASE.osi --natives data/osi/natives.json --mods data/osi/natives_beta150.json --types data/osi/types.json
#             --frames 600 --keys KEYSPEC --replay natives.log --trace out.trace [--digest out.digest]
```

Acceptance: `pytest tools/tests/test_osi_vm.py` passes; the oracle rejects a literal `0x84` with the same text as D10; `--conform` on T3's corpus is all green (that is the A5 gate for the oracle); the brief's hand-back line names the rows the author found **ambiguous or contradictory** in the table (the second-reader channel the M2a-0 lesson asks for).

Verification: `python3 -m pytest -q tools/tests/test_osi_vm.py`; `python3 tools/research/osi_vm.py --conform tests/osi/conformance` → `conformance: N/N pass`.

### 1.4 T3a / T3b — the conformance corpus (the E2 matched pair)

Files: `tests/osi/conformance/<group>/<id>.asm` + `<id>.expect.json` (format §3), `tests/osi/conformance/README.md` (the authoring rules of §3.2, written by T0 as `docs/osi/conformance.md` and copied), `tools/osi/expect_calc.py` (T0 provides it: exact rational → f32 rounding, `"%d"/"%f"`, 32-bit wrap, `atoi/atof`; it is **not** an interpreter — it evaluates one arithmetic row at a time from literal operands).

Group A (T3a, **Sonnet**): families `nop/line 00–06`, `members 10–17`, `members-by-string 18–1a`, `variables 21–25`, `calls/branch 50–54`, `arith 70–75`, `colour a0–a7`, `type tests b8–bd,bf` — 48 handlers, 41 messages, 20,474 authored chars, 33 used in `Base.osi` (M24).
Group B (T3b, **Opus-medium**): `stack ops 30–35`, `push 40–49`, `compare 60–64`, `logic/bitwise 6a–6f,76–78`, `inc/dec 7a–7b`, `game-var 80–81`, `native calls 82–84`, `arrays 90–96`, `convert b0–b2`, `halt ff`, plus `BCLUnused` (2 programs) — 48 handlers, 41 messages, 19,911 chars, 34 used (M24).
Both groups get the same brief text apart from the family list; both include the falsifier rows that fall in their families (A: 0x70–0x75 and the D8 case; B: 0x60–0x64, 0xb1, 0xb2, the 20 argc violations). Counterbalance vs round 2 (where Opus-medium had the 6× smaller task): sizes are now matched within 3 % on every measure.

Acceptance (per group, measured by the runner, §3.5): every handler in the group has ≥ 2 programs; every row×message pair in the group has ≥ 1 error program; every named falsifier in the group's rows is a program; every `.expect.json` cites its row(s); no expectation was produced by running an interpreter (the brief forbids it; the reviewer spot-checks 5 random expectations by recomputing them by hand from the row).

Verification: `python3 tools/osi/conformance_report.py tests/osi/conformance/<group>` → `handlers 48/48 ≥2, messages 41/41, falsifiers k/k, programs N`; then after T1/T2 land: `build/native/lomn_osi_conform tests/osi/conformance/<group>` and `osi_vm.py --conform …` both `N/N pass`.

### 1.5 T4 — differential run (A6)

Files: `engine/tools/lomn_osi_run.cpp` (native + wasm32 under node, as `lomn_osi_dump`), `engine/tests/osi_a6.cmake` (ctest `osi_a6_digest`: native vs wasm32 vs Python digests `cmp`; `osi_a6_window`: full trace for frames 0–2 `cmp`), Makefile target `osi-diff`, `tools/osi/keys.py` (KEYSPEC → per-frame `Input` calls, §4.1), `perf/` entry `osi-a6` (instructions/frame p50/p95 with the frames-vs-window reconciliation, natives/frame, stack high-water — A15, no thresholds).

Acceptance: the three digest files are byte-identical; the three full traces of the window are byte-identical; the run prints `frames 600, instructions N, natives M, distinct_pcs_by_frame …, stop: <none|error "…" at frame f>`; the native-return log replays into Python with 0 desyncs.

Verification: `make osi-diff` (runs native, node and Python, then `cmp` ×4 and prints the summary); the reviewer also runs `build/native/lomn_osi_run --trace-frames 0:600 …` once to confirm the full-trace `cmp` for a non-window frame range of their choice.

### 1.6 T5 — mutation bite (A7) + VM fuzz seed

Files: `tools/research/oracle/mutate_vm.py` (§5), `tools/tests/test_mutate_vm_anchors.py` (every `old` string occurs exactly once — the mutate4 anchor test pattern), `engine/tests/fuzz_osi_vm.cpp` (seeded from the corpus `.osi` files and `Base.osi`; runs `call("main")`/`call("setup")` with a step cap; ASan/UBSan; on demand — the full fuzz campaign is M2a-4, this is the seed so M2a-4 starts from a working harness).

Acceptance: every targeted mutant CAUGHT by the check it names; the no-op control MISSED; survivors listed with a ruling line each. Verification: `python3 tools/research/oracle/mutate_vm.py <scratch_tree> --only 1-33` output; the reviewer re-runs 3 random mutants by hand.

## 2. Trace format (A6) — `docs/osi/trace.md`

**Text**, LF-terminated, ASCII only, one record per line, fixed field order and widths, no JSON (JSON key order and float printing are the two ways `compare.py trace` could hide a difference; `cmp` on bytes cannot). Every implementation writes the same bytes or the run fails; nothing is "normalised" afterwards.

Grammar (`<v>` = a variant, printed as `TTTT:VVVVVVVV` with `TTTT` = the u16 type in 4 lowercase hex digits (`000f` nothing, `0010` int, `0200` by-ref) and `VVVVVVVV` = the u32 value in 8 lowercase hex digits: int bits, f32 bits, string-table index, colour, handle, PC; `----:--------` = empty):

```
# lomn-osi-trace 1 script=<sha256 of the .osi> natives=<sha256 of natives.json> mods=<sha256> types=<sha256> fpu=pc24
C <name> <argc_declared> <a0> … <a9>                 engine->script call entry (after FindFunction; name as passed)
M <name>                                             Message "Unable to locate function" (no C line follows)
I <pc:08x> <op:02x> <depth:4d> <frame:4d> <top:v>    one line BEFORE each instruction executes; op is the pristine byte (0x82 never 0x84)
N <pc:08x> <cls>.<fn> <argc> <a0:v> … <aN-1:v> -> <r:v>   emitted after the native returns; cls/fn as in the string table, case preserved
S <h:08x> <len:3d> <hex bytes>                       the text of string handle h when it is passed to or returned by a native (one S per 0x16 argument/return, before its N line)
A <k> <id:08x> [<class:04x>|<n:08x>|<len:3d>|<type:02x>]   allocation: k in {o,a,s,g}; the bracket shows class id / element count / length / game type
F <k> <id:08x>                                       free (refcount reached zero or string deleted)
R <v>                                                 the engine call's result, after the marker pop
E <pc:08x> <op:02x> <depth:4d> "<message bytes, \" and \\ escaped>"   an error; the line after it is the next C/M or EOF
K <frame:6d> <t:08x> <dt:08x> <n:08x>                frame header for A6: fake time and dt as f32 bits, Process counter
```

- **Floats bit-exact**: always the 8 hex digits of the IEEE-754 binary32 pattern (`std::memcpy` into `uint32_t`; Python `struct.unpack('<I', struct.pack('<f', x))[0]` on a value already rounded to f32). NaN is one pattern by D9. `-0.0` prints `80000000`. `K` prints time/dt as the f32 the script receives.
- **Ints**: two's complement u32 (`-1` = `ffffffff`); Python masks with `& 0xffffffff` everywhere a value is stored.
- **Strings**: the `S` line carries bytes as hex (latin-1, never decoded), so 0x80+ bytes and NULs are exact.
- **Top of stack** on `I` lines: the slot below `top_` or `--:--------` at depth 0. `frame` is the frame-base index (0 when none).
- **Alloc/free order** is observable and must match: object and array destructors dispose back to front (table), `0x52` pops with dispose from the top down, `CallWithReturn` disposes surplus arguments in index order 0..9. T2 models exactly this; a disagreement here is the ownership blind spot the spec names and is the point of the `A/F` lines.
- **Why byte-identical across three implementations is achievable**: no host pointers (D7), no printf of floats (`%08x` of bits only; `%f` is used by the VM semantics of `0xb0`, where the text goes through an `S` line as bytes — and M21 shows Python's `%f` equals libc's on f32-widened doubles for the ties tried; still UNVERIFIED vs MSVC/CodeWarrior CRT, so it stays a listed blind spot), canonical NaN (D9), `-ffp-contract=off` already set for native == wasm32 (CMake line 20), and `std::fma` only inside `mixed_div_*` (wasm32 emulates it in software, correctly rounded).
- **Digest mode** (`--digest`): one line per `K` block: `D <frame:6d> <lines:8d> <fnv1a64 of all bytes of the block:016x>`; the header line and the final `T <total lines> <fnv1a64 of the whole trace>` are included. The digest is computed over the same bytes the full trace would contain, so "digests equal" ⟺ "full traces equal" up to a 64-bit hash collision; a mismatch names the frame to re-run with `--trace-frames a:b`. Size bound: at the static upper bound (M13, 122k insns/frame) 600 frames would be 73 M `I` lines ≈ 3 GB per implementation; the digest is 600 lines. The likely size (ESTIMATE from M12/M18: a frame walks screen-list dispatch, not the whole program) is 2k–20k lines/frame → 50–500 MB; the full-trace window default is frames 0–2 plus any frame the digest flags.

### 2.4 The native-return replay log (A6/A9)

The C++ run writes `natives.log` next to the trace: `<seq:08d> <cls>.<fn> <argc> -> <r:v> [S <len> <hex>] [W <i> <v>]…` where `W` lines record what the native wrote into a by-ref slot (0x200) before the pop. Python's `NativeRegistry.replay_from(log)` replaces every native body: on call `seq` it asserts `cls.fn/argc` match the log (else `VmError("replay desync at seq N: expected a.b/2, got c.d/3")` — a VM-semantics finding, since the call order is VM behaviour), returns the logged variant, re-creating a game handle `g:<id>` by asking the heap for the **next** id and asserting it equals the logged one (handles are allocated in the same order on both sides by D7), applies the `W` writes. Thus any A6/A9 disagreement is VM semantics, never native semantics. For A6 the log is produced by the typed stubs (D11), so Python could also just run the same stub policy; the replay path is used anyway so that A9 (M2a-2, real natives) needs no new mechanism.

## 3. The conformance corpus (A5) — `docs/osi/conformance.md`

### 3.1 Format

One program = `<id>.asm` (assembled by `osiasm.py` into `<id>.osi` at test time, never committed as binary) + `<id>.expect.json`:

```json
{ "id": "0x53-float-cond", "rows": ["0x53"], "falsifier": "a float 0.0 condition is an ERROR, not a branch",
  "entry": "main", "args": [],                       // the engine call: name and ≤10 variants (the test may pass more than argc to test disposal)
  "natives": [ {"cls": "t", "name": "echo", "min": 1, "max": 1, "types": ["int"], "returns": {"type": "0x10", "value": 7}},
               {"cls": "t", "name": "obj", "min": 0, "max": 0, "types": [], "returns": {"game": "GcTimer"}},
               {"cls": "t", "name": "byref", "min": 1, "max": 1, "types": ["byref"], "writes": [{"arg": 0, "type": "0x10", "value": 5}]} ],
  "registry": "none|natives.json",                   // natives.json enables the 20 argc-violation keys and the unregistered path
  "expect": { "ok": true, "result": {"type": "0x10", "value": 1}, "depth": 0, "context_depth": 0,
              "globals": {"g": {"type": "0x12", "bits": "3f800000"}},
              "heap": {"live": {"o": 0, "a": 0, "s": 0, "g": 0}, "allocs": {"o": 1, "a": 0, "s": 2, "g": 0}} }
}
```
An error program replaces `expect` with
```json
"expect_error": { "message": "Results of comparison expression is not integral", "pc": "@L1", "op": "0x53", "depth": 1,
                  "top": {"type": "0x12", "bits": "00000000"} }
```
`pc` may be a label from the `.asm` (the runner resolves it). Values are given as `value` (decimal int, u32) or `bits` (8 hex) — floats only as bits, so an author must compute the f32 pattern with `expect_calc.py` and the row's formula. Test natives are declarative so both runners implement them from the same JSON and no corpus program depends on an engine native.

### 3.2 Authoring rules (independence of both implementations)

1. The author reads **only** `opcodes.md`, `types.md`, `conformance.md`, `osiasm.py` and `expect_calc.py`. The brief states the forbidden paths (`engine/`, `tools/research/osi_vm.py`) and the reviewer checks the agent's tool log for them.
2. Every expectation cites the row cell it comes from (`"rows"`), and a falsifier program states the alternative reading it kills (`"falsifier"`) — the reviewer's 5-sample check asks "does the alternative reading give a different `expect`?"; if not, it is not a falsifier.
3. Numbers come from `expect_calc.py` (exact rational → one f32 rounding; `"%d"`/`"%f"`; `atoi`/`atof`; 32-bit wrap; unsigned `div`; shifts) or from integer arithmetic the author can show in the file's `"note"`. Running any interpreter to produce an expectation is forbidden (that would make the corpus a copy of one implementation).
4. Programs never depend on undefined behaviour the table marks (`0x11` on a non-object, `0x33 0`, `0x84` literal except the D10 error program, INT_MIN/−1, stack over/underflow except our two error programs, behaviour after an error).
5. Programs are minimal: one row under test, the fewest other instructions, and those from the "trusted core" (`0x40–0x47`, `0x30`, `0x52`) which get their own programs first.

### 3.3 From rows and falsifiers to tests

- **Each of the 96 handlers: ≥ 2 programs** — one normal path per distinct stack-effect case listed in the row (e.g. `0x60` has 7 cases: nothing/nothing, nothing/other, object identity, game identity, string fold, float cases, int/int → 7 programs), one per **row×message pair** (114, M5; `0x82` alone has 9 inner paths that all surface as "Script is calling %s.%s", so the program asserts that text and the inner cause only through the post-error snapshot, e.g. argc kept, nothing pushed).
- **The M2a-0 falsifier table (11 rows + 2 guards)**: each row is a program with the "original" column as `expect` and the "old reading" column recorded as `"alternative"` in the JSON so the report can show it differs. Plus `0x74` `2.0 1 ^` → `4f000000`, `3 2 ^` → 81; `0x75` `-1 3 %` → 0; `0x78` `-8 1 >>` → `7ffffffc`; `0xb2` 2.7 → 2, −2.7 → −2, 3.0e9 → `b2d05e00`; `0x70` 200+60 chars → kLenTooLong (asserted as our error text for the non-OSI throw, `"kLenTooLong"`, and the post-error depth = before − 2, nothing pushed), 200+54 ok; `0x60` "ABC" = "abc" → 1, `1 = "1"` → error, nothing = 0 → 0; `0x53` float → error; `0x50` file-relative target (a program whose target would be wrong under PC-relative or script-base+4 readings); the D8 case `16777217 1.0e-10 +` → `4b800001` (M23) and its sibling for `0x71`.
- **The 20 argc-violation keys** (types.md): one program each with `"registry": "natives.json"`, expecting "Script is calling <cls>.<fn>" (the inner "Too many/few" is overwritten) — free falsifiers; a VM that runs them is wrong.
- **Engine-call protocol** programs: `call("Setup…")` resolves to `setup` by prefix; a query shorter than the entry fails (`M` line); 10 arguments supplied to a 0-argc function are all disposed (heap `allocs/live` show the strings freed); `argc > 10` error text; a `0x52` on the `0xff` marker leaves `{0xff, r}` for the caller (asserted via `depth` 0 after the call and `result`); re-entrancy: a test native `t.reenter` calls `vm.call("inner")` and the outer frame's locals are intact afterwards (context depth 2 → 0; trace `C … R` nesting).
- **Ownership** programs: array aliasing through `0x35` (shared, not copied); object owns props (free order o→props back to front in the `F` lines — asserted via `heap.allocs/live` and, between implementations, via the trace `cmp`); string deep copy; `0x82` pops dispose the converted argument (string alloc 1, free 1).
- **Our caps**: recursion through `0x51` until depth 1,000 → `"Stack overflow (1000 slots)"` at depth exactly 1000 and no write beyond; a `0x30` on an empty stack → `"Stack underflow"`.

### 3.4 Error-path assertions

`expect_error.message` is compared byte-for-byte (typos included; the `%02.2X` type formats are spelled out by the author, e.g. `(type:12)`), `pc` is the failing instruction's own address (the table: "PC not advanced" for pre-check errors; for `0x70`'s kLenTooLong it is the `0x70` itself), `depth` and `top` are the stack **at the throw**, as "After an error" documents (operands already popped stay popped; nothing pushed; `0x82`: converted arguments stay converted). The runner takes `snapshot()` before any reset, then `reset_after_error()` and asserts the VM is usable again (`call("main2")` on a second entry point succeeds) — the latter is our behaviour, documented as such, not conformance with the original.

### 3.5 Minimum counts, measured not invented

The acceptance is the report of `tools/osi/conformance_report.py` (T0 writes it; it reads only the `.expect.json` metadata): `handlers ≥2: 96/96`, `row×message pairs covered: 114/114`, `named falsifiers: k/k` (k = the count of rows whose `notes` cell contains "Falsifier" = 15 by my regex, M24 sums; the script measures it), `argc violations: 20/20`, `programs: N`. The lower bound implied by the rules is 96×2 + (114 − pairs that coincide with a handler's 2) + 11 + 2 + 20; the script prints the exact N and that is the number the ledger records. ESTIMATE: 280–340 programs.

**The 29 unused opcodes (M3) — achievable, how:** all have authored rows. `0x00/0x04–0x06` (no-ops: depth unchanged, PC +1/+5); `0x02/0x03/0x41` (always-error: text + PC not advanced + depth unchanged, 2 programs each by varying the stack content); `0x18/0x1a` (object member by string, with `0x11` and `0x16` names, 4 error texts; test natives not needed); `0x24/0x25` (frame and global variables; the copy-paste "increment" message on a global decrement); `0x31/0x32/0x34`; `0x49` + `0xa4` error; `0x6f`, `0x78`, `0x7a/0x7b` (raw bits on a float: `3f800000 → 3f800001`); `0x74` (4 case programs); `0x80/0x81` (no state change, PC +5); `0x83` (name on the stack: `0x11` and `0x16`, "Invalid member type" error, argc errors); `0x84` (the D10 literal error + the memoised-second-execution program, where the trace must print `82` both times and the result is identical); `0xb8/0xb9/0xbc/0xbf` (type tests incl. `0xbf` on a game handle → type + 0x10000); `0xff` (pushes nothing and stops Run: a `main` that halts leaves `{0xff marker, nothing}` for `CallWithReturn`, result nothing, depth 0). `BCLUnused`: 2 programs on two different unused bytes.

## 4. The A6 differential run on the real `Base.osi`, all natives stubbed

### 4.1 What "Setup + 600 Process frames + the key sequence" means concretely

Per the engine-call protocol (table "Engine calls") and probe 3:
1. `call("Setup", [])` — 0 declared args (M7). With typed stubs (D11) the static path runs `global_class.initfull` (4 `sctilefont.create` → game handles g1..g4, `setcolor` ok, `userealdeltatime(1)` stub), `base_screenlist`, `setupfrontendscreen`, `lego_level_frontend.initfull` (`gcsaver.get*` → 0, `playcinematic` ×3 → nothing), `lego_area_front.initfull` (literals), `gs_lego_screenfrontend.initfull` (`bettersaver.getbooleanvalue(100)` → 0 via the D12 stub row, so no extra movie).
2. For frame k = 0..599, in the original's priority order (controller triggers 0, mouse/key mapper 1, script Process 2; lower-first is UNVERIFIED per probe 3 and is the order we take): the key calls of this frame (below), then `call("Process", [float time, float dt, int k])` with the **fake-time** path of D3: `time = (k+1)·0.0066667`, `dt = 0.0066667` as f32 bits from the constants at `0x712d68`, so `K` lines carry exactly those bits. Process returns nothing (probe 3: the shipped script always does), so there is no re-run; the harness still implements the "false → up to 10 extra passes" rule and counts passes, so A9 cannot silently differ.
3. **Keys**: `KEYSPEC` = `down@100 down@160 enter@220 esc@280` (frames; ESTIMATE, chosen after the intro would have finished under D1 — but see 4.3). A tap is two frames (probe 3's action state machine): frame f: `Input(C1, VK, 2, 0.0, 0.0)`; frame f+1: `Input(C1, VK, 1, 0.0, 0.0)`; held frames would send 4. Down = VK `0x28`, Enter = `0x0d` through the analog-trigger site S3 (5 args, declared argc 5 — M7); Esc = mapper id `251` through S4: `Input(C1, 251, state)` with 3 args, the remaining 2 of `input`'s 5 arrive as **nothing** (the engine passes 10 variants, `SetPCAndArgs` pushes exactly the script's argc = 5; S4 supplies 3 values and 7 nothings). `C1 = 0x636e7431`. No `OnEvent(26,…)` mouse lines in A6 by default (`--mouse` adds `OnEvent(26, x, y)` per frame for M2a-3); no timers (`OnEvent(8,…)`) because `gctimer.*` is stubbed and never fires.
4. Trace header records `fpu=pc24`, the four sha256s and the KEYSPEC; `K` per frame; `C/R` per call.

### 4.2 What the stubs return (D2 + D11)

The registry = `natives.json` winners (1,049) + `natives_beta150.json` (7). Default per `ret` kind (M17: of the 650 called keys, 408 return nothing, 127 int 0, 60 float 0.0, 12 string "", 43 a fresh game handle of the overload's class). Per-row `default` overrides that A6 needs (each one line, cited): `bettersaver.getbooleanvalue → 0` (the consult's own ruling), `scslosistringfuncs.find → -1`? — **no**: A6 does not decide game behaviour; the typed defaults stand and the run goes wherever they lead. A6 asserts identity of three traces, not a menu. The 71 keys the exe never registers (D2) stay unregistered: reaching one is "Call to unregistered Game Function" → kOSIError → the run stops, which is correct and is the finding.

### 4.3 Where the run is expected to stop or error first (reasoning, INFERRED)

- With nothing-only stubs: `Setup`, 5th native (M15) — certain, by the conversion rule. That is why D11 exists.
- With typed stubs: the static `initfull` chain (M15/M16) has no further game-type argument before the screen objects, and the two mod natives are registered (D12). `Setup` should complete (ESTIMATE, 70 %: the remaining risk is a native on a dynamic path I did not walk — `0x19` method-by-string, 12 uses — or a game-handle flowing into `0x11/0x12` member access, which is always-error on game types in the original too and would be a *correct* stop).
- Per frame: the four front-end `process` methods call no native directly (M18); the natives feeding branches (M19) are `gcsaver.find*`/`isconditionset` → int 0, so every such `if` takes the false branch deterministically. The intro: `playcinematic` is a stub; whatever the script polls to detect the end of a movie (D1 in M2a-3 makes it instant) returns 0/nothing here, so the screen state machine most likely **stalls in the intro/front-end state** and repeats the same per-frame path for all 600 frames. The key calls then reach `gs_lego_screenfrontend.input` only if `menuactive == 1` (probe 3), which the stall may never set; expected outcome: the key frames produce `C Input … R` lines whose body takes the "inactive" branch.
- Errors that would end it early, in order of likelihood (ESTIMATE): (i) a tier-3 native that the script expects to return an object used later as an argument of a *different* class than the overload's (a conversion error — correct behaviour, trace shows it); (ii) one of the 71 unregistered keys reached on the front-end path (the trace names it; M2a-0 ruled the argc violations unreachable, not these); (iii) arithmetic on a nothing from a none-kind stub (`0x70` "Invalid operand" — again correct given stubs).
- Whatever happens, A6's acceptance is: **three identical digests and an identical stop line**. The stop line and the coverage report (`distinct PCs executed, by frame`, `natives hit by tier`) are the inputs to the M2a-2 worklist (spec §6), exactly as the consult planned.

### 4.4 Is 600 frames meaningful?

For VM semantics: after the first few frames of a stalled state machine, new frames add no new PCs (the report will show `distinct_pcs_by_frame` flattening; that is the measurement, not an assumption). 600 is then only a determinism soak (identical repetition; cheap: at the ESTIMATE of 2k–20k insns/frame the C++ runs in well under a second, Python in 1–8 s at 1.62 M insn/s traced (M20); even the static upper bound (M13 × 600 = 73 M) is ≈ 45 s of Python and ≈ 3 GB of full trace per side, which the digest mode avoids). For game behaviour, 600 fake-time frames = 4.0 s of script time at dt 0.0066667 (60 Hz real-time would be 10 s); whether the menu timers (`lego_timer` ×2, fades) need more is unknown until M2a-2's A9 run with the real tier-1 natives, which is where the frame count should be re-chosen from the trace (the first frame on which the menu's `input` becomes active + the tap sequence + a margin). Keep 600 in M2a-1 as the spec says, report the flattening frame, and let A9 set the number.

## 5. Mutation bite plan (A7) — `tools/research/oracle/mutate_vm.py`

Style of `mutate4.py`: a list `M = [(id, file, old, new, checks)]`, each applied in a scratch tree, built, checked, restored; `EXPECT[id]` = a regex over the failing check's output naming the **targeted** program/line (an unrelated failure is not a bite — the mutate4 lesson); a no-op control that must be MISSED; per-(mutant, check) logs under `build/mutlogs`. Checks: `unit` (`lomn_tests -tc='osi*'`), `conform` (`lomn_osi_conform tests/osi/conformance`, 180 s), `diff` (`make osi-diff` digest `cmp` native vs Python; the wasm leg skipped under `--skip-wasm` to keep a mutant under ~3 min), `asan` (`lomn_tests_asan`). Expected kill rate: **the acceptance is every targeted mutant caught**; the number of survivors is what the run measures (precedent: the M2a-0 presence-only pins let 4 of 5 semantic mutants survive — the corpus is semantic by rule 3.2.2). Mutants (33 + control), each naming the check that must bite:

| # | Handler / unit | Mutation (one line) | Must be caught by |
|---|---|---|---|
| 1 | 0x53 | a float 0.0 condition branches instead of erroring | conform `0x53-float-cond` |
| 2 | 0x53 | nothing is truthy (falls through) | conform; diff (M19 paths) |
| 3 | 0x50 | target relative to PC instead of file base | conform `0x50-file-relative` |
| 4 | 0x51 | marker inserted above the args instead of below | conform `0x51-args-frame`; diff |
| 5 | 0x52 | result disposed before re-push (string result freed) | conform heap live/allocs; diff `F` lines |
| 6 | 0x14 | pad count excludes `this` (off by one) | conform `0x14-this-counts`; diff |
| 7 | 0x21 | global bit 0x8000 ignored | conform; diff (`screenlist`) |
| 8 | 0x22 | store into a global does not dispose the old value | conform heap counts; asan |
| 9 | 0x33 | `top−n` off by one | conform `0x33-pull`; diff |
| 10 | 0x35 | Dup deep-copies arrays | conform `0x35-array-alias` |
| 11 | 0x60 | string compare case-sensitive | conform `0x60-string-fold` |
| 12 | 0x60 | int,float path rounds the int to f32 first | conform table falsifier `0x60` |
| 13 | 0x70 | mixed add via `(float)((double)a+(double)b)` | conform D8 `0x70-pc24-tie` |
| 14 | 0x70 | string overflow truncates instead of kLenTooLong | conform `0x70-len-too-long` |
| 15 | 0x72 | int×int in 64 bits (no wrap) | conform `0x72-wrap` |
| 16 | 0x73 | int/int rounds to nearest | conform `0x73-trunc` |
| 17 | 0x73 | `-0.0` divisor passes the zero test | conform `0x73-neg-zero` |
| 18 | 0x75 | signed `%` | conform `0x75-unsigned` |
| 19 | 0x78 | arithmetic shift | conform `0x78-logical` |
| 20 | 0x82 | min/max check skipped | conform argc-violation programs |
| 21 | 0x82 | unsupported source conversion errors instead of `{0,0}` | conform `0x82-convert-type0` |
| 22 | 0x82 | arguments popped without dispose | conform heap live; asan (leak report) |
| 23 | 0x82 | by-ref native gets a box copy, writes lost | conform `0x82-byref-write` |
| 24 | 0x82 | trace prints `84` after memoisation | diff (Python prints `82`) |
| 25 | 0x91 | index compared signed | conform `0x91-negative-index` |
| 26 | 0x96 | insert at `count` allowed | conform `0x96-at-end-error` |
| 27 | 0xa0 | negative channel value clamps to 0 | conform `0xa0-clamp-255` |
| 28 | 0xb2 | float→int rounds to nearest | conform `0xb2-trunc` |
| 29 | 0xb0 | `%f` with 5 decimals | conform `0xb0-float-text` |
| 30 | Stack | overflow check at 1001 | conform `cap-stack-overflow` |
| 31 | CallWithReturn | pushes all 10 engine args instead of argc | conform `call-exact-argc`; diff |
| 32 | FindFunction | exact match instead of prefix | conform `call-prefix` |
| 33 | Heap | object dtor disposes props front to back | diff `F` order; conform allocs (same count → diff is the real check) |
| 34 | control | comment-only change in `vm.cpp` | must be MISSED |

Five symmetric Python mutants (optional, 10 min): 0x53 truthiness, D8 formula, dispose order, memo trace byte, prefix match — each must be caught by `conform` on the Python side or by `diff`; they prove the C++ side is also an oracle for the Python side.

Scripted exactly like `mutate4.py` (`run()` collects every FAIL line; `EXPECT` regex per mutant; restore + rebuild after each engine change; `--only`, `--skip-wasm`; anchor test `test_mutate_vm_anchors.py` so a refactor cannot silently retire a mutant). Budget: ESTIMATE 2–4 min per mutant (incremental native build + conform + diff) → ≈ 1.5–2 h wall in the background, logged per mutant.

## 6. Risks, ranked (each with a cheap probe)

1. **x87 precision control (PC=24 vs 53) — the D8 decision rests on an inference (M1 R23).** Impact: mixed int±float ties only (M22: 0.2 % of random pairs, 0 of the `Base.osi`-sized values I can imagine; D8's falsifier program would flip). Probe (30 min, data only, first thing in T0): (a) an `fldcw`/`fnstcw`/`_controlfp`/`_control87` scan of `.text` between `CreateDevice` (`CreateDirectXWindow` 0x52e570) and `SetUpScripting` 0x4390d0, and inside every VM handler; the table cites one sequence, `fnstcw; or 0x0c00; fldcw; fistp; and 0xf3ff; fldcw` in ConvertVariant (0x60bc4b–0x60bc6a), which **preserves the PC bits 8–9** if both masks are applied to the word `fnstcw` saved (INFERRED from the cited immediates; the asm read confirms whether the restore uses the saved word or a constant — a constant such as `0x027f` would reset PC to 53 on every `0xb2` and every int-typed native argument, making precision *stateful*); (b) `R23`'s open point: the table that supplies the initial BehaviorFlags. If PC turns out to flip at run time, `mixed_*` must switch behaviour after the first float→int conversion — cheap to implement, ugly, and only the asm can settle it.
2. **Ownership/refcount modelling diverges only on long runs** (spec risk 2). Probe: the `A/F` lines in every conformance trace (`cmp` C++ vs Python on the corpus, not only on A6 — T4 adds `conform-trace-cmp`), `lomn_tests_asan` with `detect_leaks=1` on the corpus, and the `heap.live` expectation (0 after every normal program).
3. **`%f`/`atof`/`atoi` CRT behaviour** (mixed CodeWarrior/MSVC exe, D5). M21 shows Python == macOS libc on 10 cases; the original's CRT is UNVERIFIED and PC=24 may even affect `atof`'s internal arithmetic. Probe: corpus programs use only decimal strings exactly representable in f32 (`"0.5"`, `"-2.25"`, `"3"`); the `%f` programs are tagged `UNVERIFIED-CRT` in the report; settled only by the LOMNHook trace (owner, M3).
4. **Re-entrancy** (natives calling `vm.call`; 69 engine functions do). Probe: the `t.reenter` conformance program (3.3) at context depth 3, plus a mutant that stores `framePtr` in a member the nested call overwrites without recomputing (expected caught). Error inside the nested call: the C++ exception unwinds through the native; the outer `call` reports it; the snapshot shows context depth 2 — a program asserts exactly that.
5. **wasm32 32-bit pointers in identity** — removed by D7 (handles). Residual probe: `grep -n 'reinterpret_cast\|uintptr_t' engine/src/osi/` must be empty (a tripwire test).
6. **Self-patch vs pristine image**: memoisation per site (D4). Probes: the `0x84` memoised-twice program; the tripwire `sha256(script bytes) unchanged after a run` asserted by `lomn_osi_run` at exit (the image is `const`; the test makes it observable anyway).
7. **Python oracle cost**: M20 gives 1.62 M insn/s traced; worst case (M13 bound) 45 s + 3 GB; digest mode bounds the disk; the first real run prints `insns/frame` and that number replaces the estimate (A15).
8. **Byte-identity killers I have seen before**: `snprintf("%g")` anywhere in the trace path (forbidden by the grammar), locale, `\r\n` on a Windows checkout (not our host), unordered iteration (none: all tables are vectors), FMA contraction (already off), NaN payloads (D9), wasm `std::fma` fallback correctness (probe: a unit test with the 6 M23 cases on both targets).
9. **The table itself** (shared-reading blind spot; ~10 % Important rate per audit). Mitigation already in place: author ≠ oracle author ≠ corpus authors; the T2 hand-back lists ambiguous rows; any A5 disagreement goes to the asm, never to a vote between the two implementations.
10. **Process slips that cost M2a-0 time**: shared scratch helpers overwritten (per-task scratch subdirs in every brief); the "reproduce" rule for generated+authored docs (regenerate over the committed file, then `cmp`); loadavg recorded at brief time.

## 7. Size estimate

ESTIMATE **11–16 agent-hours** for M2a-1 (sum of agent wall-clock as the ledger counts it): T0 0.5–0.8, T1 2.5–4, T2 1.5–2.5, T3a+T3b 1.6–2.4, T4 1–1.5, T5 0.8–1.2, blind reviews (6 × 15–25 min) 1.5–2.5, controller frozen-copy verification and the A5/A6 disagreement loop 1.5–3. Basis: M2a-0 took ≈ 4.7 agent-hours (M25) for ~2,500 lines of generators + tables; T1 alone is a ~2,500–3,500-line VM with 96 handlers and the T4 harness, T2 ~1,200 lines, the corpus ~300 programs. Confidence **medium-low (±50 %)**: the variance is the number of C++↔Python disagreements A5/A6 surface (each is a table re-read; M2a-0's audits found ~10 % Important rows, so 5–10 disagreements is the planning case) and whether risk 1 turns stateful. Wall-clock with the parallel fan-out of §1: ESTIMATE 5–7 h.
