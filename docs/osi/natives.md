# OSI natives: rulings for M2a-2 (the native framework)

This file collects the asm rulings that the M2a-2 native framework implements. Task 2a wrote them by reading the exe and the mod DLLs as data; neither was ever run. Task 2b wrote §1 (the Decisions, the native ABI, the citation format), §3 (natives.log v2), §7 (tiers, overrides, taint sinks), §8.3 (the name bound) and the notes after D16 in §5.4. This file is one of the documents both engines may read (two-engine independence, plan "Independence").

**Citation labels.**
- **CITED (addr):** read from `objdump -d -M intel` of the canonical exe (`$LOMN_DATA/reference/LEGOBionicle.exe`) or of a DLL in `$LOMN_DATA/install/mods`, at that address.
- **EXECUTED:** a committed tool or a read whose output was observed. `tools/osi/exc.py`, `tools/osi/reach.py` and `tools/osi/event_probe.py` are the committed tools.
- **INFERRED:** a reading that was not run to a conclusion. It says so where it is used.

Decompiled code is never quoted. The decomp (`gen/decomp`) only supplied names.

## §1 Decisions, the port's native ABI and the citation format

Task 2b wrote this section and §3 and §7 from the plan's Decisions 1–13 and spec §4 (D13–D16). Where the plan's
Decision text differs from a ruling in §2–§9, the ruling here wins. The differences are marked "corrected".

### 1.1 The Decisions

1. **Acceptance rows.** M2a-2 owns A8, A9 and A10, the menu tagging of the A3 amendment (defined by the A8/A9 run
   trace), and an A15 row for the new run, with no thresholds. A11–A13 stay in M2a-3; A14 and the final A15 go to M2a-4.
2. **`--script` syntax.** `lomn_view --install DIR --script 1 [--frames N]`. `Base.osi` is read through the VFS at
   `Root/Data/base.osi`, the literal that `GcGame::SetUpScripting` passes to `CreateVM`.
3. **Generated stubs and overrides.** M2a-1's runtime stub table (D11, driven by the generated `ret` column) is the
   logging stub. Tiers and decided defaults live in `data/osi/native_overrides.json` (§7). `tools/osi/registry.py`
   merges them into `data/osi/natives.json` and adds no key to a row that no override names, so an empty file changes
   no byte.
4. **D13, game objects:** §2, and conformance.md rule 18.
5. **D14, natives.log v2:** §3, and `docs/osi/trace.md` §6.
6. **Tier 1 inside M2a-2** means the script-visible behaviour is modelled: the return value, by-reference writes, nested
   engine calls, reference counts, and state that another native reads back (set → get). Render, audio and model side
   effects are not modelled; M2a-3 promotes the natives it renders.
7. **Taint sinks:** §7.3.
8. **The engine → VM sources of the headless frame** are those of the A6 frame: the KEYSPEC `Input` calls, then the
   `Process` passes with the re-run and skip rules. §6.2 found no started timer and no other host-originated call on the
   600-frame front-end path except the Toa process path, which is a documented gap (§6.3, D8). So `C -1` (§3) stays
   reserved, and no host-side `H` record is written in M2a-2 (`H` inside natives is written, fix 1).
9. **Saver, INI, `doesfileexist`, mods** (corrected): the values are those of §8.2 and §9, not the plan's. The saver
   bytes are `[0x7474a0]` (mask, a dword), `[0x7474a4]` (glyph), `[0x7474a8]` (health), `[0x7474ac]` (energy).
   `doesfileexist` checks `Art.blk` (0x83b0b8) first, then the two block-file slots, and with both slots 0 at Setup it
   falls back to `FileExists`; on the owner's install it returns int 0. `gcsaver.cheatmenu` is int 0 (the INI has no
   `CheatMenu` key; "ini → M2a-3"). `bettersaver.getbooleanvalue` is int 0 (empty map).
10. **The swallowed nested error** (corrected): Task 2a found branch **(b)**. Three `LoadArea` blocks catch errors from
    nested engine calls (§5.2), so the asm defines the state, and the port implements **D16** (§5.4). The (a) mechanism
    of the plan (a `nested_error_pending_` flag and the error "Undefined: native returned after a nested engine-call
    error") is **not** used: under D16 a native that swallows returns normally, and dispatch continues at the error PC.
    Both engines implement D16 for `swallow` test natives (conformance.md rule 17) and for the modelled `LoadArea`
    blocks. A native that does not swallow lets the error propagate, and the 0x82/0x83/0x84 catch re-reports it
    (rule 15(b)).
11. **D15, the nesting cap:** keep 64 while 64 ≥ 4 × the measured maximum M; otherwise use 4 × M. `STACK_SIZE` is at
    least 4.5 × cap × the bytes per level, measured on a real re-entrant chain.
12. **`-fwasm-exceptions` scope.** `lomn_view` (the wasm32 node tool) inherits the flag once it links `lomn_osi`.
    `lomn_web` does not link `lomn_osi` until M2b.
13. **Close-out review:** Opus 5.5 medium.

### 1.2 The port's native ABI

What a native body in either engine sees and may do. The original's ABI is the 0x82 row of `docs/osi/opcodes.md`:
`fn(vm, w0..w9)`, with `wi` the converted argument's value, and the address of the stack slot for a by-reference
(0x200) parameter.

- **Arguments.** The handler has already checked argc and converted each argument to its declared type
  (`ConvertVariant`; a wrong game type never reaches the native). A native reads argument i as the converted variant.
  - A `type:<Class>` argument is a handle. `object_arg` maps it to the host object; nothing maps to null.
  - A by-reference argument is the stack slot the handler addressed **before** the call. A write goes to that slot,
    even after a swallowed nested error has left values above it (D16 rule 3). The write overwrites the slot and does
    not dispose the old value (conformance.md rule 14(d)).
- **Results.** A native returns one variant, built the way the original's `CreateVariant` overload builds it. A game
  object goes through `create_variant`: null → nothing; otherwise +1 reference, and the handle is minted on first use
  (D13).
- **Reference counts.** `add_ref` / `release` change the engine's references on an object. A release that reaches 0
  writes `F g` and deletes the host object. Each engine reference a native takes or drops is cited from the asm (for
  example `GcBAGroup::SetCharacter` +1 at 0x54b344, §2.4).
  Each change on an object that has a handle writes an `H` record (§3.1) at that event.
- **Nested engine calls.** `engine_call(name, args)` is `Call(name, a0..a9)`: it writes the `C <seq>` record of §3 (with the query `name`),
  runs the call, and disposes the call's result. An error inside it propagates out of the native, unless the native
  is one that swallows (D16). A native swallows only where the original catches: the three `LoadArea` blocks (§5.2).
  A swallow writes the `X N` record (§3.1) at the swallow point.
- **Order inside one native:** nested `C` calls, then `W` writes, then the return.
- **Sequence numbers** are assigned at native entry (Vm `native_seq()`), so a nested native has a higher number than
  the native that called it, while its log line comes first (§3).
- **What a native must not do:** touch VM state other than through these calls (the stack, `pc`, `frame_ptr`,
  `context_depth`, `fpu_pc`), or keep a host pointer in a variant (D7). Its own errors are thrown as `VmError` and
  re-reported by the calling handler as "Script is calling %s.%s" (0x82/0x83) or "Script is calling a game function
  directly." (0x84).
- **`sets_fpu_pc64`** is registry data. The VM applies it at the native's return, stubs included (D8′).

The exact C++ and Python interfaces are in the plan (Tasks 3 and 4). This section is their contract.

### 1.3 The citation format

Every implemented native carries one citation comment, in both engines, in this form:

```
// body 0x……, method 0x…… <Class::Method> [→ 0x…… <callee that decides>]; asm pin: <addr> <what>
```

- `body` is the registry `body` (the wrapper that `RegisterFunction` registers).
- `method` is the function the wrapper calls. `→` follows the chain down to the function that actually decides the
  result, when that is not the method itself.
- `asm pin` is the instruction address whose immediate or data constant the native's asm-pin test checks (plan Review
  focus 1). The value is read from `objdump` at that address, never from the decomp alone.
- Example: `// body 0x617700, method 0x617810 <ScSLOSIFoundation::StringToIdentifier> → 0x5fb060 <ScOSISystem::ConvertStringToID>; asm pin: 0x5fb093/0x5fb09e/0x5fb0a9/0x5fb0b2 shift immediates (first character in the high byte)`
  (calls at 0x61771d and 0x617868).
  The pin's test gives `stringtoidentifier("frnt")` = 0x66726e74.
- In `data/osi/native_overrides.json` (pure ASCII, §7.2) the arrow is written `->`; the check accepts both spellings.
- A mod native cites the DLL: `// body BetterSaver.dll 0x10001f70, …`.
- **Hex widths (review m-5):** every address is written in its shortest lowercase hex form (`0x617700`, `0x5fb060`),
  the body included, although the registry prints bodies with 8 digits (`0x00617700`). Tools compare addresses as
  numbers (the overrides check of §7.2 does), so the width never matters to a check.

## §2 Game objects (D13)

### 2.1 `CreateVariant` overloads

Each of the 45 distinct `ret_class` values over the 57 game rows (56 winning registrations plus `gchudsprite.create` 0x5366d0, which is not the winner) maps to its own overload: 45 classes, 45 addresses. That includes `gchudsprite.create` 0x5366d0, which is not the winner. EXECUTED: the `call` to a `CreateVariant` in each row's body was followed. Each overload works the same way:
- **non-null:** `inc dword ptr [edi+4]` (the AddRef; the count sits at **+4**), then `{type, ptr}`. The type id is registered lazily through `ScOSISystem::RegisterType` 0x5fae00, under the class's own name. All 45 names equal the class.
- **null:** `{0x0f, 0}`, which is nothing.
- **Exception, GcLegoGepetto 0x407bb0:** it has no null test. The AddRef at 0x407bbf runs before any check, so the original faults on a null `GcLegoGepetto*`. The two natives that use it (`gccharacter.getgepetto`, `gctoa.getgepetto`) are not on the A9 path.
- **No overload lacks the AddRef.**

| class | overload | AddRef (`inc dword [edi+4]`) | null → `{0x0f,0}` | type name (RegisterType) | game rows |
|---|---|---|---|---|---|
| GcRectangle | 0x402060 | 0x402090 | 0x402073 | GcRectangle | 1 |
| GcLine | 0x404ea0 | 0x404ed0 | 0x404eb3 | GcLine | 1 |
| GcLegoGepetto | 0x407bb0 | 0x407bbf | **none (no null test)** | GcLegoGepetto | 2 |
| GcViewPort | 0x436700 | 0x436730 | 0x436713 | GcViewPort | 2 |
| GcKeyMapper | 0x436810 | 0x436840 | 0x436823 | GcKeyMapper | 1 |
| GcMP3Player | 0x436960 | 0x436990 | 0x436973 | GcMP3Player | 2 |
| GcBigSprite | 0x43da20 | 0x43da50 | 0x43da33 | GcBigSprite | 2 |
| GcTimer | 0x445550 | 0x445580 | 0x445563 | GcTimer | 1 |
| GcLegoCamera | 0x447500 | 0x447530 | 0x447513 | GcLegoCamera | 1 |
| GcAnimSprite | 0x469dd0 | 0x469e00 | 0x469de3 | GcAnimSprite | 2 |
| GcTriggerPlane | 0x4740f0 | 0x474120 | 0x474103 | GcTriggerPlane | 1 |
| GcCharacter | 0x47ced0 | 0x47cf00 | 0x47cee3 | GcCharacter | 1 |
| GcToa | 0x47cfb0 | 0x47cfe0 | 0x47cfc3 | GcToa | 1 |
| GcBaseBoss | 0x47d090 | 0x47d0c0 | 0x47d0a3 | GcBaseBoss | 1 |
| GcArea | 0x486b30 | 0x486b60 | 0x486b43 | GcArea | 2 |
| GcMoveableObj | 0x486cd0 | 0x486d00 | 0x486ce3 | GcMoveableObj | 2 |
| GcHive | 0x486e10 | 0x486e40 | 0x486e23 | GcHive | 1 |
| GcLight | 0x48dd50 | 0x48dd80 | 0x48dd63 | GcLight | 1 |
| GcTargeteer | 0x50f600 | 0x50f630 | 0x50f613 | GcTargeteer | 1 |
| GcFilledRectangle | 0x523560 | 0x523590 | 0x523573 | GcFilledRectangle | 1 |
| GcHudFont | 0x525070 | 0x5250a0 | 0x525083 | GcHudFont | 1 |
| GcHudSprite | 0x5269b0 | 0x5269e0 | 0x5269c3 | GcHudSprite | 2 |
| GcSprite | 0x528540 | 0x528570 | 0x528553 | GcSprite | 1 |
| Gc3DSound | 0x55e070 | 0x55e0a0 | 0x55e083 | Gc3DSound | 3 |
| GcCinemaEngine | 0x592680 | 0x5926b0 | 0x592693 | GcCinemaEngine | 1 |
| GcCinemaEditor | 0x5a6a90 | 0x5a6ac0 | 0x5a6aa3 | GcCinemaEditor | 1 |
| GcEditorButton | 0x5abb20 | 0x5abb50 | 0x5abb33 | GcEditorButton | 1 |
| GcEditorSlider | 0x5abcf0 | 0x5abd20 | 0x5abd03 | GcEditorSlider | 1 |
| GcSoundscapeEditor | 0x5ade40 | 0x5ade70 | 0x5ade53 | GcSoundscapeEditor | 1 |
| GcConversationEngine | 0x5bc390 | 0x5bc3c0 | 0x5bc3a3 | GcConversationEngine | 1 |
| ScAnimTexture | 0x5d9500 | 0x5d9530 | 0x5d9513 | ScAnimTexture | 1 |
| GcBossProjectile | 0x5e63b0 | 0x5e63e0 | 0x5e63c3 | GcBossProjectile | 1 |
| ScPrinter | 0x5ec930 | 0x5ec960 | 0x5ec943 | ScPrinter | 1 |
| ScTileFont | 0x611950 | 0x611980 | 0x611963 | ScTileFont | 1 |
| ScTextObject | 0x611d40 | 0x611d70 | 0x611d53 | ScTextObject | 2 |
| ScSLOSIFileIO | 0x6157b0 | 0x6157e0 | 0x6157c3 | ScSLOSIFileIO | 2 |
| ScSLOSISLKCubicSpline | 0x61a320 | 0x61a350 | 0x61a333 | ScSLOSISLKCubicSpline | 1 |
| ScSLOSIStringMapper | 0x61c840 | 0x61c870 | 0x61c853 | ScSLOSIStringMapper | 1 |
| GcVectorProjectile | 0x631b50 | 0x631b80 | 0x631b63 | GcVectorProjectile | 1 |
| GcIceBoss | 0x6343f0 | 0x634420 | 0x634403 | GcIceBoss | 1 |
| GcWindBoss | 0x63a1b0 | 0x63a1e0 | 0x63a1c3 | GcWindBoss | 1 |
| GcFireBoss | 0x63e2b0 | 0x63e2e0 | 0x63e2c3 | GcFireBoss | 1 |
| GcMudBoss | 0x643910 | 0x643940 | 0x643923 | GcMudBoss | 1 |
| GcHydraBoss | 0x648180 | 0x6481b0 | 0x648193 | GcHydraBoss | 1 |
| GcRockBoss | 0x64bb30 | 0x64bb60 | 0x64bb43 | GcRockBoss | 1 |

### 2.2 Release (CITED)

- `ScOSIVariant::Dispose` 0x60a930 handles types 0x20–0xfe with `dec dword [obj+4]` (0x60a94c). While the count is still above 0, `jg` skips the rest (0x60a953).
- At ≤ 0 it calls `vtable[1]` (0x60a957). That slot is `SxReferenceCountable::Free` 0x4f2bc0, which calls `vtable[0]` (the deleting destructor) with 1.
- Slot 1 = `Free` was read in the vtables 0x725b50 (base), 0x718b04, 0x72cf20, 0x7049f0 (GcTimer) and 0x73c464 (GcToa). For the other classes it is INFERRED.

### 2.3 Identity (ACCEPTED, controller ruling round 3)

**One handle per live object, whatever the static type.** The variant's type tag follows the native's declared return, through its overload.

Example on the A9 path:
- `gcmodeldirector.gettoa` (key `[0x782884]` = 'cnt1', read at 0x47cf86) and Setup's `gcmodeldirector.getcharacter(stringtoidentifier("cnt1"))` both reach `GcModelDirector::Get` 0x480f70 on the same map 0x782a04.
- So they return **the same Toa object**: once with type `GcToa` (overload 0x47cfb0), once with type `GcCharacter` (0x47ced0).
- Both variants carry the same handle and add to the same count.

### 2.4 Initial engine reference counts

`G` (D14, §3.1) is the number of references the engine holds on the object at the record's event (a native's return, or the start of a `C` call), excluding every variant reference. Later engine changes are `H` records (§3.1).

**Classes constructed by the native** (G = 0).
- The base `SxReferenceCountable` ctors 0x4f2b80 and 0x4f2b90 store 0 at +4 (0x4f2b88, 0x4f2b98).
- None of these constructors adds a reference to `this`. EXECUTED: a heuristic scan of each ctor and its base chain found every `inc [reg+4]` on a member pointer.

| Native (on the A9 path) | Constructor | G |
|---|---|---|
| `gctimer.create` 0x4454f0 | `GcTimer::GcTimer` 0x445840 (→ GcEventTrigger 0x517510 → ScTrigger 0x4c19e0 → 0x4f2b80) | 0 |
| `sctilefont.create` 0x611910 | `ScTileFont::ScTileFont` 0x626b40 | 0 |
| `sctextobject.create` 0x611cd0 | **`ScTextObject::ScTextObject` 0x610c40**, the 5-argument one of the three, called at 0x611d17. Its `inc` at 0x610c92 is on member +0x2c (the font) | 0 |
| `scslosislkcubicspline.create` 0x61a2e0 | `ScSLOSISLKCubicSpline` 0x61a4d0 | 0 |
| `gcbigsprite.loadhudsprite` 0x43dad0 | `GcBigSprite::LoadHudSprite` 0x440460 always `new`s (0x44048a) and calls `GcBigSprite::GcBigSprite` 0x43f210 (→ ScRenderObject 0x4b46d0 → 0x4f2b80). It saves the current block file at 0x440468 and restores it at 0x4404be | 0 |

**Engine-owned objects returned by getters.**
- For these, G counts the engine holders alive at the call. All of these natives are called only in Setup on the A9 path, before the first `ScProcessManager::Process`.
- AppMain calls `DoSetUp` at 0x53867d, and `DoSetUp` → `SetUpScripting` runs `Call("Setup")` at 0x43925a. The frame loop 0x538700 starts later, at 0x53869a.
- So `AddProcess` registrations made so far are still queued as raw pointers (0x4a8f50 → `push_back`). Their +1 in the process map is applied only by the first `ScProcessManager::Process` (0x4a95da / insert).

| Native | Holders at the call | G |
|---|---|---|
| `gcgame.getkeymapper` 0x4367f0 (→ `[0x83aa98]`, 0x43ae90) | the global `ScAutoRefPtr` set in `SetUpControllers`: ctor 0x5cef80, +1 at 0x439583. `AddProcess` 0x4395b7 is still queued | **1** (CITED holder; that no other holder exists is INFERRED) |
| `gcgame.getmp3player` 0x436940 (→ `[0x83879c]`, 0x43aea0) | the global `ScAutoRefPtr` set in `SetUpDatabase`: ctor 0x5dcab0, +1 at 0x43a40c | **1** (same labels) |
| `gcgame.getmainview` 0x4366e0 (→ `ScScreen::GetViewPort` 0x49d3c0) | the screen's view list (`SetUpView`: ctor 0x43a54c, `ScScreen::AddViewPort` 0x43a85a; the temporaries are released at 0x43a953/0x43a96a) | **1**, INFERRED |
| `gcviewport.getcamera` 0x4474e0 (→ `[[vp+0x30]+0x1c]`, `GcViewPort::GetCamera` 0x448080; the temporary `ScAutoRefPtr` on `vp+0x30` is released inside) | the camera node; `[0x7032bc]` (0x43a945) is a raw pointer | **1**, INFERRED |
| `gcmodeldirector.gettoa` 0x47cf80 / `gcmodeldirector.getcharacter` 0x47ce90 (the same Toa) | the model-director map: `GcModelDirector::Add` 0x480eb0, temporary +1 at 0x480ec7, insert 0x480ede, temporary −1 at 0x480f08. `AddProcess` 0x568262 (in the GcToa ctor) is still queued | **1**, INFERRED (the insert copies one `ScAutoRefPtr`) |

**Engine references added later on the path.**
- **`gcmodeldirector.show('cnt1')`** (Setup) → `GcModelDirector::Show` 0x47f830 → `GcBAGroup::SetCharacter` (0x47fb42) adds **+1** to the Toa (0x54b344) and stores it in `[0x83903c]` (0x54b364). CITED.
- **The first `ScProcessManager::Process`** applies the Toa's queued `AddProcess`: +1 (INFERRED).
- **Natives that store a game argument** (for example `gchuddirector.addbigsprite` 0x47afe0, `gchuddirector.addtextobject` 0x47b010) take engine references. Each one's tier-1 citation (Tasks 3 and 4) states its +1, and it is logged as an `H` record (§3.1).

## §3 natives.log v2 (D14)

`natives.log` is the native-return replay log of `docs/osi/trace.md` §6. Version 2 adds five records (`G`, `H`, `C`,
`X`, `K`) and keeps every v1 line byte for byte. The trace format itself does not change. Fix 1 of Task 2b (controller
ruling) made `H` the authoritative record of every engine-side reference change, and `G` a snapshot that the replay
checks.

### 3.1 Records

| record | exact format (C `printf`) | meaning |
|---|---|---|
| call (v1) | `%08d <cls>.<fn> %d -> <r>[ S %d <hex>][ G %d][ W %d <v>[ S %d <hex>][ G %d]]…` | native seq N returned `<r>`; ` W` writes in argument index order |
| ` G %d` | after every game value: a result, a ` W` value, or a `C` argument | **a snapshot**: the references the engine holds on that object at the record's event, excluding **every** variant reference (all script variants of the object, the logged one included). The event is the native's return for a result or a ` W` value, and the start of the call for a `C` argument. The heap stores total = engine + variants |
| `H` | `H %04x:%08x %+d` (`H <type>:<handle> %+d`) | **authoritative**: the engine's references on a script-visible game object (one that has a live handle) change by `%+d`, here, in event order. Inside a native or outside any native (the host). `<type>` is the game type under which the handle was minted (its `A g` line), so one object always prints one type |
| `C` | `C %08d <name> <a0> <a1> … <a9>` | native seq N starts an engine → script call here. `<name>` is a name token of **the query name the engine passed to `Call`**, not the function-table entry that `FindFunction`'s prefix match chose (opcodes.md "Engine calls"; controller ruling, review m-6; this rules `natives.log` only, the trace's `C` line keeps its trace.md §2 definition). The 10 arguments use the `<v>` notation, each 0x16 followed by ` S %d <hex>` and each game value by ` G %d` |
| `X` | `X %08d` | native seq N swallowed the error of its last nested call here (D16; only the three `LoadArea` blocks swallow, §5.2). Written at the swallow point, where the trace has the swallowed `E` line |
| `K` | `K %6d` | the start of frame N (the trace's `K` frame number), written for every frame, also for one that calls no native. Setup has no `K` |
| `C -1` (reserved) | `C -1 <name> <a0> … <a9>` | a host-originated engine call, outside any native |

**What writes `H`.** Every engine-side `AddRef`/`Release` on an object that has a live handle, whoever makes it:
- *inside a native:* for example `gcmodeldirector.show('cnt1')` +1 on the Toa (`GcBAGroup::SetCharacter`, 0x54b344),
  which is neither its argument nor its result; `gchuddirector.addbigsprite` / `addtextobject` +1 on their game
  **argument**, which they do not return (§2.4). Each tier-1 native's citation states its `H` changes;
- *outside any native (the host):* for example the first `ScProcessManager::Process` applying the Toa's queued
  `AddProcess` (+1, §2.4, INFERRED). A host writes such an `H` only if it models that holder; in M2a-2 the Toa process
  path is the documented D8 gap (§6.3), so neither engine models or writes it.

One H line per AddRef/Release event, always +1 or -1, temporaries included: a pair that cancels inside one native body is written as two lines (controller ruling r-m1, 2026-10-09). X -1 is reserved for a swallow on a host route with no native (D16's process entries); no M2a-2 path writes it (ruling r-m2).
The reserved host sequence number is written as the literal `-1` (`C -1 …`, `X -1`), never as `%08d` of -1 (`-0000001`) (M2a-2 T4 fix 1, review m1).

An object that has no handle yet (never passed through `CreateVariant`) is not script-visible: its engine references are
carried by the `G` of the record that mints its handle. An `H` that takes the total to 0 frees the object: `F g` in the
trace, at that event.

`C -1` stays reserved: §6.2 found no host-originated engine call on the 600-frame path.

### 3.2 Order

- Sequence numbers are assigned at native **entry**. Lines are written in **event order**, so a native's own line is
  written at its return, after everything it caused.
- Inside one native: its `H` and `C` lines in the order they happened, each `C` followed by the lines of the nested
  call's own natives (and an `X N` if native N swallowed that call's error), then its own line. That line carries the
  result and, after it, the ` W` writes. The writes happen before the return (the order of events), but they are
  printed after `-> <r>` (the v1 layout).
- A `K` line is written before anything else of its frame.
- **Attribution.** An `H` line carries no sequence number: it belongs to the innermost native that is executing at that
  point of the event stream, or to the host when none is. The replay recovers this by reading the log as one stream in
  its own execution order (§3.3). A `C` and an `X` carry N and are checked against it.
- A native whose nested call raised and did not swallow writes no line of its own: the error ends the engine call, and
  the log ends with the nested call's records. (On the A9 path any such error is already a failure.)

### 3.3 Replay rules (I6)

The replay reads `natives.log` as a single stream with one cursor, in the order its own VM executes.
- **Order:** on call seq N, the replay asserts that `cls.fn/argc` matches the log, as in v1.
- **Inside replayed native N** it consumes records until N's own line: each `H` is **applied** to the object's engine
  count (a count reaching 0 frees it: `F g`); each `C N` is performed at that point with the logged arguments (the
  nested call consumes its own natives' records); `X N` is handled below. A `C M` with M ≠ N, or N's own line arriving
  before a record the replay expected, is `ReplayDesync`.
- **Identity and `G`:** for each logged game value (result, ` W` value, `C` argument):
  - if the logged handle is live, the replay uses it (one more variant reference, no `A g`) and **checks** that its
    engine count, computed from the minting `G` and every `H` applied since, equals `G`. A mismatch raises
    `ReplayDesync("G check at seq N: handle h, engine count c, logged G g")`;
  - otherwise the logged handle must equal the heap's next game handle, and it is minted with `G` engine references
    (the only place where `G` sets a count).
- **Swallowing natives (`X N`, review m-2):** if a `C N` nested call raises during the replay and the next record is
  `X N`, the replay applies D16 rules 1–3 at that point (the state stays as at the throw, it writes the swallowed `E`
  line) and continues native N. If the call raises and the next record is not `X N`, the error propagates out of
  native N (as in the original). If the call does not raise but the next record is `X N`, that is `ReplayDesync`.
- **Host records:** `H` outside any native and `C -1` are never performed by the replay. Host behaviour is implemented
  in both engines; the replay asserts that its own host's next reference change or host call matches the next record,
  the same way it asserts `cls.fn/argc`.
- **v1 logs** (no `G`, `H`, `C`, `X` or `K`) still replay: a missing `G` means 0 engine references at minting, and
  nothing is checked.

### 3.4 Worked example

The example is illustrative. These are not lines from the real `Base.osi` log. Assume Setup calls, in order:
`gcgame.getkeymapper`; `gctimer.create(0, 0, 5.0)`; `gcmodeldirector.addtoa("onua")`, inside which the engine calls
`OnEvent(int 0x32, int 250)` (§4), and that `OnEvent` calls `sctextobject.setstring`; `gcmodeldirector.gettoa()`;
`gcmodeldirector.show('cnt1', 0)`; `gcmodeldirector.getcharacter('cnt1')`, which reaches the same Toa (§2.3);
`gcbigsprite.loadhudsprite(…)` (5 arguments); and `gchuddirector.addbigsprite(s)` on the sprite just loaded. Frame 0
calls `gcsaver.getmask()`. The type ids are those of `data/osi/types.json` (GcKeyMapper 0x3e, GcTimer 0x27, GcBigSprite
0x26, GcToa 0x37, GcCharacter 0x22).

```
00000000 gcgame.getkeymapper 0 -> 003e:00000001 G 1
00000001 gctimer.create 3 -> 0027:00000002 G 0
C 00000002 OnEvent 0010:00000032 0010:000000fa 000f:00000000 000f:00000000 000f:00000000 000f:00000000 000f:00000000 000f:00000000 000f:00000000 000f:00000000
00000003 sctextobject.setstring 2 -> 000f:00000000
00000002 gcmodeldirector.addtoa 1 -> 0010:00000001
00000004 gcmodeldirector.gettoa 0 -> 0037:00000003 G 1
H 0037:00000003 +1
00000005 gcmodeldirector.show 2 -> 000f:00000000
00000006 gcmodeldirector.getcharacter 1 -> 0022:00000003 G 2
00000007 gcbigsprite.loadhudsprite 5 -> 0026:00000004 G 0
H 0026:00000004 +1
00000008 gchuddirector.addbigsprite 1 -> 000f:00000000
K      0
00000009 gcsaver.getmask 0 -> 0010:3f3f3f3f
```

- Line 1: the key mapper is held once by the global `ScAutoRefPtr` (§2.4), so `G 1`. The heap stores 1 + 1 = 2.
- Line 2: a timer constructed by the native: `G 0`. It is freed when the script drops its last variant.
- Lines 3–5: `addtoa` is seq 2 (numbered at entry). Its `C 00000002` line comes first, with the query name `OnEvent`.
  The nested `OnEvent`'s own native is seq 3, and its line comes next. `addtoa`'s line is written at its return, last.
- Line 6: `gettoa` mints the Toa as handle 3 under type 0x37 with `G 1` (the model-director map, §2.4).
- Lines 7–8: `show` (seq 5) adds an engine reference to the Toa, which is neither its argument nor its result
  (0x54b344). The `H` line is written at that event, before `show`'s own line, and prints the minting type 0x37. The
  replay, executing seq 5, applies it: engine count 1 → 2.
- Line 9: the same Toa as `GcCharacter` (type tag 0x22, handle 3). `G 2` is the snapshot: the map and `SetCharacter`,
  no variant counted. The replay finds handle 3 live, adds a variant reference and checks 2 = 1 + 1 (G at minting
  plus the `H`).
- Lines 10–12: `loadhudsprite` mints the sprite as handle 4 with `G 0`. `addbigsprite` (seq 8) stores its **argument**
  and takes a reference: `H 0026:00000004 +1`, then its own line (it returns nothing). The replay, executing seq 8,
  applies the `H`: engine count 0 → 1. If the script later drops its last variant of handle 4, both engines keep the
  sprite (total 1), and neither writes `F g`. Without the `H` line the replay would free it there, and C++ would not.
- Line 13: frame 0 starts. Line 14: an int result needs no `G`.

**Header hash (C1).** Merging overrides changes `natives.json`, so the `natives=` hash in the trace header and the
digest's `T` line change, while the trace does not. Digest guards therefore compare `D` lines only:
`grep '^D' a | cmp - <(grep '^D' b)`. Headers are compared with the `natives=` field stripped:
`diff <(grep '^#' a | sed 's/natives=[0-9a-f]*//') <(grep '^#' b | sed 's/natives=[0-9a-f]*//')`.

## §4 Re-entry: the four on-path natives

All four are **synchronous**: the engine-to-script call happens inside the native body, before it returns. Every argument variant is built by a `CreateVariant` overload or stored as a literal `{0x0f, 0}`. The script function is `OnEvent` (name string 0x73b964 / 0x73df20).

| Native | What re-enters, and its guard | `OnEvent` tuple (a0, a1, then nothing ×8) |
|---|---|---|
| `gcmodeldirector.addtoa` 0x47cc30 | The wrapper calls `GcModelDirector::AddToa(name, 0)`: the second argument 0 is pushed at 0x47cc36. It returns `{0x10, al}` through `CreateVariant` 0x407f30. `AddToa` 0x47e170 → `GcToa::GcToa` (0x47e1d0) → `Call` at **0x568a59**. **Unconditional**: no branch skips it | `(int 0x32, int 250)`. 0x32 comes from `CreateVariant` 0x413ed0 (byte → `{0x10, v}`). 250 is `[toa+0x380]`, set to 0xfa at 0x567e27 (its only writer in the exe), passed through `CreateVariant` 0x4183f0 (u16 → `{0x10, v}`). The same value is copied to `[0x7474c8]` (ammo) at 0x56898b |
| `gccharacter.setupmoveable` 0x4093c0 | `GcCharacter::SetupMoveable` 0x418850 → `GcMotionSystem::SetupMoveable` 0x574b80 → `SetUnderWater` 0x589050 (call at 0x574efd), **only if `[ms+0x14] == GcGame::MainCharacterID()`** (0x574ee5–0x574ef3). `SetUnderWater` calls `Call` on both of its branches (0x589154 / 0x589249) | `(int 0x36, int b)` with `b = [0x73c5a8] != 0 ? 1 : 0`: `CreateVariant` 0x407f30 with 1 (0x589094) or with 0 (0x5891a7). `[0x73c5a8]` is 0 in `.data`, and its only writer is `SetGlobalUnderWater` 0x589264, which is not on the path. So on the path: `(0x36, 0)`, called on the Toa ('cnt1' = `MainCharacterID`, INFERRED: the motion id is the character id) |
| `gcareadirector.setcurrentlevel` 0x486c10 | `SetCurrentLevel` 0x489a60 calls `RemoveArea` (0x489add) → … → `GcModelDirector::Remove` → `Call` 0x47f680, **only if** current level `[0x83902c]` ≠ `[0x70b3bc]` and current area `[0x83b0b4]` ≠ `[0x70b3c0]` (0x489aba–0x489ace). Both of the latter are 0x3f3f3f3f in `.data` | **none on the path.** At Setup's single call both globals are the null id, set at 0x48db4b/0x48db5f and 0x48771e (INFERRED that those inits run before Setup) |
| `gcareadirector.setcurrentarea` 0x486ef0 | (i) `SetCurrentArea` 0x48a540 → `LoadArea` (0x48a588) → `GcAreaLoader::LoadArea` 0x4fedd0: the `LoadObjects` and `LoadCharacters` blocks reach `SetUnderWater` under the same id guard (§5.3). (ii) `GcSaver::SetCurrentArea` 0x5d5500 → `CleanupLoad` 0x5d45d0, which BetterSaver replaces with 0x10001ae0 (§9.3) → `GcAreaLoader::RemoveObject` → `Remove` → `Call` | **none on the path.** (i): `frnt` loads no 'cnt1' object (§5.3). (ii): the removal counts are 0: `[0x83b402]`, `[0x83b401]`, `[0x83b3fa]`, `[0x83b50f]` are zeroed by `ResetData` 0x5d54a4–0x5d54b9 and written otherwise only by `Load` and the `Pickup*` functions. BetterSaver's own token vector is emptied by its `ResetData` hook (0x100016d0–0x100016e0) |

Release counts: `Call` adds a reference to the VM (0x60b338) and releases it (0x60b41f); `CallWithReturn` does the same (0x60b46b / 0x60b5ab). The VM reference count is not modelled by the port.

## §5 Errors inside nested engine calls (Decision 10 → (b)), and D16

### 5.1 The catch census (EXECUTED; `tools/osi/exc.py` after R2; recounted with true function extents in fix round 1)

**Coverage.**
- `exc.py` decodes every `.exc` action kind with the runtime's own sizes (`docs/osi/opcodes.md` "Exception tables").
- All 7,002 indexed functions decode with `truncated 0`.
- The VM re-entry closure is the reverse closure of `Call` 0x60b320 / `CallWithReturn` 0x60b450 over `gen/decomp/calls.jsonl`: **225 functions**. It covers both entry routes: the 35 direct chains and the 49 indirect entries (R6).

**Function extents (fix round 1, C-1).** A census must never take a function's end from the decomp's `size`.
- `GcAreaLoader::LoadArea` 0x4fedd0 has a decomp `size` of 7,431 bytes, which ends at 0x500ad7. The function really runs to `ret 0xc` at **0x500fe4**, and the next function starts at 0x500ff0.
- 358 functions have a direct `call` past their decomp `size`. EXECUTED: the objdump listing, with each instruction owned by the preceding function start in the union of the decomp and `.exc` starts.
- The round-2 census assigned call sites by decomp bounds, and so it missed LoadArea's third block.
- `exc.caught_calls` (fix round 1) takes each site's owner from the `.exc` table that lists it, so no size is involved. `calls.jsonl` itself does hold the LoadArea → LoadHives edge.

**Direct call sites that reach the VM.** There are 450 inside the closure (objdump listing). **5 are inside a catch**; the whole `.exc` index has no other caught `E8` site whose target reaches the VM (`exc.caught_calls`, pinned by `test_real_vm_reaching_catch_sites`):
- `_WinMain` 0x401078 and `SetUpScripting` 0x43925a. These are host roots: there the error ends the game (opcodes.md "After an error").
- `GcAreaLoader::LoadArea` **0x4ff93e** (`GcObjectLoader::LoadObjects` 0x53fc60), **0x5002ec** (`GcCharacterLoader::LoadCharacters` 0x53f3f0) and **0x500c0b** (`GcHiveLoader::LoadHives` 0x6261c0). Each has `catch(!ScCatchMessage!)`, `catch(!exception!)` and `catch(...)`, with pads 0x4ff945/964/984, 0x5002f3/312/332 and 0x500c12/31/51 (chain 0x69cd2e for the third, frame −244).

**Indirect calls inside chain functions.** There are 418 indirect (`call [reg+n]`) sites in the closure (objdump listing), and **43** are inside a catch (`exc.caught_calls`).
- All 43 are `dec [x+4]; jg; call [esi+4]` releases (the bytes `ff 56 04` at each site).
- 41 of them release SLK-file or sound-table results: `LoadArea` ×22 (2 of them the hive SLK, 0x500bbf / 0x500be2), `SetCurrentLevel` ×6, `DoSetUp` ×3, and ×2 each in `InitializeCollisionPhysics`, `AddToa`, `AddBoss`, `AddCharacter` and the `GcCharacterAIController` ctor.
- The other 2 are in `SetUpScripting` (after `CreateVM` and after `AddProcess`).
- None of these releases reaches the VM. INFERRED: the only deleting destructors among the indirect entries are `GcHive`'s and `GcCinemaEngine`'s, and the released object types are taken from the preceding call.

**The round-2 counts were bounds artefacts.** Round 2 gave 438 direct sites (4 caught) and 391 indirect sites (39 caught), all inside decomp bounds. The corrected counts are above. The objdump listing is a linear sweep, which desynchronises at `GcSaver::SwitchArea` 0x5d5520; so the listing's own closure has 224 functions, and 450 / 418 are floors. The caught counts (5 / 43) do not depend on the listing.

**No catch in the VM itself.** `Call` 0x60b320 and `CallWithReturn` 0x60b450 have only cleanup records, all with 0x413ec0 (`~ScOSIVariant` = `mov eax, ecx; ret`). `Run` 0x60b850 has a short record. None catches.

### 5.2 `LoadArea` swallows (CITED)

Three blocks of `GcAreaLoader::LoadArea` 0x4fedd0 reach the VM. Each is a try with the same three catches and the same shape:

| Block | Guard (SLK result non-null) | Call | Pads | Rejoin | `esp` restored | Rethrow / retry flags (written only with 0) | Flag tests | Continues with |
|---|---|---|---|---|---|---|---|---|
| objects | — | 0x4ff93e `LoadObjects` | 0x4ff945 / 964 / 984 | 0x4ff996 | 0x4ff9b3 | `[ebp-0x980]` 0x4ff843 / `[ebp-0x984]` 0x4ff86d | 0x4ff9e0 / 0x4ffa2a | `ShowLoadingBar` 0x4ffa45 |
| characters | 0x5002ad | 0x5002ec `LoadCharacters` | 0x5002f3 / 312 / 332 | 0x500344 | 0x500361 | `[ebp-0x958]` 0x5001fe / `[ebp-0x95c]` 0x500228 | 0x50038e / 0x5003d8 | `ShowLoadingBar` 0x5003f3 |
| hives | 0x500be5 / 0x500bec | 0x500c0b `LoadHives` | 0x500c12 / 31 / 51 | 0x500c63 | 0x500c80 | `[ebp-0x930]` 0x500b48 / `[ebp-0x934]` 0x500b72 | 0x500cad / 0x500cf7 | `ShowLoadingBar` 0x500d12 |

**What the pads do.**
- They store `{error, qualifier}` in `LoadArea`'s own locals: `kUndefinedError` 0x8391f8 for `...`, and 0x839230 for `!exception!` (hive block: 0x500c51 / 0x500c3d into `[ebp-0xfc]`). Nothing reads that pair again except the dead rethrow test.

**Why nothing is rethrown.**
- Over the full extent 0x4fedd0–0x500fe4, each rethrow or retry flag is written only with 0, at the addresses in the table, and no instruction takes its address. EXECUTED: grep of the listing for the six frame offsets.
- So each rethrow test falls through past its `___throw` 0x657260 (hive block: 0x500cef), each retry test falls through, and loading continues with `ShowLoadingBar` 0x4fea80.

**The hive block's path to the VM** (direct calls, each site CITED): `LoadHives` 0x6261c0 → `GcHive::GcHive` 0x6236a0 (0x6262b5) → `GcMoveableObj::GcMoveableObj` 0x5432a0 (0x6236d4) → `GcMoveableObj::SetupMoveable` 0x545c00 (0x54408b, past that ctor's decomp `size`) → `GcMotionSystem::SetupMoveable` 0x574b80 (0x545c20) → `SetUnderWater` 0x589050 (0x574efd, under the `MainCharacterID` guard of §4) → `Call` (0x589154 / 0x589249). The hive SLK is `GcHiveLoader::GetSLKFile` 0x500ff0 (call 0x500b8a) on the name built from `%.4s_hive.slb` (format 0x729a88, at 0x4ff606).

### 5.3 The swallow does not fire on the A9 path

**`frnt` data (CITED from the data).**
- `LoadArea` builds its file names as `Root/Data/Levels/%.4s/%.4s/%.4s_<kind>.slb` (format strings 0x4ff52b–0x4ff611).
- `levels/lev0/frnt.blk` (listed with `V=-v tools/research/blk.py $LOMN_DATA/install/data/levels/lev0/frnt.blk`, EXECUTED; re-run in fix round 1) holds 16 entries: `FRNT_CAM`, `FRNT_LIGHT`, `FRNT_OBJ` and `FRNT_POS` `.slb` files, ten `.x` files and two `.bcl` files.
- That this path maps to `lev0/frnt.blk` is INFERRED from the M1 VFS layout.
- **No `frnt_char.slb`.** `GetSLKFile` returns null, and the `LoadCharacters` block is skipped (0x5002ad).
- **No hive file:** the listing's 16 entries hold no `FRNT_HIVE.slb`. So `GcHiveLoader::GetSLKFile` returns null (INFERRED, as for the character file), and the hive block is skipped by its guard (`cmp [ebp-0x2ec], 0; je 0x500c86` at 0x500be5 / 0x500bec).
- **No instance, vine, emit or spot files** either.
- **`FRNT_OBJ.slb`** (396 bytes, read with `tools/research/slb.py`) holds 6 records of 0x3c bytes (stride: `add [ebp-0x230], 0x3c` at 0x5406fd).
  - The ids are `sky1`, `tre1`, `plnt`, `main`, `watr` and `lava`.
  - The flags (+0x38) are 0x110, 0x08, 0x08, 0x10, 0x110 and 0x10.
- **No record has bit 0x20**, so `GcAreaDirector::AddMoveableObj` (0x53fe03–0x53fe57) never runs.
- **No id is 'cnt1'.** That matters because, before `addtoa`, `MainCharacterID()` returns `[0x7796f4]` = 'cnt1' (0x43af9e; `[0x7032b8]` = 0 until `AddToa` 0x47e527). So the guard at 0x574ef1 cannot pass. INFERRED: the motion id is the record id passed at 0x53fe36 / 0x5403d2.

**Oracle check (EXECUTED).** `OnEvent(0x36, 0)`, made as a nested call while Setup is inside `gcareadirector.setcurrentarea`, runs without error in 310 instructions, and Setup completes.

**Runtime confirmation.** Task 10's `swallow_count` probe counts the nested calls and the caught errors in **all three** modelled `LoadArea` blocks (objects, characters, hives) with real natives: `swallow nested-calls k errors m` in the coverage report.

### 5.4 D16, the swallowed nested error (rule)

> **D16. The swallowed nested error.**
>
> **Scope.** Errors raised by engine calls made inside the three swallowing blocks of `GcAreaLoader::LoadArea` 0x4fedd0 are swallowed: `LoadObjects` (0x4ff93e), `LoadCharacters` (0x5002ec) and `LoadHives` (0x500c0b; it lies past the decomp's `size`, LoadArea really ends at 0x500fe4) (`docs/osi/natives.md` §5.2). `LoadArea` is reached from:
> - the natives `gcareadirector.setcurrentarea` (`SetCurrentArea` 0x48a540 → `GcAreaDirector::LoadArea` 0x4892f0 at 0x48a588 → 0x4fedd0 at 0x48935a), `gcareadirector.switchcurrentarea` and `gcareadirector.switchareas`;
> - the host process `GcCollisionPhysicsGroup::Process` 0x475fc0 → `SwitchCurrentArea` 0x48a5a0 (0x4763d7) → `GcAreaDirector::LoadArea` (0x48a727);
> - the host process `GcBAGroup::Process` 0x54ab00 → `GcBAGroup::CheckForNew` 0x54b380 (0x54b071, in the tail past the decomp's `size`) → `GcAreaDirector::SwitchAreas` 0x48a760 (0x54b4d1) → `GcCollisionPhysicsGroup::SwitchAreas` 0x476cf0 (0x48a78d) → `SwitchCurrentArea` (0x476d08). `CheckForNew` makes that call only while byte `[0x735aec]` = 1 (0x54b4a1). Only `GcBAGroup::SetSwitchAreas` writes 1 there (0x54b24e), and it is 0 in `.data`.
>
> No other VM-reaching call site sits inside a catch. The true-extent census (natives.md §5.1) finds 5 caught sites: these 3 and the 2 host roots.
>
> 1. **At the throw.** A nested `Vm::call` that raises leaves the VM state exactly as the throw found it:
>    - the stack, including the inner `{0xff, PC_ret}` marker (0x60b484), the argc arguments pushed by `SetPCAndArgs`, and the partial frame;
>    - `context_depth` +1 (0x60b4b8; the decrement at 0x60b562 never runs);
>    - `pc` = the failing handler's PC at the throw, as **the error-PC table in opcodes.md (T2a-pc)** gives it;
>    - `frame_ptr` as the failing frame set it.
>
>    No reset runs. `Call`, `CallWithReturn` and `Run` hold only no-op destructor records (0x413ec0, `mov eax, ecx; ret`), so unwinding through the VM changes no VM state. `fpu_pc` becomes 64 (the catch pad's `fninit`, 0x657245). The swallowing native catches the error inside its body and returns its normal result. The VM's own reference count (+2, from 0x60b338 / 0x60b46b) is not modelled.
> 2. **The `E` line of a swallowed error (port rule; the `E` line is a trace artifact).**
>    - The original records nothing that can be seen. Each pad stores `{error, qualifier}` in `LoadArea`'s own locals, and nothing reads that pair again (natives.md §5.2).
>    - The port writes the swallowed error's `E` line **at the swallow point**, in the swallowing host's catch (the conformance runner for a `swallow` test native; the modelled `LoadArea` block for the real natives). The line is built from the VM state as at the throw: `pc` and `op` are the failing instruction's own address and byte (as for every `E` line), and `depth` is the stack depth at the throw. A `P <pc> 64` line follows it if `fpu_pc` was 24.
>    - Unwinding runs no VM code, so when the failing instruction raised the error itself, this `E` line directly follows that instruction's `I` line. When the swallowed error is a 0x82/0x83/0x84 re-raise after its own nested call raised (rule 15(b) of conformance.md), the `E` line follows the last record of that inner call instead: its `pc` and `op` are the re-raising call's, and `depth` and `top` are the stack at the throw (amended in M2a-2 T2b; corpus `n-reenter-nested-error-swallowed`).
>    - The swallowing catch does **not** set the outermost call's `last_error`, does **not** run `reset_after_error()` and ends no engine call. The outermost `Vm::call` returns `ok = true` unless another error escapes it.
>    - This amends D10's "the outermost `Vm::call` writes the `E` line" for swallowed errors only. A nested error that is **not** swallowed is still recorded exactly once, by the outermost `Vm::call` (one `E` line, `ok = false`). The nested calls it passes through record nothing.
> 3. **The calling handler** (0x82, 0x83, 0x84), after a swallowing native returns:
>    - **By-reference writes land in the native's original argument slots, by address.** For a parameter declared 0x200, the handler gives the native the **address** of the argument's stack slot. That address is computed before the native call from the top pointer VM+0x1f50: slot = top − (n − i), at 0x601804–0x601819. The handler boxes it as `{0x200, address}` (0x60185d / 0x601862) and passes the address itself, `*(box+4)` (0x60198c–0x601abc; opcodes.md rows 0x82, 0x83 and 0x84). After a swallow those slots lie **under** the leftovers, below the inner marker. A by-ref write therefore goes to the slot the handler addressed before the nested call, never to top − (n − i) recomputed after it. As conformance rule 14(d) says, a write overwrites the slot without disposing the old value.
>    - Then the handler pops n slots with dispose **from the current top** (0x601c50–0x601c6b). After a swallow these are n inner leftovers, never the written slots. The written values stay where they are, in the leftover region.
>    - It pushes the result (0x601c8d).
>    - For 0x82 only, it self-patches the **outer** instruction through the start pointer it saved at entry (0x6015b8; written at 0x601e2b–0x601eaa).
>    - It **does not change `pc`**: 0x82 never writes VM+0x1f58 after the native call.
>
>    `Run` re-reads `pc` every step (0x60b856), so dispatch continues inside the inner function at the error PC.
> 4. **A later `0x52`** that meets an `0xff` marker stops the running `Run` loop (0x600d54–0x600d87). The engine call that owns that loop then:
>    - pops the result (0x60b505);
>    - pops with dispose to the first `0xff` marker (0x60b53e–0x60b560), which is the inner one;
>    - decrements `context_depth` (0x60b562), and rescans `frame_ptr` if `context_depth` is then not 0 (`cmp word [edi+0x1f4c], 0` at 0x60b569, `je` 0x60b571; the scan runs to 0x60b59b);
>    - sets `pc` from that marker (0x60b5a2);
>    - returns the inner function's value.
>
>    The rest of the outer function never runs in that engine call.
> 5. **Nothing else resets.** Leftover slots stay under the top until something pops them, and `context_depth` stays raised for the rest of the run.
> 6. **`reset_after_error()` after a later error (port rule; host behaviour, visible in traces through its `F` lines).**
>    - In the original, an error that reaches a host root ends the game (opcodes.md "After an error"), so there is no original reset to cite.
>    - In the port, when an error escapes the outermost `Vm::call` (D10), `reset_after_error()` pops with dispose from the top **down to and including that outermost call's own marker**. That marker is the slot at the stack depth the call recorded at its entry, before it pushed the marker. The reset emits the `F` lines of the heap values it frees.
>    - It does **not** stop at the first `0xff` it meets. Inner markers left by a swallow (as in `n-swallow-reerror`) are popped like any other slot.
>    - It then restores `context_depth`, `frame_ptr` and `pc` to their values at that call's entry.
>    - Leftovers below that entry depth, left by an **earlier** successful call, are not touched.
>    - Conformance rule 14(c)'s "the call's outer marker" means this marker.
>
> **Test-native field `"swallow": true`.**
> - The native makes its `calls` nested call. If that call raises, the runner applies rule 1 inside the native and writes the `E` line there (rule 2).
> - Then the native applies its `writes` to its original argument slots, by address (rule 3), and returns its `returns` normally. The handler applies rule 3.
> - The expectations record the outer `Vm::call` result and the post-call snapshot: `depth`, `top`, `pc` and `context_depth`. `pc` is compared under conformance rule 15 as amended by T2a-pc.
>
> **Corpus programs** (Task 2b writes the files):
> - **`n-swallow-continue`.**
>   - Setup: `main` makes a game-variable write W1, calls T (`swallow`, n = 0, `calls` `onevent`, `returns` 5), then makes a write W2 and returns 1. `onevent` executes `0x02` (which advances PC by 1 before its error, 0x6038a6), then pushes 7 and returns.
>   - Expect: `main`'s result is **7**; W1 is written and W2 is not; `context_depth` after the call is 1; exactly one `E` line, written inside T. A second `call("main2")` succeeds above the leftovers.
> - **`n-swallow-reerror`.**
>   - `onevent` executes 0x85 (`BCLUnused`, which does not advance, 0x60be60). It re-executes after the swallow and errors again inside `main`'s `Run`.
>   - Expect two `E` lines with the same pc: the first written inside T (rule 2), the second by the outermost call. The result is `ok = false`.
>   - `reset_after_error()` then returns the stack to the outermost call's entry depth: through the inner leftover marker, down to and including the outer one (rule 6).
> - **`n-swallow-pops`.** T has n = 2. `onevent` (argc 2) holds 2 locals when its `0x02` fails. The 2 pops remove those locals, not T's arguments. `onevent` then returns T's result. T's arguments stay at the bottom of the leftover region.
> - **`n-swallow-writes`.**
>   - Setup: T has n = 1 and one parameter declared 0x200 (by reference). `main` pushes int 3 as T's argument and calls T (`swallow`, `calls` `onevent` with the argument int 4, `writes` argument 0 ← int 9, `returns` 5). `onevent` (argc 1) executes `0x02`, which fails with its argument int 4 on top, then pushes 7 and returns.
>   - Expect: `main`'s result is **7**. The handler's single pop disposes `onevent`'s leftover int 4. After the call the top is **int 9**, in T's argument slot, and `context_depth` is 1.
>   - A write addressed from the top after the nested call would land on the int 4. The pop would then remove the 9, and the top would be int 3, so the program tells the two readings apart.

**Contract notes (Task 2b), for Tasks 3 and 4.**
- *The `E` line of a swallowed re-raise* is now part of rule 2 above (controller ruling, M2a-2 T2b: spec §4 D16 rule 2 carries the same text). `n-reenter-nested-error-swallowed` pins it.
- *The `N` line after a swallow* prints the arguments as converted before the native ran. The `F` lines of the n pops
  are the inner leftovers' (conformance.md rule 17(d)).
- *What a swallow catches (M2a-2 T3 fix 1, review M-3).* The three `LoadArea` pads catch `!ScCatchMessage!`,
  `!exception!` and `...` (§5.2). The port's swallowing call catches the VM's own errors only (C++ `VmError`). Every
  error the original can raise is one of those. The one non-OSI exception the port can see is its own
  "Undefined: host exception" (a `std::exception` thrown by host code), which has no original counterpart and is never
  compared (conformance.md rule 15(d)); it is not swallowed and propagates like any native error.
- *More programs than the four above:* group `n` also holds `n-reenter-nested-error-swallowed` and
  `n-reenter-nested-error-propagates` (plan Review focus 4). The README lists them all with their status.

## §6 The headless frame

### 6.1 Process order (CITED, `ScProcessManager::Process` 0x4a9170)

1. The time step (0x4a9200–0x4a92f2; D3).
2. It flips `[pm+0x40]` (0x4a92f5–0x4a930a).
3. It walks the process map at `pm+0x30` in map order. A process runs when its byte +8 equals the flip and its byte +9 is 0. The call is `vtable[2](time, dt)` (0x4a93a5). A true result marks the process done (0x4a93c1); a false result queues it (0x4a93e6).
4. It re-runs the queue up to 10 more rounds (`cmp [ebp-0xd4], 0xa` at 0x4a957d).
5. It applies queued adds (0x4a95c5–0x4a9635; +1 reference each, 0x4a95da), then removes (0x4a963a onward).

The map's key order between processes, set by the priority byte that `ScProcessManager::AddProcess` 0x4a8f50 reads (`movzx edx, byte [ebp+0xc]` at 0x4a8f60), is **not resolved**.

### 6.2 Engine-to-VM sources on the front end, within 600 frames

| Source | Status on the A9 path | Owner |
|---|---|---|
| `ScSLOSIProcess::Process` → `Process(time, dt, frame)` | the script's own process (`AddProcess` 0x4392f8) | M2a-2 (A6 frame) |
| `GcKeyMapper` → `Input(...)` | the KEYSPEC key calls | M2a-2 |
| `GcTimer::Trigger` → `OnEvent(8, …)` | **no started timer**: `gctimer.create(0, 0, 5.0)` passes arg1 0. The ctor reads `byte [ebp+0xc]` = w1 (0x445523, 0x445906), and `je` 0x44590a skips `GcTimer::Start` 0x445990. It is the only `gctimer.*` call in 600 frames | none |
| `GcMouseDriver::Process` → `OnEvent(26, x, y)` | host input | M2a-3 |
| `GcLegoCamera::Process` → `OnEvent(6)` | camera spline | M2a-3 |
| **The Toa process path** | live, see 6.3 | **M3, a documented host-source gap** |

### 6.3 D8: the Toa process path is a documented host-source gap (ACCEPTED, controller ruling round 3)

**Why it is live (CITED).** The Toa runs every frame on the front end, by two routes:
1. **As a registered process.** The GcToa ctor calls `AddProcess` (0x568262) during Setup's `addtoa("onua")`, and `vtable[2]` of 0x73c464 is `GcToa::Process` 0x569180. That reaches `GcCharacter::Process` (0x56942b).
2. **Through `GcBAGroup::Process` 0x54ab00**, a process registered by `SetUpCollisions` (0x43a4a2). It calls `[0x83903c]->PreProcess` (slot 18, at 0x54acca), gated by state that Setup itself sets:
   - the enable flag `[0x735ae8]` = 1 in `.data` and is never written;
   - the camera AI mode is 0xb, then 3, so the 9/0xe early exit (0x54abc0–0x54abcf) is not taken;
   - `[0x83903c]` = the Toa, set by `show('cnt1')` (0x47fb42 → 0x54b364).

   `GcToa::PreProcess` → `GcCharacter::PreProcess` (0x569165) → the motion system (0x4106a6/0x4106bd), whose physics are enabled by `SetupMoveable`'s `or [ms+8], 0x80000000` (0x574ed8).

**The 42 `OnEvent` sites below them** are guarded by physics and animation state. They are listed with their argument templates in `tools/osi/event_probe.py` `SITES`. Two guards are cited as off on the front end:
- `GcToa::Process` 0x56959f needs `[toa+0x3b4]` to be non-null. Its only writers are the ctor (0x567ede, null), the reset after the call (0x5695aa) and `GcToa::LiftObject` (0x56ca53).
- `CheckForNewAnim` 0x5094fd needs the animation-done list at 0x83aaac to be non-empty. It is filled only by native `checkanimdone` 0x5082c0 (not in the 69) and by boss ctors.

**The probe (EXECUTED; `tools/osi/event_probe.py`, commit `9bd3107`; repeat mode added and re-run in fix round 1).**
- Each site's event tuple, built as the asm builds it, is injected into the oracle's A9 run (stub natives, A9 keys, 600 frames). Computed arguments use 'cnt1' / 0 / 1 and 0.0 / 1.0, which gives 47 tuples.
- `tools/osi/event_probe.py --verify-sites EXE` checks the 42 sites against the exe as data: 42 / 42 hold.
- **Single shot** (the default): one run per tuple and frame, with the tuple injected once at that frame.
- **Repeat** (`--repeat`): one run per tuple, with the tuple injected at **every** frame of `--at`. This covers an event the Toa fires every frame, or N times toward a threshold.

| Mode | `--at` | Runs | SAME | CHANGED | ERROR | Time |
|---|---|---|---|---|---|---|
| single shot | 0,1,10,100,599 | 235 | 229 | 6 | 0 | 116.7 s (round 3: 116.6 s, identical lines) |
| repeat | 0,1,10,100,599 | 47 | 43 | 4 | 0 | 23.8 s |
| repeat | 2-599 | 47 | 43 | 4 | 0 | 36.7 s |
| repeat | all (0-599) | 47 | 43 | 4 | 0 | 36.4 s |

**The fatal tuple, at frames 0–2.**
- These are `OnEvent(0x28, 10000.0, 'hell' | 'crsh' | 'impc')`: the fatal-damage event of `GcMotionSystem::PostProcess` 0x576862, `DoObjectFall` 0x5804d0, `CheckCrushed1` 0x586408, `CheckWallCloseness` 0x58738b / 0x587628 and `HellCheck` 0x589f17 (after `GcToa::ToaMustDie` 0x589e50).
- The float 10000.0 is at 0x73caa0, 0x73d5f4, 0x73db90, 0x73dc30, 0x73dc48 and 0x73de5c. The ids are immediates.
- **Single shot at frame 0 or 1** (−2391 / −2083 instructions): the script skips `sctextobject.setstring` and its `gchuddirector.hide` / `show` calls in frames 0–4.
- **Single shot at frame 2** (found in fix round 1; round 3 never sampled frames 2–9): the same calls are skipped in frames 2–4 (−308, −308, −1159).
- **Single shot at frame 5, 9, 10, 100 or 599:** SAME.
- **Repeat:** at 0,1,10,100,599 and at every frame the result is −2391, first divergence frame 0. At 2-599 it is −1775, first divergence frame 2.

**The non-lethal threshold: `(0x28, int 2, 'impc')`, from `CheckWallCloseness` 0x587240** (new in fix round 1).
- A single shot at any frame is SAME.
- **Two or more injections in frames 0–1** (repeat at 0,1,10,100,599) change frame 4 by −32 instructions, with the same natives.
- **Injection at every frame from frame 2** (repeat at 2-599, or all) skips **3 `gchuddirector.hide` calls in frame 4** (−1159).

**Where it re-converges.** Every change stays inside frames 0–4, the script's HUD set-up frames. From frame 5 on, every run is identical to the baseline in its PC sequence and its native calls, including the runs that inject at every frame through 599. No injection gives ERROR. Every other tuple is SAME in every mode.

**Ruling (controller, round 3; confirmed in fix round 1).** The Toa process path is **out of scope for A9 in M2a-2** and is implemented in M3. It stays a documented host-source gap.
- The A9 differential compares script paths, and the path re-converges from frame 5 in every mode.
- The fatal tuples need lethal damage, in frames 0–2, to a Toa standing at rest on the front end. That is INFERRED to be physically implausible.
- The injection position, after the key calls and before each frame's `Process`, is INFERRED: the process-map order is not resolved.

**M2a-3 risk: the wall-closeness event.**
- `(0x28, int 2, 'impc')` from `CheckWallCloseness` 0x587240 is **non-lethal**. It is **plausible** for a Toa standing on the front end.
- If it fires twice in frames 0–1, or every frame from frame 2, the script skips 3 `gchuddirector.hide` calls in frame 4. The script path re-converges, but the skipped hide calls change **engine HUD state**, so persistent HUD visibility may differ from the original.
- **M2a-3 must either** model `CheckWallCloseness` for the front-end Toa, **or** settle the question against the owner's menu screenshots.

**Re-checks:**
- Task 10 re-runs `event_probe.py --repeat --at all --natives-impl real` with the real natives;
- M2a-3 settles the risk above;
- M3 models the physics.

### 6.4 The 69 / 71 / 144 counts (EXECUTED: grep of the full objdump listing)

- There are **144** `call 0x60b320` sites in **69** functions. Spec §3.3's "69" counts the callers of `Call`; opcodes.md's "144" counts the raw `E8` sites.
- There are **2** `call 0x60b450` sites, in `ScSLOSIProcess::Process` (0x43b32d) and in `Call` itself (0x60b402).
- So **71** functions call one of the two directly. That is Task 1's `reach.py` count.

## §7 Tiers, the registry overrides and the taint sinks

### 7.1 Tiers

Every registered native has exactly one tier. `tools/osi/coverage.py` counts natives by these names (A8).

| tier | meaning | where it is set |
|---|---|---|
| `stub` | the default: D11's typed, logging stub (the generated `ret` column, or the row's `default`) | no override |
| `mod` | a Beta 1.5.0 mod native (D12): a stub until its DLL body is cited | `data/osi/natives_beta150.json` |
| `tier1` | implemented: its script-visible behaviour is modelled (Decision 6) and cited in the §1.3 format | override |
| `tier2` | not modelled; a taint pass in the oracle shows its return value reaches no sink (§7.3) on the menu path. It returns its typed stub, or the override's `default` | override |
| `tier3` | on the worklist; hitting one on the menu path is a failure (A10) | override |

A10 requires that every native hit on the A6/A9 path is `tier1` or `tier2`.

### 7.2 The overrides file and the merge

`data/osi/native_overrides.json` is a JSON list, one row per native key, sorted by `key`. The committed file is `[]`
until Tasks 5 and 7–9 add rows. `tools/tests/test_native_overrides.py` checks the schema.

| field | required | rule |
|---|---|---|
| `key` | yes | a winner key of `natives.json` or `natives_beta150.json` (lower case `cls.fn`); each key once |
| `tier` | yes | `tier1`, `tier2` or `tier3` |
| `default` | no | the stub's return, a scalar: `{"type": "0x10" / "0x14" / "0x15", "value": u32}`, `{"type": "0x0f", "value": 0}`, or a float as `{"type": "0x12", "bits": "8 hex"}` (never `value`). Heap and game types (0x01, 0x05, 0x16, 0x20–0xfe) are refused. Not allowed on `tier1` (its body decides the result) |
| `cite` | yes | names the row's `body` address, in any width (for example `body 0x5d35b0, …`); a `tier1` row has the §1.3 shape: `// body 0x…, method 0x… <…>[ → 0x… <…>]…; asm pin: …` |
| `note` | yes | one line, for example `"ini → M2a-3"` |

No other field is allowed.

**The merge.** `make osi-tables` runs `tools/osi/registry.py --overrides data/osi/native_overrides.json`.
- On the winner row of each named key, the merge sets `tier` and, when the override has one, replaces `default`. An
  exe key is merged into `natives.json`, a mod key into `natives_beta150.json`.
- `cite` and `note` stay in the overrides file. The registry tables carry only what the engines read.
- A row that no override names is written byte for byte as before. It gains no `tier` key: no `tier` means `stub` (mod
  rows keep their own `"tier": "mod"`). So the empty file is a byte-level no-op (minor m6).
- An unknown key, a duplicated key or a schema error stops the merge (`RegistryError`).
- `docs/osi/types.md` gains one generated line with the per-tier counts.
- The merge changes `natives.json` and therefore the trace header's `natives=` hash. That is why digest guards compare
  `D` lines only (§3.4, C1).

### 7.3 Taint sinks (Decision 7)

A tier-2 native's return value must reach none of these on the menu path. The first three are the spec's (§6); the
last two are added.
1. the condition of a 0x53 (`CompareAndBranchIfFalse`);
2. the value a 0x81 writes. 0x81 is a no-op in this build (opcodes.md row 0x81), so the sink exists for the spec's
   wording and never fires;
3. any argument of a `tier1` native;
4. the `Process` result, which the re-run rule reads (spec §3.1: a false result re-runs `Process` in the same frame);
5. a 0x19 member name or a 0x83 function name that comes from a tainted string.
- **jump target**: a 0x50/0x51 call whose target address is tainted (counted under branch).
- **name**: a 0x18/0x1a member name, or a 0x19/0x83 name, that is tainted.
- **refstore**: a store through a tainted reference: 0x10/0x1a into a tainted object, 0x15 into a tainted this, 0x93–0x96 into a tainted array or at a tainted index.
- **errpath**: a tainted array index (0x91/0x93/0x95/0x96) or divisor (0x73/0x75), which can change the error path.

A native whose sinks are all type checks (== nothing, or a type test) is reported "type-only": its exact value may be a cited placeholder, but its type and nothing-vs-something must match the original exactly (controller ruling, 2026-10-09).

The CHANGED-NULL natives (flip probe) are handled by the rule "game-returning → must" (plan Review focus 5), not by a
sink: every CHANGED-NULL native must be `tier1` in the merged overrides.

## §8 Copy-kind natives and block files

### 8.1 The three `copy`-kind natives (CITED)

All three return through `CreateVariant` 0x6159e0, which copies a whole 8-byte variant `{type, value}` bitwise (0x6159ea–0x6159f3). None of the three is on the A9 path: they are not among the 69.

| Native | Body | What it returns |
|---|---|---|
| `scslosifileio.loadosiobject(string)` | 0x615990 → `ScSLOSIFileIO::LoadOSIObject` 0x615f10 | `Load_File(name, 1)` 0x54e010, then **`GetVariantFromBuffer` 0x6160a0** (0x615f36), then `Close_File` 0x54e060. Returns the variant deserialised from the file, of any type. `GetVariantFromBuffer` runs on every call. Its script-object branch (`FindClassID`, `ConstructObject` 0x61641a) calls **`ScOSIScript::FindMemberID` at 0x616519** for each stored property, so it needs the VM's FindMemberID miss cache. On a miss `GetProperty` returns null (0x61652e) |
| `scslosifileio.loadosiobjectfrommemcard(string, int, int)` | 0x615ab0 → `LoadOSIObjectFromMemCard` 0x615ff0 | the same, through 0x616016 |
| `scslosistringfuncs.getstringtable(string)` | 0x61b7d0 → `GetStringTable` 0x61bbf0 | `SrBlockFileLoad(name)` (0x61bc1e), then it writes **1 into the buffer's first dword** (0x61bc29) and returns `{0x13, buffer}` (0x61bc32–0x61bc3a). That is a type-0x13 value whose reference count sits at +0; `Dispose` releases it through `SrFree`. A missing file is INFERRED to fault (the write goes through a null buffer) |

With the Beta 1.5.0 mods, `Load_File` and `Close_File` are hooked by BetterSaver (§9.3).

D11's "copy of argument 0" stub is **wrong** for all three. If the path ever hits one, it is tier 3.

### 8.2 `scslosifileio.doesfileexist` and the block-file slots (I2)

**Chain.** Body 0x615950 → `ScSLOSIFileIO::DoesFileExist` 0x615ef0. With the Beta 1.5.0 mods, that is BetterSaver's hook 0x10001da0 (§9.3). The hook passes any path not containing `User/` (a case-sensitive `strstr`: `user/` does not match) to the original, which calls `SrBlockFileExists` 0x54ddc0 (0x615ef9). A path that does contain `User/` never reaches `SrBlockFileExists` (§9.3).

**`SrBlockFileExists` order (CITED). This corrects the plan's Decision 9.**
1. **The block file object 0x83b0b8** (0x54ddc7–0x54dde4). This is **`Root/Data/Art/Art.blk`** (`GcGame::LoadArtBlockHeader` 0x43b030, path 0x703523), **not** `Main.blk`. It is checked when its header is loaded.
2. **If `[0x736128]` (the current block file) is non-null:** `Find` in it, then `Find` in `[0x73612c]` (the second block file) if that is non-null. Then false.
3. **If `[0x736128]` is null:** `FileExists` 0x54de70 (0x54de41), which opens the name through `ScFileManager::Open` (the mounted volumes and, INFERRED, loose files).

**`Find` 0x60fec0 compares the base name only (CITED, re-derived in T7a fix 1).**
- The request's base name is the text after the LAST `\` (0x60fef6) or `/` (0x60fefc): the loop at 0x60fef0-0x60ff0c moves `esi` to just past each separator and runs to the terminating NUL. `esi` is what the comparisons get (pushes at 0x60ff1d and 0x60ff57). The directory part never takes part, so `Root/Data/Art/x.tga` and `Whatever/x.tga` find the same entry.
- The search is a binary search over the entry table (stride 0x38, `imul` at 0x60ff16; the count is `[this+8]`, and a count of 0 or less gives -1 at 0x60fee1). Equality is 0x60fd30 and less-than is 0x60fd10, both through the byte comparison 0x60fd70, which folds only `A`..`Z` (`cmp 0x41`, `cmp 0x5a`, `or 0x60`: 0x60fdb3-0x60fdd5), compares whole strings and treats the NUL as the smallest byte. The table must therefore be sorted under that fold; `Art.blk` is (2,608 entries, EXECUTED: `tools/research/blk.py`).

**A set `[0x736128]` slot suppresses the `FileExists` fallback (CITED, re-derived in T7a fix 1).**
- `cmp [0x736128], 0` at 0x54ddf0 and `je 0x54de40` at 0x54ddf7: `FileExists` (0x54de41) is reached only from that `je`, that is only when the current slot is null.
- With the current slot set, a miss in it goes to `[0x73612c]` (0x54de11, `je 0x54de32` when that is null), and a miss there ends at `xor al, al` (0x54de32): false, even when the name exists as a loose file.
- `Art.blk` (step 1) is tried before the slots in every case, and a hit there returns true whatever the slots hold.

**The slots at Setup.**
- Both are 0 in `.data`.
- Before Setup, every `SrSetCurBlockFile` 0x54db20 is a save/restore pair: `DoSetUp` 0x436a4e / 0x436bae, `SetUpDatabase` 0x43a2f0 / 0x43a3e2, `GcAreaDirector::Init` 0x487bbb → 0x487daa. `LoadSoundBlockHeader` sets the current block file (0x43b025), but inside `DoSetUp`'s save/restore.
- Before Setup's `doesfileexist` (native #360 of 980 in Setup), every native that changes a slot restores it: `ScTileFont` 0x6276dc → 0x627b07, `LoadHudSprite` 0x440468 → 0x4404be.
- So both slots are **0** at the call. INFERRED: no other writer runs earlier, and `SrSetCurSecondBlockFile` 0x54db40 is reached only from character, level and boss loads.
- So `doesfileexist` does fall back to `FileExists`. The plan's "never checks loose files while Main.blk is loaded" is incorrect.

**Value on the owner's install: int 0.** `Root/Data/AreaViewer.txt` is in neither `Art.blk` nor `Main.blk` (EXECUTED: `blk.py` listings), and no such file exists under `$LOMN_DATA/install`. Whichever route is taken, the result is int 0 (`CreateVariant` 0x407f30). The flip probe shows a 1 would change the path (+293).

### 8.3 The 255-byte bound on name copies (0x18, 0x19, 0x1a, 0x83)

| handler | copy calls (CITED) | bound | PC at the throw |
|---|---|---|---|
| 0x83 | 0x60226a (0x11 name: `operator=(const char*)` 0x4f5c00) / 0x602299 (0x16 name: `operator=(const ScFixedString&)` 0x4f5b00) | the local `ScFixedString` has capacity **0xff** (stored at 0x602224). Both operators throw `kOutOfRange` when **len > capacity**: `seta` at 0x4f5c27 / 0x4f5b16, `SrThrow` at 0x4f5c38 / 0x4f5b27. So 255 passes and 256 throws | +3, before the try (0x6022c8): not caught, not re-reported |
| 0x18, 0x19, 0x1a | 0x5fef3b / 0x5fef69, 0x5ff2e7 / 0x5ff313, 0x5ff49b / 0x5ff4c9 (the same two operators, 0x4f5c00 / 0x4f5b00) | capacity 0xff, stored at 0x5fee9b, 0x5ff261 and 0x5ff3fb: the same `len > 255` test | +1 (opcodes.md error-PC table) |

**How a name longer than 255 arises: a hostile `.osi`** (CITED; controller ruling, M2a-2 T2b).
- The 0x11 copy measures the name with `strlen`: `operator=(const char*)` 0x4f5c00 calls 0x6573c0 at 0x4f5c13 on the
  entry pointer + 1 (0x60224a–0x602262 in 0x83).
- `ScOSIScript::ScOSIScript` 0x609570 builds the string pointer table from the file bytes alone: the u16 count
  (0x6095f1–0x609601), table[0] = the first entry (0x609634), table[i] = table[i−1] + length byte + 2
  (0x60964e–0x60966c). It never checks or writes the terminator byte.
- So an entry whose terminator byte is not NUL is read on into the next entry. A file can carry a 255-byte entry with
  a non-NUL terminator, and the name is then 256 bytes or more. The port loads whatever `.osi` the user supplies, so
  this is the reachable case.

**`Base.osi` never does it** (EXECUTED):
- Its string table has 2,749 entries. The longest is **145 bytes**, none is 200 or more, and every entry ends in NUL
  (the maximum strlen is also 145). Measured on 2026-10-09 by walking the table with `tools/research/osi.py` `parse`
  (the reader `tools/osi/reach.py` uses); the walk agrees with `parse` on the count and the maximum.
- `Base.osi` has 12 0x19 sites and no 0x18, 0x1a or 0x83 site (`tools/osi/reach.py --0x19`). All 12 take their name
  from a member value (`functionpointer`, `func`), all UNRESOLVED and in UNREACHABLE code.

**A 0x16 name cannot do it.** A 0x16 is an `ScFixedStringTemplate<255>` (capacity 0xff, stored at 0x60b915). Its
builders keep it at 254 or less: `ConvertStringIDToDynamicString` throws when len ≥ 255 (`setae` at 0x60b95f), and
`append` (0x70) when len + added ≥ 255 (`setae` at 0x4f5c98).

Neither engine models the bound today. **Corpus:** `n-name-256` (a `.rawstring` entry of 255 bytes with terminator
0x61, followed by an empty entry: strlen 256 → kOutOfRange at +3, pending Tasks 3/4) and its boundary control
`n-name-255` (a normal 255-byte entry: the copy passes; "Script is calling t.<name>" at +4). Both use no native.

## §9 Saver and mods

### 9.1 Default save state (CITED). This corrects the plan's Decision 9 addresses

| Native | Body | Storage | Default at Setup |
|---|---|---|---|
| `gcsaver.setmask` / `getmask` | 0x5d35f0 / 0x5d36d0 | **dword** `[0x7474a0]` (0x5d35fb / 0x5d36d3; returned through 0x407960) | 0x3f3f3f3f (`.data`) |
| `gcsaver.setglyph` / `getglyph` | 0x5d3620 / 0x5d36f0 | byte `[0x7474a4]` (0x5d362b / 0x5d36f3) | 0 |
| `gcsaver.sethealth` / `gethealth` | 0x5d3650 / 0x5d3710 | byte **`[0x7474a8]`** (0x5d365b / 0x5d3713) | 0 |
| `gcsaver.setenergy` / `getenergy` | 0x5d3680 / 0x5d3730 | byte **`[0x7474ac]`** (0x5d368b / 0x5d3733) | 0 |
| `gcsaver.gettoa` | 0x5d36b0 | dword `[0x74749c]` (also written by `AddToa` at 0x47e221) | 0x3f3f3f3f |
| `gcsaver.getammo` / `setammo` | 0x5d3910 / 0x5d3840 | u16 `[0x7474c8]`, returned through 0x4183f0. Written by the GcToa ctor (0x56898b, = 250) and by `AddAmmo`, `LaunchElementalPower`, `ToaIsAlive` and `setammo` | 0. Setup calls `getammo` (native #419) before `addtoa` (#438), so it returns **0** |

**What the init functions touch.**
- `GcSaver::ResetData` 0x5d5490 writes only `[0x74748c]`, `[0x747490]`, `[0x747494]` and `[0x747498]`, the six counters `[0x83b3fa..0x83b50f]`, the conversation vector (call 0x5d54e7 on 0x838b90) and `[0x8391a0]` = 0 (0x5d54ec); 0x5d5493–0x5d54ec. It does not touch the fields above.
- `GcSaver::InitData` 0x5d4700 builds the save path and calls `ResetData`.
- `GcSaver::LoadIni` 0x5d4890 reads only `[GraphicsOptions]` (ResolutionW/H, Windowed, BitDepth, SvpOther, SvpRad, Fog, ParticleDetail, ShadowDetail, Gamma) and `[Misc]` (Language, ControlOption, SoundVolume). If the INI is missing it writes one (0x5d4a3c–0x5d4a57).
- So the defaults above are the `.data` bytes.
- The setters on the A9 path (frame 3: `setmask`, `sethealth`, `setenergy`) store into these fields, and nothing on the path reads them back.

**`gcsaver.cheatmenu`** 0x5d35b0 → `GcSaver::CheatMenu` 0x5d4fc0 → `GetPrivateProfileStringA("Misc", "CheatMenu", "")` (0x5d4fdb–0x5d4fea), then compares the result with "LEGOTester" (0x5d502c). The owner's `Bionicle.ini` has `[Misc]` but no `CheatMenu` key → **int 0** (note: "ini → M2a-3").

### 9.2 BetterSaver natives (CITED: `objdump` of `install/mods/BetterSaver.dll`)

**`bettersaver.getbooleanvalue(key)` 0x10001f70.**
- The key is a u16 (`movzx esi, word [ebp+0x10]`, 0x10001f77).
- It is looked up in a `std::map` (0x10005ab0, 0x10001f88). A not-found result jumps to 0x10001fc8 (`al = 0`); a found result reads the stored byte (0x10001fb8).
- **The result is always type 0x10 with value `(al != 0)`, so 0 or 1** (`setne` at 0x10001fd1, type at 0x10001fd8).
- The second lookup's not-found branch (0x10001fbd) calls `std::_Xout_of_range("invalid map<K, T> key")`. It is unreachable, because the first lookup already found the key.

**`bettersaver.setbooleanvalue(key, v)` 0x10001ff0.** It stores `v != 0` (`setne` at 0x10002005) under the u16 key (0x10001ffb) through 0x10004730, and returns nothing (`{0x0f, 0}`, 0x10002011–0x1000201d).

**`bettersaver.save()` 0x10001f40.** It calls the save routine 0x10002b00. That routine is INFERRED to write `User/data/player%d.sav` through pugixml: the string (file offset 0x7948) and the `xml_document::save_file` import are in the DLL, but 0x10002b00 was not followed. It emits a debug string (`OutputDebugStringW`, 0x10001f4d) and returns nothing.

**The map starts empty on the A9 path.**
- BetterSaver's `ResetData` hook 0x10001680 calls the original, then clears its maps (0x10001688, 0x1000168d, and the tree at 0x1000a814) and its token vector (0x100016d0–0x100016e0).
- The only fill from disk is BetterSaver's `GcSaver::Load` replacement, and `gcsaver.load` is not on the path.
- The owner's install has no `User/` directory.
- So `getbooleanvalue` returns **int 0**.

### 9.3 BetterSaver hooks into the exe (CITED: its init at 0x1000251b–0x100025dd)

Each entry is (target ← replacement; trampoline slot):

| Exe function | Replacement | Trampoline slot |
|---|---|---|
| `GcSaver::SetCondition` 0x5d56a0 | 0x10001570 | 0x1000a7c4 |
| `GcSaver::Save` 0x5d3b60 | 0x10001660 | — |
| `GcSaver::Load` 0x5d4020 | 0x10001670 | — |
| `GcSaver::UpdateConvMemory` 0x5d5750 | 0x10001750 | — |
| `GcSaver::ResetData` 0x5d5490 | 0x10001680 | 0x1000a7c0 |
| `GcSaver::InitData` 0x5d4700 | 0x100016f0 | 0x1000a7bc (calls the original, then resolves the save path with `[0x7474b0]`) |
| `GcSaver::PickupToken` 0x5d5550 | 0x10001a80 | — |
| **`GcSaver::CleanupLoad` 0x5d45d0** | **0x10001ae0** | — (replaced, not wrapped) |
| `Close_File` 0x54e060 | 0x10001be0 | — |
| `Load_File` 0x54e010 | 0x10001c90 | — |
| **`ScSLOSIFileIO::DoesFileExist` 0x615ef0** | **0x10001da0** | 0x1000a7b8 |

**The two that touch the A9 path:**
- **`DoesFileExist`:** a path containing `User/` (`strstr`, 0x10001dbe, case sensitive) is resolved on disk with `LOMNHook::ResolveGamePath` and tested with `GetFileAttributesA` (a file, not a directory). Any other path goes to the original (0x10001e24). `Root/Data/AreaViewer.txt` takes the original route.

**The hook's three branches (CITED: `BetterSaver.dll` 0x10001da0 and `ijl15.dll` `?ResolveGamePath@LOMNHook@@` 0x10001570; T7a fix 1 corrects the first version of this section).** The hook calls `ResolveGamePath(path, buf, 0x104, false)` (0x10001de2) and then `GetFileAttributesA(buf)` (0x10001df2): -1 gives false (0x10001df8), a directory gives false (`test al, 0x10`, 0x10001dfd), anything else true (0x10001e01). `ResolveGamePath` first finds `Root/` (needle 0x1001e5b4) and `User/` (0x1001e5bc) with `strstr` (0x1001e128 is VCRUNTIME140 `strstr`) and compares each hit with the path start:
1. **`Root/` at the start** (`cmp edi, ebx` 0x100015b2): `GetCurrentDirectoryA(size, buf)` (0x100015c1), then the path from after `Root` (`add ebx, 4`, 0x100015c7) is appended: the game directory. Port: `vfs->exists(path)` on the `root/` mount.
2. **`User/` at the start** (`cmp ecx, ebx` 0x100015cc): `SHGetFolderPathA(0, CSIDL_APPDATA 0x1a, 0, 0, buf)` (0x100015df), `\`, the game edition (0x100015f2), then the path from after `User`. Port: `vfs->exists(path)` on the `user/` mount (the per-run User directory).
3. **Neither** (0x10001608): the code `sprintf`s the "Fatal: We disallow writing to filenames that do not start with Root or User" text (format 0x1001e5c8) and calls `MessageBoxA(0, text, "OSI Script Security Abort", MB_ICONERROR)` (0x10001632; title 0x1001e624). It then **falls through** (0x10001638 reloads the buffer pointer; there is no exit and no return): the common tail at 0x1000163e appends the path UNCHANGED with `strcat_s(buf, size, path)` (0x10001641) and turns `/` into `\` (0x10001662-0x1000167d). `GetFileAttributesA` then resolves that relative path against the process's current directory, which is the directory branch 1 maps through `GetCurrentDirectoryA`. **The answer is `exists("Root/" + path)`.** (The first version of both engines returned false here: a shared misreading, found by review.)
- **The message box is a host UI side effect.** It is not modelled and not written to natives.log; a UI milestone decides what the browser shows (deferred to the UI milestones).
- **Divergences that stay (hostile `User/` paths):** (a) `User/../x`: Win32 resolves `..` against `%APPDATA%\<edition>` in the original, so it can leave the user directory; the VFS normalises `..` inside its mount (`fs::normalize`) and never escapes, so the port answers false. That is deliberate (safe in the browser). (b) A path too long for the 0x104-byte buffer makes `strcat_s` call the CRT invalid-parameter handler in the original (INFERRED: the process ends); a script string is at most 254 bytes, `strcat_s` appends to a prefix, so only `User/` paths near the limit are affected, and the port answers by file lookup.
- **`CleanupLoad`:** the replacement walks BetterSaver's token vector and the exe's lists at 0x8384a0 and 0x7f79f0 against the current area, calling `GcAreaLoader::RemoveObject`. On the path all of them are empty (§4).

**`OSIProject.DebugServer.dll`:** its `lsdebugger.*` natives are not on the path and were not read further.

### 9.4 Other notes on the services group (T7a fix 1)

- **`debug.print` (tier2) in a 254-byte case.** The body appends `"\n"` (0x756948) to its argument string in place (0x61d614 -> `ScFixedString::append` 0x4f5c80). `append` throws `kLenTooLong` (error 0x83aa20) when `len >= capacity - n` (0x4f5c96-0x4f5c98, `setae`), here `len >= 254`. So a 254-byte argument is a VM error in the original ("Script is calling debug.print"), while the tier2 stub returns nothing. A script string is at most 254 bytes, so only that one length differs; Base.osi's longest string-table entry is 145 bytes (§8.3). The result `{0x0f, 0}` is written at 0x61d63e/0x61d643, after the virtual call that prints (0x61d639, behind the cheat-menu gate 0x5d4f00); output is not modelled.
- **`gctimer.create` and `Start`.** A started timer is held by the trigger list: `AddTrigger` 0x4c2440 takes a temporary reference (`inc [this+4]`, 0x4c25e5) and releases it (0x4c2619); the lasting reference is the list element's copy constructor 0x4c3280 (`inc [edi+4]`, 0x4c32ae), reached from the insert 0x4c2f90 at 0x4c302b. Engine references: unstarted G 0, started G 1.
- **`UseRealDeltaTime` outside `--deterministic`.** `ScProcessManager::Process` calls `GetTime(1)` (0x4a91fe-0x4a9200; `timeGetTime() * 0.001`, 0x4fa475-0x4fa48d) and runs the whole blend and clamp (0x4a920c-0x4a929f) on EVERY pass. The flag byte `[0x712d78]` is read once, at 0x4a92a5, and only selects which dt and process time are used: the smoothed dt and `now` (0x4a92ae-0x4a92c0), or the fake step 0x712d68 added to the time (0x4a92c8-0x4a92e3). `[pm+0x10]` receives `now` in both cases (0x4a92ec-0x4a92f2). The port steps the smoothing every frame when not deterministic and selects by the flag. Under `--deterministic` nothing of this runs (D3).
- **`identifiertostring`** is a pure byte swap with no table and no unknown-id text (0x6178c0-0x617993), cut at the first NUL.
- **Block-file state in the port.** One `sage::BlockChain` per host context (`HostContext::block_chain()`, Art.blk opened on first use) stands for the process-global Art block file and the two slots; a native that sets, saves or restores the current block file must use it.
