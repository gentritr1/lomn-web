"""Builds the M1a-3 task chain T1..T7 (REF3 revision 2, item 2) from BASE ($W/base) and the final reference ($W/work).
For each task: tasks/Tn/files/<repo path> = the FULL content of every file the task adds or changes, as of the end of
the task (apply cumulatively: copy T1, T2, ... over a BASE checkout), and tasks/Tn.txt = the file list with what each
file carries in that task. The final stage must equal $W/work (engine/, tools/, Makefile) byte for byte.
Usage: build_tasks.py [W]"""
import difflib, filecmp, os, shutil, subprocess, sys

W = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
BASE, WORK, OUT = os.path.join(W, 'base'), os.path.join(W, 'work'), os.path.join(W, 'tasks')
TREES = ['engine', 'tools', 'Makefile']


def rd(root, p):
    f = os.path.join(root, p)
    return open(f).read() if os.path.exists(f) else None


def changes(p):
    a = (rd(BASE, p) or '').splitlines(keepends=True)
    b = rd(WORK, p).splitlines(keepends=True)
    return a, b, [o for o in difflib.SequenceMatcher(None, a, b, autojunk=False).get_opcodes() if o[0] != 'equal']


def select(p, sel):
    """BASE with the selected changes (1-based indices into changes(p)) applied."""
    a, b, ops = changes(p)
    out, pos = [], 0
    for i, (tag, i1, i2, j1, j2) in enumerate(ops, 1):
        out += a[pos:i1]
        out += b[j1:j2] if i in sel else a[i1:i2]
        pos = i2
    return ''.join(out + a[pos:])


def rep(s, old, new, count=1):
    assert s.count(old) == count, (old[:80], s.count(old))
    return s.replace(old, new)


def block(text, start, end):
    """The text of `text` from the line starting with `start` up to (not including) the line starting with `end`."""
    i0 = text.index(start)
    return text[i0:text.index(end, i0)]


class Stage:
    def __init__(self):
        self.files = {}  # path -> content (only paths that differ from BASE are tracked)

    def get(self, p):
        return self.files[p] if p in self.files else rd(BASE, p)


stage = Stage()
tasks = []


def task(name, title):
    tasks.append({'name': name, 'title': title, 'files': {}, 'content': {}})


def put(p, content, note):
    t = tasks[-1]
    assert content is not None, p
    if stage.get(p) == content:
        raise SystemExit('%s: %s unchanged in this task' % (t['name'], p))
    stage.files[p] = content
    t['files'][p] = note
    t['content'][p] = content


def final(p, note):
    put(p, rd(WORK, p), note)


FINAL_CM = rd(WORK, 'engine/CMakeLists.txt')
TESTS_LINE_BASE = '               tests/test_rhi_mesh.cpp tests/test_view_mips.cpp tests/test_view_area.cpp)\n'


def tests_line(extra):
    s = '               tests/test_rhi_mesh.cpp tests/test_view_mips.cpp tests/test_view_area.cpp'
    first = [x for x in extra if x in ('tests/test_d3d.cpp', 'tests/test_sage_osi.cpp')]
    second = [x for x in extra if x not in first]
    if first: s += ' ' + ' '.join(first)
    s += ('\n               ' + ' '.join(second)) if second else ''
    return s + ')\n'


def cm_tests(cm, extra):
    cur = cm[cm.index('               tests/test_rhi_mesh.cpp'):]
    cur = cur[:cur.index(')\n') + 2]
    return rep(cm, cur, tests_line(extra))


# ------------------------------------------------------------------------------------------------------------- T1
task('T1', 'rhi additive: FrontFace + depth_bias in MeshPipelineDesc; draws outside a frame rejected on null AND WebGPU')
for p in ['engine/include/lomn/rhi.h', 'engine/src/rhi/registry.h', 'engine/src/rhi/null_device.cpp', 'engine/src/rhi/webgpu_device.cpp']:
    final(p, 'full (final)')
final('engine/tests/test_rhi_mesh.cpp', 'full (final): null out-of-frame rejection')
final('engine/tests/test_gpu_mesh.cpp', 'full (final): winding / depth-bias / out-of-frame probes on Dawn')

# ------------------------------------------------------------------------------------------------------------- T2
task('T2', 'lomn_d3d: D3D8 state shim (device8) + compiler to rhi (ffp: pipeline keys, uber-shader WGSL, uniforms, CPU '
     'reference) + gpu_ffp + independent Python check of the reference (ffp_oracle)')
for p in ['engine/include/lomn/d3d/device8.h', 'engine/src/d3d/device8.cpp', 'engine/include/lomn/d3d/ffp.h',
          'engine/src/d3d/ffp.cpp']:
    final(p, 'new (final)')
for p in ['engine/tests/test_d3d.cpp', 'engine/tests/test_gpu_ffp.cpp', 'engine/tests/ffp_cases.h',
          'engine/tests/ffp_cases.cpp', 'tools/research/oracle/ffp_oracle.py']:
    final(p, 'new (final)')
cm = stage.get('engine/CMakeLists.txt')
d3d_blk = block(FINAL_CM, '# M1a-3 faithful states: the D3D8', '# M1a-2 first pixels + M1a-3')
cm = rep(cm, '''# M1a-2 first pixels: CPU mips, D3D-convention matrices, area loader + unlit renderer on rhi, the A7 synthetic scene.
add_library(lomn_area STATIC src/view/mips.cpp src/view/math.cpp src/view/area.cpp src/view/synthetic.cpp)
target_link_libraries(lomn_area PUBLIC lomn_sage)
''', d3d_blk + '''# M1a-2 first pixels: CPU mips, area loader + unlit renderer on rhi, the A7 synthetic scene.
add_library(lomn_area STATIC src/view/mips.cpp src/view/area.cpp src/view/synthetic.cpp)
target_link_libraries(lomn_area PUBLIC lomn_sage lomn_d3d)
''')
gpu_ffp = block(FINAL_CM, '    # M1a-3: the fixed-function uber-shader against', '    add_test(NAME a7_gpu_no_adapter')
cm = rep(cm, '    add_test(NAME a7_gpu_no_adapter', gpu_ffp + '    add_test(NAME a7_gpu_no_adapter')
ffp_or = block(FINAL_CM, '  # REF3 revision 2: the CPU reference of the D3D8', '  # A7 contract (no game data)')
cm = rep(cm, '  # A7 contract (no game data)', ffp_or + '  # A7 contract (no game data)')
cm = cm_tests(cm, ['tests/test_d3d.cpp'])
put('engine/CMakeLists.txt', cm, 'partial: lomn_d3d library (math.cpp moves into it; lomn_area links it), gpu_ffp, '
    'lomn_ffp_cases + ffp_oracle ctest, test_d3d.cpp in lomn_tests')

# ------------------------------------------------------------------------------------------------------------- T3
task('T3', 'lomn_sage: bounded OSI reader (caps measured on the real Base.osi) + textures.slb parser + Python osi.py / '
     'osi_maxima.py; crafted-input probes (osi_probe, native + wasm32)')
for p in ['engine/include/lomn/sage/osi.h', 'engine/src/sage/osi.cpp', 'engine/tests/osi_builder.h',
          'engine/tests/test_sage_osi.cpp', 'engine/tests/probe_osi.cpp', 'tools/research/osi.py',
          'tools/research/osi_maxima.py']:
    final(p, 'new (final)')
for p in ['engine/src/sage/bytes.h', 'engine/include/lomn/sage/slb.h', 'engine/src/sage/slb.cpp',
          'engine/tests/test_sage_slb.cpp']:
    final(p, 'full (final)')
cm = stage.get('engine/CMakeLists.txt')
cm = rep(cm, 'src/sage/xfile.cpp src/sage/image.cpp)\n', 'src/sage/xfile.cpp src/sage/image.cpp\n            src/sage/osi.cpp)\n')
cm = cm_tests(cm, ['tests/test_d3d.cpp', 'tests/test_sage_osi.cpp'])
probe = block(FINAL_CM, '# REF3 revision 2: crafted-input time/memory probes', '\n# Internal JSON string escaper')
cm = rep(cm, 'add_test(NAME unit COMMAND lomn_tests)\n', 'add_test(NAME unit COMMAND lomn_tests)\n' + probe)
put('engine/CMakeLists.txt', cm, 'partial: osi.cpp in lomn_sage, test_sage_osi.cpp in lomn_tests, lomn_osi_probe + osi_probe ctest')

# ------------------------------------------------------------------------------------------------------------- T4
task('T4', 'area loader additions (data only; the M1a-2 renderer still draws): per-subset D3DMATERIAL8, textures.slb, '
     'LIGHT.slb / default lights, Base.osi fog + cave lighting, level type, greatest-Y; synthetic A9 "stat" scene data')
put('engine/include/lomn/view/area.h', select('engine/include/lomn/view/area.h', {2, 4, 5, 6, 7, 8, 9, 10}),
    'partial: AreaData / SubsetDraw / Model / AreaStats M1a-3 fields, SceneLight, FogSettings (renderer API and '
    'camera defaults unchanged)')
fa, ba = rd(WORK, 'engine/src/view/area.cpp'), rd(BASE, 'engine/src/view/area.cpp')
anchor = '  uint64_t max_texture_bytes_ = kMaxAreaTextureBytes, max_geometry_bytes_ = kMaxAreaGeometryBytes;\n};\n'
t4 = fa[:fa.index(anchor) + len(anchor)] + ba[ba.index(anchor) + len(anchor):]
t4 = rep(t4, '      // MeshVertexColors reach the D3D vertex buffer as a D3DCOLOR (8 bits per channel): quantised like D3DXCOLOR -> DWORD.\n'
         '      for (int c = 0; c < 4; ++c) o[8 + c] = m.has_colors ? d3d::quantize_unorm8(x.colors[v * 4 + c]) : 1.0f;\n',
         '      for (int c = 0; c < 4; ++c) o[8 + c] = m.has_colors ? x.colors[v * 4 + c] : 1.0f;\n')
t4 = rep(t4, '#include <tuple>\n', '')
t4 = rep(t4, '#include "lomn/d3d/ffp.h"\n', '')
t4 = rep(t4, '#include "lomn/view/sage_frame.h"\n', '')
put('engine/src/view/area.cpp', t4, 'partial: the loader half of the final file (build_material, load_spec_textures / '
    'apply_spec_textures, load_lights, load_script, level type, greatest-Y, untextured-material warning); the M1a-2 '
    'renderer kept verbatim; vertex colours NOT yet quantised (that changes the A7 log: T5)')
final('engine/include/lomn/view/synthetic.h', 'full (final)')
final('engine/src/view/synthetic.cpp', 'full (final): stat area, LIGHT table, textures.slb, Base.osi, extra textures '
      '(the A7 "test" area is unchanged)')
final('engine/tests/test_area_states.cpp', 'new (final)')
put('engine/CMakeLists.txt', cm_tests(stage.get('engine/CMakeLists.txt'),
    ['tests/test_d3d.cpp', 'tests/test_sage_osi.cpp', 'tests/test_area_states.cpp']), 'partial: test_area_states.cpp in lomn_tests')

# ------------------------------------------------------------------------------------------------------------- T5
task('T5', 'SAGE frame replay (SageFrame) + AreaRenderer on the D3D8 shim: draw order complete (node order, DrawX '
     'passes, SortIt, moveables); vertex colours quantised; A7 regenerated, A9 state log + A9 Python oracle introduced; '
     'lomn_view --synthetic 2 / --state-log / --write-install / --lighting / --fog')
for p in ['engine/include/lomn/view/sage_frame.h', 'engine/src/view/sage_frame.cpp', 'engine/tests/test_sage_frame.cpp',
          'engine/tests/a9_state.cmake', 'engine/tests/data/a9_state.txt', 'tools/research/oracle/a9_oracle.py']:
    final(p, 'new (final)')
final('engine/src/view/area.cpp', 'full (final): renderer half (Impl, Sink, SageFrame replay) + quantised vertex colours')
put('engine/include/lomn/view/area.h', select('engine/include/lomn/view/area.h', {1, 2, 3, 4, 5, 6, 7, 8, 9, 10} | set(range(13, 23))),
    'partial: RenderOptions / RenderStats / AreaRenderer (camera defaults unchanged)')
for p in ['engine/tests/test_view_area.cpp', 'engine/tests/test_gpu_area.cpp']:
    final(p, 'full (final)')
final('engine/tests/data/a7_calls.txt', 'full (final): REGENERATED here (lazy pipelines, uber WGSL, 1024-byte slots, '
      'quantised colours, SortIt order); M1a-2 content until this task')
lv = select('engine/tools/lomn_view.cpp', {1, 2, 3, 4, 7, 15, 16, 17, 19, 20, 21, 24, 25, 26, 28})
lv = rep(lv, '//   --fov 72 --near 0.01 --far 4700 (the exe\'s camera; "exe" source) --eye x,y,z --yaw deg --pitch deg --eye-height 3\n',
         '//   --fov 60 --near 0.5 --far 4000 --eye x,y,z --yaw deg --pitch deg --eye-height 3   (camera fallback overrides)\n')
put('engine/tools/lomn_view.cpp', lv, 'partial: --synthetic 2, --state-log, --write-install, --lighting, --fog, '
    '--alpha-ref removed (RenderOptions lost it), metrics "states" block')
cm = stage.get('engine/CMakeLists.txt')
area_blk = block(FINAL_CM, '# M1a-2 first pixels + M1a-3', '\nif(EMSCRIPTEN)')
cm = rep(cm, block(cm, '# M1a-2 first pixels: CPU mips, area loader', '\nif(EMSCRIPTEN)'), area_blk)
w_a9 = block(FINAL_CM, '  # M1a-3 A9 under Node:', 'else()\n  add_executable(lomn_native')
cm = rep(cm, '-P ${CMAKE_CURRENT_SOURCE_DIR}/tests/a7_calls.cmake)\nelse()\n',
         '-P ${CMAKE_CURRENT_SOURCE_DIR}/tests/a7_calls.cmake)\n' + w_a9 + 'else()\n')
n_a9 = block(FINAL_CM, '  # M1a-3 A9 (no game data):', '  add_test(NAME view_cli_dangling_flag')
cm = rep(cm, '  add_test(NAME view_cli_dangling_flag', n_a9 + '  add_test(NAME view_cli_dangling_flag')
for key in ["for a in '--far inf' '--far nan'", 'o=\\"$1/out.txt\\"; rm -rf']:
    old = [l for l in cm.splitlines(keepends=True) if key in l]
    new = [l for l in FINAL_CM.splitlines(keepends=True) if key in l]
    assert len(old) == 1 and len(new) == 1, key
    cm = rep(cm, old[0], new[0])
cm = cm_tests(cm, ['tests/test_d3d.cpp', 'tests/test_sage_osi.cpp', 'tests/test_area_states.cpp', 'tests/test_sage_frame.cpp'])
put('engine/CMakeLists.txt', cm, 'partial: sage_frame.cpp in lomn_area, a9_state_log (native + Node), a9_oracle_synthetic, '
    'a9_oracle_real, CLI tests updated (--alpha-ref and --synthetic 3 rejected), test_sage_frame.cpp in lomn_tests')
mk = select('Makefile', {2})
put('Makefile', mk, 'partial: test-wasm also cmp\'s the native and wasm32 A9 state / call logs')

# ------------------------------------------------------------------------------------------------------------- T6
task('T6', 'lomn_view survey / metrics / exe camera defaults; wasm32 disk FS under Node (NODERAWFS) for lomn_view + '
     'lomn_datacheck; datacheck-wasm (native == wasm32 on real data); material floats in the A4 datacheck')
final('engine/tools/lomn_view.cpp', 'full (final): survey frame + scene columns, camera "exe" sources, untextured_materials, disk FS under Node')
final('engine/include/lomn/view/area.h', 'full (final): CameraSettings defaults fov 72 / near 0.01 / far 4700 (exe)')
for p in ['engine/include/lomn/fs.h', 'engine/src/fs/disk_fs.cpp', 'engine/tools/lomn_datacheck.cpp',
          'tools/research/oracle/check_datacheck.py']:
    final(p, 'full (final)')
final('engine/CMakeLists.txt', 'full (final): wasm32 lomn_view / lomn_datacheck with disk_fs + NODERAWFS, lomn_datacheck_node.sh')
mk = rd(WORK, 'Makefile')
mk = rep(mk, ' datacheck-wasm fuzz-osi\n', ' datacheck-wasm\n')
mk = rep(mk, block(mk, '# M1a-3 REF3 r2: ASan+UBSan mutation fuzz', 'check: test-tools'), '')
put('Makefile', mk, 'partial: datacheck-wasm target, check runs it')

# ------------------------------------------------------------------------------------------------------------- T7
task('T7', 'research tooling: `make fuzz-osi` (ASan+UBSan mutation fuzz of the OSI reader, native + wasm32), '
     'osi_craft.py (end-to-end hostile Base.osi probes)')
final('engine/tests/fuzz_osi.cpp', 'new (final)')
final('tools/research/osi_craft.py', 'new (final)')
final('Makefile', 'full (final): fuzz-osi target (on demand, not in check)')

# ------------------------------------------------------------------------------------------------------------- check + write
mismatch = []
for root, dirs, files in os.walk(WORK):
    rel = os.path.relpath(root, WORK)
    if rel != '.' and rel.split(os.sep)[0] not in TREES: continue
    dirs[:] = [d for d in dirs if d != '__pycache__' and (rel != '.' or d in TREES)]
    for f in files:
        if f.endswith('.pyc'): continue
        p = os.path.normpath(os.path.join(rel, f))
        if rel == '.' and p not in TREES: continue
        if stage.get(p) != rd(WORK, p): mismatch.append(p)
for p in stage.files:
    if rd(WORK, p) is None: mismatch.append(p + ' (not in work)')
if mismatch: raise SystemExit('final stage != work: %s' % mismatch)
shutil.rmtree(OUT, ignore_errors=True)
for t in tasks:
    for p in t['files']:
        snap = os.path.join(OUT, t['name'], 'files', p)
        os.makedirs(os.path.dirname(snap), exist_ok=True)
        open(snap, 'w').write(t['content'][p])
    with open(os.path.join(OUT, t['name'] + '.txt'), 'w') as f:
        f.write('%s: %s\nApply after %s: copy %s/files/* over the tree (full file contents as of the end of %s).\n\n' %
                (t['name'], t['title'], 'BASE f47cc91' if t['name'] == 'T1' else 'T%d' % (int(t['name'][1]) - 1), t['name'], t['name']))
        for p, note in t['files'].items():
            status = 'A' if rd(BASE, p) is None and all(p not in u['files'] for u in tasks[:tasks.index(t)]) else 'M'
            f.write('%s %s  -- %s\n' % (status, p, note))
print('tasks written to %s: %s' % (OUT, ', '.join('%s (%d files)' % (t['name'], len(t['files'])) for t in tasks)))
