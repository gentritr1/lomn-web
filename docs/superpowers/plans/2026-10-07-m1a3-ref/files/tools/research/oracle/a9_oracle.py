"""A9 oracle (M1a-3): the D3D8 call sequence of one area frame, written in Python straight from the decompiled
functions (gen/decomp/funcs, never the C++ shim), from independent parsers: tools/research/blk.py (blockfiles),
xbin.py (.x), our own SLB/DDS/TGA header reads below and osi.py (Base.osi literals).

  a9_oracle.py --install DIR --level lev1 --area vllg [--eye-height 3] --out expected.txt
  a9_oracle.py --install DIR --level lev1 --area vllg --compare state_log.txt     (exit 1 on a difference)

Covered lines (the C++ log is filtered the same way, init + the FIRST frame): RS, TSS, TEXTURE, DRAW, LIGHTENABLE,
CLEAR, BEGINSCENE/ENDSCENE, "# node", "# SrPushRenderMode", "MATERIAL" (values), "XFORM <type>" (the matrix checksum
is dropped) and "LIGHT <i> type=<t>" (values dropped: float32 products are not recomputed here).
Rules and addresses: see lomn/view/sage_frame.h; every function below names its source.
"""
import argparse, math, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import blk  # noqa: E402
import xbin  # noqa: E402
import osi  # noqa: E402

# ---- D3D8 numbering ----
RS_NAMES = {7: 'ZENABLE', 8: 'FILLMODE', 9: 'SHADEMODE', 14: 'ZWRITEENABLE', 15: 'ALPHATESTENABLE', 19: 'SRCBLEND',
            20: 'DESTBLEND', 22: 'CULLMODE', 23: 'ZFUNC', 24: 'ALPHAREF', 25: 'ALPHAFUNC', 26: 'DITHERENABLE',
            27: 'ALPHABLENDENABLE', 28: 'FOGENABLE', 29: 'SPECULARENABLE', 34: 'FOGCOLOR', 35: 'FOGTABLEMODE',
            36: 'FOGSTART', 37: 'FOGEND', 38: 'FOGDENSITY', 47: 'ZBIAS', 48: 'RANGEFOGENABLE', 60: 'TEXTUREFACTOR',
            137: 'LIGHTING', 139: 'AMBIENT', 140: 'FOGVERTEXMODE', 141: 'COLORVERTEX', 153: 'SOFTWAREVERTEXPROCESSING'}
TSS_NAMES = {1: 'COLOROP', 2: 'COLORARG1', 3: 'COLORARG2', 4: 'ALPHAOP', 5: 'ALPHAARG1', 6: 'ALPHAARG2',
             11: 'TEXCOORDINDEX', 16: 'MAGFILTER', 17: 'MINFILTER', 18: 'MIPFILTER', 21: 'MAXANISOTROPY',
             24: 'TEXTURETRANSFORMFLAGS'}
HEXRS = {34, 60, 139}
FLOATRS = {36, 37, 38}


def f2b(f):
    return struct.unpack('<I', struct.pack('<f', f))[0]


def b2f(b):
    return struct.unpack('<f', struct.pack('<I', b))[0]


class Device:
    """Device state with the D3D8 defaults (DirectX 8.1 SDK) for the slots the area path touches."""

    def __init__(self):
        self.rs = {7: 1, 8: 3, 9: 2, 14: 1, 15: 0, 19: 2, 20: 1, 22: 3, 23: 4, 24: 0, 25: 8, 26: 0, 27: 0, 28: 0, 29: 0,
                   34: 0, 35: 0, 36: f2b(0.0), 37: f2b(1.0), 38: f2b(1.0), 47: 0, 48: 0, 60: 0xffffffff, 137: 1, 139: 0,
                   140: 0, 141: 1, 153: 0}
        self.tss = {0: {1: 4, 2: 2, 3: 1, 4: 2, 5: 2, 6: 1, 11: 0, 16: 1, 17: 1, 18: 0, 21: 1, 24: 0},
                    1: {1: 1, 2: 2, 3: 1, 4: 1, 5: 2, 6: 1, 11: 1, 16: 1, 17: 1, 18: 0, 21: 1, 24: 0}}
        self.out = []

    def set_rs(self, r, v):
        self.rs[r] = v
        if r in FLOATRS:
            self.out.append('RS %s 0x%08x (%s)' % (RS_NAMES[r], v, '%.6g' % b2f(v)))
        elif r in HEXRS:
            self.out.append('RS %s 0x%08x' % (RS_NAMES[r], v))
        else:
            self.out.append('RS %s %d' % (RS_NAMES[r], v))

    def dx_rs(self, r, v):  # ScDxStates::SetRenderState 0x0064b1e0
        if self.rs.get(r) != v:
            self.set_rs(r, v)

    def set_tss(self, st, t, v):
        self.tss[st][t] = v
        self.out.append('TSS%d %s %d' % (st, TSS_NAMES[t], v))

    def dx_tss(self, st, t, v):  # ScDxStates::SetTextureStageState 0x0064b250
        if self.tss[st].get(t) != v:
            self.set_tss(st, t, v)

    def raw(self, line):
        self.out.append(line)


# ---- data ----
class Install:
    def __init__(self, root, level, area):
        self.root = root
        self.level, self.area = level[:4], area[:4]
        lv = os.path.join(root, 'data', 'levels', self.level)
        self.blks = []  # load order: current (area), second (level), Art
        for p in (os.path.join(lv, self.area + '.blk'), os.path.join(lv, self.level + '.blk'),
                  os.path.join(root, 'data', 'Art', 'Art.blk')):
            q = self.ci(p)
            self.blks.append((q, {e[0].lower(): e for e in blk.read(q)[2]} if q else {}))

    @staticmethod
    def ci(path):  # case-insensitive path resolution
        parts = path.split(os.sep)
        cur = os.sep if path.startswith(os.sep) else ''
        for part in [x for x in parts if x]:
            if not os.path.isdir(cur or '.'):
                return None
            names = {n.lower(): n for n in os.listdir(cur or '.')}
            if part.lower() not in names:
                return None
            cur = os.path.join(cur, names[part.lower()])
        return cur

    def entry(self, name, order=(0, 1, 2)):
        name = name.lower()
        for i in order:
            p, ents = self.blks[i]
            if name in ents:
                n, bs, fs, fl, off = ents[name]
                with open(p, 'rb') as f:
                    f.seek(off)
                    return f.read(fs)
        return None


def slb_table(b, count_off, ptr_off):
    """{u32 n @count_off, pointer field @ptr_off} -> (n, records offset) (data-relative pointers, unrelocated)."""
    return struct.unpack_from('<I', b, count_off)[0], struct.unpack_from('<I', b, ptr_off)[0]


def route(flags):
    if flags & 0x10:
        return 'moveable' if flags & 0x20 else ('trans' if flags & 0x100 else 'main')
    if flags & 0x20:
        return 'nolight'
    if flags & 0x08:
        return 'back'
    return None


def fourcc(v):
    return bytes([(v >> s) & 255 for s in (24, 16, 8, 0)]).rstrip(b'\0').decode('latin1')


def texture_alpha(b, tga):
    if b is None:
        return False
    if tga:
        return len(b) > 17 and b[16] == 32 and (b[17] & 15) > 0
    if len(b) < 128:
        return False
    flags, cc, bits, amask = struct.unpack_from('<II', b, 80) + struct.unpack_from('<I', b, 88) + struct.unpack_from('<I', b, 104)
    if flags & 4:
        return cc == 0x33545844  # DXT3
    return bits == 32 and amask != 0  # A8R8G8B8


def load_area(ins, eye_height):
    lv = ins.level
    objb = ins.entry(ins.area + '_obj.slb')
    recs = []
    if objb:
        n, base = slb_table(objb, 8, 12)
        for i in range(n):
            r = base + 60 * i
            rid = struct.unpack_from('<I', objb, r)[0]
            pos = struct.unpack_from('<3f', objb, r + 4)
            flags = struct.unpack_from('<I', objb, r + 0x38)[0]
            recs.append((i, fourcc(rid), pos, flags))
    posb = ins.entry(ins.area + '_pos.slb')
    start = None
    if posb:
        n, base = slb_table(posb, 4, 8)
        for i in range(n):
            r = base + 20 * i
            if struct.unpack_from('<I', posb, r + 16)[0] & 0x1000:
                start = struct.unpack_from('<3f', posb, r + 4)
                break
    eye = (start[0], start[1] + eye_height, start[2]) if start else (0.0, 0.0, 0.0)
    spec = []
    tb = ins.entry('textures.slb')
    if tb:
        n, base = slb_table(tb, 4, 8)
        for i in range(n):
            r = base + 20 * i
            np = struct.unpack_from('<I', tb, r)[0] + 1
            name = tb[np:tb.index(b'\0', np)].decode('latin1')
            tr, u, v = struct.unpack_from('<3f', tb, r + 4)
            spec.append((name, tr, u, v))
    models, places = {}, []
    for i, mid, pos, flags in recs:
        rt = route(flags)
        if rt is None:
            continue
        if mid.lower() not in models:
            xb = ins.entry(mid + '.x')
            models[mid.lower()] = load_model(ins, xb, spec) if xb else None
        m = models[mid.lower()]
        if m is None or not all(math.isfinite(c) for c in pos):
            continue
        places.append({'rec': i, 'id': mid, 'pos': pos, 'route': rt, 'm': m})
    return places, eye


def load_model(ins, xb, spec):
    meshes, _ = xbin.load(xb)
    if not meshes:
        return None
    m = meshes[-1]  # LoadFrames: the last Mesh wins
    used = sorted(set(m.face_material)) if m.face_material else [0]
    subs = []
    for a in used:
        mat = m.materials[a] if a < len(m.materials) else None
        sub = {'attrib': a, 'mat': mat, 'textured': bool(mat and mat[4]), 'alpha': False, 'tex': None, 'trans': 1.0,
               'uv': (0.0, 0.0)}
        if mat and mat[4]:
            name = mat[4]
            path = 'Root/Data/Levels/%s/textures/%s' % (ins.level, name)
            base = name.replace('\\', '/').split('/')[-1]
            found = None
            for ext in (('',) if '.' in base else ('.dds', '.tga')):
                if ins.entry(base + ext, order=(2, 0, 1)) is not None:  # SrBlockFileExists: Art first
                    found = base + ext
                    break
            if found:
                sub['tex'] = found
                sub['alpha'] = texture_alpha(ins.entry(found), found.lower().endswith('.tga'))
            if '_glass' in path:
                sub['trans'] = 0.0
            for nm, tr, u, v in spec:  # GcSpecTextureLoader::LoadTextures + ScDrawableModel::SetSpecialTexture
                if (tr, u, v) == (1.0, 0.0, 0.0) or nm != name:
                    continue
                if tr < 1.0:
                    sub['trans'] = tr
                if u != 0.0 or v != 0.0:
                    sub['uv'] = (u, v)
        subs.append(sub)
    # GetGreatestZ 0x00554c20: last vertex with the greatest model-space y (vertex order: the file's; R5 splits only
    # append copies of existing positions).
    gy = m.positions[0]
    for p in m.positions:
        if gy[1] <= p[1]:
            gy = p
    return {'subs': subs, 'gy': gy}


def material_line(mat, path):
    rgba, power, spec, emis, tex = mat
    d, e = list(rgba), list(emis) + [1.0]
    if '_glass' in path:
        d = [0.0, 0.0, 0.0, 0.0]
    if 'eye' in path:
        table = [('on_', [0, 1, 0, 1]), ('ga_', [1, 0.86666667, 0.0039215689, 1]), ('po_', [1, 0.50196081, 0.0039215689, 1]),
                 ('ko_', [0.28235295, 0.65882355, 1, 1]), ('le_', [0.66274512, 1, 0.0039215689, 1]),
                 ('ta_', [1, 0.0039215689, 0.14509805, 1])]
        for k, val in table:
            if k in path:
                e = val
                break
        else:
            if 'wh_' not in path:
                e = [0, 0, 1, 1]
        d = [0, 0, 0, 1]
    g = lambda v: '(%s)' % ','.join('%.6g' % struct.unpack('<f', struct.pack('<f', x))[0] for x in v)
    return 'MATERIAL d=%s a=(0.7,0.7,0.7,1) s=%s e=%s p=%s' % (g(d), g(list(spec) + [1.0]), g(e), '%.6g' % power)


class Frame:
    def __init__(self, dev, level):
        self.d, self.last, self.fog_on, self.ctx, self.level = dev, 0, False, False, level
        self.fog = (0xffffffff, 4701.0, 4702.0, 0.001)

    def push(self, m):  # SrPushRenderMode 0x00531100
        d = self.d
        d.raw('# SrPushRenderMode 0x%05x' % m)
        if not m & 0x80000:
            if self.last & 0x80000:
                d.dx_rs(27, 0); d.dx_rs(24, 1); d.dx_rs(15, 0); d.dx_rs(14, 1)
        elif not self.last & 0x80000:
            d.dx_tss(0, 4, 2); d.dx_tss(0, 5, 2); d.dx_rs(27, 1); d.dx_rs(19, 5); d.dx_rs(20, 6); d.dx_rs(24, 0)
            d.dx_rs(15, 1); d.dx_rs(14, 0); d.dx_tss(0, 1, 4); d.dx_tss(0, 2, 2)
        if not m & 0x40000:
            if self.last & 0x40000:
                d.dx_tss(1, 1, 1); d.dx_tss(1, 4, 1)
        elif not self.last & 0x40000:
            d.dx_tss(1, 2, 2); d.dx_tss(1, 1, 5); d.dx_tss(1, 3, 1); d.dx_tss(1, 4, 1)
        if m & 0x10000:  # CanUseW() true
            d.dx_rs(7, 2)
        elif m & 0x8000:
            d.dx_rs(7, 0)
        if m & 0x2000:
            d.dx_rs(8, 2)
        elif m & 0x4000:
            d.dx_rs(8, 3)
        d.dx_rs(137, 1 if m & 1 else 0)
        d.dx_rs(22, 2 if m & 0x400 else 3 if m & 0x800 else 1)
        fog_rs = d.rs[28]
        if not m & 0x200:
            if fog_rs:
                self.set_fog(False)
        elif not fog_rs:
            self.set_fog(True)
        self.last = m

    def set_fog(self, on):  # GcGraphicsOptions::SetFog 0x005e5090 (table fog caps)
        if self.fog_on == on:
            return
        self.fog_on = on
        d = self.d
        if not on:
            d.dx_rs(28, 0)
            return
        d.dx_rs(28, 1); d.dx_rs(140, 0); d.dx_rs(35, 3); d.dx_rs(48, 0); d.dx_rs(34, self.fog[0])
        d.dx_rs(36, f2b(self.fog[1])); d.dx_rs(37, f2b(self.fog[2])); d.dx_rs(38, f2b(self.fog[3]))

    def set_texture_mat(self, sub, transparency):  # ScDrawableMaterial::SetTexture 0x0054f260
        d = self.d
        if sub['uv'] == (0.0, 0.0):
            d.dx_tss(0, 24, 0)
        else:
            d.dx_tss(0, 24, 2)
            d.raw('XFORM TEXTURE0')
        if not sub['textured']:
            return
        if transparency == 1.0 and sub['trans'] < 1.0:
            d.dx_rs(27, 1); d.dx_tss(0, 4, 3); d.dx_tss(0, 6, 3)
            d.dx_rs(60, (round(255 * struct.unpack('<f', struct.pack('<f', sub['trans']))[0]) & 255) << 24)
        d.raw('TEXTURE 0 %s' % (sub['tex'] or 'NULL'))
        d.raw(material_line(sub['mat'], 'Root/Data/Levels/%s/textures/%s' % (self.level, sub['mat'][4])))

    def draw_x(self, pl, transparency=1.0):  # ScDrawableModel::DrawX 0x005528c0
        d, subs = self.d, pl['m']['subs']
        d.raw('XFORM WORLD')
        d.dx_rs(153, 0)
        if transparency < 1.0:
            d.dx_rs(47, 16); d.dx_rs(27, 1); d.dx_tss(0, 4, 4); d.dx_tss(0, 5, 3); d.dx_tss(0, 6, 2)
            d.dx_rs(60, (round(255 * transparency) & 255) << 24)
        else:
            d.dx_rs(47, 0)
        tag = '%s#%d' % (pl['id'], pl['rec'])
        nxt, any_alpha = 0, False
        for s in subs:
            if s['alpha']:
                nxt, any_alpha = s['attrib'] + 1, True
                continue
            if transparency >= 1.0:
                d.dx_rs(27, 0); d.dx_tss(0, 4, 1)
            self.set_texture_mat(s, transparency)
            if nxt <= s['attrib']:
                d.raw('DRAW %s attrib=%d' % (tag, s['attrib']))
            nxt = max(nxt, s['attrib'] + 1)
        if any_alpha:
            d.dx_rs(27, 1)
            if transparency >= 1.0:
                d.dx_tss(0, 4, 3); d.dx_tss(0, 6, 2)
            d.dx_rs(47, 0); d.dx_rs(15, 1); d.dx_rs(25, 5)
            if self.ctx:
                d.dx_rs(27, 0); d.dx_rs(25, 5)
            nxt = 0
            for s in subs:
                if not s['alpha']:
                    nxt = s['attrib'] + 1
                    continue
                self.set_texture_mat(s, transparency)
                if nxt <= s['attrib']:
                    d.raw('DRAW %s attrib=%d' % (tag, s['attrib']))
                nxt = max(nxt, s['attrib'] + 1)
            d.dx_rs(47, 16); d.dx_rs(15, 0)
        d.dx_rs(27, 0); d.dx_tss(0, 6, 2)
        d.raw('TEXTURE 0 NULL'); d.raw('TEXTURE 1 NULL')

    def area(self, pl):  # GcArea::Render 0x00505840
        d = self.d
        d.set_tss(0, 16, 2)
        plant = pl['id'] in ('plnt', 'tre1')
        self.ctx = plant
        if plant:
            self.set_fog(True)
        self.draw_x(pl)
        if plant:
            self.set_fog(False)
            self.ctx = False


def lights(ins, level_type):
    """GcLightLoader::LoadLights 0x00470010 / GrSetDefaultLighting 0x00470560: the light types, in add order."""
    def default():
        return [3] if level_type < 3 else [3, 3]
    b = ins.entry(ins.area + '_light.slb')
    if not b:
        return default()
    n, base = slb_table(b, 4, 8)
    out, directional, shad = [], False, False
    for i in range(n):
        flags = struct.unpack_from('<I', b, base + 44 * i + 0x28)[0]
        if flags & 0x40:
            out.append(1)
        elif flags & 0x80:
            directional = True
            out.append(3)
            if not shad:
                shad = True
                out.append(3)
    return out + ([] if directional else default())


def expected(root, level, area, eye_height):
    ins = Install(root, level, area)
    places, eye = load_area(ins, eye_height)
    level_type = (ord(ins.level[3]) - 0x30) & 0xff if len(ins.level) > 3 else 0xd0
    d = Device()
    f = Frame(d, ins.level)
    # fog literals of the area script (GcAreaDirector::SetFog negates start/end)
    ob = Install.ci(os.path.join(root, 'data', 'Base.osi'))
    if ob:
        data = open(ob, 'rb').read()
        o = osi.parse(data)
        cls = 'lego_area_' + ins.area + ('' if level_type == 1 else ins.level[3])
        calls = osi.eval_calls(data, o, cls, 'initfull') or []
        for k, args in calls:
            if k == 'gcareadirector.setfog' and len(args) == 4 and all(isinstance(x, (int, float)) for x in args):
                c = args[0] & 0xffffffff
                f.fog = (c, -args[1], -args[2], args[3])
                break
    # init: CreateDirectXWindow 0x0052e570, AppMain 0x005381f0, SetUpDatabase ambient, loading HUD
    d.set_rs(7, 2); d.set_rs(23, 4); d.set_rs(19, 5); d.set_rs(20, 6); d.set_rs(26, 0)
    d.set_tss(0, 1, 4); d.set_tss(0, 2, 2); d.set_tss(0, 3, 0); d.set_tss(0, 6, 2)
    d.set_tss(1, 11, 0); d.set_tss(1, 2, 2); d.set_tss(1, 3, 1)
    d.set_tss(0, 18, 2); d.set_rs(140, 0); d.set_rs(35, 3); d.set_rs(48, 0); d.set_rs(137, 1); d.set_rs(139, 0xff000000)
    d.set_rs(9, 2); d.set_tss(0, 16, 3); d.set_tss(0, 17, 3)
    d.set_rs(139, 0xff1a1a26)
    f.push(0x8c034)
    # frame 0
    d.raw('CLEAR full flags=3 color=0xff000000 z=0')
    d.raw('BEGINSCENE')
    f.push(0x10635)
    d.raw('CLEAR viewport flags=3 color=0x00000000 z=1')
    d.raw('XFORM PROJECTION'); d.raw('XFORM VIEW'); d.raw('XFORM PROJECTION')
    d.raw('# node MovingLight')
    count = 0
    for t in lights(ins, level_type):
        if count == 8:
            count = 0
        d.raw('LIGHT %d type=%d' % (count, t)); d.raw('LIGHTENABLE %d 1' % count)
        count += 1
    d.raw('# node MainArea'); f.push(0x14634)
    for p in places:
        if p['route'] == 'main':
            f.area(p)
    mv = sorted((p for p in places if p['route'] == 'moveable'),
                key=lambda p: math.dist(p['pos'], eye))  # stable: ties keep record order
    for p in mv:
        f.draw_x(p)
    d.raw('# node NoLight'); f.push(0x14634)
    for p in places:
        if p['route'] == 'nolight':
            f.draw_x(p)
    d.raw('# node StaticLight')
    d.raw('# node Shadow'); f.push(0x4400)
    d.raw('# node Main'); f.push(0x10635)
    d.raw('# node Back'); f.push(0x10234)
    for p in places:
        if p['route'] == 'back':
            f.area(p)
    d.raw('# node Trans'); f.push(0x10034)
    rem = [p for p in places if p['route'] == 'trans']
    while rem:  # GcTransDirector::SortIt 0x00485bf0
        best, bd = None, 0.0
        for k, p in enumerate(rem):
            iv = struct.unpack('>I', p['id'].encode('latin1').ljust(4, b'\0'))[0]
            if iv == 0x31796b73:
                best = k
                break
            if iv == 0x72746177 or (iv & 255) == ord('w'):
                if len(rem) == 1:
                    best = k
                    break
                continue
            gx, gz = p['m']['gy'][0] + p['pos'][0], p['m']['gy'][2] + p['pos'][2]
            dist = math.hypot(gx - eye[0], gz - eye[2])
            if bd < dist:
                best, bd = k, dist
        if best is None:
            best = 0
        f.area(rem.pop(best))
    for i in range(8):
        d.raw('LIGHTENABLE %d 0' % i)
    f.push(0x8c034)
    for i in range(8):
        d.raw('LIGHTENABLE %d 0' % i)
    d.raw('ENDSCENE')
    return d.out


def filtered(lines):
    out = []
    for l in lines:
        if l.startswith('# node ') or l.startswith('# SrPushRenderMode'):
            out.append(l)
        elif l.startswith('#'):
            continue
        elif l.startswith('XFORM '):
            out.append(l.split(' sum=')[0])
        elif l.startswith('LIGHT '):
            out.append(' '.join(l.split()[:3]))
        else:
            out.append(l)
        if l == 'ENDSCENE':
            break
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--install', required=True)
    ap.add_argument('--level', required=True)
    ap.add_argument('--area', required=True)
    ap.add_argument('--eye-height', type=float, default=3.0)
    ap.add_argument('--out')
    ap.add_argument('--compare')
    a = ap.parse_args()
    exp = filtered(expected(a.install, a.level, a.area, a.eye_height))
    if a.out:
        open(a.out, 'w').write('\n'.join(exp) + '\n')
    if a.compare:
        got = filtered(open(a.compare).read().splitlines())
        for i, (x, y) in enumerate(zip(exp, got)):
            if x != y:
                print('FIRST DIFFERENCE at line %d:\n  oracle: %s\n  shim:   %s' % (i + 1, x, y))
                return 1
        if len(exp) != len(got):
            print('LENGTH differs: oracle %d lines, shim %d lines' % (len(exp), len(got)))
            return 1
        print('A9 oracle == shim: %d lines (%s/%s)' % (len(exp), a.level, a.area))
    return 0


if __name__ == '__main__':
    sys.exit(main())
