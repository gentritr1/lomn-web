"""Reader for the CodeView NB11 debug block embedded in LEGOBionicle.exe (MSVC6, 2001).

Produces the facts every other decomp tool consumes:
  publics()       -> [(va, mangled_name)]                     S_PUB32 records from sstGlobalPub
  modules()       -> [Module(index, obj_name, text_ranges, source_files)]
  type_lines()    -> ["S name size", "M off tspec size name", ...]   class layouts (see TSPEC below)
  proc_lines()    -> ["F 0xVA", "V ebpoff tspec size name", ...]      params/locals of 580 procs

TSPEC grammar: prim:<c> | ptr:<tspec> | struct:<name> | arr:<tspec>:<count> | enum
Template instances share one stripped name in NB11 (e.g. 37 layouts named "ScAutoRefPtr"); each
distinct layout gets a unique name "<name>_<first pointee/member type>" (e.g. ScAutoRefPtr_GcMoveInterpreter).
"""
from __future__ import annotations

import collections
import struct
from dataclasses import dataclass, field

SST_MODULE, SST_SYMBOLS, SST_SRCMODULE, SST_GLOBALSYM, SST_GLOBALPUB, SST_GLOBALTYPES = 0x120, 0x125, 0x127, 0x129, 0x12A, 0x12B
S_PUB32_16T, S_PUB32, S_GDATA32 = 0x0203, 0x1009, 0x1008
S_BPREL32, S_LPROC32, S_GPROC32, S_END = 0x1006, 0x100A, 0x100B, 0x0006
SCOPE_OPENERS = (0x0207, 0x1103, 0x0206)  # block / with / thunk
LF_MODIFIER, LF_POINTER, LF_ARRAY, LF_CLASS, LF_STRUCTURE, LF_UNION, LF_ENUM = 0x1001, 0x1002, 0x1003, 0x1004, 0x1005, 0x1006, 0x1007
LF_PROCEDURE, LF_MFUNCTION, LF_BITFIELD = 0x1008, 0x1009, 0x1205
LF_BCLASS, LF_VBCLASS, LF_IVBCLASS, LF_INDEX, LF_MEMBER, LF_STMEMBER = 0x1400, 0x1401, 0x1402, 0x1404, 0x1405, 0x1406
LF_METHOD, LF_NESTTYPE, LF_VFUNCTAB, LF_ONEMETHOD, LF_VFUNCOFF, LF_ENUMERATE = 0x1407, 0x1408, 0x1409, 0x140B, 0x140C, 0x0403

PRIM = {0x00: "void", 0x03: "void", 0x10: "char", 0x20: "uchar", 0x11: "short", 0x21: "ushort", 0x12: "long",
        0x22: "ulong", 0x13: "longlong", 0x23: "ulonglong", 0x40: "float", 0x41: "double", 0x30: "bool",
        0x68: "char", 0x69: "uchar", 0x70: "char", 0x71: "wchar", 0x72: "short", 0x73: "ushort", 0x74: "int",
        0x75: "uint", 0x76: "longlong", 0x77: "ulonglong"}
PRIM_SIZE = {"void": 0, "char": 1, "uchar": 1, "short": 2, "ushort": 2, "long": 4, "ulong": 4, "longlong": 8,
             "ulonglong": 8, "float": 4, "double": 8, "bool": 1, "wchar": 2, "int": 4, "uint": 4}


class Nb11Error(Exception):
    pass


def read_numeric(buf: bytes, pos: int) -> tuple[int, int]:
    """CodeView numeric leaf: values < 0x8000 are inline; otherwise a typed value follows. Returns (value, next_pos)."""
    v = struct.unpack_from("<H", buf, pos)[0]
    if v < 0x8000:
        return v, pos + 2
    fmt = {0x8000: ("<b", 1), 0x8001: ("<h", 2), 0x8002: ("<H", 2), 0x8003: ("<i", 4), 0x8004: ("<I", 4)}.get(v)
    if fmt is None:
        raise Nb11Error(f"unsupported numeric leaf 0x{v:x} at 0x{pos:x}")
    return struct.unpack_from(fmt[0], buf, pos + 2)[0], pos + 2 + fmt[1]


def read_pascal(buf: bytes, pos: int) -> tuple[str, int]:
    n = buf[pos]
    return buf[pos + 1:pos + 1 + n].decode("latin1"), pos + 1 + n


@dataclass
class Module:
    index: int
    obj_name: str
    text_ranges: list[tuple[int, int]] = field(default_factory=list)  # (va, size) in .text
    source_files: list[str] = field(default_factory=list)            # from sstSrcModule, may be empty


class Nb11:
    def __init__(self, data: bytes):
        self.d = data
        if data[:2] != b"MZ":
            raise Nb11Error("not a PE file")
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe:pe + 4] != b"PE\0\0":
            raise Nb11Error("missing PE signature")
        nsec = struct.unpack_from("<H", data, pe + 6)[0]
        optsz = struct.unpack_from("<H", data, pe + 20)[0]
        opt = pe + 24
        self.image_base = struct.unpack_from("<I", data, opt + 28)[0]
        self.sections = []  # (name, va, vsize, raw_ptr, raw_size)
        for i in range(nsec):
            o = opt + optsz + i * 40
            name = data[o:o + 8].rstrip(b"\0").decode("latin1")
            vs, va, rs, rp = struct.unpack_from("<IIII", data, o + 8)
            self.sections.append((name, va, vs, rp, rs))
        dbg_rva, dbg_size = struct.unpack_from("<II", data, opt + 96 + 6 * 8)
        self.base = None
        o = self._rva_to_off(dbg_rva)
        for i in range(dbg_size // 28):
            _, _, _, _, typ, size, _, ptr = struct.unpack_from("<IIHHIIII", data, o + i * 28)
            if typ == 2 and data[ptr:ptr + 4] == b"NB11":
                self.base, self.cv_size = ptr, size
        if self.base is None:
            raise Nb11Error("no CodeView NB11 block")
        dh = self.base + struct.unpack_from("<I", data, self.base + 4)[0]
        cb_hdr, cb_ent, n_dir = struct.unpack_from("<HHI", data, dh)[:3]
        self.dir = [struct.unpack_from("<HHII", data, dh + cb_hdr + i * cb_ent) for i in range(n_dir)]
        self._types = None

    @classmethod
    def from_file(cls, path) -> "Nb11":
        with open(path, "rb") as f:
            return cls(f.read())

    def _rva_to_off(self, rva: int) -> int:
        for _, va, vs, rp, rs in self.sections:
            if va <= rva < va + max(vs, rs):
                return rva - va + rp
        raise Nb11Error(f"rva 0x{rva:x} outside sections")

    def _seg_va(self, seg: int, off: int) -> int | None:
        if 0 < seg <= len(self.sections):
            return self.image_base + self.sections[seg - 1][1] + off
        return None

    def _subsections(self, sst: int):
        return [(imod, self.base + lfo, cb) for t, imod, lfo, cb in self.dir if t == sst]

    # ---- publics -------------------------------------------------------------------------------
    def publics(self) -> list[tuple[int, str]]:
        """Public symbols plus global data symbols (S_GDATA32 in sstGlobalSym), de-duplicated by (va, name)."""
        out = set()
        for _, p, _ in self._subsections(SST_GLOBALPUB) + self._subsections(SST_GLOBALSYM):
            cb_symbol = struct.unpack_from("<I", self.d, p + 4)[0]
            q, end = p + 16, p + 16 + cb_symbol
            while q < end:
                reclen, rectyp = struct.unpack_from("<HH", self.d, q)
                if rectyp in (S_PUB32, S_GDATA32):
                    _, off, seg = struct.unpack_from("<IIH", self.d, q + 4)
                    name, _ = read_pascal(self.d, q + 14)
                elif rectyp == S_PUB32_16T:
                    off, seg, _ = struct.unpack_from("<IHH", self.d, q + 4)
                    name, _ = read_pascal(self.d, q + 12)
                else:
                    q += reclen + 2
                    continue
                va = self._seg_va(seg, off)
                if va is not None:
                    out.add((va, name))
                q += reclen + 2
        for _, p, cb in self._subsections(SST_SYMBOLS):  # per-module symbols: 4-byte signature, then records
            q, end = p + 4, p + cb
            while q < end:
                reclen, rectyp = struct.unpack_from("<HH", self.d, q)
                if rectyp == S_GDATA32:
                    _, off, seg = struct.unpack_from("<IIH", self.d, q + 4)
                    name, _ = read_pascal(self.d, q + 14)
                    va = self._seg_va(seg, off)
                    if va is not None:
                        out.add((va, name))
                q += reclen + 2
        return sorted(out)

    # ---- modules ---------------------------------------------------------------------------------
    def modules(self) -> list[Module]:
        mods = {}
        for imod, p, _ in self._subsections(SST_MODULE):
            _, _, cseg, _ = struct.unpack_from("<HHHH", self.d, p)
            segs = [struct.unpack_from("<HHII", self.d, p + 8 + i * 12) for i in range(cseg)]
            name, _ = read_pascal(self.d, p + 8 + cseg * 12)
            text_seg = 1 + next(i for i, s in enumerate(self.sections) if s[0] == ".text")
            ranges = [(self._seg_va(s, off), cb) for s, _, off, cb in segs if s == text_seg and cb]
            mods[imod] = Module(imod, name, ranges)
        for imod, p, _ in self._subsections(SST_SRCMODULE):
            c_file, _ = struct.unpack_from("<HH", self.d, p)
            for b in struct.unpack_from(f"<{c_file}I", self.d, p + 4):
                q = p + b
                cs = struct.unpack_from("<H", self.d, q)[0]
                name, _ = read_pascal(self.d, q + 4 + 4 * cs + 8 * cs)
                if imod in mods:
                    mods[imod].source_files.append(name)
        return [mods[k] for k in sorted(mods)]

    # ---- types -------------------------------------------------------------------------------------
    def _load_types(self):
        if self._types is not None:
            return
        subs = self._subsections(SST_GLOBALTYPES)
        if len(subs) != 1:
            raise Nb11Error("expected exactly one sstGlobalTypes")
        _, p, _ = subs[0]
        count = struct.unpack_from("<I", self.d, p + 4)[0]
        offs = struct.unpack_from(f"<{count}I", self.d, p + 8)
        tb = p + 8 + 4 * count
        self._types = {0x1000 + i: tb + o for i, o in enumerate(offs)}

    def _leaf(self, ti: int) -> tuple[int, int]:
        q = self._types[ti]
        return struct.unpack_from("<H", self.d, q + 2)[0], q + 4

    def _class_header(self, ti: int):
        lf, q = self._leaf(ti)
        if lf == LF_UNION:
            _, prop, fld = struct.unpack_from("<HHI", self.d, q)
            q2 = q + 8
        else:
            _, prop, fld, _, _ = struct.unpack_from("<HHIII", self.d, q)
            q2 = q + 16
        size, q2 = read_numeric(self.d, q2)
        name, _ = read_pascal(self.d, q2)
        return name, size, fld

    def _fields(self, fld: int) -> list[tuple[int, int | None, str]]:
        """(offset, type_index | None for vftable, member_name | 'base_') following LF_INDEX continuations."""
        out = []
        d = self.d
        while fld:
            q = self._types[fld]
            end = q + 2 + struct.unpack_from("<H", d, q)[0]
            r, nxt = q + 4, 0
            while r < end:
                if d[r] >= 0xF0:  # LF_PADx
                    r += d[r] & 0xF
                    continue
                sub = struct.unpack_from("<H", d, r)[0]
                if sub == LF_MEMBER:
                    _, ty = struct.unpack_from("<HI", d, r + 2)
                    off, r2 = read_numeric(d, r + 8)
                    name, r = read_pascal(d, r2)
                    out.append((off, ty, name))
                elif sub == LF_BCLASS:
                    _, ty = struct.unpack_from("<HI", d, r + 2)
                    off, r = read_numeric(d, r + 8)
                    out.append((off, ty, "base_"))
                elif sub == LF_VFUNCTAB:
                    out.append((0, None, "vftable"))
                    r += 8
                elif sub in (LF_STMEMBER, LF_NESTTYPE):
                    r = read_pascal(d, r + 8)[1]
                elif sub == LF_METHOD:
                    r = read_pascal(d, r + 10)[1]
                elif sub == LF_ONEMETHOD:
                    attr = struct.unpack_from("<H", d, r + 2)[0]
                    intro = ((attr >> 2) & 7) in (4, 6)
                    r = read_pascal(d, r + 8 + (4 if intro else 0))[1]
                elif sub == LF_INDEX:
                    nxt = struct.unpack_from("<I", d, r + 4)[0]
                    r += 8
                elif sub in (LF_VBCLASS, LF_IVBCLASS):
                    _, r = read_numeric(d, r + 10)
                    _, r = read_numeric(d, r)
                elif sub == LF_VFUNCOFF:
                    r += 10
                elif sub == LF_ENUMERATE:
                    _, r2 = read_numeric(d, r + 4)
                    r = read_pascal(d, r2)[1]
                else:
                    break
            fld = nxt
        return out

    def _pointee_name(self, ty, depth=0) -> str:
        if ty is None or ty < 0x1000:
            return PRIM.get((ty or 0) & 0xFF, "x")
        lf, q = self._leaf(ty)
        if lf == LF_MODIFIER:
            return self._pointee_name(struct.unpack_from("<I", self.d, q)[0], depth)
        if lf == LF_POINTER:
            return "P" + self._pointee_name(struct.unpack_from("<I", self.d, q)[0], depth + 1) if depth < 3 else "P"
        if lf in (LF_CLASS, LF_STRUCTURE, LF_UNION):
            return self._class_header(ty)[0]
        return "x"

    def _assign_names(self):
        """Give every distinct class layout a unique name; returns (uname: ti->name, layouts: name->ti)."""
        defs = collections.defaultdict(list)
        for ti in sorted(self._types):
            if self._leaf(ti)[0] in (LF_CLASS, LF_STRUCTURE, LF_UNION):
                name, size, fld = self._class_header(ti)
                if fld and size:
                    defs[name].append(ti)
        uname, layouts = {}, {}
        for name, tis in defs.items():
            sigs = {}
            for ti in tis:
                _, size, fld = self._class_header(ti)
                sig = (size, tuple((o, n, self._pointee_name(t)) for o, t, n in self._fields(fld)))
                sigs.setdefault(sig, []).append(ti)
            for group in sigs.values():
                if len(sigs) == 1:
                    u = name
                else:
                    arg = next((self._pointee_name(t)[1:] if self._pointee_name(t).startswith("P") else self._pointee_name(t)
                                for o, t, n in self._fields(self._class_header(group[0])[2]) if t and n != "base_"), "x")
                    u = f"{name}_{arg or 'x'}"
                k, i = u, 2
                while k in layouts:
                    k, i = f"{u}_{i}", i + 1
                layouts[k] = group[0]
                for ti in group:
                    uname[ti] = k
        return uname, layouts

    def _spec(self, ti, uname, layouts, byname, depth=0) -> tuple[str, int]:
        if ti is None:
            return "ptr:prim:void", 4
        if ti < 0x1000:
            prim = PRIM.get(ti & 0xFF, "uint")
            if (ti >> 8) & 7:
                return "ptr:prim:" + prim, 4
            return "prim:" + prim, PRIM_SIZE.get(prim, 4)
        lf, q = self._leaf(ti)
        if lf == LF_MODIFIER:
            return self._spec(struct.unpack_from("<I", self.d, q)[0], uname, layouts, byname, depth)
        if lf == LF_POINTER:
            if depth > 4:
                return "ptr:prim:void", 4
            inner, _ = self._spec(struct.unpack_from("<I", self.d, q)[0], uname, layouts, byname, depth + 1)
            return "ptr:" + inner, 4
        if lf in (LF_CLASS, LF_STRUCTURE, LF_UNION):
            name, size, _ = self._class_header(ti)
            if ti in uname:
                return "struct:" + uname[ti], size
            cands = byname.get(name, [])
            if len(cands) == 1:
                return "struct:" + cands[0], self._class_header(layouts[cands[0]])[1]
            return ("struct:" + name if cands else "prim:void"), size
        if lf == LF_ENUM:
            return "enum", 4
        if lf == LF_ARRAY:
            el, _ = struct.unpack_from("<II", self.d, q)
            size, _ = read_numeric(self.d, q + 8)
            et, es = self._spec(el, uname, layouts, byname, depth)
            return (f"arr:{et}:{size // es}" if es else f"arr:prim:uchar:{size}"), size
        if lf in (LF_PROCEDURE, LF_MFUNCTION):
            return "prim:void", 0
        return "prim:uint", 4

    def type_lines(self) -> list[str]:
        self._load_types()
        uname, layouts = self._assign_names()
        byname = collections.defaultdict(list)
        for k, ti in layouts.items():
            byname[self._class_header(ti)[0]].append(k)
        out = []
        for k, ti in layouts.items():
            _, size, fld = self._class_header(ti)
            out.append(f"S {k} {size}")
            for off, ty, name in self._fields(fld):
                if name == "vftable":
                    out.append("M 0 ptr:prim:void 4 vftable")
                    continue
                t, s = self._spec(ty, uname, layouts, byname)
                if name == "base_":
                    name = "base_" + t.split(":")[-1]
                out.append(f"M {off} {t} {s} {name}")
        self._uname_cache = (uname, layouts, byname)
        return out

    def proc_lines(self) -> list[str]:
        if not hasattr(self, "_uname_cache"):
            self.type_lines()
        uname, layouts, byname = self._uname_cache
        out = []
        for _, p, cb in self._subsections(SST_SYMBOLS):
            q, end, depth, inproc = p + 4, p + cb, 0, False
            while q < end:
                reclen, rectyp = struct.unpack_from("<HH", self.d, q)
                body = q + 4
                if rectyp in (S_LPROC32, S_GPROC32):
                    _, _, _, _, _, _, _, off, seg = struct.unpack_from("<IIIIIIIIH", self.d, body)
                    va = self._seg_va(seg, off)
                    if depth == 0 and va is not None:
                        out.append(f"F 0x{va:x}")
                        inproc = True
                    depth += 1
                elif rectyp in SCOPE_OPENERS:
                    depth += 1
                elif rectyp == S_END:
                    depth = max(0, depth - 1)
                    if depth == 0:
                        inproc = False
                elif rectyp == S_BPREL32 and inproc:
                    off, ti = struct.unpack_from("<iI", self.d, body)
                    name, _ = read_pascal(self.d, body + 8)
                    if name != "this" and not name.startswith("@"):
                        t, s = self._spec(ti, uname, layouts, byname)
                        out.append(f"V {off} {t} {s} {name}")
                q += reclen + 2
        return out

    # ---- imports -------------------------------------------------------------------------------------
    def imports(self) -> dict[str, list[str]]:
        """{dll: [function name or '#ordinal']} from the PE import directory."""
        pe = struct.unpack_from("<I", self.d, 0x3C)[0]
        opt = pe + 24
        o = self._rva_to_off(struct.unpack_from("<I", self.d, opt + 96 + 8)[0])
        out = {}
        while True:
            oft, _, _, name_rva, ft = struct.unpack_from("<IIIII", self.d, o)
            o += 20
            if not name_rva:
                break
            dll = self.d[self._rva_to_off(name_rva):].split(b"\0", 1)[0].decode("latin1")
            t, fns = self._rva_to_off(oft or ft), []
            while (v := struct.unpack_from("<I", self.d, t)[0]):
                t += 4
                if v & 0x80000000:
                    fns.append(f"#{v & 0xFFFF}")
                else:
                    fns.append(self.d[self._rva_to_off(v) + 2:].split(b"\0", 1)[0].decode("latin1"))
            out[dll] = fns
        return out


def main(argv: list[str]) -> int:
    """nb11.py <exe> <out_dir>: write symbols.txt, types.txt (S/M + F/V lines), modules.json."""
    import json
    from pathlib import Path
    exe, out = Path(argv[0]), Path(argv[1])
    out.mkdir(parents=True, exist_ok=True)
    n = Nb11.from_file(exe)
    pubs = n.publics()
    (out / "symbols.txt").write_text("".join(f"0x{va:x} {name}\n" for va, name in pubs))
    (out / "types.txt").write_text("\n".join(n.type_lines() + n.proc_lines()) + "\n")
    (out / "modules.json").write_text(json.dumps(
        [{"index": m.index, "obj_name": m.obj_name, "text_ranges": m.text_ranges, "source_files": m.source_files}
         for m in n.modules()], indent=0))
    print(f"symbols {len(pubs)}; modules {len(n.modules())}")
    return 0


if __name__ == "__main__":
    import sys
    sys.exit(main(sys.argv[1:]))
