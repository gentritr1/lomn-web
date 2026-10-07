"""M1a-3 Task 8B D2: the area loader's data mutations (the Task 4 review found 11 that every test survived). Each mutation
is applied ALONE to engine/src/view/area.cpp of a SCRATCH COPY of the repo (never the working tree), lomn_tests is rebuilt
and its "area:*" cases run; every mutation must make at least one case fail. Finally all of them at once.
Usage: mutate_area.py SRC_DIR BUILD_DIR   (SRC_DIR: a copy, e.g. `git archive HEAD engine tools | tar -x -C DIR` + a
.toolchain symlink; BUILD_DIR: `cmake -S DIR/engine -B BUILD_DIR -G Ninja -DLOMN_WEBGPU=OFF`). Exit 1 if one survives."""
import os, re, subprocess, sys
src, build = sys.argv[1], sys.argv[2]
P = os.path.join(src, 'engine/src/view/area.cpp')
orig = open(P).read()
M = [
 ('M01 material ambient 0.7 -> 0.5', 'm.ambient = {0.7f, 0.7f, 0.7f, 1.0f};', 'm.ambient = {0.5f, 0.5f, 0.5f, 1.0f};'),
 ('M02 _glass keeps mTrans 1', 'd.trans = 0.0f;            // mTrans', 'd.trans = 1.0f;            // mTrans'),
 ('M03 _glass keeps the .x diffuse', 'm.diffuse = {0, 0, 0, 0};  // (@0x007365fc)', 'm.diffuse = {x.diffuse[0], x.diffuse[1], x.diffuse[2], x.diffuse[3]};'),
 ('M04 eye on_ emissive blue', 'if (has("on_")) m.emissive = {0, 1, 0, 1};', 'if (has("on_")) m.emissive = {0, 0, 1, 1};'),
 ('M05 textures.slb trans never applied', 'if (r.trans < 1.0f) sd.trans = r.trans;', 'if (r.trans < 0.0f) sd.trans = r.trans;'),
 ('M06 textures.slb u/v swapped', 'sd.uv_offset[0] = r.u;\n            sd.uv_offset[1] = r.v;', 'sd.uv_offset[0] = r.v;\n            sd.uv_offset[1] = r.u;'),
 ('M07 greatest_y first instead of last', 'p[1] >= m.greatest_y[1]', 'p[1] > m.greatest_y[1]'),
 ('M08 untextured_materials not counted', '++st.untextured_materials;', ''),
 ('M09 default LmbA only below type 2', 'if (type < 3) {  // "LmbA"', 'if (type < 2) {  // "LmbA"'),
 ('M10 type-3 sun colour on type 4', 'if (type == 3) sun = col(1.0f, 0.98039216f, 0.83529413f, 1);', 'if (type == 4) sun = col(1.0f, 0.98039216f, 0.83529413f, 1);'),
 ('M11 no type-4 white directional override', 'out_.level_type == 4 ? col(1, 1, 1, 1) : c, r.range', 'c, r.range'),
 ('M12 Shad for every directional', 'if (!shad) {  // one opposite', 'if (true) {  // one opposite'),
 ('M13 range <= 0 keeps attenuation0 0', 'l.attenuation0 = 1.0f;\n          l.attenuation1 = 0.0f;', 'l.attenuation0 = 0.0f;\n          l.attenuation1 = 0.0f;'),
 ('M14 style flag 4 -> 2', 'else if (r.flags & 4) style = 3;', 'else if (r.flags & 4) style = 2;'),
 ('M15 no light_slb+default path', 'if (!directional) {  // LoadLights calls', 'if (false) {  // LoadLights calls'),
]
env = dict(os.environ, PATH='/opt/homebrew/bin:' + os.environ['PATH'])
survived = 0
try:
    for name, a, b in M:
        assert orig.count(a) == 1, name
        open(P, 'w').write(orig.replace(a, b))
        r = subprocess.run(['cmake', '--build', build, '--target', 'lomn_tests'], capture_output=True, text=True, timeout=900, env=env)
        if r.returncode:
            print('%s: BUILD FAILED' % name); survived += 1; continue
        t = subprocess.run([os.path.join(build, 'lomn_tests'), '-tc=area:*'], capture_output=True, text=True, timeout=300)
        fails = sorted(set(re.findall(r'TEST CASE:\s+(.*)', t.stdout)))
        m = re.search(r'test cases:\s+(\d+) \|\s+(\d+) passed \|\s+(\d+) failed', t.stdout)
        print('%s: %s failed case(s) -> %s' % (name, m.group(3) if m else '?', '; '.join(f[:70] for f in fails) or 'SURVIVED'))
        if t.returncode == 0: survived += 1
    # all at once
    s = orig
    for name, a, b in M: s = s.replace(a, b)
    open(P, 'w').write(s)
    subprocess.run(['cmake', '--build', build, '--target', 'lomn_tests'], capture_output=True, timeout=900, env=env)
    t = subprocess.run([os.path.join(build, 'lomn_tests'), '-tc=area:*'], capture_output=True, text=True, timeout=300)
    m = re.search(r'test cases:.*', t.stdout)
    print('ALL %d at once: %s' % (len(M), m.group(0) if m else '?'))
finally:
    open(P, 'w').write(orig)
    subprocess.run(['cmake', '--build', build, '--target', 'lomn_tests'], capture_output=True, timeout=900, env=env)
print('mutations: %d, survived: %d' % (len(M), survived))
sys.exit(1 if survived else 0)
