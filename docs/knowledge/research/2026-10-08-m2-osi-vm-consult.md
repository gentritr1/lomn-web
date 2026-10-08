# LOMN Web — M2 design consult: OSI VM to the main menu

Date: 2026-10-08 · Author: Fable 5.1 (design consultant, no product code, no commits) · Repo: `lomn-web-merge` @ `abe821f` (branch `m1-integration`) · Parent spec: `docs/superpowers/specs/2026-10-07-lomn-web-port-design.md` (§9 M2, §11, §12 A11, §13) · Style reference: `2026-10-07-lomn-web-m1-design.md`.

Labels: **EXECUTED** = I ran the command (named inline, scripts under the scratchpad `m2probe/`); **INFERRED** = read from the decomp corpus or data without running; **ESTIMATE** = judgement. Every number that drives a threshold is EXECUTED. The exe was only ever parsed as data (`handlers*.py` read the PE sections; nothing was launched).

Scratch scripts referenced below (all under `…/scratchpad/m2probe/`): `census.py`, `disasm.py`, `reach.py`/`reach2.py`, `menunat.py`, `natives3.py`, `handlers6.py`/`handlers8.py`/`handlers9.py`. Their outputs: `census.json`, `opcode_names.json`, `reach.json`, `reach_menu.json`, `menu_natives.json`, `natives.json`.

---

## 0. Ten-line summary

1. One script file: `install/data/Base.osi` (486,274 B, OSI 4.1): 2,749 strings, 8 globals, 23 global functions, 395 classes, 8,406 methods over 3,606 unique subroutines, 151,522 instructions, 4,402 `CallGameFunction` sites to **728 distinct natives in 64 native classes** (EXECUTED).
2. The VM is a byte-coded stack machine with 8-byte tagged variants (u16 type, u32 value), a 1,000-slot stack, frame/return markers on the stack, and a 256-entry handler table. **96 opcodes have real handlers** (160 are `BCLUnused`); Base.osi uses **67** of them (EXECUTED, table read from the binary, not from SAGESharp).
3. Natives are registered by name pairs at static-init time: **1,056 `RegisterFunction` call sites and 445 `RegisterType` sites exist in `.text`**, but only **134 registrations, 47 of 391 `.CRT` initializers and 0 of 215 native bodies are in the 5,618-function decomp corpus** (EXECUTED). M2 needs a corpus-completion step before anything else.
4. Engine→script entry points are four names: `Setup` (once, `GcGame::SetUpScripting`), `Process` (every frame via `ScSLOSIProcess`, priority 2), `Input` (`GcKeyMapper::ProcessKey`, controller triggers) and `OnEvent` (mouse, timers, events) (EXECUTED string check).
5. The main menu is a **3D area** (`fend`/`frnt` = `lev0/frnt`, already renderable by M1) plus HUD sprites, tile-font text, a camera spline (`frnt_CAM.slb`), key/mouse input, the saver/ini and MP3 music. The intro is three Bink movies played **from script** (`lego_level_frontend.initfull`); M2 should stub `PlayCinematic` as instant-complete behind a flag.
6. Natives on the menu path: **186 distinct** called directly by the 38 front-end classes, **365** in the fixpoint slice from the four entry points with level entry excluded, 568 with every class resolved, 728 total (EXECUTED).
7. Oracle recommendation: a **binary-cited opcode/semantics table** (generated, every row names its handler address) feeding **two independent interpreters** (C++ engine, Python oracle), compared by **differential execution traces on the real Base.osi** (native == Python == wasm32) plus per-opcode crafted conformance bytecode; owner-run trace mod later for native-side truth.
8. Native strategy: generate the registry from the completed corpus (name, min/max argc, 10 param types, body address), bind the 186 menu natives in three tiers, stub the rest with a logging default, and prove stub safety with a return-value taint pass in the Python oracle.
9. Top risks: corpus gap (initializers/bodies), string/object ownership semantics in the variant model, and the "last 20 %" of menu systems (`ScTileFont`, `GcMouseDriver::Process`, `MapControllerToScript` are very large functions). Each has a sub-hour probe.
10. Size: **33–45 agent-hours** across 6 sub-milestones (M1 was ~25), confidence medium-low (±50 %); the menu-systems slice carries most of the variance. Also flagged: the exe mixes **CodeWarrior x86 V2.3 and MSVC** object files (38 vs 29 compile records), which touches the §4.5 float assumption.

---

## 1. Measured facts

### 1.1 Script file census

| Quantity | Value | How (EXECUTED) |
|---|---|---|
| OSI files in the install | 1 (`data/Base.osi`, 486,274 B, version 4.1) | `ls -la "$LOMN_DATA/install/data/Base.osi"`; `osi.py --summary` |
| Strings / globals / global functions / classes / symbols / sources | 2,749 / 8 / 23 / 395 / 2,516 / 0 | `.venv/bin/python tools/research/osi.py Base.osi --summary` |
| Methods / unique subroutines / instructions | 8,406 / 3,606 / 151,522 | `tools/research/osi_maxima.py Base.osi` |
| Maxima | 1,707 insns per subroutine (`setupshoefitterscreen`), 48 branch targets, 78 calls per subroutine, argc ≤ 9, literal stack ≤ 16 | same |
| Distinct opcodes used | 67 | `m2probe/census.py` |
| `CallGameFunction` (0x82) sites / distinct `class.func` / native classes | 4,402 / 728 / 64 | same |
| 0x83 `CallGameFunctionFromString`, 0x84 `CallGameFunctionDirect` in the file | 0 / 0 (0x84 only appears after the VM self-patches, §1.2) | same |
| Global functions | `createmenuobject input onevent process setup setupareaviewer setupcinviewer setupdemoscreen setupfrontendscreen setupinterface setuplanguagetest setuplevel_1..7 setupmenuscreen setupoptionscreen setupprintscreen setupshoefitterscreen setuptestlevel` | same |
| Globals | `safeeventmanager globalclass gcscripttools screenlist screenx screeny screenratio newfrontend` | same |

Opcode histogram on Base.osi (count per opcode, EXECUTED `census.py`): `10:72 11:5905 12:8422 13:1941 14:3583 15:5102 16:11951 17:1743 19:12 21:14338 22:3133 23:674 30:12046 33:4983 35:8050 40:173 42:1193 43:8418 44:2099 45:5305 46:5021 47:5799 48:109 50:16 51:10177 52:3662 53:4981 54:2272 60:3597 61:119 62:157 63:65 64:755 6a:139 6b:53 6c:852 6d:10 6e:14 70:1500 71:1555 72:257 73:38 75:12 76:6 77:2 82:4402 90:406 91:2517 92:826 93:156 94:2621 95:35 96:2 a0..a7:38 b0:122 b1:6 b2:67 ba:6 bb:2 bd:5`. Never used: `0x00 0x02 0x03 0x04..0x06 0x18 0x1a 0x24 0x25 0x31 0x32 0x34 0x41 0x49 0x6f 0x74 0x78 0x7a 0x7b 0x80 0x81 0x83 0x84 0xb8 0xb9 0xbc 0xbf 0xff` (29 opcodes with handlers that no script instruction uses; they still need conformance tests because `Return`/`Halt` paths and the self-patched 0x84 execute at runtime).

### 1.2 The VM (from the decomp corpus; class layouts from `gen/decomp/types.txt`)

**Objects (INFERRED from `types.txt` + constructors).** `ScOSIVariant` = 8 bytes `{u16 type @0, u32 value @4}`. `ScOSIVirtualMachine` = 8,028 bytes: `theScript` @8, `ScOSIStack theStack` @12 (8,012 bytes: `mStack[1000]` variants @0, `mTopOfStack` @8004, `mFramePtr` @8008, `mContextDepth` u16), `thePC` (byte pointer into the script image). Constructor at `0x60b230` builds the 1,000-variant array and zeroes depth/PC. `ScOSIScript` (492 bytes, ctor `0x609570`) indexes the tables in place: string table (u16 count @8, entries `{u8 len, chars, NUL}`), globals, global functions (`{u8 len, name, u32 offset, u16 params}`), classes (`{u8 nprops, u16[nprops], u8 nmethods, {u16 symbol, u32 offset}[nmethods]}`), class names, member symbols; `mUnInterpretedScript` = first bytecode byte. Each global variable is also registered into `ScOSISystem::AddGlobal`. This matches the reader already in `engine/src/sage/osi.cpp` (EXECUTED: that reader's `check_all` walked all 151,522 instructions in M1a-3).

**Dispatch (INFERRED `Run` @`0x60b850`).** `Run` loops `handlers[*thePC]()` until a handler returns false. The handler is a CodeWarrior pointer-to-member call (`__ptmf_scall`). Only `Return` at the outermost frame (`BCLReturn`, when the popped marker is `0xff`) and `Halt` (`0xff`) return false.

**Opcode table (EXECUTED, from the binary).** `ScOSIVirtualMachine::handlers` lives at `0x752768` (symbol in `symbols.txt`) and is zero in the file; a `.CRT` static initializer at `0x60cd55` (not in the corpus) copies 12 of every 16 bytes from a template at `0x751768`; each template entry is `{this_delta 0, vtable byte offset, 0}` and `Run` dispatches through the VM vtable at `0x7514e4` (stored by the constructor). `m2probe/handlers9.py` decodes it: **96 opcodes map to real handlers, 160 to `BCLUnused`**, all deltas zero. Full table in `m2probe/opcode_names.json`; grouped:

| Range | Opcodes → handlers |
|---|---|
| Control/debug | `00 Nop`, `02 DebugOn`, `03 DebugOff`, `04/05/06 LineNumber/Alt1/Alt2` (operands `HH`, no-ops at runtime), `ff Halt` |
| Members/objects | `10 SetMemberValue`, `11 GetMemberValue`, `12 GetMemberFunction`, `13 CreateObject`, `14 MemberFunctionArgumentCheck`, `15 SetThisMemberValue`, `16 GetThisMemberValue`, `17 GetThisMemberFunction`, `18/19/1a Get/GetFn/SetMemberValueFromString` |
| Variables | `21 GetVariableValue`, `22 SetVariableValue`, `23 CreateStackVariables`, `24 IncrementVariable`, `25 DecrementVariable` |
| Stack | `30 Pop`, `31 PopN`, `32 Swap`, `33 Pull`, `34 DupN`, `35 Dup` |
| Constants | `40 i32`, `41 i24`, `42 i16`, `43 i8`, `44 f32`, `45 Constant0`, `46 ConstantString`, `47 PushNothing`, `48 Color8888`, `49 Color5551` |
| Calls/branches | `50 JumpRelative` (pops an int file offset, PC = script base + offset), `51 JumpAbsolute` (pops a raw code pointer from `GetMemberFunction`), `52 Return`, `53 CompareAndBranchIfFalse`, `54 BranchAlways` |
| Compare/logic/arith | `60 EqualTo`, `61 LessThan`, `62 GreaterThan`, `63 LessOrEqual`, `64 GreaterOrEqual`, `6a And`, `6b Or`, `6c Not`, `6d/6e/6f BitwiseAnd/Or/Xor`, `70 Add`, `71 Subtract`, `72 Multiply`, `73 Divide`, `74 Power`, `75 Modulus`, `76 BitwiseNot`, `77/78 ShiftLeft/Right`, `7a/7b Increment/Decrement` |
| Engine | `80 GetGameVariable`, `81 SetGameVariable`, `82 CallGameFunction`, `83 CallGameFunctionFromString`, `84 CallGameFunctionDirect` |
| Arrays | `90 CreateArray`, `91 GetArrayValue`, `92 ElementsInArray`, `93 SetArrayValue`, `94 AppendToArray`, `95 RemoveFromArray`, `96 InsertIntoArray` |
| Colour | `a0..a3 Set Red/Green/Blue/Alpha`, `a4..a7 Get …` |
| Conversions/tests | `b0 ConvertToString`, `b1 ConvertToFloat`, `b2 ConvertToInteger`, `b8 IsInteger`, `b9 IsFloat`, `ba IsString`, `bb IsAnObject`, `bc IsGameObject`, `bd IsArray`, `bf GetObjectClassID` |

Note the lesson in miniature: the binary names `0x50` **JumpRelative** (operand is a script-relative offset) and `0x51` **JumpAbsolute** (operand is an absolute code pointer), the opposite of the public SAGESharp naming I remembered. The operand formats in `tools/research/osi.py` (`OPS`/`NOARG`) agree with every handler I read (`0x50/0x51` read a u8 argc, `0x53/0x54` an s16, `0x82` `HHb`, `0x84` `ib`, `0x23` a u8, `0x21/0x22` a u16) — EXECUTED by reading the handler decomps, INFERRED for the 30-odd I did not open.

**Call frames and variables (INFERRED from `CallWithReturn` @`0x60b450`, `SetPCAndArgs` @`0x60b5e0`, `BCLReturn` @`0x600cf0`, `BCLJumpRelative` @`0x600ae0`, `ScOSIStack::Insert` @`0x600ba0`).**
- Engine → script: `CallWithReturn(name, 10 variants)` pushes a **frame-base marker** `{type 0xff, value = saved PC}`, increments `mContextDepth`, `SetPCAndArgs` looks the global function up by name (`ScOSIScript::FindFunction`, max 10 params, error text "Too many args required (max 10)"), pushes exactly the declared parameter count (extra args are disposed), then `Run`. `Return` at a `0xff` marker pushes the return value and stops `Run`; the caller pops to the marker, restores PC and depth. **Re-entrancy is designed in**: natives call `ScOSIVirtualMachine::Call` from 69 engine functions (EXECUTED `calls.jsonl`), e.g. `GcTimer::Trigger`, `GcMouseDriver::Process`, `GcKeyMapper::ProcessKey`, `GcCharacter::Process` — a nested call stacks a second `0xff` frame on the same 1,000-slot stack.
- Script → script: `JumpRelative/JumpAbsolute` pop the target, read a u8 argc, **insert** a `{type 2, value = return PC}` marker *below* the argc arguments (memmove), set PC and recompute `mFramePtr` by scanning down to the nearest type-2/0xff marker. `Return` pops the result, pops down to the marker, pushes the result back, restores PC and frame. Locals: `CreateStackVariables n` pushes `n` nothing-variants; `GetVariableValue idx` reads `mFramePtr[(s16)idx]` (negative = arguments, positive = locals) unless bit `0x8000` is set, in which case it is a script global (`OSI_VM_GLOBAL_VARIABLE_INDICATOR`; e.g. `screenlist` = 0x8003 in the `setup` disassembly).
- `CompareAndBranchIfFalse` pops one value: type `0x0f` (nothing) or int zero → branch (via the `BranchAlways` handler); any other type → error "Results of comparison expression…" (INFERRED @`0x600df0`). So the VM has no implicit float/string truthiness; conformance tests must cover that.
- `Halt` pushes nothing and returns false.

**Variant types (INFERRED).** `0x0f` nothing, `0x10` int32, `0x11` string-table index (pushed by `0x46`), `0x12` float32, `0x15` colour (used by `SetColor` registrations), `0x16` dynamic string (heap `ScFixedStringTemplate<255>`, 268 bytes, produced by `ConvertStringIDToDynamicString`), `1` script object (`ConstructObject` news a 32-byte `ScOSIObject`), `2` return marker, `0xff` frame-base marker, `0x200` by-reference box (an 8-byte heap box pointing at a stack slot, created and freed around a native call), and **game-object types allocated from `0x20` upward** by `ScOSISystem::RegisterType` (the system constructor starts the counter at 0x20). `0x18`, `0x21`, `0x22` appear as registered parameter types in the in-corpus registrations (EXECUTED `natives3.py`); identify them in M2a-0. The universe: **70 `theOSIType<Class>` globals and 21 `GetVariantType<T>` instantiations** in `symbols.txt` (EXECUTED grep), 445 `RegisterType` call sites in `.text` (EXECUTED `handlers5.py`, E8-target scan).

**Conversions (INFERRED `ConvertVariant` @`0x60b9d0`).** Only int↔float↔string are converted (int→float via `(float)(int)`, float→int via `ROUND`, string→number via `atoi`/`atof`, number→string via `%d`/`%f` formats); any other requested target type logs "Requested variant conversion uni…" and yields type 0. Ints are 32-bit in the machine (`uint value`), which answers the "integer width UNVERIFIED" note in `osi.h`: **the VM's integer slot is 32 bits** (INFERRED from the variant layout; wrap semantics of `BCLAdd` etc. still to be cited per opcode).

**Native call (INFERRED `BCLCallGameFunction` @`0x6015a0`, 2,081 bytes; `ScOSISystem::RegisterFunction` @`0x5fab30`).** The instruction carries two string-table indices and a u8 argc. Both names are hashed by `ScOSISystem::GetToken` (case-folded polynomial hash, multiplier 0x83) into a `ScOSIDualToken` and looked up in a `map<ScOSIDualToken, RegisteredFunction*>`; a miss raises "Call to unregistered Game Function". `RegisteredFunction` (36 bytes) = `{class token, name token, fn @8, u8 max @0xc, u8 min @0xd, u16 paramType[10] @0xe}`. Then: argc is range-checked ("Too many/few parameters passed to game function"); for each argument, if the declared type is `0x200` the slot is boxed by reference, else if the declared type differs from the actual, `ConvertVariant` replaces the slot in place; the native is called with the signature **`ScOSIVariant fn(ScOSIVirtualMachine*, u32 a0 … a9)`** receiving only the 10 raw `.value` words; boxes are freed, argc slots popped, the return variant pushed. Finally the instruction is **rewritten in place to `0x84` + the `RegisteredFunction*`** (self-patching bytecode; `BCLCallGameFunctionDirect` then skips the lookup). The script image is therefore mutable at runtime — our VM must either patch a private copy or memoize per call site, and the Python oracle must not trip on a patched image.

**Engine entry points by name (EXECUTED: string symbols in the callers).** `GcGame::SetUpScripting` @`0x4390d0` (called from `GcGame::DoSetUp` after `SetUpControllers/Renderer/Camera/Database/View`, before `SetUpCollisions`): `ScOSISystem::CreateVM("Root/Data/base.osi")` → `Call("Setup")` → `MapControllerToScript(kControllerOne)` → sets the `kScriptProvider` context → `ScProcessManager::AddProcess(new ScSLOSIProcess(vm, "Process"), 2)`. `ScSLOSIProcess::Process(double, double)` calls script `Process` every frame with two numeric arguments (the first a float delta; the second's type tag decompiles garbled by the by-value variants) and keeps running while the script returns nothing or a non-zero int. `GcKeyMapper::ProcessKey` and the analog-controller trigger built by `MapControllerToScript` call **`Input`**; `GcMouseDriver::Process` and `GcTimer::Trigger` call **`OnEvent`** (the other 60-odd `VM::Call` sites are character/collision/boss code, M3 scope; the script-side `safeeventmanager` global is the dispatcher the script uses for its own events). The script side confirms it: `setup` creates `global_class` and `base_screenlist` and calls `screenlist.initfull`; `process` calls `screenlist.process(dt)` and `globalclass.process` (EXECUTED `disasm.py setup process`).

**Timing.** The script itself calls `scprocessmanager.userealdeltatime(1)` in `global_class.initfull` (EXECUTED `osi.py --eval global_class initfull`). In deterministic test mode that native must be a logged no-op, or every L2 replay drifts.

### 1.3 Natives: how many, where they live, what the corpus holds

| Fact | Value | How |
|---|---|---|
| `RegisterFunction` call sites in `.text` | **1,056** | EXECUTED `handlers5.py` (every `E8 rel32` whose target is `0x5fab30`) |
| `RegisterType` call sites | 445 | same, target `0x5fae00` |
| Registrations present in the corpus | 134 (in 6 static-initializer functions `FUN_005470a0` 94, `FUN_0047ba20` 13, `FUN_00445c60` 10, `FUN_0048f840` 7, `FUN_00506ca0` 7, `FUN_00402890` 4) | EXECUTED text scan of `gen/decomp/funcs/*.c` |
| `.CRT` static-initializer pointers / in corpus | 391 / **47** | EXECUTED `handlers4.py` |
| Native bodies referenced as `&LAB_xxxxxxxx` function pointers / in corpus | 215 distinct / **0** | EXECUTED corpus regex |
| Natives called by Base.osi | 728 distinct | EXECUTED `census.py` |

So the corpus knows the *mechanism* completely but holds roughly one eighth of the registry and none of the native wrapper bodies: Ghidra created functions only where symbols or direct calls led it; static initializers reached through the `.CRT` pointer table and the natives reached only through registration pointers were never made into functions. **M2 cannot be planned function-by-function until a decomp re-run seeds those 344 + 215 addresses** (`tools/decomp/seedset.py` and `gen/decomp/seeds.json` already implement root seeding; `make decomp` has a 3,900 s timeout). Expected after the re-run: 1,056 registrations each with `{class, name, body address, min, max, 10 types}` and 1,056 (or fewer, if shared) wrapper bodies — thin functions that unpack the 10 words, call a C++ method and `CreateVariant` the result (45 `CreateVariant` overloads exist; INFERRED from `symbols.txt`).

The 134 in-corpus registrations already show the shape (EXECUTED `natives3.py`, `natives.json`): e.g. `GcTimer.Create` max 3 / min 3 types `(int, int, float)`; `GcHudDirector.AddFont(theOSITypeGcHudFont)`; `GcMoveableObj.Create` max 4 / min 3 `(string, string, string, int)`; `GcRectangle.SetPosition` 5 floats. Parameter types are mostly constants (`0x15`/`0x16`/`0x10`/`0x12`) or `GetVariantType<T>::GetIt()` / `theOSIType*` globals.

### 1.4 What the main menu depends on (EXECUTED unless marked)

**Boot order before the script runs** (`GcGame::DoSetUp` @`0x436a10`, INFERRED): mount volume → `LoadArtBlockHeader` → `LoadSoundBlockHeader` → sound table SLK → `SetUpControllers` → `SetUpRenderer` → `SetUpCamera` → `SetUpDatabase` → `SetUpView` → **`SetUpScripting`** → `SetUpCollisions` → performance graph → random seed `0x2005dd` → `ScNoise::Initialize(0x8000)`. `AppMain` before that: `GcSaver::LoadIni` (`Bionicle.ini`, present in the install: 640×480 windowed, language 0, volumes 100), DirectInput mouse/joystick, `Gc3DSound::InitSoundSystem`, graphics options.

**Script path to the menu** (`disasm.py`, `osi.py --eval`, `menunat.py`):
- `setup` → `global_class` (creates `hud_class, player, gs_lego_tweakers, cheatlist, pausescreen, lego_helpscreen, renderobjects, slosi_random, lego_timer, gs_legotext, legomp3player`; 4× `sctilefont.create('Root/Data/Art/Fonts/BionicleFont')`, `scprocessmanager.userealdeltatime(1)`) → `base_screenlist` → `setupfrontendscreen` → `lego_level_frontend` with area string `'frnt'` → `lego_level_frontend.initfull`: `gcsaver.get*`, `gcareadirector.setcurrentlevel`, **`gchuddirector.playcinematic` × 3** (`New_LOGO8015.bik`, `New_Saffirelogo.bik`, `BIONICLEFMV3.bik`), `gclightdirector.toggleconstantlights` → `lego_area_front.initfull`: `gcareadirector.setfog(0xFFF9DCF9, −40, −1200, 0.05)`, `setnearestmax(10)`, `gcmodeldirector.setnearestmax(10)`, `gclightdirector.setcavelighting(0)`, emitter create/release, music `Root/Data/sounds/MusicGaliAmbience` → `gs_lego_screenfrontend.initfull`: `bettersaver.getbooleanvalue(100)` gates one more movie (`ewokCelebration.bik`), cursor sprite `Root/Data/Art/MenuArt/cursor0000`, camera spline `Root/Data/Levels/lev0/frnt/frnt_CAM.slb`, `gcareadirector.setlanguage`, key options → `gs_lego_screenmenu` / `gs_lego_menuscreen` (menu text via `gs_legoactivetext`/`gs_legotext`, `lego_timer` ×2, `fadescreen`; fonts `BionicleFont` and `BionicleFontHi`).
- **The menu is the 3D area `lev0/frnt`** (M1's survey already loads it: 56,994,412 texture bytes with mips, the largest area; 16 missing textures in `fren/frnt` per `measurements.md`) with the HUD drawn over it. No character/Toa model is created on the menu path (the `player` object only wraps `gcsaver`/`gctoa` state calls).

**Native classes on the menu path.** Union over the 38 front-end/menu classes' methods: **186 distinct natives in 33 engine classes** (`menu_natives.json`): `gcsaver 31, gclegocamera 28, gcareadirector 13, gccharacter 9, gcgame 8, sctextobject 8, gctimer 8, gcmp3player 8, scslosistringfuncs 7, gcbigsprite 7, sctilefont 6, gchuddirector 6, gctoa 6, lsdebugger 4, scprocessmanager 3, gclightdirector 3, bettersaver 3, scslosimath 3, gceditorslider 3, …`. Static fixpoint reachability from `setup/process/input/onevent` with the level-entry functions excluded: **365** natives, 61 classes (`reach2.py`); with no exclusion 611; with method symbols resolved in every class 568; the whole file 728. The 186 is the working lower bound for "must work", 365 the bound for "must at least be registered".

**Fonts.** `sctilefont.create` loads `Root/Data/Art/Fonts/BionicleFont` (also `BionicleFontHi`); on disk each font is a **`.col` collective volume** plus `.col.db` (names) and `.col.t`, and one text `.info` (EXECUTED `ls`, `xxd`). `ScTileFont::ScTileFont` (3,848 bytes) mounts it via `SrMountCollectiveVolume` → `ScCollectiveFile` (`ReadHeader`: four u32 + a 0x14-byte block, byte-swap flag; `ReadDirectory` + `ScHashEntries` 16-byte entries `{hash, 0x11, offset, size}`; `ScNameEntries` for the `.db`). `BionicleFont.col` holds 153 entries, the `.db` names them `BionicleFont0_108.tga`, `BionicleFont0_75.tga`, … (one TGA per glyph); `.info` is a text file (comment lines, then integers 16/16/−4/12/12, four zeros, then the charset string `0123456789!?.:;,"()&ABC…abc…'` plus 0xA0–0xDE). Rendering: `ScTileFont::Draw` (2,837 bytes, `D3DMath_MatrixMultiply`), `GcHudFont::RenderFont` → `ScImageLoader::LoadTexture`. The `MenuArt`/`hud` folders are TGA + `.Col` pairs (142 files in `hud`).

**2D/HUD.** `GcHudDirector` owns `GcHudSprite / GcBigSprite / GcAnimSprite / GcLine / GcRectangle / GcFilledRectangle / GcHudFont / ScTextObject` (the 12 in-corpus `GcHudDirector.Add*/Remove/KillCinematic` registrations). Draw path: `ScDrawableTexture::Draw` with `this->mFVF` and `SetTransform` (string "Cant SetTransform" in it), `GcFilledRectangle::Render`/`ScRenderer::DrawLine(s)/DrawTriangles` with **FVF 0x42 (XYZ | DIFFUSE)** (EXECUTED grep of `docs/surface.md` and the four functions). **No pre-transformed (RHW) vertices**: the HUD is ordinary geometry under an orthographic/HUD transform (`ScHUDTransformation`, 9 functions), so the M1 shim and WGSL generator extend by one vertex layout (position + diffuse, no normal/UV) and textured quads, not by a new pipeline family.

**Input.** DirectInput keyboard/mouse/joystick drivers (`ScKeyboardDriver`, `GcMouseDriver` 17 functions, `ScJoyDriver`), `GcKeyMapper` (`ProcessKey` 7,720 bytes of decomp) → script `Input`; `GcGame::MapControllerToScript` (59,335 bytes of decomp: it builds every trigger/threshold for controller one) → `ScSLOSIAnalogControllerAction::Fire` → `Input`; `GcMouseDriver::Process` (35,659 bytes) → `OnEvent`. The script globals `input`/`onevent` dispatch to the active screen (`base_standardinput`, `base_standardonevent`). M2 needs SDL3 key/mouse events mapped to the same `Input(...)`/`OnEvent(...)` argument conventions, which M2a-0 must read out of `ProcessKey`/`Process`.

**Sound.** `legomp3player` (8 `gcmp3player` natives), `gcstaticsoundsptrarray.playtablesound` (menu clicks), `gc3dsound.synclistenertocamera`. Stub with logging in M2 (M4 owns audio).

**Video.** Six Bink files in `data/Cinematics` (61.3 / 8.5 / 2.5 / 94.0 / 48.5 / 30.1 MB). The intro is script-driven (above); the engine side is `GcHudDirector::PlayCinematic` → `GcPreRenderCinema::LoadCinematic` (Bink). **M2 treatment:** `playcinematic` returns immediately and the "cinematic finished" condition the script polls reports done on the next frame; `killcinematic` no-op; both behind a `--skip-video` flag that defaults **on** in M2 and is reported in metrics. Bink decoding stays in M4 (§9 of the parent spec).

**Saves/options.** `gcsaver` (31 menu natives: `getarea/getlook/getstart/gettoa/gettokencount/load/initdata/getlevel/erasegame/getpercentcomplete/setplayer/get+set fog/gamma/particledetail/shadowdetail/music+soundvolume…`), `bettersaver.get/setbooleanvalue/save` (3; these are LOMNHook-era names but they are called by the vanilla Base.osi, so they must exist in the registry — check in M2a-0 whether the vanilla exe registers them). Read `Bionicle.ini` through the VFS; `User/` mount for saves per parent §4.3.

**Language.** `gcareadirector.getscriptfluffstring(…)`/`setlanguage/getlanguage` — menu text comes from `lang_eng.blk` (50,232 B, in the M1 boot set).

### 1.5 Compiler finding (EXECUTED, affects parent §4.5)

The CodeView compile records in the exe name **"Metrowerks CodeWarrior C/C++ x86 V2.3" 38 times and "Microsoft (R) Optimizing Compiler" 29 times** (`re.finditer` over the exe bytes). The C++ runtime helpers the VM uses — `__ptmf_scall`, `__ptmf_null`, `__construct_array`, `__register_global_object`, `__destroy_global_chain`, `__throw` — are Metrowerks MSL names (EXECUTED grep of `symbols.txt`), and the handler table is a CodeWarrior pointer-to-member layout (§1.2). The engine is therefore a **mixed CodeWarrior + MSVC build**, not plain MSVC6 as `reverse-engineering-playbook.md` states. For M2 the practical impact is small (the VM's float opcodes are `float`-typed variant slots), but the x87 precision assumption in §4.5 should be re-checked per module in M3 (a 30-minute probe: map each `S_COMPILE` record to its module and list which physics modules are CodeWarrior).

### 1.6 A11 sizing (EXECUTED regex over the corpus)

`in_stack_`/`in_EAX` artefacts: **3,161 across 380 functions** (A11 quoted 3,279 with a different counter). By class: `GcMotionSystem 462, CreateVariant 274, ScOSIVirtualMachine 205, GcMouseDriver 185, GcCollisionPhysicsGroup 114, GcCollisionSphereSystem 104, GcToa 91, GcCollisionCylinderSystem 90, GcOctreeDirector 90, GcCollisionPolySystem 89, GcCharacter 81, …`. The fix the playbook already names (declare `ScOSIVariant` by-value parameters correctly for `Call`/`CallWithReturn`/`CreateVariant`/`ScOSIStack::Push`) is a Ghidra signature change, so it belongs in the same decomp re-run as the corpus completion (M2a-0). Only the VM (205) and `CreateVariant` (274) rows block M2; the motion/collision rows are M3.

---

## 2. Decomposition proposal

Ordering principle (as in M1): first measured facts, then an oracle, then end-to-end behaviour as early as possible, then the long tail. Each sub-milestone names its observable acceptance and the exact verification that the reviewer re-runs.

| Plan | Content | Acceptance |
|---|---|---|
| **M2a-0 Corpus completion and citation tables** (no engine code) | Re-run `make decomp` with seeds: the 391 `.CRT` initializer pointers, the 215 `&LAB_` native targets, and the 1,056 `RegisterFunction` call-site functions; apply the A11 signature fix for `ScOSIVariant` by-value parameters. Generate three committed, data-free tables: `docs/osi/opcodes.md` (opcode → handler address → operand format → stack effect, one row per opcode, cited by handler address and the template entry at `0x751768 + 16·op`), `gen/osi/natives.json` (1,056 rows: class, name, body address, min/max, 10 types; regenerated per contributor like the rest of `gen/`), `docs/osi/types.md` (variant type ids: builtin + the registered type order). Also the `Input`/`OnEvent` argument conventions and the `Process` return protocol. | A1–A4 |
| **M2a-1 VM core + Python oracle** | `lomn::osi::Vm` in C++ (handlers for all 96 opcodes, stack/frames/variants/strings/arrays/objects, native call protocol with a pluggable registry, re-entrant `Call`, self-patch memoisation, caps), **and** an independent Python interpreter `tools/research/osi_vm.py` written from `opcodes.md`, not from the C++. Both emit the same trace format. A crafted-bytecode conformance corpus (builder from `engine/tests/osi_builder.h` / `osi_craft.py`). | A5–A7 |
| **M2a-2 Native framework, `Setup` to the first `Process`** | Registry generated from `natives.json`: every native exists as a typed stub (logs class.name, argc, arg types/values, returns a declared-type default) with a tier tag; the ~60 natives `Setup` needs are implemented (fonts load, HUD add/remove, saver reads, areadirector level/area switch reusing M1, timers, string funcs, debug.print). `lomn_view --script` runs `Setup` then N `Process` frames headless. | A8–A10 |
| **M2a-3 Menu systems** | Tile-font rendering (`.col` reader, glyph atlas, `ScTileFont::Draw` semantics), HUD sprites/text/rectangles through the shim (FVF 0x42 + textured quads, HUD transform, draw order vs the 3D area), camera spline (`frnt_CAM.slb`), SDL3 input → `Input`/`OnEvent`, saver/ini via VFS `User/`, MP3/sound stubs, intro-skip flag, `userealdeltatime` override in deterministic mode. The menu renders and reacts to keys. | A11–A13 |
| **M2a-4 Hardening** | Fuzz the VM (bytecode + tables) under ASan/UBSan native and wasm32; caps measured on Base.osi with headroom like `osi.h`; perf entries (script ms per frame, natives per frame); mutation bite check on the conformance corpus. | A14–A15 |
| **M2b Browser** | The BYO page boots to the menu in Chromium; `?script=1` state in `window.__lomn`; Playwright drives the menu with synthetic keys. | A16 |

### Acceptance (observable; measured, never invented)

| # | Check | Oracle (independent) |
|---|---|---|
| A1 | Corpus after re-run contains ≥ 1,056 `RegisterFunction` sites with resolvable names (today 134), ≥ 340 of the 391 `.CRT` initializers, and all 215 `LAB_` native bodies as functions; `in_stack`/`in_EAX` in `ScOSIVirtualMachine::*` and `CreateVariant` falls from 205 + 274 to < 20 | `handlers5.py` E8 count (1,056) and `.CRT` pointer list; the regex counter from §1.6 |
| A2 | `opcodes.md` has exactly 96 rows; each row's handler address equals `vtable[0x7514e4][template[0x751768+16·op].offset/4]`; the 160 others are listed as unused | `handlers9.py` (reads the exe as data) |
| A3 | `natives.json` keys cover all 728 `class.func` pairs Base.osi calls, case-insensitively, and the 186 menu natives are tagged `menu` | `census.py` distinct list; `menu_natives.json` |
| A4 | Variant type table: every type id used in `natives.json` is defined; the registered-type order matches the `RegisterType` call order from the completed initializers | `types.md` vs `handlers5.py` count (445) |
| A5 | **Conformance corpus:** ≥ 2 crafted programs per opcode (96 opcodes, including the 29 Base.osi never uses) with expected final stack/variables written from the cited handler; C++ and Python agree with the expectations on all; every `Error(...)` path (unregistered native, too many/few params, bad branch operand type, conversion failure, stack overflow at 1,000) is exercised and reported, not crashed | expectations authored from `opcodes.md` rows, each citing its handler address |
| A6 | **Differential run on the real Base.osi:** `Setup` + 600 `Process` frames + a scripted key sequence (Down, Down, Enter, Esc) with all natives stubbed: the per-instruction trace (PC, opcode, stack depth, top-of-stack type/value, native name+args+return) is **byte-identical** between native C++, wasm32-under-Node and the Python oracle | the Python interpreter; `cmp` on the three trace files |
| A7 | Mutation bite: 30 single-handler mutations of the C++ VM (swap `<`/`<=`, drop a Dispose, off-by-one frame scan, skip the 0x200 box, wrong branch sign) → ≥ 28 caught by A5 or A6; the 2 misses are explained | `mutate` script in the style of M1's `mutate4.py` |
| A8 | `lomn_view --script --frames 1` runs `Setup` to completion with 0 "unregistered" errors and prints the native coverage report: distinct natives hit, by tier | the stub log |
| A9 | 600 frames of `Process` with stubs: 0 VM errors, trace matches the Python oracle (A6 re-run with the real tier-1 natives enabled, stubs still logging) | Python oracle with the same native *decisions* replayed from the C++ log (the oracle consumes the native return log as an input, so the comparison isolates VM semantics from native semantics) |
| A10 | Native coverage: every native hit on the 600-frame path is tier 1 (implemented) or tier 2 (stub proven return-unconsumed, §3); no tier-3 (unknown) native is hit | taint report from the Python oracle |
| A11 | Font: the glyph set decoded from `BionicleFont.col` equals the `.db` name list (153 entries) and the `.info` charset; a rendered string's pixel width equals `ScTileFont::GetStrLen`'s formula on the metrics (cited) | independent Python `.col` reader + `.info` parser |
| A12 | **State log, one menu frame** (null backend): the draw sequence is `area draws (as M1) → HUD draws` with HUD state settings as the decomp says (`GcHudSprite::Render` states, FVF 0x42 for rectangles/lines, textured quads for sprites/text), Z/alpha states enumerated from the decomp | expected sequence written from `GcHudSprite::Render`, `GcFilledRectangle::Render`, `ScDrawableTexture::Draw` |
| A13 | **Visual:** the menu after the (skipped) intro, with the cursor on the first and second entries, 640×480 and 1280×720, vs the owner's screenshots of the original; text legible, positions within a tolerance measured on the first pair | **user eyeball** (owner-gated; does not block A1–A12) |
| A14 | Fuzz: bytecode/table mutations of Base.osi and crafted files give no crash and a reported error in ≤ 2 s per input under ASan+UBSan native and wasm32 | fuzz harness as in M1a-4 |
| A15 | Perf entries: script ms per frame p50/p95 with the frames-vs-window reconciliation, natives per frame, stack high-water (max of 1,000) — **no thresholds until measured** | `perf/history.jsonl` |
| A16 | Browser: the BYO page reaches the menu, `window.__lomn.state == "menu"`, network shows only build assets, synthetic keys move the cursor (state change observed via `window.__lomn.script.cursor`) | Playwright smoke |

---

## 3. The oracle problem

For the renderer M1 had an independent Python decoder and `native == wasm32` state logs. The VM has no runnable original and the owner's real-game trace mod is later and owner-gated, so M2 needs something that is independent of the implementation *and* of the implementer's reading of the decomp.

### Options weighed

| Option | Catches | Blind spot |
|---|---|---|
| **(a) Independent Python interpreter** built from the opcode citation table | C++ coding slips, wasm32/native divergence, frame/stack bookkeeping, string ownership (a leak or double-dispose shows up as a trace difference once the Python side models ownership explicitly) | **Shared misreading**: if the table row for an opcode is wrong, both interpreters agree and both are wrong (the LOMN 2026-10-07 failure, three times) |
| **(b) Differential execution native vs wasm32** (free: the engine already builds both; A6 of M1 showed byte-identical traces) | 32/64-bit, UB, uninitialised memory, `size_t` arithmetic | Nothing semantic: the same source runs twice |
| **(c) Per-opcode citation table** (every opcode/constant/type id cites a handler address; every native cites its registration site and body) | Misreadings become *reviewable*: a reviewer re-opens the cited address and checks the row, which is a 2-minute check per opcode rather than a re-derivation | Catches nothing by itself; it is what makes (a) independent in practice. It also has its own trap: a cited address that is right but *read* wrong by both the table author and the Python author |
| **(d) Crafted conformance bytecode** with expectations authored per opcode from the citation | Edge semantics nobody exercises on the real file (29 unused opcodes, error paths, the 1,000-slot limit, nested `Call` from a native, string-to-number corner cases, `0x200` boxes) | Expectations are written by a human from the decomp; same shared-reading risk as (a), but per opcode and small, hence reviewable |
| **(e) Trace comparison against the original game** (LOMNHook trace mod, owner-run) | The only source of *ground truth* for native behaviour and for the few opcodes whose semantics are genuinely ambiguous in the decomp (`BCLAdd` on string+number, `EqualTo` across types, float→int `ROUND` mode) | Later, owner-gated, and it measures the whole engine, not the VM alone; needs fake-delta inside the original (M3 scope) |

### Recommendation

Use **(c) as the foundation, (a)+(d) as the gate, (b) for free, (e) when the owner can run it**, with three rules that address the "oracle shares the reading" failure directly:

1. **Two readers, two artefacts, one binary.** The citation table is authored by one agent from the decomp; the Python interpreter is written by a *different* agent from the table **and the cited decomp functions**, never from the C++; the C++ is written from the table. Review of a disagreement goes back to the cited address, not to whichever side "looks right". The M1 review that found a same-bits ternary misread is the model: the reviewer opens the address.
2. **Every row names a falsifier.** A table row is not accepted without a conformance program (d) whose expected output would differ under the most plausible alternative reading (e.g. for `0x50 JumpRelative`: a program that only passes if the operand is script-relative, failing if treated as absolute; for `CompareAndBranchIfFalse`: a float on the stack must *error*, not branch). This is the "numeric threshold must come from a measurement" rule applied to semantics.
3. **Native decisions are inputs, not oracle output.** In the differential runs (A6/A9) the Python oracle *replays* the native return log produced by the C++ run, so a disagreement is always a VM-semantics bug, never a native-stub difference. Native semantics are then verified separately per tier (§4), and finally by (e).

**Remaining blind spots, stated:** (i) a handler both readers misread the same way — mitigated by rule 2 and by the fact that the real Base.osi exercises 67 opcodes 151k times so a wrong stack effect usually derails `Setup` visibly; (ii) semantics that depend on the *engine's* state the VM reads back (game variables `0x80/0x81`, member access to game objects `0x18–0x1a`) — those are natives in disguise and sit in §4; (iii) float formatting (`%f` in `ConvertVariant`) and `ROUND` mode are libc-dependent — pin them with conformance programs and tag UNVERIFIED until (e). No "100 %" promise: M2 ships on A1–A12 + A14–A16 with A13 as the owner's eyeball, as in M1.

---

## 4. Native-function strategy

**Generate, don't hand-write.** After M2a-0 the registry is a table of 1,056 rows. A generator emits, for each row, a typed C++ stub with the exact `(min, max, types[10])` contract and a default return of the declared return kind (nothing / int 0 / float 0 / empty string / null game-object of the registered type) **and a log line** (`native class.name(args as typed) -> default`) whenever it runs. The VM never sees an "unregistered" native for a name the original registers; "unregistered" stays an error (as in the original) for names it does not.

**Three tiers, decided by data, not by taste:**
- **Tier 1 — implemented (target: the 186 menu-class natives, in the order the 600-frame trace hits them).** Each binding cites its wrapper body address and the C++ method it calls; where the method is already ported (area/level switching, fog, lighting from M1) it is a one-liner; where it is new (`sctilefont.*`, `sctextobject.*`, `gcbigsprite.*`, `gchuddirector.*`, `gctimer.*`, `gcsaver.get*`, `scslosistringfuncs.*`, `scslosifoundation.stringtoidentifier/identifiertostring`, `gckeymapper.*`, `gcgame.getresolution*`), it is M2a-3 work.
- **Tier 2 — stub, proven safe.** A stub is *safe* on the menu path when its return value is never consumed in a way that changes control flow or a tier-1 argument. Prove it with a **taint pass in the Python oracle** (A10): tag every stub return as `tainted(name)`, propagate through stack ops/arith/member stores, and report any `CompareAndBranchIfFalse`, native argument or `SetGameVariable` that reads a tainted value. A stub whose taint reaches a branch is *not* safe and is promoted to tier 1 or given a decided default (e.g. `gcmp3player.isplaying → 0`, `bettersaver.getbooleanvalue(100) → 0` so the extra movie does not play; each decision is one line in `natives.json`). Candidates from §1.4: `gcmp3player.*`, `gc3dsound.*`, `gcstaticsoundsptrarray.playtablesound`, `lsdebugger.*`, `debug.print`, `gclegocamera.*` until the spline is in.
- **Tier 3 — registered but unknown.** Everything else (≈ 870). Hitting one on the menu path is an A10 failure, which is the worklist signal.

**Worklist.** The coverage report (A8) is the queue: natives hit on the 600-frame + key-sequence trace, sorted by first-hit frame, with tier and taint status. The implementer works the queue top-down; the reviewer re-runs the trace. This replaces guessing which of 1,056 natives matter: the trace says 186 at most and names them in order.

**Verifying a tier-1 native.** Its body is a thin wrapper around a C++ method; the binding is reviewed against the *wrapper body* (argument order, unit conversions, `CreateVariant` kind) and the method's behaviour is tested where it has an observable effect (font width, sprite position in the state log, saver value round-trip). Natives that call back into the VM (`gctimer` → `OnEvent`) get a conformance program that asserts the re-entrant frame layout (nested `0xff` marker, depth 2, correct unwinding).

---

## 5. Risks, ranked, with a pre-mortem

Pre-mortem: "M2 took 3× and the menu still does not come up." The three most likely post-mortems, and the cheapest probe that would have caught each early:

1. **The corpus gap was bigger than the counts suggest** (initializers that Ghidra cannot disassemble cleanly, wrapper bodies that are thunks into templated code, registrations whose names are built at runtime). *Probe (≤ 1 h, before M2a-0 is planned):* seed only the 6 known registration initializers' neighbours plus 20 random `LAB_` targets, re-export those functions alone (`tools/decomp/seedset.py` roots), and check that ≥ 18 of the 20 bodies decompile to a recognisable `{unpack, call method, CreateVariant}` shape and that each registration in the 6 initializers now resolves both names. If the shape is not recognisable, the generator plan changes before anyone builds it.
2. **Variant ownership semantics (strings/arrays/objects: `Duplicate`/`Dispose` on every push/pop/convert) cause leaks, double frees or aliasing that only show up after thousands of frames**, and the Python oracle, written without ownership, cannot see them. *Probe (≤ 2 h in M2a-1):* make the Python oracle model ownership explicitly (reference counts per heap string/array/object; a dispose of a freed value is a trace event) and run the 600-frame trace under ASan; the A6 trace must include `alloc/free` events so a mismatch is loud. Also run the stack high-water metric: the original has a hard 1,000-slot limit with no bounds check (`Push` just increments), so the fuzz harness must prove ours errors instead of writing past the array.
3. **The menu systems are the long tail:** `ScTileFont::Draw` (2,837 bytes), `ScTileFont` ctor (3,848), `GcMouseDriver::Process` (35 kB of decomp), `MapControllerToScript` (59 kB), and the camera spline/HUD transform interplay with the M1 renderer. *Probe (≤ 1 h each, during M2a-0):* (a) decode `BionicleFont.col` + `.info` in Python and render one string to a PNG with the metrics as read from `GetStrLen`/`GetStrHeight`; (b) list the exact `Input(...)`/`OnEvent(...)` argument tuples `ProcessKey` and `GcMouseDriver::Process` build (strings and `CreateVariant` kinds in those functions), so M2a-3 implements a 20-line mapping instead of porting 95 kB of driver code; (c) run M1's `lomn_view --level lev0 --area frnt` with the `frnt_CAM.slb` first record as the camera and eyeball that the menu backdrop is what the owner's screenshot shows.

Other risks (lower): the `userealdeltatime(1)` call breaking determinism (handled by the no-op in test mode, §1.2); the `bettersaver.*` natives possibly absent from the vanilla registry (check in A3; if absent the vanilla script must already tolerate "unregistered" errors, which the VM must then emulate as a logged error + nothing result); self-patching bytecode interacting with the Python oracle (memoise instead of patch, or snapshot the image before tracing); the mixed-compiler finding changing float formatting (`%f`) between modules.

---

## 6. Size estimate (ESTIMATE; M1 ≈ 25 agent-hours)

| Sub-milestone | Hours | Driver |
|---|---|---|
| M2a-0 corpus completion + citation tables | 4–6 | one 65-min `make decomp` run, seeding script, two reviews of the generated tables (reviewer opens cited addresses) |
| M2a-1 VM core + Python oracle + conformance | 8–10 | 96 handlers × 2 implementations × 2 programs each; ownership model |
| M2a-2 native framework + `Setup` | 5–7 | generator + ~60 tier-1 natives to get through `Setup` |
| M2a-3 menu systems | 10–14 | fonts, HUD/text, input mapping, saver/ini, spline, flags; the long tail |
| M2a-4 hardening | 3–4 | fuzz + caps + mutation + perf entries |
| M2b browser | 3–4 | BYO page state + Playwright keys |
| **Total** | **33–45** | confidence medium-low: ±50 %; M2a-3 carries most of the variance, M2a-0's probe 1 could move M2a-2 by ±3 h |

What would tighten it: running probes 1 and 3 before committing the plan (≈ 3 h total) turns the two widest ranges into measured ones.

---

## 7. What needs the owner, and when

| Need | When | Why |
|---|---|---|
| Screenshots (or a short recording) of the original main menu **after the intro**, at 640×480 and the owner's native resolution, with the cursor on the first and on the second entry, plus the Options screen | before M2a-3 review (not blocking M2a-0..2) | A13 is the only visual gate; also settles the font look (`BionicleFont` vs `BionicleFontHi` for highlighted entries) |
| Decision: intro movies skipped in M2 (flag default on) and Bink deferred to M4 | at M2 plan approval | outward-facing behaviour; the recommendation is "skip, flag, report" |
| Decision: keep REBUILT/LOMNHook natives (`bettersaver.*`) as vanilla stubs | at M2 plan approval | parent §3 scopes REBUILT out, but vanilla Base.osi calls three `bettersaver` names |
| Approve the §4.5 note: the exe is a mixed CodeWarrior/MSVC build | informational now; acts in M3 | may change the x87 parity assumptions for physics modules |
| LOMNHook trace mod run (owner-launched original game) | M3, as already scheduled; optional earlier run of just `Setup`+menu with native logging if the owner is willing | the only ground truth for native semantics and the ambiguous float/`%f` cases |
| Nothing else is owner-gated: the decomp re-run, the Python oracle, the conformance corpus and all headless gates run without game data leaving the machine (`gen/` stays uncommitted per A5 of the parent spec) | — | — |

---

## Appendix A — commands run (all EXECUTED on 2026-10-08, host loadavg not recorded)

- Repo facts: `git log --oneline -3` (tip `abe821f`), `wc -l gen/decomp/*.jsonl symbols.txt types.txt`, `ls gen/decomp/funcs | wc -l` (5,618).
- Script census: `.venv/bin/python tools/research/osi.py "$LOMN_DATA/install/data/Base.osi" --summary`; `tools/research/osi_maxima.py`; `python3 m2probe/census.py Base.osi tools/research`.
- Entry points and menu path: `python3 m2probe/disasm.py Base.osi tools/research setup process setupfrontendscreen setupmenuscreen createmenuobject input onevent`; `osi.py --eval <class> <method>` for `lego_level_frontend initfull`, `lego_area_front initfull`, `gs_lego_screenfrontend initfull`, `global_class initfull`, `gs_lego_screenmenu initfull`, `menuoption initfull`, `gs_legotext initfull`; `osi.py --plain-calls gchuddirector.playcinematic`.
- Reachability: `python3 m2probe/reach.py` (UB 568 / fixpoint 611) and `reach2.py` (level entries excluded: 365, 61 classes); `python3 m2probe/menunat.py` (186 natives over 38 menu classes; class indices 273/257/185/8/9 = `lego_level_frontend`, `gs_lego_menuscreen`, `gs_lego_screenmenu`, `base_standardinput`, `base_standardonevent`).
- Decomp index queries (python over `functions.jsonl`/`calls.jsonl`): ScOSI* function list (111 functions), callers of `RegisterFunction` (6 functions), `VM::Call` (69), `CallWithReturn` (2), `SetUpScripting` ← `DoSetUp` ← `AppMain` ← `WinMain`.
- Binary-as-data probes (`m2probe/handlers*.py`, reading `$LOMN_DATA/reference/LEGOBionicle.exe`): section table; `handlers` table zero in file; 257 imm32 references to the table in `.text`, the copy sequence at `0x60cd55` (edi stride 12, esi stride 16); template decode → 96 opcodes (`handlers9.py`); `.CRT` 391 pointers / 47 in corpus; `E8` scan: 1,056 `RegisterFunction` and 445 `RegisterType` call sites; compile-record strings (38 CodeWarrior, 29 Microsoft).
- Registry extraction from the corpus: `python3 m2probe/natives3.py gen/decomp/funcs` (134 sites; 88 function names left unresolved by the throwaway parser, class names all resolved — the parser is not a deliverable).
- Data folder: `ls -la "$LOMN_DATA/install/data/Art/Fonts"` (9 fonts × `.col/.col.db/.col.t`, 1 `.info`), `xxd` of `BionicleFont.col`/`.col.db`/`.col.t`, `grep -v '^//' BionicleFont.info`, `ls data/Cinematics` (6 `.bik`), `cat Bionicle.ini`.
- A11 count: regex `\bin_stack_|\bin_EAX\b` over all 5,618 corpus files (3,161 in 380 functions).

## Appendix B — decomp functions read (names/addresses only)

`ScOSIVirtualMachine::Run 0x60b850`, `::Call 0x60b320`, `::CallWithReturn 0x60b450`, `::SetPCAndArgs 0x60b5e0`, ctor `0x60b230`, `::BCLCallGameFunction 0x6015a0`, `::ConvertVariant 0x60b9d0`, `::BCLReturn 0x600cf0`, `::BCLJumpRelative 0x600ae0`, `::BCLJumpAbsolute 0x600c30`, `::BCLCompareAndBranchIfFalse 0x600df0`, `::BCLCreateStackVariables 0x607b00`, `::BCLGetVariableValue 0x607c50`, `::BCLGetMemberFunction 0x5fe590`, `::BCLCreateObject 0x5fe530`, `::ConstructObject 0x60be00`, `::BCLHalt 0x603840`, `::BCLGetGameVariable 0x6020d0`, `::BCLPushConstantString 0x606700`, `::BCLPull 0x606420`, `::BCLDupN 0x6064b0`; `ScOSIStack::Push 0x5fd0c0`, `::Pop 0x5fd260`, `::Insert 0x600ba0`, `::PushInto 0x601ec0`, `::PushIntoFrame 0x607c00`; `ScOSISystem::RegisterFunction 0x5fab30`, `::RegisterType 0x5fae00`, `::GetToken 0x5faae0`, `::GetTypeToken 0x5fac40`, `::GetTypeString 0x5fad80`, ctor `0x5fa750`; `ScOSIScript::ScOSIScript 0x609570`; `GcGame::SetUpScripting 0x4390d0`, `GcGame::DoSetUp 0x436a10`, `_WinMain@16 0x401020`, `FUN_00401140`, `FUN_00402890`, `FUN_00445c60` (registration sites); `ScSLOSIProcess::ScSLOSIProcess 0x439410`, `::Process 0x43b250`; `ScCollectiveFile::ReadHeader 0x4cf390`; string-symbol scans of `GcKeyMapper::ProcessKey 0x5d1a10`, `GcGame::MapControllerToScript 0x4370c0`, `ScSLOSIAnalogControllerAction::Fire 0x613f80`, `GcMouseDriver::Process 0x55c160`, `GcTimer::Trigger 0x445ae0`, `ScDrawableTexture::Draw 0x5569f0`, `GcFilledRectangle::Render 0x523fc0`, `ScRenderer::DrawTriangles 0x531380`, `ScRenderer::DrawLines 0x530d50`, `ScTileFont::ScTileFont 0x626b40`.
