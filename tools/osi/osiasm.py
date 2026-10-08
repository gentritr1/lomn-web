"""M2a-1 T0: osiasm, the conformance assembler (consult §1.1): OSI 4.1 assembly text -> a .osi image.

A serialiser, not an interpreter: it lays out the tables and encodes each instruction's operands; it never evaluates
anything. Used by the conformance corpus (tests/osi/conformance), the Python oracle's unit tests and the C++ runner.
Layout (as tools/research/osi.py and lomn::sage::OsiFile read it): 'OSI\\0', u16 4, u16 1, string table, global table,
function table {u8 len, name, u32 offset, u16 argc}, class table {u8 nprops, nprops x u16 symbol, u8 nmethods,
nmethods x {u16 symbol, u32 offset}}, class names {u8 len, name, NUL}, symbol table, source table, then the code.
String-like tables are u16 count + {u8 len, bytes, NUL}. Offsets are file offsets (0 = 'O' of 'OSI\\0').

Syntax (one statement per line; `;` or `#` starts a comment outside quotes):
  .strings "a" "b" ...        seed the string table in this order (later uses append new strings in first-use order)
  .globals name ...           the global-name table (0x21/0x22 `global:name` operands index it)
  .symbols name ...           seed the symbol table (members/methods are appended in first-use order)
  .sources "file" ...         the source table (empty by default)
  .function NAME ARGC [@lbl]  a global-function entry; its code is what follows, or the label
  .class NAME [props(a, b)]   start a class; props are member symbols in property order
  .method NAME [@lbl]         a method of the current class (symbol NAME); its code follows, or the label
  label:                      a code label (its value is a file offset)
  .byte 0x00 12 ...           raw bytes (deliberately bad bytecode)
  Mnemonic operands           the BCL handler name without "BCL" (case-insensitive), operands in the table's units:
    integers (decimal, 0x hex, negative allowed; an 8/16-bit field accepts the signed or the unsigned range),
    PushConstanti32 @lbl      the label's file offset (the target 0x50 takes from the stack)
    PushConstantf32 1.5 | bits:0x3f800000
    PushConstantString "text" (escapes \\" \\\\ \\xNN); CallGameFunction cls.fn argc; CallGameFunctionFromString cls argc;
    Get/SetGameVariable cls.name; member operands .sym; CreateObject ClassName; Get/SetVariableValue global:name or a
    frame index; CompareAndBranchIfFalse/BranchAlways @lbl (encoded relative to the address after the operand).
Usage: osiasm.py IN.asm -o OUT.osi [--listing]
"""
from __future__ import annotations

import argparse
import re
import struct
import sys

# The 96 named handlers: name (BCL prefix removed) -> (op, operand letters), as docs/osi/opcodes.md lists them
# (test_osiasm.test_mnemonics_equal_the_citation_table compares the two).
MNEMONICS = {
    "Nop": (0x00, "none"), "DebugOn": (0x02, "none"), "DebugOff": (0x03, "none"), "LineNumber": (0x04, "HH"),
    "LineNumberAlt1": (0x05, "HH"), "LineNumberAlt2": (0x06, "HH"), "SetMemberValue": (0x10, "H"),
    "GetMemberValue": (0x11, "H"), "GetMemberFunction": (0x12, "H"), "CreateObject": (0x13, "H"),
    "MemberFunctionArgumentCheck": (0x14, "b"), "SetThisMemberValue": (0x15, "H"), "GetThisMemberValue": (0x16, "H"),
    "GetThisMemberFunction": (0x17, "H"), "GetMemberValueFromString": (0x18, "none"),
    "GetMemberFunctionFromString": (0x19, "none"), "SetMemberValueFromString": (0x1a, "none"),
    "GetVariableValue": (0x21, "H"), "SetVariableValue": (0x22, "H"), "CreateStackVariables": (0x23, "b"),
    "IncrementVariable": (0x24, "H"), "DecrementVariable": (0x25, "H"), "Pop": (0x30, "none"), "PopN": (0x31, "b"),
    "Swap": (0x32, "none"), "Pull": (0x33, "b"), "DupN": (0x34, "b"), "Dup": (0x35, "none"),
    "PushConstanti32": (0x40, "i"), "PushConstanti24": (0x41, "T"), "PushConstanti16": (0x42, "h"),
    "PushConstanti8": (0x43, "b"), "PushConstantf32": (0x44, "f"), "PushConstant0": (0x45, "none"),
    "PushConstantString": (0x46, "H"), "PushNothing": (0x47, "none"), "PushConstantColor8888": (0x48, "I"),
    "PushConstantColor5551": (0x49, "H"), "JumpRelative": (0x50, "b"), "JumpAbsolute": (0x51, "b"),
    "Return": (0x52, "none"), "CompareAndBranchIfFalse": (0x53, "h"), "BranchAlways": (0x54, "h"),
    "EqualTo": (0x60, "none"), "LessThan": (0x61, "none"), "GreaterThan": (0x62, "none"),
    "LessOrEqual": (0x63, "none"), "GreaterOrEqual": (0x64, "none"), "And": (0x6a, "none"), "Or": (0x6b, "none"),
    "Not": (0x6c, "none"), "BitwiseAnd": (0x6d, "none"), "BitwiseOr": (0x6e, "none"), "BitwiseXor": (0x6f, "none"),
    "Add": (0x70, "none"), "Subtract": (0x71, "none"), "Multiply": (0x72, "none"), "Divide": (0x73, "none"),
    "Power": (0x74, "none"), "Modulus": (0x75, "none"), "BitwiseNot": (0x76, "none"), "ShiftLeft": (0x77, "none"),
    "ShiftRight": (0x78, "none"), "Increment": (0x7a, "none"), "Decrement": (0x7b, "none"),
    "GetGameVariable": (0x80, "HH"), "SetGameVariable": (0x81, "HH"), "CallGameFunction": (0x82, "HHb"),
    "CallGameFunctionFromString": (0x83, "Hb"), "CallGameFunctionDirect": (0x84, "ib"), "CreateArray": (0x90, "none"),
    "GetArrayValue": (0x91, "none"), "ElementsInArray": (0x92, "none"), "SetArrayValue": (0x93, "none"),
    "AppendToArray": (0x94, "none"), "RemoveFromArray": (0x95, "none"), "InsertIntoArray": (0x96, "none"),
    "SetRedValue": (0xa0, "none"), "SetGreenValue": (0xa1, "none"), "SetBlueValue": (0xa2, "none"),
    "SetAlphaValue": (0xa3, "none"), "GetRedValue": (0xa4, "none"), "GetGreenValue": (0xa5, "none"),
    "GetBlueValue": (0xa6, "none"), "GetAlphaValue": (0xa7, "none"), "ConvertToString": (0xb0, "none"),
    "ConvertToFloat": (0xb1, "none"), "ConvertToInteger": (0xb2, "none"), "IsInteger": (0xb8, "none"),
    "IsFloat": (0xb9, "none"), "IsString": (0xba, "none"), "IsAnObject": (0xbb, "none"),
    "IsGameObject": (0xbc, "none"), "IsArray": (0xbd, "none"), "GetObjectClassID": (0xbf, "none"),
    "Halt": (0xff, "none"),
}
_BY_LOWER = {k.lower(): k for k in MNEMONICS}
_SIZE = {"b": 1, "B": 1, "h": 2, "H": 2, "i": 4, "I": 4, "f": 4, "T": 3}
_RANGE = {1: (-0x80, 0xFF), 2: (-0x8000, 0xFFFF), 3: (-0x800000, 0xFFFFFF), 4: (-0x80000000, 0xFFFFFFFF)}
MEMBER_OPS = {0x10, 0x11, 0x12, 0x15, 0x16, 0x17}
VARIABLE_OPS = {0x21, 0x22, 0x24, 0x25}
GLOBAL_BIT = 0x8000            # OSI_VM_GLOBAL_VARIABLE_INDICATOR (u16 at 0x751208), opcodes.md row 0x21


class AsmError(Exception):
    pass


def _tokens(line: str) -> list[str]:
    """Split on blanks, keeping quoted strings (with their quotes) whole; drop a trailing comment."""
    out, i, n = [], 0, len(line)
    while i < n:
        c = line[i]
        if c in " \t,()":
            i += 1
        elif c in ";#":
            break
        elif c == '"':
            j = i + 1
            while j < n and line[j] != '"':
                j += 2 if line[j] == "\\" else 1
            if j >= n:
                raise AsmError("unterminated string")
            out.append(line[i:j + 1])
            i = j + 1
        else:
            j = i
            while j < n and line[j] not in " \t,();#\"":
                j += 1
            out.append(line[i:j])
            i = j
    return out


def _string(tok: str) -> bytes:
    if not (tok.startswith('"') and tok.endswith('"')):
        raise AsmError("expected a quoted string, got %r" % tok)
    body, out, i = tok[1:-1], bytearray(), 0
    while i < len(body):
        if body[i] == "\\":
            nx = body[i + 1]
            if nx == "x":
                out.append(int(body[i + 2:i + 4], 16))
                i += 4
                continue
            out.append(ord({"n": "\n", "t": "\t", "0": "\0"}.get(nx, nx)))
            i += 2
        else:
            out += body[i].encode("latin-1")
            i += 1
    if len(out) > 255:
        raise AsmError("string of %d bytes: a table entry holds at most 255" % len(out))
    return bytes(out)


def _int(tok: str) -> int:
    try:
        return int(tok, 0)
    except ValueError:
        raise AsmError("expected an integer, got %r" % tok) from None


class _Table:
    def __init__(self):
        self.items: list[bytes] = []

    def index(self, s: bytes) -> int:
        if s not in self.items:
            self.items.append(s)
        return self.items.index(s)


def _table_bytes(items: list[bytes]) -> bytes:
    out = bytearray(struct.pack("<H", len(items)))
    for s in items:
        out += bytes([len(s)]) + s + b"\0"
    return bytes(out)


def assemble(text: str) -> tuple[bytes, dict]:
    """Returns (image, info): info["labels"] (file offsets), info["code_start"], info["listing"] (per statement:
    {"at", "op" (None for .byte), "bytes", "text", "line"})."""
    strings, symbols, sources = _Table(), _Table(), []
    globals_: list[bytes] = []
    functions, classes = [], []          # [name, argc, target]; {"name", "props", "methods": [[sym, target]]}
    code: list[dict] = []                # {"line", "text", "op", "fmt", "args"(raw tokens), "size", "at"(code-relative)}
    labels: dict[str, int] = {}
    pc = 0

    def here():
        return ("pc", pc)

    for ln, raw in enumerate(text.splitlines(), 1):
        try:
            toks = _tokens(raw)
            if not toks:
                continue
            head = toks[0]
            if head.endswith(":") and len(toks) == 1:
                name = head[:-1]
                if name in labels:
                    raise AsmError("label %r defined twice" % name)
                labels[name] = pc
            elif head == ".strings":
                for t in toks[1:]:
                    strings.index(_string(t))
            elif head == ".globals":
                globals_ += [t.encode("latin-1") for t in toks[1:]]
            elif head == ".symbols":
                for t in toks[1:]:
                    symbols.index(t.encode("latin-1"))
            elif head == ".sources":
                sources += [_string(t) for t in toks[1:]]
            elif head == ".function":
                target = ("label", toks[3][1:]) if len(toks) > 3 else here()
                functions.append([toks[1].encode("latin-1"), _int(toks[2]), target])
            elif head == ".class":
                props = []
                if len(toks) > 2:
                    if toks[2] != "props":
                        raise AsmError("expected props(...) after the class name")
                    props = [symbols.index(t.encode("latin-1")) for t in toks[3:]]
                classes.append({"name": toks[1].encode("latin-1"), "props": props, "methods": []})
            elif head == ".method":
                if not classes:
                    raise AsmError(".method outside a .class")
                target = ("label", toks[2][1:]) if len(toks) > 2 else here()
                classes[-1]["methods"].append([symbols.index(toks[1].encode("latin-1")), target])
            elif head == ".byte":
                data = bytes(_int(t) & 0xFF for t in toks[1:])
                code.append({"line": ln, "text": raw.strip(), "op": None, "data": data, "at": pc})
                pc += len(data)
            elif head.startswith("."):
                raise AsmError("unknown directive %r" % head)
            else:
                name = _BY_LOWER.get(head.lower())
                if name is None:
                    raise AsmError("unknown mnemonic %r" % head)
                op, fmt = MNEMONICS[name]
                fmt = "" if fmt == "none" else fmt
                args = toks[1:]
                # symbolic operands that add table entries are resolved now (first-use order)
                if op == 0x46 and args and args[0].startswith('"'):
                    args = [strings.index(_string(args[0]))]
                elif op in (0x82, 0x80, 0x81) and args and "." in args[0] and not args[0].startswith("."):
                    c, f = args[0].split(".", 1)
                    args = [strings.index(c.encode("latin-1")), strings.index(f.encode("latin-1"))] + args[1:]
                elif op == 0x83 and args and not re.fullmatch(r"-?(0x[0-9a-fA-F]+|\d+)", args[0]):
                    args = [strings.index(args[0].encode("latin-1"))] + args[1:]
                elif op in MEMBER_OPS and args and args[0].startswith("."):
                    args = [symbols.index(args[0][1:].encode("latin-1"))]
                n = len(fmt)
                if len(args) != n:
                    raise AsmError("%s takes %d operand(s), got %d" % (name, n, len(args)))
                size = 1 + sum(_SIZE[f] for f in fmt)
                code.append({"line": ln, "text": raw.strip(), "op": op, "fmt": fmt, "args": args, "at": pc,
                             "size": size})
                pc += size
        except AsmError as e:
            raise AsmError("line %d: %s" % (ln, e)) from None

    # ---- layout: the tables, then the code
    head = bytearray(b"OSI\0" + struct.pack("<HH", 4, 1))
    tables = bytearray()
    tables += _table_bytes(strings.items) + _table_bytes(globals_)
    fpatch = []
    tables += struct.pack("<H", len(functions))
    for name, argc, target in functions:
        tables += bytes([len(name)]) + name
        fpatch.append((len(tables), target))
        tables += struct.pack("<IH", 0, argc)
    tables += struct.pack("<H", len(classes))
    mpatch = []
    for c in classes:
        tables += bytes([len(c["props"])]) + b"".join(struct.pack("<H", p) for p in c["props"])
        tables += bytes([len(c["methods"])])
        for sym, target in c["methods"]:
            tables += struct.pack("<H", sym)
            mpatch.append((len(tables), target))
            tables += struct.pack("<I", 0)
    for c in classes:
        tables += bytes([len(c["name"])]) + c["name"] + b"\0"
    tables += _table_bytes(symbols.items) + _table_bytes(sources)
    base = len(head) + len(tables)
    image = bytearray(head + tables)

    def resolve(target, line=None) -> int:
        if target[0] == "pc":
            return base + target[1]
        if target[1] not in labels:
            raise AsmError("%sundefined label %r" % ("line %d: " % line if line else "", target[1]))
        return base + labels[target[1]]

    for at, target in fpatch + mpatch:
        struct.pack_into("<I", image, len(head) + at, resolve(target))
    class_index = {c["name"].decode("latin-1"): i for i, c in enumerate(classes)}
    gindex = {g.decode("latin-1"): i for i, g in enumerate(globals_)}
    listing = []
    for ins in code:
        if ins["op"] is None:
            image += ins["data"]
            listing.append({"at": base + ins["at"], "op": None, "bytes": ins["data"], "text": ins["text"],
                            "line": ins["line"]})
            continue
        enc = bytearray([ins["op"]])
        after = base + ins["at"] + ins["size"]
        try:
            for f, a in zip(ins["fmt"], ins["args"]):
                if isinstance(a, int):
                    v = a
                elif a.startswith("@"):
                    t = resolve(("label", a[1:]), ins["line"])
                    v = t - after if ins["op"] in (0x53, 0x54) else t
                elif a.startswith("global:"):
                    if a[7:] not in gindex:
                        raise AsmError("unknown global %r" % a[7:])
                    v = GLOBAL_BIT | gindex[a[7:]]
                elif ins["op"] == 0x13 and a in class_index:
                    v = class_index[a]
                elif f == "f":
                    if a.startswith("bits:"):
                        enc += struct.pack("<I", _int(a[5:]))
                    else:
                        enc += struct.pack("<f", float(a))
                    continue
                else:
                    v = _int(a)
                n = _SIZE[f]
                lo, hi = _RANGE[n]
                if not lo <= v <= hi:
                    raise AsmError("operand %d out of range for a %d-byte field" % (v, n))
                enc += (v & ((1 << (8 * n)) - 1)).to_bytes(n, "little")
        except AsmError as e:
            raise AsmError("line %d: %s" % (ins["line"], e)) from None
        image += enc
        listing.append({"at": base + ins["at"], "op": ins["op"], "bytes": bytes(enc), "text": ins["text"],
                        "line": ins["line"]})
    return bytes(image), {"labels": {k: base + v for k, v in labels.items()}, "code_start": base, "listing": listing,
                          "strings": [s.decode("latin-1") for s in strings.items],
                          "symbols": [s.decode("latin-1") for s in symbols.items]}


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("src")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--listing", action="store_true")
    a = ap.parse_args(argv)
    try:
        image, info = assemble(open(a.src, encoding="latin-1").read())
    except AsmError as e:
        print("%s: %s" % (a.src, e), file=sys.stderr)
        return 1
    open(a.out, "wb").write(image)
    if a.listing:
        for x in info["listing"]:
            print("%08x  %-24s %s" % (x["at"], x["bytes"].hex(), x["text"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
