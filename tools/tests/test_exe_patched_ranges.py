"""M2a-2 T12 fix b (review M-8): the 798 bytes the reference exe (tools/builds.json "canonical", 09b55e62...) was hand-patched
after linking, against the install exe (b372afb9..., the authoritative copy for behaviour and citations).

(a) The patched ranges are computed from the two exes (file bytes, mapped to virtual addresses through the PE section table) and
    must equal the pinned list. Runs closer than 9 bytes are one range (a patch is a few instructions with untouched bytes between
    the changed ones); the pinned list is the 16 maximal byte runs merged that way: 7 ranges in 6 .text functions.
(b) No address cited in the natives doc, the override table, the C++ OSI sources or the Python natives falls inside a patched
    range unless the SAME line names the copy ("install" or "reference"). Fail closed: every 0x-hex token of 5 to 8 digits counts,
    and a written range "0xA-0xB" counts when it overlaps one.
Needs LOMN_DATA (skips otherwise)."""
import os
import re
import struct
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

PATCHED = [(0x4669A1, 0x466AB0), (0x488ED2, 0x488EE0), (0x5ED019, 0x5ED01E), (0x5ED0C5, 0x5ED0C6),
           (0x6448E6, 0x6448EE), (0x6449A9, 0x644A7B), (0x64D3B9, 0x64D4EC)]               # [start, end)
FUNCTIONS = {0x4669A0: "GcSurfaceOctree::GetLeaf", 0x488E80: "GcAreaDirector::Hide", 0x5ECD30: "ScPrinter::DoPrint",
             0x6448B0: "GcMudBoss::Reset", 0x6449A0: "GcMudBoss::CleanUp", 0x64D3B0: "GcRockBoss::CleanUp"}
GAP = 9
SOURCES = ["docs/osi/natives.md", "data/osi/native_overrides.json", "engine/src/osi", "tools/research/osi_natives"]
HEX = re.compile(r"0x([0-9a-fA-F]{5,8})\b")
RANGE = re.compile(r"0x([0-9a-fA-F]{5,8})\s*(?:-|–|\.\.)\s*0x([0-9a-fA-F]{5,8})")
COPY = re.compile(r"\b(install|reference)\b")


def patched_ranges(a: bytes, b: bytes):
    assert len(a) == len(b)
    pe = struct.unpack_from("<I", a, 0x3C)[0]
    nsec, optsz = struct.unpack_from("<H", a, pe + 6)[0], struct.unpack_from("<H", a, pe + 20)[0]
    base = struct.unpack_from("<I", a, pe + 24 + 28)[0]
    secs = []
    for i in range(nsec):
        o = pe + 24 + optsz + 40 * i
        vsize, rva, rawsize, rawptr = struct.unpack_from("<IIII", a, o + 8)
        if rawsize:
            secs.append((rawptr, rawsize, base + rva))

    def va(off):
        return next(v + off - rp for rp, rs, v in secs if rp <= off < rp + rs)

    runs, i = [], 0
    while i < len(a):
        if a[i] != b[i]:
            j = i
            while j < len(a) and a[j] != b[j]:
                j += 1
            runs.append((va(i), va(j - 1) + 1))
            i = j
        else:
            i += 1
    out = []
    for s, e in runs:
        if out and s - out[-1][1] < GAP:
            out[-1] = (out[-1][0], e)
        else:
            out.append((s, e))
    return out, sum(1 for x, y in zip(a, b) if x != y)


def inside(lo, hi=None):
    hi = lo if hi is None else hi
    return any(lo < e and hi >= s for s, e in PATCHED)


def source_files():
    for rel in SOURCES:
        p = REPO / rel
        yield from ([p] if p.is_file() else sorted(q for q in p.rglob("*") if q.is_file() and q.suffix in (".py", ".cpp", ".h", ".hpp", ".md", ".json")))


def test_patched_ranges_match_the_pinned_list(lomn_data):
    a = (lomn_data / "install" / "LEGOBionicle.exe").read_bytes()
    b = (lomn_data / "reference" / "LEGOBionicle.exe").read_bytes()
    ranges, nbytes = patched_ranges(a, b)
    assert ranges == PATCHED and nbytes == 798
    owner = {s: f for s, f in FUNCTIONS.items()}
    starts = sorted(owner)
    for s, e in ranges:                                                         # each range sits in one of the 6 functions
        fn = max(x for x in starts if x <= s)
        assert e <= {**{x: y for x, y in zip(starts, starts[1:] + [1 << 32])}}[fn], (hex(s), hex(e))


def test_no_cited_address_falls_inside_a_patched_range_unless_the_line_names_the_copy():
    bad = []
    seen = 0
    for p in source_files():
        for n, line in enumerate(p.read_text(errors="replace").splitlines(), 1):
            hits = [int(m.group(1), 16) for m in HEX.finditer(line)]
            spans = [(int(m.group(1), 16), int(m.group(2), 16)) for m in RANGE.finditer(line)]
            seen += len(hits)
            if (any(inside(h) for h in hits) or any(lo <= hi and inside(lo, hi) for lo, hi in spans)) and not COPY.search(line):
                bad.append("%s:%d: %s" % (p.relative_to(REPO), n, line.strip()[:120]))
    assert seen > 1000                                                          # the scan really reads the citations
    assert bad == [], "\n".join(bad)


def test_the_rule_flags_an_address_inside_a_range_and_a_straddling_range():
    assert inside(0x4669A1) and inside(0x466AAF) and not inside(0x4669A0) and not inside(0x466AB0)
    assert inside(0x488E00, 0x488F00)                                            # a range that straddles a patch
    assert not inside(0x5ED01E) and inside(0x5ED01D)                             # exact [start, end)
