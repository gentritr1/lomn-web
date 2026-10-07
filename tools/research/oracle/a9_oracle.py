"""A9 oracle (M1a-3): the D3D8 call sequence of one area frame, written in Python straight from the decompiled
functions (gen/decomp/funcs, never the C++ shim), from independent parsers: tools/research/blk.py (blockfiles),
xbin.py (.x), our own SLB/DDS/TGA header reads below and osi.py (Base.osi literals).

  a9_oracle.py --install DIR --level lev1 --area vllg [--eye-height 3] --out expected.txt
  a9_oracle.py --install DIR --level lev1 --area vllg --compare state_log.txt     (exit 1 on a difference)

  Camera inputs (lomn_view's run settings, not game data; defaults = lomn_view's for an install): [--fov 72 --near 0.01
  --far 4700] (the exe's camera: FOV @0x00704e00, near @0x00704e1c, far = GetDrawDistance mDrawDistance @0x0074a328,
  read through the GcViewPort ctor 0x004479f0) [--pitch -10] [--yaw DEG: set; default towards the bounds centre]
  [--width 1280 --height 720] (the aspect). The synthetic state scene (--synthetic 2) is rendered with
  --fov 90 --near 0.1 --far 100 --pitch 0 --yaw 0 --width 64 --height 64 --eye-height 0 (its CameraSettings).

Covered lines (the C++ log is filtered the same way, init + the FIRST frame): RS, TSS, TEXTURE, DRAW, LIGHTENABLE,
CLEAR, BEGINSCENE/ENDSCENE, "# node", "# SrPushRenderMode", "MATERIAL" (values), and since M1a-4 (b) the whole
"XFORM <type> sum=<checksum>" line (world, view, projection and texture matrices recomputed in float32 below) and the
whole "LIGHT <i> type=<t> d=.. s=.. a=.. pos=.. dir=.. range=.. att=.." line (the D3DLIGHT8 the game builds, from
LIGHT.slb, GrSetDefaultLighting and SetCaveLighting). Only "#" notes other than "# node" / "# SrPushRenderMode" are
dropped.
Rules and addresses: see lomn/view/sage_frame.h; every function below names its source.

Where this oracle is NOT independent of the shim (LIGHT lines). Both sides model the same choice, and the real install
never exercises it (EXECUTED scan of all 123 areas, M1a-4 final review), so a green sweep says nothing about it. A new
install with such data must be read against the decompilation (GcLightLoader::LoadLights 0x00470010), not against this
oracle:
  * a point light (flag 0x40) with no style flag gets style 0 here and in the shim; the exe keeps the previous record's
    style (uninitialised stack for the first record). 0 of 1,555 real LIGHT.slb records (M1a-4 T4 re-review N-2; the
    synthetic lmp1 and the test light pt0a pin the choice);
  * duplicate light ids within one area: both sides treat every record as its own light, and SetCaveLighting's intensity
    is applied to each; the exe resolves a light by id (Get(id)), so a duplicate id would apply SetIntensity twice to one
    object. 0 of 123 areas;
  * a record whose id is "Shad" would collide with the opposite fill light named "Shad" (added once, for the first 0x80
    record); ids are not modelled on either side. 0 of 123 areas.
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


def f32(x):
    """Round a Python float to float32 (one IEEE round to nearest even; a value that rounds past FLT_MAX is +-inf, as
    in IEEE arithmetic - struct refuses to pack it)."""
    try:
        return struct.unpack('<f', struct.pack('<f', x))[0]
    except OverflowError:
        return math.copysign(math.inf, x)


def tfactor_alpha(t):
    """(int)ROUND(255.0f * t) as the exe computes it (DrawX 0x005528c0 @_DAT_00737ca8, SetTexture 0x0054f260
    @_DAT_00736598): both operands are float32 and the product is rounded to float32 BEFORE the round to nearest even,
    because the device is created without D3DCREATE_FPU_PRESERVE (CreateDirectXWindow 0x0052e570), so Direct3D 8 leaves
    the x87 in single precision (Task 8A E2a; spec M1 section 7.1 R23, INFERRED). In float64 255 * f32(0.9) is
    229.49999... -> 229; in float32 it is 229.5 -> 230 (26 real areas)."""
    return round(f32(255.0 * f32(t))) & 255


# ---- float32 matrices (M1a-4 b) ----
# D3D / D3DX convention: row-major m[row*4 + col], row vectors (v' = v * M), translation in _41.._43 (m[12..14]). Every
# step is one float32 operation (f32 of the exact float64 result: correctly rounded for + - * / and sqrt, since float64
# has more than 2 * 24 + 2 bits); trig and sqrt are evaluated in float64 and rounded once, as lomn_view evaluates them
# (its camera is the viewer's, not a replay of the game's, so its operation order is part of the contract).
KDEG = math.pi / 180.0
FLT_MIN, FLT_MAX = 1.1754943508222875e-38, 3.4028234663852886e+38


def mat_identity():
    return [1.0 if i % 5 == 0 else 0.0 for i in range(16)]


def mat_mul(a, b):
    """a * b: r[i][j] = sum_k a[i][k] * b[k][j], accumulated left to right from +0 in float32."""
    r = [0.0] * 16
    for i in range(4):
        for j in range(4):
            acc = 0.0
            for k in range(4):
                acc = f32(acc + f32(a[i * 4 + k] * b[k * 4 + j]))
            r[i * 4 + j] = acc
    return r


def mat_rotation_x(rad):
    """D3DUtil_SetRotateXMatrix (DirectX 8.1 SDK d3dutil.cpp): _22 = cos, _23 = sin, _32 = -sin, _33 = cos."""
    c, s = f32(math.cos(rad)), f32(math.sin(rad))
    m = mat_identity()
    m[5], m[6], m[9], m[10] = c, s, -s, c
    return m


def mat_translation(x, y, z):
    m = mat_identity()
    m[12], m[13], m[14] = x, y, z
    return m


def perspective_fov_rh(fovy, aspect, zn, zf):
    """D3DXMatrixPerspectiveFovRH (DirectX 8.1 SDK documentation): yScale = cot(fovY / 2), xScale = yScale / aspect;
    [xScale 0 0 0; 0 yScale 0 0; 0 0 zf/(zn-zf) -1; 0 0 zn*zf/(zn-zf) 0]."""
    ys = f32(1.0 / math.tan(fovy / 2.0))
    m = [0.0] * 16
    m[0] = f32(ys / aspect)
    m[5] = ys
    m[10] = f32(zf / f32(zn - zf))
    m[11] = -1.0
    m[14] = f32(f32(zn * zf) / f32(zn - zf))
    return m


def _dot3(a, b):
    return f32(f32(f32(a[0] * b[0]) + f32(a[1] * b[1])) + f32(a[2] * b[2]))


def _cross3(a, b):
    return [f32(f32(a[1] * b[2]) - f32(a[2] * b[1])), f32(f32(a[2] * b[0]) - f32(a[0] * b[2])),
            f32(f32(a[0] * b[1]) - f32(a[1] * b[0]))]


def _normalize3(v):
    n = f32(math.sqrt(_dot3(v, v)))
    return [f32(x / n) for x in v] if n > 0.0 else list(v)


def look_at_rh(eye, at, up):
    """D3DXMatrixLookAtRH (DirectX 8.1 SDK documentation): zaxis = normal(Eye - At), xaxis = normal(cross(Up, zaxis)),
    yaxis = cross(zaxis, xaxis); [xaxis.x yaxis.x zaxis.x 0; xaxis.y yaxis.y zaxis.y 0; xaxis.z yaxis.z zaxis.z 0;
    -dot(xaxis, eye) -dot(yaxis, eye) -dot(zaxis, eye) 1]."""
    z = _normalize3([f32(eye[k] - at[k]) for k in range(3)])
    x = _normalize3(_cross3(up, z))
    y = _cross3(z, x)
    m = [0.0] * 16
    for i in range(3):
        m[i * 4 + 0], m[i * 4 + 1], m[i * 4 + 2] = x[i], y[i], z[i]
    m[12], m[13], m[14], m[15] = -_dot3(x, eye), -_dot3(y, eye), -_dot3(z, eye), 1.0
    return m


def transform_point(m, p):
    """(p, 1) * M: out[j] = ((p0 m[j] + p1 m[4+j]) + p2 m[8+j]) + m[12+j]."""
    return [f32(f32(f32(f32(p[0] * m[j]) + f32(p[1] * m[4 + j])) + f32(p[2] * m[8 + j])) + m[12 + j]) for j in range(4)]


def mat_sum(m):
    """The state log's matrix checksum: sum = sum * 31 + byte over the 64 little-endian float32 bytes, mod 2^32."""
    acc = 0
    for byte in struct.pack('<16f', *m):
        acc = (acc * 31 + byte) & 0xffffffff
    return acc


def g6(x):
    """A float as the state log prints it: %.6g, with NaN/inf spelled "nan" / "inf" / "-inf"."""
    if math.isnan(x):
        return 'nan'
    if math.isinf(x):
        return 'inf' if x > 0 else '-inf'
    return '%.6g' % x


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
        # Task 8B E4: file-derived text (texture names, model ids) is logged with every byte outside printable ASCII
        # as \\xHH, as the C++ device does; every constant part of a line is printable, so escaping the whole line is
        # the same rule.
        self.out.append(''.join(c if 0x20 <= ord(c) < 0x7f else '\\x%02x' % ord(c) for c in line))


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


def load_area(ins):
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
                p = struct.unpack_from('<3f', posb, r + 4)
                if not all(math.isfinite(c) for c in p):
                    continue  # M1a-4 (a): a non-finite start record is skipped (the viewer's rule, warned there)
                start = p
                break
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
        # World matrix (GcArea::GcArea 0x00504220): RotX(kWorldRotX = -1.5707982, the float the exe passes to
        # D3DUtil_SetRotateXMatrix) then the record position in _41.._43, i.e. RotX * Translation(pos).
        world = mat_mul(mat_rotation_x(f32(-1.5707982)), mat_translation(*pos))
        # The viewer's untrusted-input rule (M1a-4 I-2): a placement whose model-space AABB corners overflow float in
        # world space (finite values, e.g. a vertex at 3e38 placed at 3e38) is skipped, like a non-finite one.
        bmin, bmax = m['bmin'], m['bmax']
        corners = [transform_point(world, ((bmax[0] if k & 1 else bmin[0]), (bmax[1] if k & 2 else bmin[1]),
                                           (bmax[2] if k & 4 else bmin[2])))[:3] for k in range(8)]
        if not all(math.isfinite(c) for w in corners for c in w):
            continue
        places.append({'rec': i, 'id': mid, 'pos': pos, 'route': rt, 'm': m, 'world': world})
    return places, start


def camera(places, start, cam):
    """The viewer's camera (lomn_view's make_camera; not the game's camera, which M1 does not replay) -> (eye, view
    matrix, projection matrix). Eye: the POS start + eye height; without a POS start the centre of the world bounds of
    every placed model except Back placements, + eye height (Task 8A E2b: fren/frnt has no POS table); without any bounds
    the origin. The bounds are the 8 corners of each model's model-space AABB through its world matrix. Yaw: --yaw when
    given, else towards the bounds centre in xz (0 = -Z, 90 = +X; 0 when the centre is within 1e-3 of the eye or there
    are no bounds). at = eye + (sin(yaw) cos(pitch), sin(pitch), -cos(yaw) cos(pitch)); up = +Y."""
    lo = hi = None
    for p in places:
        if p['route'] == 'back':
            continue
        bmin, bmax = p['m']['bmin'], p['m']['bmax']
        for k in range(8):
            w = transform_point(p['world'], ((bmax[0] if k & 1 else bmin[0]), (bmax[1] if k & 2 else bmin[1]),
                                             (bmax[2] if k & 4 else bmin[2])))
            lo = w[:3] if lo is None else [w[j] if w[j] < lo[j] else lo[j] for j in range(3)]
            hi = w[:3] if hi is None else [w[j] if w[j] > hi[j] else hi[j] for j in range(3)]
    ctr = [f32(f32(a + b) * 0.5) for a, b in zip(lo, hi)] if lo is not None else [0.0, 0.0, 0.0]
    eh = f32(cam['eye_height'])
    if start:
        eye = [start[0], f32(start[1] + eh), start[2]]
    elif lo is not None:
        eye = [ctr[0], f32(ctr[1] + eh), ctr[2]]
    else:
        eye = [0.0, 0.0, 0.0]  # make_camera "origin": no eye height
    cli_yaw = None
    if cam['yaw'] is not None:
        y = math.fmod(f32(cam['yaw']), 360.0)  # lomn_view normalises --yaw to (-180, 180]
        y = y - 360.0 if y > 180.0 else y + 360.0 if y <= -180.0 else y
        yaw = cli_yaw = f32(y)
    else:
        dx, dz = f32(ctr[0] - eye[0]), f32(ctr[2] - eye[2])
        if lo is not None and f32(f32(dx * dx) + f32(dz * dz)) > f32(1e-6):
            yaw = f32(math.atan2(dx, -dz) / KDEG)
        else:
            yaw = 0.0

    def build(eye, yaw):
        yr, pr = yaw * KDEG, f32(cam['pitch']) * KDEG
        fwd = [f32(math.sin(yr) * math.cos(pr)), f32(math.sin(pr)), f32(-math.cos(yr) * math.cos(pr))]
        at = [f32(eye[k] + fwd[k]) for k in range(3)]
        view = look_at_rh(eye, at, [0.0, 1.0, 0.0])
        aspect = f32(f32(cam['width']) / f32(cam['height']))
        proj = perspective_fov_rh(f32(f32(cam['fov']) * KDEG), aspect, f32(cam['near']), f32(cam['far']))
        return at, view, proj
    at, view, proj = build(eye, yaw)
    # The viewer's overflow rule (M1a-4 I-2): a non-finite eye, target, view or projection, or a zero LookAtRH axis (a
    # huge eye absorbs the unit forward step in x/z: eye - at is vertical), falls back to the origin with yaw 0 (or the
    # --yaw given).
    zero_axis = any(view[k] == 0.0 and view[4 + k] == 0.0 and view[8 + k] == 0.0 for k in range(3))
    if zero_axis or not all(math.isfinite(v) for v in eye + at + view + proj):
        eye = [0.0, 0.0, 0.0]
        at, view, proj = build(eye, cli_yaw if cli_yaw is not None else 0.0)
    return tuple(eye), view, proj


def load_model(ins, xb, spec):
    meshes, _ = xbin.load(xb)
    if not meshes:
        return None
    m = meshes[-1]  # LoadFrames: the last Mesh wins
    if not all(math.isfinite(c) for p in m.positions for c in p):
        return None  # M1a-4 (a): a model with a non-finite vertex position is rejected (the viewer's rule, warned there)
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
    bmin = [min(p[k] for p in m.positions) for k in range(3)]  # model-space AABB (make_camera's bounds fallback)
    bmax = [max(p[k] for p in m.positions) for k in range(3)]
    return {'subs': subs, 'gy': gy, 'bmin': bmin, 'bmax': bmax}


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
            # The scroll matrix: _31/_32 = (now - mStartTime) * speed; the first SetTexture of a material starts its
            # clock (mStartTime == -1), so the first frame's offset is 0: the identity (and the viewer's clock never
            # advances, spec M1: static offset only).
            d.raw('XFORM TEXTURE0 sum=%08x' % mat_sum(mat_identity()))
        if not sub['textured']:
            return
        if transparency == 1.0 and sub['trans'] < 1.0:
            d.dx_rs(27, 1); d.dx_tss(0, 4, 3); d.dx_tss(0, 6, 3)
            d.dx_rs(60, tfactor_alpha(sub['trans']) << 24)
        d.raw('TEXTURE 0 %s' % (sub['tex'] or 'NULL'))
        d.raw(material_line(sub['mat'], 'Root/Data/Levels/%s/textures/%s' % (self.level, sub['mat'][4])))

    def draw_x(self, pl, transparency=1.0):  # ScDrawableModel::DrawX 0x005528c0
        d, subs = self.d, pl['m']['subs']
        d.raw('XFORM WORLD sum=%08x' % mat_sum(pl['world']))  # SetTransform(WORLD): no hierarchy, one matrix
        d.dx_rs(153, 0)
        if transparency < 1.0:
            d.dx_rs(47, 16); d.dx_rs(27, 1); d.dx_tss(0, 4, 4); d.dx_tss(0, 5, 3); d.dx_tss(0, 6, 2)
            d.dx_rs(60, tfactor_alpha(transparency) << 24)
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


# Light constants, read from the exe's .rdata at the addresses the decompiled code loads them from (float32).
LIGHT_INTENSITY_KNEE, LIGHT_INTENSITY_BOOST = 0.9, 1.2  # GcLight ctors 0x00490480 (@0x0070c16c / 0x0070c170) and
#                                                         0x004906b0 (@0x0070c19c / 0x0070c1a0): v > 0.9 -> v * 1.2
FAR_INTENSITY, MAX_LIGHT_RANGE = 0.15, 10000.0  # kFarIntensity @0x0073e8f0, kMaxLightRange @0x0073e8f4
CAVE_INTENSITY = 0.25  # GcLightDirector::SetCaveLighting 0x0048e670: SetIntensity(@0x0070bd68) when on
CAVE_OFF_INTENSITY = 1.0  # ... and SetIntensity(@0x0070bd6c) when off
DEFAULT_INTENSITY = 1.0  # GrSetDefaultLighting 0x00470560: @0x00709300
LMBA_AMBIENT = (0.47058824, 0.47058824, 0.47058824, 1.0)  # @0x00709364..70
SUN_DIR = (0.85570401, -0.51642299, 0.0328423)  # "SunL" @0x0070931c..24; "Shad" uses @0x00709348..50 = -SUN_DIR
SUN_RGB = {3: (1.0, 0.98039216, 0.83529413), 5: (0.95686275, 1.0, 0.86666667)}  # @0x00709304.. / 0x00709310..
SHAD_COL_LEV4 = (0.35294119, 0.47058824, 0.54901963, 1.0)  # @0x00709328.. (default) / @0x007092e0.. (LoadLights)
SHAD_COL = (0.35294119, 0.35294119, 0.35294119, 1.0)  # @0x00709338.. (default) / @0x007092f0.. (LoadLights)
WHITE = (1.0, 1.0, 1.0, 1.0)  # kWhiteRGBAFloats (INFERRED from the name: initialised at run time)


def new_light(ltype):
    """A GcFastLight after its memset(+4, 0, 0x68): every D3DLIGHT8 member 0 but the type."""
    z4 = (0.0, 0.0, 0.0, 0.0)
    return {'type': ltype, 'd': z4, 's': z4, 'a': z4, 'pos': (0.0, 0.0, 0.0), 'dir': (0.0, 0.0, 0.0), 'range': 0.0,
            'att': (0.0, 0.0, 0.0), 'base': z4, 'style': 0}


def set_intensity(l, v):
    """GcFastLight::SetIntensity 0x0058ddf0: Diffuse = Specular = base colour (+0x78) * v, per component in float32."""
    l['d'] = l['s'] = tuple(f32(c * v) for c in l['base'])


def ctor_intensity(l, v):
    v = f32(v)
    if f32(LIGHT_INTENSITY_KNEE) < v:
        v = f32(v * f32(LIGHT_INTENSITY_BOOST))
    set_intensity(l, v)


def point_light(colour, intensity, pos, rng, style):
    """GcLight::GcLight 0x00490480 -> GcFastLight::SetPointLight 0x0058d6a0 (type 1, Diffuse = Specular = colour,
    Position) then SetIntensity."""
    l = new_light(1)
    l['base'] = tuple(f32(c) for c in colour)
    l['pos'] = tuple(f32(x) for x in pos)
    l['style'] = style
    if 0.0 < rng:  # @0x0073e908 = 0
        prod = f32(f32(FAR_INTENSITY) * rng)
        # Attenuation1 = (float)1.0 (double @0x0073e918) / (kFarIntensity * range); Attenuation0 = @0x0073e914 = 0.
        # Task 8B D3 (the viewer's rule for a hostile denormal range): a product below FLT_MIN stores FLT_MAX.
        l['att'] = (0.0, f32(1.0 / prod) if prod >= FLT_MIN else FLT_MAX, 0.0)
        l['range'] = rng
    else:  # Attenuation0 = @0x0073e90c = 1, Attenuation1 = @0x0073e910 = 0, Range = kMaxLightRange
        l['att'] = (1.0, 0.0, 0.0)
        l['range'] = f32(MAX_LIGHT_RANGE)
    ctor_intensity(l, intensity)
    return l


def directional_light(colour, intensity, direction):
    """GcLight::GcLight 0x004906b0 (style 0) -> GcFastLight::SetDirectionalLight 0x0058d770 (type 3, Diffuse =
    Specular = colour, Direction) then SetIntensity."""
    l = new_light(3)
    l['base'] = tuple(f32(c) for c in colour)
    l['dir'] = tuple(f32(x) for x in direction)
    ctor_intensity(l, intensity)
    return l


def ambient_light(colour):
    """GcLight::GcLight 0x00490960 (style 0, no SetIntensity) -> GcFastLight::SetAmbientLight 0x0058d8c0: type 3,
    Ambient = colour, Attenuation0 = @0x0073e938 = 1, Attenuation1 = @0x0073e93c = 0, Range = kMaxLightRange."""
    l = new_light(3)
    l['base'] = l['a'] = tuple(f32(c) for c in colour)
    l['att'] = (1.0, 0.0, 0.0)
    l['range'] = f32(MAX_LIGHT_RANGE)
    return l


def default_lighting(level_type):
    """GrSetDefaultLighting 0x00470560: "LmbA" (an ambient light) below level type 3, else "SunL" + "Shad"."""
    if level_type < 3:
        return [ambient_light(LMBA_AMBIENT)]
    sun = SUN_RGB.get(level_type, WHITE[:3]) + (WHITE[3],)
    shad = SHAD_COL_LEV4 if level_type == 4 else SHAD_COL
    return [directional_light(sun, DEFAULT_INTENSITY, SUN_DIR),
            directional_light(shad, DEFAULT_INTENSITY, tuple(-x for x in SUN_DIR))]


def lights(ins, level_type):
    """GcLightLoader::LoadLights 0x00470010: the lights in add order. <area>_LIGHT.slb {slot, n, ptr} + 44-byte records
    {id @0, position/direction 3f @4, intensity @0x10, range @0x14, rgba 4f @0x18, flags @0x28}; style from flags 2/4/8/
    0x10/0x20 = 1/3/4/5/2; 0x40 point light; 0x80 directional (colour white on level type 4) + for the first one the
    opposite fill "Shad"; no 0x80 record (or no table) -> GrSetDefaultLighting. The viewer's untrusted-input rule
    (Task 8B D3): a record with a non-finite position, intensity, range or colour is skipped."""
    b = ins.entry(ins.area + '_light.slb')
    if not b:
        return default_lighting(level_type)
    n, base = slb_table(b, 4, 8)
    out, directional, shad = [], False, False
    for i in range(n):
        r = base + 44 * i
        pos = struct.unpack_from('<3f', b, r + 4)
        inten, rng = struct.unpack_from('<2f', b, r + 0x10)
        col = struct.unpack_from('<4f', b, r + 0x18)
        flags = struct.unpack_from('<I', b, r + 0x28)[0]
        if not all(math.isfinite(x) for x in pos + (inten, rng) + col):
            continue
        style = 1 if flags & 2 else 3 if flags & 4 else 4 if flags & 8 else 5 if flags & 0x10 else 2 if flags & 0x20 else 0
        if flags & 0x40:
            out.append(point_light(col, inten, pos, rng, style))
        elif flags & 0x80:
            directional = True
            out.append(directional_light(WHITE if level_type == 4 else col, inten, pos))
            if not shad:
                shad = True
                out.append(directional_light(SHAD_COL_LEV4 if level_type == 4 else SHAD_COL, inten, tuple(-x for x in pos)))
    return out + ([] if directional else default_lighting(level_type))


def light_line(i, l):
    c4 = lambda c: '(%s)' % ','.join(g6(x) for x in c)
    return ('LIGHT %d type=%d d=%s s=%s a=%s pos=%s dir=%s range=%s att=%s' %
            (i, l['type'], c4(l['d']), c4(l['s']), c4(l['a']), c4(l['pos']), c4(l['dir']), g6(l['range']), c4(l['att'])))


def expected(root, level, area, cam):
    ins = Install(root, level, area)
    places, start = load_area(ins)
    eye, view, proj = camera(places, start, cam)
    level_type = (ord(ins.level[3]) - 0x30) & 0xff if len(ins.level) > 3 else 0xd0
    d = Device()
    f = Frame(d, ins.level)
    cave = None  # no SetCaveLighting call: the ctor intensities stay
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
        # GcLightDirector::SetCaveLighting 0x0048e670 (bool) loops over the light director's list and, for a light whose
        # GcFastLight::IsActive (byte +0x6c: 1 from GcFastLight::GcFastLight 0x0058d5b0; SetActive 0x0058ddd0 is never
        # called) holds, whose GcLight byte +0xb0 (the GeLightStyle the ctor stores) is 0 and while sUseSpots (@0x0070bd34,
        # static 0, never written) is false, calls SetIntensity(DAT_0070bd68 = 0.25) when true and SetIntensity(DAT_0070bd6c
        # = 1.0) when false: an overwrite from the base colour, both ways. It runs after the area's lights exist: InitFull
        # is reached from gs_lego_level.areasetactivenow, after lego_level_initfull's areasetcurrent -> setcurrentarea ->
        # LoadArea -> LoadLights (Base.osi bytecode). Each call overwrites, so the last one wins.
        for k, args in calls:
            if k == 'gclightdirector.setcavelighting' and len(args) == 1 and isinstance(args[0], (int, float)):
                cave = args[0] != 0
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
    # ScPerspectiveCamera::BuildProjectionMatrix (dirty on the first frame), ScRenderer::SetViewTransform 0x00530560,
    # ScRenderer::SetProjectionTransform 0x00530640
    d.raw('XFORM PROJECTION sum=%08x' % mat_sum(proj)); d.raw('XFORM VIEW sum=%08x' % mat_sum(view))
    d.raw('XFORM PROJECTION sum=%08x' % mat_sum(proj))
    d.raw('# node MovingLight')
    count = 0
    for l in lights(ins, level_type):
        if cave is not None and l['style'] == 0:
            set_intensity(l, f32(CAVE_INTENSITY) if cave else f32(CAVE_OFF_INTENSITY))
        if count == 8:  # ScRenderer::GetLightID 0x005310c0: ids cycle 0..7
            count = 0
        d.raw(light_line(count, l)); d.raw('LIGHTENABLE %d 1' % count)
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
            # The exe's ids at 0x0070b060 / 0x0070b064 are the bytes "1yks" / "rtaw": GetAreaID returns the 4CC as a
            # little-endian u32 (OBJ record id field, the same as the "tnlp" plnt check), so they ARE the ids "sky1" /
            # "watr" (Task 8A E1; this compared big-endian reads before). (iv & 255) is the id's LAST character.
            iv = struct.unpack('>I', p['id'].encode('latin1').ljust(4, b'\0'))[0]
            if p['id'] == 'sky1':
                best = k
                break
            if p['id'] == 'watr' or (iv & 255) == ord('w'):
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
        else:  # M1a-4 (b): XFORM checksums and LIGHT values are compared too
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
    ap.add_argument('--fov', type=float, default=72.0)  # @0x00704e00
    ap.add_argument('--near', type=float, default=0.01)  # @0x00704e1c
    ap.add_argument('--far', type=float, default=4700.0)  # mDrawDistance @0x0074a328
    ap.add_argument('--pitch', type=float, default=-10.0)  # lomn_view's default
    ap.add_argument('--yaw', type=float)  # lomn_view: towards the bounds centre unless given
    ap.add_argument('--width', type=int, default=1280)
    ap.add_argument('--height', type=int, default=720)
    ap.add_argument('--out')
    ap.add_argument('--compare')
    a = ap.parse_args()
    cam = {'eye_height': a.eye_height, 'fov': a.fov, 'near': a.near, 'far': a.far, 'pitch': a.pitch, 'yaw': a.yaw,
           'width': a.width, 'height': a.height}
    exp = filtered(expected(a.install, a.level, a.area, cam))
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
