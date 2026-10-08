"""M2a-1 T1: presence tripwire for the VM's error texts (consult §1.2 acceptance). Every (row, message) pair of
docs/osi/opcodes.md's errors cells must appear VERBATIM as a C string literal inside that row's region of
engine/src/osi/vm_handlers.cpp. A region starts at a line `// ==== row 0xNN` and ends at the next `// ==== ` line.

The pairs are extracted exactly as tools/osi/conformance_report.py extracts them (its `table_rows` + `message_pairs`:
quoted messages of each named row's errors cell, abbreviated "… (rhs) …" cells expanded from the row's lhs text, section
references skipped), so the denominator is the same measured number. The message is compared in its C-escaped form
(`"` -> `\\"`, `\\` -> `\\\\`). This is a presence check only; the semantic gate is the conformance corpus.

Usage: check_messages.py engine/src/osi/vm_handlers.cpp docs/osi/opcodes.md   (exit 1 if any pair is missing)"""
from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from conformance_report import message_pairs, table_rows  # noqa: E402


def regions(cpp: str) -> dict[int, str]:
    out: dict[int, str] = {}
    cur, buf = None, []
    for line in cpp.splitlines():
        if line.startswith("// ==== "):
            if cur is not None:
                out[cur] = out.get(cur, "") + "\n".join(buf)
            m = re.match(r"// ==== row 0x([0-9a-f]{2})\b", line)
            cur, buf = (int(m.group(1), 16) if m else None), []
            continue
        if cur is not None:
            buf.append(line)
    if cur is not None:
        out[cur] = out.get(cur, "") + "\n".join(buf)
    return out


def c_literal(msg: str) -> str:
    return '"' + msg.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__)
        return 2
    cpp, md = Path(argv[0]).read_text(encoding="latin-1"), Path(argv[1])
    pairs = message_pairs(table_rows(md))
    reg = regions(cpp)
    missing = [(op, m) for op, m in pairs if c_literal(m) not in reg.get(op, "")]
    for op, m in missing:
        print("MISSING 0x%02x %s" % (op, c_literal(m)))
    print("%d/%d row-message pairs present" % (len(pairs) - len(missing), len(pairs)))
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
