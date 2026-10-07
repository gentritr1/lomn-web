import struct, sys, os, collections, json
sys.path.insert(0, os.path.dirname(__file__))
from xbin import load, walk, XError
root = sys.argv[1]
st = collections.Counter(); meshes_per = collections.Counter(); fails = []; types = collections.Counter(); polysz = collections.Counter()
tex_named = collections.Counter(); vtot = ftot = 0; nofile = 0
for dp, dn, fn in os.walk(root):
    for f in fn:
        if not f.lower().endswith('.blk'): continue
        path = os.path.join(dp, f); d = open(path, 'rb').read(); cnt = struct.unpack_from('<I', d, 8)[0]
        for i in range(cnt):
            o = 12 + i * 56; name = d[o:o+40].split(b'\0')[0].decode('latin1'); cs, us, fl, off = struct.unpack_from('<IIII', d, o + 40)
            if not name.lower().endswith('.x'): continue
            try:
                ms, top = load(d[off:off+us])
                st['ok'] += 1; meshes_per[len(ms)] += 1
                for ob in walk(top): types[ob.type] += 1
                for m in ms:
                    vtot += len(m.positions); ftot += len(m.faces)
                    for fc in m.faces: polysz[len(fc)] += 1
                    for mt in m.materials: tex_named['tex' if mt[4] else 'none'] += 1
                    st['uv' if m.uvs else 'no-uv'] += 1; st['normals' if m.normals else 'no-normals'] += 1
                    if m.uvs and len(m.uvs) != len(m.positions): st['uv-count-mismatch'] += 1
            except (XError, IndexError, struct.error) as e:
                st['fail'] += 1; fails.append((os.path.relpath(path, root), name, str(e)[:80]))
print(dict(st)); print('meshes per file', dict(meshes_per)); print('polygon sizes', dict(polysz))
print('vertices', vtot, 'faces', ftot); print('materials', dict(tex_named))
print('templates', types.most_common(25)); print('fails', fails[:10])
