# HUD and host behaviour (M2a-3 asm rulings)

This file merges the two asm-ruling halves of M2a-3: Task 2a wrote §1, §2, §5, §6 and §8; Task 2b wrote §3, §4 and §7. Each half keeps its own labels and scratch-tool notes.
Binary: `LEGOBionicle.exe` (sha256 b372afb9…ab8), read as data with `objdump -d --no-show-raw-insn -M intel`. It was never run.

Labels:
- **EXECUTED**: read from the disassembly at the cited address, or measured with a script run (the command is named).
- **INFERRED**: a reading that no run or census closed. The reason is stated.

Function extents come from flow-following the binary to every `ret` and the next entry, checked against `.exc`, never from the decompiler's size. Scratch tools (not committed): `ext.py` (extent and call census), `spline.py` (emulator of `Accelerate`/`Interpolate`), `fevt.py`/`fevt2r_w.py` (F_evt, F_evt2 sweep), `pins2.py` (pins), `viewpin0.py` (VIEW/PROJECTION), `reach.py`/`reads.py` (call closures), `wall.py` (first-round §8, handler natives dropped; superseded). Committed: `tools/osi/menu_probe.py` (§8).


### Task 2b conventions (§3, §4, §7)

**Labels.** EXECUTED means read from `objdump -d --x86-asm-syntax=intel` of the authoritative exe (natives.md §1.4; sha256 b372afb9…3ab8), with the instruction address given. INFERRED means read from the Ghidra decomp, or reasoned from it, without an asm pin. D3D8 slots: SetTransform +0x94, SetMaterial +0xa8, SetRenderState +0xc8, GetRenderState +0xcc, SetTexture +0xf4, Get/SetTextureStageState +0xf8/+0xfc, DrawPrimitive +0x118, DrawIndexedPrimitive +0x11c, SetVertexShader +0x130, SetStreamSource +0x14c, SetIndices +0x154, CreateVertexBuffer +0x5c, CreateIndexBuffer +0x60. RS and TSS numbers follow `engine/include/lomn/d3d/device8.h`.

**Function extents.** Two functions here run past the decomp's `size`, and every census below uses the true extent:
- `GcBigSprite::GcBigSprite` 0x43f210: decomp 4,191 bytes; it really runs to `ret 0x1c` at **0x440455** (next entry 0x440460, `LoadHudSprite`).
- `ScTileFont::ScTileFont` 0x626b40: decomp 3,848 bytes (ends 0x627a48); it really runs to `ret 0x4` at **0x627b89** (next entry 0x627b90, `SsFontCharacter::~SsFontCharacter`). The `Close`/`UnMountVolume` tail at 0x627b2c–0x627b4a lies past the decomp size.
- `ScTileFont::Draw` 0x627dd0 ends at `ret 0x10` 0x6288e9; `ScDrawableTexture::Load` 0x5561c0 at `ret 0x4` 0x556961.

**Stop condition.** Not triggered. No HUD path draws through `ID3DXSprite`/`ID3DXFont` or with RHW vertices. The only D3DX call on the path is `D3DXCreateTextureFromFileInMemory` (0x5563ab, texture type 3 in `ScDrawableTexture::Load`), which creates a texture and draws nothing. Every HUD vertex format is untransformed: 0x112 for sprites and glyphs, 0x42 for lines and filled rectangles.

## §1 Camera (GcLegoCamera, front end)

### §1.1 Extent and entry (EXECUTED)
- **Extent.** `GcLegoCamera::Process` runs from 0x451990 to the last instruction, a `jmp` at 0x45ab96, so it ends at 0x45ab9b.
  - The next entry is `GcCharacter::GetVelocity` 0x45aba0, which is also the next `.exc` entry.
  - Size: 0x920b = 37,387 B. The decompiler gives 37,133 B.
  - Rets: 0x451a60, 0x4531ea, 0x453251, 0x45a90d, 0x45aa68 (all `ret 0x14`).
- **Arguments:** Process(double time `[ebp+8]`, double dt `[ebp+0x10]`, ScRenderNode*). It is called from `ScRenderNode::Process` 0x4ac9b0 at 0x4acb03, through camera vtable 0x707730 +0x1c.
- **Pause gate** (0x451a2b–0x451a60):
  - Process returns early when `[0x7032c4]` ≠ 0 and the mode ≠ `[0x70661c]` (= 0xd).
  - When the flag is 0, dt is stored to `[0x706398]` (0x451a6f).
  - `[0x7032c4]` has exactly two writers: `GcGame::DoSetUp` 0x436a20 (writes 0) and `GcGame::PauseGame` 0x43af3c.
  - `PauseGame` is called from 0x4368eb (the `gcgame.pausegame` native, INFERRED from its address in the native-body range), 0x537ef2 and 0x537f27.
- **Mode dispatch:** byte `[cam+0x3a1]`, compared at 0x451a7b (1), 0x451ae1 (0xd), 0x452346 (0), 0x452399 (2), 0x452a26 (3), and others.

### §1.2 Field table (EXECUTED)
`SetCameraSpline` 0x45b380 runs to a `ret 0x14` at 0x45b3fc. `SetTargetSpline` 0x45b400 ends with `ret 0x18`.

| Field | Meaning | Writer |
|---|---|---|
| `+0x3a4` | spline object (ref-counted) | SetCameraSpline / SetTargetSpline |
| `+0x3a8` | duration, double = (double) f32 argument | both |
| `+0x3b0` | start time, double = `SrGetSeconds(0)` | 0x45b3d5 (camera), 0x45b45e (target); also SetCameraAIMode 0x45b53d–0x45b545 for modes 2 and 3 |
| `+0x3b8` | camera spline index (u8 in use) | SetCameraSpline arg 2 |
| `+0x3bc` | target spline index | SetTargetSpline only (0x45b44c) |
| `+0x460` | forward flag (1 = forward, 0 = reverse) | both |
| `+0x461` | look-ahead flag | both |
| `+0x150` / `+0x15c` | eye / target | SetCameraPosition 0x405e50 / SetTargetPosition 0x405f00 |
| `+0x174` | direction = target − eye; dirty `+0x1c4` = 1 | both setters; AddYaw |

- **`setcampos` / `settargpos`, for `Z camera`** (EXECUTED).
  - `setcampos` 0x44e8a0 takes (cam, x, y, z): the cam pointer is in arg slot `[ebp+0x10]`, the floats in `[ebp+0x14/0x18/0x1c]`. It calls `SetCameraPosition` 0x405e50 (0x44e8e3) and returns nothing `{0xf,0}`.
    - Writes: `+0x150/+0x154/+0x158` = (x, y, z); then `+0x174/+0x178/+0x17c` = `+0x15c..` − `+0x150..`; then `+0x1c4` = 1.
  - `settargpos` 0x44e900 has the same shape and calls `SetTargetPosition` 0x405f00 (0x44e943).
    - Writes: `+0x15c/+0x160/+0x164` = (x, y, z); then the same `+0x174..` = target − eye; then `+0x1c4` = 1.
  - Each operation is an f32 store (`fstp dword`).

- **`SrGetSeconds` 0x4fa460.** With argument 0 and fake time (`[0x712d78]` == 0), it returns `[pm+8]`, the pass time (0x4fa497). So the start time is the time of the pass in which the script called the setter.
- **The front-end call** (EXECUTED in the oracle trace of frames 0–5, script frame 3):
  - `setcameraaimode(cam, 3)`;
  - `setcameraspline(cam, spline, 0, 4.0, 1, 1)`: index 0, forward, look-ahead;
  - `settargetspline(cam, spline, 0, 3, 4.0, 1, 1)`: camera index 0, target index 3.
- **Menu clicks** (controller input from Task 1, EXECUTED in the oracle):
  - entry 1 sets index 48 with duration 4.0;
  - Options sets index 6 with duration 5.0;
  - entry 3 sets index 0 with duration 4.0 and flags 0,1.
- **`SetCameraAIMode` 0x45b490** (to 0x45b5cd):
  - writes the mode byte;
  - if the mode < `[0x706fd8]` (= 0xf) and is not one of 0xa, 8, 6, 0xc, 5, 9, 0xe, 4: mode 0xb sets `[+0x454]` = 1 and `[+0x2f1]` = 0xb; modes 2 and 3 reset start = `SrGetSeconds(0)`;
  - **release** (0x45b558–0x45b5c0): when the old mode is 2 or 3 and the new mode is neither `[0x706fdc]` (2) nor `[0x706fe0]` (3), the spline's reference count is decremented and `+0x3a4` = 0.

### §1.3 Following the spline (EXECUTED)
**Mode 3, forward** (`[+0x460]` ≠ 0, 0x452a33):
- elapsed = time − `[+0x3b0]`. The `fsub` runs at the FPU's precision control (24-bit in the game) and is stored as a double (0x452a40–0x452a49).
- If elapsed > `[+0x3a8]`, then elapsed = duration (0x452a55–0x452a6e).
- param = f32(elapsed / duration) (0x452a74–0x452a86).
- Camera: Interpolate on `[+0x3b8]`+2 → z, +1 → y, +0 → x. Only the low byte of the index counts: `mov al` at 0x452afe, and `movzx` in the wrapper at 0x61a6e8. Then `SetCameraPosition` (call at 0x452b41).
- Target: the same with `[+0x3bc]` and the same param, then `SetTargetPosition` (0x452be9). This does not depend on `[+0x461]`.
- **Fire** when elapsed ≥ duration: `fcomp`; `test ah,5`; `jne` (0x452bfa–0x452c05). Then start = time (0x452c0b–0x452c14) and `OnEvent(6, cam, 0)` (call at 0x452d24).

**Mode 2, forward** (0x4523a6–0x4526ce): the same clamp and param.
- With `[+0x461]` set, the target comes from a look-ahead on the *camera* splines: param2 = (elapsed + 0.01 `[0x706728]`) / duration, then `SetTargetPosition` 0x452593.
- Without `[+0x461]`, the target is not set.

**Reverse** (mode 2: 0x4526d8→0x452a11; mode 3: 0x452d30→0x453032):
- elapsed = time − start, clamped to the duration exactly as in forward (mode 2: 0x4526d8–0x45270c; mode 3: 0x452d39–0x452d6a);
- then **rem = duration − clamped elapsed**, stored back into the same local (mode 2: 0x452712–0x452724; mode 3: 0x452d70–0x452d76);
- param = rem / duration; the mode-2 look-ahead subtracts 0.01 (`[0x706730]`);
- **fire when rem ≤ 0.0**: mode 2 at 0x4528d5 (`fcomp [0x706738]`); mode 3 at 0x452f04/0x452f10 (`[0x706740]`; `test ah,4` / `test ah,0x41`).

**The four `OnEvent(6)` guards** (mode 2 and mode 3, each forward and reverse) read only time, start, duration and the mode and flag bytes. None reads Toa, physics or collision state. **F18 is confirmed, so the stop condition does not fire.** A fifth `OnEvent` call at 0x45502f lies in the branch gated by `cmp byte [cam+0x3a1], 8; jne 0x455600` (0x454e86–0x454e8d), so it runs in **mode 8 only**. No jump from outside lands inside 0x454e8d–0x45502f in the linear listing (EXECUTED; jump tables were not scanned). That `+0x3a1` is the current-mode byte is INFERRED from `SetCameraAIMode`'s write.

**The `OnEvent(6)` arguments** (0x452c1a–0x452d24; the VM is `[0x83877c]` and the name "OnEvent" is at 0x7072f8):
- `CreateVariant(6)` 0x413ed0 (int, `movzx`);
- `CreateVariant(cam)` 0x447500: +1 reference at 0x447530, type id cached in `[0x83b362]`, and no reference for null;
- 0x407960(0) (int 0);
- the rest are nothing `{0xf,0}`.

**After `OnEvent` returns**, Process jumps straight to the common tail 0x45a910 (0x452d29). It reads no spline or mode field again, so a mode change inside the handler is safe in that frame.

### §1.4 What the script does inside OnEvent(6) (EXECUTED, oracle `inj_natives.py`: OnEvent(6) injected at frame 605, real natives, native records kept while injecting)
- The handler calls `setcameraaimode(cam, 0)` and `gchuddirector.show(141)`; F2 named only `startnewscreen`.
- **What the camera does** (EXECUTED by reading 0x45b490 and 0x452346):
  - The mode becomes 0 and the spline is released (old mode 3, new mode 0).
  - The eye and target keep the values written in the firing frame. Elapsed was clamped to the duration there, so param = 1.0, the spline's end point.
  - Mode 0 (0x452346): `[+0x470]` is the shake flag. Its only 1-writer is `ShakeCamera` 0x450f8f; the ctor 0x44f8b1 writes 0. While it is 0, Process jumps to 0x45ab60 → the tail 0x45a910, which only passes the eye to 0x48eb40, 0x47d660, 0x487e90, 0x485bf0 and 0x484550 (listener and area updates) and returns 1.
  - **Ruling:** after the event, the camera is frozen at the spline's end.
- **OnEvent(6) is sent exactly once on the front end.** The real handler sets mode 0. From then on Process takes the frozen path 0x452346 → 0x45ab60 → the tail 0x45a910, which contains no `OnEvent` call. The four spline guards are only reached in modes 2 and 3. So no further OnEvent(6) is sent until a script sets mode 2 or 3 again, as a menu click does (§1.2).
- *Stand-in case only* (a handler that leaves the mode at 3, e.g. a test stub): the path restarts from start = time, and the next fires come at 1205 and 1805 (pc24) (EXECUTED, `fevt.py`). This does not happen with the real handler.

### §1.5 Spline maths (EXECUTED)
- **SLK loader** (`ScSLKManager::GetSLKFile` 0x4032e0, called from 0x61a580):
  - buffer = size + 0x98, and the file is copied to object+0x98;
  - relocation trailer: the last u32 = 0xc0ffee, the u32 before it = N, then N offsets; each u32 at an offset gets += base (0x403442–0x40346f);
  - file+0 = spline count; file+4 → record array `[obj+0x9c]`; records are 12 B `{u32 knots, u32 → knot array of (f32 t, f32 v), u32 0 → y2 slot}`.
  - FRNT_CAM.slb: 5,012 B, 60 splines, 61 relocations.
- **`Interpolate` wrapper 0x61a6e0:** record = `[[this+8]+0x9c]` + 12 × u8 index, then calls 0x443a70.
- **`ScCubicSpline::Interpolate` 0x443a70** (to 0x443b45, `ret 4`) is Numerical Recipes `splint`:
  - binary search with lo = 0, hi = n−1; when knot_t > t, hi = mid, else lo = mid;
  - **no clamp**, so it extrapolates on the end segment;
  - h = t_hi − t_lo; A = (t_hi − t)/h; B = (t − t_lo)/h;
  - y = A·y_lo + B·y_hi + ((A³−A)·y2_lo + (B³−B)·y2_hi)·h²·`[0x704204]` (f32 1/6 = 0x3e2aaaab);
  - every operation is rounded at the FPU precision; y2 is f32.
- **`Accelerate` 0x4435f0** is NR `spline()` with natural ends.
  - Extent: it runs to `ret` at 0x4438cc **plus a tail chunk** 0x4438d0–0x4438d2 (`fcompp; jmp 0x4438b1`), reached from 0x44381f.
  - y2[0] = u[0] = 0 (`[0x7041ec]`); constants 2.0 (0x7041f0), 1.0 (0x7041f4), 6.0 (0x7041f8); y2[n−1] = 0 (0x7041fc).
  - y2 and u are stored as f32.
  - Then a first-derivative pass with h = 1e-4 (`[0x704200]`), stored as f32 at entry+4.
- **`AfterConstruction` 0x4435b0** calls `Accelerate` for each record. The counter is a **byte**: `mov byte [ebp-8],0` (0x4435b9), `inc byte [ebp-8]` (0x4435d2), `cmp byte [ebp-8], dl` against `[this+0x98]` (0x4435db, `jb`). So a file with 256 or more splines would loop forever, and only the low byte of the count matters (FRNT_CAM has 60).
- **`GetFirstDerivative`**:
  - Wrapper: `ScSLOSISLKCubicSpline::GetFirstDerivative` 0x61a750 (to `ret 8` at 0x61a770). It takes record = `[[this+8]+0x9c]` + 12 × u8 index, then calls `ScCubicSpline<float,float>::GetFirstDerivative` 0x61a7d0 (to `ret 4` at 0x61a859). Its caller is 0x61a4a5, the native body.
  - Search: the same binary search as `Interpolate` (`test ah,0x45` at 0x61a7f9: knot_t > t → hi = mid), with no clamp.
  - Formula: h = t_hi − t_lo, A = (t_hi − t)/h, B = (t − t_lo)/h, then **d = A·d_lo + B·d_hi** (0x61a845–0x61a84f). Here d is the f32 at entry+4 of the 8-byte entries `[[rec+8]+8]`, the derivative tabulated by `Accelerate`'s 1e-4 pass. So the result is a *linear* interpolation of tabulated derivatives, not the analytic spline derivative.
- **Time range:** `GetMinTime` 0x61a710 → 0x61a870 returns `[knots+0]` (the first knot t). `GetMaxTime` 0x61a730 → 0x61a860 returns knot n−1. On FRNT_CAM, splines 0–5 run over t ∈ [0, 1].

**Pins** (EXECUTED, `spline.py` / `pins2.py`; "textbook" = a third-party natural cubic spline in exact rationals; rel = |pc24 − textbook| / |textbook|):

| Case | t | pc24 | pc53 | textbook | rel |
|---|---|---|---|---|---|
| synthetic, knots (0,0) (0.25,1) (0.5,0) (1,2) | 0.125 | 0.72826087 `3f3a6f4e` | `3f3a6f4e` | 0.7282608696 | 7.1e-9 |
| same | 0.75 | 0.34782606 `3eb21642` | `3eb21641` | 0.3478260870 | 6.7e-8 |
| FRNT_CAM 48 | 0.10 | −16.711603 `c185b15d` | same | −16.7116027152 | 2.7e-8 |
| FRNT_CAM 48 | 0.28 | −6.5329628 `c0d10e08` | same | −6.5329629419 | 2.2e-8 |
| FRNT_CAM 48 | 0.44 | 11.726918 `413ba175` | same | 11.7269179274 | 2.5e-8 |
| FRNT_CAM 48 | 0.75 | 54.767616 `425b120a` | `425b120b` | 54.7676202591 | 7.3e-8 |
| FRNT_CAM 1 | 0.13 | 15.777580 `417c70f8` | same | 15.7775800904 | 1.1e-8 |
| FRNT_CAM 1 | 0.86 | 15.679373 `417adeb6` | `417adeb5` | 15.6793721837 | 3.9e-8 |

- The synthetic pins belong to the last knot **(1, 2)**. With (1, 1) the values would be `3f38590b` / `bcb21640` (pc24).
- On the (1, 2) case, linear interpolation gives 0.5 / 1.0 and Catmull-Rom 0.5625 / 0.9375, so a wrong model fails these pins. On (1, 1), linear gives 0.5 / 0.5.

### §1.6 F_evt and F_act (EXECUTED, `fevt.py`, `fevt2.py`)
- **Time.** t_n = round24(t_{n−1} + dt), with dt = 0.006666666828095913 (`[0x712d68]`).
- **Front end:** the spline is set in script frame 3 and the duration is 4.0.

| Precision | First fire | Elapsed at the fire | Next fires |
|---|---|---|---|
| pc24 (the game) | **604** | 4.0066633 | 1205, 1805 |
| pc53 | 603 | 4.0000001 | |

- This holds under both the port's time origin and the asm origin (§2.3). The port's K bits match: frame 3 `3cda740e`, 602 `4080a3d0`, 603 `4080da6d`, 604 `4081110a`.
- **F_act = F_evt + 1 = 605.** The viewport camera node (priority 3) runs after the script (priority 2) in the same pass (§2.4), and `OnEvent` is synchronous. So the script sees the event inside frame 604's camera step, and its first `Process` after it is frame 605.
  - Oracle check: injecting `OnEvent(6)` before `Process` at 605 runs the handler's pc 230733 in frame 605.
  - **Open assumption (m4):** this needs the viewport node `[vp+0x30]` not to be also a child of a render-node group that runs earlier. `ScRenderNodeGroup::Process` 0x4ab990 runs its own `ScRenderNode::Process` (0x4ab9a8) and then its children (0x4a97d0).
    - EXECUTED: the viewport ctor registers the node only with `ScCameraDirector::AddCountable` 0x494070 (0x447f3a), `AddProcess(…, 3)` (0x447f53) and `ScScreen::AddViewPort` 0x49d2d0 (0x447f65). No group attach is there.
    - Not done: a census of every child-attach site in the binary. INFERRED mitigation: `ScRenderNode::Process` gates on `[cam+0xc]` == parity, so the camera steps at most once per pass whichever caller reaches it first. Only the *order* relative to the script would change.
- **Options path (F_evt2)** (controller input (1), duration 5.0). The click is released at script frame F, `setcameraspline(6, 5.0)` runs at F+38, and the event fires when elapsed ≥ 5.0. f32 time resolution steps at 8, 16 and 32 s, so the offset changes with the absolute time. **No constant offset holds; compute it.**
  - Sweep: `fevt2r_w.py` (pc24, F = 4..11999, exact boundaries):

| Duration | Origin | F_evt2 − (F+38) by release frame F |
|---|---|---|
| 5.0 | port (frame k at t_{k+1}) | 751 for 4–420 · 750 for 421–1981 · 751 for 1982–4200 · 750 for 4201–11999 |
| 5.0 | asm (call k at t_{k+3}) | 751 for 4–418 · 750 for 419–1979 · 751 for 1980–4198 · 750 for 4199–11999 |
| 4.0 | port | 601 for 4–569 · 600 for 570–2057 · 601 for 2058–4312 · 600 for 4313–11999 |
| 4.0 | asm | 601 for 4–567 · 600 for 568–2055 · 601 for 2056–4310 · 600 for 4311–11999 |

  - **M2 click region (F ≈ 665–725):** 750 under both origins, so F_evt2 = F + 788 and F_act2 = F + 789. That pin stands.
  - The front end's 601 (= 604 − 3, set in frame 3) falls in the first 4.0 band.
  - At pc53 the offset is 750 (5.0) / 600 (4.0) for every F.
  - Port implementations must accumulate f32 time and compare, never add a constant.

### §1.7 View and projection (EXECUTED unless marked)
- **`Render` 0x45ad10:**
  - builds the projection when `[+0x105]` is set (vtable +0x3c = `BuildProjectionMatrix` 0x4953f0);
  - builds the view when `[+0x1c4]` is set (0x45ad70, extent to 0x45b358);
  - then calls `ScRenderer::SetProjectionTransform` 0x530640 (`+0x108`) and `SetViewTransform` 0x530560 (`+0x184`).
- **View:**
  - Modes 1, 6, 13, 12 (`[0x706f80..8c]`): target = eye + `[+0x3ec]`.
  - Otherwise: `[+0x3ec]` = `[+0x174]` and the target is unchanged.
  - Always `D3DXMatrixLookAtRH`(eye `+0x150`, at `+0x15c`, up (0,1,0) from 0x706fa8). The LH path at 0x45af50 is dead (`mov al,1`).
  - Then `SetTransform(2 = VIEW)` through `device[0x73232c]` vtable +0x94 (0x45b04f).
  - Modes 1/6 add RotateX(byte `[+0x404]` × 5°); modes 0xd/0xc add RotateX(`[+0x2e0]`).
- **`addyaw` has no visual effect on the front end** (EXECUTED reads, INFERRED conclusion). `addyaw` 0x44ecb0 → `AddYaw` 0x45c2b0 (to 0x45c3c5) rotates only `+0x174` by −deg × 0.017453292 about Y and sets dirty. It never writes `+0x150` or `+0x15c`.
  - In modes 3, 2 and 0, LookAtRH reads only the eye and target.
  - In mode 3, the camera's own step (priority 3, after the script) also overwrites `+0x174` every frame.
  - So the sway changes nothing on screen. F10's "the background moves every frame" is refuted for these modes.
  - Controller input (4): `addyaw` runs only in frames 10–257 on the click run. That does not change the ruling.
- **Projection:** `BuildProjectionMatrix` 0x4953f0 calls `D3DXMatrixPerspectiveFovRH`(fovy `[+0x148]`, aspect `[+0x14c]`, zn `[+0xfc]`, zf `[+0x100]`), then `SetTransform(3)` (0x4954c2), and stores the result at `+0x108`.
  - The `GcViewPort` ctor 0x4479f0 creates the camera (0x4b8 B) with fov 72.0 (`[0x704e00]`) × 0.017453292 (`[0x70d508]`), aspect = width/height, near 0.01 (`[0x704e1c]`), and far = `GetDrawDistance` = `[0x74a328]` 4700.0.
  - The only writer of the far plane is `SetDrawDistance` 0x5e5280 (called from 0x5e4ec4).
  - D3DX comes from `d3dx8cw.dll`. Its internal rounding is **UNVERIFIED**; the pins use the documented formulas.
- **Pin at F_act + 60 = frame 665.** The camera is frozen at the spline's end (§1.4); eye and target = splines 0–2 / 3–5 at param 1.0 (pc24 = pc53).

| Item | Value | Bits |
|---|---|---|
| eye | (−20.7616, 15.5486, 110.639) | `c1a617c2 4178c711 42dd472b` |
| at | (−20.7616, 15.5486, 102.257) | `c1a617c2 4178c711 42cc8396` |
| VIEW | identity rotation, translation (20.7616, −15.5486, −110.639) | `3f800000 0 0 0  0 3f800000 0 0  0 0 3f800000 0  41a617c2 c178c711 c2dd472b 3f800000` |
| PROJECTION, 640×480 | fovy 1.2566371 `3fa0d97c`, aspect `3faaaaab` | `3f8421f6 0 0 0  0 3fb02d48 0 0  0 0 bf800012 bf800000  0 0 bc23d721 0` |

## §2 Frame order (EXECUTED per claim unless marked; the camera-node identity has one open assumption, §1.6 m4)

### §2.1 Main loop
- `AppMain` 0x5381f0 calls `DoSetUp` (0x53867d), then `pm->vtable[3]` = `Start` 0x4a9070 (0x53868a).
- `Start` sets `[pm+0x10]` to the real time and calls `Process` once: **pass 0**.
- The loop at 0x538700 then calls `Process` (0x538791) and `RenderLoop` 0x49d550 once per frame.
- pm is a stack object (vtable 0x713048, global `[0x712d90]`).

### §2.2 `ScProcessManager::Process` 0x4a9170 (to 0x4a9796)
- **Time:** fake time is `[pm+8]` += `[0x712d68]` (0x4a92c8–0x4a92e3). Real time applies smoothing and a 0.066 cap.
- **Parity:** `[pm+0x40]` toggles (0x4a92f5–0x4a930a).
- **Walk:** the map at `pm+0x30` (begin 0x4de1d0, ++ 0x4ddd80).
  - A process runs only if `[proc+8]` == parity and `[proc+9]` == 0, via `vtable+8`(time, dt).
  - Then `[proc+8]` = !parity.
  - A false return queues up to 10 re-run passes.
- **Counter:** `[pm+0x44]`++ at 0x4a95b6.
- **Pending adds** (`pm+0x4c` list) are inserted in list order (0x4a9e70 → 0x4decd0); removals (`pm+0x58`) follow.
- **The map:** 6 buckets (`push 6` at 0x4a8d66), bucket = priority % 6, never resized.
  - Insert links the node at the head of its bucket, or before the first node with an equal key, so **the latest-added runs first within a priority**.
  - The priorities in use are 0, 1, 2, 3 and 5, so the walk is in **ascending priority**.
- **`AddProcess` 0x4a8f50:** key = the priority byte (`movzx` at 0x4a8f60); it sets `[proc+9]` = 0.

### §2.3 Start-up offset (EXECUTED; a finding for the controller)
- `[proc+8]` starts at 1 (`ScProcess` ctor 0x4a82f0 → 0x4aa7b0; the camera's own `[cam+0xc]` = 1 from `ScRenderObject` 0x4b46d0). pm parity starts at 0 (0x4a8d7a) and `[pm+8]` = 0.0.
- Pass 0 runs nothing and inserts every queued add. Pass 1 has parity 0, so everything is skipped. **Pass 2 is the first run.**
- So the original's script call k runs at time t_{k+3} with counter k+2. The port's frame k uses t_{k+1} (K0 = `3bda740e`) with counter k.
- This offset already exists in M2a-2 and in the oracle (`event_probe` frame 0 time = dt). It does not move F_evt (604 under both origins, §1.6).
- A process added during pass p first runs at p+2 if p is even, or p+1 if p is odd. For example, `gcmp3player.setupforplay` adds a priority-2 process (0x5dce0c) in script frame 3.

### §2.4 Registration and walk order
`DoSetUp` calls, in order: `SetUpControllers` 0x436bb4, 0x4395d0, `SetUpCamera` 0x436bbe, `SetUpDatabase` 0x436bc3, `SetUpView` 0x436bc8, `SetUpScripting` 0x436bcd (`Setup()` at 0x43925a runs before the script's `AddProcess` at 0x4392f8), and `SetUpCollisions` 0x436bd2.

| Add site | Process | Priority |
|---|---|---|
| 0x4394a4 | GcControllerProcess | 0 |
| 0x43955a | GcMouseDriver (ctor 0x55c010) | 1 |
| 0x4395b7 | GcKeyMapper (0x5cef80) | 1 |
| 0x43996e | SetUpCamera ScRenderNode 'htro' | 1 |
| 0x439aa4 | `[0x83aa28]` ScRenderNodeGroup | 2 |
| 0x47b592 | HudDirector `[0x83a534]` (via 0x439ecb) | 2 |
| 0x43a28f | render group | 5 |
| 0x43a2bd | GcConversationEngine | 1 |
| 0x447f53 | viewport node `[vp+0x30]` (camera) | 3 |
| 0x568262 | Toa (`addtoa`, inside `Setup`) | 2 |
| 0x4392f8 | script | 2 |
| 0x43a4a2 | GcBAGroup | 1 |
| 0x43a4c5 | GcCollisionPhysicsGroup | 0 |

**Walk order in one pass:**
- **0:** CollisionPhysicsGroup, ControllerProcess.
- **1:** BAGroup, ConversationEngine, camera node 'htro', KeyMapper, MouseDriver.
- **2:** later adds (e.g. mp3), script, Toa, HudDirector, `[0x83aa28]`.
- **3:** the viewport camera node, which runs `GcLegoCamera::Process`. EXECUTED: `[node+0x1c]` is called at 0x4acb03, and `gcviewport.getcamera` → 0x448080 reads the same `[[vp+0x30]+0x1c]`. INFERRED/open: that this is the only route by which the camera is stepped (no child-attach census, §1.6 m4).
- **5:** the render group.

So, within a frame, VK triggers (priority 0), key-mapper and mouse events (priority 1) reach the script before its `Process`, and the camera's `OnEvent(6)` comes after it (EXECUTED map order; INFERRED for the camera, see m4). The order is readable from the binary, so that stop condition does not fire.

## §3 Render path

### 3.1 Frame and pass order

- **The viewport list (EXECUTED).** `ScScreen::AddViewPort` and `MoveViewToFront` insert at the sentinel at screen+0x2a3fc. `insert_nodes` 0x4e0f30 inserts before the position, so both calls are a push_back. `RenderLoop` 0x49d550 starts at the head [screen+0x2a404] and follows node+8. The node layout is {data +0, prev +4, next +8}.
- **The main view is added first (EXECUTED).** `SetUpView` calls the GcViewPort ctor at 0x43a54c, and the ctor calls AddViewPort at 0x447f65.
- **The HUD view is added second and drawn last (EXECUTED).** The HUD view is an ScViewPort (vtable 0x7037c8) with id 'HdDr' 0x48644472, stored at [0x8390ac]. AddViewPort at 0x43a85a adds it, then MoveViewToFront at 0x43a872 puts it at the end. **The HUD is drawn after the area, every frame.** The script cannot reorder or add viewports: `gcviewport.moveviewtofront` 0x447920, `gcviewport.moveviewtoback` 0x447960 and `gcviewport.create` 0x447470 are all `called_by_base_osi: false` in natives.json.
- **DoSetUp order (EXECUTED).** SetUpRenderer 0x436bb9, SetUpCamera 0x436bbe, SetUpDatabase, SetUpView 0x436bc8.
- **RenderLoop per frame (INFERRED, decomp 0x49d550).** Clear(…, 0xff000000, …), then BeginScene, then each viewport's vfunc+8 (`ScViewPort::Render`) in list order, then EndScene and Swap.
- **The HUD viewport (EXECUTED).**
  - It has no clear of its own: byte +0x3d = 0 (store at 0x43a800).
  - Its rect runs from ([0x7033d8], [0x7033d4]) = (0.0, 0.0) to the rounded screen width and height.
  - Camera +0x30 = ScCameraDirector countable 'orth' 0x6f727468.
  - Renderer +0x34 = ScRendererDirector 'HdDr', created in SetUpRenderer with SetRenderMode(0x8c034, 1). The main renderer's mode is 0x10635.
  - Root +0x38 = ScRenderObjectDirector countable 'HdDr' = `sMainHudNode` [0x83a534]. It is an ScRenderNodeGroup (0x50 bytes, flags 0x11) registered by GcHudDirector::Init at 0x47b57d, and it is also a process at priority 2 (0x47b592).
- **ScViewPort::Render (INFERRED, decomp).** In order:
  1. Renderer Start: `SrPushRenderMode(mRenderMode)`.
  2. SetViewPort → PlatformViewSet. Not read in asm.
  3. The camera node's vfunc+0x24 → `GcOrthoCamera::Render`.
  4. `ScRenderer::Render(root)`.
  5. Renderer Finish: LightEnable(i, 0) for 8 lights.
- **`ScRenderer::Render` 0x49a870 (INFERRED, decomp; callbacks EXECUTED).**
  - It sets priority 0 and calls root+0x24. A return other than −1 queues a ReRender at that priority.
  - It then walks the priority map in ascending order (a red-black tree). A priority with no callback renders each queued node's +0x24.
  - Priority 100 holds the ScTransparentRenderListCallback (vtable 0x70eec8), installed in the ScRenderer ctor with AddRenderPriorityCallback(100, …).
- **The transparent list is NOT sorted (EXECUTED).** `ScTransparentRenderListCallback::Render` 0x49ae00 builds a ScZOrderGreater from the camera position but never passes it. The call at 0x49aeb2 to `ScRenderListCallback::Render` 0x49ad70 pushes only (list, renderer). ReRender appends with push_back, so deferred HUD items draw in the order they were deferred.
- **ScRenderNodeGroup::Render 0x4abc60 (EXECUTED).**
  - It pushes [+0x3c] (the world matrix, if any) and renders [+0x1c] (the render object, if any).
  - It then walks the children vector (data [+0x4c], size [+0x48]) in index order.
  - A child renders only if its flags [child+0x14] have both bit 0x1 and bit 0x10 set.
  - There is no frustum cull, although the frustum is computed.
- **ScRenderNode (EXECUTED, ctor 0x4ac750).** Layout: +0x10 id, +0x14 flags, +0x1c render object, +0x20 parent, +0x3c world matrix = 0. `ScRenderNode::Render` 0x4acf40 calls object+0x24(node).

**Order and visibility of HUD items (INFERRED, decomp of GcHudDirector::Show 0x47b320 / Hide 0x47b3e0).**
- **Show.** If the node is not already a child (mainHud vfunc+0x40), Show sets its parent to mainHud (node+0x28) and push_backs it into mainHud+0x44. Showing a node that is already shown does not move it.
- **Hide.** Hide erases the node from the vector, which keeps the others in order, and calls +0x28(0).
- **Rule.** An item is visible exactly when it is in the vector. Draw order is Show order. A deferred (alpha) item draws after all non-deferred items, in the same relative order, because of the unsorted priority-100 list.
- **Clearing and wrapping.** GrInitHudDirector 0x47b150 clears everything. AddBigSprite 0x47b730 and AddTextObject 0x47b780 wrap the object in a new ScRenderNode (0x44 bytes, flags 0x11, id '????') and assign sNextHudElementID.

### 3.2 The HUD pass's base states, `SrPushRenderMode` 0x531100 (EXECUTED; all through ScDxStates)

**ScDxStates (EXECUTED).**
- `SetRenderState` 0x64b1e0 re-reads the device on every call (GetRenderState at 0x64b201). It compares that value with the requested one (0x64b217) and calls Set only when they differ (0x64b239). `SetTextureStageState` 0x64b250 does the same per stage.
- Only `RestoreRenderState` 0x64b340 and `RestoreTextureStageState` 0x64b380 work from cached values. They compare without a device read (0x64b358) and then Set (0x64b36e).

**The mode push is a diff, so the HUD's states depend on the last mode pushed before it (EXECUTED).** HUD mode 0x8c034 has bits 0x80000, 0x8000, 0x4000, 0x20, 0x10 and 0x4.

What pushes modes:
- `SrPushRenderMode` 0x531100 has two callers in the whole listing: `ScRenderer::Start` 0x53042a, which pushes the viewport renderer's own mode at the start of each viewport, and `ScRenderer::SetRenderMode` 0x5308f6 (when its second argument is true). Nothing pops or restores a mode.
- `ScRenderer::SetRenderMode` is virtual (+0x10). Among the 50 functions that read `_sCurrentRenderer` [0x839214], only `GcRenderNodeGroup::Render` 0x516b30 calls +0x10 within the 40 instructions after the read. That census is a floor: it would miss a call made through another register copy.
- `GcRenderNodeGroup::Render` pushes the group's mode [+0x50] with argument 1 before rendering its children (0x516bcf). The pushed value is:
  - wireframe: (mode | 0x2000) & ~0x4000 (0x516b4a, push 0x516b5b);
  - `gUseCaustics` [0x7032a8] set and (mode & 1 or mode & 0x200): mode | 0x40200 (0x516b80, push 0x516b8c);
  - caustics set otherwise: mode | 0x200 (0x516bae, push 0x516bba). The guard at 0x516ba4 ORs with 0x10030, so it is always non-zero;
  - else the mode as stored (0x516bca).
  - `gUseCaustics` is 0 in `.data`. Only `GcLightDirector::SetUnderWaterLighting` writes it: 1 at 0x48e7d7, 0 at 0x48e904.

The group modes, from `GcGame::SetUpDatabase` (`GcRenderNodeGroup::SetRenderMode` 0x516be0 stores +0x50):

| Group | Mode | Address |
|---|---|---|
| sMainBackNode | 0x10234 | 0x43a237 |
| sMainAreaNode | 0x14634 | 0x43a247 (ebx) |
| sMainNoLightNode | 0x14634 | 0x43a253 (ebx) |
| sMainNode | the main renderer's mode (`ScRenderer::GetRenderMode` 0x49a9b0 at 0x43a220, so 0x10635) | 0x43a25f |
| sMainTransNode | 0x10034 | 0x43a26b (edi) |
| sMainShadowNode | 0x4400 | 0x43a277 |

So at the HUD viewport's `Start`, `gLastRenderMode` = L, the mode of the last group rendered in the main view (any of the above with the OR variants), or 0x10635 if no group rendered. **Which group renders last on lev0/frnt is not read here; it is the open input Task 8 measures.** The diff rules below are what the port must apply against L. Every candidate L above is EXECUTED to lack 0x80000, so with that census the first block always fires.

The diff `SrPushRenderMode` 0x531100 applies for HUD mode 0x8c034 against L (EXECUTED):
- **0x80000 (HUD has it):**
  - If L lacks it, which is every candidate above: the alpha block 0x531128–0x53119a runs, all through ScDxStates:
    - TSS0 ALPHAOP(4) = 2 and ALPHAARG1(5) = 2 (TEXTURE).
    - ALPHABLENDENABLE(0x1b) = 1, SRCBLEND(0x13) = 5 (SRCALPHA), DESTBLEND(0x14) = 6 (INVSRCALPHA).
    - ALPHAREF(0x18) = 0, ALPHATESTENABLE(0xf) = 1, ZWRITEENABLE(0xe) = 0.
    - TSS0 COLOROP(1) = 4 (MODULATE) and COLORARG1(2) = 2 (TEXTURE).
  - If L has it, the block is skipped and the HUD inherits the device state.
  - The reverse block 0x5311b0–0x5311d5 (ALPHABLENDENABLE = 0, ALPHAREF = 1, ALPHATESTENABLE = 0, ZWRITEENABLE = 1) runs on the first push after the HUD, for a mode that lacks 0x80000.
- **0x40000 (HUD lacks it):**
  - If L has it, which happens only under caustics through `| 0x40200`: TSS1 COLOROP(1) = 1 (DISABLE) at 0x531242 and TSS1 ALPHAOP(4) = 1 at 0x531250.
  - Otherwise nothing. The "on" block 0x5311f1–0x531221 sets stage 1 for a mode that has the bit: COLORARG1 = 2, COLOROP = 5, COLORARG2 = 1, ALPHAOP = 1.
- **The rest are set on every push, not diffed:**
  - **ZENABLE(7):** bit 0x10000 is clear. If `GcGraphicsOptions::CanUseW` (0x531277), bit 0x8000 gives ZENABLE = 0 (0x53129d), so there is no depth test. Otherwise ZENABLE = 1 (0x531284). A mode with 0x10000 sets ZENABLE = 2 (W) when CanUseW (0x53126e).
  - **FILLMODE(8) = 3 (SOLID)** from bit 0x4000 (0x5312cd).
  - **LIGHTING(0x89) = mode & 0x1** (mask at 0x5312d6, set at 0x5312e8), which is 0 for the HUD.
  - **CULLMODE(0x16) = 1 (NONE)**, because bits 0x400 and 0x800 are clear (0x531324).
  - **Fog:** bit 0x200 is clear. If FOGENABLE(0x1c) reads non-zero (a raw GetRenderState at 0x53133a), `GcGraphicsOptions::SetFog(false)` runs (compare at 0x531362, `push 0` at 0x531368).
- **The state log** must record these as ScDxStates writes, gated on the device's current value, never as fixed per-pass values.

### 3.3 The HUD transform

- **The ortho camera (EXECUTED).**
  - SetUpCamera 0x439870 calls the GcOrthoCamera ctor 0x406df0 with:
    - pos 0x703364 = (0,0,0), target 0x703370 = (0,0,−1500), up 0x70337c = (0,1,0);
    - left [0x70338c] = 0.0 → +0x1cc, right = width → +0x1d4;
    - top [0x703388] = 0.0 → +0x1c8, bottom = height → +0x1d0 (stores 0x406e2c–0x406e4a).
  - Width and height come from GetResolutionWidth/Height (calls 0x4398e9 / 0x4398da).
  - Near = 0.0 (0x6fe604) and far = 15000.0 (0x6fe600) come through the ScCamera ctor 0x492df0 (+0xfc / +0x100).
- **BuildOrthoMatrix 0x406f20 (EXECUTED; the doubles 2.0 at 0x6fe610 for m00 (0x406f46), 0x6fe618 for m11 (0x406f66) and 0x6fe620 for m22 (0x406f86); the f32 1.0 at 0x6fe628).** It is built lazily: `GcOrthoCamera::Render` builds it only while the dirty byte +0x105 is set (0x406ecd). The ScCamera ctor sets that byte to 1 (0x492e41), and BuildOrthoMatrix clears it (0x40700c). l/r/t/b are written only in the ctor, so the matrix is built at the first HUD render and never rebuilt. Row-vector matrix:
  - m00 = 2/W, m11 = 2/(0 − H) = −2/H, m22 = 2/15000;
  - m30 = −1, m31 = +1, m32 = (f+n)/(f−n) = 1, m33 = 1;
  - all other entries 0.
  - Hence **x_ndc = 2x/W − 1, y_ndc = 1 − 2y/H, z_ndc = 1 + z/7500**. HUD space is in pixels, with the origin at the top-left and y pointing down.
- **Device calls (EXECUTED).** `GcOrthoCamera::Render` 0x406ec0 → `ScRenderer::SetProjectionTransform` 0x530640 → SetTransform(3, PROJECTION), and `SetViewTransform` 0x530560 → SetTransform(2, VIEW). Both go straight to the device [0x73232c], not through ScDxStates.
- **The HUD view matrix: identity, with −0.0 translation.**
  - **The ctor leaves identity at +0x184 (EXECUTED, re-review: ScMatrix ctor 0x4e1d50 then MakeIdentity 0x4e1fd0 on +0x184 at 0x405e15/0x405e23).** The ctor also sets the view-dirty byte +0x1c4 = 1 (EXECUTED 0x405e2b), so the first `GcOrthoCamera::Render` runs BuildViewMatrix (test at 0x406edd, call at 0x406ee8). The only other writers of +0x1c4 on a GcLookatCamera are SetCameraPosition (0x405ee7) and SetTargetPosition (0x405f97), plus BuildViewMatrix's clear (0x4065b7). The other callers of those two setters are GcLegoCamera methods, GcCinemaCamera::PositionCamera, and the script natives `gclegocamera.setcampos` 0x44e8a0 / `settargpos` 0x44e900 (calls at 0x44e8e3/0x44e943). The natives take a GcLegoCamera handle, so the HUD camera is not reachable through them (INFERRED), and the HUD view is built exactly once.
  - **Inputs (ctor arguments EXECUTED from the constants 0x703364/0x703370/0x70337c; field stores EXECUTED from the ctor asm, re-review: pos 0x405d4e…, dir fsub 0x405dcc/0x405dda/0x405de9).** Pos +0x150 = (0,0,0), up +0x168 = (0,1,0), dir +0x174 = target − pos = (0,0,−1500), scale +0x180 = 1.0 ([0x6fe2b8]).
  - **BuildViewMatrix 0x406030 (`ret` at 0x4065cc; out-of-line `fldz`/`jmp`-back blocks at 0x4065d0/0x4065e0/0x4065f0 run to 0x4065f8, next entry 0x406600; they store nothing, so the census below stands).** The stores (EXECUTED):
    - right r → +0x184/+0x194/+0x1a4 (0x40641a/0x406425/0x406430);
    - up u → +0x188/+0x198/+0x1a8 (0x406474/0x40647f/0x40648a);
    - forward f → +0x18c/+0x19c/+0x1ac (0x4064ce/0x4064d9/0x4064e4);
    - −dot(pos,r) → +0x1b4 (0x406469); −dot(pos,u) → +0x1b8 (0x4064c3); −dot(pos,f)·[+0x180] → +0x1bc (0x406523);
    - +0x190/+0x1a0/+0x1b0 = 0.0 (0x6fe31c/20/24) and +0x1c0 = 1.0 (0x6fe328).
  - **The arithmetic (EXECUTED by the re-review's x87 emulator of 0x406030–0x4065f9, 395 steps, normal branches, inputs pos 0 / target (0,0,−1500) / up (0,1,0) / scale 1: identity, translation −0, at the store addresses above).**
    - f = normalize(−dir) = (0,0,1) (fchs 0x4060bf–0x4060cf, then sqrt 0x406100).
    - r = normalize(up × f) = (1,0,0).
    - u = normalize(r × dir) = (0,1,0) (sqrt 0x40636d).
  - **The result.** Rows (1,0,0,0), (0,1,0,0), (0,0,1,0), (−0,−0,−0,1): identity, with −0.0 translation because pos = 0.
  - The function also negates +0x174 in place (0x40655d–0x406581), which matters only if the matrix is rebuilt, and it never is for the HUD.
  - This covers the HUD camera only. The area camera belongs to Task 2a.
- **HUD space does not scale with resolution (EXECUTED).**
  - The ortho camera's l/r/t/b are set once, in its ctor (called from DoSetUp's SetUpCamera), from the current resolution.
  - No HUD sprite or text function calls GetResolutionWidth/Height (0x43ae50 / 0x43ae70): their caller census contains none.
  - `GcBigSprite::SetHudPosition` 0x441860 passes the raw coordinates.
  - So at 1280×720, text placed at x = 400 stays at pixel 400.
- **The z values (EXECUTED).** Sprite tiles are placed at tz = zDepth − 7500.0 (0x72ef80) in the ctor, which is −7499 for the default zDepth 1.0 (0x72ef60). `GcHudSprite::SetPosition` 0x526fd0 sets tz = 0.0 + [0x72efbc] = −7499.0 and ignores mZDepth. Glyphs use −7499.0 (0x758218). So z_ndc = 1/7500 ≈ 0.000133, at the near plane.
- **Note for Tasks 8 and 10 (stable knowledge, not read from the exe): pixel centres.** D3D8 rasterisation puts pixel centres on integer coordinates, while WebGPU puts them at +0.5. A port that reuses this ortho matrix unchanged shifts every HUD quad and glyph by half a pixel against the original. Compensate in the projection (−0.5 px in x and y in pixel space), or accept and measure the difference in the pixel comparison.

### 3.4 The sprite quad (GcBigSprite / GcHudSprite)

- **Buffers (EXECUTED, ScDrawableTexture::Load).** Created once, behind the flag [0x7380e0]:
  - CreateVertexBuffer(0x80, usage 8 WRITEONLY, FVF = this+0x94, still 0 at that point, pool 1 MANAGED) → [0x7380e4], filled with 128 bytes from 0x738150.
  - CreateIndexBuffer(0x10, 8, 0x65 INDEX16, 1) → [0x7380e8], filled with 8 bytes from 0x7380ec: indices (0,2,1,3). The count [0x7380d8] = 4.
  - mFVF = **0x112** (XYZ | NORMAL | TEX1, stride 0x20), stored at 0x556941.
- **Vertices (EXECUTED, written by FUN_005574a0 at 0x557bbb–0x557d2f).** Each has normal (0,0,1):
  - v0 (−0.5, 0.5, 0) uv (0,0);
  - v1 (0.5, 0.5, 0) uv (1,0);
  - v2 (−0.5, −0.5, 0) uv (0,1);
  - v3 (0.5, −0.5, 0) uv (1,1).
- **`ScDrawableTexture::Draw` 0x5569f0 (EXECUTED, device [0x738bc4]).** In order:
  1. SetStreamSource(0, [0x7380e4], 0x20) and SetVertexShader([this+0x94]). That field is 0x112 after a full Load (store at 0x556941). It is still 0 if Load took the early `ret 0x4` at 0x55645f, before that store.
  2. ScDxStates RS 0x99 SOFTWAREVERTEXPROCESSING = 0.
  3. SetIndices([0x7380e8], 0) and SetTexture(stage, [this+0x98]).
  4. If the matrix count is not 0: SetTransform(0x100 WORLD, matrix).
  5. **DrawIndexedPrimitive(5 TRIANGLESTRIP, 0, 4, 0, 2).**
  6. Then SetTexture(stage, 0) and SetStreamSource(stage, 0, 0).
- **`ScDrawableMaterial::Draw` 0x54ee70 (EXECUTED).**
  - TSS(stage, 0x18 TEXTURETRANSFORMFLAGS) = 0 via ScDxStates at 0x54eeba. When the UV transform is on, the value is 2 (0x54ee8e) and SetTransform(0x10 TEXTURE0) runs.
  - Per texture: SetMaterial(+0x50) straight to the device (0x54eef9), then the texture's vfunc+0x14 → ScDrawableTexture::Draw (0x54ef2f).
- **The world matrix (EXECUTED).**
  - `GcHudSprite::GcHudSprite` 0x526ba0 sets S = diag(x2−x1, y2−y1, 1, 1) and t = (x2 − w/2, y2 − h/2, zDepth − 7500).
  - It also sets R = RotX(−π_f32 0xc0490fdb at [0x72ef8c]), pushed at 0x526f74. `D3DUtil_SetRotateXMatrix` 0x52c2a0 stores cos at +0x14/+0x28 and ±sin at +0x18/+0x24 as f32, using CRT `_cos`/`_sin`. That gives cos = −1.0 and sin = +8.742278e−08 (0x33bbbd2e).
  - `GcHudSprite::Render` 0x527210 forms S·R (multiply at 0x527367), then adds t to row 3 (0x52738d–0x5273b1).
  - Result: [[w,0,0,0],[0,−h,h·s,0],[0,−s,−1,0],[tx,ty,tz,1]]. Vertex v0 lands at pixel (x1, y1), the top-left, with uv (0,0).
- **The tile layout (EXECUTED offsets, INFERRED loop).**
  - Rows go to +0xd8 and columns to +0xdc. The tile size is ((x2−x1)/cols, (y2−y1)/rows).
  - Tile files are "%d_%d.tga" (0x703dcc), or the plain ".tga" name for a 1×1 sprite.
  - Each tile is a GcHudSprite (0x2a0 bytes). Multi-tile sprites call SetLinear(false) (0x440337); a 1×1 sprite keeps linear = true.
- **`GcBigSprite::Render` 0x440570 (EXECUTED; true extent to `ret 0x4` at 0x4407e3).** This is the object +0x24 that the HUD ScRenderNode calls.
  - **Loops.** The outer loop is i over rows [+0xd8] (test 0x4407d0); the inner loop is j over cols [+0xdc] (test 0x4407be). Tile = array[i][j] (pointer-to-row array).
  - **Which array.** The array is +0xec when byte +0x29 is set (0x440597 branch) and +0xe8 otherwise.
  - **Before each tile draw, it writes the tile's +0x30 / +0x34.** The decomp names these ScSprite mRepeatS/mRepeatT (ScSprite ctor stores 0x49ef89/0x49ef95). Per +0x29 path:
    - If byte +0x2d is set:
      - tile (1,1) gets (+0x20c, +0x210) (0x4405b7/0x4406c6);
      - tiles (1,0) and (1,2) get (1.0, +0x218); the 1.0 is 0x703c74 on the +0xec path and 0x703c7c on the +0xe8 path;
      - tiles (0,1) and (2,1) get (+0x214, 1.0); the 1.0 is 0x703c78 / 0x703c80;
      - corners are untouched.
    - Then, if byte +0x2e is set: every tile gets (+0x214, +0x218) (0x440661/0x440770).
    - INFERRED: these are nine-slice repeat factors for the 3×3 mode (+0x2d = loadhudsprite's b6).
    - A census of `[reg+0x30]`/`[reg+0x34]` reads in every *Sprite*/ScDrawable* function found **no GcHudSprite reader**. The only readers are GcAnimSprite, GcBigSprite's own fields, and ScDrawableMaterial's own fields. So these writes have no observed effect on the HUD draw (INFERRED; the reader census is a pattern grep, and the re-review confirmed no `+0x30]`/`+0x34]` read inside GcHudSprite::Render 0x527210–0x5277b0 itself, not in its callees).
  - **Draw.** Each tile's vfunc+0x24(node) is called at 0x4406a6 (+0xec path) or 0x4407b5 (+0xe8 path).
  - **Return value.** eax is never written after the calls, so Render returns the **last tile's** `GcHudSprite::Render` result (rows−1, cols−1).
  - **Rule for the port and the state log (EXECUTED control flow; consequence INFERRED):**
    - The deferral of a multi-tile big sprite is decided by its **last tile only**.
    - At priority 0 every tile runs: a tile with neither transparency nor alpha draws; a tile with either returns 100 and draws nothing.
    - If the last tile returned 100, the whole big sprite is queued for priority 100 and every tile runs again there: the deferring tiles draw, and the non-deferring tiles **draw a second time**.
    - If the last tile did not defer, the big sprite is not queued, and any earlier deferring tile never draws that frame.
    - `enablealphachannel` sets every tile alike (§4.1), so mixed tiles need per-tile flags. The rule is still what the code does.

### 3.5 Sprite states (GcHudSprite::Render 0x527210, EXECUTED; ScDxStates unless noted)

**Deferral.** If doTransparency (+0x50) or doAlpha (+0x51) is set and the current priority is not 100, Render returns 100 (0x52723f).

Then, in order:
- **ALPHABLENDENABLE = 1**, unconditionally (0x52747d). It is never reset here.
- **If doAlpha:**
  - ALPHAREF = chop(mTransparentValue +0x40) (fistp with RC = chop).
  - ALPHAFUNC = 7 (GREATEREQUAL) if byte [+0x161] == 1, else 5 (GREATER).
  - If (!doColor || doTransparency): TSS0 COLORARG2 = 0 (DIFFUSE).
- **If doTransparency:**
  - TSS0 ALPHAOP = 3 and ALPHAARG2 = 3 (TFACTOR).
  - TEXTUREFACTOR = chop(255·mTransparentValue) << 24.
- **If doColor && !doTransparency:**
  - TSS0 COLORARG2 = 3 (TFACTOR).
  - TEXTUREFACTOR = ARGB from the diffuse at +0x168..+0x174 × 255.0 (doubles at 0x72efd0–0x72efe8), chopped and masked to a byte.
- **If !linear (+0x194):** TSS0 MINFILTER(0x11) = 1 and MAGFILTER(0x10) = 1 (POINT). They are never restored.
- **Draw:** mTexture (+0x258) vfunc+0x14(ctx [0x83aa90], &world, 1, 0.0 [0x72eff0], stage 0).
- **After the draw:** if doTransparency, TSS0 ALPHAARG2 = 2. If doColor && !doTransparency, RestoreTSS(0,2), (0,1) and (0,3).

The sprite never sets SRCBLEND, DESTBLEND or any Z state. It inherits them from the mode (§3.2) and from whatever drew before it.

Defaults (INFERRED, decomp of the ScSprite ctor 0x49ef40):
- doTransparency, doAlpha and doColor are false; byte +0x161 = 0.
- The GcHudSprite ctor sets byte +0x160 = 1 (0x526c3c, EXECUTED) and linear = 1 (0x526c32, EXECUTED).

### 3.6 Text (ScTextObject::Render 0x610f60 → ScTileFont, EXECUTED)

**ScTextObject::Render.**
- It writes the shared font's +0xb4 = chop(text +0x158) · (1/255) (0x7545a0 = 0x3b808081; chop at 0x610f7e).
- It writes font +0x8 = text +0x15c (xScale) and font +0xc = text +0x160 (yScale). These are writes to the **shared** font object, made on every render of every text object.
- **Dispatch:**
  - If byte +0x29 (box fit) is set: DrawBox(+0x148, +0x150, +0x14c, +0x154, +0x144, &string +0x30) at 0x610fff.
  - Else, if byte +0x28 (justify) is set: DrawJust(word +0x2a, x +0x13c, y +0x140, z +0x144, &string) at 0x611036.
  - Else: Draw(+0x13c, +0x140, +0x144, &string) at 0x61105e.
  - Draw's return value passes through, so text defers to priority 100 like sprites do.

**ScTileFont::Draw 0x627dd0.** It calls the raw device [0x73232c] with its own get/compare logic, not ScDxStates. In order:
- **Defer:** if transparency (+0xb0) or alpha (+0xb1) is set and the priority is not 100, return 100 (0x627e4f).
- **Baseline:** y += (u32)baseline [+0x44] · yScale [+0xc] (0x627e0e).
- **TSS0 colour:** it reads COLORARG1, COLOROP and COLORARG2, then sets each only where it differs: COLORARG1 = 2 (TEXTURE), COLOROP = 4 (MODULATE), COLORARG2 = 3 (TFACTOR) (0x627ebc–0x627f07).
- **TEXTUREFACTOR**, set unconditionally (0x628048): (chop(R+0x24·255) & 0xff) << 16 | A+0x30 << 24 | G+0x28 << 8 | B+0x2c. The colour floats are stored by ScTileFont::SetColor 0x627c20 as int × 1/255 (0x7581cc–0x7581d8 = 0x3b808081).
- **CULLMODE:** read; if it is not 1, set 1 and remember to restore it.
- **Filters:** MINFILTER is read. If !linear (+0xb2): MIN = MAG = 1. Else, if MIN ≠ 2: MIN = MAG = 2.
- **If transparency or alpha:** ALPHABLENDENABLE = 1, SRCBLEND = 5, DESTBLEND = 6 (0x628149–0x62817c).
  - If also alpha && [+0xc8] == 1: ALPHATESTENABLE = 1, ALPHAFUNC = 7, ALPHAREF = chop([+0xb4]).
  - Then ZWRITEENABLE = 0 (0x62820e).
- **Per glyph** (records of stride 0x24·8 at [+0x58]; ascii u16 at +0, width at +4, xOffset at +0xc):
  - w = charWidth [+0x3c] · xScale and h = charHeight [+0x40] · yScale; x −= xOffset · xScale.
  - The font scale [+0xcc] = diag(w, h, 1.0 [0x75820c], 1). The multiply is R·S (A = rotate [+0x14c], B = scale, at 0x628630).
  - The font's rotate is RotX(+π_f32) [0x7581c8] (ctor call 0x627b1f), so s = −8.742278e−08.
  - World = [[w,0,0,0],[0,−h,−8.74e−8,0],[0,8.74e−8·h,−1,0],[x+w/2, y−h/2, −7499.0, 1]].
  - **The glyph's screen box (EXECUTED: translation 0x628650–0x62868c with 0.5 at 0x758210/0x758214, w at [ebp−0x144] from 0x62841c, h at [ebp−0x13c] from 0x628444; baseline add at 0x627e0e–0x627e1a stored back into the y argument [ebp+0xc]).**
    - R·S has row 3 = (0,0,0,1), and row 3 += (x + 0.5w, y − 0.5h, −7499).
    - Quad vertex v0 (−0.5, 0.5) (uv 0,0) lands at (x, y − h). v2 (−0.5, −0.5) (uv 0,1) lands at (x, y). v1 lands at (x + w, y − h) and v3 at (x + w, y).
    - Here y = the caller's pen y + baseline·yScale, so **that y is the glyph's bottom edge** and the glyph spans [y − h, y] in screen pixels. x is the pen x after the xOffset step.
    - Hand check, with w = 10, h = 20, x = 100, y = 50: row 3 = (105, 40). v0: x' = −0.5·10 + 105 = 100 and y' = 0.5·(−20) + 40 = 30 = y − h. v2: y' = −0.5·(−20) + 40 = 50 = y. v1/v3: x' = 0.5·10 + 105 = 110.
  - Draw: [font+0x18c][i] vfunc+0x14(ctx [0x83aa90], &m, 1, 0.0 [0x75821c], 0). Those objects come from the single `ScDrawableMaterial::Create` (0x627a02) stored at 0x6273f6, so this is the §3.4 quad path, FVF 0x112 (INFERRED from that census).
- **Advance:**
  - A glyph: x += ((u32)(xOffset + width) + kerning [+0x34]) · xScale.
  - A space: x += (spaceWidth [+0x4c] + kerning) · xScale.
  - An unknown glyph draws nothing and advances by (maxWidth [+0x54] + kerning) · xScale.
  - '\n': x = startX and y += trunc(charHeight · yScale). **The RC is chop (0xc00), not round**, which corrects probe 2 (2026-10-08-m2a0-probe2-fonts.md).
  - 0xFF calls the callback [+0x5c]. Lead bytes 0xF0..0xFE start 2-byte codes.
- **Epilogue (restores):**
  - The TSS values that differed, and CULL if it changed.
  - MIN and MAG, both to the saved MINFILTER value.
  - If transparency or alpha: ALPHABLENDENABLE = 0. If alpha && [+0xc8] == 1, also ALPHATESTENABLE = 0 and ALPHAREF = 1. Then ZWRITEENABLE = 1.
  - Return −1.
- **Draw bypasses ScDxStates (EXECUTED).** Its sets and restores are raw device calls.
  - ScDxStates' `Set*` re-reads the device on every call (§3.2), so a later `Set*` sees the true value.
  - Only `RestoreRenderState` 0x64b340 / `RestoreTextureStageState` 0x64b380 compare cached values without a read. A Restore issued after a text draw changed the same state can therefore skip a needed write (INFERRED consequence).
  - The port's state log must model both writers.

### 3.7 Lines and rectangles: FVF 0x42 (EXECUTED)

- **`ScRenderer::DrawTriangles` 0x531380 is not on the HUD path.** Its only callers are `GcSurfaceOctree::RenderOctNode` (0x464b99, 0x464d90).
- **`ScRenderer::DrawLine` 0x530920** is called by `GcLine::Render` 0x40509e, by `ScCamera::Render` (12 sites, 0x492feb–0x49334f), by `GcToa::Render` 0x56acb9 and by `GcMotionSystem::RenderMotion` (0x58a72a, 0x58a7b3).
- **`GcRectangle::Render` 0x402350 calls `ScRenderer::DrawLines` 0x530d50 (0x402601).** That path is on the HUD: `gcrectangle.*`, `gcfilledrectangle.*` and `gchuddirector.addline/addrectangle/addfilledrectangle` are all `called_by_base_osi: true` in natives.json. **Static answer: not reached, not from the menu, not from Options, not from anywhere (EXECUTED; `tools/research/osi.py` parse/disasm plus the `tools/osi/reach.py` `Walk` closure over Base.osi, scratch scripts `rect_reach.py`/`sf.py`).**
  - `osi.py --plain-calls` over all 17 natives (4 gcrectangle, 6 gcfilledrectangle, 4 gcline and the three `gchuddirector.add*`) finds call sites only in:
    - `ss_baseline.*` (addline at file offset 401491),
    - `ss_baserect.*` (addrectangle 402335; gcrectangle.create 402237),
    - `ss_basefilledrect.*` (addfilledrectangle 416863; gcfilledrectangle.create 416786),
    - `ss_gradrect.*` (gcfilledrectangle.create 417426).
  - Only five classes own those method bodies: those four and `ss_shoefittersoundmarkerclass`. No 0x13 CreateObject constructs the four base classes anywhere.
  - The marker class is constructed only in `ss_shoefitterbottombar.addsound` and `ss_shoefittercharacterwindow.createsaveablesounds`. Those are constructed only in `setupshoefitterscreen()`, whose offset (144463) no 0x40/0x41/0x42 immediate pushes.
  - Every one of these subroutines is **unreachable** from the engine roots setup/process/input/onevent under the reach.py rule, **including the wild closure**. The main menu and Options screens are inside that closure, so they do not reach these natives.
  - Caveat (reach.py's own): objects deserialised by `ScSLOSIFileIO::GetVariantFromBuffer` 0x6160a0 are invisible to the walk.
- **DrawLine and DrawLines** both:
  1. Set TSS0 COLOROP = 2 (SELECTARG1) and COLORARG1 = 0 (DIFFUSE) through ScDxStates (0x5309a4/0x5309b2; 0x530dd1/0x530ddf).
  2. **Create a vertex buffer on every call:** CreateVertexBuffer(n, usage 8, **FVF 0x42** XYZ|DIFFUSE, pool 1), stride 0x10.
  3. SetVertexShader(0x42), then SetMaterial with a zeroed 0x44-byte material plus colour (raw), then SetTransform(WORLD).
  4. Draw: DrawLine uses DrawPrimitive(2 LINELIST, 0, 1); DrawLines uses DrawPrimitive(2 LINELIST, 0, n) (0x531079).
  5. RestoreTSS, Release the buffer, and SetStreamSource(0, 0, 0).
- **`GcFilledRectangle::Render` 0x523fc0** (does not use DrawLine/DrawTriangles):
  - If byte +0x54 is set: TSS0 ALPHAOP = 2 and ALPHAARG1 = 0 (DIFFUSE) (0x523fe4/0x523ff2).
  - Always: TSS0 COLOROP = 3 (SELECTARG2) (0x524000).
  - Per draw: CreateVertexBuffer(0x40, 8, 0x42, 1) (0x5240e2), with 4 vertices:
    - positions (−0.5,0.5), (0.5,0.5), (0.5,−0.5), (−0.5,−0.5) (0x72e814–0x72e830);
    - z = +0x50;
    - diffuse = +0x58, +0x5c, +0x60, +0x64 per vertex.
  - World = (+0x68)·(+0xa8) (multiply at 0x524062), with row 3 += (+0x118, +0x11c, +0x120).
  - Then SetStreamSource(0, vb, 0x10), SetVertexShader(0x42), SetTransform(WORLD), **DrawPrimitive(6 TRIANGLEFAN, 0, 2)** (0x524266).
  - Release; RestoreTSS 4/5 (if +0x54) and RestoreTSS 1; SetStreamSource(0, 0, 0).
  - **It returns −1 unconditionally** (`mov eax, 0xffffffff` at 0x5242ba), so a filled rectangle never defers to priority 100, even with alpha enabled.
- **DrawBox 0x6288f0 is not a rectangle.** It is `ScTileFont::DrawBox`, the box-fit text path, called only from ScTextObject::Render (0x610fff).

## §4 Render-visible native state

The ABI follows natives.md §1.2: wi = the i-th argument's raw 32-bit word (0 for an unused slot). Floats reach the engine as raw f32 bit patterns, with no conversion, unless a row says otherwise. Every native below returns the none variant (word 0xf, 0) unless a row says otherwise. Bodies come from natives.json.

### 4.1 GcBigSprite (object = w0)

| Native | Body → engine call | Field written, conversion | Renderer that reads it |
|---|---|---|---|
| `gcbigsprite.setzdepth` | 0x43dd60 → SetZDepth 0x441ac0 | big +0x3c and every tile's GcHudSprite +0x188 (mZDepth), raw f32 (EXECUTED 0x536906 for the tile store) | **None after construction.** +0x188 is read only in the ctor (0x526f19), and SetPosition resets tz to −7499 (0x526fd0). Census: every function named *Sprite*. So setzdepth has no render effect. |
| `gcbigsprite.enablealphachannel` | 0x43dd30 → 0x4408f0 | big +0x2b and every tile's +0x51 (doAlpha) = low byte of w1 | GcHudSprite::Render: deferral, ALPHAREF/ALPHAFUNC, COLORARG2 (§3.5) |
| `gcbigsprite.sethudposition` | 0x43db80 → SetHudPosition 0x441860 | big +0x48..+0x54 (x1,y1,x2,y2 raw f32), then GcHudSprite::SetPosition per tile: translate row +0x208/+0x20c/+0x210, with tz = −7499.0 | GcHudSprite::Render world matrix (§3.4); no resolution scaling |
| `gcbigsprite.setcolor` | 0x43ddc0 → 0x440970 | the low bytes of w1..w4, each × 1/255 ([0x703c90..0x703c9c] = 0x3b808081, EXECUTED), → ScSprite::SetColor 0x49fb20 → diffuse +0x168..+0x174. doColor +0x52 becomes false if the colour equals the default at 0x7100bc..c8 (INFERRED, decomp) | TEXTUREFACTOR / COLORARG2 path (§3.5) |
| `gcbigsprite.loadhudsprite` | 0x43dad0 → LoadHudSprite 0x440460 (0x43db09) → ctor 0x43f210 | ctor(x1,y1,x2,y2,&name, b6 = low byte of w5, b7 = 0). Returns a game variant (CreateVariant 0x43da20, 0x43db16) | §3.4; missing files in §7 |

### 4.2 GcHudDirector

| Native | Body → engine call | Effect |
|---|---|---|
| `gchuddirector.show` | 0x47b0a0 → Show 0x47b320 (0x47b0b6) | Appends the node to mainHud's children vector if it is absent (§3.1). Returns none. |
| `gchuddirector.hide` | 0x47b0d0 → Hide 0x47b3e0 (0x47b0e6) | Erases the node; the others keep their order. Returns none. |
| `gchuddirector.addbigsprite` | 0x47afe0 → AddBigSprite 0x47b730 (0x47afee) | Wraps the object in a new ScRenderNode (flags 0x11) and returns the id through CreateVariant 0x407df0 (0x47affd), an int. The node is not shown until `show`. |
| `gchuddirector.addtextobject` | 0x47b010 → AddTextObject 0x47b780 (0x47b01e) | The same, through CreateVariant at 0x47b02d. |

The return values and the call sites are EXECUTED. The node-creation details inside 0x47b730/0x47b780 are INFERRED (decomp).

### 4.3 ScTextObject (object = w0, EXECUTED stores)

| Native | Body | Field written, conversion | Read by (ScTextObject::Render, §3.6) |
|---|---|---|---|
| `sctextobject.setposition` | 0x611e50 | +0x13c = w1, +0x140 = w2, +0x144 = w3, raw f32 (0x611e7d/0x611e85/0x611e8d) | x, y, z arguments of Draw/DrawJust (0x611056/0x61104a; 0x611029/0x61101d), and DrawBox z (0x610fdf) |
| `sctextobject.setscale` | 0x6120d0 | +0x15c = w1 (xScale), +0x160 = w2 (yScale), raw f32 (0x6120f2/0x6120fa) | copied into the shared font's +0x8/+0xc every render (0x610fbd/0x610fb5 → 0x610fca/0x610fcf) |
| `sctextobject.setjustification` | 0x612000 | word +0x2a = low 16 bits of w1 (0x612014) | DrawJust's first argument (0x61102f) |
| `sctextobject.enablejustification` | 0x612030 | byte +0x28 = low byte of w1 (0x612043) | selects DrawJust over Draw (0x611010) |
| `sctextobject.setstring` | 0x611f40 | ScFixedString at +0x30 = the string (operator= at 0x611f57, ecx = obj+0x30) | the string argument of all three draws (lea +0x30 at 0x611046 etc.) |
| `sctextobject.setfont` | 0x611f90 | ref-counted pointer +0x2c = w1 (old one released, vfunc+4; new one +1) (0x611fce) | `this` of every font draw (0x611043) and target of the scale/alpha writes |

DrawJust 0x627c60's use of the justification word was not read here (gap; INFERRED to select left/centre/right).

### 4.4 ScTileFont (object = w0, EXECUTED)

| Native | Body | Field written, conversion | Read by |
|---|---|---|---|
| `sctilefont.setcolor` | 0x611a00 → ScTileFont::SetColor 0x627c20 (0x611a23) | +0x24/+0x28/+0x2c/+0x30 = (int)w1..w4 × 1/255 (fild + fmul 0x7581cc..d8) | Draw's TEXTUREFACTOR (0x628048) |
| `sctilefont.enabletransparency` | 0x611b70 | byte +0xb0 = low byte of w1 (0x611b83) | Draw: deferral to priority 100, the blend states and ZWRITE=0 (§3.6) |
| `sctilefont.create` | 0x611910 → operator new + ScTileFont ctor 0x626b40 (0x61192c) → CreateVariant 0x611950 | a new font; missing files in §7 | — |

The ctor defaults for +0xb0, +0xb1, +0xb2, +0xc8 and the colour were **not read** (gap).

### 4.5 Area, light and model directors

- **`gcareadirector.setfog`** 0x487090: pushes w0..w3 to `GcAreaDirector::SetFog` 0x48a7a0 (0x4870ae; EXECUTED). That calls GcGraphicsOptions::SetFogValue(colour w0, −w1, −w2, w3) (INFERRED, decomp). On the HUD pass, fog is switched off by SrPushRenderMode (§3.2) when the FOGENABLE state is on.
- **`gclightdirector.setcavelighting`** 0x48de80 → SetCaveLighting 0x48e670 (0x48de8d; EXECUTED).
  - For each active fast light whose byte +0xb0 is 0, while sUseSpots is false, it calls SetIntensity(0.25 [0x70bd68] if w0 is true, else 1.0 [0x70bd6c]). The constants are EXECUTED; the loop is INFERRED.
  - It affects area lighting only. The HUD pass sets LIGHTING = 0.
- **`gclightdirector.toggleconstantlights`** 0x48de00 → ToggleConstantLights 0x48e210 (0x48de03; EXECUTED). It returns a bool through CreateVariant 0x407f30.
  - It flips `sConstOn` and calls SrSetGlobalAmbientLight: black with 0.0 [0x70bd60] when on, the colour at 0x783fdc with 1.0 [0x70bd64] when off (INFERRED, decomp).
  - It does not affect the HUD.
- **`gcmodeldirector.setnearestmax`** 0x47d1d0: byte [0x70a7c4] = low byte of **w0** (0x47d1db; EXECUTED).
  - The readers are GcModelDirector::GetNearest (0x47d90c), and GcCinemaEngine::Play (0x593d2f, which saves it and writes 0xa at 0x593d41) and Stop (0x5941c1, which restores it).
  - It has no HUD effect.

## §5 Input tuples (EXECUTED unless marked)

### §5.1 Mouse: `GcMouseDriver::Process` 0x55c160
- **Extent:** to `ret 0x10` at 0x55d4b1; the next entry is 0x55d4c0; 13 calls to `ScOSIVirtualMachine::Call` 0x60b320.
- **No event at all** when `[0x73a0cc]` (the DirectInput device) is null (→ 0x55d4a8), or when GetDeviceState or Acquire fails (0x55c1be, 0x55c1e2, 0x55c1f2).
- **Otherwise `OnEvent(26, x, y)` on every run** (call at 0x55c753). x and y are `CreateVariant(u16)` 0x4183f0 of `[0x83b396]` / `[0x83b394]`.
  - Windowed (`[0x83b510]` == 0): GetCursorPos.
  - Otherwise: relative deltas, clamped.
  - If `[0x83b3fd]`: GetWindowRect, subtract left+3 / top+0x16, clamp ≥ 0.
  - If `[0x83b3f6]`: scale by `[0x83a4d8]` / `[0x83a4e0]`.
  - The cadence is every frame, so that stop condition does not fire.
- **Buttons 1–4** come from rgbButtons[0..3], against the previous mask `[0x83b386]` (bits 1, 2, 4, 8):
  - down and previously down → `OnEvent(24, button)`;
  - down and newly pressed → 23;
  - up and previously down → 25.
  - These are 12 sites through 0x42a5b0 (e.g. 0x55c89a, 0x55c9ad, 0x55cace, … 0x55d4a3).
  - **Order in one pass:** 26 first, then buttons 1, 2, 3, 4.
- **Menu hit boxes** (controller input (3), measured by Task 1): both bounds are strict, x 401–699 and y Y+1..Y+19.

### §5.2 Keys: `GcKeyMapper::ProcessKey` 0x5d1a10 (to `ret 8` at 0x5d1e21)
- **State argument:** 2 → 2, 4 → 4, anything else → 1 (0x5d1a21–0x5d1a40).
- **Paused** (`[0x7032c4]` ≠ 0) **or no player Toa** (`[0x7032b8]` == 0) → 0x5d1d20: only Esc (0xfb) is sent, as `Input(C1, 0xfb, state)` at 0x5d1de2.
  - `[0x7032b8]` is written by `GcModelDirector::AddToa` 0x47e527 and cleared by 0x47d39d / 0x47f4b0. On the front end, `addtoa("onua")` sets it, so this branch is not taken there (EXECUTED: writers; INFERRED: that the front-end Toa is the one stored).
- **Not paused, with a Toa:** the mode `[cam+0x3a1]` is read twice (0x5d1a67, then 0x5d1bb7 after the Toa-control part).
  - **The Toa-control part can make one script call (m1).** In any camera mode except 0xb/0xd (0x5d1a76–0x5d1a86 → 0x5d1b63), key 0xe7 (Space) goes to 0x5d1b86 → 0x5d1920. There, when the state == 2 and `[[0x7032bc]+…]` ≠ 9 (0x5d1944–0x5d1961), it calls `GcConversationEngine::ActionKeyPressed` 0x5bf270 on `[0x744860]` (0x5d1982). That function sends `OnEvent` at 0x5bf416.
  - That call is guarded by conversation state: it returns early on the engine bytes `+0xe4` (0x5bf27e) and `+0xe5` (0x5bf296), on `+0x20` == 0 (0x5bf2b0) and on `+0xc` (0x5bf2c0).
  - That no conversation is active on the front end, so the guard returns before 0x5bf416, is INFERRED. The other keys there (0xe8, 0xea, 0xe9) call Toa methods (0x56eca0, 0x56d120) and make no direct script call.
  - Mode 0xb, with 0x418bb0 / 0x50a790 conditions (character state, not followed): `Input(C1, id, state)` for any key (0x5d1d0f).
  - **Mode ∈ {0xe, 2, 3, 8} → no `Input` at all, not even Esc** (0x5d1bf6–0x5d1c14).
  - Any other mode → only Esc is sent (0x5d1c1a).
  - **Consequence for the front end** (EXECUTED reading; INFERRED that no other process sends keys): while the camera flies a spline (mode 3), the key mapper sends nothing. After `resetcamera` (mode 0), it sends only Esc. The VK keys travel by a different path that ignores the camera mode (§5.3).
- **C1** = `[0x7d62b4]` = 0x636e7431 'cnt1'. The 25 mapper pairs, the 29-iteration loop of `Process` 0x5d1e30 and the action state machine (`ScAction::IsRequested` 0x491ed0: 2 on the first frame down, 4 while held, 1 once on release) are as in probe 3 (`docs/knowledge/research/2026-10-08-m2a0-probe3-input.md`, CITED).

### §5.3 VK keys (Enter, Space, arrows): the trigger path (probe 3 open items 2 and 4)
- **Registration** (EXECUTED): `GcGame::MapControllerToScript` 0x4370c0 is called once from `SetUpScripting` at 0x439277, after `Setup()` (0x43925a), with driver id `[0x83b354]` = 'cnt1'.
  - Driver check: it takes the driver (`GrChooseDriver` 0x44a7c0) and keyboard-maps only if its type is 3 (`call 0x497c00; cmp al,3` at 0x437134–0x43713b; else → 0x4374d0). That type 3 means the keyboard is INFERRED.
  - Key table: if `Cheat()` (0x437141) is true, it maps the **21-VK cheat table** at 0x7032ec (count 0x15 at 0x43714e): 0D 20 25 26 27 28 21 22 23 24 2E 2D 6F 61 62 63 65 68 6B 6D BF. Otherwise it maps the **6-VK table** at 0x703318 (count 6 at 0x437312): 0D 20 25 26 27 28. This install has neither cheat string, so it uses the 6-VK table (§6).
  - Per VK: an `ScSLOSIAnalogControllerAction` (ctor 0x613f00 with "Input" 0x703453, VM `[0x83877c]`), an `ScControlTrigger` (0x4c4ff0) on `GetElement(VK)`, then `ScTriggerManager::AddTrigger` 0x4c2440 with the context pair `[0x83aecc]` = 0x73637250 / `[0x779718]` = 0x73637270 (set at 0x43cb98 / 0x43c139).
- **Who fires them** (EXECUTED):
  - `GcControllerProcess::Process` 0x44a7a0 (to `ret 0x10` at 0x44a7b8), **priority 0**, calls `ScInputManager::GoCacheYourself` 0x4973d0, then `ScTriggerManager::ProcessTriggers` 0x4c2bf0, then returns 1.
  - The trigger's action `ScSLOSIAnalogControllerAction::Fire` 0x613f80 (to `ret 4` at 0x61407a) sends `Input(C1 [act+0x28], VK [act+0x8], state byte [act+0x18], f, f)` at 0x61406f. Its only guard is `[act+0x20]` ≠ 0, the VM pointer (0x613f8f).
- **The gate is the trigger context** (CITED from the re-review's asm reading, not re-run here):
  - The VK triggers are registered under provider **'Prcs'** (`[0x83aecc]` = 0x73637250, set at 0x43cb98) with context **'prcs'** (`[0x779718]` = 0x73637270, set at 0x43c139).
  - That context is pushed **once**, in `SetUpScripting` 0x439288–0x4392b1 (0x4d4300 create, then 0x4d4730 push). The handle at `[ebp-0x1c]` is never referenced again.
  - 'Prcs' is read only at the 23 sites 0x4372a1–0x439288 and at shutdown (0x43acf8 → `ClearByContextProvider` 0x4c2f00). So nothing pops or switches the 'Prcs' context while the game runs.
  - EXECUTED here: the direct-call closure of 0x44a7a0 and 0x613f80 (73 functions, 24 indirect calls not followed; `reads.py`) reads none of `[0x7032c4]`, `[cam+0x3a1]`, `[0x7032bc]`, `[0x7032b8]`. Supporting evidence only; the context gate is the reason.
- **How this squares with §5.2 and with the camera** (CITED):
  - The camera's context switches (0x4152d0, called from 0x4550af and 0x455bcd with 'pnin' / 'none') push onto the **Toa's** provider `[toa+0x100]` (set by ctor 0x40b520 arg5: 'cnt1' / 'niia'). They gate Toa movement, not 'Prcs'.
  - 0x5d1bf6 gates only the key mapper's ids 0xe3..0xfb.
  - No key is delivered twice: mapper id 0xe7 (Space) goes to the conversation engine (§5.2), while VK 0x20 goes to `Input`.
- **Still INFERRED:**
  - that no provider key passed as a parameter or loaded from data equals 'Prcs';
  - that `GcControllerProcess` is live on the front end (registered at 0x4394a4, priority 0; nothing seen removing it);
  - that the menu's `Input` handler consumes 0x26/0x28. The oracle evidence for this (Task 1: Enter at 220 → a new spline at 258) is **circular**, because the oracle delivers keys by this same assumed path.
- **Ruling** (EXECUTED path; context gate CITED; liveness INFERRED): **Enter, Space and the arrows reach the menu** as `Input(C1, VK, state, f, f)` at priority 0, before the script's `Process` in the same pass, in every camera mode. Whether pausing stops `GcControllerProcess` itself (the pm's `[proc+9]`) was not read.

## §6 INI reads (EXECUTED)

### §6.1 Census
- **Import slots** (KERNEL32 IAT base 0x7713c8): `GetPrivateProfileIntA` `[0x77141c]`, `WritePrivateProfileStringA` `[0x771420]`, `GetPrivateProfileStringA` `[0x771424]`.
- **Call sites in the listing:** 16 IntA, 22 Write, 5 StrA. A raw scan of the exe for the three slot addresses finds 16, 22 and 5 occurrences, so the census is complete. All the calls are direct `call dword ptr [slot]`.
- **INI path:** buffer 0x83b410 = `GetModuleFileNameA` directory + `\Bionicle.ini` (0x74754e), built in `LoadIni` 0x5d4890 (copy at 0x5d4a07). Every read passes 0x83b410.
- **Install INI** (data, sha256 1e261cfc…036c): `[GraphicsOptions]` 640/480/Windowed=1/32/0/0/Fog=1/100/Gamma=255/Shadow=2; `[Misc]` 0/1/100/100; **`[Cheat] Urinate=Pohatu`**.

### §6.2 Every read

| Function (caller) | API | Section / key | Default | Use |
|---|---|---|---|---|
| `LoadIni` 0x5d4890 (`AppMain` 0x538246) | Int 0x5d4a24 | GraphicsOptions / ResolutionW | 0 | if 0, `fopen("w+")` truncates and writes 13 defaults (below) |
| `GetScreenData` 0x5d5080 (`AppMain` 0x53827c, `ScDrawableContext::Reset` 0x5591e9) | Int ×7 | GraphicsOptions / ResolutionW, ResolutionH, Windowed, BitDepth, SvpOther, SvpRad, Fog | 0x280, 0x1e0, 0, 0x20, 0, 0, 1 | W/H stored as u16; the rest as bytes |
| `GetLanguage` 0x5d4c10 (`GcAreaDirector::Init` 0x4877de) | Int | Misc / Language | 0 | → 0x48b200 |
| `GetKeyOption` 0x5d4c90 (`GcLegoCamera` ctor 0x45058a) | Int | Misc / ControlOption | 1 | returned |
| `GetFog` 0x5d51c0 (native `gcsaver.getfog` 0x5d3a50) | Int | GraphicsOptions / Fog | 1 | **true only if the low byte == 1** (`cmp al,1`) |
| `GetShadowDetail` 0x5d5240 (`AppMain` 0x538284) | Int | GraphicsOptions / ShadowDetail | 2 | → 0x5e54c0 |
| `GetParticleDetail` 0x5d52c0 (`AppMain` 0x538290) | Int | GraphicsOptions / ParticleDetail | 100 | → 0x5e5320 |
| `GetGamma` 0x5d5330 (`AppMain` 0x538650) | Int | GraphicsOptions / Gamma | 256 | → 0x5e5370 |
| `GetSoundVolume` 0x5d53d0 (`AppMain` 0x53829c) | Int | Misc / SoundVolume | 100 | `& 0xff`, to float → 0x55fad0 |
| `GetMusicVolume` 0x5d5470 | Int | Misc / MusicVolume | 100 | returned |
| `Cheat` 0x5d4d00 (`MapControllerToScript` 0x437141; native `gcsaver.cheat` 0x5d3590) | Str 0x5d4d2a, then 0x5d4dc0 | Cheat / Cheat, then Misc / Cheat | "" (0x74764b), buffer 0xff | true if == "NoGreenCapes", else if the second == "LEGOTester" |
| `BadCheat` 0x5d4e40 (`GcToa` ctor 0x568a5e, `GcToa::CleanUp` 0x56b63a) | Str 0x5d4e6a | Cheat / Urinate | "", 0xff | true if == "Pohatu" |
| `CheatDebug` 0x5d4f00 (`ScSLOSIDebug::Print` 0x61d5ef) | Str 0x5d4f2a | Cheat / Cheat | "", 0xff | true if == "NoGreenCapes" |
| `CheatMenu` 0x5d4fc0 (native `gcsaver.cheatmenu` 0x5d35b0 only) | Str 0x5d4fea | Misc / CheatMenu | "", 0xff | true if == "LEGOTester" |

- **The comparison:** `_ScBaseString::compare` 0x4f5990 does a byte compare over min(len) via 0x657820, then the length difference. So it is an **exact, case-sensitive match**.
- **Cheat natives:** `gcsaver.cheat` / `cheatmenu` / `getfog` return `CreateVariant` 0x407f30 (int, `movzx` byte) = 0 or 1.
- **`LoadIni` defaults written** (only when ResolutionW reads 0):
  - GraphicsOptions: ResolutionW 640, ResolutionH 480, Windowed 1, BitDepth 32, SvpOther 0, SvpRad 0, Fog 1, ParticleDetail 100, ShadowDetail 2, Gamma 256;
  - Misc (after an `fopen("a+")`): Language 0, ControlOption 1, SoundVolume 100, MusicVolume 100.
  - Note that the Windowed default of the *read* is 0, but the default *written* is 1.
- **`AppMain` use:**
  - windowed = (Windowed byte == 1) (0x5382bc);
  - BitDepth 8/16/24 → 1/2/3, anything else → 4 (0x5382ca–0x538300);
  - the resolution switch at 0x53832d runs on a constant 0 (`mov eax,0`), so its four presets (512×384 … 1024×768) are dead and the INI W/H are used (0x538396–0x538408 → floats; `CreateWindow` helper 0x537fa0 gets the u16 W/H).
- **`GetResolutionWidth` 0x43ae50 / `GetResolutionHeight` 0x43ae70** return f32 `[[0x70fa64]+0x10]` / `+0x14`. That those are the INI W/H floats from 0x5383ae/0x5383cc is INFERRED: the writer of `[0x70fa64]+0x10` was not followed.
- **The cheat key table** 0x7032ec (21 u16 VKs) replaces the 6-VK table 0x703318 in `MapControllerToScript` when `Cheat()` is true (§5.3).
- **Front-end result on this install** (EXECUTED values, INFERRED that no other reader runs on the menu): Cheat, CheatMenu and CheatDebug are false; **Urinate is true**; Fog is true; the resolution is 640×480.

### §6.3 The Urinate block, 0x568a6b–0x568c02 (Decision 13)
- **Gate** (EXECUTED): `BadCheat()` is true (0x568a5e–0x568a65), and the Toa id dword `[arg]` equals `[0x73b18c]` 'onua', `[0x73b190]` 'poha' or `[0x73b194]` 'tahu' (byte strings in .data). On this install `addtoa("onua")` passes, matching F13 and F23.
- **What it builds** (EXECUTED):
  - a 0x38-byte `ScCallbackAction<GcToa>` (`ScAction::ScAction` 0x491e60 with id 0x42644374 and mask 3; vtable 0x73c44c);
  - the member-function pointer copied from .data 0x73b198 = `{0, −1, 0x56fcf0}`, that is `GcToa::KeyPressed` 0x56fcf0;
  - a 0x2c-byte `ScControlTrigger` (ctor 0x4c4ff0) on `ScInputDriver::GetElement` 0x497c10 (element 0x50) of the driver from `GrChooseDriver` 0x44a7c0 with `[0x7cb1cc]` = 0x636e7431 'cnt1' (set at 0x570be3);
  - `ScTriggerManager::AddTrigger` 0x4c2440 (0x568bc3) with `[0x7cb2c4]` 0x61435458 / `[0x7cb2bc]` 0x64505652 (set at 0x57091f / 0x570933).
- **Input 0x50** (INFERRED): the 'cnt1' elements are Windows VK codes (probe 3: Enter 0x0D, Space 0x20, arrows 0x25..0x28 on the same driver), so element 0x50 is **VK 'P'**.
- **Vtable 0x73c44c:** dtor 0x5707d0, Free 0x4f2bc0, `IsRequested` 0x491ed0, **`Fire` 0x570890**. Fire stores the state at `+0x1c` and calls the member pointer through thunk 0x6595a0, so it calls `GcToa::KeyPressed`.
- **`GcToa::KeyPressed`** 0x56fcf0 (to `ret 4` at 0x56ffcc):
  - acts only when the action id == `[0x73b830]` (the same id) and the state == 2 (the first frame down);
  - finds bone "Bip01" (0x417ca0), stops and cleans up a 3D sound (0x560360 / 0x5612a0), and creates/sets/releases a `GcEmitter` (0x5ea7f0 / 0x5ea9d0 / 0x5ea850). This is a particle effect on the Toa.
- **Does any path reach `OnEvent`?** The direct-call closure from 0x56fcf0 has 65 functions and **does not reach 0x60b320** (EXECUTED, `reach.py`). It contains 29 indirect calls that were not followed (INFERRED: emitter and sound virtuals).
- **Ruling:** the block is **not script-visible**. It registers a P-key trigger whose action spawns a particle emitter on Onua, with no script call and no state a native reads (INFERRED for the 29 indirect calls). It does nothing unless P is pressed, so A9 (no keys) is unaffected. Under Decision 13 the row note cites this ruling: no model and no A9 re-pin.

## §7 Missing files

Error ids are FourCC words, initialised at runtime (EXECUTED stores):
- kNoException 'none' 0x6e6f6e65 at [0x8391d0] (0x4d9937);
- kFileNotFound 'fnfd' 0x666e6664 at [0x83b024] (0x4d9175);
- kFileOpen 'fopn' 0x666f706e at [0x83a9a8] (0x4d9180);
- kFileClose 'fcls' 0x66636c73 at [0x839140] (0x4d918b);
- kFileRead 'flrd' 0x666c7264 at [0x83a98c] (0x4d91ac).

The names come from the decomp. The catch tables come from `tools/osi/exc.py --func`.

### 7.1 `gcbigsprite.loadhudsprite` with a missing file

1. **Mounting `<dir>.col` (EXECUTED).**
   - If ScFileManager::GetSubVolume returns null, SrMountCollectiveVolume runs (0x43f711).
   - That site sits in a `catch(!ScCatchMessage!)` (pad 0x43f720) plus `!exception!` and `...` pads at 0x43f740/0x43f760.
   - The catch compares the id: kFileClose (0x43f7c2) or kFileRead (0x43f7d6) clears the "use the collective path" flag [ebp−0x64c] (set at 0x43f337), so the loose name is used instead (0x43f841 → 0x43f870). Any other id rethrows `!ScCatchMessage!` (0x704014) at 0x43f82c.
   - INFERRED: a missing .col opens through ScCollectiveFile → ScFileStream::Open, whose only throw is kFileOpen (SrThrow at 0x4dc41c, EXECUTED). kFileOpen is neither tolerated id, so a missing .col (on a sprite that needs mounting) rethrows. The exact id that reaches this catch was not traced through ScCollectiveFile::Open 0x4cf150.
2. **ReadMe.bin (EXECUTED).** This step is taken only when b6 = 0 (0x43f890).
   - ScFileManager::Open at 0x43f912 is inside catch(`!ScCatchMessage!` / `!exception!` / `...`), with pads 0x43f9f0/0x43fa10/0x43fa30.
   - Any error sets rows = cols = 1 (0x43fa92/0x43fa9c).
   - The rethrow flag [ebp−0x63c] is only ever written 0 (0x43f8c7), so the error is **always swallowed**.
   - A missing ReadMe.bin therefore gives a 1×1 sprite.
   - If b6 ≠ 0, the grid is 3×3 with no ReadMe read (0x43fb0b/0x43fb1b), and a second, uncaught Open (0x43fe34; its exc chain holds only destructors) reads per-tile data. INFERRED role.
3. **The tile textures (EXECUTED that no throw exists; behaviour INFERRED).**
   - Each tile's GcHudSprite ctor calls ScDrawableMaterial::Create (0x526c5e) → LoadIndexed 0x550d50 → ScDrawableTexture::Load 0x5561c0.
   - Load probes `SrBlockFileExists` (0x55627a, 0x556310) for the name and for each known extension.
   - The call census of Load to its true end, 0x556961, contains **no SrThrow, ___throw or ScFileManager::Open**.
   - With no match, mTextureType stays 0 and no texture is created (decomp). Load returns normally, and the sprite later binds a null texture (SetTexture(stage, null)).
   - **What a missing .tga draws (INFERRED, not run).** The quad still draws with the §3.2 HUD states: TSS0 COLOROP = MODULATE (4), COLORARG1 = TEXTURE, plus the sprite's COLORARG2 / TFACTOR and alpha states (§3.5), all against stage 0 holding no texture.
     - Fixed-function D3D8 treats a stage with no texture in a driver-defined way. The usual results are opaque white for the TEXTURE argument, giving a diffuse- or TFACTOR-coloured rectangle, or black.
     - Neither result was verified here. Task 8's capture should include one missing-tile case to pin it.
   - The GcHudSprite ctor call sites (0x4402f4, 0x4403a4) have only a `delete` record, with no catch.
4. **The native and the VM (EXECUTED).** The native body 0x43dad0 has a short `.exc` record with **0 catches**. So:
   - A missing ReadMe.bin or a missing .tga **does not** error: the native returns the GcBigSprite variant as usual.
   - A rethrown mount error propagates out of the native into the VM. It is not swallowed (natives.md §5.1: no catch in the VM; §5.4 D16 applies only inside LoadArea's three blocks). The outermost `Vm::call` writes one `E` line and returns ok = false. If that happens under SetUpScripting or WinMain, the game ends there (opcodes.md "After an error").

### 7.2 `sctilefont.create` with a missing file

- **Mount (EXECUTED).** It follows the same mount pattern: SrMountCollectiveVolume (0x62711c) in catch(`!ScCatchMessage!`), pads 0x627126/0x627145/0x627165.
  - kFileClose (0x6271c7) or kFileRead (0x6271da) clears [ebp−0x6ec] and falls back to the loose name.
  - Any other id rethrows `!ScCatchMessage!` (0x758464) at 0x627230.
- **The font file is opened outside any catch (EXECUTED).** ScFileManager::Open at 0x6272d4 has an exc chain of destructor records only (0x6e33fe…0x6e345e).
  - Open throws **kFileNotFound** ('fnfd') when no volume holds the file: SrThrow at 0x4daeef pushes &0x83b024 and &kNoQualifier 0x839118.
- **The native (EXECUTED).** Body 0x611910 has one `.exc` site (0x61192c) whose only record is `delete` (0x4e4710), with 0 catches. On a throw, the half-built font's memory is freed and the error reaches the VM.
- **What the VM sees.** sctilefont.create with a missing font file **is a VM error** (kFileNotFound, or the rethrown mount error), recorded once by the outermost `Vm::call` (ok = false). No font variant is returned. D16 swallowing applies only if the call happens inside a LoadArea swallowing block (natives.md §5.4).

### 7.3 Gaps (not read in this task)

- PlatformViewSet / SetViewport values (§3.1).
- DrawJust's flags (§4.3).
- The ScTileFont ctor defaults (§4.4).
- The id that a missing .col raises through ScCollectiveFile::Open 0x4cf150 (§7.1).
- Which area node group renders last on lev0/frnt, and so the mode the HUD diffs against (§3.2). This is the open input for Task 8.
- What a null texture renders as on the reference driver (§7.1).

## §8 CheckWallCloseness: OnEvent(0x28, int 2, 'impc')

The event is `OnEvent(0x28, int 2, int 0x696D7063 'impc')` from `CheckWallCloseness` 0x587240, injected before the frame's `Process` ("pre").

### §8.1 Recording mode decides what you see (EXECUTED)
There are two ways to record a run. They give opposite pictures.

| Mode | Command | What frame 4 shows with 3 events (frames 2, 3, 4) |
|---|---|---|
| **Handler natives recorded** (the truth) | `tools/osi/menu_probe.py --osi $I/data/Base.osi --install $I --natives-impl real --frames 6 --event 2:0x28:2,0x696D7063 --event 3:… --event 4:… --report calls:4-4`, run from the merged `lomn-web-m2` tree | handler: `hide(14)`, `remove(14)`, `show(13)`, `hide(13)`, **`gctoa.toamustdie`**, `gccollisionphysicsgroup.spinmask(1.2f)`, **`gclegocamera.setcameraaimode(cam, 7)`**, `hide` of 0–13, 16–18, 23, 49, 50, 58, 67–69, `gccharacter.disablecollisions(255)`, `gctoa.setcodelife(0)`; then `Process`: `hideinventory` |
| **Handler natives dropped** | the same with `:quiet` on each event; also this task's first-round `wall.py`, whose `event_probe` Recorder drops native records while `injecting` | `hideinventory` only |

- **The first round's ruling was a recording artefact and is withdrawn.** The "26 hides skipped" do happen; the **event's handler** performs them in place of the script's frame-4 block.
- The −1159 / −32 instruction deltas measured only `Process`'s own work. They are not missing HUD calls.
- Every other first-round number in this section is superseded by §8.2.

### §8.2 What the event does (EXECUTED, handler-recorded `menu_probe`, diffs against the no-event run)
- **Each event costs 20 code life.** The recorded `setcodelife` values are 40 → 20 → 0. There is no `getcodelife` native, so the counter is script state. Its starting value (60 if the step is constant) is INFERRED: no `setcodelife` occurs in the no-event run.
  - 1st event: the handler tints a sprite red (`gcbigsprite.setcolor(255,0,0,255)`), blinks life light 15 (`show` / `hide` 13–15), then `setcodelife(40)`.
  - 2nd: `hide(15)` + `remove(15)`, the same blink on 14, then `setcodelife(20)`.
  - 3rd: `hide(14)` + `remove(14)`, `show` / `hide(13)`, then the death path in the table above (`toamustdie` … `setcodelife(0)`).
- **The third event is lethal whenever it lands:**
  - frames 100/101/102 → `toamustdie` at 102;
  - frames 100/200/300 → at 300 (no regeneration in between).
- **"Non-lethal" (natives.md §6.3) holds only for a single event.** The threshold is a count: 3 events = death.
- **Changes to engine state on death:**
  - code life 0;
  - `toamustdie`;
  - mask spin 1.2;
  - collisions disabled;
  - **camera mode 7**. Whether it sticks depends on when the third event lands relative to the script's frame-3 `setcameraaimode(cam, 3)` + spline setters. That in-frame order of `CheckWallCloseness` (Toa/physics) vs the script's `Process` has **no asm basis** in this doc, so both orders are given (EXECUTED, `menu_probe` recorded mode):

| Third event | pre (before Process) | post (after Process) |
|---|---|---|
| frame ≤ 2 | death, then frame-3 mode 3: **flight runs** (0,1,2 → death at 2, mode 3 at 3) | same: **flight runs** |
| frame 3 | death, mode 7, then the script's mode 3 in the same frame: **flight runs** (1,2,3 pre) | mode 3, then death + mode 7: **flight cancelled**, mode 7 sticks (1,2,3:post) |
| frame ≥ 4 | mode 7 sticks (2,3,4; 100,101,102) | mode 7 sticks (2,3,4:post) |

  - After frame 3 the script never sets mode 3 again (frames 5–699 of the no-event and event runs contain no camera call).
- **What mode 7 does in the camera** (EXECUTED reads, §1):
  - **Spline released:** `SetCameraAIMode` 0x45b490 releases the spline (old 3 → new 7, not 2 or 3).
  - **Target only:** Process's mode-7 branch (0x455bf7–0x455c51) only calls `SetTargetPosition` with a point whose y gets `+ [cam+0x3d4]`. The eye is never moved. That the point is the Toa's position is INFERRED.
  - **No OnEvent(6):** mode 7 never reaches the four spline guards (§1.3), so OnEvent(6) is never sent. The script's camera-event handler (`setcameraaimode(0)`, `gchuddirector.show(141)`, `startnewscreen`) never runs (EXECUTED guard census: the 5th site 0x45502f is mode-8-only, §1.3; INFERRED that the handler is not reached any other way).

### §8.3 The tell for owner check O6 / Decision 7
- **HUD visibility cannot tell the cases apart.** In the 3-event case, the handler hides exactly the indices the script's frame-4 block would hide:
  - 13 is shown, then hidden;
  - 14 and 15 are *removed* instead of hidden;
  - in the 2-event case, 15 is removed and `Process` hides the rest.
  - Either way, the same elements end up invisible.
  - The only HUD trace is transient: the red tint and the life-light blinks in frames 2–4, about 20 ms of screen time.
- **The tell is the death / camera-mode-7 path.** If the event fired three times in the original, the owner would SEE:
  - **Late death** (third event at frame ≥ 4, or at frame 3 with post order):
    1. **No camera fly-in.** The 4-second sweep (z 279.9 → 110.6) stops near its start, because mode 7 releases the spline. The camera then turns to look at the Toa (INFERRED target).
    2. **Onua dying** (the `toamustdie` path, with the mask spinning; the exact animation is INFERRED).
    3. **No menu reset.** Because OnEvent(6) is never sent, `show(141)` and `startnewscreen` never happen (INFERRED).
  - **Early death** (third event at frame ≤ 2, or at frame 3 with pre order): **a normal fly-in WITH a dead or dying Onua.** The spline is set and mode 3 is set after the death, so the flight runs. That OnEvent(6) then fires at 604 and the menu appears is INFERRED; the oracle does not model the camera.
- **What rules the event out:** a screenshot of the original menu that shows the camera at the end of the fly-in, with the menu items up **and Onua standing**, means the event did not fire three times. Both conditions are needed: the early-death case also shows the fly-in. The port's no-event state is then right.
- A single event, or two, leaves no visible trace after frame 4 except the code life (40 or 20). That is not visible on the menu (INFERRED).

## §9 The `Z` snapshot record (Decision 6; contract, M2a-3 Task 3)

The `Z` block shows the state the A9 digest cannot see: engine reference counts, HUD object state that only the renderer reads, the camera, and native state nothing reads. Both engines that run natives write it (C++ native, C++ wasm32, the Python own-models engine). A **replay** run (trace.md §6) runs no native, so it has no HUD or cinema state: it writes **no** `Z` lines, and the `Z` comparison is three-way.

### §9.1 Where and when
- One block per frame, in the trace, after the frame's last record: after the camera step (priority 3, §2.4), so the frame's `OnEvent(6)` and everything it caused come before it. The block has no `Z` before frame 0 (Setup is not a frame).
- Every run that writes `K` lines writes the blocks (A6, A9, the menu runs, conformance group `h`).
- The block is **not** part of the frame's `D` hash (controller ruling on Decision 6): A6/A9 `D` lines stay byte-identical. Digest mode writes one line per block instead (trace.md §2). `T` still covers the whole trace, `Z` lines included (controller ruling, fix 1).
- **Two gates break and Task 4a changes them** (trace.md §2): `engine/tests/osi_a6.cmake:45` (`digest C++ == Python (replay)`) and `Makefile:210` (osi-diff `cmp 2/4 digest native == Python (replaying the native natives.log)`) compare whole digest files, and the replay digest has no `Z` lines and so a different `T`. The replay comparison becomes the `D` lines alone, `T` excluded; the `Z` lines and `T` are compared three-way: C++ native, wasm32, and the Python full (own-models) run.
- `natives.log` carries no `Z` line, so the replay log is unchanged.

### §9.2 Lines
- Header: `Z %6d` (the frame number, as the `K` line).
- Then one line per item below, **sorted bytewise over the whole line** (C `strcmp`, Python `sorted` on the bytes), so no engine needs to agree on an iteration order.
- Value formats (trace.md §1): f32 `%08x` bits; double `%016x` bits; byte `%02x`; word `%04x`; int `%d`; a game object `%04x:%08x` (its minting type, as on `H` lines; natives.md §3.1); a byte string `%d <hex>` as on `S` lines (empty: `0` and nothing after it); **`-`** for a field no native has written since the object was created and whose constructor default is not cited (§4.4, §7.3 gaps). A `-` replaces the whole value, even a multi-word one.
- A subsystem writes lines only for state that exists: no camera lines in a run whose host models no camera (conformance group `h`), no `cs` line before the first `playcinematic`.
- **No timer line** (Decision 6 lists "the timer flags"): no timer is started on the front-end path (natives.md §6.2), and no engine models `GcTimer` state yet. A `timer.*` line is added to this table by the task that models it.

| Line | One per | Value | Source |
|---|---|---|---|
| `Z ref <type>:<handle> %d` | live game handle | the engine's references: total minus every script variant (the `G` meaning, natives.md §3.1); may be 0 | natives.md §2, §3 |
| `Z hud.node %08x <kind> <type>:<handle> <order>` | node the HUD director wrapped (`addbigsprite`, `addtextobject`) and has not removed | `%08x` = the id the add native returned; kind `sprite` / `text`; the wrapped object; order = `%d` index in the shown vector (draw order) or `-` when hidden | §3.1 (INFERRED from the decomp), §4.2 |
| `Z sprite.rect <type>:<handle> <x1> <y1> <x2> <y2>` | GcBigSprite | 4 f32 | `loadhudsprite`, `sethudposition` (§4.1) |
| `Z sprite.alpha <…> %02x` | GcBigSprite | low byte of w1 | `enablealphachannel` |
| `Z sprite.color <…> <r> <g> <b> <a>` | GcBigSprite | 4 f32 = f32(low byte × f32 0x3b808081) | `setcolor` |
| `Z sprite.zdepth <…> <z>` | GcBigSprite | f32 | `setzdepth` (no render effect, §4.1; compared anyway) |
| `Z sprite.file <…> %d <hex>` | GcBigSprite | the name argument's bytes | `loadhudsprite` |
| `Z sprite.b6 <…> %02x` | GcBigSprite | low byte of w5 (3×3 mode) | `loadhudsprite` |
| `Z text.pos <…> <x> <y> <z>` | ScTextObject | 3 f32 (+0x13c..+0x144) | `setposition` (§4.3) |
| `Z text.scale <…> <sx> <sy>` | ScTextObject | 2 f32 (+0x15c, +0x160) | `setscale` |
| `Z text.just <…> %04x` | ScTextObject | low 16 bits of w1 (+0x2a) | `setjustification` |
| `Z text.juston <…> %02x` | ScTextObject | low byte of w1 (+0x28) | `enablejustification` |
| `Z text.string <…> %d <hex>` | ScTextObject | the string (+0x30) | `setstring` |
| `Z text.font <…> <type>:<handle>` | ScTextObject | the font object (+0x2c) | `setfont` |
| `Z font.color <…> <r> <g> <b> <a>` | ScTileFont | 4 f32 = f32(int w × f32 0x3b808081) | `sctilefont.setcolor` (§4.4) |
| `Z font.transp <…> %02x` | ScTileFont | low byte of w1 (+0xb0) | `enabletransparency` |
| `Z font.file <…> %d <hex>` | ScTileFont | the name argument's bytes | `sctilefont.create` |
| `Z camera.mode %02x` | the viewport camera | `[cam+0x3a1]` | §1.1, §1.2 |
| `Z camera.spline <type>:<handle>` | | `+0x3a4`'s script handle, or `-` (none, or released, §1.2) | |
| `Z camera.index %02x %02x` | | low bytes of `+0x3b8`, `+0x3bc` | |
| `Z camera.duration %016x` / `Z camera.start %016x` | | doubles `+0x3a8`, `+0x3b0` | |
| `Z camera.flags %02x %02x` | | `+0x460` forward, `+0x461` look-ahead | |
| `Z camera.eye <x> <y> <z>` / `Z camera.target <x> <y> <z>` | | f32 `+0x150..`, `+0x15c..` | |
| `Z cs.playing %d` | the cinema state | 0 / 1 (natives.md §5.3: written, never read) | M2a-2 directors |

- The `sprite.*`, `text.*`, `font.*` lines are written for every live object of that game type that the HUD model knows, shown or not. An object of one of those types that a **conformance test native** minted (conformance.md §6 rule 23) is known with every field `-`.
- Ownership (I6): Task 4 owns `ref` and `camera`; Task 6 owns `hud`, `sprite`, `text`, `font`; `cs` stays with the M2a-2 directors code. Adding a line kind means amending this table first.
- `Z` is compared, never tainted (natives.md §7.3).

### §9.3 Worked example
A test native mints an `ScTextObject` (type 0x2e) as handle 1 with no engine reference; the script keeps it in a global, and in frame 0 calls `setposition(o, 400.0, 320.0, 0.0)`, `setscale(o, 1.5, 2.0)` and `setjustification(o, 0x10002)`. The block of frame 0 (this is conformance program `h-z-hud`):
```
Z      0
Z ref 002e:00000001 0
Z text.font 002e:00000001 -
Z text.just 002e:00000001 0002
Z text.juston 002e:00000001 -
Z text.pos 002e:00000001 43c80000 43a00000 00000000
Z text.scale 002e:00000001 3fc00000 40000000
Z text.string 002e:00000001 -
```
- `ref … 0`: the only reference is the global's variant.
- `just 0002`: the low 16 bits of 0x00010002 (§4.3: a word store at +0x2a).
- In digest mode the block becomes `Z      0        8 a948cf67ffb8462c` (8 lines, FNV-1a 64 of the 8 lines above, LFs included; computed by a scratch script over these bytes, and the sort order checked by the same script).

## §10 The HUD lines of the D3D8 state log (contract for Tasks 8 and 9)

The A9 state log of M1a-3 (`--state-log`) keeps its grammar for the area: `RS`, `TSS<n>`, `TEXTURE`, `XFORM <type> sum=%08x`, `DRAW`, `CLEAR`, `BEGINSCENE`/`ENDSCENE`, `MATERIAL`, `LIGHT`, `LIGHTENABLE`, `# node`, `# SrPushRenderMode 0x%05x`. **Everything up to the HUD pass stays byte-identical** to that grammar (F11's 626-line `frnt` log is the pin). The HUD pass adds the lines below; nothing else changes.

### §10.1 Records
- **`# viewport HdDr`**: the HUD viewport starts (§3.1, the second viewport, drawn last). Then `# SrPushRenderMode 0x8c034` and the ScDxStates writes the diff produces against the last mode (§3.2), as ordinary `RS`/`TSS` lines.
- **`XFORM PROJECTION sum=…` / `XFORM VIEW sum=…`** for the ortho camera's device calls (§3.3), with the existing checksum (31-hash over the 64 little-endian f32 bytes).
- **`RS` / `TSS` / `TEXTURE` / `XFORM WORLD`**: one line per device call that reaches the device, as today. Both writers count: ScDxStates (gated on the device's current value, §3.2) and the raw calls of `ScTileFont::Draw` (§3.6).
- **`MATERIAL d=<c> a=<c> s=<c> e=<c> p=<g>`**: the existing grammar (`engine/src/d3d/device8.cpp` `set_material`: each colour `<c>` = `(%.6g,%.6g,%.6g,%.6g)` r,g,b,a, power `%.6g`), one line per SetMaterial call.
  - `ScDrawableMaterial::Draw` 0x54ee70 calls it raw, for each texture entry, at 0x54eef9 (vtable +0xa8, `&entry+0x50`, entry = `[mat+0x14] + i·0x1ac`), **before** the texture's vfunc+0x14 at 0x54ef2f (EXECUTED, §3.4). So it precedes that entry's `TEXTURE` line, and it is not gated on the device's current value: every draw writes one.
  - The values a HUD sprite tile carries: `LoadIndexed` 0x550d50 stores the D3DMATERIAL8 at entry+0x50 unconditionally (stores 0x550e5e–0x550f60, which every path reaches through 0x550e58; constants read from the exe at 0x7366d4–0x736718; EXECUTED): diffuse (0.7,0.7,0.7,1) (bits `3f333333` ×3, `3f800000`), ambient (1,1,1,1), specular (1,1,1,1), emissive (0,0,0,0), power 80 (`42a00000`). The line is `MATERIAL d=(0.7,0.7,0.7,1) a=(1,1,1,1) s=(1,1,1,1) e=(0,0,0,0) p=80`.
  - INFERRED: that nothing rewrites entry+0x50 between `LoadIndexed` and the draw (no writer census), and that a font's materials (§3.6, the `ScDrawableMaterial::Create` at 0x627a02) get the same values. Fills and lines (§3.7) set their own raw material (zeroed, plus the colour).
- **`TEXTURE <stage> <name>`**: `<name>` is the VFS path the texture was loaded from, case preserved, `/` separators, with the extension the loader matched (e.g. `Root/Data/Art/MenuArt/cursor0000.tga`; a multi-tile sprite's tile `…/name0_1.tga` per §3.4); `NULL` for no texture (a missing file, §7.1).
- **`DRAW hud.<kind> <type>:<handle> <where> call=<DIP|DP> prim=%d count=%d fvf=0x%03x vsum=%08x`**, one per primitive call:
  - kind and `<where>`: `sprite … tile=%d,%d` (row, column; §3.4); `glyph … ch=%02x` (the text object; the byte code; a 2-byte code prints `%04x`; §3.6); `fill …` and `line …` (`gcfilledrectangle`, `gcline`/`gcrectangle`; unreachable from Base.osi, §3.7, kept for completeness) with `<where>` = `-`;
  - `call=DIP` for DrawIndexedPrimitive, `DP` for DrawPrimitive; `prim` the D3DPT number (5 TRIANGLESTRIP, 6 TRIANGLEFAN, 2 LINELIST); `count` the primitive count;
  - `fvf` the vertex format set by the last SetVertexShader (0x112 sprites and glyphs, 0x42 lines and fills, §3.4, §3.7). SetVertexShader writes no line of its own, so the area part of the log does not change;
  - `vsum`: the 31-hash of `mat_sum` over the little-endian f32 bytes of each vertex's **transformed** position (x′, y′, z′), vertices in vertex-buffer order (§3.4: v0..v3; §3.7: its four corners; lines: buffer order). Each coordinate is `((x·m0j + y·m1j) + z·m2j) + m3j` with WORLD `m` (row vectors), every product and sum rounded to f32. This is what makes "cursor under the pointer" and "text in its hit box" checkable from the log (Review focus 3).
- A deferred item (priority 100, §3.4, §3.6) logs its lines where it really draws: after every non-deferred item, in deferral order. A multi-tile big sprite whose last tile defers draws its non-deferring tiles twice (§3.4): both draws are logged.

### §10.2 Worked example (hand-computed from §3.4; zeros are +0.0)
The cursor, `loadhudsprite(50, 50, 82, 82, "Root/Data/Art/MenuArt/cursor0000", 0)` (F6), a 1×1 sprite: w = h = 32, t = (82 − 16, 82 − 16, −7499) = (66, 66, −7499), s = 8.742278e−08 (`33bbbd2e`).
- WORLD = [[32,0,0,0],[0,−32,32·s,0],[0,−s,−1,0],[66,66,−7499,1]], bits `42000000 0 0 0  0 c2000000 363bbd2e 0  0 b3bbbd2e bf800000 0  42840000 42840000 c5ea5800 3f800000`.
- Vertices: v0 (50, 50, −7499), v1 (82, 50, −7499), v2 (50, 82, −7499), v3 (82, 82, −7499): the top-left is (x1, y1), as §3.4 says.
- The draw's lines, after the §3.5 states:
```
MATERIAL d=(0.7,0.7,0.7,1) a=(1,1,1,1) s=(1,1,1,1) e=(0,0,0,0) p=80
TEXTURE 0 Root/Data/Art/MenuArt/cursor0000.tga
XFORM WORLD sum=381857c2
DRAW hud.sprite 0026:<handle> tile=0,0 call=DIP prim=5 count=2 fvf=0x112 vsum=9f18392c
TEXTURE 0 NULL
```
- The `RS SOFTWAREVERTEXPROCESSING 0` of §3.4 step 2 appears only when the device held another value (ScDxStates). Computation: the scratch f32 script of this task (struct round-trips; each op rounded to f32), not an engine.

## §11 MOUSESPEC (Decision 4; `tools/osi/keys.py` `expand_mouse`, `tools/tests/test_keys.py`)

A MOUSESPEC is a space-separated list of tokens. Each takes effect at its frame and holds until the next token of the same kind:
- `x,y@f`: the cursor, in client pixels at the game resolution (scale 1.0, probe 3 S5), each in 0..65535 (the driver's u16 `[0x83b396]`/`[0x83b394]`, §5.1).
- `lmb+@f` / `lmb-@f`: button 1 down / up; `rmb` = 2, `mmb` = 3, `xmb` = 4 (rgbButtons[0..3], §5.1). lmb = 1 and rmb = 2 are the menu's activate/back (probe 3); mmb/xmb follow the DirectInput order (INFERRED).
- Refused: a malformed token, a negative frame, a coordinate outside 0..65535, an unknown button, and two tokens of one kind (the position, or one button) at one frame.

Expansion, frames 0..N−1 (hud.md §5.1, EXECUTED there):
- **No MOUSESPEC → no mouse source.** A6/A9 have none, so their logs do not change (Decision 12).
- **Any MOUSESPEC → every frame** `OnEvent(int 26, int x, int y)`, from frame 0 on. Before the first position token the cursor is (0, 0): a host choice, since the windowed driver reads `GetCursorPos`, so any value is a possible input.
- Then, in the same mouse step, buttons 1, 2, 3, 4 in that order: `OnEvent(int 23, int b)` on the first frame the button is down, `OnEvent(int 24, int b)` on every later frame it stays down, `OnEvent(int 25, int b)` on the first frame it is up again. A release of an up button or a press of a down one is not a transition, so it sends nothing new.
- Every argument is an int (0x10): the event id through `CreateVariant` 0x413ed0, x and y through 0x4183f0, the button through 0x42a5b0 (`mov word [ebx], 0x10`, EXECUTED at 0x42a5b5); the other arguments are nothing.
- Position in the frame: the mouse step of §2.4 (priority 1, after the key mapper, before the script's `Process`). natives.log writes each of these calls as `C -1` (trace.md §6).
- `keys.py --mouse "MOUSESPEC" N` prints the expansion (`<frame> OnEvent <v> …`), as `keys.py "KEYSPEC"` does for the keys; the C++ `--mouse` printer must match it line for line (Task 4a).
