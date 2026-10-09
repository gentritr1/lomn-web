// M2a-2 T7a: the "services and time" native group (engine/src/osi/natives/services.cpp), one asm-pin test and one
// behaviour test per native, written before the bindings (RED, then GREEN). Every pin reads the canonical exe (or a DLL
// of the Beta 1.5.0 mods) as DATA through $LOMN_DATA; without it the pin tests report SKIPPED (no LOMN_DATA) and pass.
// The bytes below were read with `objdump -d --x86-asm-syntax=intel` of LEGOBionicle.exe (and BetterSaver.dll /
// ijl15.dll) on 2026-10-09; each pin names its instruction address.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "doctest.h"
#include "lomn/fs.h"
#include "lomn/osi/heap.h"
#include "lomn/osi/host.h"
#include "lomn/osi/json.h"
#include "lomn/osi/natives.h"
#include "lomn/osi/registry.h"
#include "lomn/osi/services.h"
#include "lomn/osi/trace.h"
#include "lomn/osi/vm.h"
#include "lomn/sage/blockfile.h"
#include "osi_test_asm.h"

using namespace lomn;
using namespace lomn::osi;
using lomn::test::Asm;

namespace {

// ---------------------------------------------------------------------------------------------- the PE image reader
struct PeImage {
  std::vector<uint8_t> d;
  uint32_t base = 0;
  struct Sec {
    uint32_t va, vsz, rsz, rptr;
  };
  std::vector<Sec> secs;
  uint32_t le32(size_t o) const { return d[o] | d[o + 1] << 8 | d[o + 2] << 16 | static_cast<uint32_t>(d[o + 3]) << 24; }
  uint32_t le16(size_t o) const { return d[o] | d[o + 1] << 8; }
  bool load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    d.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    if (d.size() < 0x100 || d[0] != 'M' || d[1] != 'Z') return false;
    const size_t pe = le32(0x3c);
    if (pe + 24 > d.size() || d[pe] != 'P') return false;
    const size_t n = le16(pe + 6), opt = le16(pe + 20);
    base = le32(pe + 24 + 28);
    for (size_t i = 0; i < n; ++i) {
      const size_t o = pe + 24 + opt + i * 40;
      if (o + 40 > d.size()) return false;
      secs.push_back({le32(o + 12), le32(o + 8), le32(o + 16), le32(o + 20)});
    }
    return true;
  }
  // bytes at the absolute virtual address `va`; zero past a section's raw data (.bss), false outside every section
  bool read(uint32_t va, size_t n, std::vector<uint8_t>& out) const {
    out.assign(n, 0);
    for (size_t i = 0; i < n; ++i) {
      const uint32_t rva = va + static_cast<uint32_t>(i) - base;
      bool hit = false;
      for (const Sec& s : secs)
        if (rva >= s.va && rva < s.va + std::max(s.vsz, s.rsz)) {
          hit = true;
          if (rva - s.va < s.rsz && s.rptr + (rva - s.va) < d.size()) out[i] = d[s.rptr + (rva - s.va)];
          break;
        }
      if (!hit) return false;
    }
    return true;
  }
  std::string hex(uint32_t va, size_t n) const {
    std::vector<uint8_t> b;
    if (!read(va, n, b)) return "<outside the image>";
    std::string s;
    char t[4];
    for (size_t i = 0; i < n; ++i) {
      std::snprintf(t, sizeof t, "%02x", b[i]);
      s += (i ? " " : "") + std::string(t);
    }
    return s;
  }
  uint64_t u64(uint32_t va) const {
    std::vector<uint8_t> b;
    read(va, 8, b);
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = v << 8 | b[static_cast<size_t>(i)];
    return v;
  }
  uint32_t u32(uint32_t va) const { return static_cast<uint32_t>(u64(va)); }
  double dbl(uint32_t va) const {
    const uint64_t u = u64(va);
    double x;
    std::memcpy(&x, &u, 8);
    return x;
  }
  std::string cstr(uint32_t va) const {
    std::string s;
    std::vector<uint8_t> b;
    for (uint32_t a = va; read(a, 1, b) && b[0]; ++a) s += static_cast<char>(b[0]);
    return s;
  }
};

const PeImage* image(const char* rel) {  // rel to $LOMN_DATA; nullptr when absent
  static std::map<std::string, std::unique_ptr<PeImage>> cache;
  const char* ld = std::getenv("LOMN_DATA");
  if (!ld || !*ld) return nullptr;
  auto it = cache.find(rel);
  if (it == cache.end()) {
    auto im = std::make_unique<PeImage>();
    if (!im->load(std::string(ld) + "/" + rel)) im.reset();
    it = cache.emplace(rel, std::move(im)).first;
  }
  return it->second.get();
}
const PeImage* exe() { return image("reference/LEGOBionicle.exe"); }
const PeImage* bettersaver() { return image("install/mods/BetterSaver.dll"); }
const PeImage* ijl15() { return image("install/ijl15.dll"); }

#define SKIP_WITHOUT(img)                          \
  if (!(img)) {                                    \
    MESSAGE("SKIPPED (no LOMN_DATA)");             \
    return;                                        \
  }

// one pinned instruction (or byte run): the hex bytes at the address
#define PIN(img, va, bytes) CHECK_MESSAGE((img)->hex((va), sizeof(bytes) / 3) == bytes, "pin at " #va)

// The AND-mask / shift pairs of ConvertStringToID (0x5fb08d..0x5fb0b2) and IdentifierToString (0x6178d7..0x617908),
// decoded from the exe's own immediates: `81 /4 imm32` (and r, imm32), `c1 /4 imm8` (shl), `c1 /5 imm8` (shr).
struct MaskShift {
  uint32_t mask;
  int shift;  // + shl, - shr
};
bool decode_mask_shift(const PeImage& im, uint32_t and_va, uint32_t sh_va, MaskShift& out) {
  std::vector<uint8_t> a, s;
  if (!im.read(and_va, 6, a) || !im.read(sh_va, 3, s)) return false;
  if (a[0] != 0x81 || ((a[1] >> 3) & 7) != 4) return false;  // and r32, imm32
  if (s[0] != 0xc1) return false;
  const int reg = (s[1] >> 3) & 7;  // 4 = shl, 5 = shr
  if (reg != 4 && reg != 5) return false;
  out.mask = a[2] | a[3] << 8 | a[4] << 16 | static_cast<uint32_t>(a[5]) << 24;
  out.shift = reg == 4 ? s[2] : -static_cast<int>(s[2]);
  return true;
}
uint32_t apply_mask_shift(const std::vector<MaskShift>& ms, uint32_t v) {
  uint32_t r = 0;
  for (const MaskShift& m : ms) r |= m.shift >= 0 ? (v & m.mask) << m.shift : (v & m.mask) >> -m.shift;
  return r;
}
bool decode_all(const PeImage& im, const uint32_t ands[4], const uint32_t shifts[4], std::vector<MaskShift>& out) {
  out.clear();
  for (int i = 0; i < 4; ++i) {
    MaskShift m;
    if (!decode_mask_shift(im, ands[i], shifts[i], m)) return false;
    out.push_back(m);
  }
  return true;
}

// ---------------------------------------------------------------------------------------------- the registry rows
const char* const kRows = R"([
 {"cls": "ScSLOSIFoundation", "name": "StringToIdentifier", "min": 1, "max": 1, "types": ["string"], "ret": "int", "winner": true},
 {"cls": "ScSLOSIFoundation", "name": "IdentifierToString", "min": 1, "max": 1, "types": ["int"], "ret": "string", "winner": true},
 {"cls": "ScSLOSIFileIO", "name": "DoesFileExist", "min": 1, "max": 1, "types": ["string"], "ret": "int", "winner": true},
 {"cls": "GcTimer", "name": "Create", "min": 3, "max": 3, "types": ["int", "int", "float"], "ret": "game", "ret_class": "GcTimer", "winner": true},
 {"cls": "ScProcessManager", "name": "UseRealDeltaTime", "min": 1, "max": 1, "types": ["int"], "ret": "none", "winner": true},
 {"cls": "GcGame", "name": "GetResolutionWidth", "min": 0, "max": 0, "types": [], "ret": "float", "winner": true},
 {"cls": "GcGame", "name": "GetResolutionHeight", "min": 0, "max": 0, "types": [], "ret": "float", "winner": true}
])";
const char* const kTypes = R"({"types": [{"name": "type:GcTimer", "id": 39}]})";

struct Svc {
  sage::OsiFile file;
  StringTrace tr, log;
  std::unique_ptr<ScriptHost> host;
  CallResult res;
  Svc(Asm& a, const fs::FileSystem* vfs = nullptr, HostFlags flags = HostFlags{}, std::function<double()> clock = {},
      std::function<void(ScriptHost&)> pre = {}) {
    std::string err;
    REQUIRE_MESSAGE(file.parse(a.build(), &err), err);
    ScriptHost::Options opt;
    opt.real_natives = true;
    opt.flags = flags;
    opt.clock = clock;
    host = std::make_unique<ScriptHost>(file, kRows, "", kTypes, "", vfs, &tr, &log, opt);
    if (pre) pre(*host);
    res = host->setup();
  }
  Variant value() {
    REQUIRE_MESSAGE(res.ok, (host->vm().last_error() ? host->vm().last_error()->message : std::string("?")));
    return res.result;
  }
  std::string text(Variant v) {  // the host disposes the result after the R record: its text is result_text
    REQUIRE(v.type == kString);
    return res.result_text;
  }
  bool log_has(const std::string& p) const {
    for (const auto& l : log.lines)
      if (l.find(p) != std::string::npos) return true;
    return false;
  }
  bool trace_has(const std::string& p) const {
    for (const auto& l : tr.lines)
      if (l.rfind(p, 0) == 0) return true;
    return false;
  }
};

// A wrapper around the bound gctimer.create that copies the timer's fields at the native's return (the host disposes the
// script's only variant when Setup returns, which deletes a timer the engine does not hold).
struct TimerProbe {
  bool seen = false;
  int32_t id = 0, engine_refs = -1;
  double duration = -1;
  bool started = false;
  NativeFn real = nullptr;
  void* real_user = nullptr;
};
TimerProbe g_probe;
Variant probe_timer_create(NativeCall& c, void* user) {
  Variant v = g_probe.real(c, g_probe.real_user);
  HostContext& ctx = *static_cast<HostContext*>(user);
  if (GcTimer* t = dynamic_cast<GcTimer*>(ctx.object(v.value))) {
    g_probe.seen = true;
    g_probe.id = t->id();
    g_probe.duration = t->duration();
    g_probe.started = t->started();
    g_probe.engine_refs = t->engine_refs();
  }
  return v;
}
void install_timer_probe(ScriptHost& h) {
  g_probe = TimerProbe{};
  const NativeEntry* e = h.registry().find("gctimer", "create");
  REQUIRE(e);
  g_probe.real = e->fn;
  g_probe.real_user = e->user;
  h.registry().bind("gctimer.create", probe_timer_create, &h.context(), Tier::Implemented);
}

Variant call_str(const char* cls, const char* fn, const std::string& arg, const fs::FileSystem* vfs = nullptr) {
  Asm a;
  a.fn("setup", 0).s(arg).call(cls, fn, 1).ret();
  Svc s(a, vfs);
  return s.value();
}

// A BLKF container: header + 56-byte entries {name[40], blk size, full size, flags, offset}, no entry data.
std::vector<uint8_t> make_blk(const std::vector<std::string>& names) {
  std::vector<uint8_t> b = {'B', 'L', 'K', 'F', 1, 0, 0, 0};
  const uint32_t n = static_cast<uint32_t>(names.size());
  for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(n >> (8 * i)));
  for (const std::string& nm : names) {
    std::vector<uint8_t> e(56, 0);
    for (size_t i = 0; i < nm.size() && i < 39; ++i) e[i] = static_cast<uint8_t>(nm[i]);
    e[48] = 2;  // flags
    b.insert(b.end(), e.begin(), e.end());
  }
  return b;
}

std::string slurp(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

}  // namespace

// ============================== R-1: the fail-closed check counts only non-selftest groups ==============================

TEST_CASE("osi services R-1: bind_natives reports game bodies apart from the selftest group's") {
  // a registry that has ONLY the selftest row: the selftest group binds its body, but that is no game native
  const char* selftest_row = R"([{"cls": "T3SelfTest", "name": "Ping", "min": 0, "max": 0, "types": [], "ret": "int", "winner": true}])";
  Asm a;
  a.fn("setup", 0).call("t3selftest", "ping", 0).ret();
  sage::OsiFile file;
  std::string err;
  REQUIRE_MESSAGE(file.parse(a.build(), &err), err);
  StringTrace tr;
  ScriptHost::Options stub;
  ScriptHost host(file, selftest_row, "", R"({"types": []})", "", nullptr, &tr, nullptr, stub);
  const BindReport rep = bind_natives(host.registry(), host.context());
  CHECK(rep.bodies == 1);       // the selftest body
  CHECK(rep.game_bodies == 0);  // R-1: it does not count
  CHECK(rep.game_groups_binding == 0);
  // the services group's rows are game natives
  const BindReport rep2 = [&] {
    StringTrace t2;
    ScriptHost h2(file, kRows, "", kTypes, "", nullptr, &t2, nullptr, stub);
    return bind_natives(h2.registry(), h2.context());
  }();
  CHECK(rep2.game_bodies == 7);
  CHECK(rep2.game_groups_binding == 1);
  // and the host fails closed on a selftest-only registry under real_natives
  ScriptHost::Options real;
  real.real_natives = true;
  bool threw = false;
  std::string what;
  try {
    ScriptHost h3(file, selftest_row, "", R"({"types": []})", "", nullptr, &tr, nullptr, real);
  } catch (const std::runtime_error& e) {
    threw = true;
    what = e.what();
  }
  CHECK(threw);
  CHECK(what.find("no native group binds a body") != std::string::npos);
}

// ============================== stringtoidentifier ==============================

TEST_CASE("osi services stringtoidentifier asm pin: ConvertStringToID 0x5fb060 (length 4, shift immediates, flag discarded)") {
  const PeImage* im = exe();
  SKIP_WITHOUT(im);
  PIN(im, 0x5fb06e, "3d 04 00 00 00");        // cmp eax, 4 : the length test
  PIN(im, 0x5fb075, "c7 07 ff ff ff ff");     // mov [edi], 0xffffffff : any other length
  PIN(im, 0x5fb089, "8b 32");                 // mov esi, [edx] : the first four data bytes as one dword
  PIN(im, 0x5fb0b5, "09 cb 09 d3 09 f3");     // the three ors
  PIN(im, 0x617882, "39 15 98 57 75 00");     // cmp [0x755798], edx : the wrapper's validity flag ...
  PIN(im, 0x617888, "0f 94 c0");              // sete al
  PIN(im, 0x61788b, "89 1e");                 // ... which is not stored: the id (ebx) is
  PIN(im, 0x61771d, "e8 ee 00 00 00");        // body 0x617700 -> StringToIdentifier 0x617810
  PIN(im, 0x617868, "e8 f3 37 fe ff");        // 0x617810 -> ConvertStringToID 0x5fb060
  PIN(im, 0x40187c, "85 db");                 // assign(p, npos): test ebx
  PIN(im, 0x401874, "3b 3d 44 6a 72 00");     // cmp edi, [0x726a44] : npos ...
  CHECK(im->u32(0x726a44) == 0xffffffffu);    // ... = 0xffffffff
  PIN(im, 0x401881, "e8 3a 5b 25 00");        // call 0x6573c0 : strlen
  PIN(im, 0x401894, "3b 7e 08");              // cmp edi, [esi+8] : the capacity ...
  PIN(im, 0x401897, "0f 93 c0");              // setae : n >= capacity throws
  PIN(im, 0x617831, "83 c3 0c");              // (local string layout {data, len, cap, buf[]}: +0xc ...
  PIN(im, 0x61783c, "c7 42 08 ff 00 00 00");  // ... capacity 0xff)
  // the shift immediates (first character in the high byte): decoded from the exe and applied to "frnt"
  const uint32_t ands[4] = {0x5fb08d, 0x5fb098, 0x5fb0a3, 0x5fb0ac}, shifts[4] = {0x5fb093, 0x5fb09e, 0x5fb0a9, 0x5fb0b2};
  std::vector<MaskShift> ms;
  REQUIRE(decode_all(*im, ands, shifts, ms));
  CHECK(ms[0].shift == 24);
  CHECK(ms[1].shift == 8);
  CHECK(ms[2].shift == -8);
  CHECK(ms[3].shift == -24);
  const uint32_t frnt_le = 'f' | 'r' << 8 | 'n' << 16 | static_cast<uint32_t>('t') << 24;  // the dword the load at 0x5fb089 reads
  CHECK(apply_mask_shift(ms, frnt_le) == 0x66726e74u);
  // the model agrees with the exe's immediates on a spread of strings
  for (const char* s : {"frnt", "abcd", "\x01\x02\x03\x04", "\xff\x80\x7f\x00", "A1b2"}) {
    uint32_t le = 0;
    for (int i = 3; i >= 0; --i) le = le << 8 | static_cast<uint8_t>(s[i]);
    CHECK(convert_string_to_id(std::string(s, 4)) == apply_mask_shift(ms, le));
  }
}

TEST_CASE("osi services stringtoidentifier: \"frnt\" -> 0x66726e74; any other length -> 0xffffffff; the flag is discarded") {
  CHECK(call_str("scslosifoundation", "stringtoidentifier", "frnt") == make_int(0x66726e74));
  CHECK(call_str("scslosifoundation", "stringtoidentifier", "x") == make_int(-1));
  CHECK(call_str("scslosifoundation", "stringtoidentifier", "") == make_int(-1));
  CHECK(call_str("scslosifoundation", "stringtoidentifier", "frnts") == make_int(-1));
  CHECK(call_str("scslosifoundation", "stringtoidentifier", "cnt1") == make_int(0x636e7431));  // MainCharacterID ('cnt1')
  CHECK(call_str("scslosifoundation", "stringtoidentifier", "onua") == make_int(0x6f6e7561));
  // C-string semantics: the copy stops at the first NUL (assign(p, npos) = strlen), so the length is measured there
  CHECK(osi_cstring(std::string("ab\0cd", 5)) == "ab");
  CHECK(convert_string_to_id(osi_cstring(std::string("abc\0d", 5))) == 0xffffffffu);
  CHECK(convert_string_to_id(osi_cstring(std::string("abcd\0", 5))) == 0x61626364u);
  CHECK(std::string(osi_cstring(std::string(254, 'x'))).size() == 254);
  bool threw = false;
  try {
    osi_cstring(std::string(255, 'x'));  // n >= capacity 255 (0x401894-0x4018a8)
  } catch (const VmError& e) {
    threw = e.message == "kOutOfRange";
  }
  CHECK(threw);
  // the log line of the call: an int result
  Asm a;
  a.fn("setup", 0).s("frnt").call("scslosifoundation", "stringtoidentifier", 1).ret();
  Svc s(a);
  REQUIRE(s.res.ok);
  CHECK(s.log_has("scslosifoundation.stringtoidentifier 1 -> 0010:66726e74"));
}

// ============================== identifiertostring ==============================

TEST_CASE("osi services identifiertostring asm pin: IdentifierToString 0x6178c0 is a byte swap with no table") {
  const PeImage* im = exe();
  SKIP_WITHOUT(im);
  PIN(im, 0x6177c2, "e8 f9 00 00 00");        // body 0x6177a0 -> IdentifierToString 0x6178c0
  PIN(im, 0x6177d4, "e8 27 66 e5 ff");        // -> CreateVariant(string) 0x46de00
  PIN(im, 0x617938, "6a 04");                 // push 4 : the explicit length of the first assign (a NUL is kept here) ...
  PIN(im, 0x61796b, "a1 44 6a 72 00");        // ... and npos for the copy into the result (strlen : cut at the first NUL)
  PIN(im, 0x61790d, "89 8d e4 fe ff ff");     // mov [ebp-0x11c], ecx : the swapped dword is stored little-endian ...
  PIN(im, 0x61793a, "8d 85 e4 fe ff ff");     // ... and its four bytes are what assign copies
  const uint32_t ands[4] = {0x6178d7, 0x6178e0, 0x6178f1, 0x617902}, shifts[4] = {0x6178dd, 0x6178e6, 0x6178f7, 0x617908};
  std::vector<MaskShift> ms;
  REQUIRE(decode_all(*im, ands, shifts, ms));
  CHECK(apply_mask_shift(ms, 0x66726e74u) == 0x746e7266u);  // the byte swap
  for (uint32_t id : {0x66726e74u, 0x636e7431u, 0x00000000u, 0x66000000u, 0x00727374u, 0x12345678u, 0xffffffffu, 0x41004243u}) {
    const uint32_t sw = apply_mask_shift(ms, id);
    std::string want;
    for (int i = 0; i < 4; ++i) {
      const char c = static_cast<char>(sw >> (8 * i));
      if (c == 0) break;  // the result copy is strlen-cut
      want += c;
    }
    CHECK(identifier_to_string(id) == want);
  }
}

TEST_CASE("osi services identifiertostring: the inverse of stringtoidentifier; a NUL byte cuts the text; no unknown-id text") {
  Asm a;
  a.fn("setup", 0).i32(0x66726e74).call("scslosifoundation", "identifiertostring", 1).ret();
  Svc s(a);
  const Variant v = s.value();
  CHECK(s.text(v) == "frnt");
  CHECK(s.trace_has("A s "));
  CHECK(identifier_to_string(0) == "");
  CHECK(identifier_to_string(0x66000000u) == "f");
  CHECK(identifier_to_string(0x00727374u) == "");
  CHECK(identifier_to_string(0xffffffffu) == std::string(4, '\xff'));
  CHECK(identifier_to_string(0x41004243u) == "A");
  // round trip over every 4-byte string of the model's domain that holds no NUL, sampled across the byte space
  uint32_t x = 1;
  for (int it = 0; it < 100000; ++it, x = x * 1664525u + 1013904223u) {  // an LCG walk over the 32-bit space
    std::string s4;
    bool nul = false;
    for (int i = 3; i >= 0; --i) {
      const uint8_t c = static_cast<uint8_t>(x >> (8 * i));
      nul |= c == 0;
      s4 += static_cast<char>(c);
    }
    if (nul) continue;
    CHECK(identifier_to_string(convert_string_to_id(s4)) == s4);
    CHECK(convert_string_to_id(identifier_to_string(x)) == x);
  }
}

TEST_CASE("osi services identifiertostring: round trip over every 4-character string of Base.osi's string table") {
  const char* ld = std::getenv("LOMN_DATA");
  if (!ld || !*ld) {
    MESSAGE("SKIPPED (no LOMN_DATA)");
    return;
  }
  const std::string raw = slurp(std::string(ld) + "/install/data/Base.osi");
  REQUIRE(!raw.empty());
  sage::OsiFile osi;
  std::string err;
  REQUIRE_MESSAGE(osi.parse(std::vector<uint8_t>(raw.begin(), raw.end()), &err), err);
  size_t n4 = 0;
  for (const std::string& s : osi.strings()) {
    if (s.size() != 4 || s.find('\0') != std::string::npos) continue;
    ++n4;
    const uint32_t id = convert_string_to_id(osi_cstring(s));
    CHECK(id != 0xffffffffu);
    CHECK(identifier_to_string(id) == s);
  }
  MESSAGE("4-character strings in Base.osi's table: " << n4);
  CHECK(n4 > 0);
}

// ============================== doesfileexist ==============================

TEST_CASE("osi services doesfileexist asm pin: SrBlockFileExists 0x54ddc0 and the BetterSaver hook 0x10001da0") {
  const PeImage* im = exe();
  SKIP_WITHOUT(im);
  PIN(im, 0x615969, "e8 82 05 00 00");        // body 0x615950 -> DoesFileExist 0x615ef0
  PIN(im, 0x615ef9, "e8 c2 7e f3 ff");        // 0x615ef0 -> SrBlockFileExists 0x54ddc0
  PIN(im, 0x615974, "e8 b7 25 df ff");        // result through CreateVariant 0x407f30 (movzx byte -> {0x10, 0|1})
  PIN(im, 0x407f35, "66 c7 03 10 00");
  PIN(im, 0x54ddc7, "b9 b8 b0 83 00");        // mov ecx, 0x83b0b8 : the Art block file object ...
  PIN(im, 0x43b033, "b9 b8 b0 83 00");        // ... the one GcGame::LoadArtBlockHeader loads
  PIN(im, 0x43b041, "68 23 35 70 00");        // push 0x703523 :
  CHECK(im->cstr(0x703523) == "Root/Data/Art/Art.blk");
  PIN(im, 0x60fff0, "89 ca 83 7a 10 00 0f 95 c0 c3");  // IsHeaderLoaded: [this+0x10] != 0
  PIN(im, 0x54ddcc, "e8 1f 22 0c 00");        // call IsHeaderLoaded (0x60fff0)
  PIN(im, 0x54dddb, "e8 e0 20 0c 00");        // call Find (0x60fec0) on 0x83b0b8
  PIN(im, 0x54dde2, "7c 0c");                 // jl : not found -> the slots
  PIN(im, 0x54ddf0, "83 3d 28 61 73 00 00");  // cmp [0x736128], 0 : the current block file slot
  PIN(im, 0x54ddf7, "74 47");                 // je 0x54de40 : null -> FileExists
  PIN(im, 0x54de11, "83 3d 2c 61 73 00 00");  // cmp [0x73612c], 0 : the second slot (only after the current one missed)
  PIN(im, 0x54de40, "53 e8 2a 00 00 00");     // push ebx ; call FileExists 0x54de70
  PIN(im, 0x60fef6, "80 3c 13 5c");           // Find: cmp byte [ebx+edx], '\\' ...
  PIN(im, 0x60fefc, "80 3c 13 2f");           // ... '/' : the basename
  PIN(im, 0x60ff16, "6b d3 38");              // imul edx, ebx, 0x38 : the entry stride
  // the BetterSaver replacement (DLL as data)
  const PeImage* bs = bettersaver();
  REQUIRE(bs);
  PIN(bs, 0x10001dbe, "ff 15 9c 80 00 10");   // call [strstr]
  CHECK(bs->cstr(0x1000833c) == "User/");     // push 0x1000833c at 0x10001db7
  PIN(bs, 0x10001db7, "68 3c 83 00 10");
  PIN(bs, 0x10001dc9, "74 58");               // je 0x10001e23 : no "User/" -> the original (trampoline 0x1000a7b8)
  PIN(bs, 0x10001e24, "ff 15 b8 a7 00 10");
  PIN(bs, 0x10001de2, "ff 15 3c 81 00 10");   // call ResolveGamePath (imported from ijl15.dll)
  PIN(bs, 0x10001df8, "83 f8 ff");            // cmp eax, -1 : GetFileAttributesA failed -> 0
  PIN(bs, 0x10001dfd, "a8 10");               // test al, 0x10 : a directory -> 0
  PIN(bs, 0x10001e01, "b0 01");               // mov al, 1
  const PeImage* ij = ijl15();
  REQUIRE(ij);
  CHECK(ij->cstr(0x1001e5b4) == "Root/");     // ResolveGamePath's strstr needles
  CHECK(ij->cstr(0x1001e5bc) == "User/");
  PIN(ij, 0x100015b2, "3b fb");               // cmp edi, ebx : "Root/" found at the start of the path
  PIN(ij, 0x100015cc, "3b cb");               // cmp ecx, ebx : "User/" found at the start
}

TEST_CASE("osi services doesfileexist: Art.blk first (basename, case folded), then a loose file; the hook for \"User/\"") {
  fs::MemoryFileSystem mem;
  REQUIRE(mem.mount("Root/Data/Art/Art.blk", make_blk({"alpha.tga", "Beta.TGA", "zeta.x"})));
  REQUIRE(mem.mount("Root/Data/loose.txt", {'x'}));
  REQUIRE(mem.mount("Root/Data/User/alpha.tga", {'x'}));
  const uint8_t one = 1;
  REQUIRE(mem.write("User/data/player0.sav", &one, 1) == fs::Error::None);
  auto exists = [&](const std::string& n) { return call_str("scslosifileio", "doesfileexist", n, &mem); };
  CHECK(exists("Root/Data/Art/ALPHA.TGA") == make_int(1));  // Find: basename, 'A'..'Z' folded
  CHECK(exists("Root/Whatever/beta.tga") == make_int(1));
  CHECK(exists("Root\\Data\\zeta.X") == make_int(1));       // either slash
  CHECK(exists("Root/Data/AreaViewer.txt") == make_int(0));
  CHECK(exists("Root/Data/loose.txt") == make_int(1));      // FileExists on the loose file (no current block file)
  CHECK(exists("Root/Data/missing.txt") == make_int(0));
  // BetterSaver: a path holding "User/" never reaches SrBlockFileExists - disk only (the Art.blk entry alpha.tga is ignored)
  CHECK(exists("Root/Data/User/alpha.tga") == make_int(1));   // the loose file, found by GetFileAttributesA
  CHECK(exists("Root/Data/User/beta.tga") == make_int(0));    // in Art.blk by basename, but the hook goes to disk
  CHECK(exists("User/data/player0.sav") == make_int(1));
  CHECK(exists("User/data/player1.sav") == make_int(0));
  CHECK(exists("elsewhere/User/data/player0.sav") == make_int(0));  // neither prefix: ResolveGamePath aborts (INFERRED 0)
  CHECK(exists("") == make_int(0));
  // does_file_exist works on the C string the native hands it (osi_cstring cut at the first NUL, 0x615ef6)
  Asm a;
  a.fn("setup", 0).nothing().ret();
  Svc s(a, &mem);
  CHECK(does_file_exist(s.host->context(), "Root/Data/loose.txt"));
  CHECK_FALSE(does_file_exist(s.host->context(), "Root/Data/AreaViewer.txt"));
}

TEST_CASE("osi services doesfileexist: with a current block file set the loose files are not searched (0x54ddf7 / 0x54de11)") {
  fs::MemoryFileSystem mem;
  REQUIRE(mem.mount("Root/Data/Art/Art.blk", make_blk({"art.tga"})));
  REQUIRE(mem.mount("Root/Data/cur.blk", make_blk({"incur.txt"})));
  REQUIRE(mem.mount("Root/Data/sec.blk", make_blk({"insec.txt"})));
  REQUIRE(mem.mount("Root/Data/loose.txt", {'x'}));
  Asm a;
  a.fn("setup", 0).nothing().ret();
  Svc s(a, &mem);
  BlockFiles& bf = s.host->context().subsystem<BlockFiles>();
  REQUIRE(bf.chain());
  CHECK(bf.exists("Root/Data/loose.txt"));  // slots 0: FileExists
  std::string err;
  REQUIRE(bf.chain()->open_current("Root/Data/cur.blk", &err));
  REQUIRE(bf.chain()->open_second("Root/Data/sec.blk", &err));
  CHECK(bf.exists("x/incur.txt"));
  CHECK(bf.exists("x/insec.txt"));          // [0x73612c] after the current one missed
  CHECK(bf.exists("Root/Data/Art/art.tga"));  // Art.blk first
  CHECK_FALSE(bf.exists("Root/Data/loose.txt"));  // [0x736128] != 0: no FileExists
}

TEST_CASE("osi services doesfileexist: no VFS gives 0, never a crash") {
  CHECK(call_str("scslosifileio", "doesfileexist", "Root/Data/AreaViewer.txt", nullptr) == make_int(0));
  CHECK(call_str("scslosifileio", "doesfileexist", "User/x", nullptr) == make_int(0));
}

// native only: the disk file system (fs::open_disk) is not linked into the wasm32 unit tests
#ifndef __EMSCRIPTEN__
TEST_CASE("osi services doesfileexist: every Art.blk entry name from the independent reader (tools/research/blk.py) is found") {
  const char* ld = std::getenv("LOMN_DATA");
  const char* names = std::getenv("LOMN_ART_NAMES");  // written by tools/osi/art_names.py (ctest osi_services_art_names)
  if (!ld || !*ld || !names || !*names) {
    MESSAGE("SKIPPED (no LOMN_DATA / LOMN_ART_NAMES)");
    return;
  }
  const std::string list = slurp(names);
  REQUIRE(!list.empty());
  std::unique_ptr<fs::FileSystem> disk = fs::open_disk(std::string(ld) + "/install", "");
  REQUIRE(disk);
  Asm a;
  a.fn("setup", 0).nothing().ret();
  Svc s(a, disk.get());
  HostContext& ctx = s.host->context();
  size_t n = 0, miss = 0;
  size_t pos = 0;
  while (pos < list.size()) {
    size_t nl = list.find('\n', pos);
    if (nl == std::string::npos) nl = list.size();
    const std::string name = list.substr(pos, nl - pos);
    pos = nl + 1;
    if (name.empty()) continue;
    ++n;
    if (!does_file_exist(ctx, "Root/Data/Art/" + name)) ++miss;
    std::string up = name;
    for (char& c : up)
      if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
    if (!does_file_exist(ctx, "Root/Data/Art/" + up)) ++miss;                  // 'A'..'Z' folding of Find
    if (does_file_exist(ctx, "Root/Data/Art/" + name + "~")) ++miss;           // one byte more: not an entry
  }
  MESSAGE("Art.blk entries checked: " << n << ", misses: " << miss);
  CHECK(n > 2000);
  CHECK(miss == 0);
  CHECK_FALSE(does_file_exist(ctx, "Root/Data/AreaViewer.txt"));  // the Setup call: int 0 on the owner's install (§8.2)
}
#endif

// ============================== gctimer.create ==============================

TEST_CASE("osi services gctimer.create asm pin: GcTimer::GcTimer 0x445840 and Start 0x445990") {
  const PeImage* im = exe();
  SKIP_WITHOUT(im);
  PIN(im, 0x4454f9, "8d 75 10");              // lea esi, [ebp+0x10] : w0 (the id)
  PIN(im, 0x4454fc, "8d 7d 14");              // lea edi, [ebp+0x14] : w1
  PIN(im, 0x4454ff, "8d 5d 18");              // lea ebx, [ebp+0x18] : w2
  PIN(im, 0x44551c, "d9 03");                 // fld dword [ebx] : the float ...
  PIN(im, 0x445520, "dd 1c 24");              // ... fstp qword [esp] : passed as a double
  PIN(im, 0x445523, "8a 07");                 // mov al, byte [edi] : the LOW BYTE of w1 only
  PIN(im, 0x44552a, "e8 11 03 00 00");        // call GcTimer::GcTimer 0x445840
  PIN(im, 0x445536, "e8 15 00 00 00");        // CreateVariant(GcTimer*) 0x445550
  PIN(im, 0x4458fc, "dd 5a 68");              // fstp qword [edx+0x68] : the duration
  PIN(im, 0x445902, "c6 42 78 00");           // mov byte [edx+0x78], 0 : not started
  PIN(im, 0x445906, "80 7d 0c 00");           // cmp byte [ebp+0xc], 0 : the Start guard ...
  PIN(im, 0x44590a, "74 08");                 // je : skips ...
  PIN(im, 0x44590f, "e8 7c 00 00 00");        // ... call Start 0x445990
  PIN(im, 0x4459c6, "80 7e 78 00");           // Start: cmp byte [esi+0x78], 0
  PIN(im, 0x4459ca, "75 29");
  PIN(im, 0x4459e9, "e8 52 ca 07 00");        // call AddTrigger 0x4c2440
  PIN(im, 0x4459f1, "c6 46 78 01");           // mov byte [esi+0x78], 1
  PIN(im, 0x4c25e5, "ff 41 04");              // AddTrigger: inc [this+4] : the trigger list's reference on the timer
  PIN(im, 0x445580, "ff 47 04");              // CreateVariant(GcTimer*): inc [edi+4] (the variant's reference)
  PIN(im, 0x445563, "66 c7 06 0f 00");        // null -> {0x0f, 0}
  CHECK(im->u64(0x7047b0) == 0);              // Start compares the duration with 0.0 ...
  CHECK(im->u64(0x7047b8) == 0);              // ... and the saved duration with 0.0
  CHECK(im->u64(0x704798) == 0);              // the constructor's [+0x60]
}

TEST_CASE("osi services gctimer.create: create(0, 0, 5.0) is a timer that is not started; nothing fires in 900 frames") {
  Asm a;
  a.fn("setup", 0).i8(0).i8(0).f(5.0f).call("gctimer", "create", 3).ret();
  a.fn("process", 3).i8(1).ret();
  Svc s(a, nullptr, HostFlags{}, {}, install_timer_probe);
  const Variant v = s.value();
  CHECK(v == Variant{39, 1});  // type 0x27, handle 1
  CHECK(s.log_has("gctimer.create 3 -> 0027:00000001 G 0"));
  REQUIRE(g_probe.seen);
  CHECK(g_probe.id == 0);
  CHECK(g_probe.duration == 5.0);
  CHECK_FALSE(g_probe.started);       // Start was skipped (0x44590a)
  CHECK(g_probe.engine_refs == 0);    // G 0 (natives.md §2.4)
  for (int i = 0; i < 900; ++i) REQUIRE(s.host->frame());
  for (const auto& l : s.log.lines) CHECK(l.rfind("C ", 0) != 0);  // no engine call into the script: OnEvent never fires
  CHECK_FALSE(s.trace_has("E "));
  CHECK(s.host->context().check_engine_refs().empty());
}

TEST_CASE("osi services gctimer.create: a started timer is held by the trigger list (G 1); the byte test is on w1's low byte") {
  {
    Asm a;
    a.fn("setup", 0).i8(7).i8(1).f(2.5f).call("gctimer", "create", 3).ret();
    Svc s(a, nullptr, HostFlags{}, {}, install_timer_probe);
    s.value();
    REQUIRE(g_probe.seen);
    CHECK(g_probe.id == 7);
    CHECK(g_probe.duration == 2.5);
    CHECK(g_probe.started);
    CHECK(g_probe.engine_refs == 1);  // AddTrigger's inc [this+4] (0x4c25e5), INFERRED to be the list's copy
    CHECK(s.log_has("gctimer.create 3 -> 0027:00000001 G 1"));
  }
  {
    Asm a;  // w1 = 256: the low byte is 0, the constructor tests one byte (0x445523 / 0x445906)
    a.fn("setup", 0).i8(7).i32(256).f(1.0f).call("gctimer", "create", 3).ret();
    Svc s(a, nullptr, HostFlags{}, {}, install_timer_probe);
    s.value();
    REQUIRE(g_probe.seen);
    CHECK_FALSE(g_probe.started);
  }
  {
    Asm a;  // the float argument is widened to a double exactly (fld dword / fstp qword)
    a.fn("setup", 0).i32(-5).i8(0).f(0.1f).call("gctimer", "create", 3).ret();
    Svc s(a, nullptr, HostFlags{}, {}, install_timer_probe);
    s.value();
    REQUIRE(g_probe.seen);
    CHECK(g_probe.id == -5);
    CHECK(g_probe.duration == static_cast<double>(0.1f));
  }
}

TEST_CASE("osi services gctimer.create: identity and lifetime (the unstarted timer dies with its last variant; F g, then delete)") {
  Asm a;
  a.fn("setup", 0).i8(0).i8(0).f(5.0f).call("gctimer", "create", 3).pop().i8(0).i8(1).f(5.0f).call("gctimer", "create", 3).pop();
  a.nothing().ret();
  Svc s(a);
  REQUIRE(s.res.ok);
  CHECK(s.trace_has("A g 00000001 27"));
  CHECK(s.trace_has("F g 00000001"));  // total 0: freed
  CHECK(s.trace_has("A g 00000002 27"));
  CHECK_FALSE(s.trace_has("F g 00000002"));  // the started one keeps the list's reference
  CHECK(s.host->context().object(1) == nullptr);
  CHECK(s.host->context().object(2) != nullptr);
  CHECK(s.host->context().check_engine_refs().empty());
}

// ============================== userealdeltatime ==============================

TEST_CASE("osi services userealdeltatime asm pin: 0x4a90e0 stores the byte; the Process constants of the real-time branch") {
  const PeImage* im = exe();
  SKIP_WITHOUT(im);
  PIN(im, 0x4a8ac7, "8d 55 10");              // lea edx, [ebp+0x10] : w0
  PIN(im, 0x4a8aca, "8a 02");                 // mov al, byte [edx] : the low byte
  PIN(im, 0x4a8acd, "e8 0e 06 00 00");        // call UseRealDeltaTime 0x4a90e0
  PIN(im, 0x4a8ad3, "66 c7 03 0f 00");        // returns nothing
  PIN(im, 0x4a90f2, "88 15 78 2d 71 00");     // mov [0x712d78], dl
  // the real-time branch of Process 0x4a9170 and the data it reads
  PIN(im, 0x4a9200, "e8 5b 12 05 00");        // call GetTime(1) 0x4fa460 (push 1 at 0x4a91fe)
  PIN(im, 0x4a91fe, "6a 01");
  PIN(im, 0x4a920c, "dd 05 70 2d 71 00");    // fld [0x712d70]   smoothed dt
  PIN(im, 0x4a9212, "dd 1d 80 2d 71 00");    // fstp [0x712d80]  previous
  PIN(im, 0x4a9218, "83 3d 88 2d 71 00 0a"); // cmp [0x712d88], 10
  PIN(im, 0x4a921f, "7d 0c");                 // jge : count >= 10 keeps the previous smoothed dt
  PIN(im, 0x4a9221, "dd 05 b0 2d 71 00");    // fld [0x712db0]
  PIN(im, 0x4a9239, "dc 60 10");              // fsub [pm+0x10] : cur - last real time
  PIN(im, 0x4a923c, "dc 1d b8 2d 71 00");    // fcomp [0x712db8] (0.5)
  PIN(im, 0x4a9244, "f6 c4 45");              // test ah, 0x45 : zero only when delta > 0.5
  PIN(im, 0x4a9247, "75 0e");
  PIN(im, 0x4a9249, "dd 05 c0 2d 71 00");    // fld [0x712dc0] : a gap above 0.5 -> this
  PIN(im, 0x4a924f, "dd 15 80 2d 71 00");    // fst [0x712d80]
  PIN(im, 0x4a9266, "dc 0d c8 2d 71 00");    // fmul [0x712dc8]
  PIN(im, 0x4a926c, "dd 05 d0 2d 71 00");    // fld [0x712dd0]
  PIN(im, 0x4a9272, "dc 0d 80 2d 71 00");    // fmul [0x712d80]
  PIN(im, 0x4a9278, "de c1");                 // faddp
  PIN(im, 0x4a927a, "dd 1d 70 2d 71 00");    // fstp [0x712d70]
  PIN(im, 0x4a9280, "ff 05 88 2d 71 00");    // inc [0x712d88]
  PIN(im, 0x4a928c, "dc 1d d8 2d 71 00");    // fcomp [0x712dd8]
  PIN(im, 0x4a9299, "dd 05 e0 2d 71 00");    // fld [0x712de0] : the clamp
  PIN(im, 0x4a92a5, "80 3d 78 2d 71 00 00"); // cmp byte [0x712d78], 0
  PIN(im, 0x4a92c8, "dd 05 68 2d 71 00");    // fld [0x712d68] : the fake dt (D3)
  CHECK(im->u64(0x712d68) == 0x3f7b4e81c0000000ull);
  CHECK(im->u64(0x712db0) == 0x3f9110e460000000ull);
  CHECK(im->u64(0x712db8) == 0x3fe0000000000000ull);  // 0.5
  CHECK(im->u64(0x712dc0) == 0x3f9110e460000000ull);
  CHECK(im->u64(0x712dc8) == 0x3fb1eb8520000000ull);
  CHECK(im->u64(0x712dd0) == 0x3fedc28f60000000ull);
  CHECK(im->u64(0x712dd8) == 0x3fb0e56040000000ull);
  CHECK(im->u64(0x712de0) == 0x3fb0e56040000000ull);
  CHECK(im->u64(0x712d70) == 0);  // .data: the smoothed dt, the flag byte 0x712d78 and the count start at 0
  CHECK(im->u32(0x712d88) == 0);
  CHECK(im->hex(0x712d78, 1) == "00");
  PIN(im, 0x4fa46c, "80 3d 78 2d 71 00 00");  // GetTime(0): the flag byte switches the clock to the real one
  PIN(im, 0x4fa475, "ff 15 f0 14 77 00");    // call [timeGetTime]
  PIN(im, 0x4fa48d, "dc 0d 38 80 72 00");     // fmul [0x728038]
  CHECK(im->u64(0x728038) == 0x3f50624dd2f1a9fcull);  // 0.001
}

TEST_CASE("osi services userealdeltatime: a logged no-op under --deterministic (the fake time path stays)") {
  Asm a;
  a.fn("setup", 0).i8(1).call("scprocessmanager", "userealdeltatime", 1).ret();
  a.fn("process", 3).i8(1).ret();
  Svc s(a);
  CHECK(s.value() == nothing());
  RealDeltaTime& rt = s.host->context().subsystem<RealDeltaTime>();
  CHECK(rt.requested);
  CHECK_FALSE(rt.effective);  // [0x712d78] stays 0
  CHECK(s.log_has("scprocessmanager.userealdeltatime 1 -> 000f:00000000"));
  REQUIRE(s.host->frame());
  // K <frame> <t> <dt> <counter>: dt = the double at 0x712d68 as a float
  const uint32_t dt_bits = f32_bits(static_cast<float>(0.006666666828095913));
  char want[64];
  std::snprintf(want, sizeof want, "K      0 %08x %08x 00000000", f32_bits(static_cast<float>(0.006666666828095913)), dt_bits);
  CHECK(s.trace_has(want));
}

TEST_CASE("osi services userealdeltatime: non-deterministic, the byte switches the smoothing on and off (0x4a9200-0x4a92f2)") {
  Asm a;
  a.fn("setup", 0).i8(1).call("scprocessmanager", "userealdeltatime", 1).ret();
  a.fn("process", 3).i8(1).ret();
  HostFlags nd;
  nd.deterministic = false;
  std::vector<double> clock_values = {10.0, 10.02, 10.5, 11.5, 11.52};
  size_t ci = 0;
  Svc s(a, nullptr, nd, [&] { return clock_values[ci++]; });
  REQUIRE(s.res.ok);
  RealDeltaTime& rt = s.host->context().subsystem<RealDeltaTime>();
  CHECK(rt.effective);
  // the constants, read the way the pin test reads them (the same data); expectations follow the asm order
  const double k_init = 0.016666000708937645, k_blend = 0.07000000029802322, k_keep = 0.9300000071525574,
               k_cap = 0.06599999964237213;
  double smoothed = 0, last = 0;
  int count = 0;
  auto expect = [&](double now) {
    double prev = smoothed;
    if (count < 10) prev = k_init;
    const double delta = now - last;
    double nv;
    if (delta > 0.5) nv = k_init;
    else nv = delta * k_blend + k_keep * prev;
    ++count;
    if (nv > k_cap) nv = k_cap;
    smoothed = nv;
    last = now;
    return nv;
  };
  for (size_t f = 0; f < 4; ++f) {
    REQUIRE(s.host->frame());
    const double dt = expect(clock_values[f]);
    char want[64];
    std::snprintf(want, sizeof want, "K %6zu %08x %08x %08x", f, f32_bits(static_cast<float>(clock_values[f])),
                  f32_bits(static_cast<float>(dt)), static_cast<unsigned>(f));
    CHECK_MESSAGE(s.trace_has(want), want);
  }
  // 10.5 - 10.02 = 0.48 <= 0.5 blends; 11.5 - 10.5 = 1.0 > 0.5 resets to 0.016666
  // and a request of 0 turns the flag off: the fake path returns
  Asm b;
  b.fn("setup", 0).i8(1).call("scprocessmanager", "userealdeltatime", 1).pop().i8(0).call("scprocessmanager", "userealdeltatime", 1).ret();
  b.fn("process", 3).i8(1).ret();
  size_t cj = 0;
  Svc s2(b, nullptr, nd, [&] { return 1.0 * ++cj; });
  REQUIRE(s2.res.ok);
  CHECK_FALSE(s2.host->context().subsystem<RealDeltaTime>().effective);
  REQUIRE(s2.host->frame());
  CHECK(cj == 0);  // the clock is not read on the fake path
  // only the low byte counts (0x4a8aca)
  Asm c;
  c.fn("setup", 0).i32(256).call("scprocessmanager", "userealdeltatime", 1).ret();
  Svc s3(c, nullptr, nd, [] { return 1.0; });
  CHECK_FALSE(s3.host->context().subsystem<RealDeltaTime>().effective);
}

TEST_CASE("osi services RealDeltaTime::step: the clamps (a big gap -> 0.016666; the smoothed value is capped at 0.066)") {
  HostContext* none = nullptr;
  (void)none;
  Asm a;
  a.fn("setup", 0).nothing().ret();
  Svc s(a);
  RealDeltaTime rt(s.host->context());
  CHECK(rt.step(100.0) == 0.016666000708937645);  // delta 100 > 0.5
  CHECK(rt.count == 1);
  const double v = rt.step(100.4);                // 0.4 <= 0.5: 0.4 * 0.07 + 0.93 * 0.016666 (count < 10)
  CHECK(v == doctest::Approx(0.4 * 0.07000000029802322 + 0.9300000071525574 * 0.016666000708937645));
  rt.smoothed = 0.5;
  rt.count = 20;                                   // count >= 10: the previous smoothed value is used
  const double w = rt.step(100.8);                 // 0.4 * 0.07 + 0.93 * 0.5 = 0.493 > 0.066 -> 0.066
  CHECK(w == 0.06599999964237213);
}

// ============================== getresolutionwidth / height ==============================

TEST_CASE("osi services getresolutionwidth/height asm pin: [[0x70fa64]+0x10] and [+0x14], the default INI 640x480") {
  const PeImage* im = exe();
  SKIP_WITHOUT(im);
  PIN(im, 0x4367b3, "e8 98 46 00 00");        // body 0x4367b0 -> GetResolutionWidth 0x43ae50
  PIN(im, 0x4367d3, "e8 98 46 00 00");        // body 0x4367d0 -> GetResolutionHeight 0x43ae70
  PIN(im, 0x4367c0, "e8 2b 10 fd ff");        // float through CreateVariant 0x4077f0
  PIN(im, 0x407800, "66 c7 03 12 00");        // {0x12, f32}
  // GetResolutionWidth: both pushes of the screen's two floats end with st0 = [screen+0x10]
  CHECK(im->hex(0x43ae50, 29) == "d9 ee d9 ee 8b 15 64 fa 70 00 83 c2 10 d9 02 dd da d9 42 04 dd d9 d9 c1 dd da dd d8 c3");
  // GetResolutionHeight: st0 = [screen+0x14]
  CHECK(im->hex(0x43ae70, 25) == "d9 ee 8b 15 64 fa 70 00 83 c2 10 d9 02 dd d9 d9 42 04 dd d9 d9 c0 dd d9 c3");
  // the screen size is the INI's ResolutionW/H (INI -> M2a-3); a missing INI is written with these literals (LoadIni)
  PIN(im, 0x5d4a5d, "68 10 b4 83 00 68 a2 75 74 00 68 5c 75 74 00 68 68 75 74 00");  // (file, "640", "ResolutionW", section)
  PIN(im, 0x5d4a77, "68 10 b4 83 00 68 a6 75 74 00 68 aa 75 74 00 68 68 75 74 00");  // (file, "480", "ResolutionH", section)
  CHECK(im->cstr(0x747568) == "GraphicsOptions");
  CHECK(im->cstr(0x74755c) == "ResolutionW");
  CHECK(im->cstr(0x7475a2) == "640");
  CHECK(im->cstr(0x7475aa) == "ResolutionH");
  CHECK(im->cstr(0x7475a6) == "480");
  // the screen constructor leaves the fields 0 (0x49d21a / 0x49d221): they are set after construction
  PIN(im, 0x49d21a, "c7 43 10 00 00 00 00");
  PIN(im, 0x49d221, "c7 43 14 00 00 00 00");
}

TEST_CASE("osi services getresolutionwidth/height: floats, the default 640.0 / 480.0, then whatever the display holds") {
  Asm a;
  a.fn("setup", 0).call("gcgame", "getresolutionwidth", 0).pop().call("gcgame", "getresolutionheight", 0).ret();
  Svc s(a);
  CHECK(s.value() == make_float(480.0f));
  CHECK(s.log_has("gcgame.getresolutionwidth 0 -> 0012:44200000"));
  CHECK(s.log_has("gcgame.getresolutionheight 0 -> 0012:43f00000"));
  Asm b;
  b.fn("setup", 0).call("gcgame", "getresolutionwidth", 0).ret();
  Svc s2(b);
  s2.host->context().subsystem<DisplayState>().width = 800.0f;
  CallResult r = s2.host->vm().call("Setup", {});
  REQUIRE(r.ok);
  CHECK(r.result == make_float(800.0f));
}

// ============================== the copy-kind natives, debug.print ==============================

TEST_CASE("osi services copy-kind asm pin: none returns argument 0 (loadosiobject x2, getstringtable) - tier3, unbound") {
  const PeImage* im = exe();
  SKIP_WITHOUT(im);
  PIN(im, 0x6159b1, "e8 5a 05 00 00");        // body 0x615990 -> LoadOSIObject 0x615f10
  PIN(im, 0x6159c1, "e8 1a 00 00 00");        // result through the whole-variant copy 0x6159e0
  PIN(im, 0x6159ea, "66 8b 13 66 89 16 8b 53 04 89 56 04");  // {type, value} copied bitwise
  PIN(im, 0x615f24, "e8 e7 80 f3 ff");        // Load_File(name, 1) 0x54e010
  PIN(im, 0x615f36, "e8 65 01 00 00");        // GetVariantFromBuffer 0x6160a0 : the variant comes from the file
  PIN(im, 0x61bc1e, "e8 3d 1f f3 ff");        // GetStringTable: SrBlockFileLoad 0x54db60
  PIN(im, 0x61bc29, "c7 02 01 00 00 00");     // writes 1 into the buffer's first dword
  PIN(im, 0x61bc32, "ba 13 00 00 00");        // and returns {0x13, buffer}
  PIN(im, 0x61bc37, "66 89 16");
}

TEST_CASE("osi services debug.print asm pin: returns nothing; the string argument is only appended to (tier2)") {
  const PeImage* im = exe();
  SKIP_WITHOUT(im);
  PIN(im, 0x61d5ef, "e8 0c 79 fb ff");        // call 0x5d4f00 (the cheat-menu test)
  PIN(im, 0x61d614, "e8 67 86 ed ff");        // append "\n" (0x756948) to the argument string in place
  CHECK(im->cstr(0x756948) == "\n");
}

// ============================== the tables: overrides <-> bound bodies ==============================

#ifdef LOMN_OSI_DATA_DIR
TEST_CASE("osi services real tables: every tier1 override is bound by a group, no tier2/tier3 override is; copy-kind stay stubs") {
  const std::string dir = LOMN_OSI_DATA_DIR;
  const std::string nj = slurp(dir + "/natives.json"), mj = slurp(dir + "/natives_beta150.json"),
                    tj = slurp(dir + "/types.json"), ov = slurp(dir + "/native_overrides.json");
  REQUIRE(!nj.empty());
  Asm a;
  a.fn("setup", 0).s("x").call("scslosifileio", "loadosiobject", 1).pop().s("x").call("scslosistringfuncs", "getstringtable", 1).pop();
  a.s("x").i8(1).i8(2).call("scslosifileio", "loadosiobjectfrommemcard", 3).ret();
  sage::OsiFile file;
  std::string err;
  REQUIRE_MESSAGE(file.parse(a.build(), &err), err);
  StringTrace tr;
  ScriptHost::Options real;
  real.real_natives = true;
  ScriptHost host(file, nj, mj, tj, ov, nullptr, &tr, nullptr, real);  // throws on a tier1 row without a body
  Json j;
  REQUIRE(parse_json(ov, j, &err));
  size_t t1 = 0, t2 = 0, t3 = 0;
  for (const Json& row : j.arr) {
    const std::string key = row.get("key")->str, tier = row.get("tier")->str;
    const size_t dot = key.find('.');
    const NativeEntry* e = host.registry().find(key.substr(0, dot), key.substr(dot + 1));
    REQUIRE_MESSAGE(e, key);
    if (tier == "tier1") {
      ++t1;
      CHECK_MESSAGE(e->fn != nullptr, key);
      CHECK_MESSAGE(e->tier == Tier::Implemented, key);
    } else {
      (tier == "tier2" ? t2 : t3) += 1;
      CHECK_MESSAGE(e->fn == nullptr, key);
    }
  }
  MESSAGE("overrides: tier1 " << t1 << ", tier2 " << t2 << ", tier3 " << t3);
  // the three copy-kind natives return nothing (the cited default), not argument 0 (D11's copy stub is wrong for them)
  CallResult r = host.setup();
  REQUIRE(r.ok);
  CHECK(r.result == nothing());
  for (const char* k : {"loadosiobject", "loadosiobjectfrommemcard"}) {
    const NativeEntry* e = host.registry().find("scslosifileio", k);
    REQUIRE(e);
    CHECK(e->tier == Tier::Tier3);
  }
  CHECK(host.registry().find("scslosistringfuncs", "getstringtable")->tier == Tier::Tier3);
}
#endif
