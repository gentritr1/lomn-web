# LOMN Web — M2 design spec (script VM to the main menu)

Date: 2026-10-08 · Status: approved by the controller in autonomous mode (owner directive: proceed without confirmations; owner-gated items are listed in §9) · Parent spec: `2026-10-07-lomn-web-port-design.md` (§9 M2, §12 A11, §13) · Branch: `m2-osi-vm`, cut from `m1-integration` @ `abe821f`.
Evidence: Fable 5.1 design consult `docs/knowledge/research/2026-10-08-m2-osi-vm-consult.md` (EXECUTED/INFERRED/ESTIMATE labels carried over). The controller re-measured the load-bearing counts with its own script (§2, "re-checked").

## 1. Goal

Run the game's own script, `Base.osi`, in our engine **the way the original VM does**, from `Setup` to an interactive main menu: the `lev0/frnt` area as backdrop, HUD sprites and tile-font text over it, keyboard and mouse input reaching the script, natively first and then in the browser.

Behaviour is defined by the cited decompiled functions and by the binary. It is reproduced, not reinvented.

**Non-goals (M2):** Bink/AVI video (M4), audio output (M4), saves beyond reading `Bionicle.ini` and the default save state (M4), character/collision/camera gameplay (M3), REBUILT/LOMNHook content (M7), any gameplay level past the menu.

## 2. Measured facts

Re-checked by the controller (EXECUTED 2026-10-08, own script `m2verify.py`, not the consultant's):

| Fact | Value |
|---|---|
| Script files | one: `Base.osi`, 486,274 B, OSI 4.1 |
| Instructions / distinct opcodes used | 151,522 / 67 |
| Natives called (`class.func`, case-folded) / native classes | 728 / 64 |
| Opcodes with a real handler / `BCLUnused` | 96 / 160 (template `0x751768`, 16 B per entry, through the VM vtable `0x7514e4`) |
| Handler table `0x752768` in the file | all zero (filled by a `.CRT` initializer at run time) |
| `RegisterFunction` / `RegisterType` call sites in `.text` (`E8` scan) | 1,056 / 445 |
| `RegisterFunction` call sites in the decomp corpus | 135 (in 6 functions; the first version said 134, corrected by probe 1 and re-counted: 139 matches − 4 in the definition file) |

From the consult and probe 1: 47 of 391 `.CRT` initializers are in the corpus; the 215 `&LAB_` targets are 126 OSI natives, 86 emitter callbacks and 3 others. **Probe 1 (re-checked by the controller):** a seeded Ghidra run encloses all 1,056 registrations in 70 functions (6,944 functions exported); see `docs/knowledge/research/2026-10-08-m2a0-probe1-seeding.md`. Also from the consult: 186 distinct natives are called by the 38 front-end classes; 365 are reachable from the four entry points with level entry excluded; 3,161 `in_stack`/`in_EAX` artefacts, 205 in the VM and 274 in `CreateVariant`. M2a-0 re-measures them.

## 3. What the original does (summary; details and addresses in the consult §1.2)

1. **Boot:** `GcGame::DoSetUp` → … → `GcGame::SetUpScripting`: create the VM from `Root/Data/base.osi`, call the script's `Setup`, map controller one to the script, then add a per-frame `ScSLOSIProcess` (priority 2) that calls `Process(float time, float dt, int frame)` (probe 3: time first; the consult had dt first). A false return re-runs `Process` in the same frame, up to 10 extra passes; nothing stops it.
2. **Entry points:** `Setup` (once), `Process` (every frame), `Input` (key mapper, analog triggers), `OnEvent` (mouse, timers).
3. **VM:** a stack machine with 8-byte tagged variants, a 1,000-slot stack, frame-base markers (`0xff`) for engine→script calls and return markers (type 2) for script→script calls; re-entrant (69 engine functions call into the VM).
4. **Natives:** registered at static-init time by (class, name) token pairs with min/max argc and 10 parameter types. Arguments are converted or boxed by reference (`0x200`). The native ABI is `ScOSIVariant fn(VM*, u32 × 10)`. The call instruction **self-patches** to `0x84` plus a direct pointer.
5. **Menu path:** `setup` → `global_class` (4 tile fonts, `userealdeltatime(1)`) → front-end screen → `lego_level_frontend.initfull` (3 intro movies via `gchuddirector.playcinematic`) → area `frnt` (fog, lighting, music) → `gs_lego_screenfrontend` (cursor sprite, camera spline `frnt_CAM.slb`, language) → menu screens.
6. **Fonts:** `.col` collective volumes of per-glyph TGAs plus `.col.db` names and a text `.info` (metrics and charset).
7. **HUD:** ordinary geometry under a HUD transform: FVF `0x42` (XYZ | DIFFUSE) for lines and rectangles, textured quads for sprites and text. No pre-transformed (RHW) vertices.

## 4. Decisions (rulings, recorded in the ledger)

- **D1 — Intro movies.** `playcinematic` completes instantly behind `--skip-video` (default **on** in M2; the state is reported in metrics and on `window.__lomn`). Bink stays in M4. *Cost if wrong:* a flag flip.
- **D2 — Unregistered natives.** A native the original registers is never "unregistered" in ours: every one of the 1,056 registrations exists, as a stub or a binding. A name the original does not register raises the original's error path (logged error, the call yields nothing). Whether `bettersaver.*` is registered by this exe is decided by the completed registry (A3), not by assumption. **Amended 2026-10-08 (M2a-0 T3, controller-checked):** the owner's install is the community **LOMN Beta 1.5.0** patch (`edition.txt`), with mod DLLs in `install/mods`. Its `Base.osi` calls **78** `class.func` pairs that the exe never registers. `BetterSaver.dll` names `bettersaver.{get,set}booleanvalue`/`save` and `OSIProject.DebugServer.dll` names `lsdebugger.*` (their strings, read as data). The other 71 (`gclegosounddirector` 33, `gcwavesound` 11, `gclegolite` 10, …) exist in neither the exe nor the mods, so the original raises "Call to unregistered Game Function" if it reaches them. Ruling: the port targets the owner's install. The 7 mod-provided natives become a documented **Beta 1.5.0 mod tier**: their behaviour is taken from the DLLs read as data, never run, and each is cited. The other 71 keep the original's unregistered-error path. The A8/A10 trace shows whether the menu reaches any of them.
- **D3 — Deterministic mode.** `scprocessmanager.userealdeltatime` is a logged no-op under `--deterministic`, which leaves the original's own fake-time path on: time += 0.0066667 per pass, dt = 0.0066667 (`0x712d68`). Real-time mode reproduces the original smoothing: dt = 0.07·raw + 0.93·prev, 0.016666 when raw > 0.5, capped at 0.066 (`0x4a9170`).
- **D4 — Self-patching.** Our VM memoises the resolved native per call site and never writes into the script image. The oracle runs on a pristine image. Traces print the logical opcode (`0x82`) on every execution.
- **D5 — Compiler finding.** The exe mixes Metrowerks CodeWarrior x86 V2.3 (38 compile records) and MSVC (29) objects (consult §1.5). The parent spec's MSVC6 x87 assumption is re-checked per module in M3. M2 is not affected: VM floats are `float` variant slots.
- **D6 — No thresholds before measurement.** Caps, perf budgets and corpus-completion targets are written as "measured value, with the gap listed", never as invented percentages.

- **D7 — Handles, never pointers** (M2a-1). Every heap value a variant holds (script object, array, dynamic string, game object) is a `uint32_t` handle from a per-kind monotonic counter starting at 1, never reused. Identity (0x60 on type 1 and on game types) is handle equality; traces print handles. This makes native, wasm32 and Python allocate identical ids in the same order and removes the 32-bit-pointer risk.
- **D8′ — x87 precision control is VM state `fpu_pc` ∈ {24, 64}** (M2a-1; amended 2026-10-08, replacing D8 "round the exact result once to f32", which assumed the precision never changes). Evidence (M2a-1 T0 probe, EXECUTED on the exe as data; reachability INFERRED from the static call graph, the controller re-read 0x65f886):
  - **Initial 24.** `CreateDevice` without `D3DCREATE_FPU_PRESERVE` (M1 R23, `CreateDirectXWindow` 0x52e570) sets 24-bit precision before `SetUpScripting`. INFERRED; UNVERIFIED if `d3d8.dll` or another runtime path changes it before `Setup`, and UNVERIFIED whether `d3d8.dll` or the driver re-applies it later (an A6 risk; the scan covers the exe only).
  - **64 for ever after the first string → float conversion.** MSL's `___dec2num` (0x65f80a) does `or word [ebp−0xe], 0x300; fldcw` (0x65f886–0x65f88c) and returns without restoring. Its only live path is `_atof` (0x65c7b0), called by `ConvertVariant` at 0x60bd0d (0xb1 on a string; a float-declared native argument given a string) and by `GcScriptTools::StringToFloat` at 0x5654bc (the native `gcscripttools.stringtofloat`, flagged `sets_fpu_pc64` in the registry). The switch happens whenever `_atof` runs, even if the text parses to 0, except when a decimal exponent beyond ±32767 takes `___strtold`'s overflow path (0x65c539–0x65c65f, INFERRED), which skips `___dec2num`.
  - **64 after an error.** The C++ exception runtime runs `fninit` before every landing pad (0x657068, 0x657245); in the original every VM error ends the game, in ours (D10) the recorded error sets 64.
  - Every other control-word writer in the exe leaves the precision bits alone: of 499 `fldcw`, 472 are the float → int idiom, 26 form 13 save/constant/restore pairs in the math library, 1 is `___dec2num`'s; there is no `_controlfp`/`_control87`.
  - **Semantics:** every f32 result of 0x70–0x73, whatever the operand types, = round_f32(round_p(exact)), p = `fpu_pc` bits, nearest-even, x87 extended exponent range at the round_p step. At p = 24 that is double-rounded when the result is an f32 subnormal and the op is float × float or ÷ (+, − and int × float results are multiples of 2^−149, exact at that step); measured on 20,000 uniform f32 pairs per op: p 24 vs one rounding + 0, − 0, × 27, ÷ 18; p 64 vs one rounding 0 (an earlier "float,float unaffected, 0/160,000" claim was wrong: it never compared p 24). Compares are unaffected. The `(float)((double)a OP (double)b)` formula is the 53-bit setting and is not the original. Falsifiers: `16777217 1.0e-15 +` → `4b800001` at 24 and `4b800000` at 64, and `16777217 1.0e-10 +` → `4b800001` at 64 vs `4b800000` at 53, run before and after a 0xb1 on a string; `f32:0x24368a23 × f32:0x1af99066` → `002c7cd4` at 24 vs `002c7cd5` with one rounding. The trace records the switch as `P <pc> 64` (`docs/osi/trace.md`). Any exact method is acceptable (the C++ VM uses exact wide-integer arithmetic; Python uses `fractions`); only the result is specified.
- **D9 — NaN canonicalisation.** Any NaN f32 result is stored as `0xffc00000`, the x87 real indefinite (UNVERIFIED on this exe).
- **D10 — Errors.** Inside the VM an error is a C++ exception that unwinds through handlers and natives. The outermost `Vm::call` records the error, writes the `E` trace line and returns `ok=false` without touching VM state, so `snapshot()` shows the state that "After an error" documents. `reset_after_error()` is our own host behaviour. We add two errors the original does not have: "Stack overflow (1000 slots)" and "Stack underflow".
- **D11 — Typed stubs.** The registry gains a generated `ret` column: the `CreateVariant` overload each body calls. A stub returns nothing, int 0, float 0.0, "", colour 0, a fresh game handle of the overload's class, or a copy of argument 0, as that column says. A per-row `default` overrides it. Without typed stubs, `Setup` dies at its 5th native call (`sctilefont.setcolor` takes a `type:ScTileFont` argument, controller-checked).
- **D12 — The 7 Beta 1.5.0 mod natives are registered as stubs in M2a-1** (`data/osi/natives_beta150.json`). Their signatures are read from the DLLs as data, never by running them. Two of them sit on the static `Setup` path. Their bodies come in M2a-2.

- **D13 (proposed), game objects.**
  - `CreateVariant(T*)`: null → nothing; otherwise the object's reference count goes up by 1 (0x445550: `inc dword [edi+4]` at 0x445580).
  - The handle is minted when the object first passes through `CreateVariant` (`A g` at that moment), not when it is constructed.
  - The count stored with the handle is the engine's references plus the variants' references.
  - `Dispose` → −1; at 0 → `F g`, and the host object is deleted (the original's virtual delete).
  - The same live object always maps to the same handle.
  - Each class's initial engine count is cited by Task 2a from its constructor.
  - "45 overloads" means the 45 distinct `ret_class` values over the 57 game rows: 56 winning registrations plus `gchudsprite.create` 0x5366d0, which is not the winner. Task 2a mapped each class to its own overload address: 45 classes, 45 addresses (`docs/osi/natives.md` §2.1).
  - **Amended (M2a-2 T2a, controller ruling, round 3):** identity is per object. One handle per live object, whatever the static type; the variant's type tag follows the native's declared return (the same Toa is returned as `GcToa` and as `GcCharacter`). `GcBAGroup::SetCharacter` adds an engine reference (0x54b344). Initial counts and holders: `docs/osi/natives.md` §2.
- **D14 (proposed), natives.log v2,** an amendment to `trace.md` §6; the trace format itself does not change.
  - **New records:**
    - ` G %d` after every game value (a result or a `W` write): the references the engine holds on that object after the native returns, **not counting this variant**. The heap stores total = engine + variants.
    - `C %08d <name> <a0>…<a9>`: native seq N starts an engine → script call at this point.
    - `K %6d` at the start of each frame.
    - **Reserved** (C3): `C -1 …` (a host-originated call) and `H <type>:<handle> %+d` (a reference change on the host side, outside any native). No M2a-2 code writes them unless Task 2a §6 finds a host source on the 600-frame path. Their rules are fixed now so they are not invented later.
  - **Sequence numbers** are assigned at native **entry**. Lines are written in event order.
  - **Order inside one native** (as the conformance runner already does, `lomn_osi_conform.cpp:12-14`): nested `C` calls, then `W` writes, then the return.
  - **Replay rules (I6):**
    - *Identity.* If the logged handle is live, the replay returns it with one more variant reference and writes no `A g`. Otherwise the logged handle must equal the next handle, and it is minted with `G` engine references.
    - *`C N` records.* The replay performs the nested call at that point, inside native N.
    - *`C -1` records.* The replay **never performs** one. Host-originated calls are host behaviour, implemented in both engines. The replay **asserts** that its own host's next call matches the next `C -1` record (name and arguments), the same way it asserts `cls.fn/argc`. `H` is asserted the same way.
  - **Header hash (C1).** The overrides merge changes `natives.json`, and therefore the `natives=` hash in the trace header and the digest's `T` line, but not the trace. Digest guards compare **`D` lines only**: `grep '^D' a | cmp - <(grep '^D' b)`. Headers are compared with the `natives=` field stripped: `diff <(grep '^#' a | sed 's/natives=[0-9a-f]*//') <(grep '^#' b | sed 's/natives=[0-9a-f]*//')`.
- **D15 (proposed), the nesting cap after re-measurement.**
  - Keep 64 while 64 ≥ 4 × the measured maximum M; otherwise use 4 × M.
  - `STACK_SIZE` ≥ 4.5 × cap × the bytes per level, measured on a **real** re-entrant chain. 4.5× is the M2a-1 margin.
- **D16. The swallowed nested error.**

  **Scope.** Errors raised by engine calls made inside the three swallowing blocks of `GcAreaLoader::LoadArea` 0x4fedd0 are swallowed: `LoadObjects` (0x4ff93e), `LoadCharacters` (0x5002ec) and `LoadHives` (0x500c0b; it lies past the decomp's `size`, LoadArea really ends at 0x500fe4) (`docs/osi/natives.md` §5.2). `LoadArea` is reached from:
  - the natives `gcareadirector.setcurrentarea` (`SetCurrentArea` 0x48a540 → `GcAreaDirector::LoadArea` 0x4892f0 at 0x48a588 → 0x4fedd0 at 0x48935a), `gcareadirector.switchcurrentarea` and `gcareadirector.switchareas`;
  - the host process `GcCollisionPhysicsGroup::Process` 0x475fc0 → `SwitchCurrentArea` 0x48a5a0 (0x4763d7) → `GcAreaDirector::LoadArea` (0x48a727);
  - the host process `GcBAGroup::Process` 0x54ab00 → `GcBAGroup::CheckForNew` 0x54b380 (0x54b071, in the tail past the decomp's `size`) → `GcAreaDirector::SwitchAreas` 0x48a760 (0x54b4d1) → `GcCollisionPhysicsGroup::SwitchAreas` 0x476cf0 (0x48a78d) → `SwitchCurrentArea` (0x476d08). `CheckForNew` makes that call only while byte `[0x735aec]` = 1 (0x54b4a1). Only `GcBAGroup::SetSwitchAreas` writes 1 there (0x54b24e), and it is 0 in `.data`.

  No other VM-reaching call site sits inside a catch. The true-extent census (natives.md §5.1) finds 5 caught sites: these 3 and the 2 host roots.

  1. **At the throw.** A nested `Vm::call` that raises leaves the VM state exactly as the throw found it:
     - the stack, including the inner `{0xff, PC_ret}` marker (0x60b484), the argc arguments pushed by `SetPCAndArgs`, and the partial frame;
     - `context_depth` +1 (0x60b4b8; the decrement at 0x60b562 never runs);
     - `pc` = the failing handler's PC at the throw, as **the error-PC table in opcodes.md (T2a-pc)** gives it;
     - `frame_ptr` as the failing frame set it.

     No reset runs. `Call`, `CallWithReturn` and `Run` hold only no-op destructor records (0x413ec0, `mov eax, ecx; ret`), so unwinding through the VM changes no VM state. `fpu_pc` becomes 64 (the catch pad's `fninit`, 0x657245). The swallowing native catches the error inside its body and returns its normal result. The VM's own reference count (+2, from 0x60b338 / 0x60b46b) is not modelled.
  2. **The `E` line of a swallowed error (port rule; the `E` line is a trace artifact).**
     - The original records nothing that can be seen. Each pad stores `{error, qualifier}` in `LoadArea`'s own locals, and nothing reads that pair again (natives.md §5.2).
     - The port writes the swallowed error's `E` line **at the swallow point**, in the swallowing host's catch (the conformance runner for a `swallow` test native; the modelled `LoadArea` block for the real natives). The line is built from the VM state as at the throw: `pc` and `op` are the failing instruction's own address and byte (as for every `E` line), and `depth` is the stack depth at the throw. A `P <pc> 64` line follows it if `fpu_pc` was 24.
     - Unwinding runs no VM code, so when the failing instruction raised the error itself, this `E` line directly follows that instruction's `I` line. When the swallowed error is a 0x82/0x83/0x84 re-raise after its own nested call raised (rule 15(b) of conformance.md), the `E` line follows the last record of that inner call instead: its `pc` and `op` are the re-raising call's, and `depth` and `top` are the stack at the throw (amended in M2a-2 T2b; corpus `n-reenter-nested-error-swallowed`).
     - The swallowing catch does **not** set the outermost call's `last_error`, does **not** run `reset_after_error()` and ends no engine call. The outermost `Vm::call` returns `ok = true` unless another error escapes it.
     - This amends D10's "the outermost `Vm::call` writes the `E` line" for swallowed errors only. A nested error that is **not** swallowed is still recorded exactly once, by the outermost `Vm::call` (one `E` line, `ok = false`). The nested calls it passes through record nothing.
  3. **The calling handler** (0x82, 0x83, 0x84), after a swallowing native returns:
     - **By-reference writes land in the native's original argument slots, by address.** For a parameter declared 0x200, the handler gives the native the **address** of the argument's stack slot. That address is computed before the native call from the top pointer VM+0x1f50: slot = top − (n − i), at 0x601804–0x601819. The handler boxes it as `{0x200, address}` (0x60185d / 0x601862) and passes the address itself, `*(box+4)` (0x60198c–0x601abc; opcodes.md rows 0x82, 0x83 and 0x84). After a swallow those slots lie **under** the leftovers, below the inner marker. A by-ref write therefore goes to the slot the handler addressed before the nested call, never to top − (n − i) recomputed after it. As conformance rule 14(d) says, a write overwrites the slot without disposing the old value.
     - Then the handler pops n slots with dispose **from the current top** (0x601c50–0x601c6b). After a swallow these are n inner leftovers, never the written slots. The written values stay where they are, in the leftover region.
     - It pushes the result (0x601c8d).
     - For 0x82 only, it self-patches the **outer** instruction through the start pointer it saved at entry (0x6015b8; written at 0x601e2b–0x601eaa).
     - It **does not change `pc`**: 0x82 never writes VM+0x1f58 after the native call.

     `Run` re-reads `pc` every step (0x60b856), so dispatch continues inside the inner function at the error PC.
  4. **A later `0x52`** that meets an `0xff` marker stops the running `Run` loop (0x600d54–0x600d87). The engine call that owns that loop then:
     - pops the result (0x60b505);
     - pops with dispose to the first `0xff` marker (0x60b53e–0x60b560), which is the inner one;
     - decrements `context_depth` (0x60b562), and rescans `frame_ptr` if `context_depth` is then not 0 (`cmp word [edi+0x1f4c], 0` at 0x60b569, `je` 0x60b571; the scan runs to 0x60b59b);
     - sets `pc` from that marker (0x60b5a2);
     - returns the inner function's value.

     The rest of the outer function never runs in that engine call.
  5. **Nothing else resets.** Leftover slots stay under the top until something pops them, and `context_depth` stays raised for the rest of the run.
  6. **`reset_after_error()` after a later error (port rule; host behaviour, visible in traces through its `F` lines).**
     - In the original, an error that reaches a host root ends the game (opcodes.md "After an error"), so there is no original reset to cite.
     - In the port, when an error escapes the outermost `Vm::call` (D10), `reset_after_error()` pops with dispose from the top **down to and including that outermost call's own marker**. That marker is the slot at the stack depth the call recorded at its entry, before it pushed the marker. The reset emits the `F` lines of the heap values it frees.
     - It does **not** stop at the first `0xff` it meets. Inner markers left by a swallow (as in `n-swallow-reerror`) are popped like any other slot.
     - It then restores `context_depth`, `frame_ptr` and `pc` to their values at that call's entry.
     - Leftovers below that entry depth, left by an **earlier** successful call, are not touched.
     - Conformance rule 14(c)'s "the call's outer marker" means this marker.

  **Test-native field `"swallow": true`.**
  - The native makes its `calls` nested call. If that call raises, the runner applies rule 1 inside the native and writes the `E` line there (rule 2).
  - Then the native applies its `writes` to its original argument slots, by address (rule 3), and returns its `returns` normally. The handler applies rule 3.
  - The expectations record the outer `Vm::call` result and the post-call snapshot: `depth`, `top`, `pc` and `context_depth`. `pc` is compared under conformance rule 15 as amended by T2a-pc.

  **Corpus programs** (Task 2b writes the files):
  - **`n-swallow-continue`.**
    - Setup: `main` makes a game-variable write W1, calls T (`swallow`, n = 0, `calls` `onevent`, `returns` 5), then makes a write W2 and returns 1. `onevent` executes `0x02` (which advances PC by 1 before its error, 0x6038a6), then pushes 7 and returns.
    - Expect: `main`'s result is **7**; W1 is written and W2 is not; `context_depth` after the call is 1; exactly one `E` line, written inside T. A second `call("main2")` succeeds above the leftovers.
  - **`n-swallow-reerror`.**
    - `onevent` executes 0x85 (`BCLUnused`, which does not advance, 0x60be60). It re-executes after the swallow and errors again inside `main`'s `Run`.
    - Expect two `E` lines with the same pc: the first written inside T (rule 2), the second by the outermost call. The result is `ok = false`.
    - `reset_after_error()` then returns the stack to the outermost call's entry depth: through the inner leftover marker, down to and including the outer one (rule 6).
  - **`n-swallow-pops`.** T has n = 2. `onevent` (argc 2) holds 2 locals when its `0x02` fails. The 2 pops remove those locals, not T's arguments. `onevent` then returns T's result. T's arguments stay at the bottom of the leftover region.
  - **`n-swallow-writes`.**
    - Setup: T has n = 1 and one parameter declared 0x200 (by reference). `main` pushes int 3 as T's argument and calls T (`swallow`, `calls` `onevent` with the argument int 4, `writes` argument 0 ← int 9, `returns` 5). `onevent` (argc 1) executes `0x02`, which fails with its argument int 4 on top, then pushes 7 and returns.
    - Expect: `main`'s result is **7**. The handler's single pop disposes `onevent`'s leftover int 4. After the call the top is **int 9**, in T's argument slot, and `context_depth` is 1.
    - A write addressed from the top after the nested call would land on the int 4. The pop would then remove the 9, and the top would be int 3, so the program tells the two readings apart.

## 5. Oracle

The renderer had an independent Python decoder plus `native == wasm32` state logs. The VM's equivalent (consult §3):

1. **Citation table** (`docs/osi/opcodes.md`): one row per opcode giving the handler address, the template entry, the operand format, the stack effect and the error paths. It is generated from the binary where possible (handler mapping) and authored from the cited decomp otherwise.
2. **Two independent interpreters**:
   - C++ `lomn::osi::Vm`, written from the table;
   - Python `tools/research/osi_vm.py`, written by a *different* agent from the table **and the cited functions**, never from the C++.
3. **Falsifier per row:** every row has at least one crafted conformance program whose expected result differs under the most plausible alternative reading. Examples: `0x50` is script-relative; a float fed to `CompareAndBranchIfFalse` raises an error rather than branching.
4. **Differential traces** on the real `Base.osi`:
   - Must be byte-identical across native, wasm32-under-Node and Python.
   - Each trace line records PC, opcode, stack depth, top type/value, native name, args and return, and alloc/free events.
   - The Python side **replays the native return log** of the C++ run, so any disagreement is VM semantics.
5. **Ground truth for natives** (later, owner-gated): a trace from the original game (LOMNHook trace mod).

Known blind spots (stated, not hidden):
- a handler both readers misread the same way;
- engine-state opcodes (`0x80/0x81`, `0x18–0x1a`), which are natives in disguise;
- libc `%f` and `ROUND` behaviour, tagged UNVERIFIED until a trace from the original.

## 6. Natives

The 1,056-row registry (`data/osi/natives.json` (committed; was `gen/osi/natives.json` before the M2a-0 final-fix ruling): class, name, body address, min/max, 10 types) is generated from the completed corpus. A generator emits a typed, logging stub for each row. Each row is in one of three tiers:

- **Tier 1, implemented:** cites its wrapper body and the method it calls; worked in the order the trace first hits them.
- **Tier 2, stub proven safe:** a taint pass in the Python oracle shows its return value never reaches a branch, a tier-1 argument or a game-variable write on the menu path. A decided default is one line in the registry.
- **Tier 3, unknown:** hitting one on the menu path is a failure and goes on the worklist.

## 7. Sub-milestones and acceptance

Each sub-milestone gets its own plan, written after the previous one lands (its measured facts feed the next plan).

| Plan | Content |
|---|---|
| **M2a-0** Corpus completion + citation tables (no engine code) | Probes done 2026-10-08 (`docs/knowledge/research/2026-10-08-m2a0-probe{1,2,3}-*.md`): seeding works (body shapes: 5, not 1); the engine reads the binary `.fnt` inside the `.col`, never the text `.info`; input tuples are cited. Then: the seeded `make decomp` re-run (`.CRT` initializers, `&LAB_` bodies, registration callers) with the A11 `ScOSIVariant` by-value signature fix; generators for `docs/osi/opcodes.md`, `data/osi/natives.json` (committed; was `gen/osi/natives.json` before the M2a-0 final-fix ruling), `docs/osi/types.md`. |
| **M2a-1** VM core + Python oracle + conformance | 96 handlers; stack, frames, variants, strings, arrays, objects; native-call protocol with a pluggable registry; re-entrant `Call`; per-site memoisation; caps with errors (never a write past slot 1,000). |
| **M2a-2** Native framework, `Setup` to the first `Process` | Generated stubs; the natives `Setup` needs implemented; `lomn_view --script` runs `Setup` and N `Process` frames headless. |
| **M2a-3** Menu systems | Tile fonts, HUD sprites/text/rectangles through the shim (FVF `0x42`, textured quads, HUD transform, order after the area), camera spline, SDL3 input → `Input`/`OnEvent`, `Bionicle.ini` via the VFS, sound stubs, D1/D3 flags. |
| **M2a-4** Hardening | VM fuzz (bytecode and tables) under ASan/UBSan native and wasm32; caps measured on `Base.osi`; perf entries; mutation bite check. |
| **M2b** Browser | The BYO page boots to the menu; `window.__lomn.script` state; Playwright drives the menu with keys. |

| # | Acceptance (observable) | Verification the reviewer re-runs |
|---|---|---|
| A1 | The re-run corpus resolves every one of the 1,056 `RegisterFunction` call sites to `{class, name, body}`; any unresolved site is listed by address with a reason. The `in_stack`/`in_EAX` counts for `ScOSIVirtualMachine::*` and `CreateVariant` are reported before and after, with every remaining artefact listed. | `E8` scan count vs registry rows; the artefact regex counter |
| A2 | `opcodes.md` has 256 rows: 96 named handlers, 160 `BCLUnused`. Each handler address equals `vtable[0x7514e4][template[0x751768 + 16·op].offset / 4]`. | An independent re-derivation from the exe (a different script from the generator's) |
| A3 | The registry covers all 728 `class.func` pairs `Base.osi` calls; any missing pair is listed and handled per D2. *(Amended 2026-10-08: menu tagging moved to M2a-2, where the A8 run trace defines the menu set; the registry records `called_by_base_osi` only. The 186 came from a consultant's static classification.)* | A census from `tools/research/osi.py` |
| A4 | Every type id used in the registry is defined in `types.md`; the registered-type order matches the `RegisterType` call order (445 sites). | `types.md` vs the `E8` scan |
| A5 | **Conformance:** at least 2 crafted programs per handler (all 96, including the 29 that `Base.osi` never uses), each citing its row. C++ and Python both match the authored expectations. Every error path (unregistered native, too many/few args, bad branch operand type, conversion failure, stack full) is reported, not crashed. | The conformance runner, run on native, wasm32 and Python |
| A6 | **Differential:** `Setup` + 600 `Process` frames + the key sequence Down, Down, Enter, Esc, all natives stubbed. The trace is byte-identical across native, wasm32 and Python. | `cmp` of the three traces |
| A7 | **Mutation bite:** single-handler mutations of the C++ VM, each targeted at a check. Every targeted mutant is caught by A5 or A6; a surviving mutant is explained and either gets a test or is parked with a ruling. | A `mutate` script in the style of `mutate4.py` |
| A8 | `lomn_view --script --frames 1` runs `Setup` with no unregistered-native error and prints the coverage report (distinct natives hit, by tier). | The stub log |
| A9 | 600 `Process` frames with the real tier-1 natives: no VM error; the trace matches Python with native returns replayed. | A6 re-run |
| A10 | Every native hit on the A6/A9 path is tier 1 or tier 2 (taint-proven). No tier 3 is hit. | The taint report |
| A11 | The `BionicleFont` glyph set equals the `.col` directory and the charset of the packed `.fnt` (the on-disk `.info` is stale and never read by the engine). A rendered string's pixel width equals the cited `GetStrLen` formula on the metrics. | An independent Python `.col`/`.info` reader |
| A12 | **State log for one menu frame** (null backend): area draws (as M1), then HUD draws with the cited states (FVF `0x42`, textured quads, Z/alpha as decompiled). native == wasm32. | The expected sequence written from the cited render functions |
| A13 | **Visual:** the menu after the skipped intro, with the cursor on entries 1 and 2, at 640×480 and 1280×720, vs the owner's screenshots of the original. | **Owner eyeball** (gated; does not block A1–A12) |
| A14 | **Fuzz:** mutated `Base.osi` and crafted files give no crash and a reported error under ASan+UBSan, native and wasm32. The per-input time budget is set from the measured median. | The fuzz harness, as in M1a-4 |
| A15 | **Perf entries:** script ms per frame p50/p95 with the frames-vs-window reconciliation, natives per frame, stack high-water. No thresholds until measured. | `perf/history.jsonl` |
| A16 | **Browser:** the BYO page reaches the menu (`window.__lomn.state == "menu"`). Only build assets go over the network. Synthetic keys move the cursor, and the state change is read from `window.__lomn.script`. | Playwright smoke |

## 8. Risks (consult §5; each has a probe in M2a-0)

1. **The corpus gap is worse than counted:** thunks, templated wrappers, names built at run time. *Probe 1.*
2. **Variant ownership** (Duplicate/Dispose on push, pop and convert) leaks or double-frees only after thousands of frames. *Mitigation:* the Python oracle models ownership explicitly; traces carry alloc/free; ASan on the 600-frame run.
3. **The long tail of menu systems** (`ScTileFont`, a 35 kB mouse driver, a 59 kB controller map). *Probes 2 and 3:* reproduce the font metrics and the argument tuples, not the drivers.
4. **Disk space on the host** (2.9 GB free on 2026-10-08). Builds and the decomp re-run check free space first and stop below 2 GB.

## 9. Owner items

- **Screenshots of the original main menu** after the intro: 640×480 and native resolution, cursor on entries 1 and 2, and the Options screen. Needed before the M2a-3 review (A13).
- **Eyeball check on the original:** on the front-end Options screen, press Esc, then Backspace, then right-click. Probe 3 infers that only right-click goes Back on PC.
- **Informational:** D1 (intro skipped by default in M2), D2 (`bettersaver.*` handled per registry), D5 (mixed-compiler exe; acts in M3).
- **Later (M3):** a run of the LOMNHook trace mod on the original game.

## 10. Size

ESTIMATE 33–45 agent-hours (M1 took about 25), confidence medium-low (±50 %). M2a-3 carries most of the variance. The M2a-0 probes turn the widest ranges into measured ones, and the board's forecast is updated after them.
