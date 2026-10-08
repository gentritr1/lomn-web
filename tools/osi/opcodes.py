"""Generate the generated columns of docs/osi/opcodes.md (the OSI VM opcode citation table, M2a-0 T4).

Generated columns, from the canonical exe read as DATA (never run) plus the decomp corpus and Base.osi:
  op, handler, handler_addr  the template at TEMPLATE holds 256 x 16-byte CodeWarrior pointer-to-member entries
                             {this_delta, vtbl_offset, vptr_offset, pad}. ___ptmf_scall (0x6595a0) calls
                             *(*(this + this_delta + vptr_offset) + vtbl_offset) when vtbl_offset != -1, so the handler
                             is the dword at VTABLE + vtbl_offset. VTABLE (0x7514e4) is the value the constructor
                             (0x60b230) stores as the vptr: the ??_7ScOSIVirtualMachine@@6B@ symbol (0x7514dc) + 8.
                             Names come from gen/decomp/functions.jsonl.
  template_entry, vtable_slot  TEMPLATE + 16*op; vtbl_offset / 4 (and the byte offset).
  operands                   the operand format letters of tools/research/osi.py OPS (struct letters, 'T' = s24),
                             'none' for NOARG, 'n/a' for an opcode osi.py does not know.
  base_osi_count             occurrences in Base.osi over osi.subroutines() + osi.disasm() (the M1a-3 walk).
Authored columns (stack_effect, errors, notes) are kept from the existing document, keyed by op; everything outside the
BEGIN/END markers is kept verbatim. Usage:
  opcodes.py --exe EXE --osi BASE.OSI --corpus gen/decomp --out docs/osi/opcodes.md [--check]
"""
from __future__ import annotations

import argparse
import collections
import importlib.util
import json
import re
import struct
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
TEMPLATE = 0x751768       # handler template (.data), 16 bytes per opcode
VTABLE = 0x7514E4         # ScOSIVirtualMachine vtable as stored by the constructor
RUNTIME_TABLE = 0x752768  # ScOSIVirtualMachine::handlers (12-byte entries, zero in the file, filled by .CRT init)

GENERATED = ("op", "handler", "handler_addr", "template_entry", "vtable_slot", "operands", "base_osi_count")
AUTHORED = ("stack_effect", "errors", "notes")
COLUMNS = GENERATED + AUTHORED
BEGIN = "<!-- BEGIN opcode-table (generated columns: tools/osi/opcodes.py; authored columns kept by op) -->"
END = "<!-- END opcode-table -->"


def _load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class Pe:
    """Just enough PE32 to map a virtual address to file bytes."""

    def __init__(self, data: bytes):
        self.d = data
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe:pe + 4] != b"PE\0\0":
            raise ValueError("not a PE file")
        nsec = struct.unpack_from("<H", data, pe + 6)[0]
        optsz = struct.unpack_from("<H", data, pe + 20)[0]
        self.base = struct.unpack_from("<I", data, pe + 24 + 28)[0]
        self.sections = []
        for i in range(nsec):
            o = pe + 24 + optsz + 40 * i
            vsize, rva, rawsize, rawptr = struct.unpack_from("<IIII", data, o + 8)
            self.sections.append((self.base + rva, rawsize, rawptr))

    def u32(self, va: int) -> int:
        for start, rawsize, rawptr in self.sections:
            if start <= va and va + 4 <= start + rawsize:
                return struct.unpack_from("<I", self.d, rawptr + va - start)[0]
        raise ValueError("VA %#x is not file-backed" % va)


def handler_table(exe_bytes: bytes) -> list[dict]:
    pe = Pe(exe_bytes)
    out = []
    for op in range(256):
        entry = TEMPLATE + 16 * op
        this_delta, vtbl_off, vptr_off = pe.u32(entry), pe.u32(entry + 4), pe.u32(entry + 8)
        if this_delta or vptr_off or vtbl_off == 0xFFFFFFFF or vtbl_off % 4:
            raise ValueError("op %#x: template entry %#x is not a plain virtual PTMF" % (op, entry))
        out.append({"op": op, "entry": entry, "vtbl_off": vtbl_off, "handler": pe.u32(VTABLE + vtbl_off)})
    return out


def handler_names(corpus: Path) -> dict[int, str]:
    names = {}
    with open(corpus / "functions.jsonl") as f:
        for line in f:
            j = json.loads(line)
            names[int(j["addr"], 16)] = j["name"]
    return names


def opcode_counts(osi_bytes: bytes) -> collections.Counter:
    osi = _load("osi_research", TOOLS / "research" / "osi.py")
    o = osi.parse(osi_bytes)
    c = collections.Counter()
    for _, off in osi.subroutines(o):
        for _, op, _ in osi.disasm(osi_bytes, off):
            c[op] += 1
    return c


def operand_format(op: int) -> str:
    osi = _load("osi_research", TOOLS / "research" / "osi.py")
    if op in osi.OPS:
        return osi.OPS[op]
    return "none" if op in osi.NOARG else "n/a"


def _check_canonical(exe_bytes: bytes) -> None:
    import hashlib
    sys.path.insert(0, str(TOOLS))
    from lomn_data import BUILDS
    if hashlib.sha256(exe_bytes).hexdigest() != BUILDS["canonical"]["exe_sha256"]:
        raise ValueError("not the canonical exe (%s); the addresses here are only valid for it"
                         % BUILDS["canonical"]["source"])


def build_rows(exe: Path, osi_path: Path, corpus: Path) -> list[dict]:
    exe_bytes = Path(exe).read_bytes()
    _check_canonical(exe_bytes)
    names = handler_names(Path(corpus))
    counts = opcode_counts(Path(osi_path).read_bytes())
    rows = []
    for h in handler_table(exe_bytes):
        name = names.get(h["handler"])
        if name is None or not name.startswith("ScOSIVirtualMachine::BCL"):
            raise ValueError("op %#x -> %#x is not a BCL handler in the corpus (%r)" % (h["op"], h["handler"], name))
        rows.append({"op": "0x%02x" % h["op"], "handler": name.split("::", 1)[1], "handler_addr": "0x%x" % h["handler"],
                     "template_entry": "0x%06x" % h["entry"],
                     "vtable_slot": "%d (+0x%x)" % (h["vtbl_off"] // 4, h["vtbl_off"]),
                     "operands": operand_format(h["op"]), "base_osi_count": str(counts.get(h["op"], 0))})
    return rows


def _esc(s: str) -> str:
    return s.replace("\n", " ").replace("|", "\\|")


def _split(line: str) -> list[str]:
    return [c.strip().replace("\\|", "|") for c in re.split(r"(?<!\\)\|", line)[1:-1]]


def parse_table(text: str) -> dict[int, dict]:
    """{op: {column: cell}} from the table between the markers (empty if there is none)."""
    if BEGIN not in text:
        return {}
    block = text.split(BEGIN, 1)[1].split(END, 1)[0]
    header, out = None, {}
    for line in block.splitlines():
        if not line.startswith("|"):
            continue
        cells = _split(line)
        if header is None:
            header = cells
        elif not set(cells[0]) <= set("-: "):
            out[int(cells[0], 16)] = dict(zip(header, cells))
    return out


def render_table(rows: list[dict], authored: dict[int, dict]) -> str:
    lines = ["| " + " | ".join(COLUMNS) + " |", "|" + "|".join("---" for _ in COLUMNS) + "|"]
    for r in rows:
        a = authored.get(int(r["op"], 16), {})
        lines.append("| " + " | ".join(_esc(r[c]) if c in GENERATED else _esc(a.get(c, "")) for c in COLUMNS) + " |")
    return "\n".join(lines)


SKELETON = """# OSI VM opcode citation table

Regenerate with `make osi-tables`.

{begin}
{table}
{end}
"""


def update_document(existing: str | None, rows: list[dict], authored: dict[int, dict] | None = None) -> str:
    """Rewrite the generated columns; authored cells come from `existing` (by op), overridden by `authored`."""
    kept = parse_table(existing) if existing else {}
    for op, cells in (authored or {}).items():
        kept.setdefault(op, {}).update(cells)
    table = render_table(rows, kept)
    if not existing or BEGIN not in existing:
        return SKELETON.format(begin=BEGIN, table=table, end=END)
    head, rest = existing.split(BEGIN, 1)
    return head + BEGIN + "\n" + table + "\n" + END + rest.split(END, 1)[1]


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--exe", required=True)
    ap.add_argument("--osi", required=True)
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--check", action="store_true", help="exit 1 if --out is not up to date")
    a = ap.parse_args(argv)
    rows = build_rows(Path(a.exe), Path(a.osi), Path(a.corpus))
    out = Path(a.out)
    existing = out.read_text() if out.exists() else None
    new = update_document(existing, rows)
    named = sum(r["handler"] != "BCLUnused" for r in rows)
    summary = "opcodes: %d rows, %d named handlers, %d BCLUnused, %d Base.osi instructions over %d opcodes" % (
        len(rows), named, len(rows) - named, sum(int(r["base_osi_count"]) for r in rows),
        sum(r["base_osi_count"] != "0" for r in rows))
    if a.check:
        print(summary + ("; up to date" if new == existing else "; OUT OF DATE"))
        return 0 if new == existing else 1
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(new)
    print(summary + "; wrote " + str(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
