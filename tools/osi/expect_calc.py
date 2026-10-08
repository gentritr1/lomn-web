"""M2a-1 T0: the conformance-expectation calculator (docs/osi/conformance.md, rule 3).

It evaluates ONE table row's arithmetic from literal operands, so a corpus author can compute an `.expect.json` value
without running any interpreter. It reads no script and knows no instruction encoding: the author applies the row (which
operand is popped first, which type wins, which error fires) and calls the matching helper here. Every result is given in
the corpus format: {"type": "0x10", "value": <u32>} or {"type": "0x12", "bits": "<8 hex digits>"}.

Semantics, each from docs/osi/opcodes.md (row cited) and the D8' ruling (2026-10-08):
- Floats are f32 values. Mixed and float arithmetic (rows 0x70-0x73) is computed exactly, then rounded to `pc`
  significant bits with the x87 extended exponent range (unbounded here), then rounded to f32 by the final `fstp dword`:
  result = round_f32(round_pc(exact)). `pc` is the VM's precision-control state (D8'): 24 from CreateDevice, 64 for
  ever after the VM's first string->float conversion (`___dec2num` 0x65f886 leaves PC = 64). It must be passed
  explicitly; 53 (the old `(float)((double)a OP (double)b)` formula) is not the original and is refused.
  ONE rule for every f32 result of 0x70-0x73, whatever the operand types. At pc 24 it equals one rounding of the exact
  result unless the result is an f32 subnormal: the extended exponent keeps the 24-bit intermediate normal, then
  `fstp dword` rounds again to fewer bits. + and - (and int * float) are never affected: their exact result is a
  multiple of 2^-149, so a result below 2^-126 has at most 23 significant bits and the first rounding is exact. x and /
  are (e.g. f32 0x24368a23 * 0x1af99066: pc24 002c7cd4, one rounding 002c7cd5). At pc 64 the result always equals one
  rounding (64 >= 2*24+2). Measured on 20,000 uniform-bit-pattern f32 pairs per op (scratch ff_pc24.py, seed 7):
  pc24 vs one rounding + 0, - 0, * 27, / 18; pc53 and pc64 vs one rounding 0 for every op.
- NaN results are stored as 0xffc00000 (D9). ±inf results come from f32 overflow at the final rounding.
- Compares (0x60-0x64) are exact, unordered -> 0, +0.0 == -0.0; no precision dependence.
- Ints are int32: + - * wrap; / truncates toward zero (signed idiv, 0x73); % is UNSIGNED (0x75); >> is LOGICAL (0x78);
  shift counts use the low 5 bits (x86 shl/shr by cl). Zero divisors raise "Divide by 0" (float ±0.0 included).
- float -> int (0xb2, int native arguments) chops toward zero and keeps the low 32 bits of the 64-bit result; NaN or
  |x| >= 2^63 store the integer indefinite, whose low dword is 0.
- int -> float (0xb1) rounds once. int -> text is "%d"; float -> text is "%f" of the f32 widened exactly to double
  (Python's %f equals libc's here, consult M21; the original's CRT is UNVERIFIED). The original's `___num2dec` keeps 18
  BCD digits (0x65fa25, 0x65fa57), so %f of |x| >= 1e17 is refused. text -> int is atoi, text -> float is atof rounded
  once to f32; atof also switches the VM to pc 64 (D8'), which `atof` reports as "pc_after" (None = unchanged: a
  decimal exponent beyond +-32767 takes ___strtold's overflow path and skips ___dec2num, INFERRED).
"""
from __future__ import annotations

import math
import re
import struct
from fractions import Fraction

NAN_BITS = 0xFFC00000          # D9: the x87 real indefinite
PRECISIONS = (24, 64)          # D8': the two values the VM's precision-control state takes


class CalcError(Exception):
    """The row raises an error here (the message is the row's text) or the case is outside what is defined."""


def _u32(n: int) -> int:
    return n & 0xFFFFFFFF


def _s32(n: int) -> int:
    n &= 0xFFFFFFFF
    return n - (1 << 32) if n & 0x80000000 else n


class Int:
    """An int32 operand (value wrapped to 32 bits)."""

    def __init__(self, n: int):
        self.n = _s32(n)

    def __repr__(self):
        return "Int(%d)" % self.n


class Flt:
    """An f32 operand: Flt(1.5) rounds the Python float to f32 once (as PushConstantf32's literal); Flt.bits(b) takes
    the raw pattern."""

    def __init__(self, x: float):
        self.b = struct.unpack("<I", struct.pack("<f", x))[0]

    @classmethod
    def bits(cls, b: int) -> "Flt":
        f = cls.__new__(cls)
        f.b = b & 0xFFFFFFFF
        return f

    @property
    def value(self) -> float:
        return struct.unpack("<f", struct.pack("<I", self.b))[0]

    def __repr__(self):
        return "Flt.bits(0x%08x)" % self.b


def int_result(n: int) -> dict:
    return {"type": "0x10", "value": _u32(n)}


def float_result(b: int) -> dict:
    return {"type": "0x12", "bits": "%08x" % b}


def round_p(x: Fraction, p: int) -> Fraction:
    """x rounded to p significant bits, ties to even, unbounded exponent."""
    if x == 0:
        return x
    sign = -1 if x < 0 else 1
    a = abs(x)
    e = a.numerator.bit_length() - a.denominator.bit_length()
    if Fraction(2) ** e > a:
        e -= 1
    q = a / Fraction(2) ** (e - p + 1)
    n = q.numerator // q.denominator
    r = q - n
    if r > Fraction(1, 2) or (r == Fraction(1, 2) and n % 2):
        n += 1
    return sign * Fraction(n) * Fraction(2) ** (e - p + 1)


def f32_bits(x: Fraction) -> int:
    """The binary32 pattern of x rounded once: ties to even, subnormals, overflow to ±inf. Zero is +0.0."""
    if x == 0:
        return 0
    sign = 0x80000000 if x < 0 else 0
    a = abs(x)
    e = a.numerator.bit_length() - a.denominator.bit_length()
    if Fraction(2) ** e > a:
        e -= 1
    e = max(e, -126)
    q = a / Fraction(2) ** (e - 23)
    n = q.numerator // q.denominator
    r = q - n
    if r > Fraction(1, 2) or (r == Fraction(1, 2) and n % 2):
        n += 1
    if n == 1 << 24:
        n, e = 1 << 23, e + 1
    if e > 127:
        return sign | 0x7F800000
    if n < 1 << 23:
        return sign | n
    return sign | ((e + 127) << 23) | (n - (1 << 23))


def _special(v) -> bool:
    return isinstance(v, Flt) and not math.isfinite(v.value)


def _exact(v) -> Fraction:
    return Fraction(v.n) if isinstance(v, Int) else Fraction(v.value)


def _check_pc(pc: int) -> None:
    if pc not in PRECISIONS:
        raise ValueError("pc must be 24 or 64 (D8'); %r is not a state the VM takes" % (pc,))


def arith(op: str, a, b, *, pc: int) -> dict:
    """Rows 0x70 (+), 0x71 (-), 0x72 (*), 0x73 (/) on numeric operands. Strings and other types are the row's errors,
    which the author reads from the table."""
    _check_pc(pc)
    if op not in "+-*/" or len(op) != 1:
        raise ValueError("op %r" % op)
    if isinstance(a, Int) and isinstance(b, Int):
        if op == "+":
            return int_result(a.n + b.n)
        if op == "-":
            return int_result(a.n - b.n)
        if op == "*":
            return int_result(a.n * b.n)
        if b.n == 0:
            raise CalcError("Divide by 0")
        if a.n == -2**31 and b.n == -1:
            raise CalcError("undefined: INT_MIN / -1 faults (#DE) in the original")
        q = abs(a.n) // abs(b.n)
        return int_result(q if (a.n < 0) == (b.n < 0) else -q)
    if op == "/" and isinstance(b, Flt) and b.value == 0.0:   # ordered equality: ±0.0 errors, a NaN divisor does not
        raise CalcError("Divide by 0")
    if op == "/" and isinstance(b, Int) and b.n == 0:
        raise CalcError("Divide by 0")
    if _special(a) or _special(b):
        x = float(a.n) if isinstance(a, Int) else a.value
        y = float(b.n) if isinstance(b, Int) else b.value
        try:
            r = {"+": lambda: x + y, "-": lambda: x - y, "*": lambda: x * y, "/": lambda: x / y}[op]()
        except ZeroDivisionError:
            raise CalcError("Divide by 0")
        if math.isnan(r):
            return float_result(NAN_BITS)
        return float_result(struct.unpack("<I", struct.pack("<f", r))[0])
    x, y = _exact(a), _exact(b)
    r = {"+": lambda: x + y, "-": lambda: x - y, "*": lambda: x * y, "/": lambda: x / y}[op]()
    if r == 0:
        # An exact zero result keeps the x87 (IEEE round-to-nearest) sign, which a Fraction cannot carry
        # (C++ VM review, 2026-10-08): ×/÷ → sign(a) xor sign(b); + → −0 only for (−0) + (−0); − → −0 only for (−0) − (+0).
        # An int operand is +0 when zero (fild of 0 gives +0).
        na, nb = _negative(a), _negative(b)
        neg = {"*": na != nb, "/": na != nb, "+": na and nb, "-": na and not nb}[op]
        return float_result(0x80000000 if neg else 0)
    return float_result(f32_bits(round_p(r, pc)))


def _negative(v) -> bool:
    """Sign bit of an operand: an Int is negative iff n < 0; a Flt by its sign bit (so -0.0 counts as negative)."""
    return v.n < 0 if isinstance(v, Int) else math.copysign(1.0, v.value) < 0


def compare(op: str, a, b) -> dict:
    """Rows 0x60 (=, numeric cases), 0x61 (<), 0x62 (>), 0x63 (<=), 0x64 (>=): exact, unordered -> 0."""
    for v in (a, b):
        if isinstance(v, Flt) and math.isnan(v.value):
            return int_result(0)
    if _special(a) or _special(b):
        x = float(a.n) if isinstance(a, Int) else a.value
        y = float(b.n) if isinstance(b, Int) else b.value
    else:
        x, y = _exact(a), _exact(b)
    r = {"=": x == y, "<": x < y, ">": x > y, "<=": x <= y, ">=": x >= y}[op]
    return int_result(1 if r else 0)


def modulus(a: Int, b: Int) -> dict:
    """Row 0x75: unsigned remainder (xor edx, edx; div)."""
    if b.n == 0:
        raise CalcError("Divide by 0")
    return int_result(_u32(a.n) % _u32(b.n))


def shift_left(a: Int, b: Int) -> dict:
    return int_result(_u32(a.n) << (b.n & 31))


def shift_right(a: Int, b: Int) -> dict:
    """Row 0x78: logical shift."""
    return int_result(_u32(a.n) >> (b.n & 31))


def int_to_float(a: Int) -> dict:
    """Row 0xb1 on an int: fild (exact) then one fstp dword rounding."""
    return float_result(f32_bits(Fraction(a.n)))


def float_to_int(a: Flt) -> dict:
    """Row 0xb2 on a float: chop, fistp qword, low 32 bits; NaN or |x| >= 2^63 -> integer indefinite (low dword 0)."""
    x = a.value
    if math.isnan(x) or abs(x) >= 2.0 ** 63:
        return int_result(0)
    return int_result(int(x))   # int() truncates toward zero; the low 32 bits are kept by int_result


def to_string(v) -> bytes:
    """Row 0xb0: int -> "%d", float -> "%f" of the f32 widened to double (libc formatting, UNVERIFIED vs the CRT)."""
    if isinstance(v, Int):
        return b"%d" % v.n
    x = v.value
    if not math.isfinite(x):
        raise CalcError("%f of a non-finite value: the original CRT's text is UNVERIFIED; do not author it")
    if abs(x) >= 1e17:
        raise CalcError("%%f of |x| >= 1e17: ___num2dec keeps only 18 digits (0x65fa25), the CRT text is not modelled")
    return (b"%f" % x)


_WS = b" \t\n\v\f\r"
_INT = re.compile(rb"[+-]?[0-9]+")
_DEC = re.compile(rb"[+-]?(?:[0-9]+\.?[0-9]*|\.[0-9]+)(?:[eE][+-]?[0-9]+)?")


def atoi(text: bytes) -> dict:
    """atoi = _strtol(s, 0, 10) (0x65cde0 -> 0x65cd40): leading white space, optional sign, decimal digits; anything
    else ends the number; no digits -> 0. Out-of-range values SATURATE to 0x7fffffff / 0x80000000 (errno ERANGE),
    0x65cd95-0x65cdbb (corrected 2026-10-08 after the C++ VM review: all three readers had wrapped)."""
    m = _INT.match(text.lstrip(_WS))
    n = int(m.group(0)) if m else 0
    return int_result(max(-2**31, min(2**31 - 1, n)))


def _strtold_overflow(s: str) -> tuple[bool, bool]:
    """___strtold's range test on matched decimal text: (flag, exponent negative). Same digit model as
    the research oracle's _strtold_overflow (0x65c2a0-0x65c3e0 digits, 0x65c539 raw exponent, 0x65c5c1-0x65c613 the
    trailing-zero strip and the range test on exponent + adjustment): at most 20 stored digits, leading zeros skipped,
    an integer digit past the 20th +1, a fraction digit -1 while fewer than 20 are stored, each stripped trailing zero +1."""
    m = re.fullmatch(r"[+-]?([0-9]*)(?:\.([0-9]*))?(?:[eE]([+-]?)([0-9]+))?", s)
    stored, adj, flag = "", 0, False
    for c in m.group(1).lstrip("0"):
        if len(stored) < 20:
            stored += c
        else:
            adj += 1
    for c in m.group(2) or "":
        if len(stored) < 20:
            if c != "0" or stored:
                stored += c
            adj -= 1
    exp, exp_neg = 0, m.group(3) == "-"
    for c in m.group(4) or "":
        exp = (exp * 10 + int(c)) & 0xFFFFFFFF
        if _s32(exp) > 0x7FFF:
            flag = True
    exp = _s32(-exp if exp_neg else exp)
    adj += len(stored) - len(stored.rstrip("0"))
    return flag or not -32768 <= exp + adj <= 32767, exp_neg


def atof(text: bytes, inexact_ok: bool = False) -> dict:
    """atof then one f32 rounding (row 0xb1 on a string). Only decimal text is defined here; a value that is not
    exactly an f32 raises unless inexact_ok (the CRT's decimal->double step is UNVERIFIED; consult risk 3). The VM's
    precision state is 64 afterwards in every case (D8'): "pc_after"."""
    m = _DEC.match(text.lstrip(_WS))
    if not m:
        return dict(float_result(0), pc_after=64)
    s = m.group(0).decode("ascii")
    flag, exp_neg = _strtold_overflow(s)
    if flag:
        # ___strtold's overflow flag (0x65c539 raw exponent > 0x7fff; 0x65c603-0x65c613 ADJUSTED exponent outside
        # [-32768, 32767]): +-HUGE_VAL (0x7620dc) for a positive exponent, +0.0 (0x762538) for a negative one, without
        # ___dec2num: the precision state is unchanged.
        if exp_neg:
            return dict(float_result(0), pc_after=None)
        return dict(float_result(0xFF800000 if s.startswith("-") else 0x7F800000), pc_after=None)
    exact = Fraction(s) if not s.endswith(".") else Fraction(s[:-1] or "0")
    b = f32_bits(exact)
    # The f32 store overflows to +-inf or flushes to +-0 for text beyond the f32 range (the oracle's c_atof then f32_bits
    # returns those bits as they are). Those bits have no finite Fraction, so the exactness check does not apply to them.
    out_of_range = (b & 0x7FFFFFFF) in (0, 0x7F800000)
    if not out_of_range and Fraction(struct.unpack("<f", struct.pack("<I", b))[0]) != exact and not inexact_ok:
        raise CalcError("atof(%r) is not exactly an f32: the CRT reading is UNVERIFIED" % s)
    if exact == 0 and s.lstrip().startswith("-"):
        b = 0x80000000
    return dict(float_result(b), pc_after=64)
