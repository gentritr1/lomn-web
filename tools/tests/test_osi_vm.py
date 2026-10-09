"""M2a-1 T2: unit tests of the Python OSI oracle (tools/research/osi_vm.py), one or more per handler family, written
from docs/osi/opcodes.md, docs/osi/trace.md, docs/osi/conformance.md (§6 rulings included) and the cited asm. Programs
are assembled with tools/osi/osiasm.py. Expected numbers are computed from the row's formula (ints by hand, floats via
the exact rational helpers), never by running another interpreter. Each test names its row and, where the table was
silent or ambiguous, the asm address that decided it.
"""
import importlib.util
import json
import struct
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "osi"))
sys.path.insert(0, str(REPO / "tools" / "research"))

import osiasm  # noqa: E402
import osi_vm  # noqa: E402
from osi_vm import Variant, NOTHING  # noqa: E402

TYPES = osi_vm.TypeTable.load(REPO / "data" / "osi" / "types.json")


def fbits(x: float) -> int:
    return struct.unpack("<I", struct.pack("<f", x))[0]


def I(n):
    return Variant(0x10, n & 0xFFFFFFFF)


def F(x):
    return Variant(0x12, fbits(x))


def run(asm: str, entry="main", args=(), natives=(), registry=None, fpu_pc=24):
    image, info = osiasm.assemble(asm)
    reg = registry or osi_vm.NativeRegistry(TYPES)
    for n in natives:
        reg.add(osi_vm.test_native_entry(n, TYPES, reg))
    tr = osi_vm.ListTrace()
    heap = osi_vm.Heap(tr)
    vm = osi_vm.Vm(image, reg, heap, tr, fpu_pc=fpu_pc)
    args = [osi_vm.host_variant(vm, a, TYPES) if isinstance(a, dict) else a for a in args]
    res = vm.call(entry, list(args))
    return vm, res, tr.lines, info


def ok(asm, **kw):
    vm, res, lines, info = run(asm, **kw)
    assert res.ok, (vm.last_error.message if vm.last_error else None, lines[-5:])
    return vm, res, lines, info


def err(asm, **kw):
    vm, res, lines, info = run(asm, **kw)
    assert not res.ok, "expected an error"
    return vm, vm.last_error, lines, info


def main_of(body: str, pre: str = "") -> str:
    return pre + "\n.function main 0\n" + body + "\n"


# --------------------------------------------------------------------------------------------------- trace format
def test_trace_worked_example_matches_trace_md_byte_for_byte():
    """docs/osi/trace.md §4: the worked example's records (the header hashes differ by construction)."""
    asm = """.strings "t" "echo"
.function main 0
    PushConstantString "abc"
    CallGameFunction t.echo 1
    Pop
    PushConstantf32 1.0
    CompareAndBranchIfFalse @end
end:
    Return
"""
    native = {"cls": "t", "name": "echo", "min": 1, "max": 1, "types": ["string"], "returns": {"text": "abc"}}
    vm, e, lines, _ = err(asm, natives=[native])
    nothing10 = " ".join(["000f:00000000"] * 10)
    assert lines == [
        "C main 0 " + nothing10,
        "I 0000002d 46    1    1 00ff:00000000",
        "I 00000030 82    2    1 0011:00000002",
        "A s 00000001   3",
        "A s 00000002   3",
        "S 00000001   3 616263",
        "S 00000002   3 616263",
        "N 00000030 t.echo 1 0016:00000001 -> 0016:00000002",
        "F s 00000001",
        "I 00000036 30    2    1 0016:00000002",
        "F s 00000002",
        "I 00000037 44    1    1 00ff:00000000",
        "I 0000003c 53    2    1 0012:3f800000",
        'E 0000003c 53    1 "Results of comparison expression is not integral"',
        "P 0000003c 64",
    ]
    assert vm.fpu_pc == 64


# --------------------------------------------------------------------------------------------------- nop/line/debug
def test_nop_and_line_numbers_change_nothing():                     # 0x00, 0x04-0x06
    vm, res, lines, _ = ok(main_of("Nop\nLineNumber 1 2\nLineNumberAlt1 3 4\nLineNumberAlt2 5 6\nPushConstanti8 9\nReturn"))
    assert res.result == I(9)
    pcs = [int(l.split()[1], 16) for l in lines if l.startswith("I ")]
    assert [b - a for a, b in zip(pcs, pcs[1:])][:4] == [1, 5, 5, 5]


@pytest.mark.parametrize("mn,msg", [("DebugOn", "Debugger not enabled! but encountered DebugOn"),
                                    ("DebugOff", "Debugger not enabled! but encountered DebugOff")])
def test_debug_ops_always_error_after_advancing_pc(mn, msg):        # 0x02/0x03; asm 0x6038a6 inc PC first
    vm, e, lines, info = err(main_of("PushConstanti8 1\nat:\n" + mn + "\nReturn"))
    assert e.message == msg and e.pc == info["labels"]["at"] and e.depth == 2
    assert vm.snapshot().pc == info["labels"]["at"] + 1               # the PC register was advanced (0x6038a6)


@pytest.mark.parametrize("byte", [0x01, 0x20, 0x8f, 0xfe])
def test_unused_opcodes_error_without_advancing(byte):               # BCLUnused 0x60be60
    vm, e, lines, info = err(main_of("at:\n.byte %d\nReturn" % byte))
    assert e.message == "Call of undefined instruction" and e.pc == info["labels"]["at"] and e.op == byte
    assert vm.snapshot().pc == info["labels"]["at"]


# --------------------------------------------------------------------------------------------------- pushes
def test_push_family():                                             # 0x40-0x49
    body = """PushConstanti32 -5
PushConstanti16 -2
PushConstanti8 -1
PushConstantf32 1.5
PushConstant0
PushConstantString "x"
PushNothing
PushConstantColor8888 0x11223344
PushConstantColor5551 0x8001
Halt"""
    vm, res, lines, _ = ok(main_of(body))
    tops = [l.split()[5] for l in lines if l.startswith("I ")][1:]
    assert tops == ["0010:fffffffb", "0010:fffffffe", "0010:ffffffff", "0012:3fc00000", "0010:00000000",
                    "0011:00000000", "000f:00000000", "0015:11223344", "0014:00008001"]


def test_push_i24_always_errors_pc_not_advanced():                  # 0x41 (0x6065ee: no PC increment)
    vm, e, lines, info = err(main_of("at:\nPushConstanti24 5\nReturn"))
    assert e.message == "unwritten instruction BCL_PushConstanti24 called" and vm.snapshot().pc == info["labels"]["at"]


# --------------------------------------------------------------------------------------------------- stack ops
def test_stack_ops_order_and_dispose():                             # 0x30-0x35
    body = """PushConstanti8 1
PushConstanti8 2
PushConstanti8 3
Swap
Pull 3
DupN 2
PopN 2
Dup
Add
Add
Add
Add
Return"""
    # 1 2 3 -> swap 1 3 2 -> pull 3 (copies top-3 = 1): 1 3 2 1 -> dupn 2: 1 3 2 1 2 1 -> popn 2: 1 3 2 1
    # -> dup: 1 3 2 1 1 -> adds: 1+3+2+1+1 = 8
    vm, res, lines, _ = ok(main_of(body))
    assert res.result == I(8)


def test_pop_disposes_strings_and_dup_deep_copies():                 # 0x30, 0x35 (Duplicate 0x60aa20)
    vm, res, lines, _ = ok(main_of('PushConstanti8 0\nPushConstantString "ab"\nConvertToString\nDup\nPop\nPop\nReturn'))
    # 0xb0 on a 0x11: dynamic copy (A s 1); 0x35: deep copy (A s 2); 0x30 twice: F s 2, F s 1
    assert [l for l in lines if l[0] in "AF"] == ["A s 00000001   2", "A s 00000002   2", "F s 00000002",
                                                  "F s 00000001"]
    assert vm.heap.live() == {"o": 0, "a": 0, "s": 0, "g": 0} and vm.heap.allocs["s"] == 2


# --------------------------------------------------------------------------------------------------- variables
def test_frame_and_global_variables():                              # 0x21-0x25
    asm = """.globals g h
.function main 2
    GetVariableValue 0
    GetVariableValue 1
    Add
    SetVariableValue global:g
    IncrementVariable global:g
    DecrementVariable 0
    GetVariableValue 0
    SetVariableValue global:H
    CreateStackVariables 2
    GetVariableValue 2
    Return
"""
    vm, res, lines, _ = ok(asm.replace("global:H", "global:h"), args=[I(10), I(20)])
    assert res.result == NOTHING
    assert vm.global_value("g") == I(31) and vm.global_value("H") == I(9)   # GetToken folds a-z (0x5faae0)


def test_global_decrement_reports_increment_text():                  # 0x25 copy-paste message (0x607deb)
    e = err(".globals g\n.function main 0\nDecrementVariable global:g\nReturn")[1]
    assert e.message == "increment of non-integral type"
    e = err(".function main 0\nPushNothing\nDecrementVariable 0\nReturn")[1]
    assert e.message == "decrement of non-integral type"


# --------------------------------------------------------------------------------------------------- calls/branches
def test_jump_relative_is_file_relative_and_returns():              # 0x50, 0x52
    asm = """.function main 0
    PushConstanti8 4
    PushConstanti32 @sub
    JumpRelative 1
    PushConstanti8 1
    Add
    Return
sub:
    GetVariableValue 0
    PushConstanti8 10
    Multiply
    Return
"""
    vm, res, lines, info = ok(asm)
    assert res.result == I(41)
    assert any(l.startswith("I %08x" % info["labels"]["sub"]) for l in lines)


def test_branch_rules():                                             # 0x53, 0x54
    body = """PushNothing
CompareAndBranchIfFalse @a
PushConstanti8 99
Return
a:
PushConstanti8 2
CompareAndBranchIfFalse @b
PushConstanti8 7
BranchAlways @c
b:
PushConstanti8 98
c:
Return"""
    assert ok(main_of(body))[1].result == I(7)


def test_float_condition_is_an_error():                              # 0x53 falsifier
    vm, e, lines, info = err(main_of("PushConstantf32 0.0\nat:\nCompareAndBranchIfFalse @z\nz:\nReturn"))
    assert e.message == "Results of comparison expression is not integral" and e.depth == 1


def test_halt_pushes_nothing_and_stops():                            # 0xff
    vm, res, lines, _ = ok(main_of("PushConstanti8 5\nHalt"), args=[{"text": "a"}, {"text": "b"}])
    assert res.result == NOTHING and vm.depth == 0 and vm.heap.live()["s"] == 0
    # surplus engine arguments are disposed between C and the first I, in index order (SetPCAndArgs 0x60b644)
    assert lines[:5] == ["A s 00000001   1", "A s 00000002   1", lines[2], "F s 00000001", "F s 00000002"]


# --------------------------------------------------------------------------------------------------- objects
OBJ = """.class Foo props(x, y)
.method get @get
.function main 0
%s
get:
    MemberFunctionArgumentCheck 1
    GetThisMemberValue .x
    Return
"""


def test_object_lifecycle_and_member_ops():                          # 0x10-0x17
    body = """    CreateObject Foo
    Dup
    PushConstanti8 7
    Swap
    SetMemberValue .x
    Dup
    GetMemberValue .x
    Swap
    Dup
    GetMemberFunction .get
    JumpAbsolute 1
    Add
    Return"""
    vm, res, lines, _ = ok(OBJ % body)
    assert res.result == I(14)
    assert "A o 00000001 0000" in lines and "F o 00000001" in lines
    assert vm.heap.live()["o"] == 0


def test_set_member_on_absent_property_pops_v_first():              # 0x10: v popped (0x5fe7fc) before GetProperty
    asm = OBJ.replace("props(x, y)", "props(x)") % "    PushConstanti8 7\n    CreateObject Foo\n    SetMemberValue .get\n    Return"
    vm, e, lines, _ = err(asm.replace(".method get @get", ".method get @get"))
    assert e.message == 'Reference to non-existant member property "get"' and e.depth == 1


def test_set_member_on_non_object_keeps_v():                        # 0x10: type check (0x5fe7d8) before the v pop
    vm, e, lines, _ = err(OBJ % "    PushConstanti8 7\n    PushConstanti8 1\n    SetMemberValue .x\n    Return")
    assert e.message == "Member property of non member variable" and e.depth == 2


def test_arg_check_counts_this_and_pads():                           # 0x14
    asm = OBJ % "    CreateObject Foo\n    Dup\n    GetMemberFunction .get\n    JumpAbsolute 1\n    Return"
    vm, res, lines, _ = ok(asm)
    assert res.result == NOTHING
    e = err(OBJ.replace("MemberFunctionArgumentCheck 1", "MemberFunctionArgumentCheck 0")
            % "    CreateObject Foo\n    Dup\n    GetMemberFunction .get\n    JumpAbsolute 1\n    Return")[1]
    assert e.message == "Too many arguments to method"


def test_member_by_string_and_case_folding():                        # 0x18/0x19/0x1a; FindMemberID 0x609ba0 lower-cases
    body = """    CreateObject Foo
    Dup
    PushConstanti8 5
    Swap
    PushConstantString "X"
    SetMemberValueFromString
    Dup
    PushConstantString "x"
    GetMemberValueFromString
    Swap
    PushConstantString "GET"
    GetMemberFunctionFromString
    Pop
    Return"""
    assert ok(OBJ % body)[1].result == I(5)
    e = err(OBJ % '    CreateObject Foo\n    PushConstantString "nope"\n    GetMemberValueFromString\n    Return')[1]
    assert e.message == 'Reference to non-existant member property "nope"' and e.depth == 1
    e = err(OBJ % "    CreateObject Foo\n    PushConstanti8 1\n    GetMemberValueFromString\n    Return")[1]
    assert e.message == "Invalid type for member access (must be a string)" and e.depth == 1   # both popped first
    e = err(OBJ % '    PushConstanti8 3\n    PushConstanti8 1\n    PushConstantString "x"\n    SetMemberValueFromString\n    Return')[1]
    assert e.message == "Member property of non object variable" and e.depth == 2              # v still on the stack


def test_object_props_freed_back_to_front_after_the_object():        # dtor 0x6085cd: o, then props size-1..0
    body = """    CreateObject Foo
    Dup
    PushConstantString "p"
    ConvertToString
    Swap
    SetMemberValue .x
    Dup
    PushConstantString "q"
    ConvertToString
    Swap
    SetMemberValue .y
    Pop
    PushNothing
    Return"""
    lines = ok(OBJ % body)[2]
    frees = [l for l in lines if l.startswith("F ")]
    assert frees == ["F o 00000001", "F s 00000002", "F s 00000001"]


# --------------------------------------------------------------------------------------------------- compare
@pytest.mark.parametrize("a,b,mn,want", [
    ("PushConstanti32 16777217", "PushConstantf32 16777216.0", "EqualTo", 0),       # 0x60#1
    ("PushConstanti32 16777219", "PushConstantf32 16777220.0", "LessThan", 1),      # 0x61#1
    ("PushConstanti32 16777217", "PushConstantf32 16777216.0", "GreaterThan", 1),   # 0x62#1
    ("PushConstanti32 16777217", "PushConstantf32 16777216.0", "LessOrEqual", 0),   # 0x63#1
    ("PushConstanti32 16777219", "PushConstantf32 16777220.0", "GreaterOrEqual", 0),  # 0x64#1
    ("PushConstantf32 16777216.0", "PushConstanti32 16777217", "GreaterOrEqual", 0),  # 0x64#2
    ("PushConstantf32 -0.0", "PushConstantf32 0.0", "EqualTo", 1),
    ("PushConstanti8 -1", "PushConstanti8 1", "LessThan", 1),                       # signed int compare
])
def test_compare_falsifiers(a, b, mn, want):
    assert ok(main_of("%s\n%s\n%s\nReturn" % (a, b, mn)))[1].result == I(want)


def test_equal_strings_nothing_and_errors():                         # 0x60
    assert ok(main_of('PushConstantString "ABC"\nPushConstantString "abc"\nEqualTo\nReturn'))[1].result == I(1)
    assert ok(main_of("PushNothing\nPushConstant0\nEqualTo\nReturn"))[1].result == I(0)
    vm, e, lines, _ = err(main_of('PushConstanti8 1\nPushConstantString "1"\nEqualTo\nReturn'))
    # the 0x11 operand is converted to 0x16 before the type checks (0x605397), so the message prints type 16
    assert e.message == "Invalid operand to operator '=' (rhs) (type:16)" and vm.heap.live()["s"] == 1
    e = err(main_of('PushConstantString "a"\nPushConstantf32 1.0\nLessThan\nReturn'))[1]
    assert e.message == "Invalid operand to operator '<' (lhs) (type:11)" and e.depth == 1


# --------------------------------------------------------------------------------------------------- logic/bitwise
def test_logic_and_bitwise():                                        # 0x6a-0x6f, 0x76-0x78
    cases = [("PushConstanti8 2\nPushConstanti8 3\nAnd", 1), ("PushConstant0\nPushConstanti8 3\nOr", 1),
             ("PushNothing\nNot", 1), ("PushConstanti8 5\nNot", 0), ("PushConstanti8 6\nPushConstanti8 3\nBitwiseAnd", 2),
             ("PushConstanti8 6\nPushConstanti8 3\nBitwiseOr", 7), ("PushConstanti8 6\nPushConstanti8 3\nBitwiseXor", 5),
             ("PushNothing\nBitwiseNot", 1), ("PushConstant0\nBitwiseNot", 0xFFFFFFFF),
             ("PushConstanti8 1\nPushConstanti8 33\nShiftLeft", 2), ("PushConstanti8 -8\nPushConstanti8 1\nShiftRight", 0x7FFFFFFC)]
    for body, want in cases:
        assert ok(main_of(body + "\nReturn"))[1].result == I(want), body
    assert err(main_of("PushNothing\nPushConstanti8 1\nAnd\nReturn"))[1].message == \
        "Invalid operand to operator 'and' (lhs) (type:0F)"
    assert err(main_of("PushConstanti8 1\nPushConstantf32 1.0\nBitwiseXor\nReturn"))[1].message == \
        "Invalid operand to operator '@' (rhs) (type:12)"


def test_increment_raw_bits():                                       # 0x7a/0x7b: no type check
    assert ok(main_of("PushConstantf32 1.0\nIncrement\nReturn"))[1].result == Variant(0x12, 0x3F800001)
    assert ok(main_of("PushConstant0\nDecrement\nReturn"))[1].result == I(-1)


# --------------------------------------------------------------------------------------------------- arithmetic
@pytest.mark.parametrize("a,b,mn,want", [
    ("PushConstanti32 16777217", "PushConstantf32 1.0", "Add", 0x4B800001),          # 0x70#1
    ("PushConstanti32 16777217", "PushConstantf32 1.0", "Subtract", 0x4B800000),     # 0x71#1
    ("PushConstanti32 16777217", "PushConstantf32 3.0", "Multiply", 0x4C400001),     # 0x72#1: 50331652.0
    ("PushConstanti32 50331651", "PushConstantf32 3.0", "Divide", 0x4B800000),       # 0x73#1
    ("PushConstantf32 1.0e30", "PushConstantf32 1.0e-30", "Divide", 0x7F800000),     # 0x73#2
    ("PushConstanti32 16777217", "PushConstantf32 1.0e-15", "Add", 0x4B800001),      # 0x70#2 at pc 24
    ("PushConstantf32 bits:0x24368a23", "PushConstantf32 bits:0x1af99066", "Multiply", 0x002C7CD4),  # 0x72#2
    ("PushConstantf32 -0.0", "PushConstantf32 -0.0", "Add", 0x80000000),
    ("PushConstantf32 1.0", "PushConstantf32 1.0", "Subtract", 0x00000000),
    ("PushConstantf32 bits:0x7f800000", "PushConstanti8 0", "Multiply", 0xFFC00000),   # inf * 0 -> D9 NaN
])
def test_float_arithmetic_d8_prime(a, b, mn, want):
    assert ok(main_of("%s\n%s\n%s\nReturn" % (a, b, mn)))[1].result == Variant(0x12, want)


def test_precision_switches_to_64_after_atof():                      # D8' 0x70#3/#4, P record
    body = """PushConstantString "0.5"
at:
ConvertToFloat
Pop
PushConstanti32 16777217
PushConstantf32 1.0e-15
Add
PushConstanti32 16777217
PushConstantf32 1.0e-10
Add
Add
Return"""
    vm, res, lines, info = ok(main_of(body))
    p = "P %08x 64" % info["labels"]["at"]
    assert vm.fpu_pc == 64 and [l for l in lines if l.startswith("P ")] == [p]
    i = lines.index(p)
    assert lines[i - 1] == "A s 00000001   3" and lines[i + 1] == "F s 00000001"    # A s, P, F s (trace.md §3)
    # after the switch: 1.0e-15 is lost (4b800000), 1.0e-10 is kept (4b800001); their sum
    assert res.result == Variant(0x12, 0x4C000000 | 0)  # 16777216 + 16777218 = 33554434 -> f32 33554432 (4c000000)


def test_pc64_values():
    r64 = ok(main_of("PushConstanti32 16777217\nPushConstantf32 1.0e-15\nAdd\nReturn"), fpu_pc=64)[1].result
    r64b = ok(main_of("PushConstanti32 16777217\nPushConstantf32 1.0e-10\nAdd\nReturn"), fpu_pc=64)[1].result
    assert (r64.value, r64b.value) == (0x4B800000, 0x4B800001)


def test_integer_arithmetic_wrap_and_errors():                       # 0x70-0x75
    cases = [("PushConstanti32 2147483647\nPushConstanti8 1\nAdd", -2 ** 31),
             ("PushConstanti32 65536\nPushConstanti32 65536\nMultiply", 0),
             ("PushConstanti8 -7\nPushConstanti8 2\nDivide", -3), ("PushConstanti8 -1\nPushConstanti8 3\nModulus", 0),
             ("PushConstanti8 3\nPushConstanti8 2\nPower", 81), ("PushConstanti8 5\nPushConstant0\nPower", 5)]
    for body, want in cases:
        assert ok(main_of(body + "\nReturn"))[1].result == I(want), body
    assert ok(main_of("PushConstantf32 2.0\nPushConstanti8 1\nPower\nReturn"))[1].result == Variant(0x12, 0x4F000000)
    assert ok(main_of("PushConstanti8 2\nPushConstantf32 3.0\nPower\nReturn"))[1].result == F(8.0)
    for body, msg in [("PushConstanti8 1\nPushConstant0\nDivide", "Divide by 0"),
                      ("PushConstantf32 1.0\nPushConstantf32 -0.0\nDivide", "Divide by 0"),
                      ("PushConstanti8 1\nPushConstant0\nModulus", "Divide by 0"),
                      ("PushNothing\nPushConstanti8 1\nMultiply", "Invalid left-hand-side operand to operator '*' (type:0F)"),
                      # one float operand and one invalid one: the float cases fail, then a.type != 0x10 reports
                      # lhs with the FLOAT's type (0x6043e3, as the row says for '1.0 vs a string')
                      ("PushConstantf32 1.0\nPushNothing\nSubtract", "Invalid operand to operator '-' (lhs) (type:12)"),
                      ("PushConstanti8 1\nPushNothing\nSubtract", "Invalid operand to operator '-' (rhs) (type:0F)"),
                      ("PushConstantString \"a\"\nPushConstanti8 1\nAdd", "Invalid operand to string operator '+' (rhs) (type:10)"),
                      ("PushConstanti8 1\nPushConstantString \"a\"\nAdd", "Invalid operand to string operator '+' (lhs) (type:10)")]:
        e = err(main_of(body + "\nReturn"))[1]
        assert e.message == msg and e.depth == 1, body                  # both operands popped before the checks


def test_string_concat_and_len_too_long():                           # 0x70 strings, kLenTooLong at 255
    a200, a54, a55 = "a" * 200, "b" * 54, "b" * 55
    vm, res, lines, _ = ok(main_of('PushConstantString "%s"\nPushConstantString "%s"\nAdd\nReturn' % (a200, a54)))
    assert [l for l in lines if l.startswith("S ") or l.startswith("A ")][:2] == ["A s 00000001 200", "A s 00000002  54"]
    vm, e, lines, _ = err(main_of('PushConstanti8 9\nPushConstantString "%s"\nPushConstantString "%s"\nAdd\nReturn'
                                  % (a200, a55)))
    assert e.message == "kLenTooLong" and e.depth == 2 and vm.heap.live()["s"] == 2     # both leak (no dispose)


# --------------------------------------------------------------------------------------------------- arrays
def test_arrays_alias_and_bounds():                                  # 0x90-0x96
    body = """CreateArray
Dup
Dup
PushConstanti8 10
AppendToArray
PushConstanti8 20
AppendToArray
Dup
PushConstant0
PushConstanti8 5
InsertIntoArray
Dup
PushConstanti8 1
RemoveFromArray
Dup
PushConstanti8 1
PushConstanti8 7
SetArrayValue
Dup
ElementsInArray
Swap
PushConstanti8 1
GetArrayValue
Add
Return"""
    vm, res, lines, _ = ok(main_of(body))
    assert res.result == I(2 + 7) and vm.heap.live()["a"] == 0
    e = err(main_of("CreateArray\nPushConstanti8 -1\nGetArrayValue\nReturn"))[1]
    assert e.message == "Reference beyond start/end of array" and e.depth == 1          # unsigned index
    e = err(main_of("CreateArray\nPushConstant0\nPushConstant0\nInsertIntoArray\nReturn"))[1]
    assert e.message == "insertion outside of array bounds"
    e = err(main_of("PushConstanti8 1\nElementsInArray\nReturn"))[1]
    assert e.message == "array reference on non-array"


# --------------------------------------------------------------------------------------------------- colour
def test_colour_channels():                                          # 0xa0-0xa7
    body = """PushConstantColor8888 0x11223344
PushConstanti8 -1
SetRedValue
PushConstanti32 300
SetAlphaValue
Dup
GetGreenValue
Swap
GetRedValue
Add
Return"""
    assert ok(main_of(body))[1].result == I(0x22 + 0xFF)
    e = err(main_of("PushConstantColor5551 1\nGetRedValue\nReturn"))[1]
    assert e.message == "cant access red value of anything but a color"
    e = err(main_of("PushConstanti8 1\nPushConstantf32 1.0\nSetBlueValue\nReturn"))[1]
    assert e.message == "cant access blue value of anything but a color" and e.depth == 1   # c checked first (0x6000f9)


# --------------------------------------------------------------------------------------------------- convert
def test_conversions():                                              # 0xb0-0xb2, ConvertVariant 0x60b9d0
    vm, res, lines, _ = ok(main_of("PushConstantf32 1.5\nConvertToString\nReturn"))
    assert "S" not in lines and "A s 00000001   8" in lines                                # "1.500000"
    cases = [("PushConstantf32 2.7\nConvertToInteger", I(2)), ("PushConstantf32 -2.7\nConvertToInteger", I(-2)),
             ("PushConstantf32 3.0e9\nConvertToInteger", I(-1294967296)),
             ("PushConstanti32 16777217\nConvertToFloat", F(16777216.0)),
             ('PushConstantString " -12x"\nConvertToInteger', I(-12)),
             ("PushNothing\nConvertToString", Variant(0, 0)), ("PushConstanti8 4\nConvertToInteger", I(4))]
    for body, want in cases:
        assert ok(main_of(body + "\nReturn"))[1].result == want, body
    assert ok(main_of('PushConstantString "-2.25"\nConvertToFloat\nReturn'))[0].fpu_pc == 64


# --------------------------------------------------------------------------------------------------- type tests
def test_type_tests_and_class_id():                                  # 0xb8-0xbd, 0xbf
    asm = """.class A
.class B
.function main 0
    PushConstanti8 1
    IsInteger
    PushConstantf32 1.0
    IsFloat
    Add
    PushConstantString "s"
    IsString
    Add
    CreateObject B
    IsAnObject
    Add
    CreateArray
    IsArray
    Add
    PushConstanti8 1
    IsGameObject
    Add
    CreateObject B
    GetObjectClassID
    Add
    CallGameFunction t.obj 0
    GetObjectClassID
    Add
    Return
"""
    native = {"cls": "t", "name": "obj", "min": 0, "max": 0, "types": [], "returns": {"game": "GcTimer"}}
    vm, res, lines, _ = ok(asm, natives=[native])
    assert res.result == I(5 + 1 + 0x27 + 0x10000)                     # GcTimer = 0x20 + rank 7 (types.json)
    assert err(main_of("PushConstanti8 1\nGetObjectClassID\nReturn"))[1].message == "Cant get classID of non object"


# --------------------------------------------------------------------------------------------------- native calls
def test_native_conversion_byref_memo_and_ownership():               # 0x82/0x84 (memoised path)
    asm = """.function main 0
    PushConstant0
loop:
    PushConstantString "7"
    PushConstanti8 3
    CallGameFunction t.f 2
    Add
    Dup
    PushConstanti8 10
    LessThan
    CompareAndBranchIfFalse @out
    BranchAlways @loop
out:
    Return
"""
    native = {"cls": "t", "name": "f", "min": 2, "max": 2, "types": ["int", "byref"], "returns": {"arg": 0},
              "writes": [{"arg": 1, "text": "w"}]}
    vm, res, lines, _ = ok(asm, natives=[native])
    assert res.result == I(14)
    ns = [l for l in lines if l.startswith("N ")]
    assert len(ns) == 2 and all(" t.f 2 0010:00000007 0200:" in l for l in ns)
    assert sum(1 for l in lines if l.startswith("I ") and l.split()[2] == "82") == 2   # memoised: still 82
    assert vm.heap.live() == {"o": 0, "a": 0, "s": 0, "g": 0}
    assert vm.image == osiasm.assemble(asm)[0]                           # D4: the image is never written
    # 0x11 -> int: in-place dynamic string, then PushInto disposes it (0x601edc) before storing the int
    first = lines.index(ns[0])
    assert lines[first - 3:first] == ["A s 00000001   1", "F s 00000001", "A s 00000002   1"]


def test_native_errors_are_wrapped():                                # 0x82 catch(...) -> "Script is calling %s.%s"
    native = {"cls": "t", "name": "f", "min": 1, "max": 1, "types": ["colour"]}
    vm, e, lines, _ = err(main_of("PushConstanti8 1\nCallGameFunction t.F 1\nReturn"), natives=[native])
    assert e.message == "Script is calling t.F" and e.depth == 2 and e.op == 0x82
    vm, e, lines, _ = err(main_of("CallGameFunction t.nope 0\nReturn"), natives=[native])
    assert e.message == "Call to unregistered Game Function"
    vm, e, lines, _ = err(main_of("CallGameFunction t.f 0\nReturn"), natives=[native])
    assert e.message == "Script is calling t.f"


def test_native_by_string_and_argc_violation_registry():             # 0x83; types.md argc violations
    native = {"cls": "t", "name": "g", "min": 0, "max": 0, "types": [], "returns": {"type": "0x10", "value": 3}}
    vm, res, lines, _ = ok(main_of('PushConstantString "G"\nPushConstantString "g"\nConvertToString\nSwap\nPop\n'
                                   'CallGameFunctionFromString t 0\nReturn'), natives=[native])
    assert res.result == I(3) and vm.heap.live()["s"] == 0
    e = err(main_of("PushConstanti8 1\nCallGameFunctionFromString t 0\nReturn"), natives=[native])[1]
    assert e.message == "Invalid member type for function call (must be a string)"
    reg = osi_vm.NativeRegistry.load(REPO / "data/osi/natives.json", REPO / "data/osi/natives_beta150.json", TYPES)
    e = err(main_of("PushConstanti8 1\nCallGameFunction GcAreaDirector.Hide 1\nReturn"), registry=reg)[1]
    assert e.message == "Script is calling GcAreaDirector.Hide"


def test_literal_0x84_is_the_d10_error():                            # plan acceptance; opcodes.md D10
    vm, e, lines, info = err(main_of("at:\n.byte 0x84 0 0 0 0 0\nReturn"))
    assert e.message == "Direct call without a memoised site" and e.op == 0x84 and e.pc == info["labels"]["at"]


def test_typed_stub_returns_d11():                                   # D11 policy over natives.json rows
    reg = osi_vm.NativeRegistry.load(REPO / "data/osi/natives.json", REPO / "data/osi/natives_beta150.json", TYPES)
    vm, res, lines, _ = ok(main_of("CallGameFunction GcLine.Create 0\nReturn"), registry=reg)
    assert res.result == Variant(0x21, 1) and "A g 00000001 21" in lines and "F g 00000001" in lines
    vm, res, lines, _ = ok(main_of("PushConstanti8 1\nCallGameFunction bettersaver.getbooleanvalue 1\nReturn"),
                           registry=reg)
    assert res.result == I(0)


def test_sets_fpu_pc64_native():                                     # D8': gcscripttools.stringtofloat
    reg = osi_vm.NativeRegistry.load(REPO / "data/osi/natives.json", None, TYPES)
    vm, res, lines, _ = ok(main_of('PushConstantString "1"\nCallGameFunction GcScriptTools.StringToFloat 1\nReturn'),
                           registry=reg)
    p = [i for i, l in enumerate(lines) if l.startswith("P ")]
    assert vm.fpu_pc == 64 and len(p) == 1 and lines[p[0] + 1].startswith("S ")       # P before S/N


# --------------------------------------------------------------------------------------------------- engine calls
def test_engine_call_prefix_and_not_found():                         # FindFunction 0x609ce0
    asm = ".function setup 0\nPushConstanti8 1\nReturn\n.function main 0\nReturn\n"
    vm, res, lines, _ = ok(asm, entry="SetupLevel_1")
    assert res.result == I(1) and lines[0].startswith("C SetupLevel_1 0 ")
    vm, res, lines, _ = run(asm, entry="Set")
    assert res.found is False and lines == ["M Set"] and vm.depth == 0


def test_too_many_entry_args_before_run():                           # SetPCAndArgs 0x60b62c; conformance §6.5
    vm, e, lines, info = err(".function main 11\nat:\nPushNothing\nReturn\n")
    assert e.message == "Too many args required (max 10) to call requested entry point."
    assert e.pc == info["labels"]["at"] and e.op == 0x47 and e.depth == 1 and e.top == Variant(0xFF, 0)


def test_reentrant_native_marker_and_nesting():                      # conformance §6.1, §6.11; 0x82 PC before the native
    asm = """.globals g
.function inner 1
    GetVariableValue 0
    SetVariableValue global:g
    PushConstanti8 4
    Return
.function main 0
    PushConstanti8 7
at:
    CallGameFunction t.re 0
    Add
    Return
"""
    native = {"cls": "t", "name": "re", "min": 0, "max": 0, "types": [],
              "calls": {"entry": "inner", "args": [{"type": "0x10", "value": 5}]}, "returns": {"type": "0x10", "value": 2}}
    vm, res, lines, info = ok(asm, natives=[native])
    assert vm.global_value("g") == I(5) and vm.context_depth == 0
    # the nested marker saves the VM PC, which 0x82 has already advanced past its 6 bytes (0x6015c6-0x601780)
    first_inner = next(l for l in lines if l.startswith("I ") and int(l.split()[1], 16) < info["labels"]["at"]
                       and l.split()[4] == "3")
    # the marker value is visible in a native that reads the (now stale) slot right after its nested call returns
    seen = []

    def fn(call):
        call.vm.call("inner", [I(5)])
        seen.append(call.vm.stack[2])
        return I(2)

    reg = osi_vm.NativeRegistry(TYPES)
    reg.add(osi_vm.NativeEntry("t", "re", "t.re", 0, 0, (15,) * 10, "test", 0, None, fn, "test"))
    ok(asm, registry=reg)
    assert seen == [Variant(0xFF, info["labels"]["at"] + 6)]
    assert first_inner.split()[3] == "4" and first_inner.endswith("0010:00000005")
    i_c, i_r = lines.index(next(l for l in lines if l.startswith("C inner"))), lines.index("R 0010:00000004")
    i_n = lines.index(next(l for l in lines if l.startswith("N ")))
    assert i_c < i_r < i_n and lines[i_r + 1] == lines[i_n]          # nothing between the nested R and the N
    assert res.result == I(9)


def test_nested_error_is_rewrapped_and_snapshot_keeps_depth():       # D10 + 0x82 catch(...)
    asm = """.function inner 0
    PushConstantf32 1.0
    CompareAndBranchIfFalse @x
x:
    Return
.function main2 0
    PushConstanti8 1
    Return
.function main 0
at:
    CallGameFunction t.re 0
    Return
"""
    native = {"cls": "t", "name": "re", "min": 0, "max": 0, "types": [], "calls": {"entry": "inner", "args": []}}
    vm, e, lines, info = err(asm, natives=[native])
    assert e.message == "Script is calling t.re" and e.pc == info["labels"]["at"] and e.op == 0x82
    assert vm.context_depth == 2 and sum(1 for l in lines if l.startswith("E ")) == 1
    vm.reset_after_error()
    assert vm.depth == 0 and vm.context_depth == 0
    assert vm.call("main2", []).ok and vm.fpu_pc == 64


def test_engine_call_nesting_cap_d10():                              # port cap (not original), same as the C++ VM
    asm = ".function main 0\nat:\nCallGameFunction t.deep 0\nReturn\n"
    inner = []

    def deep_native(levels):
        left = [levels]

        def fn(call):
            if left[0] > 0:
                left[0] -= 1
                try:
                    call.vm.call("main", [])
                except osi_vm.VmError as e:
                    if not inner:
                        inner.append(e.message)
                    raise
            return I(1)
        reg = osi_vm.NativeRegistry(TYPES)
        reg.add(osi_vm.NativeEntry("t", "deep", "t.deep", 0, 0, (15,) * 10, "test", 0, None, fn, "test"))
        return reg, left

    reg, _ = deep_native(63)                                         # the outer call + 63 re-entries = 64 active
    vm, res, lines, info = ok(asm, registry=reg)
    assert not inner and sum(1 for l in lines if l.startswith("C ")) == 64
    assert vm.depth == 0 and vm.context_depth == 0
    reg, left = deep_native(64)                                      # the 65th call is refused before it starts
    vm, e, lines, info = err(asm, registry=reg)
    assert inner == ["Undefined: engine-call nesting limit (64)"]
    assert e.message == "Script is calling t.deep" and e.pc == info["labels"]["at"] and e.op == 0x82
    assert sum(1 for l in lines if l.startswith("C ")) == 64 and sum(1 for l in lines if l.startswith("E ")) == 1
    assert vm.context_depth == 64
    vm.reset_after_error()
    assert vm.depth == 0 and vm.context_depth == 0
    left[0] = 1                                                      # the nesting count was restored too
    assert vm.call("main", []).ok


def test_run_program_reports_recursion_error_as_host_error(monkeypatch, tmp_path):   # M1: --conform never aborts
    asm = tmp_path / "x-recursion.asm"
    asm.write_text(".function main 0\nPushConstanti8 1\nReturn\n")
    (tmp_path / "x-recursion.expect.json").write_text(json.dumps(
        {"id": "x-recursion", "rows": [], "entry": "main", "args": [], "natives": [], "registry": "none",
         "expect": {"ok": True}}))

    def boom(self, *a, **k):
        raise RecursionError("maximum recursion depth exceeded")
    monkeypatch.setattr(osi_vm.Vm, "_run", boom)
    fails = osi_vm.run_program(asm)
    assert fails == ["host error: Undefined: host exception: RecursionError: maximum recursion depth exceeded"]


def test_stack_caps_d10():                                           # our two errors
    vm, e, lines, info = err(".function main 0\nrec:\nPushConstanti32 @rec\nJumpRelative 0\n")
    assert e.message == "Stack overflow (1000 slots)" and e.depth == 1000
    e = err(".function main 0\nPop\nPop\nReturn\n")[1]
    assert e.message == "Stack underflow" and e.depth == 0


# --------------------------------------------------------------------------------------------------- replay log
def test_replay_log_round_trip(tmp_path):                            # trace.md §6
    asm = main_of('CallGameFunction t.s 0\nCallGameFunction t.o 0\nPop\nReturn')
    natives = [{"cls": "t", "name": "s", "min": 0, "max": 0, "types": [], "returns": {"hex": "00ff"}},
               {"cls": "t", "name": "o", "min": 0, "max": 0, "types": [], "returns": {"game": "GcTimer"}}]
    image, _ = osiasm.assemble(asm)
    reg = osi_vm.NativeRegistry(TYPES)
    for n in natives:
        reg.add(osi_vm.test_native_entry(n, TYPES, reg))
    reg.return_log = []
    t1 = osi_vm.ListTrace()
    osi_vm.Vm(image, reg, osi_vm.Heap(t1), t1).call("main", [])
    # v2 (natives.md §3.1, M2a-2 T4): a game value is followed by ` G <engine references>`, 0 for a fresh stub object
    assert reg.return_log == ["00000000 t.s 0 -> 0016:00000001 S 2 00ff", "00000001 t.o 0 -> 0027:00000001 G 0"]
    log = tmp_path / "natives.log"
    log.write_text("\n".join(reg.return_log) + "\n")
    reg2 = osi_vm.NativeRegistry(TYPES)
    for n in natives:
        reg2.add(osi_vm.NativeEntry(n["cls"], n["name"], "", 0, 0, (15,) * 10, "none", 0, None, None, "stub"))
    reg2.replay_from(log)
    t2 = osi_vm.ListTrace()
    osi_vm.Vm(image, reg2, osi_vm.Heap(t2), t2).call("main", [])
    assert t1.lines == t2.lines


def test_conform_cli_on_a_tiny_corpus(tmp_path, capsys):
    (tmp_path / "p.asm").write_text(main_of("PushConstanti8 1\nPushConstanti8 2\nAdd\nReturn"))
    (tmp_path / "p.expect.json").write_text(json.dumps(
        {"id": "p", "rows": ["0x70"], "entry": "main", "args": [], "natives": [], "registry": "none",
         "expect": {"ok": True, "result": {"type": "0x10", "value": 3}, "depth": 0, "context_depth": 0, "fpu_pc": 24}}))
    assert osi_vm.main(["--conform", str(tmp_path)]) == 0
    assert "conformance: 1/1 pass" in capsys.readouterr().out


def test_memoised_site_fails_with_the_direct_call_message():        # 0x82 self-patch -> 0x84 path (0x60303c)
    """The second execution of a site is the self-patched 0x84: no lookup, no min/max, and a failure inside its
    catch(...) reports "Script is calling a game function directly." (string 0x74ed9a). The trace still prints 82."""
    asm = """.function main 0
    PushConstantColor8888 0x01020304
loop:
    CallGameFunction t.c 1
    PushConstanti8 1
    BranchAlways @loop
"""
    native = {"cls": "t", "name": "c", "min": 1, "max": 1, "types": ["colour"], "returns": {"type": "0x10", "value": 0}}
    vm, e, lines, info = err(asm, natives=[native])
    assert e.message == "Script is calling a game function directly." and e.op == 0x82
    assert e.pc == info["labels"]["loop"] and e.depth == 3 and len(vm.memo) == 1   # marker, 0, 1 (unpopped)


def test_base_osi_setup_with_typed_stubs(lomn_data):
    """Early A6 signal (D11/D12 typed stubs): Setup on the real Base.osi completes. The counts are pinned from
    `osi_vm.py --run Base.osi --frames 3` run on 2026-10-08 (setup: instructions 48850, natives 980)."""
    base = lomn_data / "install" / "data" / "Base.osi"
    reg = osi_vm.NativeRegistry.load(REPO / "data/osi/natives.json", REPO / "data/osi/natives_beta150.json", TYPES)
    image = base.read_bytes()
    vm = osi_vm.Vm(image, reg, osi_vm.Heap(), None)
    r = vm.call("Setup", [])
    assert r.ok, vm.last_error and vm.last_error.message
    assert (vm.instructions, vm.native_calls, vm.depth, vm.context_depth) == (48850, 980, 0, 0)
    assert vm.image == image


# --------------------------------------------------------------------------------------------------- fix round 1
PROC = """.globals n
.function setup 0
    PushConstant0
    SetVariableValue global:n
    PushNothing
    Return
.function process 3
    GetVariableValue global:n
    PushConstanti8 1
    Add
    Dup
    SetVariableValue global:n
    PushConstanti8 %d
    GreaterThan
    CompareAndBranchIfFalse @zero
    PushNothing
    Return
zero:
    PushConstant0
    Return
"""


def _run_cli(tmp_path, asm, frames, capsys):
    p = tmp_path / "s.osi"
    p.write_bytes(osiasm.assemble(asm)[0])
    t = tmp_path / "s.trace"
    assert osi_vm.main(["--run", str(p), "--frames", str(frames), "--trace", str(t)]) == 0
    out = capsys.readouterr().out
    return [l for l in t.read_text().splitlines() if l.startswith("C Process") or l.startswith("K ")], out


def test_run_reruns_process_on_int_zero(tmp_path, capsys):
    """I1: ScSLOSIProcess::Process (0x43b33f-0x43b35f) is false only for an int 0 result; ScProcessManager::Process
    re-runs it with the same time, dt and counter (0x4a9449-0x4a9584); the counter increments at frame end (0x4a95b6).
    process returns 0 on its first two calls, then nothing: frame 0 runs 3 passes, frame 1 one."""
    lines, out = _run_cli(tmp_path, PROC % 2, 2, capsys)
    kinds = [l.split()[0] for l in lines]
    assert kinds == ["K", "C", "C", "C", "K", "C"]
    assert lines[1] == lines[2] == lines[3]                              # same time, dt and counter on a re-run
    assert lines[5].split()[3:6] != lines[1].split()[3:6] and lines[5].split()[5] == "0010:00000001"
    assert "process passes: total 4 over 2 frames, max 3, frames with re-runs 1 (first: 0:3), frames skipped 0" in out


def test_run_rerun_is_capped_at_ten_more(tmp_path, capsys):          # cmp [ebp-0xd4], 0xa; jb (0x4a957d)
    lines, out = _run_cli(tmp_path, PROC % 100, 1, capsys)
    assert sum(1 for l in lines if l.startswith("C ")) == 11
    assert "max 11" in out


def test_find_member_id_cache_poisoning():
    """I2: FindMemberID (0x609ba0) caches a miss as {last symbol compared, 0xffff} (0x609c57-0x609cc8), so after a lookup
    of a non-symbol, the LAST symbol of the table also resolves to 0xffff and 0x18 raises the absent-member error."""
    asm = """.class Foo props(x, y)
.function main2 0
    CreateObject Foo
    PushConstantString "y"
    GetMemberValueFromString
    Return
.function main 0
    CreateObject Foo
    PushConstantString "nope"
    GetMemberValueFromString
    Return
"""
    vm, res, lines, _ = ok(asm, entry="main2")                         # control: "y" resolves in a fresh VM
    assert res.result == NOTHING
    vm, e, lines, _ = err(asm)
    assert e.message == 'Reference to non-existant member property "nope"'
    assert vm.script.member_cache == [(b"y", 0xFFFF)]
    vm.reset_after_error()
    r = vm.call("main2", [])
    assert not r.ok and vm.last_error.message == 'Reference to non-existant member property "y"'


def test_find_member_id_cache_overflow_is_undefined():               # M1: no bound in the original (+0x1d8)
    names = ["m%d" % i for i in range(51)]
    asm = ".symbols " + " ".join(names) + "\n.class Foo props(" + ", ".join(names) + ")\n.function main 0\n"
    for nm in names:
        asm += '    CreateObject Foo\n    PushConstantString "%s"\n    GetMemberValueFromString\n    Pop\n' % nm
    asm += "    PushNothing\n    Return\n"
    vm, e, lines, _ = err(asm)
    assert e.message == "Undefined behaviour: FindMemberID cache overflow (51st miss)"
    assert len(vm.script.member_cache) == 50


@pytest.mark.parametrize("text,want", [
    (b"-1e-40000", (0.0, False)),          # negative out-of-range exponent: +0.0 whatever the sign (0x65c626-0x65c635)
    (b"0.01e-32767", (0.0, False)),        # adjusted exponent -32769 < -32768 (0x65c5fa-0x65c613)
    (b"0.1e-32767", None),                 # adjusted -32768: in range, ___dec2num runs
    (b"10e32767", (math_inf := float("inf"), False)),   # trailing zero stripped: adjusted 32768 -> +HUGE_VAL
    (b"-5e40000", (-math_inf, False)),
    (b"1e-32768", (0.0, False)),           # the magnitude 32768 > 0x7fff sets the flag during the parse (0x65c539)
])
def test_atof_overflow_range_and_sign(text, want):                  # M2
    got = osi_vm.c_atof(text)
    if want is None:
        assert got[1] is True
    else:
        assert got == want and str(got[0]) == str(want[0])            # +0.0, not -0.0


def test_reset_after_error_rescans_the_frame():                       # M4
    vm, e, lines, _ = err(main_of("PushConstantf32 1.0\nCompareAndBranchIfFalse @z\nz:\nReturn"))
    assert vm.snapshot().frame == 1
    vm.reset_after_error()
    assert vm.snapshot().frame == 0 and vm.depth == 0


def test_run_skips_the_frame_after_eleven_int_zero_passes(tmp_path, capsys):
    """Fix round 2: PM+0x40 parity (toggle 0x4a92fd-0x4a930a, check 0x4a937d, set-on-true 0x4a93b7-0x4a93c1 and
    0x4a94a6-0x4a94bb). A process that returns int 0 on all 11 passes of frame k does not run on frame k+1 and runs
    again on k+2; the K line, the time and the counter of frame k+1 are unchanged (0x4a922d-0x4a92f2, 0x4a95b6)."""
    lines, out = _run_cli(tmp_path, PROC % 100, 3, capsys)
    kinds = [l.split()[0] for l in lines]
    assert kinds == ["K"] + ["C"] * 11 + ["K", "K"] + ["C"] * 11
    ks = [l for l in lines if l.startswith("K ")]
    assert [k.split()[4] for k in ks] == ["00000000", "00000001", "00000002"]       # counter increments every frame
    assert len({k.split()[2] for k in ks}) == 3                                       # time advances every frame
    assert lines[-1].split()[5] == "0010:00000002"                                   # frame 2's call carries k = 2
    assert "process passes: total 22 over 3 frames, max 11, frames with re-runs 2 (first: 0:11 2:11), " \
           "frames skipped 1 (first: 1)" in out


def test_atoi_saturates_like_strtol():
    # _atoi -> _strtol saturates (0x65cd95-0x65cdbb); controller fix after the C++ VM review (all readers had wrapped).
    from osi_vm import c_atoi
    assert c_atoi(b"3000000000") == 0x7FFFFFFF
    assert c_atoi(b"-2147483649") == 0x80000000
    assert c_atoi(b"42") == 42


def test_reset_restores_pc_even_when_the_script_popped_the_marker():
    """D10 / consult §1.0: reset_after_error sets PC = the outer call's marker value. In conformance program
    b-30-cap-underflow the script pops the {0xff, 0} marker itself, then underflows; the next call's marker must still
    save PC 0 (C++ and the conform-trace-cmp agree: `I 0000002e 45    1    1 00ff:00000000`)."""
    asm = """.function main2 0 @m2
.function main 0
    Pop
err:
    Pop
    Return
m2:
    PushConstant0
    Return
"""
    vm, e, lines, info = err(asm)
    assert e.message == "Stack underflow" and e.pc == info["labels"]["err"] and e.depth == 0
    vm.reset_after_error()
    assert vm.snapshot().pc == 0 and vm.depth == 0
    tr = osi_vm.ListTrace()
    vm.trace = vm.heap.trace = tr
    assert vm.call("main2", []).ok
    assert tr.lines[1] == "I %08x 45    1    1 00ff:00000000" % info["labels"]["m2"]
