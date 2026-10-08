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


# ------------------------------------------------------------------------------------------ fix round 1 (I1, I4, m1, m4)
# I1 (controller-verified with an independent exact-rational script, re-measured here with scratchpad m2a1-t0/ff_pc24.py):
# f32 0x24368a23 * f32 0x1af99066 has a subnormal exact product; pc24 rounds it to 24 bits with the extended exponent,
# then fstp dword rounds again: 002c7cd4; one rounding (and pc 53/64) gives 002c7cd5.
def test_float_float_multiply_is_double_rounded_at_pc24_when_subnormal(ec):
    a, b = ec.Flt.bits(0x24368A23), ec.Flt.bits(0x1AF99066)
    assert ec.arith("*", a, b, pc=24) == {"type": "0x12", "bits": "002c7cd4"}
    assert ec.arith("*", a, b, pc=64) == {"type": "0x12", "bits": "002c7cd5"}


# Re-review M5 (controller re-verified with its own exact-rational script): the float/float divide path, same rule.
def test_float_float_divide_is_double_rounded_at_pc24_when_subnormal(ec):
    a, b = ec.Flt.bits(0x395EC8D0), ec.Flt.bits(0x78C19999)
    assert ec.arith("/", a, b, pc=24) == {"type": "0x12", "bits": "0049a5d2"}
    assert ec.arith("/", a, b, pc=64) == {"type": "0x12", "bits": "0049a5d3"}


def test_f32_bits_subnormals_and_ties(ec):
    from fractions import Fraction
    assert ec.f32_bits(Fraction(1, 2 ** 149)) == 0x00000001          # the smallest subnormal
    assert ec.f32_bits(Fraction(3, 2 ** 150)) == 0x00000002          # 1.5 ulp: tie, to even
    assert ec.f32_bits(Fraction(1, 2 ** 150)) == 0x00000000          # 0.5 ulp: tie, to even (zero)
    assert ec.f32_bits(-Fraction(5, 2 ** 150)) == 0x80000002         # 2.5 ulp: tie, to even
    assert ec.f32_bits(Fraction(2 ** 23 - 1, 2 ** 149)) == 0x007FFFFF  # the largest subnormal
    assert ec.f32_bits(Fraction(1, 2 ** 126)) == 0x00800000           # the smallest normal


def _py53(op, a, b):
    """The 53-bit reading computed independently: (float)((double)a OP (double)b) with Python doubles."""
    r = {"+": a + b, "-": a - b, "*": a * b, "/": a / b}[op]
    return fbits(r)


def _f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


# The D8' and I1 lines of opcodes.md's falsifier table: (id, a, b, op, pc, original, alternative reading, its value)
D8P_LINES = [
    ("0x70#2", I(16777217), F(1.0e-15), "+", 24, "4b800001", "53", None),
    ("0x70#3", I(16777217), F(1.0e-15), "+", 64, "4b800000", "24 forever", 24),
    ("0x70#4", I(16777217), F(1.0e-10), "+", 64, "4b800001", "53", None),
    ("0x72#2", ("bits", 0x24368A23), ("bits", 0x1AF99066), "*", 24, "002c7cd4", "single rounding (= pc 64)", 64),
]


@pytest.mark.parametrize("fid,a,b,op,pc,want,alt,alt_pc", D8P_LINES)
def test_d8_prime_table_lines_and_their_alternatives(ec, fid, a, b, op, pc, want, alt, alt_pc):
    def val(x):
        return ec.Flt.bits(x[1]) if x[0] == "bits" else _value(ec, x)
    A, B = val(a), val(b)
    got = ec.arith(op, A, B, pc=pc)["bits"]
    assert got == want, fid
    if alt_pc is not None:
        other = ec.arith(op, A, B, pc=alt_pc)["bits"]
    else:
        x = float(a[1]) if a[0] == "int" else _f32(a[1])
        y = float(b[1]) if b[0] == "int" else _f32(b[1])
        other = _py53(op, x, y)
    assert other != want, (fid, alt)     # a falsifier: the alternative reading gives a different value


def test_atof_exponent_overflow_skips_the_switch(ec):
    """m1: ___strtold sets its overflow flag when the decimal exponent leaves ±32767 (0x65c539, 0x65c603-0x65c613) and
    returns +-HUGE_VAL (0x7620dc) or 0.0 (0x762538) without calling ___dec2num, so the precision mode is unchanged."""
    assert ec.atof(b"1e99999") == {"type": "0x12", "bits": "7f800000", "pc_after": None}
    assert ec.atof(b"-1e99999") == {"type": "0x12", "bits": "ff800000", "pc_after": None}
    assert ec.atof(b"1e-99999") == {"type": "0x12", "bits": "00000000", "pc_after": None}


def test_percent_f_refuses_values_beyond_18_digits(ec):
    """m4: ___num2dec keeps 18 BCD digits (0x65fa25, 0x65fa57); %f of |x| >= 1e17 is not reproducible here."""
    assert ec.to_string(ec.Flt(1.0e16)) == b"10000000272564224.000000"
    assert ec.to_string(ec.Flt(1.0e17)) == b"99999998430674944.000000"   # f32(1e17) is below 1e17: 17 digits, allowed
    for x in (2.0e17, -3.0e20):
        with pytest.raises(ec.CalcError, match="18 digits"):
            ec.to_string(ec.Flt(x))


# Signed zero (C++ VM review 2026-10-08): an exact zero keeps the x87 sign; before this fix expect_calc returned +0 always.
def test_exact_zero_results_keep_the_x87_sign(ec):
    F, I = ec.Flt, ec.Int
    neg0, pos0 = {"type": "0x12", "bits": "80000000"}, {"type": "0x12", "bits": "00000000"}
    assert ec.arith("*", F(-1.0), I(0), pc=24) == neg0
    assert ec.arith("*", I(0), F(-1.0), pc=24) == neg0
    assert ec.arith("*", F(-0.0), F(-2.0), pc=24) == pos0
    assert ec.arith("/", F(0.0), F(-5.0), pc=24) == neg0
    assert ec.arith("/", F(-0.0), I(5), pc=64) == neg0
    assert ec.arith("+", F(-0.0), F(-0.0), pc=24) == neg0
    assert ec.arith("+", F(1.0), F(-1.0), pc=24) == pos0
    assert ec.arith("+", I(3), F(-3.0), pc=24) == pos0
    assert ec.arith("-", F(-0.0), F(0.0), pc=24) == neg0
    assert ec.arith("-", F(-0.0), F(-0.0), pc=24) == pos0
    assert ec.arith("-", I(3), F(3.0), pc=24) == pos0


# atoi saturates like _strtol (0x65cd95-0x65cdbb); all three readers had wrapped (C++ VM review 2026-10-08).
def test_atoi_saturates(ec):
    assert ec.atoi(b"3000000000") == {"type": "0x10", "value": 2147483647}
    assert ec.atoi(b"-2147483649") == {"type": "0x10", "value": 2147483648}
    assert ec.atoi(b"99999999999") == {"type": "0x10", "value": 2147483647}
    assert ec.atoi(b" -12x") == {"type": "0x10", "value": 4294967284}
