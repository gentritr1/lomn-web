"""Seed addresses for the decomp re-run, computed from the canonical exe's bytes (data only; nothing is executed).

Ghidra's auto-analysis never made functions of the static initializers reached only through the `.CRT` pointer table,
nor of the script natives reached only through the fn pointer passed to ScOSISystem::RegisterFunction. This step lists
both so the decomp can create functions there.

  crt        every dword of the `.CRT` section that points into `.text`, in file order
  reg_sites  one {site, fnptr, candidates} per `E8 rel32` call to RegisterFunction, ascending by site
  type_sites every `E8 rel32` call to RegisterType
  seeds      sorted, de-duplicated union of `crt` and every `fnptr`

fnptr rule: walk back from the call site to the end of the previous RegisterFunction call, or at most WALK_BACK bytes,
whichever is nearer; every `68 imm32` (push imm32) in that window whose imm lies in `.text` is a candidate; the fn
pointer is the last candidate. The first registration of an initializer also sees the ScInitFunc pushes (several
candidates); later ones see exactly their own (the fn pointer is followed only by the name and class string pushes,
which point into data, not `.text`).

CLI: python tools/decomp/seed_addrs.py <exe> <out_dir>  ->  <out_dir>/seed_addrs.txt, <out_dir>/reg_sites.json
"""
from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from lomn_data import BUILDS, sha256_file  # noqa: E402

# gen/decomp/symbols.txt: 0x5fab30 ?RegisterFunction@ScOSISystem@@QAEXABV_ScBaseString@@0P6A?AVScOSIVariant@@...
REGISTER_FUNCTION = 0x5FAB30
# gen/decomp/symbols.txt: 0x5fae00 ?RegisterType@ScOSISystem@@QAEGABV_ScBaseString@@@Z
REGISTER_TYPE = 0x5FAE00
WALK_BACK = 256


def sections(exe: bytes) -> tuple[int, list[dict]]:
    """(image base, [{name, va, vsz, rsz, roff}]) from the PE section table."""
    pe = struct.unpack_from("<I", exe, 0x3C)[0]
    if exe[pe:pe + 4] != b"PE\0\0":
        raise ValueError("not a PE file")
    nsec = struct.unpack_from("<H", exe, pe + 6)[0]
    optsz = struct.unpack_from("<H", exe, pe + 20)[0]
    base = struct.unpack_from("<I", exe, pe + 24 + 28)[0]
    out = []
    for i in range(nsec):
        o = pe + 24 + optsz + 40 * i
        name = exe[o:o + 8].rstrip(b"\0").decode("latin-1")
        vsz, rva, rsz, roff = struct.unpack_from("<IIII", exe, o + 8)
        out.append(dict(name=name, va=base + rva, vsz=vsz, rsz=rsz, roff=roff))
    return base, out


def _section(secs: list[dict], name: str) -> dict:
    found = [s for s in secs if s["name"] == name]
    if len(found) != 1:
        raise ValueError(f"expected one {name} section, found {len(found)}")
    return found[0]


def _mapped(exe: bytes, s: dict) -> bytes:
    """The section's file bytes that are also mapped (min of virtual and raw size)."""
    return exe[s["roff"]:s["roff"] + min(s["vsz"], s["rsz"])]


def compute(exe_bytes: bytes, register_function: int = REGISTER_FUNCTION, register_type: int = REGISTER_TYPE) -> dict:
    _, secs = sections(exe_bytes)
    text = _section(secs, ".text")
    tb = _mapped(exe_bytes, text)
    tva = text["va"]

    def in_text(a: int) -> bool:
        return tva <= a < tva + text["vsz"]

    raw = _mapped(exe_bytes, _section(secs, ".CRT"))
    crt = [p for (p,) in struct.iter_unpack("<I", raw[:len(raw) & ~3]) if in_text(p)]

    sites: dict[int, list[int]] = {register_function: [], register_type: []}
    for i in range(len(tb) - 4):
        if tb[i] == 0xE8:
            tgt = (tva + i + 5 + struct.unpack_from("<i", tb, i + 1)[0]) & 0xFFFFFFFF
            if tgt in sites:
                sites[tgt].append(i)

    reg_sites = []
    prev_end = 0
    for i in sites[register_function]:
        lo = max(prev_end, i - WALK_BACK, 0)
        cands = [v for j in range(lo, i - 4) if tb[j] == 0x68
                 for v in (struct.unpack_from("<I", tb, j + 1)[0],) if in_text(v)]
        if not cands:
            raise ValueError(f"no .text push imm32 before RegisterFunction call at {tva + i:#x}")
        reg_sites.append({"site": tva + i, "fnptr": cands[-1], "candidates": len(cands)})
        prev_end = i + 5

    seeds = sorted(set(crt) | {r["fnptr"] for r in reg_sites})
    return {"crt": crt, "reg_sites": reg_sites, "type_sites": [tva + i for i in sites[register_type]], "seeds": seeds}


def main(argv: list[str] | None = None) -> dict:
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 2:
        raise SystemExit("usage: seed_addrs.py <exe> <out_dir>")
    exe, out = Path(argv[0]), Path(argv[1])
    sha = sha256_file(exe)
    if sha != BUILDS["canonical"]["exe_sha256"]:
        raise SystemExit(f"{exe} is not the canonical exe (sha256 {sha})")
    r = compute(exe.read_bytes())
    out.mkdir(parents=True, exist_ok=True)
    (out / "seed_addrs.txt").write_text("".join("0x%08x\n" % a for a in r["seeds"]))
    (out / "reg_sites.json").write_text(json.dumps(
        [{"site": "0x%08x" % s["site"], "fnptr": "0x%08x" % s["fnptr"], "candidates": s["candidates"]}
         for s in r["reg_sites"]], indent=0) + "\n")
    print(f"crt {len(r['crt'])}  reg_sites {len(r['reg_sites'])}  "
          f"distinct fnptrs {len({s['fnptr'] for s in r['reg_sites']})}  type_sites {len(r['type_sites'])}  "
          f"seeds {len(r['seeds'])}  -> {out}")
    return r


if __name__ == "__main__":
    main()
