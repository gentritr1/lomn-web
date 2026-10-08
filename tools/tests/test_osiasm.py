"""M2a-1 T0: tools/osi/osiasm.py, the conformance assembler (consult §1.1). It is a serialiser, not an interpreter.

Checks:
1. its mnemonic table equals docs/osi/opcodes.md (96 named handlers: op, BCL name minus the prefix, operand letters);
2. round trip: every instruction it emits is decoded by tools/research/osi.py (an independent reader of the format) to
   the same opcode, address and operand bytes, and the tables (strings, globals, functions with argc, classes with
   properties and methods, symbols) read back as written;
3. the synthetic statt image (engine/tests/osi_builder.h `statt_osi()`, whose program is `osi_statt_program()`) is
   rebuilt byte for byte from asm text. The hex was measured 2026-10-08 by compiling a scratch dumper against
   osi_builder.h (scratchpad m2a1-t0/dump_statt.cpp); engine/tests/test_sage_osi.cpp pins the same hex.
"""
import importlib.util
import re
import struct
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]

STATT_PROGRAM_HEX = "40808080ff45440000a040714544000048427144cdcccc3d820000010004304301820200030001304752"
STATT_FILE_HEX = (
    "4f5349000400010004000e6763617265616469726563746f720006736574666f67000f67636c696768746469726563746f72000f7365746361"
    "76656c69676874696e670000000000010000010000710000000f6c65676f5f617265615f737461747400010008696e697466756c6c0000004080"
    "8080ff45440000a040714544000048427144cdcccc3d820000010004304301820200030001304752")

STATT_ASM = """
; engine/tests/osi_builder.h statt_osi(): SetFog(0xff808080, 0 - 5, 0 - 50, 0.1f); SetCaveLighting(1)
.strings "gcareadirector" "setfog" "gclightdirector" "setcavelighting"
.class lego_area_statt
.method initfull
    PushConstanti32 0xff808080
    PushConstant0
    PushConstantf32 5.0
    Subtract
    PushConstant0
    PushConstantf32 50.0
    Subtract
    PushConstantf32 0.1
    CallGameFunction gcareadirector.setfog 4
    Pop
    PushConstanti8 1
    CallGameFunction gclightdirector.setcavelighting 1
    Pop
    PushNothing
    Return
"""


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture(scope="module")
def asm():
    return _load("osiasm", REPO / "tools" / "osi" / "osiasm.py")


@pytest.fixture(scope="module")
def osi():
    return _load("osi_research", REPO / "tools" / "research" / "osi.py")


def test_mnemonics_equal_the_citation_table(asm):
    rows = re.findall(r"^\| (0x[0-9a-f]{2}) \| BCL(\w+) \| [^|]+\| [^|]+\| [^|]+\| ([^|]+) \|",
                      (REPO / "docs" / "osi" / "opcodes.md").read_text(), re.M)
    table = {int(op, 16): (name, fmt.strip()) for op, name, fmt in rows if name != "Unused"}
    assert len(table) == 96
    assert {op: (n, f) for op, (n, f) in table.items()} == {op: (n, f) for n, (op, f) in asm.MNEMONICS.items()}


def test_statt_program_byte_for_byte(asm):
    image, info = asm.assemble(STATT_ASM)
    assert image.hex() == STATT_FILE_HEX
    code = info["code_start"]
    assert image[code:].hex() == STATT_PROGRAM_HEX


ROUNDTRIP_ASM = r"""
.strings "first"
.globals screenlist g1
.function main 0
    LineNumber 7 9
    PushConstanti32 -2
    PushConstanti32 @callee            ; file offset of a label (0x50's target, file-relative)
    PushConstanti24 -5
    PushConstanti16 -300
    PushConstanti8 -1
    PushConstantf32 bits:0x7fc00001
    PushConstantString "hello \"q\" \x80"
    PushConstantColor8888 0xff808080
    PushConstantColor5551 0x1234
    GetVariableValue global:g1
    SetVariableValue 3
    CreateStackVariables 200          ; read unsigned by the handler; osi.py reads it signed
    CreateObject thing
    GetMemberValue .size
    SetThisMemberValue .size
    GetThisMemberFunction .tick
    MemberFunctionArgumentCheck 1
    CallGameFunction t.echo 1
    CallGameFunctionFromString t 2
    CallGameFunctionDirect 0 0
    GetGameVariable t.echo
top:
    CompareAndBranchIfFalse @done
    BranchAlways @top
    JumpRelative 2
    .byte 0x00 0x00
done:
    Return
.function SetupLevel 3 @callee
.class thing props(size, colour)
.method tick
callee:
    MemberFunctionArgumentCheck 1
    PushNothing
    Return
.method size @callee
"""


def test_roundtrip_through_the_independent_reader(asm, osi):
    image, info = asm.assemble(ROUNDTRIP_ASM)
    o = osi.parse(image)
    assert o["globals"] == ["screenlist", "g1"]
    assert [f[0] for f in o["functions"]] == ["main", "SetupLevel"] and [f[2] for f in o["functions"]] == [0, 3]
    assert o["functions"][1][1] == info["labels"]["callee"]
    (cname, props, meths), = o["classes"]
    assert cname == "thing" and [o["symbols"][p] for p in props] == ["size", "colour"]
    assert [(o["symbols"][s], off) for s, off in meths] == [("tick", info["labels"]["callee"]),
                                                            ("size", info["labels"]["callee"])]
    assert o["strings"][0] == "first" and 'hello "q" \x80' in o["strings"]
    # instruction by instruction: same address, opcode and operand bytes as the assembler's own listing
    listing = [x for x in info["listing"] if x["op"] is not None]
    raw = [x for x in info["listing"] if x["op"] is None]
    raw_at = {x["at"] + i for x in raw for i in range(len(x["bytes"]))}
    decoded = osi.disasm(image, o["functions"][0][1]) + osi.disasm(image, info["labels"]["callee"])
    assert [(at, op) for at, op, _ in decoded if at in raw_at] == [(at, 0x00) for at in sorted(raw_at)]  # .byte 0 = Nop
    decoded = [d for d in decoded if d[0] not in raw_at]
    assert [(at, op) for at, op, _ in decoded] == [(x["at"], x["op"]) for x in listing]
    for (at, op, args), x in zip(decoded, listing):
        enc = b""
        for f, v in zip(osi.OPS.get(op, ""), args):
            enc += (struct.pack("<i", v)[:3] if f == "T" else struct.pack("<" + f, v))
        assert enc == x["bytes"][1:], (hex(at), x)
    # operand meanings that the reader can confirm
    by_at = {at: (op, args) for at, op, args in decoded}
    lab = info["labels"]
    op, args = by_at[[x["at"] for x in listing if x["text"].startswith("CompareAndBranchIfFalse")][0]]
    assert args[0] + [x["at"] for x in listing if x["text"].startswith("CompareAndBranchIfFalse")][0] + 3 == lab["done"]
    push_label = [x for x in listing if x["text"].startswith("PushConstanti32 @callee")][0]
    assert by_at[push_label["at"]][1] == [lab["callee"]]
    call = [x for x in listing if x["text"].startswith("CallGameFunction t.echo")][0]
    c, f, n = by_at[call["at"]][1]
    assert (o["strings"][c], o["strings"][f], n) == ("t", "echo", 1)
    gv = [x for x in listing if x["text"].startswith("GetVariableValue")][0]
    assert by_at[gv["at"]][1] == [0x8001]
    assert by_at[[x["at"] for x in listing if x["text"].startswith("CreateStackVariables")][0]][1] == [200 - 256]
    assert [x["bytes"] for x in raw] == [b"\x00\x00"]


def test_errors_are_reported_with_the_line(asm):
    for bad, msg in (("    Frobnicate", "unknown mnemonic"), ("    PushConstanti8 300", "out of range"),
                     ("    BranchAlways @nowhere", "undefined label"), ('    PushConstantString "' + "x" * 256 + '"', "255")):
        with pytest.raises(asm.AsmError, match=msg):
            asm.assemble(".function main 0\n" + bad + "\n    Return\n")
