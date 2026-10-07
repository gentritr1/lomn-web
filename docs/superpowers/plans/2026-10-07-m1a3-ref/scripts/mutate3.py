# Deliberate defects in the M1a-3 state logic; each must make at least one ctest fail (unit / a7_* / a9_* / gpu_mesh /
# gpu_ffp / osi_probe / ffp_oracle; a9_oracle_real runs when LOMN_DATA is set). The no-op control must SURVIVE. Restores every file.
# Usage: python3 mutate3.py <work dir>
import os, subprocess, sys
W = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'work')
OSI = 'engine/src/sage/osi.cpp'
SF, FFP, DEV, AREA, RHI = 'engine/src/view/sage_frame.cpp', 'engine/src/d3d/ffp.cpp', 'engine/src/d3d/device8.cpp', 'engine/src/view/area.cpp', 'engine/src/rhi'
M = [
 (SF, 'd.dx_set_rs(rs::LIGHTING, (m & 1) ? 1u : 0u);', 'd.dx_set_rs(rs::LIGHTING, 1u);', 'render mode bit 0 ignored: area nodes lit'),
 (SF, '      d.dx_set_rs(rs::ALPHAREF, 1);\n', '      d.dx_set_rs(rs::ALPHAREF, 0);\n', 'leaving a 0x80000 mode sets ALPHAREF 0'),
 (SF, 'd.dx_set_rs(rs::CULLMODE, (m & 0x400) ? kCullCW : (m & 0x800) ? kCullCCW : kCullNone);', 'd.dx_set_rs(rs::CULLMODE, (m & 0x400) ? kCullCCW : (m & 0x800) ? kCullCW : kCullNone);', 'cull bits swapped'),
 (SF, "  if ((m & 0x200) == 0 || !can_fog) {", "  if ((m & 0x200) != 0 || !can_fog) {", 'fog bit inverted'),
 (SF, '    ctx_flag_ = true;\n    set_fog(true);', '    set_fog(true);', 'plnt/tre1 context flag not set (alpha pass blends)'),
 (SF, '  if (plant) {\n    set_fog(false);\n    ctx_flag_ = false;\n  }', '  if (plant) {\n    ctx_flag_ = false;\n  }', 'fog left on after plnt/tre1'),
 (SF, '    if (ctx_flag_) {\n      d.dx_set_rs(rs::ALPHABLENDENABLE, 0);', '    if (false) {\n      d.dx_set_rs(rs::ALPHABLENDENABLE, 0);', 'DrawX context flag ignored (R14)'),
 (SF, '    d.dx_set_rs(rs::ALPHAFUNC, kCmpGreater);\n    if (ctx_flag_)', '    d.dx_set_rs(rs::ALPHAFUNC, kCmpGreaterEqual);\n    if (ctx_flag_)', 'alpha pass GREATEREQUAL'),
 (SF, '  if (transparency < 1.0f) {\n    d.dx_set_rs(rs::ZBIAS, 16);', '  if (transparency < 1.0f) {\n    d.dx_set_rs(rs::ZBIAS, 0);', 'model fade without ZBIAS 16'),
 (SF, '  if (transparency == 1.0f && sd.trans < 1.0f) {', '  if (false) {', 'material mTrans (TFACTOR) path removed'),
 (SF, '      if (id == kIdSort2 || (id & 0xffu) == \'w\') {', '      if (id == kIdSort2) {', "trans ids ending in 'w' not deferred"),
 (SF, 'std::stable_sort(mv.begin(), mv.end(), [](const auto& x, const auto& y) { return x.first < y.first; });', 'std::stable_sort(mv.begin(), mv.end(), [](const auto& x, const auto& y) { return x.first > y.first; });', 'moveables farthest first'),
 (SF, '  d.set_rs(rs::DESTBLEND, kBlendInvSrcAlpha);\n  d.set_rs(rs::DITHERENABLE, 0);', '  d.set_rs(rs::DITHERENABLE, 0);', 'CreateDirectXWindow DESTBLEND missing (blend factors ONE/ZERO-style)'),
 (AREA, '      d.trans = 0.0f;            // mTrans (@0x0073660c)', '      d.trans = 1.0f;', '"_glass" not transparent'),
 (AREA, '    if (v > 0.9f) v *= 1.2f;', '    (void)0;', 'light intensity x1.2 rule removed'),
 (AREA, '    if (type < 3) {  // "LmbA"', '    if (type < 5) {  // "LmbA"', 'default lighting threshold'),
 (AREA, '        out_.fog.start = -static_cast<float>(v[1]);', '        out_.fog.start = static_cast<float>(v[1]);', 'SetFog start not negated'),
 (AREA, '          if (r.trans == 1.0f && r.u == 0.0f && r.v == 0.0f) continue;', '          if (r.trans == 1.0f) continue;', 'textures.slb skip rule widened (scroll-only records dropped)'),
 (AREA, "      for (int c = 0; c < 4; ++c) o[8 + c] = m.has_colors ? d3d::quantize_unorm8(x.colors[v * 4 + c]) : 1.0f;", "      for (int c = 0; c < 4; ++c) o[8 + c] = m.has_colors ? x.colors[v * 4 + c] : 1.0f;", 'vertex colours not quantised to D3DCOLOR'),
 (FFP, "  k.cull = r[rs::CULLMODE] == kCullCW ? rhi::CullMode::Front", "  k.cull = r[rs::CULLMODE] == kCullCW ? rhi::CullMode::Back", 'D3DCULL_CW mapped to Back'),
 (FFP, '  k.depth_bias = -static_cast<int32_t>(std::min<uint32_t>(r[rs::ZBIAS], 16));', '  k.depth_bias = static_cast<int32_t>(std::min<uint32_t>(r[rs::ZBIAS], 16));', 'ZBIAS sign flipped'),
 (FFP, '      f = clamp((u.fog.y - w) / (u.fog.y - u.fog.x), 0.0, 1.0);', '      f = clamp((w - u.fog.x) / (u.fog.y - u.fog.x), 0.0, 1.0);', 'linear fog factor inverted (WGSL)'),
 (FFP, '      att = 1.0 / max(at.x + at.y * dist + at.z * dist * dist, 1e-20);', '      att = 1.0 / max(at.x + at.z * dist * dist, 1e-20);', 'point attenuation drops a1 (WGSL)'),
 (FFP, '    o.color = select(vec4f(1.0), c, u.a.y > 0.5);', '    o.color = select(u.mat_diffuse, c, u.a.y > 0.5);', 'LIGHTING off uses the material instead of white (WGSL)'),
 (FFP, '    case 5u: { return a > r; }', '    case 5u: { return a >= r; }', 'alpha test GREATER evaluated as >= (WGSL)'),
 (FFP, '  o[kD + 0] = float(s.rs[rs::ALPHAREF] & 0xff);', '  o[kD + 0] = 0.0f;', 'ALPHAREF not passed to the shader'),
 (FFP, "  o[kE + 2] = s.rs[rs::ALPHABLENDENABLE] ? 0.0f : 1.0f;", "  o[kE + 2] = 0.0f;", 'opaque draws write their alpha'),
 (FFP, '  dif = dif + ld.rgb * (max(dot(wn, dir), 0.0) * att);', '  dif = dif + ld.rgb * (abs(dot(wn, dir)) * att);', 'N.L not clamped (WGSL)'),
 ('engine/include/lomn/d3d/device8.h', '    if (get_rs(state) != value) set_rs(state, value);', '    set_rs(state, value);', 'ScDxStates filter removed'),
 (RHI + '/null_device.cpp', '    if (!frame_open_) {  // M1a-3 parity with WebGPU: a draw outside begin_frame/end_frame is a counted rejection', '    if (false) {', 'null out-of-frame draw accepted'),
 (RHI + '/webgpu_device.cpp', 'rpd.primitive.frontFace = p.mesh_desc.front_face == FrontFace::CW ? WGPUFrontFace_CW : WGPUFrontFace_CCW;', 'rpd.primitive.frontFace = WGPUFrontFace_CCW;', 'WebGPU front face ignored'),
 (FFP, '  const float c = x >= 1.0f ? 1.0f : x > 0.0f ? x : 0.0f;', '  const float c = x >= 1.0f ? 1.0f : x <= 0.0f ? 0.0f : x;', 'NaN vertex colour reaches the int conversion (UBSan finding; caught under -fsanitize only: expected MISSED here)'),
 # REF3 revision 2: OSI caps (unit + osi_probe) and the CPU reference of the FFP formulas (ffp_oracle; gpu_ffp's own
 # cases cannot see these: attenuation2 = 0, w = 1, alpha on exact bytes).
 (OSI, '        if (out.size() >= kMaxOsiCallsPerMethod) return fail(err, "method exceeds kMaxOsiCallsPerMethod");\n', '', 'OSI calls-per-method cap removed'),
 (OSI, '        if (args_total > kMaxOsiArgsPerMethod) return fail(err, "method exceeds kMaxOsiArgsPerMethod");\n', '', 'OSI args-per-method cap removed'),
 (OSI, '    if (st.size() > kMaxOsiStackDepth) return fail(err, "literal stack exceeds kMaxOsiStackDepth");\n', '', 'OSI literal-stack cap removed'),
 (OSI, '        if (string_bytes > kMaxOsiStringArgBytes) return fail(err, "method exceeds kMaxOsiStringArgBytes");\n', '', 'OSI string-bytes cap removed'),
 (OSI, '    if (!seen.insert(off).second) return true;\n', '    seen.insert(off);\n', 'check_all re-walks shared bytecode'),
 (OSI, '  *this = OsiFile();  // no partial state after an error\n', '', 'failed OSI parse keeps partial tables'),
 (FFP, '      att = 1.0f / std::max(l.attenuation0 + l.attenuation1 * dist + l.attenuation2 * dist * dist, 1e-20f);', '      att = 1.0f / std::max(l.attenuation0 + l.attenuation1 * dist, 1e-20f);', 'reference attenuation drops a2 (CPU reference)'),
 (FFP, '    else if (mode == kFogExp2) f = std::exp(-(de * w) * (de * w));', '    else if (mode == kFogExp2) f = std::exp(-de * w * w);', 'reference EXP2 fog squares only w (CPU reference)'),
 (FFP, '    const float a8 = std::floor(std::min(1.0f, std::max(0.0f, alp.a)) * 255.0f + 0.5f), r = float(s.rs[rs::ALPHAREF] & 0xff);', '    const float a8 = std::floor(std::min(1.0f, std::max(0.0f, alp.a)) * 255.0f), r = float(s.rs[rs::ALPHAREF] & 0xff);', 'reference alpha test truncates instead of rounding (CPU reference)'),
 (SF, '  d.note("node StaticLight");', '  d.note("node StaticLight" "");', 'no-op control (must SURVIVE)'),
]
env = dict(os.environ)
def run(cmd):
    return subprocess.run(cmd, cwd=W, shell=True, capture_output=True, text=True, env=env)
tests = 'unit|a7_|a9_|gpu_mesh|gpu_ffp|osi_probe|ffp_oracle'
results = []
for path, old, new, what in M:
    p = os.path.join(W, path); src = open(p).read()
    assert src.count(old) == 1, (path, what, src.count(old))
    open(p, 'w').write(src.replace(old, new))
    try:
        b = run('timeout 900 cmake --build build/native')
        if b.returncode != 0:
            results.append((what, 'BUILD-FAIL')); print(results[-1], flush=True); continue
        t = run('timeout 900 ctest --test-dir build/native -R "%s" --timeout 300' % tests)
        failed = [l.split(':')[1].split()[0] for l in t.stdout.splitlines() if 'Test' in l and '#' in l and ('***' in l or 'Failed  ' in l)]
        results.append((what, 'CAUGHT ' + ','.join(failed) if t.returncode != 0 else 'MISSED'))
    finally:
        open(p, 'w').write(src)
    print(results[-1], flush=True)
b = run('timeout 900 cmake --build build/native'); t = run('timeout 900 ctest --test-dir build/native --timeout 300')
caught = sum(1 for w, r in results if r.startswith('CAUGHT'))
print('caught %d of %d (the no-op control must be MISSED); restored build rc %d, full ctest rc %d' % (caught, len(results), b.returncode, t.returncode))
