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
