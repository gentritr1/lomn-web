# M2a-0 probe 3: Input / OnEvent / Process argument tuples (controller's record of the subagent hand-back, 2026-10-08)

The subagent could not write files; this is the controller's condensed copy. Every address comes from objdump of the reference exe. Controller re-check (EXECUTED, read straight from the exe):
- the key table at `0x703318` is `0D 20 25 26 27 28`;
- the dt constants are `0x712dc0` = 0.016666 (not exactly 1/60), `0x712dc8` = 0.07, `0x712dd0` = 0.93, `0x712d68` = 0.0066667 (fake dt), `0x712dd8` = 0.066 (cap);
- the mouse scale factors at `0x73a0f0` and `0x73a0f4` are both 1.0;
- the name strings are `0x703453`/`0x747174` "Input", `0x73a11c`/`0x7047e8` "OnEvent" and `0x703496` "Process".

The install exe and the reference exe differ in 798 bytes, none of them in input-path functions; they sit in the MudBoss, RockBoss, SurfaceOctree::GetLeaf, AreaDirector::Hide and Printer code.

## Calling convention
`ScOSIVirtualMachine::Call` (`0x60b320`, thiscall) takes the name plus 10 pointers to caller-built `{u16 type, u32 value}` temporaries and returns with `ret 0x2c`.

`CreateVariant` overloads:

| Address | Takes | Produces |
|---|---|---|
| `0x413ed0` | event (u8) | int |
| `0x4183f0` | u16 | int |
| `0x42a5b0` | int | int |
| `0x407df0` | ScIdentifier | int |
| `0x445730` | double | `{0x12, (float)}` |

There are 146 call sites: 140 OnEvent, 2 named "Input", 1 Setup, and 3 that take the name from a member (Fire, Process, and Call's own forward).

## Call sites (C1 = int 0x636E7431 'cnt1'; arguments not listed are nothing `{0x0f,0}`)

| Id | Site | Call |
|---|---|---|
| S1 | `SetUpScripting` @`0x43925a` | `Setup()` |
| S2 | `ScSLOSIProcess::Process` @`0x43b32d` | `Process(float time, float dt, int frameCounter)`. The counter is `gProcessManager+0x44`. **Time is first and dt second; the consult had them reversed.** |
| S3 | `ScSLOSIAnalogControllerAction::Fire` @`0x61406f` | `Input(C1, VK, state, f, f)`. VK is one of {0x0D, 0x20, 0x25..0x28}, from table `0x703318`. The cheat table `0x7032ec` applies only if the INI holds `[Cheat] Cheat=NoGreenCapes` or `[Misc] Cheat=LEGOTester`, and the install INI has neither. The two floats are read past the end of the trigger object (heap garbage) and the script never reads them, so the port passes 0.0f. |
| S4 | `GcKeyMapper::ProcessKey` @`0x5d1d0f`/`0x5d1de2` | `Input(C1, mapperId 0xe3..0xfb, state)`. With no Toa or while paused, only 0xfb (Esc) is forwarded. The 25 pairs are listed below. |
| S5 | `GcMouseDriver::Process` @`0x55c753` | `OnEvent(26, x, y)` every frame. x and y are client pixels at the game resolution; the scale factor is 1.0. |
| S6 | same function, 12 sites | `OnEvent(23 press / 24 held / 25 release, button 1..4)` |
| S7 | `GcTimer::Trigger` @`0x445bca` | `OnEvent(8, timerId, float currentTime)` |

**Mapper pairs** (`GcKeyMapper` ctor `0x5cef80`):

| Id | Input | Id | Input |
|---|---|---|---|
| e3 | W | ea | LMB (0xfc) |
| e4 | S | eb | Q |
| e5 | A | ec | Tab |
| e6 | D | ed | Shift |
| e7 | Space | ee..f7 | digits 1..9, 0 |
| e8 | Ctrl | f8 | '-' |
| e9 | RMB (0xfd) | f9 | '=' |
| | | fa | '\' |
| | | fb | Esc |

Quirk: `GcKeyMapper::Process` loops 29 times over a 25-entry array.

**Action state machine** (`ScAction::IsRequested`, `0x491ed0`): while the key is down the state goes 1→2→4→4…; on release it goes back to 1. With mask 7 every state fires. Per key the stream is: 2 on the first frame down, 4 on every held frame, and 1 once on release.

**Within a frame:** controller triggers run at priority 0, the mouse and key mapper at priority 1, and the script `Process` at priority 2. Whether lower numbers run first is UNVERIFIED; read `ScProcessManager::Process` @`0x4a9170`.

## Process timing (`ScProcessManager`, `0x4a9170`)
- **Real time** (the script calls `userealdeltatime(1)`):
  - time = `SrGetSeconds(1)`;
  - if raw > 0.5, dt = 0.016666; otherwise dt = 0.07·raw + 0.93·prev, where prev is forced to 0.016666 while `sAreaLoops` < 10;
  - dt is capped at 0.066.
- **Fake time:** time += 0.0066667 per pass, and dt = 0.0066667.
- **Return value:** nothing or a non-int counts as true; an int counts as `!= 0`. A false result re-runs the process in the same frame, with the same time, dt and counter, up to 10 extra passes. Nothing stops the process, and the shipped script always returns nothing.

## Script side (Base.osi)
- `input(5)` @143462 builds `base_standardinput{controller, button, event, xposition, yposition}`. It skips dispatch when `cam.getaimode() == 9` and the button is not 251.
- `onevent(10)` @143620 builds `{eventid, args[9]}`.
- `process(3)` @143706 builds `{currenttime, deltatime, callcount}`. `deltatime` is read in 31 methods, which confirms the argument order.
- `xposition` and `yposition` are never read. The literal `event` is compared only with 1, 2 and 4.

**`gs_lego_screenfrontend.input`** @226534, active only while `menuactive == 1`:

| Input | Action |
|---|---|
| button 9 or 38, event 2 | scrollup |
| button 12 or 40, event 2 | scrolldown |
| 13, any event | forward |
| 27 (unless curscreen is −1 or 10) | back |
| 8 | back |

**`.onevent`** @225994:

| Event | Action |
|---|---|
| 26 | `mousex`/`mousey` → `setmenupointer` (cursor box ±16) → `checkhighlites` |
| 25, button 1 | activate |
| 25, button 2 | back |
| 6 | `resetcamera` (sent by `GcLegoCamera::Process`) |
| 23, 24 | ignored |

**Consequence (INFERRED, owner check):** vanilla never sends 27 (Esc arrives as mapper id 251) or 8, so on the PC front-end only a right-click goes Back.

## Mapping for the port (all deviations are flagged)
- **Keys:** a per-VK state machine over the 6 keys, called once per frame: `Input(C1, VK, state, 0.0f, 0.0f)`. SDL RETURN/KP_ENTER/SPACE/arrows; browser Enter/NumpadEnter/Space/Arrow*.
- **Esc:** the mapper path `Input(C1, 251, state)`.
- **Mouse:** `OnEvent(26, x, y)` every frame, with x and y converted from canvas CSS pixels to backbuffer pixels and clamped ≥ 0. Then 23/24/25 per button 1..4 (browser buttons 0/2/1/3 → 1/2/3/4). The original's +3/+22 border bias is dropped.
- **Taps shorter than a frame:** latched as one down frame (a deviation, under a flag).
- **Deterministic mode:** time = frame·dt_fixed.

## Open (UNVERIFIED)
1. The order of processes within a frame (`0x4a9170`).
2. Whether the key mapper's trigger context is processed on the menu (`0x4c2bf0`, `SetUseContext` `0x5d22a0`).
3. Whether the Toa is null on the menu (the A8 coverage report will show it).
4. Owner check that Esc and Backspace do nothing on the front-end and right-click is Back.
5. The exclusive-mode mouse clamp (irrelevant for the browser).
6. Joystick ids 9/12.
7. `GcTimer.mCurrentTime`.
