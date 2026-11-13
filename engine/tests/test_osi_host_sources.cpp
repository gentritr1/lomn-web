// M2a-3 T4a: the host sources (engine/src/osi/natives/host_sources.cpp, the frame order in engine/src/osi/host.cpp):
// the camera process (hud.md §1), the mouse driver (§5.1, §11), the process-map order (§2.4), `C -1` (trace.md §6
// Version 3) and the `Z` snapshot (§9). Written before the code (RED, then GREEN). The asm pins read the canonical exe as
// DATA through $LOMN_DATA (objdump -d --x86-asm-syntax=intel of LEGOBionicle.exe, 2026-11-14); the FRNT_CAM pins and the
// real Base.osi runs read $LOMN_DATA/install. Without LOMN_DATA those tests report SKIPPED and pass.
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "doctest.h"
#include "lomn/fs.h"
#include "lomn/osi/directors.h"
#include "lomn/osi/heap.h"
#include "lomn/osi/host.h"
#include "lomn/osi/natives.h"
#include "lomn/osi/objects.h"
#include "lomn/osi/trace.h"
#include "lomn/sage/blockchain.h"
#include "osi_test_asm.h"

using namespace lomn;
using namespace lomn::osi;
using lomn::test::Asm;

namespace {

// ---------------------------------------------------------------------------------------------- the exe as data
struct Pe {
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
  uint32_t u32(uint32_t va) const {
    std::vector<uint8_t> b;
    read(va, 4, b);
    return b[0] | b[1] << 8 | b[2] << 16 | static_cast<uint32_t>(b[3]) << 24;
  }
};

std::string lomn_data() {
  const char* ld = std::getenv("LOMN_DATA");
  return ld && *ld ? std::string(ld) : std::string();
}

const Pe* exe() {
  static std::unique_ptr<Pe> im;
  static bool tried = false;
  if (!tried) {
    tried = true;
    if (!lomn_data().empty()) {
      im = std::make_unique<Pe>();
      if (!im->load(lomn_data() + "/reference/LEGOBionicle.exe")) im.reset();
    }
  }
  return im.get();
}

#define SKIP_WITHOUT(c)                \
  if (!(c)) {                          \
    MESSAGE("SKIPPED (no LOMN_DATA)"); \
    return;                            \
  }
#define PIN(img, va, bytes) CHECK_MESSAGE((img)->hex((va), (sizeof(bytes) + 1) / 3) == bytes, "pin at " #va)

std::string bits(float f) {
  char b[16];
  std::snprintf(b, sizeof b, "%08x", f32_bits(f));
  return b;
}
std::string dbits(double d) {
  uint64_t u;
  std::memcpy(&u, &d, 8);
  char b[24];
  std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(u));
  return b;
}

// ---------------------------------------------------------------------------------------------- a synthetic SLB
// count, -> records; records {u32 n, -> knots, u32 0}; knots (f32 t, f32 v); the trailer GetSLKFile reads (slots, count,
// 0xc0ffee), every pointer a data-relative offset (lomn/sage/slb.h).
std::vector<uint8_t> make_slb(const std::vector<std::vector<std::pair<float, float>>>& recs) {
  std::vector<uint8_t> b;
  auto put = [&](uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
  };
  auto set = [&](size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = static_cast<uint8_t>(v >> (8 * i));
  };
  std::vector<uint32_t> fields = {4};
  put(static_cast<uint32_t>(recs.size()));
  put(8);
  const size_t rec0 = b.size();
  for (size_t i = 0; i < recs.size(); ++i) {
    put(static_cast<uint32_t>(recs[i].size()));
    put(0);
    put(0);
    fields.push_back(static_cast<uint32_t>(rec0 + 12 * i + 4));
  }
  for (size_t i = 0; i < recs.size(); ++i) {
    set(rec0 + 12 * i + 4, static_cast<uint32_t>(b.size()));
    for (auto [t, v] : recs[i]) put(f32_bits(t)), put(f32_bits(v));
  }
  for (size_t k = fields.size(); k-- > 0;) put(fields[k]);  // slot k (1-based) sits at m - 4 - 4k
  put(static_cast<uint32_t>(fields.size()));
  put(0x00c0ffee);
  return b;
}
const std::vector<std::pair<float, float>> kKnots = {{0.0f, 0.0f}, {0.25f, 1.0f}, {0.5f, 0.0f}, {1.0f, 2.0f}};

// ---------------------------------------------------------------------------------------------- the registry rows
const char* const kRows = R"([
 {"cls": "GcGame", "name": "GetMainView", "min": 0, "max": 0, "types": [], "ret": "game", "ret_class": "GcViewPort", "winner": true},
 {"cls": "GcViewPort", "name": "GetCamera", "min": 1, "max": 1, "types": ["type:GcViewPort"], "ret": "game", "ret_class": "GcLegoCamera", "winner": true},
 {"cls": "ScSLOSISLKCubicSpline", "name": "Create", "min": 1, "max": 1, "types": ["string"], "ret": "game", "ret_class": "ScSLOSISLKCubicSpline", "winner": true},
 {"cls": "GcLegoCamera", "name": "SetCameraAIMode", "min": 2, "max": 2, "types": ["type:GcLegoCamera", "int"], "ret": "none", "winner": true},
 {"cls": "GcLegoCamera", "name": "SetCameraSpline", "min": 6, "max": 6, "types": ["type:GcLegoCamera", "type:ScSLOSISLKCubicSpline", "int", "float", "int", "int"], "ret": "none", "winner": true},
 {"cls": "GcLegoCamera", "name": "SetTargetSpline", "min": 7, "max": 7, "types": ["type:GcLegoCamera", "type:ScSLOSISLKCubicSpline", "int", "int", "float", "int", "int"], "ret": "none", "winner": true},
 {"cls": "GcLegoCamera", "name": "SetCamPos", "min": 4, "max": 4, "types": ["type:GcLegoCamera", "float", "float", "float"], "ret": "none", "winner": true},
 {"cls": "GcLegoCamera", "name": "SetTargPos", "min": 4, "max": 4, "types": ["type:GcLegoCamera", "float", "float", "float"], "ret": "none", "winner": true},
 {"cls": "GcLegoCamera", "name": "AddYaw", "min": 2, "max": 2, "types": ["type:GcLegoCamera", "float"], "ret": "none", "winner": true},
 {"cls": "ScSLOSIMath", "name": "Abs", "min": 1, "max": 1, "types": ["float"], "ret": "float", "winner": true}
])";
const char* const kTypes = R"({"types": [
 {"name": "type:ScSLOSISLKCubicSpline", "id": 35}, {"name": "type:GcViewPort", "id": 40}, {"name": "type:GcLegoCamera", "id": 42}]})";

Asm& gset(Asm& a, const char* g) { return a.op(0x22).u16(a.glob(g)); }
Asm& gget(Asm& a, const char* g) { return a.op(0x21).u16(a.glob(g)); }
Asm& local(Asm& a, uint16_t i) { return a.op(0x21).u16(i); }
// setup: cam = getmainview().getcamera(); s = scslosislkcubicspline.create("Root/Data/cam.slb")
void cam_and_spline(Asm& a) {
  a.call("gcgame", "getmainview", 0).call("gcviewport", "getcamera", 1);
  gset(a, "cam").s("Root/Data/cam.slb").call("scslosislkcubicspline", "create", 1);
  gset(a, "s");
}
// the front end's three calls (hud.md §1.2): setcameraspline(cam, s, ci, d, 1, 1), settargetspline(cam, s, ci, ti, d, 1, 1),
// setcameraaimode(cam, 3)
void fly(Asm& a, int ci_cam, int ci_targ, int ti, float d) {
  gget(a, "cam");
  gget(a, "s").i8(static_cast<int8_t>(ci_cam)).f(d).i8(1).i8(1).call("gclegocamera", "setcameraspline", 6).pop();
  gget(a, "cam");
  gget(a, "s").i8(static_cast<int8_t>(ci_targ)).i8(static_cast<int8_t>(ti)).f(d).i8(1).i8(1);
  a.call("gclegocamera", "settargetspline", 7).pop();
  gget(a, "cam").i8(3).call("gclegocamera", "setcameraaimode", 2).pop();
}

struct Host {
  sage::OsiFile file;
  fs::MemoryFileSystem vfs;
  StringTrace tr, log;
  std::unique_ptr<ScriptHost> host;
  CallResult res;
  Host(Asm& a, const std::string& keys, const std::string& mouse, bool trace = true) {
    std::string err;
    REQUIRE_MESSAGE(file.parse(a.build(), &err), err);
    REQUIRE(vfs.mount("Root/Data/cam.slb", make_slb({kKnots, kKnots, kKnots, kKnots, kKnots, kKnots})));
    ScriptHost::Options opt;
    opt.real_natives = true;
    opt.keys = keys;
    opt.mouse = mouse;
    host = std::make_unique<ScriptHost>(file, kRows, "", kTypes, "", &vfs, trace ? &tr : nullptr, &log, opt);
    res = host->setup();
  }
  // the natives.log lines of frame k (after its K line, before the next)
  std::vector<std::string> log_frame(long k) const {
    std::vector<std::string> v;
    char kl[16];
    std::snprintf(kl, sizeof kl, "K %6ld", k);
    bool in = false;
    for (const auto& l : log.lines) {
      if (l.rfind("K ", 0) == 0) in = l == kl;
      else if (in) v.push_back(l);
    }
    return v;
  }
  // the trace's Z block of frame k, header included
  std::vector<std::string> z_block(long k) const {
    std::vector<std::string> v;
    char zl[16];
    std::snprintf(zl, sizeof zl, "Z %6ld", k);
    bool in = false;
    for (const auto& l : tr.lines) {
      if (l == zl) in = true;
      else if (in && l.rfind("Z ", 0) != 0) break;
      if (in) v.push_back(l);
    }
    return v;
  }
};

std::string nothings(int n) {
  std::string s;
  for (int i = 0; i < n; ++i) s += " 000f:00000000";
  return s;
}

}  // namespace

// ============================== asm pins ==============================

TEST_CASE("host sources asm pin: Interpolate 0x443a70, Accelerate 0x4435f0 and their constants") {
  const Pe* im = exe();
  SKIP_WITHOUT(im);
  PIN(im, 0x443aa0, "d8 da");              // Interpolate: fcomp knot_t with t ...
  PIN(im, 0x443aa4, "f6 c4 45");           // ... knot_t <= t (or unordered) -> lo = mid, else hi = mid; no clamp
  PIN(im, 0x443b30, "d8 0d 04 42 70 00");  // (...) * f32 [0x704204] (1/6) ...
  PIN(im, 0x443b36, "de c4");              // ... added to B*yhi + A*ylo
  PIN(im, 0x443702, "de f1");              // Accelerate: sig = (x_i - x_i-1) / (x_i+1 - x_i-1) (fdivrp)
  PIN(im, 0x44371a, "d8 05 f0 41 70 00");  // p = sig * y2[i-1] + 2.0
  PIN(im, 0x443733, "d8 25 f4 41 70 00");  // y2[i] = (sig - 1.0) / p
  PIN(im, 0x443761, "de e1");              // u[i] = d2 - d1 (fsubrp)
  PIN(im, 0x443781, "d9 05 f8 41 70 00");  // 6.0 * u[i] ...
  PIN(im, 0x443790, "de f9");              // ... / (x_i+1 - x_i-1) ...
  PIN(im, 0x443796, "de e9");              // ... - sig * u[i-1], then / p
  PIN(im, 0x443807, "d9 06 d8 09 d8 07 d9 1a");  // back substitution: y2[k] = y2[k] * y2[k+1] + u[k], fstp dword
  PIN(im, 0x4435d2, "fe 45 f8");           // AfterConstruction: a BYTE record counter
  CHECK(im->u32(0x704204) == 0x3e2aaaabu);  // 1/6 as f32
  CHECK(im->u32(0x7041f0) == 0x40000000u);  // 2.0
  CHECK(im->u32(0x7041f4) == 0x3f800000u);  // 1.0
  CHECK(im->u32(0x7041f8) == 0x40c00000u);  // 6.0
  CHECK(im->u32(0x7041ec) == 0u);           // y2[0] = u[0] = 0
  CHECK(im->u32(0x7041fc) == 0u);           // y2[n-1] = 0
}

TEST_CASE("host sources asm pin: GetSLKFile 0x4032e0 goes through SrBlockFileExists / SrBlockFileLoad") {
  const Pe* im = exe();
  SKIP_WITHOUT(im);
  PIN(im, 0x40339e, "e8 1d aa 14 00");  // SrBlockFileExists 0x54ddc0(name)
  PIN(im, 0x4033d4, "e8 87 a7 14 00");  // SrBlockFileLoad 0x54db60(name, &buf, &size)
  PIN(im, 0x61a514, "e8 67 00 00 00");  // the spline constructor's GetSLKFile wrapper 0x61a580
  PIN(im, 0x61a5ca, "e8 e1 8f e2 ff");  // AfterConstruction 0x4435b0 (Accelerate per record)
}

TEST_CASE("host sources asm pin: GcLegoCamera::Process mode 3 forward, the OnEvent(6) guard 0x452a26-0x452d29") {
  const Pe* im = exe();
  SKIP_WITHOUT(im);
  PIN(im, 0x452a43, "dc a0 b0 03 00 00");  // elapsed = time - [cam+0x3b0] (at the FPU precision) ...
  PIN(im, 0x452a49, "dd 95 8c f1 ff ff");  // ... stored as a double
  PIN(im, 0x452a5d, "f6 c4 45");           // elapsed > duration -> elapsed = duration (the clamp)
  PIN(im, 0x452a80, "dc b0 a8 03 00 00");  // param = elapsed / duration ...
  PIN(im, 0x452a86, "d9 9d 4c f6 ff ff");  // ... stored f32
  PIN(im, 0x452afe, "8a 80 b8 03 00 00");  // the camera index's low byte (x); +1 -> y, +2 -> z
  PIN(im, 0x452c02, "f6 c4 05");           // fire when elapsed >= duration
  PIN(im, 0x452c0b, "dd 45 08");           // start = time ...
  PIN(im, 0x452c14, "dd 98 b0 03 00 00");  // ... [cam+0x3b0]
  PIN(im, 0x452c1a, "6a 06");              // OnEvent's first argument: 6
  PIN(im, 0x452d24, "e8 f7 85 1b 00");     // ScOSIVirtualMachine::Call 0x60b320
  PIN(im, 0x452d29, "e9 e2 7b 00 00");     // then straight to the tail 0x45a910
  // mode 2 forward look-ahead (+0.01), the reverse guards (rem <= 0.0) and their start resets
  PIN(im, 0x4524cc, "dd 05 28 67 70 00");
  PIN(im, 0x45280f, "dc 25 30 67 70 00");
  PIN(im, 0x4528db, "dc 1d 38 67 70 00");
  PIN(im, 0x452efc, "dc 1d 40 67 70 00");
  PIN(im, 0x4525be, "dd 98 b0 03 00 00");
  PIN(im, 0x452901, "dd 98 b0 03 00 00");
  PIN(im, 0x452f22, "dd 98 b0 03 00 00");
  CHECK(im->u32(0x706728) == 0x47ae147bu);  // 0.01 (double 0x3f847ae147ae147b)
  CHECK(im->u32(0x70672c) == 0x3f847ae1u);
  CHECK(im->u32(0x706738) == 0u);
  CHECK(im->u32(0x70673c) == 0u);
}

TEST_CASE("host sources asm pin: SetCameraSpline / SetTargetSpline / SetCameraAIMode stores, setcampos, settargpos, addyaw, abs") {
  const Pe* im = exe();
  SKIP_WITHOUT(im);
  PIN(im, 0x45b3c6, "89 97 b8 03 00 00");  // SetCameraSpline: +0x3b8 = w2
  PIN(im, 0x45b3cf, "dd 9f a8 03 00 00");  // +0x3a8 = (double) f32 w3
  PIN(im, 0x45b3dd, "dd 9f b0 03 00 00");  // +0x3b0 = SrGetSeconds(0)
  PIN(im, 0x45b3e6, "88 97 60 04 00 00");  // +0x460 = low byte of w4
  PIN(im, 0x45b3ef, "88 97 61 04 00 00");  // +0x461 = low byte of w5
  PIN(im, 0x45b446, "89 97 b8 03 00 00");  // SetTargetSpline ALSO writes +0x3b8 (= w2) ...
  PIN(im, 0x45b44f, "89 97 bc 03 00 00");  // ... and +0x3bc = w3
  PIN(im, 0x45b458, "dd 9f a8 03 00 00");
  PIN(im, 0x45b466, "dd 9f b0 03 00 00");
  PIN(im, 0x45b545, "dd 9f b0 03 00 00");  // SetCameraAIMode, modes 2 and 3: start = SrGetSeconds(0)
  PIN(im, 0x44e8e3, "e8 68 75 fb ff");     // setcampos -> SetCameraPosition 0x405e50
  PIN(im, 0x44e943, "e8 b8 75 fb ff");     // settargpos -> SetTargetPosition 0x405f00
  PIN(im, 0x44ecc1, "e8 ea d5 00 00");     // addyaw -> AddYaw 0x45c2b0 (rotates +0x174 only, hud.md §1.7)
  PIN(im, 0x6182a8, "e8 99 fa 03 00");     // abs: the CRT's fabs 0x657d46 ...
  PIN(im, 0x657d4a, "d9 e1");              // ... fabs
  PIN(im, 0x55c753, "e8 c8 eb 0a 00");     // GcMouseDriver::Process: OnEvent(26, x, y) through Call 0x60b320
}

// ============================== spline maths ==============================

TEST_CASE("host sources spline: the synthetic pins of hud.md §1.5 under pc24 and pc53 (and the (1,1) variant)") {
  SplineCurve c;
  for (auto [t, v] : kKnots) c.t.push_back(t), c.v.push_back(v);
  spline_accelerate(c, 24);
  CHECK(bits(spline_interpolate(c, 0.125f, 24)) == "3f3a6f4e");
  CHECK(bits(spline_interpolate(c, 0.75f, 24)) == "3eb21642");
  SplineCurve d = c;
  spline_accelerate(d, 53);
  CHECK(bits(spline_interpolate(d, 0.125f, 53)) == "3f3a6f4e");
  CHECK(bits(spline_interpolate(d, 0.75f, 53)) == "3eb21641");
  // linear interpolation would give 0.5 / 1.0, Catmull-Rom 0.5625 / 0.9375
  SplineCurve e = c;
  e.v.back() = 1.0f;
  spline_accelerate(e, 24);
  CHECK(bits(spline_interpolate(e, 0.125f, 24)) == "3f38590b");
  CHECK(bits(spline_interpolate(e, 0.75f, 24)) == "bcb21640");
  // the end knot: y = y_hi exactly (A = 0, B = 1)
  CHECK(spline_interpolate(c, 1.0f, 24) == 2.0f);
}

// native only: the disk file system (fs::open_disk) is not linked into the wasm32 unit tests
#ifndef __EMSCRIPTEN__
TEST_CASE("host sources spline: FRNT_CAM (levels/lev0/frnt.blk) pins of hud.md §1.5 under pc24 and pc53") {
  SKIP_WITHOUT(!lomn_data().empty());
  const std::string user = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/lomn_t4a_user";
  std::filesystem::create_directories(user);
  auto vfs = fs::open_disk(lomn_data() + "/install", user);
  REQUIRE(vfs);
  sage::BlockChain chain(*vfs);
  std::string err;
  REQUIRE_MESSAGE(chain.open_current("Root/Data/Levels/lev0/frnt.blk", &err), err);
  const std::string name = "Root/Data/Levels/lev0/frnt/frnt_CAM.slb";  // the script's argument (A9 trace, frame 3)
  std::vector<SplineCurve> c24, c53;
  REQUIRE(load_slk_splines(&chain, name, 24, c24));
  REQUIRE(load_slk_splines(&chain, name, 53, c53));
  REQUIRE(c24.size() == 60);
  CHECK(bits(spline_interpolate(c24[48], 0.10f, 24)) == "c185b15d");
  CHECK(bits(spline_interpolate(c24[48], 0.28f, 24)) == "c0d10e08");
  CHECK(bits(spline_interpolate(c24[48], 0.44f, 24)) == "413ba175");
  CHECK(bits(spline_interpolate(c24[48], 0.75f, 24)) == "425b120a");
  CHECK(bits(spline_interpolate(c24[1], 0.13f, 24)) == "417c70f8");
  CHECK(bits(spline_interpolate(c24[1], 0.86f, 24)) == "417adeb6");
  CHECK(bits(spline_interpolate(c53[48], 0.10f, 53)) == "c185b15d");
  CHECK(bits(spline_interpolate(c53[48], 0.28f, 53)) == "c0d10e08");
  CHECK(bits(spline_interpolate(c53[48], 0.44f, 53)) == "413ba175");
  CHECK(bits(spline_interpolate(c53[48], 0.75f, 53)) == "425b120b");
  CHECK(bits(spline_interpolate(c53[1], 0.13f, 53)) == "417c70f8");
  CHECK(bits(spline_interpolate(c53[1], 0.86f, 53)) == "417adeb5");
  std::filesystem::remove_all(user);
}
#endif  // __EMSCRIPTEN__

// ============================== MOUSESPEC ==============================

TEST_CASE("host sources MOUSESPEC: the expansion of hud.md §11 (keys.py --mouse) and its refusals") {
  MouseSpec m;
  std::string err;
  REQUIRE(parse_mouse("450,330@1 lmb+@2 450,370@3 lmb-@4", m, &err));
  CHECK(m.any);
  CHECK(mouse_listing(m, 6) ==
        "0 OnEvent 0010:0000001a 0010:00000000 0010:00000000\n"
        "1 OnEvent 0010:0000001a 0010:000001c2 0010:0000014a\n"
        "2 OnEvent 0010:0000001a 0010:000001c2 0010:0000014a\n"
        "2 OnEvent 0010:00000017 0010:00000001\n"
        "3 OnEvent 0010:0000001a 0010:000001c2 0010:00000172\n"
        "3 OnEvent 0010:00000018 0010:00000001\n"
        "4 OnEvent 0010:0000001a 0010:000001c2 0010:00000172\n"
        "4 OnEvent 0010:00000019 0010:00000001\n"
        "5 OnEvent 0010:0000001a 0010:000001c2 0010:00000172\n");
  // buttons in order 1..4 after 26; a press of a down button / a release of an up one is no transition
  REQUIRE(parse_mouse("xmb+@0 lmb+@0 lmb+@1 rmb-@1 xmb-@2", m, &err));
  CHECK(mouse_listing(m, 3) ==
        "0 OnEvent 0010:0000001a 0010:00000000 0010:00000000\n"
        "0 OnEvent 0010:00000017 0010:00000001\n"
        "0 OnEvent 0010:00000017 0010:00000004\n"
        "1 OnEvent 0010:0000001a 0010:00000000 0010:00000000\n"
        "1 OnEvent 0010:00000018 0010:00000001\n"
        "1 OnEvent 0010:00000018 0010:00000004\n"
        "2 OnEvent 0010:0000001a 0010:00000000 0010:00000000\n"
        "2 OnEvent 0010:00000018 0010:00000001\n"
        "2 OnEvent 0010:00000019 0010:00000004\n");
  REQUIRE(parse_mouse("", m, &err));
  CHECK_FALSE(m.any);
  CHECK(mouse_listing(m, 3).empty());
  REQUIRE(parse_mouse("  ", m, &err));
  CHECK_FALSE(m.any);
  for (const char* bad : {"lmb@3", "1,2@-1", "65536,0@1", "0,65536@1", "zmb+@1", "1,2@3 4,5@3", "lmb+@2 lmb-@2", "1,2",
                          "-1,2@3", "1,2,3@4", "1,2@+5", "1,2@", "@3", "lmb+@x", "1,@2", ",1@2"})
    CHECK_MESSAGE(!parse_mouse(bad, m, &err), bad);
}

// ============================== the digest's Z line ==============================

TEST_CASE("host sources digest: the Z block leaves the D line unchanged and gets its own Z line (trace.md §2, hud.md §9.3)") {
  const std::vector<std::string> zb = {"Z      0",
                                       "Z ref 002e:00000001 0",
                                       "Z text.font 002e:00000001 -",
                                       "Z text.just 002e:00000001 0002",
                                       "Z text.juston 002e:00000001 -",
                                       "Z text.pos 002e:00000001 43c80000 43a00000 00000000",
                                       "Z text.scale 002e:00000001 3fc00000 40000000",
                                       "Z text.string 002e:00000001 -"};
  StringTrace with, without;
  DigestTrace dw(&with), dn(&without);
  for (DigestTrace* d : {&dw, &dn}) {
    d->line("# header");
    d->line("K      0 3bda740e 3bda740e 00000000");
    d->line("I 00000010 21    0    0 ----:--------");
  }
  for (const auto& l : zb) dw.line(l);
  for (DigestTrace* d : {&dw, &dn}) d->line("K      1 3c5a740e 3bda740e 00000001");
  dw.line("Z      1");
  dw.finish();
  dn.finish();
  REQUIRE(with.lines.size() == 6);
  REQUIRE(without.lines.size() == 4);
  CHECK(with.lines[0] == "# header");
  CHECK(with.lines[1] == without.lines[1]);  // D 0: the Z lines are not part of it
  CHECK(with.lines[2] == "Z      0        8 a948cf67ffb8462c");  // the hud.md §9.3 worked example
  CHECK(with.lines[3] == without.lines[2]);  // D 1
  CHECK(with.lines[4].rfind("Z      1        1 ", 0) == 0);
  CHECK(with.lines[5].rfind("T 13 ", 0) == 0);  // T covers the Z lines too
}

// ============================== the frame order, C -1 and Z ==============================

namespace {
// g = g*10 + d per host call: VK Input 1, key-mapper Input 2, mouse OnEvent 3, Process 4, camera OnEvent(6) 5
void order_script(Asm& a, float duration) {
  a.fn("setup", 0).i8(0);
  gset(a, "g");
  cam_and_spline(a);
  fly(a, 5, 1, 3, duration);  // camera index 5, then the target setter writes 1 (and target 3)
  a.nothing().ret();
  a.fn("input", 5);
  gget(a, "g").i8(10).op(0x72);
  local(a, 1).i32(251).op(0x60).op(0x70).i8(1).op(0x70);
  gset(a, "g").nothing().ret();
  a.fn("onevent", 10);
  gget(a, "g").i8(10).op(0x72);
  local(a, 0).i8(6).op(0x60).i8(2).op(0x72).op(0x70).i8(3).op(0x70);
  gset(a, "g").nothing().ret();
  a.fn("process", 3);
  gget(a, "g").i8(10).op(0x72).i8(4).op(0x70);
  gset(a, "g").i8(1).ret();
}
}  // namespace

TEST_CASE("host sources frame order: VK Input, key-mapper Input, mouse, Process, camera OnEvent(6), then Z (hud.md §2.4)") {
  Asm a;
  order_script(a, 0.001f);  // 0.001 < t_0: the guard fires in frame 0
  Host h(a, "esc@0 enter@0", "1,2@0");  // KEYSPEC order is esc first: the VK key (priority 0) still runs first
  REQUIRE_MESSAGE(h.res.ok, (h.host->vm().last_error() ? h.host->vm().last_error()->message : std::string("ok")));
  REQUIRE(h.host->frame());
  const Variant* g = h.host->vm().find_global("g");
  REQUIRE(g);
  CHECK(g->value == 12345u);
  // natives.log: the mouse and camera calls as C -1, Input and Process never; no H line for the camera's argument
  const std::vector<std::string> f0 = h.log_frame(0);
  REQUIRE(f0.size() == 2);
  CHECK(f0[0] == "C -1 OnEvent 0010:0000001a 0010:00000001 0010:00000002" + nothings(7));
  CHECK(f0[1] == "C -1 OnEvent 0010:00000006 002a:00000002 G 1 0010:00000000" + nothings(7));
  // the Z block closes the frame, after the camera's call
  const long zi = [&] {
    for (size_t i = 0; i < h.tr.lines.size(); ++i)
      if (h.tr.lines[i] == "Z      0") return static_cast<long>(i);
    return -1L;
  }();
  REQUIRE(zi > 0);
  long ci = -1;
  for (size_t i = 0; i < h.tr.lines.size(); ++i)
    if (h.tr.lines[i].rfind("C OnEvent 10 0010:00000006", 0) == 0) ci = static_cast<long>(i);
  CHECK(ci > 0);
  CHECK(ci < zi);
  const double t0 = fadd_pc(0.0, 0.006666666828095913, 24);
  const std::vector<std::string> want = {"Z      0",
                                         "Z camera.duration " + dbits(static_cast<double>(0.001f)),
                                         "Z camera.eye 40000000 40000000 40000000",
                                         "Z camera.flags 01 01",
                                         "Z camera.index 01 03",
                                         "Z camera.mode 03",
                                         "Z camera.spline 0023:00000003",
                                         "Z camera.start " + dbits(t0),
                                         "Z camera.target 40000000 40000000 40000000",
                                         "Z ref 0023:00000003 1",
                                         "Z ref 0028:00000001 1",
                                         "Z ref 002a:00000002 1"};
  CHECK(h.z_block(0) == want);
  CHECK(h.tr.lines.back() == want.back());  // nothing after the block
}

TEST_CASE("host sources Z: setcampos / settargpos write the eye and target; unwritten fields print '-'; abs and addyaw") {
  Asm a;
  a.fn("setup", 0).call("gcgame", "getmainview", 0).call("gcviewport", "getcamera", 1);
  gset(a, "cam");
  gget(a, "cam").f(1).f(2).f(3).call("gclegocamera", "setcampos", 4).pop();
  gget(a, "cam").f(4).f(5).f(6).call("gclegocamera", "settargpos", 4).pop();
  gget(a, "cam").f(5).call("gclegocamera", "addyaw", 2).pop();
  a.f(-1.5f).call("scslosimath", "abs", 1);
  gset(a, "x").fbits(0x80000000u).call("scslosimath", "abs", 1);
  gset(a, "z").nothing().ret();
  a.fn("process", 3).i8(1).ret();
  Host h(a, "", "");
  REQUIRE_MESSAGE(h.res.ok, (h.host->vm().last_error() ? h.host->vm().last_error()->message : std::string("ok")));
  CHECK(h.host->vm().find_global("x")->value == 0x3fc00000u);
  CHECK(h.host->vm().find_global("z")->value == 0u);
  REQUIRE(h.host->frame());
  CHECK(h.z_block(0) == std::vector<std::string>{"Z      0", "Z camera.duration -",
                                                 "Z camera.eye 3f800000 40000000 40400000", "Z camera.flags -",
                                                 "Z camera.index -", "Z camera.mode 00", "Z camera.spline -",
                                                 "Z camera.start -", "Z camera.target 40800000 40a00000 40c00000",
                                                 "Z ref 0028:00000001 1", "Z ref 002a:00000002 1"});
  CHECK(h.log_frame(0).empty());  // no mouse source, no camera event: nothing logged in the frame
}

TEST_CASE("host sources Z: no camera lines without a viewport camera; the block is written every frame, header first") {
  Asm a;
  a.fn("setup", 0).nothing().ret();
  a.fn("process", 3).i8(1).ret();
  Host h(a, "", "");
  REQUIRE(h.host->frame());
  REQUIRE(h.host->frame());
  CHECK(h.z_block(0) == std::vector<std::string>{"Z      0"});
  CHECK(h.z_block(1) == std::vector<std::string>{"Z      1"});
}

TEST_CASE("host sources Z: index prints '-' per unwritten byte; a frame whose call errors writes no Z block (oracle ruling)") {
  Asm a;
  a.fn("setup", 0);
  cam_and_spline(a);
  gget(a, "cam");
  gget(a, "s").i8(5).f(4.0f).i8(1).i8(0).call("gclegocamera", "setcameraspline", 6).pop();
  a.nothing().ret();
  a.fn("process", 3);
  local(a, 2).i8(1).op(0x60).rel(0x53, "ok");
  a.nothing().i8(0).call("gclegocamera", "setcameraaimode", 2);  // a null camera: the frame-1 Process errors
  a.label("ok").i8(1).ret();
  Host h(a, "", "");
  REQUIRE(h.res.ok);
  REQUIRE(h.host->frame());
  const std::vector<std::string> z0 = h.z_block(0);
  CHECK(std::find(z0.begin(), z0.end(), "Z camera.index 05 -") != z0.end());
  CHECK(std::find(z0.begin(), z0.end(), "Z camera.flags 01 00") != z0.end());
  CHECK_FALSE(h.host->frame());
  CHECK(h.z_block(1).empty());
}

// ============================== re-fire ==============================

namespace {
// the front end's timing: the spline is set in frame 3 (counter 3) with duration 4.0; onevent either leaves the mode
// (the stand-in) or sets mode 0 (what the real handler does, hud.md §1.4)
std::vector<long> fire_frames(bool set_mode_0, long frames) {
  Asm a;
  a.fn("setup", 0);
  cam_and_spline(a);
  a.nothing().ret();
  a.fn("process", 3);
  local(a, 2).i8(3).op(0x60).rel(0x53, "skip");
  fly(a, 0, 0, 3, 4.0f);
  a.label("skip").i8(1).ret();
  a.fn("onevent", 10);
  if (set_mode_0) gget(a, "cam").i8(0).call("gclegocamera", "setcameraaimode", 2).pop();
  a.nothing().ret();
  Host h(a, "", "", false);
  REQUIRE(h.res.ok);
  for (long k = 0; k < frames; ++k) REQUIRE(h.host->frame());
  std::vector<long> v;
  long frame = -1;
  for (const auto& l : h.log.lines) {
    if (l.rfind("K ", 0) == 0) frame = std::strtol(l.c_str() + 2, nullptr, 10);
    if (l.rfind("C -1 OnEvent 0010:00000006 ", 0) == 0) v.push_back(frame);
    CHECK_MESSAGE(l.rfind("H 002a:", 0) != 0, l);  // never an H line on the camera
  }
  return v;
}
}  // namespace

TEST_CASE("host sources re-fire (a): a stand-in handler that leaves mode 3 is sent OnEvent(6) at 604, 1205, 1805 only") {
  CHECK(fire_frames(false, 1900) == std::vector<long>{604, 1205, 1805});
}

TEST_CASE("host sources re-fire: a handler that sets mode 0 freezes the camera, OnEvent(6) is sent once (604)") {
  CHECK(fire_frames(true, 1900) == std::vector<long>{604});
}

// native only: the disk file system (fs::open_disk) is not linked into the wasm32 unit tests
#ifndef __EMSCRIPTEN__
// ============================== the real Base.osi ==============================

namespace {
struct RealRun {
  bool ran = false;
  std::string stop;
  long frames = 0;
  std::vector<std::pair<long, std::string>> cminus1;  // natives.log C -1 lines and their frame
  std::vector<std::pair<long, std::string>> h_camera;  // natives.log `H 002a:` lines and their frame
  long f_act = -1;                                   // the first frame in which pc 0x3854d (230733) executes
  std::map<long, std::string> k_bits;                // K frame -> its time field
  long z_blocks = 0, z_refs = 0, z_ref_mismatch = 0, z_ref_unknown = 0;
  std::string first_mismatch;
  std::vector<std::string> z604;  // the camera lines of frame 604's Z block
};

struct LogSink : TraceSink {
  RealRun* r;
  long frame = -1;
  std::map<uint32_t, long> engine;  // the minting G plus every H since (natives.md §3.1)
  void line(std::string_view sv) override {
    const std::string s(sv);
    if (s.rfind("K ", 0) == 0) {
      frame = std::strtol(s.c_str() + 2, nullptr, 10);
      return;
    }
    if (s.rfind("C -1 ", 0) == 0) r->cminus1.push_back({frame, s});
    if (s.rfind("H ", 0) == 0) {
      const uint32_t h = static_cast<uint32_t>(std::strtoul(s.c_str() + 7, nullptr, 16));
      engine[h] += std::strtol(s.c_str() + 16, nullptr, 10);
      if (s.rfind("H 002a:", 0) == 0) r->h_camera.push_back({frame, s});
      return;
    }
    // every `<type>:<handle> G n` of a game value: the first one of a handle is its minting count
    std::istringstream is(s);
    std::vector<std::string> tok{std::istream_iterator<std::string>(is), std::istream_iterator<std::string>()};
    for (size_t i = 0; i + 2 < tok.size(); ++i)
      if (tok[i].size() == 13 && tok[i][4] == ':' && tok[i + 1] == "G") {
        const uint32_t h = static_cast<uint32_t>(std::strtoul(tok[i].c_str() + 5, nullptr, 16));
        if (!engine.count(h)) engine[h] = std::strtol(tok[i + 2].c_str(), nullptr, 10);
      }
  }
};

struct TraceScan : TraceSink {
  RealRun* r;
  LogSink* log;
  long frame = -1;
  void line(std::string_view s) override {
    if (s.size() > 2 && s[0] == 'K' && s[1] == ' ') {
      frame = std::strtol(std::string(s.substr(2, 6)).c_str(), nullptr, 10);
      r->k_bits[frame] = std::string(s.substr(9, 8));
    } else if (r->f_act < 0 && s.rfind("I 0003854d ", 0) == 0) {
      r->f_act = frame;
    } else if (s.rfind("Z ref ", 0) == 0) {
      ++r->z_refs;
      const uint32_t h = static_cast<uint32_t>(std::strtoul(std::string(s.substr(11, 8)).c_str(), nullptr, 16));
      const long n = std::strtol(std::string(s.substr(20)).c_str(), nullptr, 10);
      auto it = log->engine.find(h);
      if (it == log->engine.end()) {
        ++r->z_ref_unknown;
      } else if (it->second != n) {
        if (!r->z_ref_mismatch++) r->first_mismatch = "frame " + std::to_string(frame) + ": " + std::string(s) +
                                                      " vs H sum " + std::to_string(it->second);
      }
    } else if (s.size() == 8 && s[0] == 'Z') {
      ++r->z_blocks;
    } else if (frame == 604 && s.rfind("Z camera.", 0) == 0) {
      r->z604.push_back(std::string(s));
    }
  }
};

const RealRun& real_run() {
  static RealRun r;
  if (r.ran || lomn_data().empty()) return r;
  r.ran = true;
  const std::string user = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/lomn_t4a_real_user";
  std::filesystem::create_directories(user);
  auto vfs = fs::open_disk(lomn_data() + "/install", user);
  REQUIRE(vfs);
  std::vector<uint8_t> image;
  REQUIRE(vfs->read("Root/Data/base.osi", image) == fs::Error::None);
  sage::OsiFile file;
  std::string err;
  REQUIRE_MESSAGE(file.parse(image, &err), err);
  auto slurp = [](const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
  };
  const std::string dir = LOMN_OSI_DATA_DIR;
  const std::string nj = slurp(dir + "/natives.json"), mj = slurp(dir + "/natives_beta150.json"),
                    tj = slurp(dir + "/types.json"), ov = slurp(dir + "/native_overrides.json");
  LogSink log;
  log.r = &r;
  TraceScan scan;
  scan.r = &r;
  scan.log = &log;
  ScriptHost::Options opt;
  opt.real_natives = true;
  ScriptHost host(file, nj, mj, tj, ov, vfs.get(), &scan, &log, opt);
  host.setup();
  for (long k = 0; k <= 2400 && host.frame(); ++k) {}
  r.stop = host.stats().stop;
  r.frames = host.stats().frames;
  std::filesystem::remove_all(user);
  return r;
}
}  // namespace

TEST_CASE("host sources real Base.osi: F_evt = 604 (OnEvent(6) not before, sent at 604), F_act = 605, K bits of hud.md §1.6") {
  SKIP_WITHOUT(!lomn_data().empty());
  const RealRun& r = real_run();
  REQUIRE(r.stop == "none");
  CHECK(r.frames == 2401);
  CHECK(r.k_bits.at(3) == "3cda740e");
  CHECK(r.k_bits.at(602) == "4080a3d0");
  CHECK(r.k_bits.at(603) == "4080da6d");
  CHECK(r.k_bits.at(604) == "4081110a");
  REQUIRE_FALSE(r.cminus1.empty());
  CHECK(r.cminus1.front().first == 604);
  CHECK(r.cminus1.front().second.rfind("C -1 OnEvent 0010:00000006 002a:", 0) == 0);
  CHECK(r.cminus1.front().second.find(" G 1 0010:00000000" + nothings(7)) != std::string::npos);
  CHECK(r.f_act == 605);
  // the oracle's (4b) frame-604 values (controller note): the handler set mode 0 and released the spline
  std::vector<std::string> z = r.z604;
  REQUIRE(z.size() == 8);
  CHECK(z[6].rfind("Z camera.start ", 0) == 0);  // the time of the firing pass (start = time, 0x452c14)
  z.erase(z.begin() + 6);
  CHECK(z == std::vector<std::string>{"Z camera.duration " + dbits(4.0), "Z camera.eye c1a617c2 4178c711 42dd472b",
                                      "Z camera.flags 01 01", "Z camera.index 00 03", "Z camera.mode 00",
                                      "Z camera.spline -", "Z camera.target c1a617c2 4178c711 42cc8396"});
}

TEST_CASE("host sources re-fire (b): the real handler (camera AI mode 0) is never sent OnEvent(6) again through frame 2400") {
  SKIP_WITHOUT(!lomn_data().empty());
  const RealRun& r = real_run();
  REQUIRE(r.stop == "none");
  long sixes = 0;
  for (const auto& c : r.cminus1) sixes += c.second.rfind("C -1 OnEvent 0010:00000006 ", 0) == 0 ? 1 : 0;
  CHECK(sixes == 1);
  CHECK(r.cminus1.size() == 1);  // no mouse source: the camera's call is the only C -1
}

TEST_CASE("host sources real Base.osi: the camera-event frame has no `H 002a:` line; every Z ref equals its H running sum") {
  SKIP_WITHOUT(!lomn_data().empty());
  const RealRun& r = real_run();
  REQUIRE(r.stop == "none");
  for (const auto& h : r.h_camera) CHECK_MESSAGE(h.first != 604, h.second);
  CHECK(r.h_camera.empty());
  CHECK(r.z_blocks == 2401);
  CHECK(r.z_refs > 2401);
  CHECK_MESSAGE(r.z_ref_mismatch == 0, r.first_mismatch);
  CHECK(r.z_ref_unknown == 0);
}
#endif  // __EMSCRIPTEN__
