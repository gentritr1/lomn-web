# Deliberate defects in the M1a-2 code; each must make at least one ctest (unit / a7_*) fail. Restores every file.
import os, subprocess, sys
W = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'work')
M = [
 ('engine/src/view/area.cpp', 'pd.depth_compare = rhi::CompareFunc::LessEqual;', 'pd.depth_compare = rhi::CompareFunc::Always;', 'depth test off'),
 ('engine/src/view/area.cpp', '  if (c.a <= u.params.y) {\n    discard;\n  }\n', '', 'alpha test (discard) removed'),
 ('engine/src/view/area.cpp', 'o.color = select(u.diffuse, c, u.params.x > 0.5);', 'o.color = u.diffuse;', 'vertex colours ignored'),
 ('engine/src/view/area.cpp', 'p.world = mat_mul(rot, mat_translation(r.pos[0], r.pos[1], r.pos[2]));', 'p.world = mat_mul(mat_translation(r.pos[0], r.pos[1], r.pos[2]), rot);', 'world = T * RotX (wrong order)'),
 ('engine/src/view/area.cpp', 'trans.push_back({-d, i});', 'trans.push_back({d, i});', 'trans sorted near-to-far'),
 ('engine/src/view/area.cpp', "if (m.subsets[s].alpha == (pass == 1)) items.push_back({o, s});", "if (m.subsets[s].alpha == (pass == 0)) items.push_back({o, s});", 'alpha subsets drawn first'),
 ('engine/src/view/area.cpp', 'if (pos[i].flags & kPosStartFlag) {', 'if (true) {', 'start = first POS record regardless of 0x1000'),
 ('engine/src/view/area.cpp', 'tex.alpha_on = ti.alpha_bits > 0;', 'tex.alpha_on = ti.bits == 32;', 'TGA alpha from bpp, not declared alpha bits'),
 ('engine/src/view/area.cpp', 'if (rt == sage::Route::Ignored) {', 'if (false) {', 'ignored records loaded'),
 ('engine/src/view/area.cpp', 'if (out_.placements.size() >= kMaxPlacements)', 'if (out_.placements.size() > kMaxPlacements)', 'placement cap off by one'),
 ('engine/src/view/math.cpp', 'r.m[10] = zf / (zn - zf);', 'r.m[10] = (zf + zn) / (zn - zf);', 'GL-style [-1,1] depth'),
 ('engine/src/view/math.cpp', 'r.m[6] = s;\n  r.m[9] = -s;', 'r.m[6] = -s;\n  r.m[9] = s;', 'RotX sign flipped (upside-down world)'),
 ('engine/src/view/mips.cpp', 'o[c] = static_cast<uint8_t>((acc[c] + total / 2) / total);', 'o[c] = static_cast<uint8_t>(acc[c] / total);', 'mip rounding truncates'),
 ('engine/src/view/mips.cpp', 'const uint32_t dw = width > 1 ? width / 2 : 1, dh = height > 1 ? height / 2 : 1;', 'const uint32_t dw = width > 1 ? width / 2 : 1, dh = height / 2 ? height / 2 : 1;', 'no-op control (must SURVIVE)'),
 ('engine/src/view/mips.cpp', 'for (uint64_t i = lo / d; i < s && i * d < hi; ++i) {', 'for (uint64_t i = lo / d + (s % 2); i < s && i * d < hi; ++i) {', 'odd-size first tap dropped'),
 ('engine/src/rhi/registry.h', 'if ((uint64_t(d.first_index) + d.index_count) * 4 > ib->second.bytes) return "index range outside the index buffer";', '', 'draw index range unchecked'),
 ('engine/src/rhi/registry.h', 'if (bytes != uint64_t(mip_dim(it->second.width, mip)) * mip_dim(it->second.height, mip) * 4) return "byte count does not match the mip size";', '', 'write_texture size unchecked'),
 ('engine/src/rhi/null_device.cpp', 'rec("write_buffer %u +%u %u bytes sum=%08x", b, off, size, detail::checksum(data, size));', 'rec("write_buffer %u +%u %u bytes", b, off, size);', 'null log drops buffer content checksum'),
 ('engine/src/rhi/webgpu_device.cpp', 'blend.color = {WGPUBlendOperation_Add, WGPUBlendFactor_SrcAlpha, WGPUBlendFactor_OneMinusSrcAlpha};', 'blend.color = {WGPUBlendOperation_Add, WGPUBlendFactor_One, WGPUBlendFactor_One};', 'WebGPU alpha blend additive'),
 # revision 2 (BindGroupDesc, stride, normals, rejected counter, M0 draw validation, CPU release)
 ('engine/src/rhi/registry.h', 'if (pi == pipelines.end() || pi->second.mesh) return "unknown M0 pipeline";', 'if (pi == pipelines.end()) return "unknown M0 pipeline";', 'M0 draw() accepts a mesh pipeline'),
 ('engine/src/rhi/registry.h', 'if (bg->second.stride != uniform_stride(p->second.uniform_bytes) || bg->second.slots != p->second.slots)', 'if (bg->second.slots != p->second.slots)', 'bind group stride vs pipeline not checked'),
 ('engine/src/rhi/registry.h', 'if (d.uniform_bytes == 0 || d.uniform_bytes > kMaxUniformBytes) return "uniform_bytes out of range";', 'if (d.uniform_bytes == 0 || d.uniform_bytes > kMaxUniformBytes + 1) return "uniform_bytes out of range";', 'uniform cap off by one'),
 ('engine/src/rhi/null_device.cpp', '    stats_.rejected = 0;\n', '', 'rejected counter not reset per frame'),
 ('engine/src/rhi/webgpu_device.cpp', '      x.size = stride;  // the whole slot: offset + stride is validated against the buffer', '      x.size = d.uniform_bytes;', 'WebGPU binds uniform_bytes, not the slot stride'),
 ('engine/src/rhi/webgpu_device.cpp', '    if (!why.empty()) return reject("draw", why);  // unknown id or a mesh pipeline: never reaches Dawn, never throws', '', 'WebGPU M0 draw() not validated'),
 ('engine/src/view/area.cpp', 'for (int k = 0; k < 3; ++k) o[3 + k] = has_normals ? x.normals[v * 3 + k] : 0.0f;', 'for (int k = 0; k < 3; ++k) o[3 + k] = 0.0f;', 'normals not written'),
 ('engine/src/view/area.cpp', '    std::vector<MipLevel>().swap(t.mips);\n', '', 'release keeps texture mips'),
]
def run(cmd):
    return subprocess.run(cmd, cwd=W, shell=True, capture_output=True, text=True)
results = []
for path, old, new, what in M:
    p = os.path.join(W, path); src = open(p).read()
    assert src.count(old) == 1, (path, what)
    open(p, 'w').write(src.replace(old, new))
    try:
        b = run('timeout 900 cmake --build build/native')
        if b.returncode != 0:
            results.append((what, 'BUILD-FAIL')); continue
        t = run('timeout 600 ctest --test-dir build/native -R "unit|a7_|gpu_mesh" --timeout 300')
        # a failing test shows '***Failed' or, when it aborts (e.g. an uncaught throw), '***Exception'
        failed = [l.split(':')[1].split()[0] for l in t.stdout.splitlines() if 'Test' in l and '#' in l and ('***' in l or 'Failed  ' in l)]
        results.append((what, 'CAUGHT ' + ','.join(failed) if t.returncode != 0 else 'MISSED'))
    finally:
        open(p, 'w').write(src)
    print(results[-1], flush=True)
b = run('timeout 900 cmake --build build/native'); t = run('timeout 600 ctest --test-dir build/native --timeout 300')
print('restored build rc', b.returncode, 'ctest rc', t.returncode)
