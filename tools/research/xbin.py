"""Independent parser for binary DirectX .x files (xof 0303bin 0032/0064). Oracle for the C++ parser.

Token stream (little-endian u16 token ids): NAME=1(u32 n, chars) STRING=2(u32 n, chars, u16 terminator)
INTEGER=3(u32) GUID=5(16 bytes) INTEGER_LIST=6(u32 n, n*u32) FLOAT_LIST=7(u32 n, n*float32|64)
OBRACE=10 CBRACE=11 ... TEMPLATE=31 and primitive-type tokens 40..53 (only inside template declarations).
Data object: NAME(type) [NAME(name)] [GUID] '{' members... '}'. A reference is '{' NAME '}' with no members.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field

T_NAME, T_STRING, T_INTEGER, T_GUID, T_INTLIST, T_FLTLIST = 1, 2, 3, 5, 6, 7
T_OBRACE, T_CBRACE, T_TEMPLATE = 10, 11, 31
SIMPLE = {12, 13, 14, 15, 16, 17, 18, 19, 20, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53}
MAX_DEPTH = 64


class XError(Exception):
    pass


@dataclass
class XObj:
    type: str
    name: str = ""
    values: list = field(default_factory=list)      # ('i', int) / ('f', float) / ('s', str) in file order
    children: list = field(default_factory=list)    # XObj
    refs: list = field(default_factory=list)        # referenced object names


class Reader:
    def __init__(self, data: bytes):
        if len(data) < 16 or data[:4] != b"xof ":
            raise XError("not a .x file")
        if data[8:12] != b"bin ":
            raise XError(f"not binary: {data[8:12]!r}")
        self.fsize = {b"0032": 4, b"0064": 8}.get(data[12:16])
        if not self.fsize:
            raise XError(f"float size {data[12:16]!r}")
        self.d, self.p, self.n = data, 16, len(data)

    def need(self, k):
        if self.p + k > self.n:
            raise XError(f"truncated at {self.p} (+{k} of {self.n})")

    def u16(self):
        self.need(2); v = struct.unpack_from("<H", self.d, self.p)[0]; self.p += 2; return v

    def u32(self):
        self.need(4); v = struct.unpack_from("<I", self.d, self.p)[0]; self.p += 4; return v

    def peek(self):
        self.need(2); return struct.unpack_from("<H", self.d, self.p)[0]

    def name(self):
        n = self.u32(); self.need(n); s = self.d[self.p:self.p + n].decode("latin1"); self.p += n; return s

    def skip_template(self):
        if self.u16() != T_NAME:                      # template name is a NAME token
            raise XError(f"template without name at {self.p}")
        self.name()
        depth = 0
        while True:
            t = self.u16()
            if t == T_NAME: self.name()
            elif t == T_GUID: self.need(16); self.p += 16
            elif t == T_INTEGER: self.u32()
            elif t == T_OBRACE: depth += 1
            elif t == T_CBRACE:
                depth -= 1
                if depth == 0: return
            elif t in SIMPLE: pass
            else: raise XError(f"token {t} in template at {self.p}")

    def obj(self, type_name: str, depth: int) -> XObj:
        if depth > MAX_DEPTH:
            raise XError("nesting too deep")
        o = XObj(type_name)
        t = self.peek()
        if t == T_NAME:
            self.u16(); o.name = self.name(); t = self.peek()
        if t == T_GUID:
            self.u16(); self.need(16); self.p += 16; t = self.peek()
        if self.u16() != T_OBRACE:
            raise XError(f"expected '{{' at {self.p}")
        while True:
            t = self.u16()
            if t == T_CBRACE:
                return o
            if t == T_INTLIST:
                c = self.u32(); self.need(4 * c)
                o.values += [("i", v) for v in struct.unpack_from(f"<{c}I", self.d, self.p)]; self.p += 4 * c
            elif t == T_FLTLIST:
                c = self.u32(); self.need(self.fsize * c)
                fmt = f"<{c}{'f' if self.fsize == 4 else 'd'}"
                o.values += [("f", v) for v in struct.unpack_from(fmt, self.d, self.p)]; self.p += self.fsize * c
            elif t == T_INTEGER:
                o.values.append(("i", self.u32()))
            elif t == T_STRING:
                o.values.append(("s", self.name())); self.u16()      # terminator ; or ,
            elif t == T_NAME:
                nm = self.name()
                if self.peek() == T_CBRACE and False:
                    pass
                o.children.append(self.obj(nm, depth + 1))
            elif t == T_OBRACE:                                        # reference { name }
                if self.u16() != T_NAME: raise XError("bad reference")
                o.refs.append(self.name())
                if self.u16() != T_CBRACE: raise XError("bad reference end")
            elif t in SIMPLE:
                pass
            else:
                raise XError(f"token {t} in data at {self.p - 2}")

    def parse(self) -> list[XObj]:
        top = []
        while self.p < self.n:
            t = self.u16()
            if t == T_TEMPLATE: self.skip_template()
            elif t == T_NAME: top.append(self.obj(self.name(), 0))
            else: raise XError(f"token {t} at top level {self.p - 2}")
        return top


def walk(objs):
    for o in objs:
        yield o
        yield from walk(o.children)


@dataclass
class Mesh:
    positions: list
    faces: list            # list of index lists (polygons)
    uvs: list | None
    normals: list | None
    face_material: list | None
    materials: list        # [(rgba, power, spec, emis, texture or None)]


def ints(vals):
    return [v for k, v in vals if k == "i"]


def floats(vals):
    return [v for k, v in vals if k == "f"]


def decode_mesh(m: XObj, named: dict) -> Mesh:
    iv, fv = ints(m.values), floats(m.values)
    nv = iv[0]
    if len(fv) < 3 * nv: raise XError("mesh vertex floats short")
    pos = [tuple(fv[3 * i:3 * i + 3]) for i in range(nv)]
    nf, k, faces = iv[1], 2, []
    for _ in range(nf):
        c = iv[k]; faces.append(iv[k + 1:k + 1 + c]); k += 1 + c
    if any(i >= nv for f in faces for i in f): raise XError("face index out of range")
    uvs = normals = fmat = None; mats = []
    for c in m.children:
        if c.type == "MeshTextureCoords":
            n = ints(c.values)[0]; f = floats(c.values); uvs = [tuple(f[2 * i:2 * i + 2]) for i in range(n)]
        elif c.type == "MeshNormals":
            n = ints(c.values)[0]; f = floats(c.values); normals = [tuple(f[3 * i:3 * i + 3]) for i in range(n)]
        elif c.type == "MeshMaterialList":
            ci = ints(c.values); nmat, nfi = ci[0], ci[1]; fmat = ci[2:2 + nfi]
            matobjs = [x for x in c.children if x.type == "Material"] + [named[r] for r in c.refs if r in named]
            for mo in matobjs:
                f = floats(mo.values)
                tex = next((s for x in mo.children if x.type == "TextureFilename" for k2, s in x.values if k2 == "s"), None)
                mats.append((tuple(f[0:4]), f[4], tuple(f[5:8]), tuple(f[8:11]), tex))
            if len(mats) != nmat: raise XError(f"materials {len(mats)} != {nmat}")
    return Mesh(pos, faces, uvs, normals, fmat, mats)


def load(data: bytes):
    top = Reader(data).parse()
    named = {o.name: o for o in walk(top) if o.name}
    return [decode_mesh(o, named) for o in walk(top) if o.type == "Mesh"], top
