import struct, sys, os, collections, statistics
sys.path.insert(0, os.path.dirname(__file__))
from xbin import load
root = sys.argv[1]
used_l, distinct_l, ratio = [], [], []; multi = []; area_used = area_distinct = 0
for dp, dn, fn in os.walk(root):
    for f in fn:
        if not f.lower().endswith('.blk') or 'levels' not in dp: continue
        d = open(os.path.join(dp, f), 'rb').read(); cnt = struct.unpack_from('<I', d, 8)[0]
        for i in range(cnt):
            o = 12 + i * 56; name = d[o:o+40].split(b'\0')[0].decode('latin1'); cs, us, fl, off = struct.unpack_from('<IIII', d, o + 40)
            if not name.lower().endswith('.x'): continue
            ms, _ = load(d[off:off+us])
            if len(ms) > 1: multi.append((os.path.relpath(os.path.join(dp, f), root), name, [len(m.faces) for m in ms]))
            m = ms[-1]                                  # the game keeps the LAST mesh (LoadFrames overwrites)
            used = set(m.face_material or [])
            keys = {(m.materials[a][4] or '', m.materials[a][0], m.materials[a][3]) for a in used}   # texture, diffuse rgba, emissive
            used_l.append(len(used)); distinct_l.append(len(keys))
print('level meshes', len(used_l))
print('used subsets per mesh: median', statistics.median(used_l), 'p95', sorted(used_l)[int(.95*len(used_l))], 'max', max(used_l), 'total', sum(used_l))
print('distinct (texture,diffuse,emissive) per mesh: median', statistics.median(distinct_l), 'p95', sorted(distinct_l)[int(.95*len(distinct_l))], 'max', max(distinct_l), 'total', sum(distinct_l))
print('multi-mesh files', multi)
