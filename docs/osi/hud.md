# HUD and host behaviour (M2a-3 asm rulings)

Task 2b sections only (§3, §4, §7). Task 2a writes §1, §2, §5, §6 and §8; the controller merges the two files.

**Labels.** EXECUTED means read from `objdump -d --x86-asm-syntax=intel` of the authoritative exe (natives.md §1.4; sha256 b372afb9…3ab8), with the instruction address given. INFERRED means read from the Ghidra decomp, or reasoned from it, without an asm pin. D3D8 slots: SetTransform +0x94, SetMaterial +0xa8, SetRenderState +0xc8, GetRenderState +0xcc, SetTexture +0xf4, Get/SetTextureStageState +0xf8/+0xfc, DrawPrimitive +0x118, DrawIndexedPrimitive +0x11c, SetVertexShader +0x130, SetStreamSource +0x14c, SetIndices +0x154, CreateVertexBuffer +0x5c, CreateIndexBuffer +0x60. RS and TSS numbers follow `engine/include/lomn/d3d/device8.h`.

**Function extents.** Two functions here run past the decomp's `size`, and every census below uses the true extent:
- `GcBigSprite::GcBigSprite` 0x43f210: decomp 4,191 bytes; it really runs to `ret 0x1c` at **0x440455** (next entry 0x440460, `LoadHudSprite`).
- `ScTileFont::ScTileFont` 0x626b40: decomp 3,848 bytes (ends 0x627a48); it really runs to `ret 0x4` at **0x627b89** (next entry 0x627b90, `SsFontCharacter::~SsFontCharacter`). The `Close`/`UnMountVolume` tail at 0x627b2c–0x627b4a lies past the decomp size.
- `ScTileFont::Draw` 0x627dd0 ends at `ret 0x10` 0x6288e9; `ScDrawableTexture::Load` 0x5561c0 at `ret 0x4` 0x556961.

**Stop condition.** Not triggered. No HUD path draws through `ID3DXSprite`/`ID3DXFont` or with RHW vertices. The only D3DX call on the path is `D3DXCreateTextureFromFileInMemory` (0x5563ab, texture type 3 in `ScDrawableTexture::Load`), which creates a texture and draws nothing. Every HUD vertex format is untransformed: 0x112 for sprites and glyphs, 0x42 for lines and filled rectangles.

## §3 Render path

### 3.1 Frame and pass order

- **The viewport list (EXECUTED).** `ScScreen::AddViewPort` and `MoveViewToFront` insert at the sentinel at screen+0x2a3fc. `insert_nodes` 0x4e0f30 inserts before the position, so both calls are a push_back. `RenderLoop` 0x49d550 starts at the head [screen+0x2a404] and follows node+8. The node layout is {data +0, prev +4, next +8}.
- **The main view is added first (EXECUTED).** `SetUpView` calls the GcViewPort ctor at 0x43a54c, and the ctor calls AddViewPort at 0x447f65.
- **The HUD view is added second and drawn last (EXECUTED).** The HUD view is an ScViewPort (vtable 0x7037c8) with id 'HdDr' 0x48644472, stored at [0x8390ac]. AddViewPort at 0x43a85a adds it, then MoveViewToFront at 0x43a872 puts it at the end. **The HUD is drawn after the area, every frame.** `gcviewport.moveviewtofront` (FUN_00447920) is not called by Base.osi (natives.json).
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

ScDxStates (INFERRED, decomp 0x64b1e0 / 0x64b250) calls GetRenderState into `sRenderStates[s]`. It sets the state only when the value differs, and it keeps the previous value for Restore*.

Mode bits are diffed against `gLastRenderMode`. HUD mode 0x8c034 has bits 0x80000, 0x8000, 0x4000, 0x20, 0x10 and 0x4. The main mode 0x10635 has none of the 0x80000 or 0x8000/0x4000 group. So the main → HUD transition sets:

- **0x80000 on (main had it off), 0x531128–0x53119a:**
  - TSS0 ALPHAOP(4) = 2 and ALPHAARG1(5) = 2 (TEXTURE).
  - ALPHABLENDENABLE(0x1b) = 1, SRCBLEND(0x13) = 5 (SRCALPHA), DESTBLEND(0x14) = 6 (INVSRCALPHA).
  - ALPHAREF(0x18) = 0, ALPHATESTENABLE(0xf) = 1, ZWRITEENABLE(0xe) = 0.
  - TSS0 COLOROP(1) = 4 (MODULATE) and COLORARG1(2) = 2 (TEXTURE).
- **The reverse transition (HUD → main next frame), 0x5311b0–0x5311d5:** ALPHABLENDENABLE = 0, ALPHAREF = 1, ALPHATESTENABLE = 0, ZWRITEENABLE = 1.
- **ZENABLE(7):** bit 0x10000 is clear.
  - If `GcGraphicsOptions::CanUseW` (0x531277), bit 0x8000 gives ZENABLE = 0 (0x53129d), so there is no depth test.
  - Otherwise ZENABLE = 1 (0x531284).
  - The main mode sets ZENABLE = 2 (W) when CanUseW (0x53126e).
- **FILLMODE(8) = 3 (SOLID)** from bit 0x4000 (0x5312cd).
- **LIGHTING(0x89) = 0**, because bit 1 is clear (0x5312e8).
- **CULLMODE(0x16) = 1 (NONE)**, because bits 0x400 and 0x800 are clear (0x531324).
- **Fog:** bit 0x200 is clear. If FOGENABLE(0x1c) reads non-zero (a raw GetRenderState at 0x53133a), `GcGraphicsOptions::SetFog(false)` runs (compare at 0x531362, `push 0` at 0x531368).

### 3.3 The HUD transform

- **The ortho camera (EXECUTED).**
  - SetUpCamera 0x439870 calls the GcOrthoCamera ctor 0x406df0 with:
    - pos 0x703364 = (0,0,0), target 0x703370 = (0,0,−1500), up 0x70337c = (0,1,0);
    - left [0x70338c] = 0.0 → +0x1cc, right = width → +0x1d4;
    - top [0x703388] = 0.0 → +0x1c8, bottom = height → +0x1d0 (stores 0x406e2c–0x406e4a).
  - Width and height come from GetResolutionWidth/Height (calls 0x4398e9 / 0x4398da).
  - Near = 0.0 (0x6fe604) and far = 15000.0 (0x6fe600) come through the ScCamera ctor 0x492df0 (+0xfc / +0x100).
- **BuildOrthoMatrix 0x406f20 (EXECUTED; the double 2.0 at 0x6fe610, the f32 1.0 at 0x6fe628).** Row-vector matrix:
  - m00 = 2/W, m11 = 2/(0 − H) = −2/H, m22 = 2/15000;
  - m30 = −1, m31 = +1, m32 = (f+n)/(f−n) = 1, m33 = 1;
  - all other entries 0.
  - Hence **x_ndc = 2x/W − 1, y_ndc = 1 − 2y/H, z_ndc = 1 + z/7500**. HUD space is in pixels, with the origin at the top-left and y pointing down.
- **Device calls (EXECUTED).** `GcOrthoCamera::Render` 0x406ec0 → `ScRenderer::SetProjectionTransform` 0x530640 → SetTransform(3, PROJECTION), and `SetViewTransform` 0x530560 → SetTransform(2, VIEW). Both go straight to the device [0x73232c], not through ScDxStates.
- **The view matrix (INFERRED, decomp of BuildViewMatrix 0x406030; constants EXECUTED at 0x6fe2b8 = 1.0).** The view matrix is identity, with −0.0 in m30/m31/m32. The asm stores are not pinned.
- **HUD space does not scale with resolution (EXECUTED).**
  - The ortho camera is built once, in DoSetUp, from the current resolution.
  - No HUD sprite or text function calls GetResolutionWidth/Height (0x43ae50 / 0x43ae70): their caller census contains none.
  - `GcBigSprite::SetHudPosition` 0x441860 passes the raw coordinates.
  - So at 1280×720, text placed at x = 400 stays at pixel 400.
- **The z values (EXECUTED).** Sprite tiles are placed at tz = zDepth − 7500.0 (0x72ef80) in the ctor, which is −7499 for the default zDepth 1.0 (0x72ef60). `GcHudSprite::SetPosition` 0x526fd0 sets tz = 0.0 + [0x72efbc] = −7499.0 and ignores mZDepth. Glyphs use −7499.0 (0x758218). So z_ndc = 1/7500 ≈ 0.000133, at the near plane.

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
  1. SetStreamSource(0, [0x7380e4], 0x20) and SetVertexShader(0x112).
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
- **Restores bypass ScDxStates.** Draw restores with raw device calls, so ScDxStates' cached `sRenderStates` can go stale after text draws (INFERRED consequence; the port's state log must model both writers).

### 3.7 Lines and rectangles: FVF 0x42 (EXECUTED)

- **`ScRenderer::DrawTriangles` 0x531380 is not on the HUD path.** Its only callers are `GcSurfaceOctree::RenderOctNode` (0x464b99, 0x464d90).
- **`ScRenderer::DrawLine` 0x530920** is called by `GcLine::Render` 0x40509e, by `ScCamera::Render` (12 sites, 0x492feb–0x49334f), by `GcToa::Render` 0x56acb9 and by `GcMotionSystem::RenderMotion` (0x58a72a, 0x58a7b3).
- **`GcRectangle::Render` 0x402350 calls `ScRenderer::DrawLines` 0x530d50 (0x402601).** That path is on the HUD: `gcrectangle.*`, `gcfilledrectangle.*` and `gchuddirector.addline/addrectangle/addfilledrectangle` are all `called_by_base_osi: true` in natives.json. Whether the M1/M2 front-end screens actually reach them was **not established** here (no runtime trace).
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
- The BuildViewMatrix asm stores (§3.3).
- DrawJust's flags (§4.3).
- The ScTileFont ctor defaults (§4.4).
- The id that a missing .col raises through ScCollectiveFile::Open 0x4cf150 (§7.1).
- Whether the front-end screens reach GcRectangle/GcFilledRectangle at runtime (§3.7).
