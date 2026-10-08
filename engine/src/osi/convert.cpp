// M2a-1 T1: the VM's arithmetic (D8', D9) and ConvertVariant's numeric/text conversions (opcodes.md "ConvertVariant").
// The f32 results of 0x70-0x73 are computed EXACTLY with integer arithmetic and then rounded twice as the x87 does:
// round_p (p = the precision-control state, 24 or 64 significant bits, nearest-even, unbounded exponent), then the final
// `fstp dword` (f32: subnormals, overflow to +-inf). No host floating-point operation is involved except for
// non-finite operands (IEEE double, then canonical NaN), so native == wasm32 by construction.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

#include "lomn/osi/variant.h"

namespace lomn::osi {

Variant make_int(int32_t i) { return Variant{kInt, static_cast<uint32_t>(i)}; }
Variant make_float(float f) { return Variant{kFloat, f32_bits(f)}; }
Variant make_float_bits(uint32_t bits) { return Variant{kFloat, bits}; }
Variant make_colour(uint32_t rgba) { return Variant{kColour, rgba}; }
Variant nothing() { return Variant{kNothing, 0}; }
float as_float(Variant v) { return bits_f32(v.value); }
int32_t as_int(Variant v) { return static_cast<int32_t>(v.value); }
uint32_t f32_bits(float f) {
  uint32_t b;
  std::memcpy(&b, &f, 4);
  return b;
}
float bits_f32(uint32_t b) {
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}

namespace {

// A 320-bit unsigned integer, 10 x 32-bit limbs, little-endian. Enough for the exact sum of an int32 and an f32 at any
// exponent distance (31 + 253 bits) and for the 128-bit quotients of the division path.
struct Big {
  uint32_t w[10] = {};
  static Big from(uint64_t m, int shift) {  // m << shift, shift >= 0
    Big b;
    int limb = shift / 32, bit = shift % 32;
    unsigned __int128 v = static_cast<unsigned __int128>(m) << bit;
    for (int i = 0; i < 4 && limb + i < 10; ++i) b.w[limb + i] = static_cast<uint32_t>(v >> (32 * i));
    return b;
  }
  static Big from128(unsigned __int128 v) {
    Big b;
    for (int i = 0; i < 4; ++i) b.w[i] = static_cast<uint32_t>(v >> (32 * i));
    return b;
  }
  bool zero() const {
    for (uint32_t x : w)
      if (x) return false;
    return true;
  }
  int bitlen() const {
    for (int i = 9; i >= 0; --i)
      if (w[i]) {
        int n = 32;
        while (!(w[i] >> (n - 1))) --n;
        return 32 * i + n;
      }
    return 0;
  }
  bool bit(int k) const { return k >= 0 && k < 320 && ((w[k / 32] >> (k % 32)) & 1u); }
  bool any_below(int k) const {  // any bit in [0, k)
    for (int i = 0; i < k && i < 320; ++i)
      if (bit(i)) return true;
    return false;
  }
  uint64_t bits_from(int k, int n) const {  // n <= 64 bits starting at bit k
    uint64_t r = 0;
    for (int i = n - 1; i >= 0; --i) r = (r << 1) | (bit(k + i) ? 1u : 0u);
    return r;
  }
};
int cmp(const Big& a, const Big& b) {
  for (int i = 9; i >= 0; --i)
    if (a.w[i] != b.w[i]) return a.w[i] < b.w[i] ? -1 : 1;
  return 0;
}
Big add(const Big& a, const Big& b) {
  Big r;
  uint64_t c = 0;
  for (int i = 0; i < 10; ++i) {
    c += static_cast<uint64_t>(a.w[i]) + b.w[i];
    r.w[i] = static_cast<uint32_t>(c);
    c >>= 32;
  }
  return r;
}
Big sub(const Big& a, const Big& b) {  // a >= b
  Big r;
  int64_t br = 0;
  for (int i = 0; i < 10; ++i) {
    int64_t d = static_cast<int64_t>(a.w[i]) - b.w[i] - br;
    br = d < 0 ? 1 : 0;
    r.w[i] = static_cast<uint32_t>(d + (br << 32));
  }
  return r;
}

// Encodes (-1)^neg * M * 2^E (M exact, < 2^65) as f32 bits with one nearest-even rounding (the `fstp dword`).
uint32_t to_f32(bool neg, unsigned __int128 M, int E) {
  const uint32_t sign = neg ? 0x80000000u : 0u;
  if (M == 0) return sign;
  int L = 0;
  for (unsigned __int128 t = M; t; t >>= 1) ++L;
  const int lead = E + L - 1;
  if (lead > 127) return sign | 0x7f800000u;
  const int lsb = lead >= -126 ? lead - 23 : -149;
  const int sh = lsb - E;
  uint64_t q;
  if (sh <= 0) {
    q = static_cast<uint64_t>(M << (-sh));
  } else if (sh > 120) {
    q = 0;  // far below half an ulp of the smallest subnormal
  } else {
    q = static_cast<uint64_t>(M >> sh);
    const bool rbit = (M >> (sh - 1)) & 1;
    const unsigned __int128 mask = (static_cast<unsigned __int128>(1) << (sh - 1)) - 1;
    const bool rest = (M & mask) != 0;
    if (rbit && (rest || (q & 1))) ++q;
  }
  int l = lsb;
  if (q >= (1u << 24)) {
    q >>= 1;
    ++l;
  }
  if (q == 0) return sign;
  if (q < (1u << 23)) return sign | static_cast<uint32_t>(q);  // subnormal (l == -149)
  const int field = l + 150;
  if (field > 254) return sign | 0x7f800000u;
  return sign | (static_cast<uint32_t>(field) << 23) | (static_cast<uint32_t>(q) & 0x7fffffu);
}

// round_p (nearest-even, unbounded exponent) of N * 2^E (+ a sticky fraction below N's last bit), then to_f32.
uint32_t round_twice(bool neg, const Big& N, int E, bool sticky, int p) {
  const int L = N.bitlen();
  if (L == 0) return neg ? 0x80000000u : 0u;
  if (L <= p) return to_f32(neg, static_cast<unsigned __int128>(N.bits_from(0, L)), E);  // exact at p (no sticky here)
  const int s = L - p;
  unsigned __int128 M = N.bits_from(s, p);
  const bool rbit = N.bit(s - 1);
  const bool rest = sticky || N.any_below(s - 1);
  if (rbit && (rest || (M & 1))) ++M;  // may become 2^p: still exact
  return to_f32(neg, M, E + s);
}

struct Parts {
  bool neg = false;
  uint64_t m = 0;
  int e = 0;
};
Parts parts(Num n) {
  Parts p;
  if (n.is_int) {
    p.neg = n.i < 0;
    p.m = p.neg ? static_cast<uint64_t>(-static_cast<int64_t>(n.i)) : static_cast<uint64_t>(n.i);
    p.e = 0;
  } else {
    p.neg = (n.f >> 31) != 0;
    const uint32_t ex = (n.f >> 23) & 0xff, fr = n.f & 0x7fffff;
    if (ex == 0) {
      p.m = fr;
      p.e = -149;
    } else {
      p.m = fr | 0x800000u;
      p.e = static_cast<int>(ex) - 150;
    }
  }
  return p;
}
bool finite(Num n) { return n.is_int || ((n.f >> 23) & 0xff) != 0xff; }
double widen(Num n) { return n.is_int ? static_cast<double>(n.i) : static_cast<double>(bits_f32(n.f)); }

}  // namespace

uint32_t canon_nan_bits(uint32_t b) { return ((b & 0x7f800000u) == 0x7f800000u && (b & 0x7fffffu)) ? kCanonicalNaN : b; }
float canon_nan(float f) { return bits_f32(canon_nan_bits(f32_bits(f))); }

uint32_t x87_arith(Op op, Num a, Num b, int fpu_pc) {
  const int p = fpu_pc == 64 ? 64 : 24;
  if (!finite(a) || !finite(b)) {  // inf/NaN operands: IEEE (the x87 agrees except for NaN payloads, D9)
    const double x = widen(a), y = widen(b);
    double r = 0;
    switch (op) {
      case Op::Add: r = x + y; break;
      case Op::Sub: r = x - y; break;
      case Op::Mul: r = x * y; break;
      default: r = x / y; break;
    }
    return canon_nan_bits(f32_bits(static_cast<float>(r)));
  }
  Parts pa = parts(a), pb = parts(b);
  switch (op) {
    case Op::Add:
    case Op::Sub: {
      if (op == Op::Sub) pb.neg = !pb.neg;
      const int emin = pa.e < pb.e ? pa.e : pb.e;
      const Big A = Big::from(pa.m, pa.e - emin), Bv = Big::from(pb.m, pb.e - emin);
      if (A.zero() && Bv.zero()) return (pa.neg && pb.neg) ? 0x80000000u : 0u;  // x + (-x) = +0; -0 + -0 = -0
      if (pa.neg == pb.neg) return round_twice(pa.neg, add(A, Bv), emin, false, p);
      const int c = cmp(A, Bv);
      if (c == 0) return 0u;
      return c > 0 ? round_twice(pa.neg, sub(A, Bv), emin, false, p) : round_twice(pb.neg, sub(Bv, A), emin, false, p);
    }
    case Op::Mul: {
      const bool neg = pa.neg != pb.neg;
      const unsigned __int128 m = static_cast<unsigned __int128>(pa.m) * pb.m;
      if (m == 0) return neg ? 0x80000000u : 0u;
      return round_twice(neg, Big::from128(m), pa.e + pb.e, false, p);
    }
    default: {  // Div: a zero divisor is the handler's error; here x/0 follows IEEE for completeness
      const bool neg = pa.neg != pb.neg;
      if (pb.m == 0) return canon_nan_bits(f32_bits(static_cast<float>(widen(a) / widen(b))));
      if (pa.m == 0) return neg ? 0x80000000u : 0u;
      const unsigned __int128 num = static_cast<unsigned __int128>(pa.m) << 90;  // pa.m < 2^32: num < 2^122
      const unsigned __int128 q = num / pb.m, r = num % pb.m;                       // q >= 2^90 / 2^31: >= 60 bits
      // q has >= 60 bits; p = 64 needs p + 2 bits, so widen once more when short (exact: q*4 + 2*r/m split)
      unsigned __int128 qq = q, rr = r;
      int e = pa.e - pb.e - 90;
      while ((qq >> 66) == 0) {
        rr <<= 1;
        qq = (qq << 1) | (rr >= pb.m ? 1 : 0);
        if (rr >= pb.m) rr -= pb.m;
        --e;
      }
      return round_twice(neg, Big::from128(qq), e, rr != 0, p);
    }
  }
}

float mixed_add(int32_t a, float b, int pc) { return bits_f32(x87_arith(Op::Add, Num::of_int(a), Num::of_float_bits(f32_bits(b)), pc)); }
float mixed_sub_if(int32_t a, float b, int pc) { return bits_f32(x87_arith(Op::Sub, Num::of_int(a), Num::of_float_bits(f32_bits(b)), pc)); }
float mixed_sub_fi(float a, int32_t b, int pc) { return bits_f32(x87_arith(Op::Sub, Num::of_float_bits(f32_bits(a)), Num::of_int(b), pc)); }
float mixed_mul(int32_t a, float b, int pc) { return bits_f32(x87_arith(Op::Mul, Num::of_int(a), Num::of_float_bits(f32_bits(b)), pc)); }
float mixed_div_if(int32_t a, float b, int pc) { return bits_f32(x87_arith(Op::Div, Num::of_int(a), Num::of_float_bits(f32_bits(b)), pc)); }
float mixed_div_fi(float a, int32_t b, int pc) { return bits_f32(x87_arith(Op::Div, Num::of_float_bits(f32_bits(a)), Num::of_int(b), pc)); }

int cmp_exact(double a, double b, Op op) {
  if (std::isnan(a) || std::isnan(b)) return 0;
  switch (op) {
    case Op::Eq: return a == b;
    case Op::Lt: return a < b;
    case Op::Gt: return a > b;
    case Op::Le: return a <= b;
    case Op::Ge: return a >= b;
    default: return 0;
  }
}

int32_t chop_to_int(uint32_t b) {
  const double x = static_cast<double>(bits_f32(b));
  if (std::isnan(x) || std::fabs(x) >= 9223372036854775808.0) return 0;  // integer indefinite 0x8000000000000000
  const int64_t t = static_cast<int64_t>(x);                            // truncation toward zero (chop)
  return static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(t)));
}

uint32_t int_to_f32_bits(int32_t i) { return x87_arith(Op::Add, Num::of_int(i), Num::of_int(0), 24); }

namespace {
bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
std::string upto_nul(const std::string& s) {
  const size_t z = s.find('\0');
  return z == std::string::npos ? s : s.substr(0, z);
}
}  // namespace

int32_t osi_atoi(const std::string& text) {
  const std::string s = upto_nul(text);
  size_t i = 0;
  while (i < s.size() && is_ws(s[i])) ++i;
  bool neg = false;
  if (i < s.size() && (s[i] == '+' || s[i] == '-')) neg = s[i++] == '-';
  // _atoi (0x65cde0) = _strtol(s, 0, 10) (0x65cd40): the magnitude is accumulated with an overflow flag, then clamped by
  // sign to 0x7fffffff / 0x80000000 (0x65cd95-0x65cdbb). It SATURATES; it does not wrap.
  uint64_t v = 0;
  bool over = false;
  while (i < s.size() && is_digit(s[i])) {
    v = v * 10u + static_cast<uint64_t>(s[i++] - '0');
    if (v > 0x80000000ull) over = true, v = 0x80000001ull;
  }
  if (!neg) return (over || v > 0x7fffffffull) ? INT32_MAX : static_cast<int32_t>(v);
  if (over || v > 0x80000000ull) return INT32_MIN;
  return static_cast<int32_t>(0u - static_cast<uint32_t>(v));
}

uint32_t osi_atof_bits(const std::string& text, bool* switches_pc) {
  // The decimal grammar only ([ws][sign](digits[.digits]|.digits)([eE][sign]digits)?): the 2001 CRT has no hex floats,
  // inf or nan text. The matched text goes through strtod (correctly rounded on both hosts), then one f32 rounding.
  const std::string s = upto_nul(text);
  size_t i = 0;
  while (i < s.size() && is_ws(s[i])) ++i;
  const size_t start = i;
  if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
  size_t digits = 0;
  while (i < s.size() && is_digit(s[i])) ++i, ++digits;
  if (i < s.size() && s[i] == '.') {
    ++i;
    while (i < s.size() && is_digit(s[i])) ++i, ++digits;
  }
  if (switches_pc) *switches_pc = true;
  if (digits == 0) return 0;  // nothing parses: 0.0 (___dec2num still ran)
  size_t end = i;
  if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
    size_t j = i + 1;
    bool eneg = false;
    if (j < s.size() && (s[j] == '+' || s[j] == '-')) eneg = s[j++] == '-';
    size_t ed = 0;
    long long ev = 0;
    while (j < s.size() && is_digit(s[j])) {
      if (ev < 1000000) ev = ev * 10 + (s[j] - '0');
      ++j, ++ed;
    }
    if (ed) {
      end = j;
      if (ev > 32767) {  // ___strtold's overflow flag: +-HUGE_VAL or 0.0, without ___dec2num (INFERRED, opcodes.md)
        if (switches_pc) *switches_pc = false;
        const bool mneg = s[start] == '-';
        if (eneg) return mneg ? 0x80000000u : 0u;
        return mneg ? 0xff800000u : 0x7f800000u;
      }
    }
  }
  const std::string num = s.substr(start, end - start);
  const double d = std::strtod(num.c_str(), nullptr);
  return canon_nan_bits(f32_bits(static_cast<float>(d)));
}

std::string format_d(int32_t i) {
  char b[16];
  std::snprintf(b, sizeof b, "%d", i);
  return b;
}

std::string format_f(uint32_t bits) {
  char b[400];
  std::snprintf(b, sizeof b, "%f", static_cast<double>(bits_f32(bits)));
  return b;
}

}  // namespace lomn::osi
