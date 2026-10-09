"""M2a-2 T1: decoder for the exe's `.exc` exception tables (read as data; the exe is never run).

Written from docs/osi/opcodes.md "Exception tables". Layout:
- The `.exc` section is an index of `{u32 function start, u32 info pointer}` pairs ending at `0xffffffff`.
- A function's info record is `u8 flags, u32 action-table pointer, u16 n`, then `n x {u32 call-instruction address,
  u32 action-chain pointer}`. A call that is not listed has no actions.
- An info record is only 4 bytes long, and has no call-site table, when flags bit 0 is set. Measured here on the
  canonical exe (the first correction to the document): 361 more index entries have bit 0 clear and are 4 bytes too.
  In every short record bytes 1..3 are zero, so a null action-table pointer is also "short". Without this a decoder
  reads the next record's bytes as call sites.
- An action chain runs to the first record whose flags have bit 0x8000. The kind is `flags & 0xff`, as the runtime
  reads it (FUN_00656880 at 0x656893, FUN_00656d60 at 0x656d99); no record in the canonical exe sets bits 0x7f00.
- Record sizes are the runtime's own (M2a-2 T2a, R2): the unwinder FUN_00656d60 (jump table 0x761dc4) and the walker
  FUN_006568a0 (table 0x761d64) advance by the same amount per kind:
  0x01 local dtor {i32 frame, dtor} 10; 0x02 conditional local dtor {i32 frame, i32 flag frame, dtor} 14;
  0x04 local-pointer dtor {i32 frame, dtor} 10; 0x05 local array {i32 frame, dtor, u32 count, u32 size} 18;
  0x06 local array via pointers {i32 x4} 18; 0x07 member dtor {i32 frame, dtor, u32 offset} 14;
  0x08 conditional member dtor {i32 frame, i32 flag frame, dtor, u32 offset} 18;
  0x09 member array {i32 frame, dtor, u32 offset, u32 count, u32 size} 22; 0x0a delete {i32 frame, operator delete} 10;
  0x0b conditional delete {i32 frame, operator delete, i32 flag frame} 14; 0x0c base dtor {i32 frame, dtor, u32
  offset} 14; 0x10 catch {type-name ptr or 0, landing pad, i32 frame} 14; 0x11 end of an active catch {i32 frame} 6;
  0x12 terminate (std::terminate, 0x656ef1; only as a final record, 2 in the runtime's own functions) 2;
  0x13 exception specification {u16 count, unexpected-handler pc, i32 frame, count x type-name ptr} 12 + 4 count.
- **Only kind 0x10 catches**: the catch search FUN_00657080 sends kind 0x10 to ___throw_catch_compare (table 0x761e50,
  call 0x65716f), 0x13 to the specification check (0x65718b), kinds 0x03, 0x0d-0x0f and 0x12 to std::terminate, and
  skips every other kind. A kind outside the table (0x00, 0x03, 0x0d-0x0f, > 0x13) cannot be sized: it is reported as
  `unknown`, the chain is not decoded further, and the function line says `truncated`.

Usage:
  exc.py --func ADDR [--exe EXE]          one function (ADDR hex, or a name from gen/decomp/functions.jsonl)
  exc.py --chain ADDR [ADDR ...]          several functions, in order
The exe defaults to $LOMN_DATA/reference/LEGOBionicle.exe. One line per record; the last line of a function is
`catches N pads 0x... 0x...` (the distinct landing pads of its catch records, ascending) or `catches 0`.
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

END_BIT = 0x8000
SIZES = {0x01: 10, 0x02: 14, 0x04: 10, 0x05: 18, 0x06: 18, 0x07: 14, 0x08: 18, 0x09: 22, 0x0A: 10, 0x0B: 14,
         0x0C: 14, 0x10: 14, 0x11: 6, 0x12: 2}
KIND_NAMES = {0x01: "dtor", 0x02: "dtor-cond", 0x04: "dtor-ptr", 0x05: "dtor-array", 0x06: "dtor-array-ptr",
              0x07: "dtor-member", 0x08: "dtor-member-cond", 0x09: "dtor-member-array", 0x0A: "delete",
              0x0B: "delete-cond", 0x0C: "dtor-base", 0x10: "catch", 0x11: "endcatch", 0x12: "terminate", 0x13: "spec"}


class Image:
    """PE32 sections as data: read(va, n) -> bytes. `regions` (tests) is {va: bytes}."""

    def __init__(self, regions: list[tuple[int, bytes]], sections: dict | None = None):
        self.regions = regions
        self.sections = sections or {}          # section name -> virtual address (from the PE header)

    @classmethod
    def from_pe(cls, data: bytes) -> "Image":
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe:pe + 4] != b"PE\0\0":
            raise ValueError("not a PE file")
        nsec = struct.unpack_from("<H", data, pe + 6)[0]
        optsz = struct.unpack_from("<H", data, pe + 20)[0]
        base = struct.unpack_from("<I", data, pe + 24 + 28)[0]
        regions, sections = [], {}
        for i in range(nsec):
            o = pe + 24 + optsz + 40 * i
            name = data[o:o + 8].rstrip(b"\0").decode("latin-1")
            vsize, rva, rawsize, rawptr = struct.unpack_from("<IIII", data, o + 8)
            n = min(vsize, rawsize) if vsize else rawsize
            regions.append((base + rva, data[rawptr:rawptr + n]))
            sections.setdefault(name, base + rva)
        return cls(regions, sections)

    def section_va(self, name: str) -> int:
        if name not in self.sections:
            raise ValueError("no %s section in this image" % name)
        return self.sections[name]

    @classmethod
    def from_file(cls, path) -> "Image":
        return cls.from_pe(Path(path).read_bytes())

    def has(self, va: int, n: int = 1) -> bool:
        return any(s <= va and va + n <= s + len(b) for s, b in self.regions)

    def read(self, va: int, n: int) -> bytes:
        for s, b in self.regions:
            if s <= va and va + n <= s + len(b):
                return b[va - s:va - s + n]
        raise ValueError("VA %#x (+%d) is not file-backed" % (va, n))

    def u16(self, va: int) -> int:
        return struct.unpack("<H", self.read(va, 2))[0]

    def u32(self, va: int) -> int:
        return struct.unpack("<I", self.read(va, 4))[0]

    def i32(self, va: int) -> int:
        return struct.unpack("<i", self.read(va, 4))[0]

    def cstr(self, va: int, limit: int = 128) -> str:
        out = bytearray()
        while len(out) < limit and self.has(va + len(out)):
            c = self.read(va + len(out), 1)[0]
            if c == 0:
                break
            out.append(c)
        return out.decode("latin-1")


def load_index(img: Image, start: int | None = None) -> dict[int, int]:
    """{function start: info pointer}, up to the 0xffffffff terminator. The index is the `.exc` section, found by name in
    the PE section table (0x6ef000 in the canonical exe); an image without one raises ValueError."""
    idx = {}
    va = img.section_va(".exc") if start is None else start
    while img.has(va, 8):
        f, i = struct.unpack("<II", img.read(va, 8))
        if f == 0xFFFFFFFF:
            break
        idx[f] = i
        va += 8
    return idx


def decode_chain(img: Image, ptr: int, limit: int = 400) -> list[dict]:
    """The records of one action chain, to the first with bit 0x8000. A record of unknown kind ends the decoding
    (its size is unknown): it is returned as {'kind': k, 'unknown': True} and the chain is truncated there."""
    recs = []
    p = ptr
    for _ in range(limit):
        if not img.has(p, 2):
            recs.append({"addr": p, "kind": -1, "unknown": True, "end": False, "raw": ""})
            break
        flags = img.u16(p)
        kind = flags & 0xFF
        end = bool(flags & END_BIT)
        rec = {"addr": p, "flags": flags, "kind": kind, "end": end}
        if kind == 0x13:
            count = img.u16(p + 2)
            size = 12 + 4 * count
            rec.update(count=count, unexpected=img.u32(p + 4), frame=img.i32(p + 8),
                       types=[img.cstr(img.u32(p + 12 + 4 * k)) if img.u32(p + 12 + 4 * k) else ""
                              for k in range(count)])
        elif kind in SIZES:
            size = SIZES[kind]
            if kind == 0x10:
                tp = img.u32(p + 2)
                rec.update(type_ptr=tp, type=img.cstr(tp) if tp else "", pad=img.u32(p + 6), frame=img.i32(p + 10))
            elif kind in (0x01, 0x04, 0x0A):
                rec.update(frame=img.i32(p + 2), dtor=img.u32(p + 6))
            elif kind in (0x07, 0x0C):
                rec.update(frame=img.i32(p + 2), dtor=img.u32(p + 6), offset=img.u32(p + 10))
            elif kind == 0x02:
                rec.update(frame=img.i32(p + 2), cond=img.i32(p + 6), dtor=img.u32(p + 10))
            elif kind == 0x05:
                rec.update(frame=img.i32(p + 2), dtor=img.u32(p + 6), count=img.u32(p + 10), elem=img.u32(p + 14))
            elif kind == 0x06:
                rec.update(frame=img.i32(p + 2), raw=img.read(p + 6, 12).hex())
            elif kind == 0x08:
                rec.update(frame=img.i32(p + 2), cond=img.i32(p + 6), dtor=img.u32(p + 10), offset=img.u32(p + 14))
            elif kind == 0x09:
                rec.update(frame=img.i32(p + 2), dtor=img.u32(p + 6), offset=img.u32(p + 10), count=img.u32(p + 14),
                           elem=img.u32(p + 18))
            elif kind == 0x0B:
                rec.update(frame=img.i32(p + 2), dtor=img.u32(p + 6), cond=img.i32(p + 10))
            elif kind == 0x11:
                rec.update(frame=img.i32(p + 2))
            # 0x12 (terminate) has no fields
        else:
            n = 16 if img.has(p, 16) else 2
            rec.update(unknown=True, raw=img.read(p, n).hex())
            recs.append(rec)
            break
        rec["size"] = size
        recs.append(rec)
        if end:
            break
        p += size
    return recs


def decode_function(img: Image, func: int, index: dict[int, int] | None = None) -> dict:
    """{'func', 'info', 'flags', 'short', 'table', 'sites': [{'call', 'chain', 'records'}], 'catches': [...]}"""
    index = index if index is not None else load_index(img)
    if func not in index:
        return {"func": func, "info": None}
    info = index[func]
    b = img.read(info, 7) if img.has(info, 7) else img.read(info, 4)
    flags = b[0]
    out = {"func": func, "info": info, "flags": flags, "sites": [], "catches": [], "truncated": 0}
    if flags & 1 or len(b) < 7 or b[1:4] == b"\0\0\0":
        out["short"] = True
        return out
    out["short"] = False
    out["table"] = struct.unpack_from("<I", b, 1)[0]
    n = struct.unpack_from("<H", b, 5)[0]
    out["n"] = n
    seen = {}
    for k in range(n):
        call, chain = struct.unpack("<II", img.read(info + 7 + 8 * k, 8))
        recs = decode_chain(img, chain)
        out["sites"].append({"call": call, "chain": chain, "records": recs})
        for r in recs:
            if r.get("unknown"):
                out["truncated"] += 1
            elif r["kind"] == 0x10:
                seen[r["addr"]] = r
    out["catches"] = sorted(seen.values(), key=lambda r: (r["pad"], r["addr"]))
    return out


def caught_calls(img: Image, index: dict[int, int] | None = None) -> list[dict]:
    """Every call site inside a catch, over the whole `.exc` index: the sites whose action chain holds a kind-0x10
    record, each owned by the function whose table lists it (M2a-2 T2a fix round 1). No function size is used, so a
    census built on this cannot drop a site that a decompiler's short `size` leaves outside its function (C-1:
    GcAreaLoader::LoadArea's third block at 0x500c0b lies past the decomp's 7,431 bytes). Each entry is
    {'func', 'call', 'kind', 'target'}: kind 'direct' (E8 rel32; target = the callee), 'indirect' (FF /2; target None)
    or 'other' (any other bytes). Sorted by call address."""
    index = index if index is not None else load_index(img)
    out = []
    for f in index:
        d = decode_function(img, f, index)
        if d["info"] is None or d["short"]:
            continue
        for s in d["sites"]:
            if not any(r.get("kind") == 0x10 and not r.get("unknown") for r in s["records"]):
                continue
            a = s["call"]
            kind, target = "other", None
            if img.has(a, 5) and img.read(a, 1) == b"\xe8":
                kind, target = "direct", (a + 5 + img.i32(a + 1)) & 0xFFFFFFFF
            elif img.has(a, 2) and img.read(a, 1) == b"\xff" and (img.read(a + 1, 1)[0] >> 3) & 7 == 2:
                kind = "indirect"
            out.append({"func": f, "call": a, "kind": kind, "target": target})
    return sorted(out, key=lambda c: c["call"])


def _s(n: int) -> str:
    return "%d" % n


def format_function(d: dict) -> list[str]:
    f = d["func"]
    if d["info"] is None:
        return ["func 0x%08x no .exc entry" % f]
    if d["short"]:
        return ["func 0x%08x info 0x%08x flags 0x%02x short (4-byte record, no call-site table)" % (f, d["info"], d["flags"]),
                "catches 0"]
    lines = ["func 0x%08x info 0x%08x flags 0x%02x table 0x%08x sites %d" % (f, d["info"], d["flags"], d["table"],
                                                                          d["n"])]
    for s in d["sites"]:
        lines.append("site 0x%08x chain 0x%08x" % (s["call"], s["chain"]))
        for r in s["records"]:
            lines.append("  " + format_record(r))
    pads = sorted({r["pad"] for r in d["catches"]})
    tail = "catches %d" % len(pads)
    if pads:
        tail += " pads " + " ".join("0x%08x" % p for p in pads)
    if d["truncated"]:
        tail += " truncated %d" % d["truncated"]
    lines.append(tail)
    return lines


def format_record(r: dict) -> str:
    end = " end" if r.get("end") else ""
    if r.get("unknown"):
        return "rec 0x%08x unknown kind 0x%02x raw %s (chain not decoded further)" % (r["addr"], r["kind"] & 0xFF, r.get("raw", ""))
    k = r["kind"]
    head = "rec 0x%08x %s" % (r["addr"], KIND_NAMES[k])
    if k == 0x10:
        ty = ' type "%s"' % r["type"] if r["type_ptr"] else " type ..."
        return "%s%s pad 0x%08x frame %s%s" % (head, ty, r["pad"], _s(r["frame"]), end)
    if k in (0x01, 0x04, 0x0A):
        return "%s fn 0x%08x frame %s%s" % (head, r["dtor"], _s(r["frame"]), end)
    if k in (0x07, 0x0C):
        return "%s fn 0x%08x frame %s offset %d%s" % (head, r["dtor"], _s(r["frame"]), r["offset"], end)
    if k in (0x02, 0x0B):
        return "%s fn 0x%08x frame %s cond %s%s" % (head, r["dtor"], _s(r["frame"]), _s(r["cond"]), end)
    if k == 0x05:
        return "%s fn 0x%08x frame %s count %d size %d%s" % (head, r["dtor"], _s(r["frame"]), r["count"], r["elem"], end)
    if k == 0x08:
        return "%s fn 0x%08x frame %s cond %s offset %d%s" % (head, r["dtor"], _s(r["frame"]), _s(r["cond"]),
                                                              r["offset"], end)
    if k == 0x09:
        return "%s fn 0x%08x frame %s offset %d count %d size %d%s" % (head, r["dtor"], _s(r["frame"]), r["offset"],
                                                                        r["count"], r["elem"], end)
    if k == 0x11:
        return "%s frame %s%s" % (head, _s(r["frame"]), end)
    if k == 0x12:
        return "%s%s" % (head, end)
    if k == 0x13:
        return "%s unexpected 0x%08x frame %s types %s%s" % (head, r["unexpected"], _s(r["frame"]),
                                                              ",".join('"%s"' % t for t in r["types"]) or "-", end)
    if k == 0x06:
        return "%s frame %s raw %s%s" % (head, _s(r["frame"]), r["raw"], end)
    return "%s raw %s%s" % (head, r["raw"], end)


def resolve(token: str, names: dict[str, int]) -> int:
    try:
        return int(token, 0)
    except ValueError:
        if token in names:
            return names[token]
        raise SystemExit("exc.py: %r is neither an address nor a name in gen/decomp/functions.jsonl" % token)


def load_names() -> dict[str, int]:
    p = REPO / "gen" / "decomp" / "functions.jsonl"
    names: dict[str, int] = {}
    if p.exists():
        for ln in p.read_text().splitlines():
            r = json.loads(ln)
            names.setdefault(r["name"], int(r["addr"], 16))
    return names


def default_exe() -> Path:
    d = os.environ.get("LOMN_DATA")
    if not d:
        raise SystemExit("exc.py: pass --exe or set LOMN_DATA")
    return Path(d) / "reference" / "LEGOBionicle.exe"


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--exe")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--func", metavar="ADDR")
    g.add_argument("--chain", nargs="+", metavar="ADDR")
    a = ap.parse_args(argv)
    img = Image.from_file(a.exe or default_exe())
    index = load_index(img)
    toks = [a.func] if a.func else a.chain
    names = load_names() if any(not t.lower().startswith("0x") for t in toks) else {}
    rc = 0
    for t in toks:
        f = resolve(t, names)
        d = decode_function(img, f, index)
        if d["info"] is None:
            rc = 1
        print("\n".join(format_function(d)))
    return rc


if __name__ == "__main__":
    sys.exit(main())
