# OSI VM opcode citation table (`ScOSIVirtualMachine`, canonical exe)

One row per opcode of the script VM in the canonical `LEGOBionicle.exe` (`tools/builds.json` "canonical"). This table
is the specification the two M2 interpreters (C++ `lomn::osi::Vm` and Python `tools/research/osi_vm.py`) are written
from (spec §5). Every authored cell cites, in brackets, the address of the handler it was read from; the semantics are
described in words, never as decompiled code.

- **Generated columns** (`op` … `base_osi_count`) are rewritten by `make osi-tables` (`tools/osi/opcodes.py`) from the
  exe read as data, the decomp corpus `gen/decomp/functions.jsonl` and `Base.osi`.
- **Authored columns** (`stack_effect`, `errors`, `notes`) were written on 2026-10-08 (M2a-0 T4) by reading each
  handler's decomp in `gen/decomp/funcs/` and, wherever the decomp garbles by-value `ScOSIVariant`s or `Pop` flags, the
  handler's disassembly (`objdump -d --x86-asm-syntax=intel` on the exe as data). The generator keeps them by `op`;
  everything outside the table markers is kept verbatim. The rows 0x60–0x64, 0x70–0x75 and 0xb0–0xb2 were
  re-derived from the disassembly alone in the M2a-0 sweep (each operand-type path cited by instruction address).
- `AMBIGUOUS:` in `notes` would mark a reading the binary does not settle, naming both readings. No row is marked:
  the three earlier ones (0x74, 0x82, 0x83) were resolved from the asm and the `.exc` tables (fix round 1).

## How the generated columns are derived

- `Run` (0x60b850) loops `___ptmf_scall(handlers + 12·*PC)` until a handler returns false: `movzx edx, byte [PC];
  imul edx, edx, 0xc; add edx, 0x752768` at 0x60b85c–0x60b862. `handlers` (0x752768, 12-byte entries) is all zero in
  the file. The `.CRT` initializer **FUN_0060c670** (`.CRT` index 346, in the corpus since the T2 re-run) fills it at
  start-up: from 0x60cd54 it holds 256 unrolled groups `mov edi, 0x752768 + 12·op; mov esi, 0x751768 + 16·op; movsd`
  ×3, so it copies the first 12 bytes of each 16-byte template entry into the 12-byte runtime entry of the same op.
  This is a fourth reader of the op → template mapping, from executed code
  (`test_crt_fill_copies_16_byte_template_entries_into_12_byte_handlers` pins the strides 16 and 12).
- The template at **0x751768** holds 256 × 16-byte CodeWarrior pointer-to-member entries `{this_delta, vtbl_offset,
  vptr_offset, pad}`. `___ptmf_scall` (0x6595a0): when `vtbl_offset ≠ −1` it calls the dword at
  `*(this + this_delta + vptr_offset) + vtbl_offset`. All 256 entries have `this_delta = vptr_offset = 0`.
- The vptr is **0x7514e4**, the value the constructor (0x60b230) stores; it is the `??_7ScOSIVirtualMachine@@6B@`
  symbol (0x7514dc) + 8 (two header dwords). `vtable_slot` = `vtbl_offset / 4` from 0x7514e4. Slots 0–5 are the
  destructor, `Free`, `Message`, `Error` (+0xc), `SetPCAndArgs`, `Run`.
- 96 opcodes map to named `BCL*` handlers, 160 to `BCLUnused` (0x60be60). The set of 96 equals the opcode set that
  `tools/research/osi.py` knows (`OPS` ∪ `NOARG`), with no exceptions either way.
- `operands` are `osi.py`'s struct letters (`b`/`B` 8-bit, `h`/`H` 16-bit, `i`/`I`/`f` 32-bit, `T` 24-bit); `none` =
  no operand. Where a handler reads an operand differently from `osi.py`, the row's `notes` say so (summary below).
- `base_osi_count` counts instructions in `Base.osi` over `osi.subroutines()` + `osi.disasm()` (the M1a-3 walk:
  151,522 instructions, 67 distinct opcodes).

## Conventions used in the authored columns

- **Stack notation.** `a b → r`: `b` was on top; `a` and `b` are popped (the top first) and `r` is pushed. `—` = no
  stack change. Operands follow the opcode byte; every handler advances PC past the opcode and its operands unless the
  row says otherwise.
- **Errors never return.** `Error(fmt, …)` is the virtual at vtable +0xc = `ScOSIVirtualMachine::Error` (0x60be80):
  `vsprintf` into a static buffer, then `SrOSIError` (0x5faee0), which prefixes "An OSI Error Occured" and calls
  `SrThrow(kOSIError)` (0x4d91f0) → `___throw`. Code after an error call is unreachable, and some handlers call
  `SrOSIError` directly (marked). Messages are quoted exactly as in the binary, typos included ("non-existant",
  "unimplimented", "Occured"). The thrown object is only `{kOSIError, kNoQualifier}`: the text lives in Error's
  static buffer (0x7e57e1), which the next Error overwrites, and `SrOSIError` called directly keeps it nowhere.
  Only 0x82, 0x83 and 0x84 catch anything (see "Exception tables"); everywhere else the error leaves the handler.
  One non-OSI error exists: string `+` overflow in 0x70 throws `kLenTooLong`.
- **Pop and ownership.** `ScOSIStack::Pop(dispose)` (0x5fd260) moves the slot out; with `dispose = true` it first
  calls `ScOSIVariant::Dispose` (0x60a930). Most handlers pop with `false` and then own the variant. Pops WITH dispose:
  0x30, 0x31, 0x52 (locals), 0x82/0x83/0x84 (arguments). `Dispose`: types 1, 5 and 0x20..0xfe release a reference
  (count at +4, virtual delete at zero); 0x13 releases a reference (count at +0, `SrFree`); 0x16 deletes the string;
  every other type does nothing. `Duplicate` (0x60aa20) adds a reference or deep-copies a 0x16 string.
- **No bounds checks.** `Push` (0x5fd0c0) and `Pop` check nothing: overflowing the 1,000-slot stack or popping an
  empty one corrupts memory in the original. Conformance programs must not depend on either.
- **Frames.** A script→script call (0x50/0x51) inserts a type-2 return marker below its arguments; the engine→script
  path (`CallWithReturn` 0x60b450) pushes a type-0xff marker. After a call or return, `framePtr` = the first slot,
  scanning down from the top, whose type is 2 or 0xff, plus one. So arguments are `framePtr[0..argc−1]`, locals
  follow, and `framePtr[0]` is `this` inside a method (0x15–0x17).
- **Integers** are 32-bit two's complement: add, subtract and multiply wrap. Exceptions are named per row: 0x73 uses
  signed `idiv`, 0x75 unsigned `div`, 0x78 a logical shift.
- **Floats** are computed on the x87. A float operand is loaded with `fld dword` / used as a `dword` memory operand,
  and an int operand of a mixed int/float operation (0x60–0x64, 0x70–0x73, and 0x74's int,float path; in 0x74's float,int path the fild reinterprets x's bits and b is a loop count) is loaded **exactly** with `fild dword`
  (a 32-bit int is exact on the x87): it is **not** converted to f32 first. The operation then runs on the exact
  operands, and:
  - arithmetic (0x70–0x73, and the `_pow` result of 0x74) is computed on the x87 at the **current precision-control
    setting** and then rounded to f32 by the final `fstp dword` (D8′, below);
  - comparisons (0x60–0x64) are `fcomp dword` against the f32 operand with no rounding at all. Port them as
    `(double)a CMP (double)b`. Every compare's `fnstsw` test is ordered: any NaN gives 0 (false), and +0.0 equals −0.0.

  **D8′ — precision control is VM state `fpu_pc` ∈ {24, 64}** (controller ruling 2026-10-08, replacing D8; the probe that
  found it is EXECUTED, its reachability INFERRED from the static call graph):
  - **Initial value 24.** `CreateDirectXWindow` (0x52e570) calls `CreateDevice` without `D3DCREATE_FPU_PRESERVE` (M1
    R23), which sets 24-bit precision, and `SetUpRenderer` runs before `SetUpScripting`. INFERRED; it becomes
    UNVERIFIED if `d3d8.dll` or another runtime path changes the precision before `Setup` (not readable from this exe).
    Whether `d3d8.dll` or the driver re-applies 24-bit precision later (per frame, on `Reset`) is also UNVERIFIED: the
    scan below covers this exe only. It is listed as an A6 risk: if it does, the mode can return to 24 after a switch.
  - **64 from then on, permanently**, once the VM performs a string → float conversion: `ConvertVariant` (0x60b9d0) calls
    `_atof` (0x65c7b0) at **0x60bd0d** for a 0x11/0x16 source with target 0x12 (0xb1 on a string, and a float-declared
    native argument given a 0x11/0x16 in 0x82/0x83/0x84). `_atof` → `_strtod` → `___strtold` → `___dec2num`
    (0x65f80a), which runs `fnstcw [ebp−0xe]; or word [ebp−0xe], 0x300; fldcw [ebp−0xe]` at 0x65f883–**0x65f886**–0x65f88c
    and returns at 0x65f8a8 without restoring. `___strtold` reaches it on every in-range parse (0x65c66d), so the
    switch happens whenever `_atof` runs, even when the text parses to 0. Exception (INFERRED from the asm, not run): the
    adjusted decimal exponent (after the digit normalisation at 0x65c5c1–0x65c5fa) outside [-32768, 32767], or a raw exponent above 0x7fff, sets `___strtold`'s overflow flag (0x65c539, 0x65c603–0x65c613) and returns ±HUGE_VAL
    (0x7620dc) or 0.0 (0x762538) through 0x65c626–0x65c65f without calling `___dec2num`, so the mode does not switch;
    corpus strings stay in the normal range. The other paths into `___dec2num` are dead: `___strtold`'s other caller
    (0x662f70) is in a function with no callers (controller-checked), and `___wcstold`'s call (0x66637d) has no live
    caller (review-checked).
  - **The native `gcscripttools.stringtofloat`** (body 0x564d50 → `GcScriptTools::StringToFloat` 0x5654b0, whose
    `_atof` call is at **0x5654bc**) sets 64 when it runs. The registry marks it (`sets_fpu_pc64` in
    `data/osi/natives.json`); the VM applies the flag when it calls that native, stubs included.
  - **After an error** `fpu_pc` becomes 64: the C++ exception runtime runs `fninit` (control word 0x037f, precision 64)
    before jumping to every landing pad, at **0x657068** (`FUN_00657010`, cleanup pads) and **0x657245**
    (`FUN_00657080`, catch pads), from `___throw` (0x657260). In the original every VM error ends the game (see "After
    an error"); our VM keeps running (D10), so it models the landing pads by setting 64 when an error is recorded.
  - **Every other control-word writer in this exe leaves the precision bits alone** (scan of the exe's `.text` only,
    EXECUTED 2026-10-08, re-derived by the review): of the 499 `fldcw` instructions, 472 are the compiler's float → int
    idiom `fnstcw X; or X, 0xc00; fldcw X; fistp; and X, 0xf3ff; fldcw X`, which changes only the rounding bits — among
    them `ConvertVariant` 0x60bc45–0x60bc69, which restores from the saved word; 26 are 13 save / load-constant /
    restore pairs in the MSL math functions (acos, asin, exp, tanh, cosh, sinh, ceil, floor, modf, hypot, trunc, exp2,
    and `___num2dec`, the `%f` path); 1 is `___dec2num`'s. There is no `fldenv`, `frstor` or `fxrstor`, and no
    `_controlfp` / `_control87` / `_fpreset` import. DLLs (`d3d8.dll`, drivers, and `D3dx8cw.dll`, which ships with the install and has ~80 control-word writers) are outside the scan.
  - **Port — one rule for every f32 result of 0x70–0x73, whatever the operand types:** result =
    round_f32(round_p(exact a OP b)), p = `fpu_pc` significand bits, round to nearest even, with the x87 **extended
    exponent range** at the round_p step (the intermediate never overflows or goes subnormal); round_f32 is the final
    `fstp dword` (subnormals, overflow to ±inf). Any exact method is acceptable: the C++ VM uses exact integer
    arithmetic (wide sums/products, quotient with a sticky bit) then the two roundings; Python: `fractions` (`tools/osi/expect_calc.py`).
  - **Where the two settings differ from one rounding:**
    - At p = 24 the result equals one rounding of the exact value **unless the result is an f32 subnormal**: the
      intermediate is rounded to 24 bits with the extended exponent (so it stays normal), then `fstp dword` rounds it
      again to fewer bits. + and − are never affected, and neither is int × float: their exact result is a multiple of
      2^−149, so a result below 2^−126 has at most 23 significant bits and the first rounding is exact. **× of two
      floats and ÷ are affected**: f32 0x24368a23 × f32 0x1af99066 gives 0x002c7cd4 at p = 24 and 0x002c7cd5 with one
      rounding (falsifier `0x72#2` below).
    - At p = 64 every f32 OP f32 result equals one rounding (64 ≥ 2·24+2, so double rounding is innocuous); int,float
      operands can differ (falsifier `0x70#3`).
    - Measured (EXECUTED, exact-rational script, 20,000 f32 pairs with uniform bit patterns per op, seed 7): p = 24 vs
      one rounding: + 0, − 0, × 27, ÷ 18 (all on subnormal results; 2,568 and 2,529 of the × and ÷ results were
      subnormal); p = 53 and p = 64 vs one rounding: 0 for every op. (An earlier draft claimed "float,float
      unaffected" from a run that never compared p = 24 and bounded the exponents; that claim was wrong.)
  - **Not affected:** every compare, `fild`, int → float (0xb1 on an int), and float → int. 0x74's `_pow` runs its own
    x87 code at the current setting (UNVERIFIED; corpus programs use exact powers only). 0x75 has no float path at all
    (no x87 instruction).
  - **Not the original:** the earlier port formula `(float)((double)a OP (double)b)` is the 53-bit setting, which this
    exe never uses while the script runs. Programs that tell the three apart: `16777217 1.0e-15 +` → 4b800001 at 24,
    4b800000 at 64 and at 53 (separates 24 from 64 and 53); `16777217 1.0e-10 +` → 4b800001 at 24 and 64, 4b800000 at
    53 (separates 53 from 64). Run before and after a string → float conversion, the pair separates all three readings
    and the "24 forever" reading (falsifiers `0x70#2`–`0x70#4`).
  - **Trace:** the header records the initial mode (`fpu=pc24`); a record `P <pc:08x> 64` is written at the moment the
    mode switches, in event order with the other records, and after an error directly after its `E` line
    (`docs/osi/trace.md` §3).
- **`ConvertVariant(v, target)`** (0x60b9d0), used by 0x82–0x84 and 0xb0–0xb2:
  - same type → `Duplicate`;
  - target int: float → **truncation** (the asm sets the x87 rounding field to chop, `fistp qword`, keeps the low 32
    bits), string → `atoi` = `_strtol(s, 0, 10)` (0x65cde0 → 0x65cd40), which **saturates** out-of-range values to 0x7fffffff / 0x80000000 (0x65cd95–0x65cdbb; it does not wrap — corrected 2026-10-08, the C++ VM review found all three readers had wrapped);
  - target float: int → `(float)int` (exact `fild`, one rounding by `fstp dword`, 0x60bd53–0x60bd55), string →
    `atof`, rounded once by `fstp dword` (0x60bd13);
  - target 0x16: int → `"%d"`, float → `"%f"`, 0x11 → dynamic copy;
  - a source not listed for the target → `{type 0, value 0}`, silently;
  - any target other than 0x10/0x12/0x16 → error "Requested variant conversion unimplimented.  Recompile to debug for
    types".

  A 0x11 source is first converted to a dynamic string **in place**: for an int or float target, ConvertVariant
  overwrites the caller's variant (its `ScOSIVariant&` argument) with `ConvertStringIDToDynamicString(v)`
  (0x60bbf9–0x60bc02, 0x60bcf9–0x60bd02) and then reads the text. Called from 0x82–0x84 on a stack slot, the slot is a 0x16 string only **transiently**: the handler then calls
  `PushInto` (0x60191b → 0x601ec0), which disposes the slot (0x601edc, frees the in-place string: trace `A s` … `F s`)
  and stores the converted value (0x601ef2), so the slot ends as the int/float (corrected 2026-10-08, oracle second-reader). See "Objects, arrays and strings" below.
- **NaN results (D9, M2a-1).** Any f32 result that is a NaN is stored as **0xffc00000**, the x87 "real indefinite" an
  `fstp dword` writes for an invalid operation (stable x87 knowledge; UNVERIFIED on this exe). This is **our choice**,
  not the original's behaviour in every case: the x87 propagates a NaN *operand* (quietened, payload kept) instead of
  writing the indefinite, so an operation on a NaN operand can give a different NaN in the original. Both interpreters
  canonicalise because wasm32 NaN payloads are not deterministic across engines. Conformance programs therefore never
  use a NaN operand (an invalid operation on ordinary operands, such as inf × 0, is fine: the x87 writes the
  indefinite there too).
- **Errors in the port (D10, M2a-1).** Inside our VM an error unwinds to the outermost engine call, which records it,
  writes the trace's `E` line and returns failure without touching VM state, so a snapshot shows exactly the state "After
  an error" documents (operands already popped stay popped, nothing pushed, 0x82: converted arguments stay converted).
  `fpu_pc` becomes 64 at that moment (the landing-pad `fninit`, D8′). Resetting afterwards is our own host behaviour,
  outside what the original defines. We add two errors the original does not have, with our own texts:
  `"Stack overflow (1000 slots)"` on a push at depth 1,000 and `"Stack underflow"` on a pop at depth 0. A literal 0x84 in
  the file is our error `"Direct call without a memoised site"` (the original would call an arbitrary address).
  **Engine-call nesting cap (port cap, not original behaviour).** Entering an engine call while 64 are already active
  (a native re-entering the VM through `vm.call`, recursively) raises `"Undefined: engine-call nesting limit (64)"` in
  both engines. The original has no cap: unbounded re-entry would overflow its native stack. The cap is never hit by the
  outermost call, so the error unwinds through the calling native's 0x82/0x83 catch like any other native error and is
  re-reported as `"Script is calling <cls>.<fn>"`; only the post-error snapshot shows it (depth and context_depth 64
  for a script with no arguments). Measured real need: at most 1 active call over the A6 run (Setup + 600 frames of
  `Base.osi`, 2026-10-09). The A6 stubs never re-enter, so 1 is a floor, not a measured game maximum. The cap keeps the
  wasm32 C++ recursion (about 7 frames per level) inside its stack. Corpus: `b-eng-reenter-depth`.

## Variant types seen in the handlers

| type | meaning | cited from |
|---|---|---|
| 0x00 | result of an unsupported conversion (not otherwise a VM type) | `ConvertVariant` 0x60b9d0 |
| 0x01 | script object (`ScOSIObject`, 32 bytes, refcounted; classID u16 at +0x14); owns its properties (below) | 0x13 / `ConstructObject` 0x60be00, 0xbf; ctor 0x608520, dtor 0x6085c0 |
| 0x02 | return marker (value = return PC) | 0x50, 0x51, 0x52 |
| 0x05 | array (`ScOSIArray`, refcounted; count at +0xc, element data at +0x10); shared by reference (below) | 0x90–0x96; ctors 0x5fc690/0x5fc710, dtor 0x5fc790 |
| 0x0f | nothing | `ScOSIVariant` ctor 0x60b2a0, 0x47 |
| 0x10 | int32 | 0x40–0x43, 0x45 |
| 0x11 | string-table index | 0x46 |
| 0x12 | float32 | 0x44 |
| 0x13 | "string table": a refcounted block (count at +4, length-prefixed strings from +0x10), read-only to scripts; no handler creates one, and no `CreateVariant` overload does (none of the 63; 0x6159e0 copies its argument's type, so a 0x13 can only pass through it) | 0x91, 0x92 |
| 0x14 | 16-bit colour from 0x49; rejected by the channel ops | 0x49 |
| 0x15 | 32-bit colour (R bits 24..31, G 16..23, B 8..15, A 0..7) | 0x48, 0xa0–0xa7 |
| 0x16 | dynamic string (heap `ScFixedStringTemplate<255>`, 268 bytes) | 0x70, 0x91, `ConvertVariant` |
| 0x20..0xfe | registered game types (game objects, refcounted). Member access on one (0x10/0x11/0x18/0x1a) is always-error, map never populated (see "The game-variable map") | 0x10, 0x11, 0x60, 0xbc, 0xbf |
| 0xff | engine-call frame-base marker | 0x52, `CallWithReturn` 0x60b450 |
| 0x200 | by-reference box `{0x200, pointer to a stack slot}`, built only around a native call | 0x82–0x84 |

### Objects, arrays and strings (ownership)

- **`ScOSIObject`** (ctor 0x608520, dtor 0x6085c0). Layout: vptr +0, reference count +4 (the ctor ends with `inc
  [this+4]`, so a new object holds 1), a vector of `nprops` variants at +8 (size +0xc, data +0x10, filled with nothing
  by 0x5fc830), classID u16 +0x14, class record +0x18, script +0x1c. **The object owns its properties**: the dtor
  disposes every property, back to front (index size−1 down to 0, 0x6085cd–0x6085e3), then frees the vector. A
  property store (0x10, 0x15, 0x1a) disposes the old value and moves the new one in, without `Duplicate`.
- **`GetProperty(m)`** (0x6086a0): the class record is a u8 count, then count × u16 property symbols. It returns
  `&props[i]` for the first i whose symbol is m (i counted in 8 bits), else null. 0x10, 0x15, 0x16, 0x18 and 0x1a test
  for null and raise "Reference to non-existant member property"; 0x11 does not.
- **`ScOSIArray`** (ctors 0x5fc690 = empty and 0x5fc710 = n nothings, dtor 0x5fc790). The same refcounted base (count
  +4, 1 after either ctor), the element vector at +8 (count +0xc, data +0x10). **Arrays are shared by reference**:
  `Duplicate` adds a reference (type 5), nothing copies the elements. The dtor disposes the elements **back to front**:
  `Dispose(data[count−1])`, then `pop_back`, until the count is 0 (0x5fc7a0–0x5fc7bd).
- **`ConvertStringIDToDynamicString(v)`** (0x60b880) returns `{0x16, new 268-byte string}` built from the string-table
  entry `v.value & 0xffff` (the script's pointer table at script+0x10; entry + 1 skips one prefix byte). Its string
  constructor 0x60b8d0 throws `kOutOfRange` (`SrThrow` at 0x60b970, a non-OSI error) if the text has 255 characters or
  more. **It replaces the slot in place**: 0x70 (call 0x604053) and 0x60 (call 0x605369) overwrite the popped
  operand's local with the result (0x604058–0x604062, 0x60536e–0x60537b), and `ConvertVariant` overwrites its source
  reference (above). The old 0x11 value owns nothing, so nothing is disposed; the new string is disposed with the
  operand or, for a stack slot, by the later `Pop(true)`.
- **`CreateVariant`**: of its 63 overloads, 12 store 0x10, 2 store 0x12, 1 stores 0x15 (`StRGBABytes`, 0x447890), 2 store
  0x16, 45 store a game type (`theOSIType<Class>`; a null pointer gives nothing) and 0x6159e0 copies its argument
  (asm read of each overload's `mov word [ret], …`). **None creates a 0x13.**

### The game-variable map (0x10/0x11/0x18/0x1a on a game type)

**Always-error, map never populated.** A member access on a game-type value (0x20..0xfe) looks up
`map<ScOSIDualToken, RegisteredVariable*>`, and nothing in the exe ever inserts into that map, so every such access
raises "Reference to unregistered Game Variable" (`SrOSIError`). Evidence (all from the exe's bytes):

- The map is the 16-byte tree at **`ScOSISystem`+0x14**. The ctor 0x5fa750 builds the four maps of the system with
  their own tree ctors: +0x4 `RegisteredFunction` (0x5fb390), **+0x14 `RegisteredVariable` (0x5fb410, call
  0x5fa799)**, +0x24 type names (0x5fb5d0), +0x34 globals (0x5fb660). The dtor 0x5fa880 walks +0x14 from its begin
  pointer (+0x20) to its end node (+0x18), deletes each value, then calls the `RegisteredVariable` `clear` (0x5fb110)
  and `destroy` (0x5fb3d0).
- The only readers are the four `find` (0x5ff7a0) calls at 0x5fe74d (0x10), 0x5fead4 (0x11), 0x5ff0b2 (0x18) and
  0x5ff612 (0x1a), each with `ecx = [0x74d644] + 0x14` (the system pointer).
- `symbols.txt` has five `RegisteredVariable` tree functions (map dtor 0x5fa820, `clear`, `destroy`, tree ctor,
  `find`) and no `insert_one`, `insert_node_at` or copy. Folded code could hide an insert under another type's
  symbol, so the call graph was checked too. Every red-black insert ends in `__red_black_tree::balance_insert`
  (0x49b790). A raw `E8`/`E9` scan of `.text` finds 10 calls to it, each inside the `insert_node_at` of a different map
  type. The `RegisteredFunction` one (0x5fbcb0) is called only by `insert_one` 0x5fb290, which is called only at
  0x5fac1f in `RegisterFunction`, with `ecx = this + 4`.
- No other code reaches +0x14. After each of the 1,156 `mov reg, [0x74d644]` loads in `.text`, every `[reg + d]`
  operand within 40 instructions was collected: +0x14 appears only at the four `find` sites above. The `ScOSISystem`
  methods (0x5faa10–0x5fb110, `AddGlobal`, `GetGlobalVariable`) never address +0x14..+0x20 of `this`.
- There is no `RegisterVariable` symbol, and no absolute reference to any `RegisteredVariable` tree function other than
  the `.exc` records and one cleanup pointer to the map dtor.

So the "game type" paths of 0x10, 0x11, 0x18 and 0x1a in the table are dead code in this exe: the lookup always misses.
A port raises the error and need not model getters or setters.

## Exception tables (`.exc`), decoded for 0x70 and 0x82–0x84

The `.exc` section (0x6ef000) is an index of `{function start, info pointer}` pairs. A function's info record (for
0x6015a0 it is at 0x6dafe4) is `u8 flags, u32 action-table pointer, u16 n`, then `n × {u32 call-instruction address,
u32 action-chain pointer}`. A call that is not listed has no actions, so an exception there leaves the function
directly. An action chain is a list of records that ends at the first record whose flags have bit 0x8000. The kind
is `flags & 0xff`, as the runtime reads it (0x656893, 0x656d99). The sizes below are the runtime's own: the unwinder
FUN_00656d60 (jump table 0x761dc4) and the walker FUN_006568a0 (table 0x761d64) advance by the same amount per kind
(M2a-2 T2a, R2; `tools/osi/exc.py` decodes all of them):

| kind | action | layout after `u16 flags` | bytes |
|---|---|---|---|
| 0x01 | destroy a local | i32 frame, u32 destructor | 10 |
| 0x02 | destroy a local if a flag is set | i32 frame, i32 flag frame, u32 destructor | 14 |
| 0x04 | destroy a local through a pointer | i32 frame, u32 destructor | 10 |
| 0x05 | destroy a local array | i32 frame, u32 destructor, u32 count, u32 element size | 18 |
| 0x06 | destroy an array described by locals | 4 × i32 frame | 18 |
| 0x07 | destroy a member | i32 frame, u32 destructor, u32 member offset | 14 |
| 0x08 | destroy a member if a flag is set | i32 frame, i32 flag frame, u32 destructor, u32 member offset | 18 |
| 0x09 | destroy a member array | i32 frame, u32 destructor, u32 member offset, u32 count, u32 element size | 22 |
| 0x0a | delete a pointer | i32 frame, u32 operator delete | 10 |
| 0x0b | delete a pointer if a flag is set | i32 frame, u32 operator delete, i32 flag frame | 14 |
| 0x0c | destroy a base | i32 frame, u32 destructor, u32 base offset | 14 |
| 0x10 | **catch** | u32 type-name pointer or 0 for catch(...), u32 landing pad, i32 frame | 14 |
| 0x11 | end of an active catch | i32 frame | 6 |
| 0x12 | terminate (`std::terminate`; only as a final record, 2 in the runtime's own functions) | — | 2 |
| 0x13 | exception specification | u16 count, u32 unexpected-handler pc, i32 frame, count × u32 type-name pointer | 12 + 4 count |

**Only kind 0x10 catches.** The catch search FUN_00657080 dispatches on the kind (table 0x761e50): 0x10 goes to
`___throw_catch_compare` (call 0x65716f), 0x13 to the specification check (0x65718b), kinds 0x03, 0x0d–0x0f and 0x12
to `std::terminate`, and every other kind is skipped. With these sizes all 7,002 indexed functions decode with no
unknown record (28,221 call sites, 15,060 distinct chains; `tools/tests/test_osi_exc.py`).

An info record is only 4 bytes long, and has no call-site table, when its flags byte has **bit 0 set** or its bytes
1–3 are zero: nothing in that frame is destroyed or caught. Of the 7,002 index entries that have an info record (the
index holds 7,003 `{function, info}` pairs; one has a null info pointer), **4,966** are short: 4,605 by bit 0 (e.g.
`Run` 0x60b850, flags 0x09) and 361 with bit 0 clear but bytes 1–3 zero (flags 0x00, 0x08, 0x48, 0xc8 and similar;
measured in M2a-2 Task 1). The `{flags, action-table pointer, n}` layout holds in the other 2,036. A decoder that
ignores either rule misreads the next function's action chains as call sites.

The landing pads decoded this way match the catch blocks in the asm. Each of them stores the caught ScError, or
`_kMSLError` / `_kUndefinedError` for `!exception!` / `...`, and continues to the code after the try. The catch-type
names are `!ScCatchMessage!` (0x74ec0c) and `!exception!` (0x74ec00). Decoding was done with a scratch script
(not committed); the per-row `errors` cells name the call sites and landing pads used. In 0x70 the only record around
`ScFixedString::append` (0x6040bf) is a kind-0x13 specification allowing `!ScCatchMessage!`, so `kLenTooLong` (thrown
as `!ScCatchMessage!!`) passes through.

## Corrections to earlier notes, found while reading (each cited in its row)

- String `+` (0x70) does not truncate. At 255 characters or more it throws `kLenTooLong` from
  `ScFixedString::append` (0x4f5c80), after both operands have been popped and before anything is pushed.
- A by-reference (0x200) native argument reaches the native as the address of its stack slot, not of the box
  (0x60198c–0x601abc).
- 0x82/0x83/0x84 catch nearly every failure and re-report it as "Script is calling …" (decoded from `.exc`). Only the
  map miss in 0x82 and the name-type check in 0x83 escape with their own message.

- Float → int conversion (0xb2, and native arguments declared int) **truncates**. Ghidra's `ROUND()` and the consult's
  "ROUND" were misreadings (asm 0x60bc45–0x60bc6f).
- The `Pop` flag in the decomp is unreliable. 0x53 (0x600df0) and 0x15 (0x5fec10) show `Pop(true)` but push 0 (no
  dispose) in the asm. Every `Pop` flag here was taken from the asm.
- Frame indices are never negative: bit 15 of the operand selects a global (0x21), so arguments start at
  `framePtr[0]`. The consult said "negative = arguments".
- `Halt` (0xff) pushes a *nothing variant*; 0x80/0x81 change no state at all; 0x49 pushes type 0x14, not 0x15.
- Mixed int/float operations (0x60–0x64, 0x70–0x73, and 0x74's int,float path) do **not** convert the int to f32 first (M2a-0 sweep). The int is
  loaded exactly by `fild`, arithmetic rounds once at the final `fstp dword`, and compares are exact (e.g. 0x71:
  `fild` 0x60435e, `fsub dword` 0x604361, `fstp dword` 0x604363). The earlier cells shared the "converted to float
  first" misreading, as did an earlier audit that "confirmed" 0x63 and 0x72 (per the sweep brief). 0x73's note "no
  IEEE inf/NaN from division" was also wrong: zero divisors are errors, but f32 overflow still gives ±inf and a NaN
  operand gives NaN.

## Conformance falsifiers for M2a-1 (mixed int/float, M2a-0 sweep)

Each line is `a b op` → the value the original computes, then the value the old "int converted to float first"
reading gives. Ints are written without a decimal point and floats with one. Every expected value was computed
from the exact rational result rounded per D8′ (round_p, then f32; single rounding except the subnormal ×/÷ line 0x72#2) (a scratch Python script using `fractions` and `numpy.float32`,
not committed). For the first 11 lines every result rounds to the same f32 at 24, 53 and 64 bits (each one either is
exact in 53 bits or overflows: `1.0e30 1.0e-30 /` is +inf under every setting), so they do not depend on the precision
setting. The last four lines (D8′, fix round 1) are the exception: `0x70#2`–`0x70#4` run at `fpu_pc` 24 and, after a
0xb1 on a string, at 64, and `0x72#2` is the float × float subnormal case at 24. Their "old reading" column names the
alternative reading each one kills. `tools/osi/expect_calc.py` reproduces every line (`tools/tests/test_expect_calc.py`,
both settings for the first 11, and the alternatives of the last four).
f32(16777217) = 16777216.0 and f32(16777219) = 16777220.0 (round to nearest even).

| row | program | original (asm reading) | old reading |
|---|---|---|---|
| 0x60 | `16777217 16777216.0 =` | 0 | 1 |
| 0x61 | `16777219 16777220.0 <` | 1 | 0 |
| 0x62 | `16777217 16777216.0 >` | 1 | 0 |
| 0x63 | `16777217 16777216.0 <=` | 0 | 1 |
| 0x64 | `16777219 16777220.0 >=` | 0 | 1 |
| 0x64 | `16777216.0 16777217 >=` (the float,int path) | 0 | 1 |
| 0x70 | `16777217 1.0 +` | 16777218.0 | 16777216.0 |
| 0x71 | `16777217 1.0 -` | 16777216.0 | 16777215.0 |
| 0x72 | `16777217 3.0 *` | 50331652.0 | 50331648.0 |
| 0x73 | `50331651 3.0 /` | 16777216.0 | 16777218.0 |
| 0x73 | `1.0e30 1.0e-30 /` | +inf, no error | (old note: "no IEEE inf") |
| 0x70 | `16777217 1.0e-15 +` at `fpu_pc` 24 (no string → float before it) | 16777218.0 (4b800001) | 53-bit formula: 16777216.0 (4b800000) |
| 0x70 | `16777217 1.0e-15 +` at `fpu_pc` 64 (after `"0.5"` 0xb1) | 16777216.0 (4b800000) | "24 forever": 16777218.0 (4b800001) |
| 0x70 | `16777217 1.0e-10 +` at `fpu_pc` 64 (after `"0.5"` 0xb1) | 16777218.0 (4b800001) | 53-bit formula: 16777216.0 (4b800000) |
| 0x72 | `f32:0x24368a23 f32:0x1af99066 *` at `fpu_pc` 24 (subnormal result) | 0x002c7cd4 | one rounding (= 53, 64): 0x002c7cd5 |

0x74 (int,float), 0xb1 (int → float) and 0xb2 did not change meaning: `pow` already took `(double)a`, and
`ConvertVariant` already rounds an int to f32 once. Guards kept for them: 0xb1 `16777217` → 16777216.0; 0xb2
3.0e9 → −1294967296 (the low 32 bits of the `fistp qword`).

## Operand-format notes against `tools/research/osi.py`

- 0x14, 0x23, 0x31, 0x33, 0x34 and the argc byte of 0x50/0x51/0x82/0x83/0x84 are read **unsigned** by the handlers.
  `osi.py` uses signed `b`. No Base.osi operand reaches 128 (largest seen: 20), so the walk is unaffected; the
  interpreters must read them unsigned.
- 0x41 (`T`): the handler never reads its operand; it always errors and does not advance PC.
- 0x84 (`i`): a raw host pointer that exists only after self-patching.
- 0x18/0x19/0x1a and 0x83 take their member or function name from the stack, not from an operand.

## Engine calls (Call / CallWithReturn)

Every engine→script call goes through `ScOSIVirtualMachine::Call` (0x60b320, 144 call sites, raw `E8` scan) or
`CallWithReturn` (0x60b450, called directly only by `ScSLOSIProcess::Process` at 0x43b32d and by `Call` at 0x60b402).
- The names pushed as string literals in front of the `Call` sites are `OnEvent`, `Input` and `Setup` (per-name counts depend on the counting method: a backward window for the nearest `push imm32` gives OnEvent 136 + 5 "other"; a 600-byte window over every literal copy gives OnEvent 140, Input 2, Setup 1, 1 other — the final-review re-check and probe 3 agree on 140/2/1).
- 5 sites take the name from elsewhere: 0x476950, 0x55d089, 0x55d1aa, 0x57e051, and 0x61406f
  (`ScSLOSIAnalogControllerAction`, a name given to its ctor).
- `ScSLOSIProcess` passes its stored name (`Process`, given by `GcGame::SetUpScripting`).

Everything below was read from the asm of the function named.

**`ScOSIScript::FindFunction(name)`** (0x609ce0) walks the global-function table (count: a **u8** at script+0x20;
table at script+0x24). Each entry is `{u8 len, name[len], u32 offset, u16 argc}` (not NUL-terminated). The first entry
for which `__strnicmp(name, entry, len) == 0` (0x65bdf0) wins. It returns `{script base (+0x4) + offset, argc}`, or
`{0, 0}` when nothing matches.
- `__strnicmp` compares `tolower` of both bytes for the entry's own `len` characters only, stopping early at a shared
  NUL. `tolower` (0x65ef50) is a table (0x762a18) that folds exactly A–Z.
- So the match is **prefix-tolerant**: an entry matches every query that **starts with** the entry's name,
  case-insensitively. Query characters after `len` are never looked at. A query shorter than the entry fails, because
  its NUL meets a letter.
- The `Base.osi` table is in alphabetical order, so `setup` (entry 4) shadows every `setup…` entry for any query that
  starts with "setup": a query `SetupLevel_1` resolves to `setup`. The names the engine passes (`Process`, `Input`,
  `OnEvent`, `Setup`) are unaffected; each equals an entry.

**`SetPCAndArgs(name, a0..a9)`** (0x60b5e0, vtable +0x10; the arguments are references):
- Not found (code pointer 0) → returns false at once: PC is unchanged, nothing is pushed and **no argument is
  disposed**.
- Found → PC (VM+0x1f58) = the code pointer. If argc > 10 (`cmp ax, 0xa; jbe` at 0x60b62c), it raises
  `Error("Too many args required (max 10) to call requested entry point.")` (string 0x7512a0, vtable +0xc).
- Then, for i = 0..9: if i < argc, a bitwise copy of a_i is **pushed** (ownership moves to the stack, no
  `Duplicate`); otherwise a_i is **disposed** (0x60b644–0x60b838).
- So a callee gets **exactly argc** of the 10 supplied variants, as declared by the script, not by the engine.
  `Base.osi` declares `process` 3, `input` 5, `onevent` 10, `setup` 0. Returns true.

**`CallWithReturn(name, a0..a9)`** (0x60b450), in order:
1. result = `{0x0f, 0}`; the VM's reference count (+0x4) is incremented.
2. A `{0xff, PC}` marker is pushed (0x60b484).
3. framePtr (+0x1f54) = (the first slot, scanning down from the top, whose type is 2 or 0xff) + 1, i.e. the slot just
   above the new marker (0x60b489–0x60b4b1).
4. `mContextDepth` (a **u16** at +0x1f4c) is incremented.
5. `SetPCAndArgs` (vtable +0x10, call 0x60b4ee).
   - If it returns false: `Message("Unable to locate function %s in scripts", name)` (string 0x751278; vtable **+0x8**,
     `Message` 0x60bef0). This is **not an Error**: it only `vsprintf`s into a static buffer (0x7e5be1) and returns.
     Run is skipped and the result stays nothing. The supplied arguments are neither pushed nor disposed (a refcounted
     or string argument leaks).
   - Otherwise: `Run` (vtable +0x14, call 0x60b4f9), then `Pop(false)` = the result (`push 0` at **0x60b4fc**).
6. `Pop(true)` (`push 1` at **0x60b53e**) is repeated until the popped slot's type is 0xff. On the normal path this pops
   only the marker, since 0x52 leaves `{0xff, r}` and r was popped in step 5.
7. `mContextDepth` is decremented. If it is not 0, framePtr is recomputed by the same scan (0x60b569–0x60b59b).
   Otherwise it is left as it was.
8. PC = the marker's value (0x60b5a2). The reference count is decremented; at ≤ 0 the VM frees itself (vtable +0x4).
   The result is returned.

**`Call(name, a0..a9)`** (0x60b320) = reference count +1, `CallWithReturn` on bitwise copies of the 10 arguments
(0x60b402), `Dispose` of the result (0x60b417), reference count −1 (vtable +0x4 at ≤ 0).

**The stack** is 1,000 slots of 8 bytes inside the VM, built by the VM ctor 0x60b230 with
`__construct_array(stack, ScOSIVariant ctor 0x60b2a0, dtor 0x413ec0, 8, 1000)` (`push 0x3e8; push 8` at 0x60b257). The
top pointer is at stack+0x1f44 (VM+0x1f50), and `mContextDepth` = 0 and PC = 0 are set there too. `Push` (0x5fd0c0)
and `Pop` (0x5fd260) never check the bounds, and nothing checks the depth of script recursion through 0x50/0x51: the
only limit is the 1,000 slots, and overrunning them corrupts the VM object. `mContextDepth` is a u16 that nothing
compares except against 0.

### After an error

`Error` (vtable +0xc) and `SrOSIError` throw kOSIError (`SrThrow` 0x4d91f0 → `___throw` 0x657260 with the type list
`!ScCatchMessage!!`, which a catch(`!ScCatchMessage!`) matches).
Run (0x60b850: flags 0x09, no call-site table), `CallWithReturn` and `Call` hold only `ScOSIVariant` destructor actions
(0x413ec0, which is `mov eax, ecx; ret`). None of them catches, and none of their epilogues runs. Each engine caller
was followed up its call chain with the `.exc` decoder above (call-site records re-derived at every site):

| engine caller (call site) | `.exc` at the site | frames above it | where it is caught |
|---|---|---|---|
| `ScSLOSIProcess::Process` 0x43b250 (`CallWithReturn` 0x43b32d) | 10 variant dtors | `ScProcessManager::Process` 0x4a9170 (vtable +8 calls at 0x4a93a5/0x4a949f: dtors only) → main loop 0x538700 (flags 0x49: no table) → `AppMain` 0x5381f0 (call 0x53869a: dtors only) | `_WinMain@16` |
| `GcMouseDriver::Process` 0x55c160 (0x55c753, 0x55c89a, … 0x55d4a3, 13 sites) | 10 variant dtors each | a process (vtable +8 = 0x55c160): the same chain | `_WinMain@16` |
| `GcKeyMapper::ProcessKey` 0x5d1a10 (0x5d1d0f, 0x5d1de2) | 10 variant dtors | `GcKeyMapper::Process` 0x5d1e30 (a process; no table), or `KeyPressed` 0x5d18e0 (no table), whose address sits in the pointer-to-member table `sKeyFlagMap` that `ScCallbackAction<GcKeyMapper>::Fire` 0x5d2530 (no table; vtable +0xc) calls through `___ptmf_scall` (target INFERRED), ← `ScAction::IsRequested` and the trigger chain below | `_WinMain@16` |
| `ScSLOSIAnalogControllerAction::Fire` 0x613f80 (0x61406f) | 10 variant dtors | `ScAction::IsRequested` 0x491ed0 (calls Fire at 0x491f4e; no table) ← `ScTrigger::Trigger` 0x4c1bd0 (no table) ← `ScTrigger::Process` 0x4c1b90 (no table) ← `ScTriggerTree::ProcessTrigger` 0x4c4420 (call 0x4c44a8: dtor only) ← `ScTriggerManager::ProcessTriggers` 0x4c2bf0 (dtors only) ← `GcControllerProcess::Process` 0x44a7a0 (a process; no table) → the process chain | `_WinMain@16` |
| `GcTimer::Trigger` 0x445ae0 (0x445bca) | 10 variant dtors | `ScTrigger::Process` 0x4c1b90 (vtable +0x20; `GcTimer::Process` 0x445920 has no table) → the trigger chain | `_WinMain@16` |
| `GcGame::SetUpScripting` 0x4390d0 (`Call("Setup")` 0x43925a) | 10 variant dtors, then catch(`!ScCatchMessage!`) 0x439324, catch(`!exception!`) 0x439343, catch(...) 0x439363 | the catch stores the ScError and skips the rest of the try (`MapControllerToScript`, the context provider and the `ScSLOSIProcess("Process")` + `AddProcess`). After the try a stored error is **re-thrown** (`___throw` 0x4393e9). `GcGame::DoSetUp` 0x436a10 has no record at its call 0x436bcd; `AppMain` (call 0x53867d) has dtors only | `_WinMain@16` |

**`_WinMain@16`** (0x401020) wraps `SrInitializeAll` + `AppMain` (calls 0x40105f and 0x401078) in catch(`!ScCatchMessage!`)
**0x401080**, catch(`!exception!`) 0x401093 and catch(...) 0x4010b0. Each stores the error code (the ScError, `_kMSLError`
or `_kUndefinedError`) and continues at 0x4010d3.
- It then compares the code with `_kNoException`, but the result (edx) is never used.
- It calls `SrDestroyAll` if `SrInitializeAll` had finished, and returns 0 (0x401134).
- Its re-throw flag ([ebp−0x38]) and its retry flag ([ebp−0x3c]) are only ever written 0.

So **a kOSIError from any engine→script call on these paths ends the game**. The process exits normally with code 0.
No message box is shown: the formatted text sits in `Error`'s static buffer and nothing reads it. This is INFERRED
from the asm; the exe was never run.

Consequences for the VM: in the original there is no next call. The error unwinds past `CallWithReturn` and `Call`
without their epilogues, so the state at exit is as follows.
- The VM reference count is still +2 (`Call` and `CallWithReturn` each).
- `mContextDepth` is still +1.
- The 0xff marker, the frame and any partial values are still on the stack.
- PC points into the failed handler's instruction. framePtr points into the failed frame.

A port that chooses to keep running after a script error is outside what the original defines. It must reset these
four itself, and conformance tests must not depend on behaviour after an error.

## Error PC (the PC at the throw), per handler

**What this is.** The VM PC (`[VM+0x1f58]`) that each handler has written when it raises, given relative to the start
of the instruction being executed (+0 = not advanced, +1 = past the opcode byte, and so on). D10 leaves the VM state
"as at the throw", and D16 continues dispatch at exactly this PC after a swallowed nested error, so both engines must
leave it. Conformance rule 15 (`docs/osi/conformance.md` §6) points here.

**How it was derived** (EXECUTED 2026-10-09, `tools/osi/error_pc.py`, test `tools/tests/test_osi_error_pc.py`). The
exe is read as data: `objdump -d` on `.text`, the handler table through `tools/osi/opcodes.py`, the `.exc` tables
through `tools/osi/exc.py`. From each of the 97 distinct handlers the pass walks every path with a small abstract
interpreter that follows `this` (the VM, `ecx` at entry) through registers and frame slots.
- **PC writes.** It counts the PC as a delta from the instruction start. Every PC write reached is one of four forms:
  `inc [this+0x1f58]` (161 instructions), `add [this+0x1f58], imm` (4), `add [this+0x1f58], reg` (1, 0x54's s16
  branch offset) and `mov [this+0x1f58], reg` (3: the 0x50/0x51 jump targets and 0x52's return address). Any other
  store to that address, or a store at displacement 0x1f58 through a base the pass lost track of, stops the pass with
  an error. A grep of every `[reg+0x1f58]` store in the linear-sweep listing of `.text` finds writes outside the
  handlers only in the constructor (0x60b283), `CallWithReturn` (0x60b5a5) and `SetPCAndArgs` (0x60b623). That grep
  misses 6 handler writes, which the sweep decodes out of step; `test_pc_write_forms` reconciles the two counts.
- **Raises.** `Error` (vtable +0xc) and direct calls to `SrOSIError` 0x5faee0, `SrThrow` 0x4d91f0 and `___throw`
  0x657260 end a path. Calls to VM methods (the vtable, `ConvertVariant`, `ConvertStringIDToDynamicString`,
  `ConstructObject`) are followed inline with the current PC. Every other direct call is scanned, with its own direct
  callees, for those throwers. The indirect calls (natives, landing-pad cleanups, the dead game-variable accessors)
  are classified by hand in the script, which refuses one it does not know.
- **Catches.** A raise at a call site with a catch record (kind 0x10, `!ScCatchMessage!` or catch-all) continues at
  the landing pad with the PC unchanged. The CodeWarrior retry and rethrow byte flags (written only with 0) and the
  `kNoException` identity test after each try are evaluated. Every other branch is taken both ways. So a row reached
  only through an impossible branch could appear. The "caught" exits are reported apart and never counted as normal
  exits.
- **Self-check.** Every handler that falls through leaves PC at 1 + its operand bytes on every normal exit (osi.py
  `OPS`, test `test_normal_exits_match_operand_sizes`), and the pinned rows were read by hand from objdump.
- **The oracle agrees (EXECUTED):** the 178 corpus programs that expect an error were run through
  `tools/research/osi_vm.py`. `snapshot().pc − pc` equals this table for all 173 whose error the original raises
  (168 `Error`/`SrOSIError` rows, 3 `FindMethod` "Call to undefined member method", 2 kLenTooLong). The 2 stack-cap
  programs match the ScOSIStack column. The remaining 3 are port and engine rules (a literal 0x84, argc > 10 and the
  nesting cap; conformance rule 15). The C++ VM was **not** run for this. It sets PC at the end of most handlers, and
  its differences are listed in the M2a-2 T2a-pc report (from reading the source).

**The result in words.** Almost every handler advances PC past its whole instruction at entry, before any pop or
check, so its errors are at **+(1 + operand bytes)**. The exceptions:
- **+0:** BCLUnused (160 ops) and 0x41 never write the PC. 0x53 pops and checks before it writes the PC: its error and
  its pop are at +0, and only then is the PC set (+3, or through 0x54 inlined to +3 + offset). 0x52 raises nothing; its
  pops are at +0.
- **0x50 / 0x51: +2.** The operand is consumed, and the error comes before the jump target is stored.
- **0x82: +5, then +6.** The map miss (`SrOSIError` at 0x601725, outside the try) is raised before the sixth `inc`
  (0x601780). Every error after it, and the re-raise "Script is calling %s.%s" at 0x601dd3, is at +6.
- **0x83: +3, then +4.** "Invalid member type…" (0x6022c3) is at +3. The n byte is read (`inc` 0x60237c) inside the
  try before the lookup, so every caught OSI error is re-raised at 0x602b24 with +4. +3 reaches that re-raise only
  after an allocation failure.
- **0x84 (the self-patched 0x82): +6** for the whole body, and for its re-raise "Script is calling a game function
  directly." at 0x60303c.
- **A native's own error** is raised with the PC the handler had at the native call (+6 for 0x82/0x84, +4 for 0x83).
  If the native made a nested engine call and **that** raised, the nested `CallWithReturn` never restores the PC
  (opcodes.md "After an error"). The error then carries the inner handler's PC at its throw, by this same table, and
  the outer re-raise keeps it. INFERRED from the asm of `CallWithReturn`; D16 depends on it.

Not listed per row: 73 allocation failures (`operator new` / `new[]` throwing error object 0x83aa0c, std
`length_error` in the containers), which the port does not model, and 5 cleanup calls in landing pads. Each table
counts them per handler. The "PC at its errors" column of the first table is the PC at every listed raise of that
handler on a direct (not caught) path. The "PC at its ScOSIStack calls" column is the PC at each `Pop`/`Push`/
`PushInto`/`Insert` call. Rule 15 uses it for the port's own stack errors.

<!-- BEGIN error-pc (generated: tools/osi/error_pc.py --markdown) -->

Generated by `tools/osi/error_pc.py --markdown` from the canonical exe: 97 handlers, 272 raise sites (73 allocation failures and 5 landing-pad cleanup calls are counted, not listed), 164 rows below. PC writes reached from the handlers: add imm 4, add reg 1, inc 161, mov reg 3.

| op | handler | PC writes (instruction → PC after it) | PC at its errors | PC at its ScOSIStack calls | normal exit | allocation-failure sites |
|---|---|---|---|---|---|---|
| 0x00 | 0x603830 | 0x603832 → +1 | no error | — | +1 | 0 |
| 160 unused ops (0x01, 0x07, …, 0xfe) | 0x60be60 | none | +0 | — | never returns | 0 |
| 0x02 | 0x6038a0 | 0x6038a6 → +1 | +1 | — | never returns | 0 |
| 0x03 | 0x6038d0 | 0x6038d6 → +1 | +1 | — | never returns | 0 |
| 0x04 | 0x603870 | 0x603872 → +5 | no error | — | +5 | 0 |
| 0x05 | 0x603880 | 0x603882 → +5 | no error | — | +5 | 0 |
| 0x06 | 0x603890 | 0x603892 → +5 | no error | — | +5 | 0 |
| 0x10 | 0x5fe610 | 0x5fe61f → +1, 0x5fe62e → +2, 0x5fe646 → +3 | +3 | pop +3 | +3 | 4 |
| 0x11 | 0x5fe9a0 | 0x5fe9af → +1, 0x5fe9be → +2, 0x5fe9d3 → +3 | +3 | pop +3; push +3 | +3 | 4 |
| 0x12 | 0x5fe590 | 0x5fe59a → +1, 0x5fe5a6 → +2, 0x5fe5b5 → +3 | +3 | pop +3; push +3 | +3 | 0 |
| 0x13 | 0x5fe530 | 0x5fe53a → +1, 0x5fe546 → +2, 0x5fe555 → +3 | no error | push +3 | +3 | 2 |
| 0x14 | 0x5feb90 | 0x5feb9b → +1, 0x5feba7 → +2 | +2 | push +2 | +2 | 0 |
| 0x15 | 0x5fec10 | 0x5fec1f → +1, 0x5fec2e → +2, 0x5fec40 → +3 | +3 | pop +3 | +3 | 0 |
| 0x16 | 0x5fecf0 | 0x5fecff → +1, 0x5fed0e → +2, 0x5fed20 → +3 | +3 | push +3 | +3 | 0 |
| 0x17 | 0x5fedb0 | 0x5fedbb → +1, 0x5fedc7 → +2, 0x5fedd9 → +3 | +3 | push +3 | +3 | 0 |
| 0x18 | 0x5fee40 | 0x5fee58 → +1 | +1 | pop +1; push +1 | +1 | 4 |
| 0x19 | 0x5ff210 | 0x5ff21e → +1 | +1 | pop +1; push +1 | +1 | 0 |
| 0x1a | 0x5ff3a0 | 0x5ff3b8 → +1 | +1 | pop +1 | +1 | 4 |
| 0x21 | 0x607c50 | 0x607c5a → +1, 0x607c66 → +2, 0x607c75 → +3 | +3 | push +3 | +3 | 1 |
| 0x22 | 0x607b50 | 0x607b59 → +1, 0x607b65 → +2, 0x607b77 → +3 | no error | pop +3; push +3 | +3 | 1 |
| 0x23 | 0x607b00 | 0x607b09 → +1, 0x607b15 → +2 | no error | push +2 | +2 | 0 |
| 0x24 | 0x607cf0 | 0x607cf8 → +1, 0x607d04 → +2, 0x607d13 → +3 | +3 | — | +3 | 1 |
| 0x25 | 0x607d90 | 0x607d98 → +1, 0x607da4 → +2, 0x607db3 → +3 | +3 | — | +3 | 1 |
| 0x30 | 0x606310 | 0x606318 → +1 | no error | pop +1 | +1 | 0 |
| 0x31 | 0x606340 | 0x606349 → +1, 0x606355 → +2 | no error | pop +2 | +2 | 0 |
| 0x32 | 0x606390 | 0x606399 → +1 | no error | pop +1; push +1 | +1 | 0 |
| 0x33 | 0x606420 | 0x606429 → +1, 0x606435 → +2 | +2 | push +2 | +2 | 0 |
| 0x34 | 0x6064b0 | 0x6064ba → +1, 0x6064c6 → +2 | +2 | push +2 | +2 | 0 |
| 0x35 | 0x606470 | 0x606479 → +1 | +1 | push +1 | +1 | 0 |
| 0x40 | 0x606600 | 0x606609 → +1, 0x606615 → +2, 0x606624 → +3, 0x606638 → +4, 0x60664c → +5 | no error | push +5 | +5 | 0 |
| 0x41 | 0x6065e0 | none | +0 | — | never returns | 0 |
| 0x42 | 0x606580 | 0x60658a → +1, 0x606596 → +2, 0x6065a5 → +3 | no error | push +3 | +3 | 0 |
| 0x43 | 0x606540 | 0x606549 → +1, 0x606555 → +2 | no error | push +2 | +2 | 0 |
| 0x44 | 0x606680 | 0x606689 → +1, 0x606695 → +2, 0x6066a4 → +3, 0x6066b8 → +4, 0x6066cc → +5 | no error | push +5 | +5 | 0 |
| 0x45 | 0x606510 | 0x606518 → +1 | no error | push +1 | +1 | 0 |
| 0x46 | 0x606700 | 0x60670a → +1, 0x606716 → +2, 0x606725 → +3 | no error | push +3 | +3 | 0 |
| 0x47 | 0x606840 | 0x606848 → +1 | no error | push +1 | +1 | 0 |
| 0x48 | 0x6067c0 | 0x6067c9 → +1, 0x6067d5 → +2, 0x6067e4 → +3, 0x6067f8 → +4, 0x60680c → +5 | no error | push +5 | +5 | 0 |
| 0x49 | 0x606760 | 0x60676a → +1, 0x606776 → +2, 0x606785 → +3 | no error | push +3 | +3 | 0 |
| 0x50 | 0x600ae0 | 0x600aeb → +1, 0x600af7 → +2, 0x600b58 → jump target (mov at 0x600b58) | +2 | pop +2; push +2 | jump target (mov at 0x600b58) | 0 |
| 0x51 | 0x600c30 | 0x600c3b → +1, 0x600c47 → +2, 0x600ca2 → jump target (mov at 0x600ca2) | +2 | pop +2; push +2 | jump target (mov at 0x600ca2) | 0 |
| 0x52 | 0x600cf0 | 0x600dac → jump target (mov at 0x600dac) | no error | pop +0; push +0 | +0 / jump target (mov at 0x600dac) | 0 |
| 0x53 | 0x600df0 | 0x600e55 → +3, 0x600e73 → +1, 0x600e7f → +2, 0x600e8e → +3, 0x600e9f → +3 + s16 operand (add at 0x600e9f) | +0 | pop +0 | +3 / +3 + s16 operand (add at 0x600e9f) | 0 |
| 0x54 | 0x600e70 | 0x600e73 → +1, 0x600e7f → +2, 0x600e8e → +3, 0x600e9f → +3 + s16 operand (add at 0x600e9f) | no error | — | +3 + s16 operand (add at 0x600e9f) | 0 |
| 0x60 | 0x605180 | 0x60518e → +1 | +1 | pop +1; push +1 | +1 | 2 |
| 0x61 | 0x604fd0 | 0x604fdb → +1 | +1 | pop +1; push +1 | +1 | 0 |
| 0x62 | 0x604e30 | 0x604e3b → +1 | +1 | pop +1; push +1 | +1 | 0 |
| 0x63 | 0x604c80 | 0x604c8b → +1 | +1 | pop +1; push +1 | +1 | 0 |
| 0x64 | 0x604ad0 | 0x604adb → +1 | +1 | pop +1; push +1 | +1 | 0 |
| 0x6a | 0x6055b0 | 0x6055ba → +1 | +1 | pop +1; push +1 | +1 | 0 |
| 0x6b | 0x605670 | 0x60567a → +1 | +1 | pop +1; push +1 | +1 | 0 |
| 0x6c | 0x605b40 | 0x605b4a → +1 | +1 | pop +1; push +1 | +1 | 0 |
| 0x6d | 0x605730 | 0x60573a → +1 | +1 | pop +1; push +1 | +1 | 0 |
| 0x6e | 0x6057e0 | 0x6057ea → +1 | +1 | pop +1; push +1 | +1 | 0 |
| 0x6f | 0x605890 | 0x60589a → +1 | +1 | pop +1; push +1 | +1 | 0 |
| 0x70 | 0x603ff0 | 0x603ffe → +1 | +1 | pop +1; push +1 | +1 | 2 |
| 0x71 | 0x6042b0 | 0x6042ba → +1 | +1 | pop +1; push +1 | +1 | 0 |
| 0x72 | 0x604450 | 0x60445a → +1 | +1 | pop +1; push +1 | +1 | 0 |
| 0x73 | 0x6045f0 | 0x6045fa → +1 | +1 | pop +1; push +1 | +1 | 0 |
| 0x74 | 0x6047f0 | 0x6047fa → +1 | +1 | pop +1; push +1 | +1 | 0 |
| 0x75 | 0x604a00 | 0x604a0a → +1 | +1 | pop +1; push +1 | +1 | 0 |
| 0x76 | 0x605aa0 | 0x605aaa → +1 | +1 | pop +1; push +1 | +1 | 0 |
| 0x77 | 0x605940 | 0x60594a → +1 | +1 | pop +1; push +1 | +1 | 0 |
| 0x78 | 0x6059f0 | 0x6059fa → +1 | +1 | pop +1; push +1 | +1 | 0 |
| 0x7a | 0x605be0 | 0x605be2 → +1 | no error | — | +1 | 0 |
| 0x7b | 0x605c00 | 0x605c02 → +1 | no error | — | +1 | 0 |
| 0x80 | 0x6020d0 | 0x6020da → +1, 0x6020e6 → +2, 0x6020f5 → +3, 0x60211c → +4, 0x60212b → +5 | no error | — | +5 | 2 |
| 0x81 | 0x602040 | 0x60204a → +1, 0x602056 → +2, 0x602065 → +3, 0x60208c → +4, 0x60209b → +5 | no error | — | +5 | 2 |
| 0x82 | 0x6015a0 | 0x6015c6 → +1, 0x6015d8 → +2, 0x6015f3 → +3, 0x601611 → +4, 0x60162c → +5, 0x601780 → +6 | +5 / +6 | pop +6; push +6 | +6 | 9 |
| 0x83 | 0x602160 | 0x602178 → +1, 0x60218a → +2, 0x6021a5 → +3, 0x60237c → +4 | +3 / +4 | pop +3 / +4; push +4 | +4 | 9 |
| 0x84 | 0x602bb0 | 0x602bc8 → +1, 0x602bda → +2, 0x602bf5 → +3, 0x602c13 → +4, 0x602c31 → +5, 0x602c83 → +6 | +6 | pop +6; push +6 | +6 | 6 |
| 0x90 | 0x5fd070 | 0x5fd07a → +1 | no error | push +1 | +1 | 1 |
| 0x91 | 0x5fd0f0 | 0x5fd0ff → +1 | +1 | pop +1; push +1 | +1 | 1 |
| 0x92 | 0x5fd2d0 | 0x5fd2db → +1 | +1 | pop +1; push +1 | +1 | 0 |
| 0x93 | 0x5fd360 | 0x5fd36b → +1 | +1 | pop +1 | +1 | 0 |
| 0x94 | 0x5fd450 | 0x5fd45a → +1 | +1 | pop +1 | +1 | 1 |
| 0x95 | 0x5fd500 | 0x5fd50b → +1 | +1 | pop +1 | +1 | 0 |
| 0x96 | 0x5fd5d0 | 0x5fd5db → +1 | +1 | pop +1 | +1 | 1 |
| 0xa0 | 0x5fff30 | 0x5fff3a → +1 | +1 | pop +1; push +1 | +1 | 0 |
| 0xa1 | 0x5ffff0 | 0x5ffffa → +1 | +1 | pop +1; push +1 | +1 | 0 |
| 0xa2 | 0x6000b0 | 0x6000ba → +1 | +1 | pop +1; push +1 | +1 | 0 |
| 0xa3 | 0x600170 | 0x60017a → +1 | +1 | pop +1; push +1 | +1 | 0 |
| 0xa4 | 0x600230 | 0x60023a → +1 | +1 | pop +1; push +1 | +1 | 0 |
| 0xa5 | 0x6002a0 | 0x6002aa → +1 | +1 | pop +1; push +1 | +1 | 0 |
| 0xa6 | 0x600310 | 0x60031a → +1 | +1 | pop +1; push +1 | +1 | 0 |
| 0xa7 | 0x600380 | 0x60038a → +1 | +1 | pop +1; push +1 | +1 | 0 |
| 0xb0 | 0x606f60 | 0x606f69 → +1 | +1 | pop +1; push +1 | +1 | 5 |
| 0xb1 | 0x606fe0 | 0x606fe9 → +1 | +1 | pop +1; push +1 | +1 | 5 |
| 0xb2 | 0x607060 | 0x607069 → +1 | +1 | pop +1; push +1 | +1 | 5 |
| 0xb8 | 0x6070e0 | 0x6070e9 → +1 | no error | pop +1; push +1 | +1 | 0 |
| 0xb9 | 0x607140 | 0x607149 → +1 | no error | pop +1; push +1 | +1 | 0 |
| 0xba | 0x6071a0 | 0x6071a9 → +1 | no error | pop +1; push +1 | +1 | 0 |
| 0xbb | 0x607220 | 0x607229 → +1 | no error | pop +1; push +1 | +1 | 0 |
| 0xbc | 0x607280 | 0x607289 → +1 | no error | pop +1; push +1 | +1 | 0 |
| 0xbd | 0x6072f0 | 0x6072f9 → +1 | no error | pop +1; push +1 | +1 | 0 |
| 0xbf | 0x607350 | 0x60735a → +1 | +1 | pop +1; push +1 | +1 | 0 |
| 0xff | 0x603840 | 0x603848 → +1 | no error | push +1 | +1 | 0 |

| opcode | handler addr | error site addr | message | PC at throw |
|---|---|---|---|---|
| 160 unused ops (0x01, 0x07, …, 0xfe) | 0x60be60 | 0x60be6e | "Call of undefined instruction" (Error) | +0 |
| 0x02 | 0x6038a0 | 0x6038b4 | "Debugger not enabled! but encountered DebugOn" (Error) | +1 |
| 0x03 | 0x6038d0 | 0x6038e4 | "Debugger not enabled! but encountered DebugOff" (Error) | +1 |
| 0x10 | 0x5fe610 | 0x5fe778 | "Reference to unregistered Game Variable" (SrOSIError) | +3 |
| 0x10 | 0x5fe610 | 0x5fe793 | registered game-variable accessor (dead: the RegisteredVariable map is never populated), +0xc | +3; dead code ("The game-variable map") |
| 0x10 | 0x5fe610 | 0x5fe7bd | registered game-variable accessor (dead: the RegisteredVariable map is never populated), +0x10 | +3; dead code ("The game-variable map") |
| 0x10 | 0x5fe610 | 0x5fe7eb | "Member property of non member variable" (Error) | +3 |
| 0x10 | 0x5fe610 | 0x5fe845 | "Reference to non-existant member property "%s"" (Error) | +3 |
| 0x11 | 0x5fe9a0 | 0x5feb01 | "Reference to unregistered Game Variable" (SrOSIError) | +3 |
| 0x11 | 0x5fe9a0 | 0x5feb1c | registered game-variable accessor (dead: the RegisteredVariable map is never populated), +0xc | +3; dead code ("The game-variable map") |
| 0x11 | 0x5fe9a0 | 0x5feb29 (ScOSIVariant::Duplicate) | kOutOfRange (non-OSI ScError): SrThrow at 0x46dee0 | +3; INFERRED reachable only with a string the 255-byte buffer cannot hold |
| 0x11 | 0x5fe9a0 | 0x5feb62 (ScOSIVariant::Duplicate) | kOutOfRange (non-OSI ScError): SrThrow at 0x46dee0 | +3; INFERRED reachable only with a string the 255-byte buffer cannot hold |
| 0x12 | 0x5fe590 | 0x5fe5e1 | ScOSIObject::FindMethod → "Call to undefined member method" (SrOSIError at 0x60867c) | +3 |
| 0x14 | 0x5feb90 | 0x5febd9 | "Too many arguments to method" (Error) | +2 |
| 0x15 | 0x5fec10 | 0x5fec6d | "Member property of non member variable" (Error) | +3 |
| 0x15 | 0x5fec10 | 0x5fecc1 | "Reference to non-existant member property "%s"" (Error) | +3 |
| 0x16 | 0x5fecf0 | 0x5fed4d | "Member property of non member variable" (Error) | +3 |
| 0x16 | 0x5fecf0 | 0x5fed83 | "Reference to non-existant member property "%s"" (Error) | +3 |
| 0x16 | 0x5fecf0 | 0x5fed8f (ScOSIVariant::Duplicate) | kOutOfRange (non-OSI ScError): SrThrow at 0x46dee0 | +3; INFERRED reachable only with a string the 255-byte buffer cannot hold |
| 0x17 | 0x5fedb0 | 0x5fedff | "Member method of non member variable" (Error) | +3 |
| 0x17 | 0x5fedb0 | 0x5fee0c | ScOSIObject::FindMethod → "Call to undefined member method" (SrOSIError at 0x60867c) | +3 |
| 0x18 | 0x5fee40 | 0x5fef3b (ScFixedString::operator=) | kOutOfRange (non-OSI ScError): SrThrow at 0x4f5c38 | +1; INFERRED reachable only with a string the 255-byte buffer cannot hold |
| 0x18 | 0x5fee40 | 0x5fef69 (ScFixedString::operator=) | kOutOfRange (non-OSI ScError): SrThrow at 0x4f5b27 | +1; INFERRED reachable only with a string the 255-byte buffer cannot hold |
| 0x18 | 0x5fee40 | 0x5fef93 | "Invalid type for member access (must be a string)" (Error) | +1 |
| 0x18 | 0x5fee40 | 0x5ff0e1 | "Reference to unregistered Game Variable" (SrOSIError) | +1 |
| 0x18 | 0x5fee40 | 0x5ff105 | registered game-variable accessor (dead: the RegisteredVariable map is never populated), +0xc | +1; dead code ("The game-variable map") |
| 0x18 | 0x5fee40 | 0x5ff115 (ScOSIVariant::Duplicate) | kOutOfRange (non-OSI ScError): SrThrow at 0x46dee0 | +1; INFERRED reachable only with a string the 255-byte buffer cannot hold |
| 0x18 | 0x5fee40 | 0x5ff171 | "Member property of non member variable" (Error) | +1 |
| 0x18 | 0x5fee40 | 0x5ff1b9 | "Reference to non-existant member property "%s"" (Error) | +1 |
| 0x18 | 0x5fee40 | 0x5ff1c5 (ScOSIVariant::Duplicate) | kOutOfRange (non-OSI ScError): SrThrow at 0x46dee0 | +1; INFERRED reachable only with a string the 255-byte buffer cannot hold |
| 0x19 | 0x5ff210 | 0x5ff2c2 | "Member method of non member variable" (Error) | +1 |
| 0x19 | 0x5ff210 | 0x5ff2e7 (ScFixedString::operator=) | kOutOfRange (non-OSI ScError): SrThrow at 0x4f5c38 | +1; INFERRED reachable only with a string the 255-byte buffer cannot hold |
| 0x19 | 0x5ff210 | 0x5ff313 (ScFixedString::operator=) | kOutOfRange (non-OSI ScError): SrThrow at 0x4f5b27 | +1; INFERRED reachable only with a string the 255-byte buffer cannot hold |
| 0x19 | 0x5ff210 | 0x5ff338 | "Invalid method type for function call (must be a string)" (Error) | +1 |
| 0x19 | 0x5ff210 | 0x5ff350 | ScOSIObject::FindMethod → "Call to undefined member method" (SrOSIError at 0x60867c) | +1 |
| 0x1a | 0x5ff3a0 | 0x5ff49b (ScFixedString::operator=) | kOutOfRange (non-OSI ScError): SrThrow at 0x4f5c38 | +1; INFERRED reachable only with a string the 255-byte buffer cannot hold |
| 0x1a | 0x5ff3a0 | 0x5ff4c9 (ScFixedString::operator=) | kOutOfRange (non-OSI ScError): SrThrow at 0x4f5b27 | +1; INFERRED reachable only with a string the 255-byte buffer cannot hold |
| 0x1a | 0x5ff3a0 | 0x5ff4f3 | "Invalid type for member set (must be a string)" (Error) | +1 |
| 0x1a | 0x5ff3a0 | 0x5ff641 | "Reference to unregistered Game Variable" (SrOSIError) | +1 |
| 0x1a | 0x5ff3a0 | 0x5ff665 | registered game-variable accessor (dead: the RegisteredVariable map is never populated), +0xc | +1; dead code ("The game-variable map") |
| 0x1a | 0x5ff3a0 | 0x5ff69b | registered game-variable accessor (dead: the RegisteredVariable map is never populated), +0x10 | +1; dead code ("The game-variable map") |
| 0x1a | 0x5ff3a0 | 0x5ff6e2 | "Member property of non object variable" (Error) | +1 |
| 0x1a | 0x5ff3a0 | 0x5ff751 | "Reference to non-existant member property "%s"" (Error) | +1 |
| 0x21 | 0x607c50 | 0x607cd3 (ScOSIVariant::Duplicate) | kOutOfRange (non-OSI ScError): SrThrow at 0x46dee0 | +3; INFERRED reachable only with a string the 255-byte buffer cannot hold |
| 0x24 | 0x607cf0 | 0x607d4b | "increment of non-integral type" (Error) | +3 |
| 0x24 | 0x607cf0 | 0x607d6f | "increment of non-integral type" (Error) | +3 |
| 0x25 | 0x607d90 | 0x607deb | "increment of non-integral type" (Error) | +3 |
| 0x25 | 0x607d90 | 0x607e0f | "decrement of non-integral type" (Error) | +3 |
| 0x33 | 0x606420 | 0x606452 (ScOSIVariant::Duplicate) | kOutOfRange (non-OSI ScError): SrThrow at 0x46dee0 | +2; INFERRED reachable only with a string the 255-byte buffer cannot hold |
| 0x34 | 0x6064b0 | 0x6064eb (ScOSIVariant::Duplicate) | kOutOfRange (non-OSI ScError): SrThrow at 0x46dee0 | +2; INFERRED reachable only with a string the 255-byte buffer cannot hold |
| 0x35 | 0x606470 | 0x60648c (ScOSIVariant::Duplicate) | kOutOfRange (non-OSI ScError): SrThrow at 0x46dee0 | +1; INFERRED reachable only with a string the 255-byte buffer cannot hold |
| 0x41 | 0x6065e0 | 0x6065ee | "unwritten instruction BCL_PushConstanti24 called" (Error) | +0 |
| 0x50 | 0x600ae0 | 0x600b2b | "Internal error: Jump to non-integral address" (Error) | +2 |
| 0x51 | 0x600c30 | 0x600c7b | "Internal error: Jump to non-integral address" (Error) | +2 |
| 0x53 | 0x600df0 | 0x600e40 | "Results of comparison expression is not integral" (Error) | +0 |
| 0x60 | 0x605180 | 0x605552 | "Invalid operand to operator '=' (lhs) (type:%02.2X)" (Error) | +1 |
| 0x60 | 0x605180 | 0x605572 | "Invalid operand to operator '=' (rhs) (type:%02.2X)" (Error) | +1 |
| 0x60 | 0x605180 | call at 0x605369 (ConvertStringIDToDynamicString); inner calls 0x60b8b7 | kOutOfRange (non-OSI ScError): SrThrow at 0x60b970 | +1; INFERRED reachable only with a string the 255-byte buffer cannot hold |
| 0x60 | 0x605180 | call at 0x605397 (ConvertStringIDToDynamicString); inner calls 0x60b8b7 | kOutOfRange (non-OSI ScError): SrThrow at 0x60b970 | +1; INFERRED reachable only with a string the 255-byte buffer cannot hold |
| 0x61 | 0x604fd0 | 0x605128 | "Invalid operand to operator '<' (lhs) (type:%02.2X)" (Error) | +1 |
| 0x61 | 0x604fd0 | 0x605145 | "Invalid operand to operator '<' (rhs) (type:%02.2X)" (Error) | +1 |
| 0x62 | 0x604e30 | 0x604f7f | "Invalid operand to operator '>' (lhs) (type:%02.2X)" (Error) | +1 |
| 0x62 | 0x604e30 | 0x604f9c | "Invalid operand to operator '>' (rhs) (type:%02.2X)" (Error) | +1 |
| 0x63 | 0x604c80 | 0x604ddd | "Invalid operand to operator '<=' (lhs) (type:%02.2X)" (Error) | +1 |
| 0x63 | 0x604c80 | 0x604dfa | "Invalid operand to operator '<=' (rhs) (type:%02.2X)" (Error) | +1 |
| 0x64 | 0x604ad0 | 0x604c27 | "Invalid operand to operator '>=' (lhs) (type:%02.2X)" (Error) | +1 |
| 0x64 | 0x604ad0 | 0x604c44 | "Invalid operand to operator '>=' (rhs) (type:%02.2X)" (Error) | +1 |
| 0x6a | 0x6055b0 | 0x60560d | "Invalid operand to operator 'and' (lhs) (type:%02.2X)" (Error) | +1 |
| 0x6a | 0x6055b0 | 0x60562a | "Invalid operand to operator 'and' (rhs) (type:%02.2X)" (Error) | +1 |
| 0x6b | 0x605670 | 0x6056cd | "Invalid operand to operator 'or' (lhs) (type:%02.2X)" (Error) | +1 |
| 0x6b | 0x605670 | 0x6056ea | "Invalid operand to operator 'or' (rhs) (type:%02.2X)" (Error) | +1 |
| 0x6c | 0x605b40 | 0x605baf | "Invalid operand to operator 'not' (type:%02.2X)" (Error) | +1 |
| 0x6d | 0x605730 | 0x60578d | "Invalid operand to operator '&' (lhs) (type:%02.2X)" (Error) | +1 |
| 0x6d | 0x605730 | 0x6057aa | "Invalid operand to operator '&' (rhs) (type:%02.2X)" (Error) | +1 |
| 0x6e | 0x6057e0 | 0x60583d | "Invalid operand to operator '\|' (lhs) (type:%02.2X)" (Error) | +1 |
| 0x6e | 0x6057e0 | 0x60585a | "Invalid operand to operator '\|' (rhs) (type:%02.2X)" (Error) | +1 |
| 0x6f | 0x605890 | 0x6058ed | "Invalid operand to operator '@' (lhs) (type:%02.2X)" (Error) | +1 |
| 0x6f | 0x605890 | 0x60590a | "Invalid operand to operator '@' (rhs) (type:%02.2X)" (Error) | +1 |
| 0x70 | 0x603ff0 | 0x6040bf | ScFixedString::append → kLenTooLong (non-OSI ScError, SrThrow at 0x4f5ca9) | +1 |
| 0x70 | 0x603ff0 | 0x604114 | "Invalid operand to string operator '+' (rhs) (type:%02.2X)" (Error) | +1 |
| 0x70 | 0x603ff0 | 0x60413b | "Invalid operand to string operator '+' (lhs) (type:%02.2X)" (Error) | +1 |
| 0x70 | 0x603ff0 | 0x604257 | "Invalid operand to operator '+' (lhs) (type:%02.2X)" (Error) | +1 |
| 0x70 | 0x603ff0 | 0x604277 | "Invalid operand to operator '+' (rhs) (type:%02.2X)" (Error) | +1 |
| 0x70 | 0x603ff0 | call at 0x604053 (ConvertStringIDToDynamicString); inner calls 0x60b8b7 | kOutOfRange (non-OSI ScError): SrThrow at 0x60b970 | +1; INFERRED reachable only with a string the 255-byte buffer cannot hold |
| 0x70 | 0x603ff0 | call at 0x60407e (ConvertStringIDToDynamicString); inner calls 0x60b8b7 | kOutOfRange (non-OSI ScError): SrThrow at 0x60b970 | +1; INFERRED reachable only with a string the 255-byte buffer cannot hold |
| 0x71 | 0x6042b0 | 0x6043f7 | "Invalid operand to operator '-' (lhs) (type:%02.2X)" (Error) | +1 |
| 0x71 | 0x6042b0 | 0x604414 | "Invalid operand to operator '-' (rhs) (type:%02.2X)" (Error) | +1 |
| 0x72 | 0x604450 | 0x604597 | "Invalid left-hand-side operand to operator '*' (type:%02.2X)" (Error) | +1 |
| 0x72 | 0x604450 | 0x6045b4 | "Invalid operand to operator '*' (rhs) (type:%02.2X)" (Error) | +1 |
| 0x73 | 0x6045f0 | 0x604668 | "Divide by 0" (Error) | +1 |
| 0x73 | 0x6045f0 | 0x6046d7 | "Divide by 0" (Error) | +1 |
| 0x73 | 0x6045f0 | 0x604733 | "Divide by 0" (Error) | +1 |
| 0x73 | 0x6045f0 | 0x604787 | "Invalid operand to operator '/' (lhs) (type:%02.2X)" (Error) | +1 |
| 0x73 | 0x6045f0 | 0x6047a4 | "Invalid operand to operator '/' (rhs) (type:%02.2X)" (Error) | +1 |
| 0x73 | 0x6045f0 | 0x6047b8 | "Divide by 0" (Error) | +1 |
| 0x74 | 0x6047f0 | 0x60499a | "Invalid operand to operator '^' (lhs) (type:%02.2X)" (Error) | +1 |
| 0x74 | 0x6047f0 | 0x6049b7 | "Invalid operand to operator '^' (rhs) (type:%02.2X)" (Error) | +1 |
| 0x75 | 0x604a00 | 0x604a5d | "Invalid operand to operator '%' (lhs) (type:%02.2X)" (Error) | +1 |
| 0x75 | 0x604a00 | 0x604a7a | "Invalid operand to operator '%' (rhs) (type:%02.2X)" (Error) | +1 |
| 0x75 | 0x604a00 | 0x604a8e | "Divide by 0" (Error) | +1 |
| 0x76 | 0x605aa0 | 0x605b0f | "Invalid operand to operator '~' (type:%02.2X)" (Error) | +1 |
| 0x77 | 0x605940 | 0x60599d | "Invalid operand to operator '<<' (lhs) (type:%02.2X)" (Error) | +1 |
| 0x77 | 0x605940 | 0x6059ba | "Invalid operand to operator '<<' (rhs) (type:%02.2X)" (Error) | +1 |
| 0x78 | 0x6059f0 | 0x605a4d | "Invalid operand to operator '>>' (lhs) (type:%02.2X)" (Error) | +1 |
| 0x78 | 0x6059f0 | 0x605a6a | "Invalid operand to operator '>>' (rhs) (type:%02.2X)" (Error) | +1 |
| 0x82 | 0x6015a0 | 0x601725 | "Call to unregistered Game Function" (SrOSIError) | +5 |
| 0x82 | 0x6015a0 | 0x6017ac | "Too many parameters passed to game function" (Error); caught (pad 0x601c94), then raised again at 0x601dd3 with this PC | +6 |
| 0x82 | 0x6015a0 | 0x6017d5 | "Too few parameters passed to game function" (Error); caught (pad 0x601c94), then raised again at 0x601dd3 with this PC | +6 |
| 0x82 | 0x6015a0 | 0x601af5 | the native (RegisteredFunction +0x8); caught (pad 0x601b10), then raised again at 0x601be5 → 0x601dd3 with this PC | +6; INFERRED: when the native's nested engine call raised, that inner error's PC (D16) |
| 0x82 | 0x6015a0 | 0x601be5 | re-throw of the native's error from the inner catch (pad 0x601b10); caught (pad 0x601c94), then raised again at 0x601dd3 with this PC | +6 |
| 0x82 | 0x6015a0 | 0x601dd3 | "Script is calling %s.%s" (Error) | +6: the PC of the caught error; after a native whose nested engine call raised, that inner error's PC (D16; INFERRED) |
| 0x82 | 0x6015a0 | call at 0x6018db (ConvertVariant); inner calls 0x60b8b7, 0x60ba4c, 0x60bb0a, 0x60bb78 | kOutOfRange (non-OSI ScError): SrThrow at 0x46dee0, 0x60b970; caught (pad 0x601c94), then raised again at 0x601dd3 with this PC | +6; INFERRED reachable only with a string the 255-byte buffer cannot hold |
| 0x82 | 0x6015a0 | 0x60bdc3 (in an inlined VM method, called at 0x6018db) | "Requested variant conversion unimplimented.  Recompile to debug for types" (Error); caught (pad 0x601c94), then raised again at 0x601dd3 with this PC | +6 |
| 0x83 | 0x602160 | 0x60226a (ScFixedString::operator=) | kOutOfRange (non-OSI ScError): SrThrow at 0x4f5c38 | +3; INFERRED reachable only with a string the 255-byte buffer cannot hold |
| 0x83 | 0x602160 | 0x602299 (ScFixedString::operator=) | kOutOfRange (non-OSI ScError): SrThrow at 0x4f5b27 | +3; INFERRED reachable only with a string the 255-byte buffer cannot hold |
| 0x83 | 0x602160 | 0x6022c3 | "Invalid member type for function call (must be a string)" (Error) | +3 |
| 0x83 | 0x602160 | 0x602431 | "Call to unregistered Game Function" (SrOSIError); caught (pad 0x6029f6), then raised again at 0x602b24 with this PC | +4 |
| 0x83 | 0x602160 | 0x60246d | "Too many parameters passed to game function" (Error); caught (pad 0x6029f6), then raised again at 0x602b24 with this PC | +4 |
| 0x83 | 0x602160 | 0x602496 | "Too few parameters passed to game function" (Error); caught (pad 0x6029f6), then raised again at 0x602b24 with this PC | +4 |
| 0x83 | 0x602160 | 0x602738 | "Argument type mismatch in call to game function (argument %d [zero based])" (Error); caught (pad 0x6029f6), then raised again at 0x602b24 with this PC | +4 |
| 0x83 | 0x602160 | 0x602938 | the native (RegisteredFunction +0x8); caught (pad 0x6029f6), then raised again at 0x602b24 with this PC | +4; INFERRED: when the native's nested engine call raised, that inner error's PC (D16) |
| 0x83 | 0x602160 | 0x602b24 | "Script is calling %s.%s" (Error) | +4: the PC of the caught error (+3 only after an allocation failure); after a native whose nested engine call raised, that inner error's PC (D16; INFERRED) |
| 0x83 | 0x602160 | call at 0x6025c3 (ConvertVariant); inner calls 0x60b8b7, 0x60ba4c, 0x60bb0a, 0x60bb78 | kOutOfRange (non-OSI ScError): SrThrow at 0x46dee0, 0x60b970; caught (pad 0x602620), then raised again at 0x602738 → 0x602b24 with this PC | +4; INFERRED reachable only with a string the 255-byte buffer cannot hold |
| 0x83 | 0x602160 | 0x60bdc3 (in an inlined VM method, called at 0x6025c3) | "Requested variant conversion unimplimented.  Recompile to debug for types" (Error); caught (pad 0x602620), then raised again at 0x602738 → 0x602b24 with this PC | +4 |
| 0x84 | 0x602bb0 | 0x602efd | the native (RegisteredFunction +0x8, from the self-patched pointer); caught (pad 0x602fa0), then raised again at 0x60303c with this PC | +6; INFERRED: when the native's nested engine call raised, that inner error's PC (D16) |
| 0x84 | 0x602bb0 | 0x60303c | "Script is calling a game function directly." (Error) | +6: the PC of the caught error; after a native whose nested engine call raised, that inner error's PC (D16; INFERRED) |
| 0x84 | 0x602bb0 | call at 0x602d76 (ConvertVariant); inner calls 0x60b8b7, 0x60ba4c, 0x60bb0a, 0x60bb78 | kOutOfRange (non-OSI ScError): SrThrow at 0x46dee0, 0x60b970; caught (pad 0x602fa0), then raised again at 0x60303c with this PC | +6; INFERRED reachable only with a string the 255-byte buffer cannot hold |
| 0x84 | 0x602bb0 | 0x60bdc3 (in an inlined VM method, called at 0x602d76) | "Requested variant conversion unimplimented.  Recompile to debug for types" (Error); caught (pad 0x602fa0), then raised again at 0x60303c with this PC | +6 |
| 0x91 | 0x5fd0f0 | 0x5fd164 | "Reference beyond end of string table" (Error) | +1 |
| 0x91 | 0x5fd0f0 | 0x5fd1ba (ScFixedString::Init) | kOutOfRange (non-OSI ScError): SrThrow at 0x4018a8 | +1; INFERRED reachable only with a string the 255-byte buffer cannot hold |
| 0x91 | 0x5fd0f0 | 0x5fd203 | "Reference beyond start/end of array" (Error) | +1 |
| 0x91 | 0x5fd0f0 | 0x5fd215 (ScOSIVariant::Duplicate) | kOutOfRange (non-OSI ScError): SrThrow at 0x46dee0 | +1; INFERRED reachable only with a string the 255-byte buffer cannot hold |
| 0x91 | 0x5fd0f0 | 0x5fd23d | "array reference on non-array" (Error) | +1 |
| 0x92 | 0x5fd2d0 | 0x5fd32b | "array reference on non-array" (Error) | +1 |
| 0x93 | 0x5fd360 | 0x5fd3d3 | "Assigning into string table is illegal" (Error) | +1 |
| 0x93 | 0x5fd360 | 0x5fd3fc | "Access beyond end of array" (Error) | +1 |
| 0x93 | 0x5fd360 | 0x5fd42e | "array reference on non-array" (Error) | +1 |
| 0x94 | 0x5fd450 | 0x5fd4a7 | "Assigning into string table is illegal" (Error) | +1 |
| 0x94 | 0x5fd450 | 0x5fd4e9 | "array reference on non-array" (Error) | +1 |
| 0x95 | 0x5fd500 | 0x5fd558 | "Removal from string table is illegal" (Error) | +1 |
| 0x95 | 0x5fd500 | 0x5fd57c | "removal outside of array bounds" (Error) | +1 |
| 0x95 | 0x5fd500 | 0x5fd5af | "array reference on non-array" (Error) | +1 |
| 0x96 | 0x5fd5d0 | 0x5fd643 | "Assigning into string table is illegal" (Error) | +1 |
| 0x96 | 0x5fd5d0 | 0x5fd675 | "insertion outside of array bounds" (SrOSIError) | +1 |
| 0x96 | 0x5fd5d0 | 0x5fd6c0 | "array reference on non-array" (Error) | +1 |
| 0xa0 | 0x5fff30 | 0x5fff87 | "cant access red value of anything but a color" (Error) | +1 |
| 0xa0 | 0x5fff30 | 0x5fff9d | "cant set red to a non-integral value" (Error) | +1 |
| 0xa1 | 0x5ffff0 | 0x600047 | "cant access green value of anything but a color" (Error) | +1 |
| 0xa1 | 0x5ffff0 | 0x60005d | "cant set green to a non-integral value" (Error) | +1 |
| 0xa2 | 0x6000b0 | 0x600107 | "cant access blue value of anything but a color" (Error) | +1 |
| 0xa2 | 0x6000b0 | 0x60011d | "cant set blue to a non-integral value" (Error) | +1 |
| 0xa3 | 0x600170 | 0x6001c7 | "cant access alpha value of anything but a color" (Error) | +1 |
| 0xa3 | 0x600170 | 0x6001dd | "cant set alpha to a non-integral value" (Error) | +1 |
| 0xa4 | 0x600230 | 0x60026c | "cant access red value of anything but a color" (Error) | +1 |
| 0xa5 | 0x6002a0 | 0x6002dc | "cant access green value of anything but a color" (Error) | +1 |
| 0xa6 | 0x600310 | 0x60034c | "cant access blue value of anything but a color" (Error) | +1 |
| 0xa7 | 0x600380 | 0x6003bc | "cant access alpha value of anything but a color" (Error) | +1 |
| 0xb0 | 0x606f60 | call at 0x606f96 (ConvertVariant); inner calls 0x60b8b7, 0x60ba4c, 0x60bb0a, 0x60bb78 | kOutOfRange (non-OSI ScError): SrThrow at 0x46dee0, 0x60b970 | +1; INFERRED reachable only with a string the 255-byte buffer cannot hold |
| 0xb0 | 0x606f60 | 0x60bdc3 (in an inlined VM method, called at 0x606f96) | "Requested variant conversion unimplimented.  Recompile to debug for types" (Error) | +1 |
| 0xb1 | 0x606fe0 | call at 0x607016 (ConvertVariant); inner calls 0x60b8b7, 0x60ba4c, 0x60bb0a, 0x60bb78 | kOutOfRange (non-OSI ScError): SrThrow at 0x46dee0, 0x60b970 | +1; INFERRED reachable only with a string the 255-byte buffer cannot hold |
| 0xb1 | 0x606fe0 | 0x60bdc3 (in an inlined VM method, called at 0x607016) | "Requested variant conversion unimplimented.  Recompile to debug for types" (Error) | +1 |
| 0xb2 | 0x607060 | call at 0x607096 (ConvertVariant); inner calls 0x60b8b7, 0x60ba4c, 0x60bb0a, 0x60bb78 | kOutOfRange (non-OSI ScError): SrThrow at 0x46dee0, 0x60b970 | +1; INFERRED reachable only with a string the 255-byte buffer cannot hold |
| 0xb2 | 0x607060 | 0x60bdc3 (in an inlined VM method, called at 0x607096) | "Requested variant conversion unimplimented.  Recompile to debug for types" (Error) | +1 |
| 0xbf | 0x607350 | 0x6073fb | "Cant get classID of non object" (Error) | +1 |

<!-- END error-pc -->

## Trace format (M2a-1)

The differential runs (native, wasm32, Python) and the conformance runners write the fixed-format text trace specified
in [`docs/osi/trace.md`](trace.md): one record per instruction, native call, allocation, free, precision switch (`P`,
D8′) and error, with every float printed as its 8 hex f32 bits. Traces are compared byte for byte. The conformance
corpus format and its authoring rules are in [`docs/osi/conformance.md`](conformance.md).

## Table

Regenerate the generated columns with `make osi-tables`. `--check` exits 1 if the file is stale.

<!-- BEGIN opcode-table (generated columns: tools/osi/opcodes.py; authored columns kept by op) -->
| op | handler | handler_addr | template_entry | vtable_slot | operands | base_osi_count | stack_effect | errors | notes |
|---|---|---|---|---|---|---|---|---|---|
| 0x00 | BCLNop | 0x603830 | 0x751768 | 6 (+0x18) | none | 0 | — (no change); PC+=1 [0x603830] | none [0x603830] |  |
| 0x01 | BCLUnused | 0x60be60 | 0x751778 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x02 | BCLDebugOn | 0x6038a0 | 0x751788 | 8 (+0x20) | none | 0 | — ; PC+=1, then always errors [0x6038a0] | "Debugger not enabled! but encountered DebugOn" (always) [0x6038a0] | Always throws (Error never returns, see Conventions); the trailing `return true` is unreachable. |
| 0x03 | BCLDebugOff | 0x6038d0 | 0x751798 | 9 (+0x24) | none | 0 | — ; PC+=1, then always errors [0x6038d0] | "Debugger not enabled! but encountered DebugOff" (always) [0x6038d0] | Always throws, like 0x02. |
| 0x04 | BCLLineNumber | 0x603870 | 0x7517a8 | 10 (+0x28) | HH | 0 | — ; PC+=5 (the HH operands are skipped, never read) [0x603870] | none [0x603870] | Line-number marker; a runtime no-op. |
| 0x05 | BCLLineNumberAlt1 | 0x603880 | 0x7517b8 | 11 (+0x2c) | HH | 0 | — ; PC+=5 (the HH operands are skipped, never read) [0x603880] | none [0x603880] | Line-number marker; a runtime no-op. |
| 0x06 | BCLLineNumberAlt2 | 0x603890 | 0x7517c8 | 12 (+0x30) | HH | 0 | — ; PC+=5 (the HH operands are skipped, never read) [0x603890] | none [0x603890] | Line-number marker; a runtime no-op. |
| 0x07 | BCLUnused | 0x60be60 | 0x7517d8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x08 | BCLUnused | 0x60be60 | 0x7517e8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x09 | BCLUnused | 0x60be60 | 0x7517f8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x0a | BCLUnused | 0x60be60 | 0x751808 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x0b | BCLUnused | 0x60be60 | 0x751818 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x0c | BCLUnused | 0x60be60 | 0x751828 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x0d | BCLUnused | 0x60be60 | 0x751838 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x0e | BCLUnused | 0x60be60 | 0x751848 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x0f | BCLUnused | 0x60be60 | 0x751858 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x10 | BCLSetMemberValue | 0x5fe610 | 0x751868 | 15 (+0x3c) | H | 72 | v o → (empty); operand u16 m (member-symbol index). o is popped first (top, no dispose). Game type o (0x20..0xfe): RegisteredVariable = map[(GetTypeToken(o.type), GetMemberTableToken(m))]; its getter (+0xc)(vm, o.value) is called and the result disposed, THEN v is popped and the setter (+0x10)(vm, o.value, v) called; o disposed. Script object o (type 1): v popped, prop = GetProperty(o, m) (0x6086a0, linear search of the class property symbols), old prop disposed, prop ← v (moved); o disposed [0x5fe610] | "Reference to unregistered Game Variable" (SrOSIError, game type, map miss; always-error, map never populated: nothing inserts into ScOSISystem+0x14, see "The game-variable map"); "Member property of non member variable" (o neither type 1 nor 0x20..0xfe); "Reference to non-existant member property \"%s\"" (type 1, property absent; %s = member-symbol name) [0x5fe610] Order (asm): o popped first (0x5fe664); the unregistered-game-variable error (0x5fe778) and 'non member variable' (0x5fe7eb) are raised BEFORE v is popped: depth −1, v on top. Type 1: v popped (0x5fe7fc) before GetProperty (0x5fe818), so the absent-property error (0x5fe845) leaves depth −2. Nothing disposed (o, v leak). (two independent asm readers agree, 2026-10-08: oracle second-reader + corpus-A review) | The game path calls the getter before popping v (a native-visible side effect; the ICALL order in the asm is getter, Dispose, Pop, setter). That path is dead in this exe: the map lookup before it always misses ("The game-variable map"). |
| 0x11 | BCLGetMemberValue | 0x5fe9a0 | 0x751878 | 16 (+0x40) | H | 5905 | o → value; operand u16 m. o popped (no dispose). Game type: push Duplicate(getter(vm, o.value)) via the RegisteredVariable map. Any other type: push Duplicate(*GetProperty((ScOSIObject*)o.value, m)); o disposed [0x5fe9a0] | "Reference to unregistered Game Variable" (SrOSIError, game type, map miss; always-error, map never populated: nothing inserts into ScOSISystem+0x14, see "The game-variable map") [0x5fe9a0] | No type check and no null check on the non-game path: a non-object o or an absent property dereferences a bad pointer in the original (undefined behaviour; conformance tests must not exercise it). Contrast 0x16, which checks. |
| 0x12 | BCLGetMemberFunction | 0x5fe590 | 0x751888 | 14 (+0x38) | H | 8422 | o → int(code address); operand u16 m. o popped (no dispose), FindMethod(o, m) (0x608600) scans the class method records {u16 symbol, u32 offset}; pushes {0x10, script-file base + offset}; o disposed [0x5fe590] | "Call to undefined member method" (SrOSIError inside FindMethod 0x608600, no method with that symbol) [0x5fe590] | No type check on o (a non-object is undefined behaviour). The pushed int is a HOST ADDRESS consumed by 0x51; a port must carry it as a tagged file offset (D4: never write host pointers into script-visible state). |
| 0x13 | BCLCreateObject | 0x5fe530 | 0x751898 | 13 (+0x34) | H | 1941 | → obj; operand u16 c (class index). ConstructObject (0x60be00): FindClass(c), new 32-byte ScOSIObject with classID=c and nprops = first byte of the class record, every property nothing, refcount 1; pushes {1, obj} [0x5fe530] | none (FindClass 0x609af0 does not range-check c) [0x5fe530] |  |
| 0x14 | BCLMemberFunctionArgumentCheck | 0x5feb90 | 0x7518a8 | 17 (+0x44) | b | 3583 | a1..ak → a1..ak nothing×(n−k); operand u8 n (unsigned). k = (top − framePtr) in slots, truncated to 8 bits; pads with nothings until exactly n slots sit above the frame base [0x5feb90] | "Too many arguments to method" (n < k) [0x5feb90] | First instruction of every class method (after 0x51). k and n both count framePtr[0] = `this` plus the arguments: a zero-parameter method starts with 0x14 01 and is called with 0x51 01 (Base.osi: 1,809 of the 3,583 distinct method bodies, which the 8,406 method-table entries share; all 3,583 bodies start with 0x14, and all 3,445 `Dup; 0x12; 0x51` call sites use 0x51 01). 3,583 uses. |
| 0x15 | BCLSetThisMemberValue | 0x5fec10 | 0x7518b8 | 18 (+0x48) | H | 5102 | v → (empty); operand u16 m; this = framePtr[0]. this.type must be 1; v popped WITHOUT dispose (asm 0x5fec72 pushes 0; the decomp's Pop(true) is a misreading); prop = GetProperty(this, m); old prop disposed; prop ← v [0x5fec10] | "Member property of non member variable" (framePtr[0].type ≠ 1); "Reference to non-existant member property \"%s\"" (property absent) [0x5fec10] Order (asm): the type test (0x5fec5a) precedes any pop: 'non member variable' (0x5fec6d) leaves depth unchanged; v popped (0x5fec7e) before GetProperty (0x5fec98), so the absent-property error (0x5fecc1) leaves depth −1. (two independent asm readers agree, 2026-10-08: oracle second-reader + corpus-A review) | The type check happens before the pop, so on error v is still on the stack (moot: Error throws). |
| 0x16 | BCLGetThisMemberValue | 0x5fecf0 | 0x7518c8 | 19 (+0x4c) | H | 11951 | → value; operand u16 m; this = framePtr[0]; pushes Duplicate(*GetProperty(this, m)) [0x5fecf0] | "Member property of non member variable" (this.type ≠ 1; the decomp prints the argument as 0xe0f0 = low half of 0x74e0f0); "Reference to non-existant member property \"%s\"" [0x5fecf0] |  |
| 0x17 | BCLGetThisMemberFunction | 0x5fedb0 | 0x7518d8 | 20 (+0x50) | H | 1743 | → int(code address); operand u16 m; this = framePtr[0]; pushes {0x10, FindMethod(this, m)} (host address, as 0x12) [0x5fedb0] | "Member method of non member variable" (this.type ≠ 1; string 0x74e163); "Call to undefined member method" (FindMethod 0x608600) [0x5fedb0] |  |
| 0x18 | BCLGetMemberValueFromString | 0x5fee40 | 0x7518e8 | 21 (+0x54) | none | 0 | o s → value; s (member name) popped first, then o (both no dispose). s text = string-table entry (0x11) or the dynamic string (0x16). Game type o: map[(GetTypeToken(o.type), GetToken(s))] getter, push Duplicate(result), o disposed (s is not disposed on this path). Type 1: id = FindMemberID(s) (0x609ba0), push Duplicate(*GetProperty(o, id)); o and s disposed [0x5fee40] | "Invalid type for member access (must be a string)" (s not 0x11/0x16; string 0x74e188); "Reference to unregistered Game Variable" (SrOSIError; always-error, map never populated: nothing inserts into ScOSISystem+0x14, see "The game-variable map"); "Member property of non member variable" (o neither game type nor 1; string 0x74e0f0); "Reference to non-existant member property \"%s\"" [0x5fee40] Order (asm): s then o popped (0x5feec1, 0x5feef1) before every check; s-type test first (error 0x5fef93), then o (map miss 0x5ff0e1; not type 1: 0x5ff171), then the property (0x5ff1b9). Every error leaves depth −2. %s = s's text (0x5ff19f). (two independent asm readers agree, 2026-10-08: oracle second-reader + corpus-A review) | FindMemberID (0x609ba0) has no error path: on a name that is not a member symbol it returns 0xffff (initialised at 0x609baf), so the following GetProperty fails as an absent property. It lowercases the query (A–Z table 0x762a18) and strcmps the raw symbols; every lookup that misses the CACHE inserts {last symbol compared, id} (0x609ca7–0x609cc8) — on a symbol match that is the matched symbol and its id, on no match the last symbol and 0xffff — so after a failed lookup a later lookup of that symbol also returns 0xffff; symbol hits count toward the 50 entries too. An empty symbol table caches a NULL symbol pointer (0x609c57 xor esi, 0x609cb0), and any later lookup then strcmps against NULL (crash in the original; both implementations raise undefined behaviour) (cache poisoning, deterministic — both implementations model it; the cache has no bound in the original and the 51st insert overwrites its own count, which both implementations raise as undefined behaviour). 0 uses in Base.osi. |
| 0x19 | BCLGetMemberFunctionFromString | 0x5ff210 | 0x7518f8 | 22 (+0x58) | none | 12 | o s → int(code address); s popped first, then o (no dispose). o.type must be 1; s must be 0x11/0x16; id = FindMemberID(s); pushes {0x10, FindMethod(o, id)}; o and s disposed [0x5ff210] | "Member method of non member variable" (o.type ≠ 1, checked first; string 0x74e163); "Invalid method type for function call (must be a string)" (string 0x74e1ba); "Call to undefined member method" (FindMethod) [0x5ff210] Order (asm): s then o popped (0x5ff284, 0x5ff29f) before every check; the o test first (0x5ff2c2), then s (0x5ff338), then FindMethod (0x5ff350). Every error leaves depth −2. (two independent asm readers agree, 2026-10-08: oracle second-reader + corpus-A review) | 12 uses in Base.osi. |
| 0x1a | BCLSetMemberValueFromString | 0x5ff3a0 | 0x751908 | 23 (+0x5c) | none | 0 | v o s → (empty); s popped first, then o (no dispose). Game type o: getter called and result disposed, then v popped and setter (+0x10)(vm, o.value, v) called; o disposed. Type 1: v popped, id = FindMemberID(s), prop = GetProperty(o, id), old disposed, prop ← v; o and s disposed [0x5ff3a0] | "Invalid type for member set (must be a string)" (s not 0x11/0x16); "Reference to unregistered Game Variable" (SrOSIError; always-error, map never populated: nothing inserts into ScOSISystem+0x14, see "The game-variable map"); "Member property of non object variable" (o neither game type nor 1 — note 'object', unlike 0x10/0x18); "Reference to non-existant member property \"%s\"" [0x5ff3a0] Order (asm): s then o popped (0x5ff421, 0x5ff451); the s-type error (0x5ff4f3), the map miss (0x5ff641) and 'non object' (0x5ff6e2) come BEFORE v is popped: depth −2, v on top. Type 1: v popped (0x5ff6f6) before FindMemberID/GetProperty, so the absent-property error (0x5ff751) leaves depth −3. (two independent asm readers agree, 2026-10-08: oracle second-reader + corpus-A review) | 0 uses in Base.osi. |
| 0x1b | BCLUnused | 0x60be60 | 0x751918 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x1c | BCLUnused | 0x60be60 | 0x751928 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x1d | BCLUnused | 0x60be60 | 0x751938 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x1e | BCLUnused | 0x60be60 | 0x751948 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x1f | BCLUnused | 0x60be60 | 0x751958 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x20 | BCLUnused | 0x60be60 | 0x751968 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x21 | BCLGetVariableValue | 0x607c50 | 0x751978 | 26 (+0x68) | H | 14338 | → Duplicate(var); operand u16 idx. idx & 0x8000 (OSI_VM_GLOBAL_VARIABLE_INDICATOR, u16 at 0x751208 = 0x8000) selects a global: ScOSIScript::GetGlobalVariable (0x609d90) uses only idx & 0xff, walks the script's global-name table to that entry, hashes the name and returns the system-wide global with that token (ScOSISystem::GetGlobalVariable 0x609e10); otherwise the slot is framePtr[(int16)idx] [0x607c50] | none [0x607c50] | Bit 15 selects globals, so (int16)idx is never negative for a frame slot: arguments are framePtr[0..argc−1] (the frame base is the slot above the call marker, see 0x50) and locals follow. This corrects the consult's 'negative = arguments'. Globals are shared by NAME across scripts. ScOSISystem::GetGlobalVariable (0x609e10) inserts {nothing} when the name's token is absent, so the first access to an unknown global creates it as nothing: no error. |
| 0x22 | BCLSetVariableValue | 0x607b50 | 0x751988 | 25 (+0x64) | H | 3133 | v → (empty); operand u16 idx; v popped (no dispose); global: old disposed, var ← v; frame: PushIntoFrame (0x607c00) disposes framePtr[idx] and stores v [0x607b50] | none [0x607b50] | Same addressing as 0x21; storing to an unknown global creates it (GetGlobalVariable 0x609e10 inserts {nothing} first), no error. |
| 0x23 | BCLCreateStackVariables | 0x607b00 | 0x751998 | 24 (+0x60) | b | 674 | → nothing×n; operand u8 n (unsigned; osi.py reads 'b' signed, max in Base.osi 20) [0x607b00] | none [0x607b00] |  |
| 0x24 | BCLIncrementVariable | 0x607cf0 | 0x7519a8 | 27 (+0x6c) | H | 0 | — ; operand u16 idx (as 0x21); var.value += 1 in place (32-bit wrap) [0x607cf0] | "increment of non-integral type" (var.type ≠ 0x10) [0x607cf0] | 0 uses in Base.osi. |
| 0x25 | BCLDecrementVariable | 0x607d90 | 0x7519b8 | 28 (+0x70) | H | 0 | — ; operand u16 idx (as 0x21); var.value −= 1 in place (32-bit wrap) [0x607d90] | "decrement of non-integral type" (frame variable not int); "increment of non-integral type" (GLOBAL variable not int — the original's copy-paste message) [0x607d90] | 0 uses in Base.osi. |
| 0x26 | BCLUnused | 0x60be60 | 0x7519c8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x27 | BCLUnused | 0x60be60 | 0x7519d8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x28 | BCLUnused | 0x60be60 | 0x7519e8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x29 | BCLUnused | 0x60be60 | 0x7519f8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x2a | BCLUnused | 0x60be60 | 0x751a08 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x2b | BCLUnused | 0x60be60 | 0x751a18 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x2c | BCLUnused | 0x60be60 | 0x751a28 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x2d | BCLUnused | 0x60be60 | 0x751a38 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x2e | BCLUnused | 0x60be60 | 0x751a48 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x2f | BCLUnused | 0x60be60 | 0x751a58 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x30 | BCLPop | 0x606310 | 0x751a68 | 29 (+0x74) | none | 12046 | a → (empty); Pop with dispose [0x606310] | none [0x606310] |  |
| 0x31 | BCLPopN | 0x606340 | 0x751a78 | 30 (+0x78) | b | 0 | a1..an → (empty); operand u8 n (unsigned); n Pops with dispose [0x606340] | none [0x606340] | 0 uses. |
| 0x32 | BCLSwap | 0x606390 | 0x751a88 | 31 (+0x7c) | none | 0 | a b → b a; two Pops without dispose, pushes the first-popped then the second-popped [0x606390] | none [0x606390] | Decomp types are garbled; the asm (0x6063ba–0x606404) confirms the order. 0 uses. |
| 0x33 | BCLPull | 0x606420 | 0x751a98 | 32 (+0x80) | b | 4983 | x_n … x_1 → x_n … x_1 Duplicate(x_n); operand u8 n (unsigned): copies the slot at top−n, where top−1 is the top element (n=1 ≡ Dup) [0x606420] | none [0x606420] | n=0 copies the slot above the top (stale memory): undefined, never in Base.osi (operand range 1..17). |
| 0x34 | BCLDupN | 0x6064b0 | 0x751aa8 | 34 (+0x88) | b | 0 | a1..an → a1..an a1..an; operand u8 n: n times push Duplicate(top−n), which copies the top n elements in order [0x6064b0] | none [0x6064b0] | 0 uses. |
| 0x35 | BCLDup | 0x606470 | 0x751ab8 | 33 (+0x84) | none | 8050 | a → a Duplicate(a) [0x606470] | none [0x606470] | Duplicate (0x60aa20): refcount +1 for types 1, 5, 0x13, 0x20..0xfe; a deep copy for 0x16. |
| 0x36 | BCLUnused | 0x60be60 | 0x751ac8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x37 | BCLUnused | 0x60be60 | 0x751ad8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x38 | BCLUnused | 0x60be60 | 0x751ae8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x39 | BCLUnused | 0x60be60 | 0x751af8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x3a | BCLUnused | 0x60be60 | 0x751b08 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x3b | BCLUnused | 0x60be60 | 0x751b18 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x3c | BCLUnused | 0x60be60 | 0x751b28 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x3d | BCLUnused | 0x60be60 | 0x751b38 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x3e | BCLUnused | 0x60be60 | 0x751b48 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x3f | BCLUnused | 0x60be60 | 0x751b58 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x40 | BCLPushConstanti32 | 0x606600 | 0x751b68 | 39 (+0x9c) | i | 173 | → int(operand i32 little-endian) [0x606600] | none [0x606600] |  |
| 0x41 | BCLPushConstanti24 | 0x6065e0 | 0x751b78 | 38 (+0x98) | T | 0 | none: errors before any change; PC NOT advanced [0x6065e0] | "unwritten instruction BCL_PushConstanti24 called" (always) [0x6065e0] | osi.py assumes a 3-byte operand ('T') but the handler never reads one. 0 uses. |
| 0x42 | BCLPushConstanti16 | 0x606580 | 0x751b88 | 37 (+0x94) | h | 1193 | → int(sign-extended i16 operand) [0x606580] | none [0x606580] |  |
| 0x43 | BCLPushConstanti8 | 0x606540 | 0x751b98 | 36 (+0x90) | b | 8418 | → int(sign-extended i8 operand) [0x606540] | none [0x606540] |  |
| 0x44 | BCLPushConstantf32 | 0x606680 | 0x751ba8 | 40 (+0xa0) | f | 2099 | → float(the raw 4 operand bytes as f32) [0x606680] | none [0x606680] |  |
| 0x45 | BCLPushConstant0 | 0x606510 | 0x751bb8 | 35 (+0x8c) | none | 5305 | → int 0 [0x606510] | none [0x606510] |  |
| 0x46 | BCLPushConstantString | 0x606700 | 0x751bc8 | 41 (+0xa4) | H | 5021 | → {0x11, u16 operand zero-extended} (string-table index, not range-checked) [0x606700] | none [0x606700] |  |
| 0x47 | BCLPushNothing | 0x606840 | 0x751bd8 | 44 (+0xb0) | none | 5799 | → nothing {0x0f, 0} [0x606840] | none [0x606840] |  |
| 0x48 | BCLPushConstantColor8888 | 0x6067c0 | 0x751be8 | 43 (+0xac) | I | 109 | → {0x15, u32 operand} (colour, R in bits 24..31, A in bits 0..7; see 0xa0) [0x6067c0] | none [0x6067c0] |  |
| 0x49 | BCLPushConstantColor5551 | 0x606760 | 0x751bf8 | 42 (+0xa8) | H | 0 | → {0x14, u16 operand zero-extended} [0x606760] | none [0x606760] | Type 0x14, NOT the 0x15 colour type: the channel ops 0xa0–0xa7 reject it. 0 uses. |
| 0x4a | BCLUnused | 0x60be60 | 0x751c08 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x4b | BCLUnused | 0x60be60 | 0x751c18 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x4c | BCLUnused | 0x60be60 | 0x751c28 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x4d | BCLUnused | 0x60be60 | 0x751c38 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x4e | BCLUnused | 0x60be60 | 0x751c48 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x4f | BCLUnused | 0x60be60 | 0x751c58 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x50 | BCLJumpRelative | 0x600ae0 | 0x751c68 | 45 (+0xb4) | b | 16 | a1..an t → {2, retPC} a1..an; operand u8 n (unsigned). t popped (no dispose) and must be int; the return marker {type 2, value = PC after the n byte} is Inserted (0x600ba0, memmove) below the n arguments; framePtr = (first slot scanning down from the top whose type is 2 or 0xff) + 1; PC = script-file base (ScOSIScript::data, file offset 0 = 'OSI\0') + t.value [0x600ae0] | "Internal error: Jump to non-integral address" (t.type ≠ 0x10) [0x600ae0] | The binary names 0x50 JumpRelative: a file-relative offset (the opposite of the SAGESharp naming). 16 uses, all n=0. |
| 0x51 | BCLJumpAbsolute | 0x600c30 | 0x751c78 | 46 (+0xb8) | b | 10177 | a1..an t → {2, retPC} a1..an; operand u8 n (unsigned). t popped (no dispose) and must be int; the return marker {type 2, value = PC after the n byte} is Inserted (0x600ba0, memmove) below the n arguments; framePtr = (first slot scanning down from the top whose type is 2 or 0xff) + 1; PC = t.value used as a raw host pointer (as pushed by 0x12/0x17/0x19) [0x600c30] | "Internal error: Jump to non-integral address" (t.type ≠ 0x10) [0x600c30] | 0x51 JumpAbsolute: the target is an absolute code pointer. A port must resolve the tagged offset from 0x12/0x17/0x19; an int that did not come from them is undefined in the original. 10,177 uses, n ≤ 17. |
| 0x52 | BCLReturn | 0x600cf0 | 0x751c88 | 47 (+0xbc) | none | 3662 | … M l1..lk r → … r ; r popped (no dispose), then Pop WITH dispose until a type-2 or 0xff marker M has been popped. Type 2: push r, PC = M.value, framePtr recomputed as in 0x50, continue. Type 0xff: push M back, then push r, handler returns false (Run stops; PC unchanged) [0x600cf0] | none (no underflow check) [0x600cf0] | The 0xff case leaves {0xff marker, r} for the engine caller (CallWithReturn 0x60b450) to pop. Return does not advance PC itself. |
| 0x53 | BCLCompareAndBranchIfFalse | 0x600df0 | 0x751c98 | 48 (+0xc0) | h | 4981 | c → (empty); operand s16 d. c popped WITHOUT dispose (asm 0x600dfd pushes 0; the decomp's Pop(true) is a misreading). c nothing (0x0f) or int 0 → branch via vtable+0xc4 = BranchAlways 0x600e70 with PC still at this opcode, so PC = (address after d) + d; int ≠ 0 → PC += 3 [0x600df0] | "Results of comparison expression is not integral" (c.type neither 0x0f nor 0x10: float, string, object, colour…) [0x600df0] | No float/string truthiness: a float 0.0 condition is an ERROR, not a branch (falsifier). |
| 0x54 | BCLBranchAlways | 0x600e70 | 0x751ca8 | 49 (+0xc4) | h | 2272 | — ; operand s16 d (movsx); PC = (address after d) + d [0x600e70] | none [0x600e70] |  |
| 0x55 | BCLUnused | 0x60be60 | 0x751cb8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x56 | BCLUnused | 0x60be60 | 0x751cc8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x57 | BCLUnused | 0x60be60 | 0x751cd8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x58 | BCLUnused | 0x60be60 | 0x751ce8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x59 | BCLUnused | 0x60be60 | 0x751cf8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x5a | BCLUnused | 0x60be60 | 0x751d08 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x5b | BCLUnused | 0x60be60 | 0x751d18 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x5c | BCLUnused | 0x60be60 | 0x751d28 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x5d | BCLUnused | 0x60be60 | 0x751d38 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x5e | BCLUnused | 0x60be60 | 0x751d48 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x5f | BCLUnused | 0x60be60 | 0x751d58 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x60 | BCLEqualTo | 0x605180 | 0x751d68 | 60 (+0xf0) | none | 3597 | a b → int(a = b), type 0x10 value 0/1; in order: a nothing → 1 if b nothing else 0 (b disposed when it is not nothing) (0x6051d3–0x60522c); b nothing → 0, a disposed (0x605236–0x605261); both type 1 → pointer identity (sete 0x605293); both game types (0x20..0xfe) → same type AND same pointer (0x6052d0–0x605310); then 0x11 operands become dynamic strings in place (a 0x605369, then b 0x605397) and two strings compare case-insensitively (InsensitiveComparison 0x5faf80, called 0x6053d2: lengths must be equal; only ASCII a–z are folded to A–Z; every other byte, including ≥ 0x80, must match exactly); float,float → fld a, fcomp dword b (0x605442–0x605444); int a, float b → the int is loaded exactly by fild (0x60549f), fcomp dword b (0x6054a5); float a, int b → fild b (0x6054fa), fcomp dword a (0x605509); each float case tests `and ah,0x44; cmp ah,0x40` (C3 set and C2 clear: equal and ordered), so the result is the exact (double)a == (double)b with no rounding: any NaN → 0, +0.0 = −0.0 → 1; int,int by value (sete 0x605583). Operands disposed on the nothing/object/game-type/string paths only [0x605180] | "Invalid operand to operator '=' (lhs) (type:%02.2X)" (a not int after the cases above, e.g. string vs int, colour, array, object vs game object; 0x60554a); "… '=' (rhs) …" (b, 0x60556a) [0x605180] | Falsifiers: "ABC" = "abc" → 1; 1 = "1" → error; nothing = 0 → 0 (not an error); `16777217 16777216.0 =` → 0 (exact compare; the 'int converted to float first' reading gives 1). |
| 0x61 | BCLLessThan | 0x604fd0 | 0x751d78 | 59 (+0xec) | none | 119 | a b → int(a < b), type 0x10 value 0/1; float,float: fld a, fcomp dword b, `and ah,5; cmp ah,1` (C0 set, C2 clear) (0x605037–0x605040); int a, float b: the int is loaded exactly by fild (0x605090), fcomp dword b (0x605093), same test (0x605097–0x60509a); float a, int b: fild b (0x6050dc), fcomp dword a (0x6050e5), `test ah,0x45` → 1 only if b > a (0x6050e9); so every float case is the exact (double)a < (double)b with no rounding, and any NaN → 0; int,int signed (setl 0x605153) [0x604fd0] | "Invalid operand to operator '<' (lhs) (type:%02.2X)" (a.type ≠ 0x10 once the three float cases are excluded, so 1.0 vs a string reports lhs) (0x605120); "… '<' (rhs) …" (b, 0x60513d) [0x604fd0] | Strings, nothing and objects are errors here (only 0x60 handles them). Falsifier: `16777219 16777220.0 <` → 1 (the 'int converted to float first' reading gives 0). |
| 0x62 | BCLGreaterThan | 0x604e30 | 0x751d88 | 58 (+0xe8) | none | 157 | a b → int(a > b), type 0x10 value 0/1; float,float: fld a, fcomp dword b, `test ah,0x45` → 1 only if C0, C2 and C3 are all clear (0x604e97–0x604e9d); int a, float b: the int is loaded exactly by fild (0x604ee7), fcomp dword b (0x604eea), same test (0x604eee); float a, int b: fild b (0x604f30), fcomp dword a (0x604f39), `and ah,5; cmp ah,1` → 1 only if b < a (0x604f3d–0x604f40); so every float case is the exact (double)a > (double)b with no rounding, and any NaN → 0; int,int signed (setg 0x604faa) [0x604e30] | "Invalid operand to operator '>' (lhs) (type:%02.2X)" (a.type ≠ 0x10 once the three float cases are excluded, so 1.0 vs a string reports lhs) (0x604f77); "… '>' (rhs) …" (b, 0x604f94) [0x604e30] | Strings, nothing and objects are errors here (only 0x60 handles them). Falsifier: `16777217 16777216.0 >` → 1 (the 'int converted to float first' reading gives 0). |
| 0x63 | BCLLessOrEqual | 0x604c80 | 0x751d98 | 57 (+0xe4) | none | 65 | a b → int(a <= b), type 0x10 value 0/1; float,float: fld a, fcomp dword b, `test ah,4` (C2 set → 0), then `and ah,0x45; test ah,0x41` (C0 or C3 → 1) (0x604ce7–0x604cf5); int a, float b: the int is loaded exactly by fild (0x604d3a), fcomp dword b (0x604d43), same tests (0x604d47–0x604d4f); float a, int b: fild b (0x604d97), fcomp dword a (0x604d9a), `test ah,5` → 1 only if b ≥ a and ordered (0x604d9e); so every float case is the exact (double)a <= (double)b with no rounding, and any NaN → 0; int,int signed (setle 0x604e08) [0x604c80] | "Invalid operand to operator '<=' (lhs) (type:%02.2X)" (a.type ≠ 0x10 once the three float cases are excluded, so 1.0 vs a string reports lhs) (0x604dd5); "… '<=' (rhs) …" (b, 0x604df2) [0x604c80] | Strings, nothing and objects are errors here (only 0x60 handles them). Falsifier: `16777217 16777216.0 <=` → 0 (the 'int converted to float first' reading gives 1). |
| 0x64 | BCLGreaterOrEqual | 0x604ad0 | 0x751da8 | 56 (+0xe0) | none | 755 | a b → int(a >= b), type 0x10 value 0/1; float,float: fld a, fcomp dword b, `test ah,5` (C0 or C2 → 0) (0x604b37–0x604b3d); int a, float b: the int is loaded exactly by fild (0x604b87), fcomp dword b (0x604b8a), same test (0x604b8e); float a, int b: fild b (0x604bd0), fcomp dword a (0x604bd9), `test ah,4` (C2 → 0) then `and ah,0x45; test ah,0x41` → 1 only if b ≤ a (0x604bdd–0x604be5); so every float case is the exact (double)a >= (double)b with no rounding, and any NaN → 0; int,int signed (setge 0x604c52) [0x604ad0] | "Invalid operand to operator '>=' (lhs) (type:%02.2X)" (a.type ≠ 0x10 once the three float cases are excluded, so 1.0 vs a string reports lhs) (0x604c1f); "… '>=' (rhs) …" (b, 0x604c3c) [0x604ad0] | Strings, nothing and objects are errors here (only 0x60 handles them). Falsifiers: `16777219 16777220.0 >=` → 0 (the 'int converted to float first' reading gives 1); `16777216.0 16777217 >=` → 0 (the float,int path; the old reading gives 1). |
| 0x65 | BCLUnused | 0x60be60 | 0x751db8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x66 | BCLUnused | 0x60be60 | 0x751dc8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x67 | BCLUnused | 0x60be60 | 0x751dd8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x68 | BCLUnused | 0x60be60 | 0x751de8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x69 | BCLUnused | 0x60be60 | 0x751df8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x6a | BCLAnd | 0x6055b0 | 0x751e08 | 61 (+0xf4) | none | 139 | a b → int(a ≠ 0 AND b ≠ 0); both popped first (no short-circuit) [0x6055b0] | "Invalid operand to operator 'and' (lhs) (type:%02.2X)"; "… 'and' (rhs) …" (any non-int, nothing included) [0x6055b0] |  |
| 0x6b | BCLOr | 0x605670 | 0x751e18 | 62 (+0xf8) | none | 53 | a b → int(a ≠ 0 OR b ≠ 0) [0x605670] | "Invalid operand to operator 'or' (lhs) (type:%02.2X)"; "… 'or' (rhs) …" [0x605670] |  |
| 0x6c | BCLNot | 0x605b40 | 0x751e28 | 66 (+0x108) | none | 852 | a → int(a = 0); nothing → int 1 [0x605b40] | "Invalid operand to operator 'not' (type:%02.2X)" (neither int nor nothing) [0x605b40] |  |
| 0x6d | BCLBitwiseAnd | 0x605730 | 0x751e38 | 63 (+0xfc) | none | 10 | a b → int(a & b), 32-bit [0x605730] | "Invalid operand to operator '&' (lhs) (type:%02.2X)"; "… (rhs) …" (non-int) [0x605730] |  |
| 0x6e | BCLBitwiseOr | 0x6057e0 | 0x751e48 | 64 (+0x100) | none | 14 | a b → int(a OR (bitwise) b), 32-bit [0x6057e0] | "Invalid operand to operator '\|' (lhs) (type:%02.2X)"; "… (rhs) …" (non-int) [0x6057e0] |  |
| 0x6f | BCLBitwiseXor | 0x605890 | 0x751e58 | 65 (+0x104) | none | 0 | a b → int(a XOR b), 32-bit [0x605890] | "Invalid operand to operator '@' (lhs) (type:%02.2X)"; "… (rhs) …" (non-int) [0x605890] | The original spells XOR as '@' in its message. |
| 0x70 | BCLAdd | 0x603ff0 | 0x751e68 | 50 (+0xc8) | none | 1500 | a b → a+b (b popped first, 0x60400d, then a, 0x60402e; both without dispose); 0x11 operands are first converted to dynamic strings (a 0x604053, then b 0x60407e); string+string → a with b's text appended in place (ScFixedString::append 0x4f5c80; capacity mMaxSize = 0xff, set by the ctors 0x46de40 and 0x60b8d0), b disposed (0x6040f3), a pushed; if len(a)+len(b) ≥ 255, append throws BEFORE anything is pushed: a and b are already popped without dispose (0x60400d, 0x60402e) and 0x11 operands already copied to new dynamic strings, so the stack is 2 slots shorter and nothing is disposed (both leak); float,float → fld a, fadd dword b, fstp dword (0x604165–0x604169); int a, float b → the int is loaded exactly by fild (0x6041b7), fadd dword b (0x6041bd), fstp dword (0x6041bf); float a, int b → fild b (0x604207), fadd dword a (0x604213), fstp dword (0x604215); so every float result is the exact result rounded per the precision state `fpu_pc` (Conventions, D8′); int,int → int, add (0x604283), 32-bit two's-complement wrap, pushed with a's type word [0x603ff0] | "Invalid operand to string operator '+' (rhs) (type:%02.2X)" (a string, b not; prints b's type, 0x60410c); "Invalid operand to string operator '+' (lhs) (type:%02.2X)" (b string, a not; prints a's type, 0x604133); "Invalid operand to operator '+' (lhs) …" (0x60424f) / "… (rhs) …" (0x60426f) (non-numbers, after the three float cases); kLenTooLong — a NON-OSI ScError (0x83aa20, qualifier kNoQualifier 0x839118) thrown by SrThrow (0x4d91f0) inside ScFixedString::append (0x4f5c80–0x4f5ca9: throws when mStrSize ≥ mMaxSize − n, unsigned) for string+string with len(a)+len(b) ≥ 255; the call site 0x6040bf has only an exception specification (!ScCatchMessage!) and no catch, so it leaves the handler [0x603ff0] Order (asm): both operands popped before any test (0x60400d/0x60402e; lhs 0x604257, rhs 0x604277), so every error leaves depth −2. After the float cases the lhs test is a.type ≠ 0x10: one valid float with one invalid operand always raises the (lhs) message (a float lhs prints type 12). Exception: a 0x11/0x16 operand takes the string branch first — string a with float b → `string operator '+' (rhs) (type:12)` (0x60410c); string b with float a → `(lhs) (type:12)` (0x604133). (two independent asm readers agree, 2026-10-08: oracle second-reader + corpus-A review) | Only '+' concatenates; string + int is an error (no implicit conversion). There is no truncation. Falsifiers: a 200-char string + a 60-char string must raise kLenTooLong; 200 + 54 (= 254 chars) succeeds; `16777217 1.0 +` → 16777218.0 (the 'int converted to float first' reading gives 16777216.0). |
| 0x71 | BCLSubtract | 0x6042b0 | 0x751e78 | 51 (+0xcc) | none | 1555 | a b → a−b (b popped first, 0x6042c9, then a, 0x6042e4; both without dispose); float,float → fld a, fsub dword b, fstp dword (0x604314–0x604318); int a, float b → the int is loaded exactly by fild (0x60435e), fsub dword b (0x604361), fstp dword (0x604363), with no f32 rounding of the int; float a, int b → fild b (0x6043a8), fsubr dword a = a − b (0x6043b1), fstp dword (0x6043b3); so every float result is the exact result rounded per the precision state `fpu_pc` (Conventions, D8′); int,int → int, sub (0x60441d), 32-bit two's-complement wrap, pushed with a's type word [0x6042b0] | "Invalid operand to operator '-' (lhs) (type:%02.2X)" (a.type ≠ 0x10 once the three float cases are excluded, 0x6043ef); "… '-' (rhs) …" (b, 0x60440c) [0x6042b0] Order (asm): both operands popped before any test (0x6042c9/0x6042e4; lhs 0x6043f7, rhs 0x604414), so every error leaves depth −2. After the float cases the lhs test is a.type ≠ 0x10: one valid float with one invalid operand always raises the (lhs) message (a float lhs prints type 12). (two independent asm readers agree, 2026-10-08: oracle second-reader + corpus-A review) | Strings, nothing and objects are errors. Falsifier: `16777217 1.0 -` → 16777216.0 (the 'int converted to float first' reading gives 16777215.0). |
| 0x72 | BCLMultiply | 0x604450 | 0x751e88 | 52 (+0xd0) | none | 257 | a b → a×b (b popped first, 0x604469, then a, 0x604484; both without dispose); float,float → fld a, fmul dword b, fstp dword (0x6044b4–0x6044b8); int a, float b → the int is loaded exactly by fild (0x6044fe), fmul dword b (0x604501), fstp dword (0x604503); float a, int b → fild b (0x604548), fmul dword a (0x604551), fstp dword (0x604553); so every float result is the exact result rounded per the precision state `fpu_pc` (Conventions, D8′); int,int → int, imul (0x6045c0), low 32 bits (two's-complement wrap), pushed with a's type word [0x604450] | "Invalid left-hand-side operand to operator '*' (type:%02.2X)" (lhs — different wording; a.type ≠ 0x10 once the three float cases are excluded, 0x60458f); "Invalid operand to operator '*' (rhs) (type:%02.2X)" (b, 0x6045ac) [0x604450] Order (asm): both operands popped before any test (0x604469/0x604484; lhs 0x604597, rhs 0x6045b4), so every error leaves depth −2. After the float cases the lhs test is a.type ≠ 0x10: one valid float with one invalid operand always raises the (lhs) message (a float lhs prints type 12). (two independent asm readers agree, 2026-10-08: oracle second-reader + corpus-A review) | Falsifier: `16777217 3.0 *` → 50331652.0 (the 'int converted to float first' reading gives 50331648.0). |
| 0x73 | BCLDivide | 0x6045f0 | 0x751e98 | 53 (+0xd4) | none | 38 | a b → a÷b (b popped first, 0x604609, then a, 0x604624; both without dispose); float,float: zero test of b (fld 0.0 from 0x74f388, fcomp dword b, `and ah,0x44; cmp ah,0x40` = equal and ordered, 0x60464e–0x60465e), then fld a, fdiv dword b, fstp dword (0x604679–0x60467d); int a, float b: the same zero test of b (0.0 from 0x74f38c, 0x6046bd–0x6046cd), then the int is loaded exactly by fild (0x6046e8), fdiv dword b (0x6046eb), fstp dword (0x6046ed); float a, int b: b == 0 (cmp dword, 0x604725), then fild b (0x60473e), fdivr dword a = a ÷ b (0x604747), fstp dword (0x604749); so every float result is the exact result rounded per the precision state `fpu_pc` (Conventions, D8′); int,int: the two type checks, then b == 0 (0x6047aa), then cdq; idiv (0x6047c3–0x6047c4), truncating toward 0, pushed as type 0x10 (0x6047c6) [0x6045f0] | "Divide by 0" (0x74f557; float b == ±0.0 — the compare is an ordered equality, so −0.0 errors and a NaN b does not; int b == 0; tested before the division in every case, and in int,int after the two type checks); "Invalid operand to operator '/' (lhs) (type:%02.2X)" (0x60477f); "… '/' (rhs) …" (0x60479c) [0x6045f0] Order (asm): both operands popped before any test (0x604609/0x604624; lhs 0x604787, rhs 0x6047a4), so every error leaves depth −2. After the float cases the lhs test is a.type ≠ 0x10: one valid float with one invalid operand always raises the (lhs) message (a float lhs prints type 12). (two independent asm readers agree, 2026-10-08: oracle second-reader + corpus-A review) | Zero divisors are errors, but a float result can still be ±inf (f32 overflow at the final fstp dword: `1.0e30 1.0e-30 /` → +inf, no error) or NaN (a NaN operand passes the zero test). INT_MIN / −1 raises a CPU fault (#DE) in the original (UNVERIFIED crash; untestable). Falsifier: `50331651 3.0 /` → 16777216.0 (the 'int converted to float first' reading gives 16777218.0). |
| 0x74 | BCLPower | 0x6047f0 | 0x751ea8 | 54 (+0xd8) | none | 0 | a b → a^b by case (asm 0x604829–0x6049e5; two fldz at 0x604829 reserve two x87 slots that every path releases with one fcompp): float,float → (float)pow((double)a,(double)b) (_pow 0x604870, one rounding by fstp dword 0x604878; the three later fld/fstp dword pairs are no-ops on an f32); int,float → the int is loaded exactly by fild (0x6048d4) and widened to double: (float)pow((double)a,(double)b) (_pow 0x6048e9, fstp dword 0x6048f1); float a, int b → repeat b times (b unsigned: −1 = 2^32−1): x ← (float)(int32 reinterpretation of x's bits) × x, result float; int,int → repeat b times x ← x×x (wrapping imul, 0x6049c0–0x6049cf), i.e. a^(2^b), result int with a's type word [0x6047f0] | "Invalid operand to operator '^' (lhs) (type:%02.2X)" (0x604990); "Invalid operand to operator '^' (rhs) (type:%02.2X)" (string 0x74f5ff, 0x6049af; the decomp drops both arguments) [0x6047f0] Order (asm): both operands popped before any test (0x604809/0x604824; lhs 0x60499a, rhs 0x6049b7), so every error leaves depth −2. After the float cases the lhs test is a.type ≠ 0x10: one valid float with one invalid operand always raises the (lhs) message (a float lhs prints type 12). (two independent asm readers agree, 2026-10-08: oracle second-reader + corpus-A review) | Resolved from the asm: the float^int loop (0x604944–0x604953) copies x's raw bits (written by the previous fstp) to a temporary and loads them with fild, so x ← (float)(int32 bits of x) × x. The decomp's (float)(int)x reads like a value truncation and is wrong. Falsifier kept as a guard: `2.0 1 ^` → 2147483648.0 (not 4.0, not 2.0). The int,int loop squares: `3 2 ^` → 81, not 9; b = 0 leaves a unchanged (a^0 = a). 0 uses in Base.osi. |
| 0x75 | BCLModulus | 0x604a00 | 0x751eb8 | 55 (+0xdc) | none | 12 | a b → int((uint32)a mod (uint32)b) (unsigned div: xor edx, edx; div ecx at 0x604a99–0x604a9b; remainder pushed with a's type word); there is no float path and no x87 instruction in this handler [0x604a00] | "Invalid operand to operator '%' (lhs) (type:%02.2X)" (a, 0x604a55); "… '%' (rhs) …" (b, 0x604a72) (non-int; floats included); "Divide by 0" (b == 0, tested after both type checks, 0x604a80–0x604a86) [0x604a00] Order (asm): both popped (0x604a19/0x604a34) before any test; no float path: a float lhs → (lhs) (type:12) (0x604a5d); int lhs with a float rhs → (rhs) (type:12) (0x604a7a); then the zero test (0x604a80). (two independent asm readers agree, 2026-10-08: oracle second-reader + corpus-A review) | Falsifier: `-1 3 %` → 0 (unsigned), not −1 (C signed %). 12 uses. |
| 0x76 | BCLBitwiseNot | 0x605aa0 | 0x751ec8 | 67 (+0x10c) | none | 6 | a → int(~a); nothing → int 1 (the same nothing rule as 0x6c) [0x605aa0] | "Invalid operand to operator '~' (type:%02.2X)" [0x605aa0] |  |
| 0x77 | BCLShiftLeft | 0x605940 | 0x751ed8 | 68 (+0x110) | none | 2 | a b → int(a << (b & 31)) (shl by cl) [0x605940] | "Invalid operand to operator '<<' (lhs) (type:%02.2X)"; "… '<<' (rhs) …" [0x605940] |  |
| 0x78 | BCLShiftRight | 0x6059f0 | 0x751ee8 | 69 (+0x114) | none | 0 | a b → int((uint32)a >> (b & 31)) (LOGICAL shr, asm 0x605a76) [0x6059f0] | "Invalid operand to operator '>>' (lhs) (type:%02.2X)"; "… '>>' (rhs) …" [0x6059f0] | Falsifier: `-8 1 >>` → 0x7ffffffc, not −4. 0 uses. |
| 0x79 | BCLUnused | 0x60be60 | 0x751ef8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x7a | BCLIncrement | 0x605be0 | 0x751f08 | 70 (+0x118) | none | 0 | a → a' : top.value += 1 in place, type unchanged, NO type check [0x605be0] | none [0x605be0] | On a float this adds 1 to the raw bits (next representable float), not 1.0. 0 uses. |
| 0x7b | BCLDecrement | 0x605c00 | 0x751f18 | 71 (+0x11c) | none | 0 | a → a' : top.value −= 1 in place, NO type check [0x605c00] | none [0x605c00] | As 0x7a. 0 uses. |
| 0x7c | BCLUnused | 0x60be60 | 0x751f28 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x7d | BCLUnused | 0x60be60 | 0x751f38 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x7e | BCLUnused | 0x60be60 | 0x751f48 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x7f | BCLUnused | 0x60be60 | 0x751f58 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x80 | BCLGetGameVariable | 0x6020d0 | 0x751f68 | 74 (+0x128) | HH | 0 | — ; operands u16 c, u16 n: computes GetStringTableToken (0x601f10, memoised per index) of both and discards them; no stack change [0x6020d0] | none [0x6020d0] | A no-op in this build (the spec's 'engine-state opcode' has no state effect). 0 uses. |
| 0x81 | BCLSetGameVariable | 0x602040 | 0x751f78 | 73 (+0x124) | HH | 0 | — ; operands u16 c, u16 n: computes GetStringTableToken (0x601f10, memoised per index) of both and discards them; no stack change [0x602040] | none [0x602040] | A no-op in this build (the spec's 'engine-state opcode' has no state effect). 0 uses. |
| 0x82 | BCLCallGameFunction | 0x6015a0 | 0x751f88 | 72 (+0x120) | HHb | 4402 | a1..an → r; operands u16 C, u16 F (string-table indices: class, function), u8 n. Lookup map<ScOSIDualToken(GetStringTableToken(F), GetStringTableToken(C)), RegisteredFunction*>; n checked against max (+0xc) and min (+0xd); arguments: arg i (i = 0 deepest … n−1 top) is the slot top−(n−i); declared type paramType[i] (u16 at RegisteredFunction+0xe+2i) 0x200 → args[i] = a fresh 8-byte box {0x200, address of the slot}, whose type equals the declared one so no conversion runs; else a type that differs from the slot's is converted in place: slot ← ConvertVariant(slot, paramType[i]) (0x60b9d0; old value disposed by PushInto 0x601ec0; a float→int conversion truncates and forces the x87 rounding field to nearest, `and 0xf3ff` at 0x60bc60); the native is called as fn(vm, w0..w9) with wi = args[i] ? *(args[i]+4) : 0 (0x60198c–0x601abc), so for a 0x200 parameter the native receives the ADDRESS OF THE ARGUMENT'S STACK SLOT (an ScOSIVariant*), never the box address; boxes freed; n Pops WITH dispose (0x601c5f), which also dispose whatever the native wrote into a by-reference slot; the returned variant pushed; finally the instruction self-patches: byte 0 ← 0x84, bytes 1..4 ← RegisteredFunction* (LE), byte 5 (n) unchanged; PC = after n [0x6015a0] | Per path, from the .exc call-site table (function info 0x6dafe4; see 'Exception tables'): the map miss (SrOSIError at 0x601725, no catch record) escapes as "Call to unregistered Game Function". Every later call is inside a catch(…) (catch records !ScCatchMessage! → 0x601c94, !exception! → 0x601cb3, … → 0x601cd3): "Too many parameters passed to game function" (0x6017ac), "Too few parameters passed to game function" (0x6017d5), the box operator new (0x601845), ConvertVariant (0x6018db, e.g. "Requested variant conversion unimplimented.  Recompile to debug for types"), PushInto (0x60191b), the native call (0x601af5; its inner catch 0x601b10/0x601b30/0x601b50 re-throws at 0x601be5), operator delete, the pops (0x601c5f) and the result push (0x601c8d) all end in Error("Script is calling %s.%s", C, F) at 0x601dd3, which is outside every try and throws kOSIError. On those caught paths nothing is pushed and the remaining arguments stay (a Dispose that throws inside the pop loop at 0x601c5f leaves some already popped), arguments already converted stay converted, boxes leak and there is no self-patch [0x6015a0] | Conversions to 0x10/0x12/0x16 from an unsupported type do NOT error: the slot becomes {type 0, 0}. Self-patch per D4: our VM memoises per call site and traces 0x82. Error() formats into a static buffer (0x7e57e1) before throwing {kOSIError, kNoQualifier}, so the inner messages are overwritten and never thrown. 4,402 uses. |
| 0x83 | BCLCallGameFunctionFromString | 0x602160 | 0x751f98 | 75 (+0x12c) | Hb | 0 | a1..an s → r; operands u16 C (class string index), u8 n. s (function name) is popped first (no dispose): text of the string-table entry (0x11) or dynamic string (0x16). Lookup map[(GetToken(s), GetStringTableToken(C))]; min/max as 0x82; arguments: arg i (i = 0 deepest … n−1 top) is the slot top−(n−i); declared type paramType[i] (u16 at RegisteredFunction+0xe+2i) 0x200 → args[i] = a fresh 8-byte box {0x200, address of the slot}, whose type equals the declared one so no conversion runs; else a type that differs from the slot's is converted in place: slot ← ConvertVariant(slot, paramType[i]) (0x60b9d0; old value disposed by PushInto 0x601ec0; a float→int conversion truncates and forces the x87 rounding field to nearest, `and 0xf3ff` at 0x60bc60); the native is called as fn(vm, w0..w9) with wi = args[i] ? *(args[i]+4) : 0 (0x6027bf up to the native call at 0x602938), so for a 0x200 parameter the native receives the ADDRESS OF THE ARGUMENT'S STACK SLOT (an ScOSIVariant*), never the box address; boxes freed; n Pops WITH dispose (0x6029b8), which also dispose whatever the native wrote into a by-reference slot; the returned variant pushed; s disposed; no self-patch [0x602160] | Per path, from the .exc table (function info 0x6db484): "Invalid member type for function call (must be a string)" (0x6022c3, s not 0x11/0x16; no catch record) escapes. Everything from the token lookup on (0x602316) is inside a catch(…) (0x6029f6 / 0x602a15 / 0x602a35), except the lazy ScOSISystem ctor calls at 0x602340/0x6023a3, which have cleanup records only (no practical effect): the map miss (SrOSIError 0x602431), too many (0x60246d), too few (0x602496), the box new, ConvertVariant/PushInto (inner catch 0x602620/0x602640/0x602660 → Error("Argument type mismatch in call to game function (argument %d [zero based])") at 0x602738, itself caught by the outer one), the native call (0x602938), the pops and the push all end in Error("Script is calling %s.%s", C, s) at 0x602b24, outside every try → kOSIError. On those paths s has been popped (not disposed), the arguments stay (converted ones stay converted) and nothing is pushed [0x602160] | Not self-patching. 0 uses in Base.osi. |
| 0x84 | BCLCallGameFunctionDirect | 0x602bb0 | 0x751fa8 | 76 (+0x130) | ib | 0 | a1..an → r; operands u32 p (RegisteredFunction*), u8 n. NO lookup and NO min/max check; arguments: arg i (i = 0 deepest … n−1 top) is the slot top−(n−i); declared type paramType[i] (u16 at RegisteredFunction+0xe+2i) 0x200 → args[i] = a fresh 8-byte box {0x200, address of the slot}, whose type equals the declared one so no conversion runs; else a type that differs from the slot's is converted in place: slot ← ConvertVariant(slot, paramType[i]) (0x60b9d0; old value disposed by PushInto 0x601ec0; a float→int conversion truncates and forces the x87 rounding field to nearest, `and 0xf3ff` at 0x60bc60); the native is called as fn(vm, w0..w9) with wi = args[i] ? *(args[i]+4) : 0 (0x602dd1 onward), so for a 0x200 parameter the native receives the ADDRESS OF THE ARGUMENT'S STACK SLOT (an ScOSIVariant*), never the box address; boxes freed; n Pops WITH dispose (0x602f6f), which also dispose whatever the native wrote into a by-reference slot; the returned variant pushed [0x602bb0] | Every failing call (box new 0x602cf6, ConvertVariant 0x602d76, PushInto 0x602daa, the native 0x602efd, delete, pops 0x602f6f, push 0x602f97) is inside one catch(…) (0x602fa0 / 0x602fc0 / 0x602fe0, function info 0x6db630), which ends in Error("Script is calling a game function directly.") at 0x60303c, outside every try → kOSIError; the ConvertVariant message never surfaces [0x602bb0] | Only ever produced by 0x82's self-patch (0 in the pristine file). A literal 0x84 in a script calls an arbitrary address in the original; conformance tests must not contain one. osi.py's 'i' (signed) is a raw pointer. |
| 0x85 | BCLUnused | 0x60be60 | 0x751fb8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x86 | BCLUnused | 0x60be60 | 0x751fc8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x87 | BCLUnused | 0x60be60 | 0x751fd8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x88 | BCLUnused | 0x60be60 | 0x751fe8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x89 | BCLUnused | 0x60be60 | 0x751ff8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x8a | BCLUnused | 0x60be60 | 0x752008 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x8b | BCLUnused | 0x60be60 | 0x752018 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x8c | BCLUnused | 0x60be60 | 0x752028 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x8d | BCLUnused | 0x60be60 | 0x752038 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x8e | BCLUnused | 0x60be60 | 0x752048 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x8f | BCLUnused | 0x60be60 | 0x752058 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x90 | BCLCreateArray | 0x5fd070 | 0x752068 | 77 (+0x134) | none | 406 | → {5, new empty ScOSIArray (refcount 1)} [0x5fd070] | none [0x5fd070] | Arrays are reference types: 0x93–0x96 mutate the array in place and push nothing back. |
| 0x91 | BCLGetArrayValue | 0x5fd0f0 | 0x752078 | 78 (+0x138) | none | 2517 | a i → elem; i popped first (type NOT checked; its value word is used unsigned). a type 5: push Duplicate(a[i]). a type 0x13 (string table): push a new dynamic string (0x16) copy of entry i. a disposed (not on the 0x13 path) [0x5fd0f0] | "Reference beyond start/end of array" (5, i ≥ count; negative i counts as huge); "Reference beyond end of string table" (0x13, i ≥ count); "array reference on non-array" (neither 5 nor 0x13) [0x5fd0f0] | 0x13 walk: entries start at +0x10, stride = len + 2 bytes (asm-proven, 0x5fd171–0x5fd177); the string is copied from entry+1 up to its first NUL (ScFixedString::Init with npos, 0x5fd1ba), so the layout is presumably length byte, len chars, NUL — UNVERIFIED: no 0x13 value is ever created; the walk counts with the low 16 bits of i (bound check uses all 32) [0x5fd171–0x5fd1b5]. i is never disposed. |
| 0x92 | BCLElementsInArray | 0x5fd2d0 | 0x752088 | 79 (+0x13c) | none | 826 | a → int(count) (type 5: vector size at +0xc; type 0x13: count at +4); a disposed [0x5fd2d0] | "array reference on non-array" (neither 5 nor 0x13) [0x5fd2d0] |  |
| 0x93 | BCLSetArrayValue | 0x5fd360 | 0x752098 | 80 (+0x140) | none | 156 | a i v → (empty); v popped first, then i, then a; i's type is never checked and i is not disposed: its value word is compared unsigned (jb at 0x5fd3ef); type 5: old a[i] disposed, a[i] ← v; a disposed [0x5fd360] | "Access beyond end of array" (i ≥ count, unsigned); "Assigning into string table is illegal" (0x13); "array reference on non-array" (neither 5 nor 0x13) [0x5fd360] |  |
| 0x94 | BCLAppendToArray | 0x5fd450 | 0x7520a8 | 81 (+0x144) | none | 2621 | a v → (empty); type 5: resize to count+1, a[count] ← v; a disposed [0x5fd450] | "Assigning into string table is illegal" (0x13); "array reference on non-array" (neither 5 nor 0x13) [0x5fd450] |  |
| 0x95 | BCLRemoveFromArray | 0x5fd500 | 0x7520b8 | 82 (+0x148) | none | 35 | a i → (empty); type 5: a[i] disposed then erased (later elements shift down); a disposed [0x5fd500] | "removal outside of array bounds" (i ≥ count); "Removal from string table is illegal" (0x13); "array reference on non-array" (neither 5 nor 0x13) [0x5fd500] |  |
| 0x96 | BCLInsertIntoArray | 0x5fd5d0 | 0x7520c8 | 83 (+0x14c) | none | 2 | a i v → (empty); type 5: v inserted before a[i]; a disposed [0x5fd5d0] | "insertion outside of array bounds" (SrOSIError when i ≥ count: inserting at i = count is an ERROR, use 0x94); "Assigning into string table is illegal" (0x13); "array reference on non-array" (neither 5 nor 0x13) [0x5fd5d0] |  |
| 0x97 | BCLUnused | 0x60be60 | 0x7520d8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x98 | BCLUnused | 0x60be60 | 0x7520e8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x99 | BCLUnused | 0x60be60 | 0x7520f8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x9a | BCLUnused | 0x60be60 | 0x752108 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x9b | BCLUnused | 0x60be60 | 0x752118 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x9c | BCLUnused | 0x60be60 | 0x752128 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x9d | BCLUnused | 0x60be60 | 0x752138 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x9e | BCLUnused | 0x60be60 | 0x752148 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0x9f | BCLUnused | 0x60be60 | 0x752158 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xa0 | BCLSetRedValue | 0x5fff30 | 0x752168 | 84 (+0x150) | none | 6 | c v → c'; v popped first; v clamped: (uint32)v > 255 → 255 (so negatives → 255); c' = c with bits 24..31 ← v; pushed as 0x15 [0x5fff30] | "cant access red value of anything but a color" (c.type ≠ 0x15); "cant set red to a non-integral value" (v.type ≠ 0x10) [0x5fff30] Order (asm): v then c popped (no dispose; 0x5fff49, 0x5fff64) before both tests; c.type ≠ 0x15 tested first (error 0x5fff87), then v.type ≠ 0x10 (0x5fff9d). Either error leaves depth −2; with both wrong the colour message wins. (two independent asm readers agree, 2026-10-08: oracle second-reader + corpus-A review) |  |
| 0xa1 | BCLSetGreenValue | 0x5ffff0 | 0x752178 | 85 (+0x154) | none | 6 | c v → c'; v popped first; v clamped: (uint32)v > 255 → 255 (so negatives → 255); c' = c with bits 16..23 ← v; pushed as 0x15 [0x5ffff0] | "cant access green value of anything but a color" (c.type ≠ 0x15); "cant set green to a non-integral value" (v.type ≠ 0x10) [0x5ffff0] Order (asm): v then c popped (no dispose; 0x600009, 0x600024) before both tests; c.type ≠ 0x15 tested first (error 0x600047), then v.type ≠ 0x10 (0x60005d). Either error leaves depth −2; with both wrong the colour message wins. (two independent asm readers agree, 2026-10-08: oracle second-reader + corpus-A review) |  |
| 0xa2 | BCLSetBlueValue | 0x6000b0 | 0x752188 | 86 (+0x158) | none | 6 | c v → c'; v popped first; v clamped: (uint32)v > 255 → 255 (so negatives → 255); c' = c with bits 8..15 ← v; pushed as 0x15 [0x6000b0] | "cant access blue value of anything but a color" (c.type ≠ 0x15); "cant set blue to a non-integral value" (v.type ≠ 0x10) [0x6000b0] Order (asm): v then c popped (no dispose; 0x6000c9, 0x6000e4) before both tests; c.type ≠ 0x15 tested first (error 0x600107), then v.type ≠ 0x10 (0x60011d). Either error leaves depth −2; with both wrong the colour message wins. (two independent asm readers agree, 2026-10-08: oracle second-reader + corpus-A review) |  |
| 0xa3 | BCLSetAlphaValue | 0x600170 | 0x752198 | 87 (+0x15c) | none | 6 | c v → c'; v popped first; v clamped: (uint32)v > 255 → 255 (so negatives → 255); c' = c with bits 0..7 ← v; pushed as 0x15 [0x600170] | "cant access alpha value of anything but a color" (c.type ≠ 0x15); "cant set alpha to a non-integral value" (v.type ≠ 0x10) [0x600170] Order (asm): v then c popped (no dispose; 0x600189, 0x6001a4) before both tests; c.type ≠ 0x15 tested first (error 0x6001c7), then v.type ≠ 0x10 (0x6001dd). Either error leaves depth −2; with both wrong the colour message wins. (two independent asm readers agree, 2026-10-08: oracle second-reader + corpus-A review) |  |
| 0xa4 | BCLGetRedValue | 0x600230 | 0x7521a8 | 88 (+0x160) | none | 3 | c → int((c >> 24) & 0xff) [0x600230] | "cant access red value of anything but a color" (c.type ≠ 0x15, so a 0x14 colour from 0x49 errors) [0x600230] |  |
| 0xa5 | BCLGetGreenValue | 0x6002a0 | 0x7521b8 | 89 (+0x164) | none | 3 | c → int((c >> 16) & 0xff) [0x6002a0] | "cant access green value of anything but a color" (c.type ≠ 0x15, so a 0x14 colour from 0x49 errors) [0x6002a0] |  |
| 0xa6 | BCLGetBlueValue | 0x600310 | 0x7521c8 | 90 (+0x168) | none | 5 | c → int((c >> 8) & 0xff) [0x600310] | "cant access blue value of anything but a color" (c.type ≠ 0x15, so a 0x14 colour from 0x49 errors) [0x600310] |  |
| 0xa7 | BCLGetAlphaValue | 0x600380 | 0x7521d8 | 91 (+0x16c) | none | 3 | c → int((c >> 0) & 0xff) [0x600380] | "cant access alpha value of anything but a color" (c.type ≠ 0x15, so a 0x14 colour from 0x49 errors) [0x600380] |  |
| 0xa8 | BCLUnused | 0x60be60 | 0x7521e8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xa9 | BCLUnused | 0x60be60 | 0x7521f8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xaa | BCLUnused | 0x60be60 | 0x752208 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xab | BCLUnused | 0x60be60 | 0x752218 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xac | BCLUnused | 0x60be60 | 0x752228 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xad | BCLUnused | 0x60be60 | 0x752238 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xae | BCLUnused | 0x60be60 | 0x752248 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xaf | BCLUnused | 0x60be60 | 0x752258 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xb0 | BCLConvertToString | 0x606f60 | 0x752268 | 92 (+0x170) | none | 122 | a → ConvertVariant(a, 0x16) (0x60b9d0, call 0x606f96); a popped without dispose (0x606f78) and disposed after the conversion (0x606fab); int → "%d" (0x60bac6–0x60bad8), float → "%f" of the f32 widened exactly to double (fld dword, fstp qword, 0x60bb33–0x60bb37; libc, 6 decimals), 0x11 → dynamic copy (0x60ba9f), 0x16 → deep copy (Duplicate 0x60ba4c); any other source → {type 0, value 0} (0x60bba0) [0x606f60] | none: ConvertVariant's only error ("Requested variant conversion unimplimented…") needs a target other than 0x10/0x12/0x16 [0x606f60] | Type 0 is not a VM type elsewhere: converting nothing, an object or a colour yields it silently (falsifier: IsString(ConvertToString(nothing)) → 0, no error). '%f' formatting is libc's: UNVERIFIED until a trace. |
| 0xb1 | BCLConvertToFloat | 0x606fe0 | 0x752278 | 93 (+0x174) | none | 6 | a → ConvertVariant(a, 0x12) (0x60b9d0, call 0x607016); a popped without dispose (0x606ff8) and disposed after the conversion (0x60702b); int → (float)int: the int is loaded exactly by fild and rounded once by fstp dword (0x60bd53–0x60bd55), round-to-nearest-even; 0x11/0x16 → atof(text) (a double) rounded once by fstp dword (0x60bd0d–0x60bd13; the following fld/fstp dword pair is a no-op) (a 0x11 slot is first replaced by a dynamic string, 0x60bce9–0x60bd02, which the handler then disposes with a); float → itself (Duplicate); any other source → {type 0, value 0} (0x60bd83) [0x606fe0] | none (as 0xb0) [0x606fe0] | Falsifier (unchanged meaning, a guard): `16777217` → 16777216.0 (one round-to-nearest-even; 16777219 → 16777220.0). |
| 0xb2 | BCLConvertToInteger | 0x607060 | 0x752288 | 94 (+0x178) | none | 67 | a → ConvertVariant(a, 0x10) (0x60b9d0, call 0x607096); a popped without dispose (0x607078) and disposed after the conversion (0x6070ab); float → TRUNCATION toward zero (ConvertVariant asm 0x60bc43–0x60bc6f: fld dword, fnstcw, or 0x0c00 = chop, fldcw, fistp qword, low 32 bits kept): a float with 2^31 ≤ \|trunc(x)\| < 2^63 wraps mod 2^32 (3.0e9 → −1294967296, −3.0e9 → 1294967296), and a NaN or \|x\| ≥ 2^63 stores the integer indefinite 0x8000000000000000, whose low 32 bits are 0 (invalid-operation exception masked, the default; UNVERIFIED at run time); 0x11/0x16 → atoi(text) (0x60bc0d); int → itself; any other source → {type 0, value 0} (0x60bca0) [0x607060] | none (as 0xb0) [0x607060] | Falsifiers: 2.7 → 2 and −2.7 → −2 (Ghidra's ROUND() and the consult's 'ROUND' are misreadings of the chop); 3.0e9 → −1294967296 (low 32 bits of 3000000000). Side effect: the handler then forces the x87 rounding field to nearest (and 0xf3ff, 0x60bc60), whatever it was before. |
| 0xb3 | BCLUnused | 0x60be60 | 0x752298 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xb4 | BCLUnused | 0x60be60 | 0x7522a8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xb5 | BCLUnused | 0x60be60 | 0x7522b8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xb6 | BCLUnused | 0x60be60 | 0x7522c8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xb7 | BCLUnused | 0x60be60 | 0x7522d8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xb8 | BCLIsInteger | 0x6070e0 | 0x7522e8 | 95 (+0x17c) | none | 0 | a → int(a.type = 0x10); a disposed [0x6070e0] | none [0x6070e0] |  |
| 0xb9 | BCLIsFloat | 0x607140 | 0x7522f8 | 96 (+0x180) | none | 0 | a → int(a.type = 0x12); a disposed [0x607140] | none [0x607140] |  |
| 0xba | BCLIsString | 0x6071a0 | 0x752308 | 97 (+0x184) | none | 6 | a → int(a.type = 0x11 or 0x16); a disposed [0x6071a0] | none [0x6071a0] |  |
| 0xbb | BCLIsAnObject | 0x607220 | 0x752318 | 98 (+0x188) | none | 2 | a → int(a.type = 1 (script object)); a disposed [0x607220] | none [0x607220] |  |
| 0xbc | BCLIsGameObject | 0x607280 | 0x752328 | 99 (+0x18c) | none | 0 | a → int(0x20 ≤ a.type < 0xff (game object)); a disposed [0x607280] | none [0x607280] |  |
| 0xbd | BCLIsArray | 0x6072f0 | 0x752338 | 100 (+0x190) | none | 5 | a → int(a.type = 5 (a 0x13 string table → 0)); a disposed [0x6072f0] | none [0x6072f0] |  |
| 0xbe | BCLUnused | 0x60be60 | 0x752348 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xbf | BCLGetObjectClassID | 0x607350 | 0x752358 | 101 (+0x194) | none | 0 | o → int; type 1 → the object's classID (u16 at ScOSIObject+0x14 = the 0x13 class index); game type → type + 0x10000; o disposed [0x607350] | "Cant get classID of non object" [0x607350] | 0 uses. |
| 0xc0 | BCLUnused | 0x60be60 | 0x752368 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xc1 | BCLUnused | 0x60be60 | 0x752378 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xc2 | BCLUnused | 0x60be60 | 0x752388 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xc3 | BCLUnused | 0x60be60 | 0x752398 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xc4 | BCLUnused | 0x60be60 | 0x7523a8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xc5 | BCLUnused | 0x60be60 | 0x7523b8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xc6 | BCLUnused | 0x60be60 | 0x7523c8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xc7 | BCLUnused | 0x60be60 | 0x7523d8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xc8 | BCLUnused | 0x60be60 | 0x7523e8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xc9 | BCLUnused | 0x60be60 | 0x7523f8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xca | BCLUnused | 0x60be60 | 0x752408 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xcb | BCLUnused | 0x60be60 | 0x752418 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xcc | BCLUnused | 0x60be60 | 0x752428 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xcd | BCLUnused | 0x60be60 | 0x752438 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xce | BCLUnused | 0x60be60 | 0x752448 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xcf | BCLUnused | 0x60be60 | 0x752458 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xd0 | BCLUnused | 0x60be60 | 0x752468 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xd1 | BCLUnused | 0x60be60 | 0x752478 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xd2 | BCLUnused | 0x60be60 | 0x752488 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xd3 | BCLUnused | 0x60be60 | 0x752498 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xd4 | BCLUnused | 0x60be60 | 0x7524a8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xd5 | BCLUnused | 0x60be60 | 0x7524b8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xd6 | BCLUnused | 0x60be60 | 0x7524c8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xd7 | BCLUnused | 0x60be60 | 0x7524d8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xd8 | BCLUnused | 0x60be60 | 0x7524e8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xd9 | BCLUnused | 0x60be60 | 0x7524f8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xda | BCLUnused | 0x60be60 | 0x752508 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xdb | BCLUnused | 0x60be60 | 0x752518 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xdc | BCLUnused | 0x60be60 | 0x752528 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xdd | BCLUnused | 0x60be60 | 0x752538 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xde | BCLUnused | 0x60be60 | 0x752548 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xdf | BCLUnused | 0x60be60 | 0x752558 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xe0 | BCLUnused | 0x60be60 | 0x752568 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xe1 | BCLUnused | 0x60be60 | 0x752578 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xe2 | BCLUnused | 0x60be60 | 0x752588 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xe3 | BCLUnused | 0x60be60 | 0x752598 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xe4 | BCLUnused | 0x60be60 | 0x7525a8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xe5 | BCLUnused | 0x60be60 | 0x7525b8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xe6 | BCLUnused | 0x60be60 | 0x7525c8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xe7 | BCLUnused | 0x60be60 | 0x7525d8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xe8 | BCLUnused | 0x60be60 | 0x7525e8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xe9 | BCLUnused | 0x60be60 | 0x7525f8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xea | BCLUnused | 0x60be60 | 0x752608 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xeb | BCLUnused | 0x60be60 | 0x752618 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xec | BCLUnused | 0x60be60 | 0x752628 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xed | BCLUnused | 0x60be60 | 0x752638 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xee | BCLUnused | 0x60be60 | 0x752648 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xef | BCLUnused | 0x60be60 | 0x752658 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xf0 | BCLUnused | 0x60be60 | 0x752668 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xf1 | BCLUnused | 0x60be60 | 0x752678 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xf2 | BCLUnused | 0x60be60 | 0x752688 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xf3 | BCLUnused | 0x60be60 | 0x752698 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xf4 | BCLUnused | 0x60be60 | 0x7526a8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xf5 | BCLUnused | 0x60be60 | 0x7526b8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xf6 | BCLUnused | 0x60be60 | 0x7526c8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xf7 | BCLUnused | 0x60be60 | 0x7526d8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xf8 | BCLUnused | 0x60be60 | 0x7526e8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xf9 | BCLUnused | 0x60be60 | 0x7526f8 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xfa | BCLUnused | 0x60be60 | 0x752708 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xfb | BCLUnused | 0x60be60 | 0x752718 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xfc | BCLUnused | 0x60be60 | 0x752728 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xfd | BCLUnused | 0x60be60 | 0x752738 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xfe | BCLUnused | 0x60be60 | 0x752748 | 102 (+0x198) | n/a | 0 | none: errors before any change; PC not advanced [0x60be60] | "Call of undefined instruction" (always) [0x60be60] | osi.py also rejects it (unknown opcode). 0 uses. |
| 0xff | BCLHalt | 0x603840 | 0x752758 | 7 (+0x1c) | none | 0 | → nothing ; PC+=1, handler returns false so Run (0x60b850) stops [0x603840] | none [0x603840] | Pushes a nothing variant {0x0f,0} (not 'pushes nothing'); does not unwind to a frame marker. 0 uses in Base.osi. |
<!-- END opcode-table -->
