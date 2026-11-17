"""M2a-3 Task 9: the independent HUD state-log oracle. Like a9_oracle.py for areas, it never reads engine/.

Writes the expected HUD part of the D3D8 state log for one frame, from:
  - docs/osi/hud.md §3 (the pass), §4.11 (port rulings), §10 (the log grammar) and §1.7 (the camera);
  - the Python oracle's own run of Base.osi (tools/research/osi_vm.py + osi_natives, real natives): that frame's `Z`
    block (hud.md §9) and its block-file model (`sr_block_file_exists`, for the sprite texture lookup);
  - the native state log of the frames before (every frame kept, `--shot-frames 0,1,...,F`), read only for the device
    state the area leaves behind (the HUD's dx writes are gated on it) and the last render mode pushed (§3.2's L).

  hud_oracle.py --osi B --install I --frames N [--keys K] [--mouse M] --frame F --state-log S [--out O] [--check]

The HUD part is the span from `# viewport HdDr` up to (not including) `# HUD view: ScRenderer::Finish`. --check diffs it
against that span of S and exits 1 on a difference. It also prints the VIEW / PROJECTION sums from the Z camera (I3) and
the cursor / text boxes from the matrices (I2) as `# oracle …` lines on stdout. With --check the camera sums must equal the
frame's native XFORM VIEW / PROJECTION, and with --native-boxes the boxes must equal lomn_view's `hud box` cell boxes.

Refused (exit 3, never a guess): a shown coloured sprite (RestoreTSS reads the ScDxStates caches, whose history the
log does not show), b6 != 0, DrawJust 0x100/0x200, box-fit text, codes >= 0xF0, a .dds texture, L with 0x80000 or 0x40000,
FOGENABLE set before the HUD.
"""
from __future__ import annotations

import argparse
import math
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path[:0] = [str(HERE.parent), str(HERE.parent.parent / "osi")]

import colfont  # noqa: E402  (tools/research/colfont.py: the .col/.fnt reader, written from the font probe)


class Refused(Exception):
    pass


# ---- f32 arithmetic: each x87 op rounded once to f32 (pc24); double then f32 is exact for + - * / (53 >= 2*24 + 2)
def f(x: float) -> float:
    return struct.unpack("<f", struct.pack("<f", x))[0]


def bits(x: float) -> int:
    return struct.unpack("<I", struct.pack("<f", x))[0]


def chop(x: float) -> int:  # fistp with RC = chop
    return int(x) if math.isfinite(x) else -(2**31)


def h31(data: bytes, h: int = 0) -> int:  # the M0 checksum: sum = sum * 31 + byte (§10.1)
    for c in data:
        h = (h * 31 + c) & 0xFFFFFFFF
    return h


def mat_sum(m) -> int:
    return h31(b"".join(struct.pack("<f", v) for v in m))


def mul(a, b):
    """Row-vector product, 0x52b580's order: ((a0*b0 + a1*b1) + a2*b2) + a3*b3, each op f32 (§4.11)."""
    out = []
    for i in range(4):
        for j in range(4):
            acc = f(a[4 * i] * b[j])
            for k in range(1, 4):
                acc = f(acc + f(a[4 * i + k] * b[4 * k + j]))
            out.append(acc)
    return out


def rot_x(rad_f32: float):
    """D3DUtil_SetRotateXMatrix 0x52c2a0 (§3.4): identity with cos at m5/m10, sin at m6, -sin at m9, CRT cos/sin as f32."""
    c, s = f(math.cos(rad_f32)), f(math.sin(rad_f32))
    m = [0.0] * 16
    m[0] = m[15] = 1.0
    m[5], m[6], m[9], m[10] = c, s, -s, c
    return m


PI_F32 = f(math.pi)  # 0x40490fdb
QUAD = ((-0.5, 0.5), (0.5, 0.5), (-0.5, -0.5), (0.5, -0.5))  # v0..v3 (§3.4)
HUD_Z = -7499.0


def xform(m, vx, vy):
    def c(j):
        return f(f(f(f(vx * m[j]) + f(vy * m[4 + j])) + f(0.0 * m[8 + j])) + m[12 + j])
    return c(0), c(1), c(2)


def vsum(m) -> int:
    return h31(b"".join(struct.pack("<3f", *xform(m, *v)) for v in QUAD))


# ---- D3D8 device: defaults (DirectX 8.1 SDK "Render states" / "Texture stage states" defaults) + the log lines
RS = {"ZENABLE": 7, "FILLMODE": 8, "ZWRITEENABLE": 14, "ALPHATESTENABLE": 15, "SRCBLEND": 19, "DESTBLEND": 20,
      "CULLMODE": 22, "ALPHAREF": 24, "ALPHAFUNC": 25, "ALPHABLENDENABLE": 27, "FOGENABLE": 28, "TEXTUREFACTOR": 60,
      "LIGHTING": 137, "SOFTWAREVERTEXPROCESSING": 153}
RS_DEFAULT = {"ZENABLE": 1, "FILLMODE": 3, "ZWRITEENABLE": 1, "ALPHATESTENABLE": 0, "SRCBLEND": 2, "DESTBLEND": 1,
              "CULLMODE": 3, "ALPHAREF": 0, "ALPHAFUNC": 8, "ALPHABLENDENABLE": 0, "FOGENABLE": 0,
              "TEXTUREFACTOR": 0xFFFFFFFF, "LIGHTING": 1, "SOFTWAREVERTEXPROCESSING": 0}
TSS_DEFAULT = {(0, "COLOROP"): 4, (0, "COLORARG1"): 2, (0, "COLORARG2"): 1, (0, "ALPHAOP"): 2, (0, "ALPHAARG1"): 2,
               (0, "ALPHAARG2"): 1, (0, "MAGFILTER"): 1, (0, "MINFILTER"): 1, (0, "TEXTURETRANSFORMFLAGS"): 0,
               (1, "COLOROP"): 1, (1, "ALPHAOP"): 1}
MATERIAL = "MATERIAL d=(0.7,0.7,0.7,1) a=(1,1,1,1) s=(1,1,1,1) e=(0,0,0,0) p=80"  # §10.1, LoadIndexed's constants


class Device:
    def __init__(self, rs=None, tss=None):
        self.rs = {**RS_DEFAULT, **(rs or {})}
        self.tss = {**TSS_DEFAULT, **(tss or {})}
        self.out: list[str] = []

    def _fmt(self, name, v):
        return "0x%08x" % v if name in ("TEXTUREFACTOR",) else str(v)

    # raw device calls: every call is a line
    def set_rs(self, name, v):
        self.rs[name] = v
        self.out.append("RS %s %s" % (name, self._fmt(name, v)))

    def set_tss(self, stage, name, v):
        self.tss[(stage, name)] = v
        self.out.append("TSS%d %s %d" % (stage, name, v))

    # ScDxStates 0x64b1e0 / 0x64b250: read the device, Set only on a change (§3.2)
    def dx_rs(self, name, v):
        if self.rs[name] != v:
            self.set_rs(name, v)

    def dx_tss(self, stage, name, v):
        if self.tss[(stage, name)] != v:
            self.set_tss(stage, name, v)

    def xform(self, kind, m):
        self.out.append("XFORM %s sum=%08x" % (kind, mat_sum(m)))

    def texture(self, name):
        self.out.append("TEXTURE 0 %s" % (name or "NULL"))

    def note(self, s):
        self.out.append("# " + s)


def replay_log(lines):
    """Device state from RS/TSS lines (the first token of the value; float states print '0x… (v)')."""
    rs, tss = {}, {}
    for ln in lines:
        t = ln.split()
        if len(t) >= 3 and t[0] == "RS":
            rs[t[1]] = int(t[2], 0)
        elif len(t) >= 3 and t[0].startswith("TSS") and t[0][3:].isdigit():
            tss[(int(t[0][3:]), t[1])] = int(t[2], 0)
    return rs, tss


def split_log(path: Path, frame: int):
    """(lines before the HUD of `frame`, the native HUD span, the last SrPushRenderMode before it, {VIEW/PROJECTION: sum=…}
    of the frame's viewport camera)."""
    lines = path.read_text(encoding="latin-1").splitlines()
    start = lines.index("# script frame %d" % frame)
    v = lines.index("# viewport HdDr", start)
    end = lines.index("# HUD view: ScRenderer::Finish", v)
    pushes = [ln for ln in lines[start:v] if ln.startswith("# SrPushRenderMode ")]
    if not pushes:
        raise Refused("no render mode pushed in frame %d before the HUD" % frame)
    prev = lines[:v]
    need = range(frame)  # every earlier frame must be in the log, or the device state is unknown
    have = {int(ln.split()[3]) for ln in prev if ln.startswith("# script frame ")}
    missing = [k for k in need if k not in have]
    if missing:
        raise Refused("the state log lacks frames %s… (run with --shot-frames 0,1,…,%d)" % (missing[:3], frame))
    cam = {ln.split()[1]: ln.split()[2] for ln in lines[start:v] if ln.startswith(("XFORM VIEW ", "XFORM PROJECTION "))}
    return prev, lines[v:end], int(pushes[-1].split()[2], 16), cam


# ---- the scene from the Z block (hud.md §9.2)
def hexf(s):
    return struct.unpack("<f", struct.pack("<I", int(s, 16)))[0]


def hex_bytes(tok):
    n = int(tok[0])
    raw = bytes.fromhex(tok[1]) if n else b""
    if len(raw) != n:
        raise Refused("byte-string field")
    return raw


def parse_z(z):
    nodes, fields = [], {}
    for ln in z:
        t = ln.split()
        if t[0] == "Z":
            t = t[1:]
        if not t:
            continue
        if t[0] == "hud.node":
            if t[4] != "-":
                nodes.append((int(t[4]), t[2], t[3]))
        elif "." in t[0] and t[0].split(".")[0] in ("sprite", "text", "font", "camera"):
            if t[0].startswith("camera."):
                fields[(t[0], "")] = t[1:]
            else:
                fields[(t[0], t[1])] = t[2:]
    nodes.sort()
    return nodes, fields


def written(v):
    return v is not None and v and v[0] != "-"


# ---- the pass
class Hud:
    def __init__(self, dev: Device, fields, vfs_read, tex_exists, width=640, height=480):
        self.d, self.F, self.read, self.exists = dev, fields, vfs_read, tex_exists
        self.w, self.h = float(width), float(height)
        self.fonts = {}
        self.boxes = {}

    def get(self, key, who):
        return self.F.get((key, who))

    def box(self, who, m):
        for v in QUAD:
            x, y, _ = xform(m, *v)
            b = self.boxes.setdefault(who, [x, y, x, y])
            b[:] = [min(b[0], x), min(b[1], y), max(b[2], x), max(b[3], y)]

    def material_draw(self, tex, world, what, who):
        """ScDrawableMaterial::Draw 0x54ee70 -> ScDrawableTexture::Draw 0x5569f0 (§3.4)."""
        d = self.d
        d.dx_tss(0, "TEXTURETRANSFORMFLAGS", 0)
        d.out.append(MATERIAL)
        d.dx_rs("SOFTWAREVERTEXPROCESSING", 0)  # after SetStreamSource/SetVertexShader(0x112): no line of their own
        d.texture(tex)
        d.xform("WORLD", world)
        d.out.append("DRAW %s call=DIP prim=5 count=2 fvf=0x112 vsum=%08x" % (what, vsum(world)))
        d.texture(None)
        self.box(who, world)

    # §3.4 / §3.5
    def sprite(self, who, pri):
        rect, file, b6 = self.get("sprite.rect", who), self.get("sprite.file", who), self.get("sprite.b6", who)
        if not (written(rect) and written(file) and written(b6)):
            raise Refused("sprite %s: no rect/file/b6" % who)
        if b6[0] != "00":
            raise Refused("sprite %s: b6 != 0 (3x3)" % who)
        c = self.get("sprite.color", who)
        if written(c) and [hexf(v) for v in c] != [1.0, 1.0, 1.0, 1.0]:  # SetColor: (1,1,1,1) clears doColor (§4.11)
            raise Refused("sprite %s: coloured (RestoreTSS needs the ScDxStates cache history)" % who)
        al = self.get("sprite.alpha", who)
        alpha = written(al) and al[0] != "00"
        if alpha and pri != 100:  # 0x52723f (doTransparency: no Z field, no native writes +0x50)
            return 100
        d = self.d
        d.dx_rs("ALPHABLENDENABLE", 1)
        if alpha:
            d.dx_rs("ALPHAREF", chop(1.0))       # mTransparentValue 1.0 (§4.11 sprite defaults)
            d.dx_rs("ALPHAFUNC", 5)               # +0x161 = 0
            d.dx_tss(0, "COLORARG2", 0)           # !doColor
        x1, y1, x2, y2 = (hexf(v) for v in rect)
        w, h = f(x2 - x1), f(y2 - y1)
        S = [w, 0.0, 0.0, 0.0, 0.0, h, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0]
        m = mul(S, rot_x(-PI_F32))                # S·R (0x527367)
        tx, ty, tz = f(f(x2 - f(w * 0.5)) + 0.0), f(f(y2 - f(h * 0.5)) + 0.0), f(0.0 + HUD_Z)  # SetPosition (§4.11)
        m[12], m[13], m[14] = f(m[12] + tx), f(m[13] + ty), f(m[14] + tz)
        name = hex_bytes(file).decode("latin-1")
        if "." in name:
            raise Refused("sprite file with a '.' (%s)" % name)
        if self.exists(name + ".dds"):
            raise Refused("sprite texture %s.dds" % name)
        tex = name + ".tga" if self.exists(name + ".tga") else None  # ScDrawableTexture::Load (§4.11): .dds, .tga, else NULL
        self.material_draw(tex, m, "hud.sprite %s tile=0,0" % who, who)
        return -1

    def font(self, fwho):
        if fwho in self.fonts:
            return self.fonts[fwho]
        ff = self.get("font.file", fwho)
        if not written(ff):
            raise Refused("font %s: no file" % fwho)
        path = hex_bytes(ff).decode("latin-1")
        data = self.read(path + ".col")
        if data is None:
            raise Refused("font %s: %s.col missing" % (fwho, path))
        name = path.replace("\\", "/").rsplit("/", 1)[-1]
        tf = colfont.TileFont(name, colfont.Col(data))
        col = self.get("font.color", fwho)
        color = [hexf(v) for v in col] if written(col) else [1.0, 1.0, 1.0, 1.0]  # ctor (1,1,1,1) (§4.11)
        tr = self.get("font.transp", fwho)
        transp = tr[0] != "00" if written(tr) else True  # ctor +0xb0 = 1
        self.fonts[fwho] = (path, tf, color, transp)
        return self.fonts[fwho]

    # §3.6 ScTextObject::Render -> DrawJust / Draw
    def text(self, who, pri):
        pos, st, fh = self.get("text.pos", who), self.get("text.string", who), self.get("text.font", who)
        if not (written(pos) and written(st) and written(fh)):
            raise Refused("text %s: no pos/string/font" % who)
        x, y, z = (hexf(v) for v in pos)
        sc = self.get("text.scale", who)
        xs, ys = (hexf(sc[0]), hexf(sc[1])) if written(sc) else (1.0, 1.0)
        jo, jw = self.get("text.juston", who), self.get("text.just", who)
        juston = written(jo) and jo[0] != "00"
        just = int(jw[0], 16) if written(jw) else 0
        path, tf, color, transp = self.font(fh[0])
        fnt = tf.fnt
        s = hex_bytes(st)
        if juston:  # DrawJust 0x627c60 (§4.11)
            chh = chop(f(float(fnt.char_h & 0xFFFFFFFF) * ys)) & 0xFFFFFFFF
            if just & 1:
                y = f(float(chh) + y)
            elif just & 2:
                y = f(y - f(float(fnt.baseline & 0xFFFFFFFF) * ys))
            elif just & 4:
                y = f(f(float(chh >> 1) - f(float(fnt.baseline & 0xFFFFFFFF) * ys)) + y)
            if just & 0x300:
                raise Refused("text %s: DrawJust 0x%x (GetStrLen path)" % (who, just))
        return self.draw(who, path, tf, color, transp, x, y, s, xs, ys, pri)

    def draw(self, who, path, tf, color, transp, x, y, s, xs, ys, pri):
        """ScTileFont::Draw 0x627dd0 (§3.6, §4.11), raw device calls."""
        fnt, d = tf.fnt, self.d
        start = x
        y = f(f(float(fnt.baseline & 0xFFFFFFFF) * ys) + y)
        kern = float(fnt.kerning)
        alpha = True  # +0xb1 = 1 in the ctor, no native writes it
        if (transp or alpha) and pri != 100:
            return 100
        s1, s2, s3 = d.tss[(0, "COLORARG1")], d.tss[(0, "COLOROP")], d.tss[(0, "COLORARG2")]
        if s1 != 2:
            d.set_tss(0, "COLORARG1", 2)
        if s2 != 4:
            d.set_tss(0, "COLOROP", 4)
        if s3 != 3:
            d.set_tss(0, "COLORARG2", 3)
        ch = [chop(f(c * 255.0)) & 0xFF for c in color]
        d.set_rs("TEXTUREFACTOR", (ch[0] << 16 | ch[3] << 24 | ch[1] << 8 | ch[2]) & 0xFFFFFFFF)
        cull = d.rs["CULLMODE"]
        if cull != 1:
            d.set_rs("CULLMODE", 1)
        minf, filt = d.tss[(0, "MINFILTER")], False
        if minf != 2:  # linear (+0xb2) = 1 in the ctor
            d.set_tss(0, "MINFILTER", 2)
            d.set_tss(0, "MAGFILTER", 2)
            filt = True
        d.set_rs("ALPHABLENDENABLE", 1)
        d.set_rs("SRCBLEND", 5)
        d.set_rs("DESTBLEND", 6)
        d.set_rs("ZWRITEENABLE", 0)  # alpha test branch: +0xc8 == 0, never taken
        R = rot_x(PI_F32)
        for b in s:
            if b >= 0xF0:
                raise Refused("text %s: code 0x%02x" % (who, b))
            if b == 0x0A:
                x = start
                y = f(float(chop(f(float(fnt.char_h & 0xFFFFFFFF) * ys)) & 0xFFFFFFFF) + y)
                continue
            if b == 0x20:
                sp = (fnt.space + fnt.kerning + 2**31) % 2**32 - 2**31
                x = f(f(float(sp) * xs) + x)
                continue
            i = fnt._first.get(b)
            if i is None:
                x = f(f(f(float(fnt.max_width & 0xFFFFFFFF) + kern) * xs) + x)
                continue
            _, gw, _, xo, _ = fnt.records[i]
            w = f(float(fnt.char_w & 0xFFFFFFFF) * xs)
            h = f(float(fnt.char_h & 0xFFFFFFFF) * ys)
            x = f(x - f(float(xo & 0xFFFFFFFF) * xs))
            S = [w, 0.0, 0.0, 0.0, 0.0, h, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0]
            m = mul(R, S)  # R·S (0x628630)
            m[12] = f(f(f(0.5 * w) + x) + m[12])
            m[13] = f(f(y - f(0.5 * h)) + m[13])
            m[14] = f(m[14] + HUD_Z)
            tex = "%s.col/%s" % (path, tf.glyph_name(i))
            self.material_draw(tex, m, "hud.glyph %s ch=%02x" % (who, b), who)
            x = f(f(f(float((xo + gw) & 0xFFFFFFFF) + kern) * xs) + x)
        if s1 != 2:
            d.set_tss(0, "COLORARG1", s1)
        if s2 != 4:
            d.set_tss(0, "COLOROP", s2)
        if s3 != 3:
            d.set_tss(0, "COLORARG2", s3)
        if cull != 1:
            d.set_rs("CULLMODE", cull)
        if filt:  # 0x628823: both to the saved MINFILTER
            d.set_tss(0, "MINFILTER", minf)
            d.set_tss(0, "MAGFILTER", minf)
        d.set_rs("ALPHABLENDENABLE", 0)
        d.set_rs("ZWRITEENABLE", 1)
        return -1

    def run(self, nodes, last_mode, can_use_w=True):
        d = self.d
        d.note("viewport HdDr")
        d.note("HUD view: ScRenderer::Start")
        d.note("SrPushRenderMode 0x8c034")
        # §3.2 against L
        if last_mode & 0x80000 or last_mode & 0x40000:
            raise Refused("last mode 0x%x has 0x80000/0x40000" % last_mode)
        for name, v in (("ALPHAOP", 2), ("ALPHAARG1", 2)):
            d.dx_tss(0, name, v)
        for name, v in (("ALPHABLENDENABLE", 1), ("SRCBLEND", 5), ("DESTBLEND", 6), ("ALPHAREF", 0),
                        ("ALPHATESTENABLE", 1), ("ZWRITEENABLE", 0)):
            d.dx_rs(name, v)
        d.dx_tss(0, "COLOROP", 4)
        d.dx_tss(0, "COLORARG1", 2)
        d.dx_rs("ZENABLE", 0 if can_use_w else 1)  # 0x8000 with CanUseW
        d.dx_rs("FILLMODE", 3)
        d.dx_rs("LIGHTING", 0)
        d.dx_rs("CULLMODE", 1)
        if d.rs["FOGENABLE"]:
            raise Refused("FOGENABLE set before the HUD (SetFog(false) not modelled)")
        # §3.3 BuildOrthoMatrix, SetTransform PROJECTION then VIEW (raw)
        P = [0.0] * 16
        P[0], P[5], P[10] = f(2.0 / self.w), f(2.0 / f(0.0 - self.h)), f(2.0 / 15000.0)
        P[12], P[13], P[14], P[15] = -1.0, 1.0, 1.0, 1.0
        V = [1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, -0.0, -0.0, -0.0, 1.0]
        d.xform("PROJECTION", P)
        d.xform("VIEW", V)
        deferred = []
        for _, kind, who in nodes:
            r = self.sprite(who, 0) if kind == "sprite" else self.text(who, 0)
            if r == 100:
                deferred.append((kind, who))
        for kind, who in deferred:
            self.sprite(who, 100) if kind == "sprite" else self.text(who, 100)
        return d.out


# ---- §1.7: the viewport camera's VIEW / PROJECTION from the Z camera fields (I3)
def look_at_rh(eye, at, up):
    def sub(a, b):
        return [f(a[i] - b[i]) for i in range(3)]

    def cross(a, b):
        return [f(f(a[1] * b[2]) - f(a[2] * b[1])), f(f(a[2] * b[0]) - f(a[0] * b[2])), f(f(a[0] * b[1]) - f(a[1] * b[0]))]

    def norm(a):
        n = f(math.sqrt(f(f(f(a[0] * a[0]) + f(a[1] * a[1])) + f(a[2] * a[2]))))
        return [f(v / n) for v in a]

    def dot(a, b):
        return f(f(f(a[0] * b[0]) + f(a[1] * b[1])) + f(a[2] * b[2]))

    z = norm(sub(eye, at))
    x = norm(cross(up, z))
    y = cross(z, x)
    return [x[0], y[0], z[0], 0.0, x[1], y[1], z[1], 0.0, x[2], y[2], z[2], 0.0,
            -dot(x, eye), -dot(y, eye), -dot(z, eye), 1.0]


def perspective_fov_rh(w, h):
    """D3dx8cw.dll 0x10008c8d (hud.md §1.7 correction): fsincos, cos/sin, zf/(zn-zf), m14 = m10*zn."""
    fov = f(f(72.0) * f(0.017453292))
    half = f(fov * 0.5)
    ys = f(f(math.cos(half)) / f(math.sin(half)))
    zn, zf = f(0.01), f(4700.0)
    m = [0.0] * 16
    m[0], m[5] = f(ys / f(float(w) / float(h))), ys
    m[10] = f(zf / f(zn - zf))
    m[11] = -1.0
    m[14] = f(m[10] * zn)
    return m


def camera_sums(fields, w, h):
    eye, at = fields.get(("camera.eye", "")), fields.get(("camera.target", ""))
    mode = fields.get(("camera.mode", ""))
    if not (written(eye) and written(at)):
        return None
    if written(mode) and int(mode[0], 16) in (1, 6, 0xC, 0xD):
        raise Refused("camera mode %s adds a RotateX" % mode[0])
    E, A = [hexf(v) for v in eye], [hexf(v) for v in at]
    return mat_sum(look_at_rh(E, A, [0.0, 1.0, 0.0])), mat_sum(perspective_fov_rh(w, h))


# ---- driver: the Python oracle's own run to `frame`
def python_run(a):
    import osi_vm
    from osi_natives import framework
    from osi_natives.host_sources import FrameHost
    from osi_natives.services import sr_block_file_exists

    root = HERE.parent.parent.parent
    data = root / "data" / "osi"
    script = Path(a.osi).read_bytes()
    types = osi_vm.TypeTable.load(str(data / "types.json"))
    reg = osi_vm.NativeRegistry.load(str(data / "natives.json"), str(data / "natives_beta150.json"), types)
    reg.return_log = []
    heap = osi_vm.Heap(None)
    vm = osi_vm.Vm(script, reg, heap, None)
    vfs = framework.Vfs(a.install)
    ctx = framework.HostContext(vm, heap, vfs, framework.HostFlags(), reg.return_log)
    framework.bind_natives(reg, ctx)
    vm.pcs_seen = set()
    events = {}
    for fr, calls in (osi_vm.key_calls(a.keys).items() if a.keys else []):
        for name, args in calls:
            events.setdefault((fr, "keymapper" if args[1].value == 251 else "vk"), []).append((name, args))
    if a.mouse:
        import keys as osi_keys
        for fr, args in osi_keys.expand_mouse(a.mouse, a.frames):
            events.setdefault((fr, "mouse"), []).append(("OnEvent", [osi_vm.Variant(*x) for x in args]))
    host = FrameHost(vm, None, ctx, events, camera_model=True)
    if not vm.call("Setup", []).ok:
        raise Refused("Setup failed")
    for k in range(a.frame + 1):
        vm.pcs_seen = set()
        if not host.frame(k):
            raise Refused("the Python run stopped at frame %d" % k)
    return host.z[a.frame], vfs.read, (lambda n: sr_block_file_exists(ctx, n.encode("latin-1")))


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--osi", required=True)
    ap.add_argument("--install", required=True)
    ap.add_argument("--frames", type=int, required=True)
    ap.add_argument("--keys", default="")
    ap.add_argument("--mouse", default="")
    ap.add_argument("--frame", type=int, required=True)
    ap.add_argument("--state-log", required=True)
    ap.add_argument("--width", type=int, default=640)
    ap.add_argument("--height", type=int, default=480)
    ap.add_argument("--out")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--native-boxes", help="lomn_view stdout: its `hud box` cell boxes of --frame must equal ours (I2)")
    a = ap.parse_args(argv)
    try:
        prev, native, last, ncam = split_log(Path(a.state_log), a.frame)
        rs, tss = replay_log(prev)
        z, read, exists = python_run(a)
        nodes, fields = parse_z(z)
        hud = Hud(Device(rs, tss), fields, read, exists, a.width, a.height)
        mine = hud.run(nodes, last, can_use_w=True)  # the port reports CanUseW (hud.md §3.2 ZENABLE)
        cams = camera_sums(fields, a.width, a.height)
    except Refused as e:
        print("hud_oracle: refused: %s" % e, file=sys.stderr)
        return 3
    if a.out:
        Path(a.out).write_text("".join(ln + "\n" for ln in mine), encoding="latin-1")
    if cams:
        print("# oracle camera frame %d VIEW sum=%08x PROJECTION sum=%08x" % (a.frame, *cams))
    for who, b in sorted(hud.boxes.items()):
        print("# oracle box frame %d %s %g %g %g %g" % (a.frame, who, *b))
    if a.check:
        ok = mine == native
        if cams:
            want = {"VIEW": "sum=%08x" % cams[0], "PROJECTION": "sum=%08x" % cams[1]}
            if ncam != want:
                print("camera: native %s, oracle %s" % (ncam, want))
                ok = False
        if not ok:
            import difflib
            sys.stdout.writelines(difflib.unified_diff([l + "\n" for l in native], [l + "\n" for l in mine],
                                                       "native", "oracle", n=2))
        if a.native_boxes:
            nb = {}
            for ln in Path(a.native_boxes).read_text(encoding="latin-1").splitlines():
                t = ln.split(" ")
                if ln.startswith("hud box ") and int(t[2]) == a.frame:
                    nb[t[3]] = [float(v) for v in t[5:9]]
            if nb != hud.boxes:
                print("boxes: native %s, oracle %s" % (nb, hud.boxes))
                ok = False
        print("hud_oracle frame %d: %s (%d lines)" % (a.frame, "IDENTICAL" if ok else "DIFFERS", len(mine)))
        return 0 if ok else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
