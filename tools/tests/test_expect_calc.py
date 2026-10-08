"""M2a-1 T0: tools/osi/expect_calc.py, the corpus expectation calculator (consult §3.2 rule 3).

The pinned values are the "original" column of docs/osi/opcodes.md "Conformance falsifiers for M2a-1" (11 falsifiers + 2
guards), the D8' cases (controller ruling 2026-10-08: x87 precision control is VM state, 24 bits initially, 64 after the
first string->float conversion), and the 0x75/0x78/0xb2 row falsifiers. Expected f32 bit patterns are computed HERE from
the table's decimal values with `struct` (one f32 rounding of a decimal that is exactly representable), never by
expect_calc itself. The 16777217 + 1.0e-10 / 1.0e-15 bits were measured with a separate rational script
(scratchpad m2a1-t0/pc_readings.py): pc24 -> 4b800001 for both; pc64 -> 4b800001 / 4b800000; the 53-bit formula
(not the original) -> 4b800000 for both."""
import ast
import importlib.util
import math
import struct
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
SRC = REPO / "tools" / "osi" / "expect_calc.py"


@pytest.fixture(scope="module")
def ec():
    spec = importlib.util.spec_from_file_location("expect_calc", SRC)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def fbits(x: float) -> str:
    return "%08x" % struct.unpack("<I", struct.pack("<f", x))[0]


def F(x):
    return ("float", x)


def I(n):
    return ("int", n)


# (row, a, b, op, original) — docs/osi/opcodes.md "Conformance falsifiers for M2a-1", in table order.
FALSIFIERS = [
    ("0x60", I(16777217), F(16777216.0), "=", 0),
    ("0x61", I(16777219), F(16777220.0), "<", 1),
    ("0x62", I(16777217), F(16777216.0), ">", 1),
    ("0x63", I(16777217), F(16777216.0), "<=", 0),
    ("0x64", I(16777219), F(16777220.0), ">=", 0),
    ("0x64", F(16777216.0), I(16777217), ">=", 0),
    ("0x70", I(16777217), F(1.0), "+", fbits(16777218.0)),
    ("0x71", I(16777217), F(1.0), "-", fbits(16777216.0)),
    ("0x72", I(16777217), F(3.0), "*", fbits(50331652.0)),
    ("0x73", I(50331651), F(3.0), "/", fbits(16777216.0)),
    ("0x73", F(1.0e30), F(1.0e-30), "/", fbits(math.inf)),
]


def _value(ec, x):
    return ec.Int(x[1]) if x[0] == "int" else ec.Flt(x[1])


@pytest.mark.parametrize("pc", [24, 64])
@pytest.mark.parametrize("row,a,b,op,want", FALSIFIERS)
def test_conformance_falsifiers_give_the_original_column(ec, row, a, b, op, want, pc):
    A, B = _value(ec, a), _value(ec, b)
    if op in ("=", "<", ">", "<=", ">="):
        got = ec.compare(op, A, B)                      # compares are exact; no precision argument
        assert got == {"type": "0x10", "value": want}, row
    else:
        got = ec.arith(op, A, B, pc=pc)
        assert got == {"type": "0x12", "bits": want}, (row, pc)


def test_guards_0xb1_and_0xb2(ec):
    assert ec.int_to_float(ec.Int(16777217)) == {"type": "0x12", "bits": fbits(16777216.0)}
    assert ec.int_to_float(ec.Int(16777219)) == {"type": "0x12", "bits": fbits(16777220.0)}
    assert ec.float_to_int(ec.Flt(3.0e9)) == {"type": "0x10", "value": (-1294967296) & 0xFFFFFFFF}


def test_d8_prime_precision_cases(ec):
    a = ec.Int(16777217)
    assert ec.arith("+", a, ec.Flt(1.0e-10), pc=24) == {"type": "0x12", "bits": "4b800001"}
    assert ec.arith("+", a, ec.Flt(1.0e-10), pc=64) == {"type": "0x12", "bits": "4b800001"}
    assert ec.arith("+", a, ec.Flt(1.0e-15), pc=24) == {"type": "0x12", "bits": "4b800001"}
    assert ec.arith("+", a, ec.Flt(1.0e-15), pc=64) == {"type": "0x12", "bits": "4b800000"}
    assert ec.arith("-", a, ec.Flt(-1.0e-15), pc=24) == {"type": "0x12", "bits": "4b800001"}
    assert ec.arith("-", a, ec.Flt(-1.0e-15), pc=64) == {"type": "0x12", "bits": "4b800000"}
    with pytest.raises(ValueError):
        ec.arith("+", a, ec.Flt(1.0), pc=53)           # the 53-bit formula is not the original (D8')
    with pytest.raises(TypeError):
        ec.arith("+", a, ec.Flt(1.0))                  # pc must be explicit


def test_integer_rows(ec):
    assert ec.arith("+", ec.Int(0x7FFFFFFF), ec.Int(1), pc=24) == {"type": "0x10", "value": 0x80000000}
    assert ec.arith("*", ec.Int(65536), ec.Int(65536), pc=24) == {"type": "0x10", "value": 0}
    assert ec.arith("/", ec.Int(-7), ec.Int(2), pc=24) == {"type": "0x10", "value": (-3) & 0xFFFFFFFF}
    assert ec.modulus(ec.Int(-1), ec.Int(3)) == {"type": "0x10", "value": 0}            # 0x75 falsifier: unsigned
    assert ec.shift_right(ec.Int(-8), ec.Int(1)) == {"type": "0x10", "value": 0x7FFFFFFC}  # 0x78 falsifier: logical
    assert ec.shift_left(ec.Int(1), ec.Int(33)) == {"type": "0x10", "value": 2}           # count & 31
    for op, b in (("/", ec.Int(0)), ("/", ec.Flt(0.0)), ("/", ec.Flt(-0.0))):
        with pytest.raises(ec.CalcError, match="Divide by 0"):
            ec.arith(op, ec.Int(1), b, pc=24)
    with pytest.raises(ec.CalcError, match="Divide by 0"):
        ec.modulus(ec.Int(1), ec.Int(0))
    with pytest.raises(ec.CalcError, match="undefined"):
        ec.arith("/", ec.Int(-2**31), ec.Int(-1), pc=24)


def test_float_to_int_chop(ec):
    assert ec.float_to_int(ec.Flt(2.7)) == {"type": "0x10", "value": 2}
    assert ec.float_to_int(ec.Flt(-2.7)) == {"type": "0x10", "value": (-2) & 0xFFFFFFFF}
    assert ec.float_to_int(ec.Flt(-3.0e9)) == {"type": "0x10", "value": 1294967296}
    assert ec.float_to_int(ec.Flt(1.0e19)) == {"type": "0x10", "value": 0}            # |x| >= 2^63: indefinite, low dword 0
    assert ec.float_to_int(ec.Flt.bits(0x7FC00000)) == {"type": "0x10", "value": 0}   # NaN


def test_text_conversions(ec):
    assert ec.to_string(ec.Int(-42)) == b"-42"
    assert ec.to_string(ec.Flt(0.5)) == b"0.500000"
    assert ec.to_string(ec.Flt(0.1)) == b"0.100000"          # %f of the f32 widened exactly to double
    assert ec.atoi(b"  -17xyz") == {"type": "0x10", "value": (-17) & 0xFFFFFFFF}
    assert ec.atoi(b"abc") == {"type": "0x10", "value": 0}
    assert ec.atof(b"-2.25") == {"type": "0x12", "bits": fbits(-2.25), "pc_after": 64}
    assert ec.atof(b"zz") == {"type": "0x12", "bits": "00000000", "pc_after": 64}   # the switch happens even for 0


def test_nan_is_canonical_and_values_are_bits(ec):
    nan = ec.Flt.bits(0x7FC00001)
    assert ec.arith("+", nan, ec.Flt(1.0), pc=24) == {"type": "0x12", "bits": "ffc00000"}   # D9
    assert ec.arith("*", ec.Flt(math.inf), ec.Flt(0.0), pc=64) == {"type": "0x12", "bits": "ffc00000"}
    assert ec.compare("=", nan, nan) == {"type": "0x10", "value": 0}
    assert ec.compare("=", ec.Flt(0.0), ec.Flt(-0.0)) == {"type": "0x10", "value": 1}


def test_it_does_not_interpret_bytecode():
    """Consult §3.2 rule 3: the calculator evaluates one row's arithmetic from literal operands. It imports no script
    reader or VM and contains no opcode dispatch."""
    tree = ast.parse(SRC.read_text())
    imported = {a.name.split(".")[0] for n in ast.walk(tree) if isinstance(n, ast.Import) for a in n.names}
    imported |= {n.module.split(".")[0] for n in ast.walk(tree) if isinstance(n, ast.ImportFrom) and n.module}
    assert imported <= {"fractions", "math", "struct", "re", "argparse", "json", "sys", "__future__"}
    text = SRC.read_text()
    for banned in ("osi_vm", "disasm", "osiasm", "0x82", "dispatch", "OsiFile", "bytecode ="):
        assert banned not in text, banned
