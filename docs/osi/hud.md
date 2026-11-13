# HUD and host behaviour (M2a-3 asm rulings)

Binary: `LEGOBionicle.exe` (sha256 b372afb9…ab8), read as data with `objdump -d --no-show-raw-insn -M intel`. It was never run.

Labels:
- **EXECUTED**: read from the disassembly at the cited address, or measured with a script run (the command is named).
- **INFERRED**: a reading that no run or census closed. The reason is stated.

Function extents come from flow-following the binary to every `ret` and the next entry, checked against `.exc`, never from the decompiler's size. Scratch tools (not committed): `ext.py` (extent and call census), `spline.py` (emulator of `Accelerate`/`Interpolate`), `fevt.py`/`fevt2r_w.py` (F_evt, F_evt2 sweep), `pins2.py` (pins), `viewpin0.py` (VIEW/PROJECTION), `reach.py`/`reads.py` (call closures), `wall.py` (first-round §8, handler natives dropped; superseded). Committed: `tools/osi/menu_probe.py` (§8).

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

**The four `OnEvent(6)` guards** (mode 2 and mode 3, each forward and reverse) read only time, start, duration and the mode and flag bytes. None reads Toa, physics or collision state. **F18 is confirmed, so the stop condition does not fire.** A fifth `OnEvent` call at 0x45502f is outside these four guards and was not followed.

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
- **Camera mode and pause:** the direct-call closure of 0x44a7a0 and 0x613f80 (73 functions, 24 indirect calls not followed; `reads.py`) never reads `[0x7032c4]` (pause), `[cam+0x3a1]`, `[0x7032bc]` or `[0x7032b8]`. **So the VK triggers fire in every camera mode, including during the spline flight (mode 3)**, unlike the key mapper (§5.2).
- **Open:** which trigger tree `ProcessTriggers` walks depends on the context providers (`[0x79908c]`, the maps at `[0x799040]` / `[0x79903c]`, `ScContextManager` 0x4d4300). That the 'scrP'/'scrp' context is active on the front end is INFERRED. The oracle's Task 1 run shows Enter at frame 220 reaching the script (a new spline at 258).
- **Ruling** (EXECUTED path, INFERRED context): **Enter, Space and the arrows reach the menu** as `Input(C1, VK, state, f, f)` at priority 0, before the script's `Process` in the same pass, in every camera mode. Whether pausing stops `GcControllerProcess` itself (the pm's `[proc+9]`) was not read.

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
- **Each event costs 20 code life.**
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
  - **camera mode 7**. The script never sets mode 3 again after frame 3, so this sticks (frames 5–699 contain no camera call).
  - Exception: if the third event lands *before* frame 3 (frames 0, 1, 2), the script's own frame-3 `setcameraaimode(cam, 3)` restarts the flight.
- **What mode 7 does in the camera** (EXECUTED reads, §1):
  - **Spline released:** `SetCameraAIMode` 0x45b490 releases the spline (old 3 → new 7, not 2 or 3).
  - **Target only:** Process's mode-7 branch (0x455bf7–0x455c51) only calls `SetTargetPosition` with a point whose y gets `+ [cam+0x3d4]`. The eye is never moved. That the point is the Toa's position is INFERRED.
  - **No OnEvent(6):** mode 7 never reaches the four spline guards (§1.3), so OnEvent(6) is never sent. The script's camera-event handler (`setcameraaimode(0)`, `gchuddirector.show(141)`, `startnewscreen`) never runs (INFERRED from the guard census; the 5th site 0x45502f was not followed).

### §8.3 The tell for owner check O6 / Decision 7
- **HUD visibility cannot tell the cases apart.** In the 3-event case, the handler hides exactly the indices the script's frame-4 block would hide:
  - 13 is shown, then hidden;
  - 14 and 15 are *removed* instead of hidden;
  - in the 2-event case, 15 is removed and `Process` hides the rest.
  - Either way, the same elements end up invisible.
  - The only HUD trace is transient: the red tint and the life-light blinks in frames 2–4, about 20 ms of screen time.
- **The tell is the death / camera-mode-7 path.** If the event fired three times in the original, the owner would SEE:
  1. **No camera fly-in.** The 4-second sweep (z 279.9 → 110.6) stops near its start, because mode 7 releases the spline in frame 4. The camera then turns to look at the Toa (INFERRED target).
  2. **Onua dying** (the `toamustdie` path, with the mask spinning; the exact animation is INFERRED).
  3. **No menu reset.** Because OnEvent(6) is never sent, `show(141)` and `startnewscreen` never happen (INFERRED).
- **What rules the event out:** a screenshot of the original menu that shows the camera at the end of the fly-in, with the menu items up and Onua standing, means the event did not fire three times. The port's no-event state is then right.
- A single event, or two, leaves no visible trace after frame 4 except the code life (40 or 20). That is not visible on the menu (INFERRED).
