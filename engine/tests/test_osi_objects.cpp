// M2a-2 T9a: the "objects, model director, camera, HUD" native group (engine/src/osi/natives/objects.cpp), one asm-pin test
// and behaviour tests per native, written before the bindings (RED, then GREEN). Every pin reads the canonical exe as DATA
// through $LOMN_DATA; without it the pin tests report SKIPPED (no LOMN_DATA) and pass. The bytes below were read with
// `objdump -d --x86-asm-syntax=intel` of LEGOBionicle.exe on 2026-10-09; each pin names its instruction address.
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
#include "lomn/osi/heap.h"
#include "lomn/osi/host.h"
#include "lomn/osi/natives.h"
#include "lomn/osi/objects.h"
#include "lomn/osi/registry.h"
#include "lomn/osi/trace.h"
#include "lomn/osi/vm.h"
#include "osi_test_asm.h"

using namespace lomn;
using namespace lomn::osi;
using lomn::test::Asm;

namespace {

// ---------------------------------------------------------------------------------------------- the PE image reader
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
  bool read(uint32_t va, size_t n, std::vector<uint8_t>& out) const {  // zero past a section's raw data (.bss)
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
  uint8_t u8(uint32_t va) const {
    std::vector<uint8_t> b;
    read(va, 1, b);
    return b[0];
  }
  std::string cstr(uint32_t va) const {
    std::string s;
    std::vector<uint8_t> b;
    for (uint32_t a = va; read(a, 1, b) && b[0]; ++a) s += static_cast<char>(b[0]);
    return s;
  }
};

const Pe* exe() {
  static std::unique_ptr<Pe> im;
  static bool tried = false;
  if (!tried) {
    tried = true;
    const char* ld = std::getenv("LOMN_DATA");
    if (ld && *ld) {
      im = std::make_unique<Pe>();
      if (!im->load(std::string(ld) + "/reference/LEGOBionicle.exe")) im.reset();
    }
  }
  return im.get();
}

#define SKIP_WITHOUT(img)              \
  if (!(img)) {                        \
    MESSAGE("SKIPPED (no LOMN_DATA)"); \
    return;                            \
  }
#define PIN(img, va, bytes) CHECK_MESSAGE((img)->hex((va), (sizeof(bytes) + 1) / 3) == bytes, "pin at " #va)

// ---------------------------------------------------------------------------------------------- the registry rows
// The rows of data/osi/natives.json (min/max/types/ret as registered), plus a test native t9.mark that OnEvent calls to
// show it ran, and gcgame.getmainview / gcviewport.getcamera, left typed stubs as in this worktree (another group binds
// them). A `nothing` argument for a game-typed parameter never reaches a native: 0x82's ConvertVariant refuses it
// ("Requested variant conversion unimplimented", opcodes.md), so the bodies' null branches are unreachable from a script.
const char* const kRows = R"([
 {"cls": "ScTileFont", "name": "Create", "min": 1, "max": 1, "types": ["string"], "ret": "game", "ret_class": "ScTileFont", "winner": true},
 {"cls": "ScTextObject", "name": "Create", "min": 5, "max": 5, "types": ["float", "float", "float", "type:ScTileFont", "string"], "ret": "game", "ret_class": "ScTextObject", "winner": true},
 {"cls": "GcBigSprite", "name": "LoadHudSprite", "min": 5, "max": 7, "types": ["float", "float", "float", "float", "string", "int", "int"], "ret": "game", "ret_class": "GcBigSprite", "winner": true},
 {"cls": "ScSLOSISLKCubicSpline", "name": "Create", "min": 1, "max": 1, "types": ["string"], "ret": "game", "ret_class": "ScSLOSISLKCubicSpline", "winner": true},
 {"cls": "GcModelDirector", "name": "AddToa", "min": 1, "max": 1, "types": ["string"], "ret": "int", "winner": true},
 {"cls": "GcModelDirector", "name": "GetToa", "min": 0, "max": 0, "types": [], "ret": "game", "ret_class": "GcToa", "winner": true},
 {"cls": "GcModelDirector", "name": "GetCharacter", "min": 1, "max": 2, "types": ["int", "int"], "ret": "game", "ret_class": "GcCharacter", "winner": true},
 {"cls": "GcModelDirector", "name": "Show", "min": 2, "max": 2, "types": ["int", "int"], "ret": "none", "winner": true},
 {"cls": "GcCharacter", "name": "SetupMoveable", "min": 5, "max": 7, "types": ["type:GcCharacter", "float", "float", "int", "int", "int", "float"], "ret": "none", "winner": true},
 {"cls": "GcLegoCamera", "name": "GetCameraAIMode", "min": 1, "max": 1, "types": ["type:GcLegoCamera"], "ret": "int", "winner": true},
 {"cls": "GcLegoCamera", "name": "SetCameraAIMode", "min": 2, "max": 2, "types": ["type:GcLegoCamera", "int"], "ret": "none", "winner": true},
 {"cls": "GcLegoCamera", "name": "SetCameraSpline", "min": 6, "max": 6, "types": ["type:GcLegoCamera", "type:ScSLOSISLKCubicSpline", "int", "float", "int", "int"], "ret": "none", "winner": true},
 {"cls": "GcLegoCamera", "name": "SetTargetSpline", "min": 7, "max": 7, "types": ["type:GcLegoCamera", "type:ScSLOSISLKCubicSpline", "int", "int", "float", "int", "int"], "ret": "none", "winner": true},
 {"cls": "GcHudDirector", "name": "AddBigSprite", "min": 1, "max": 1, "types": ["type:GcBigSprite"], "ret": "int", "winner": true},
 {"cls": "GcHudDirector", "name": "AddTextObject", "min": 1, "max": 1, "types": ["type:ScTextObject"], "ret": "int", "winner": true},
 {"cls": "GcViewPort", "name": "GetCamera", "min": 1, "max": 1, "types": ["type:GcViewPort"], "ret": "game", "ret_class": "GcLegoCamera", "winner": true},
 {"cls": "GcGame", "name": "GetMainView", "min": 0, "max": 0, "types": [], "ret": "game", "ret_class": "GcViewPort", "winner": true},
 {"cls": "T9", "name": "Mark", "min": 2, "max": 2, "types": ["int", "int"], "ret": "none", "winner": true}
])";
const char* const kTypes = R"({"types": [
 {"name": "type:GcCharacter", "id": 34}, {"name": "type:ScSLOSISLKCubicSpline", "id": 35}, {"name": "type:GcBigSprite", "id": 38},
 {"name": "type:GcViewPort", "id": 40}, {"name": "type:GcLegoCamera", "id": 42}, {"name": "type:ScTextObject", "id": 46},
 {"name": "type:GcToa", "id": 55}, {"name": "type:ScTileFont", "id": 66}]})";

struct Run {
  sage::OsiFile file;
  StringTrace tr, log;
  std::unique_ptr<ScriptHost> host;
  CallResult res;
  Run(Asm& a, std::function<void(ScriptHost&)> pre = {}) {
    std::string err;
    REQUIRE_MESSAGE(file.parse(a.build(), &err), err);
    ScriptHost::Options opt;
    opt.real_natives = true;
    host = std::make_unique<ScriptHost>(file, kRows, "", kTypes, "", nullptr, &tr, &log, opt);
    if (pre) pre(*host);
    res = host->setup();
  }
  std::string err() const { return host->vm().last_error() ? host->vm().last_error()->message : std::string(); }
  bool log_has(const std::string& p) const {
    for (const auto& l : log.lines)
      if (l.find(p) != std::string::npos) return true;
    return false;
  }
  long log_index(const std::string& p) const {
    for (size_t i = 0; i < log.lines.size(); ++i)
      if (log.lines[i].find(p) != std::string::npos) return static_cast<long>(i);
    return -1;
  }
  bool trace_has(const std::string& p) const {
    for (const auto& l : tr.lines)
      if (l.rfind(p, 0) == 0) return true;
    return false;
  }
  long trace_index(const std::string& p) const {
    for (size_t i = 0; i < tr.lines.size(); ++i)
      if (tr.lines[i].rfind(p, 0) == 0) return static_cast<long>(i);
    return -1;
  }
  std::vector<std::string> h_lines() const {
    std::vector<std::string> v;
    for (const auto& l : log.lines)
      if (l.rfind("H ", 0) == 0) v.push_back(l);
    return v;
  }
  HostContext& ctx() { return host->context(); }
};

// script helpers: a global per name; 0x22 stores (pops), 0x21 pushes a duplicate
Asm& gset(Asm& a, const char* g) { return a.op(0x22).u16(a.glob(g)); }
Asm& gget(Asm& a, const char* g) { return a.op(0x21).u16(a.glob(g)); }
// OnEvent(a0..a9) calls t9.mark(a0, a1), so the log shows the nested call and its arguments
void on_event(Asm& a) {
  a.fn("onevent", 10).op(0x21).u16(0).op(0x21).u16(1).call("t9", "mark", 2).pop().nothing().ret();
}
const char* kOnEvent0x32 =
    "OnEvent 0010:00000032 0010:000000fa 000f:00000000 000f:00000000 000f:00000000 000f:00000000 000f:00000000 "
    "000f:00000000 000f:00000000 000f:00000000";

}  // namespace

// ============================== the group ==============================

TEST_CASE("osi objects: bind_natives binds the 15 bodies of the objects group") {
  Asm a;
  a.fn("setup", 0).nothing().ret();
  sage::OsiFile file;
  std::string err;
  REQUIRE_MESSAGE(file.parse(a.build(), &err), err);
  StringTrace tr;
  ScriptHost host(file, kRows, "", kTypes, "", nullptr, &tr, nullptr, ScriptHost::Options{});
  const BindReport rep = bind_natives(host.registry(), host.context());
  CHECK(rep.game_bodies == 15);
  CHECK(host.registry().find("gcviewport", "getcamera")->fn == nullptr);  // another group's: stays a stub here
  CHECK(host.registry().find("t9", "mark")->fn == nullptr);
}

// ============================== sctilefont.create ==============================

TEST_CASE("osi objects sctilefont.create asm pin: new 0x190, ScTileFont::ScTileFont 0x626b40, CreateVariant 0x611950") {
  const Pe* im = exe();
  SKIP_WITHOUT(im);
  PIN(im, 0x611918, "68 90 01 00 00");  // push 0x190 : operator_new size
  PIN(im, 0x611927, "74 08");           // je : a null new skips the constructor ...
  PIN(im, 0x61192c, "e8 0f 52 01 00");  // call ScTileFont::ScTileFont 0x626b40 (argument: w0, the string)
  PIN(im, 0x611936, "e8 15 00 00 00");  // ... and CreateVariant(ScTileFont*) 0x611950 makes nothing of it
  PIN(im, 0x611963, "66 c7 06 0f 00");  // null -> {0x0f, 0}
  PIN(im, 0x611980, "ff 47 04");        // inc [edi+4] : the AddRef
  PIN(im, 0x4f2b88, "c7 40 04 00 00 00 00");  // SxReferenceCountable ctor: count 0 (G 0, natives.md 2.4)
  PIN(im, 0x6276dc, "e8 4f 64 f2 ff");  // the constructor saves the current block file (SrGetCurBlockFile 0x54db30) ...
  PIN(im, 0x6276e9, "e8 32 64 f2 ff");  // ... sets it (0x54db20) ...
  PIN(im, 0x627b07, "e8 14 60 f2 ff");  // ... and restores it: no net change of the slot
}

TEST_CASE("osi objects sctilefont.create: a new font each call, G 0, its name recorded; dies with its last variant") {
  Asm a;
  a.fn("setup", 0).s("Font1").call("sctilefont", "create", 1);
  gset(a, "f").s("Font2").call("sctilefont", "create", 1).pop().nothing().ret();
  ScTileFont* f1 = nullptr;
  Run r(a);
  REQUIRE_MESSAGE(r.res.ok, r.err());
  CHECK(r.log.lines.at(0) == "00000000 sctilefont.create 1 -> 0042:00000001 G 0");
  CHECK(r.log.lines.at(1) == "00000001 sctilefont.create 1 -> 0042:00000002 G 0");
  f1 = dynamic_cast<ScTileFont*>(r.ctx().object(1));
  REQUIRE(f1);
  CHECK(f1->name() == "Font1");
  CHECK(f1->engine_refs() == 0);
  CHECK(r.trace_has("F g 00000002"));  // the second font had one variant: popped, freed
  CHECK_FALSE(r.trace_has("F g 00000001"));
  CHECK(r.ctx().object(2) == nullptr);
}

// ============================== sctextobject.create ==============================

TEST_CASE("osi objects sctextobject.create asm pin: ScTextObject::ScTextObject 0x610c40 holds the font (+1), ~ releases it") {
  const Pe* im = exe();
  SKIP_WITHOUT(im);
  PIN(im, 0x611cf1, "68 64 01 00 00");  // push 0x164
  PIN(im, 0x611d00, "74 1a");           // je : null new
  PIN(im, 0x611d17, "e8 24 ef ff ff");  // call the 5-argument ScTextObject::ScTextObject 0x610c40
  PIN(im, 0x611d21, "e8 1a 00 00 00");  // CreateVariant(ScTextObject*) 0x611d40
  PIN(im, 0x611d53, "66 c7 06 0f 00");  // null -> nothing
  PIN(im, 0x611d70, "ff 47 04");        // the AddRef
  PIN(im, 0x610c83, "89 53 2c");        // [this+0x2c] = the font (w3)
  PIN(im, 0x610c89, "83 7a 2c 00");     // a null font ...
  PIN(im, 0x610c8d, "74 06");           // ... skips ...
  PIN(im, 0x610c92, "ff 42 04");        // ... the font's +1
  PIN(im, 0x610cce, "e8 7d 0b df ff");  // ScFixedString::Init 0x401850 (npos: the text up to its NUL)
  PIN(im, 0x610cd9, "d9 9a 3c 01 00 00");  // x, y, z at +0x13c / +0x140 / +0x144
  PIN(im, 0x610ce5, "d9 9a 40 01 00 00");
  PIN(im, 0x610cf1, "d9 9a 44 01 00 00");
  // the deleting destructor (vtable 0x754758 slot 0) calls ~ScTextObject 0x611070, which releases the font
  CHECK(im->u32(0x754758) == 0x6110c0u);
  CHECK(im->u32(0x75475c) == 0x4f2bc0u);  // slot 1: SxReferenceCountable::Free
  PIN(im, 0x6110e7, "e8 84 ff ff ff");    // call 0x611070
  PIN(im, 0x61108f, "83 7f 2c 00");       // a null font is skipped
  PIN(im, 0x611098, "ff 49 04");          // dec [font+4]
}

TEST_CASE("osi objects sctextobject.create: +1 on its font (an H line, before its own line), G 0, the fields recorded") {
  Asm a;
  a.fn("setup", 0).s("F").call("sctilefont", "create", 1);
  gset(a, "f").f(1.5f).f(2.5f).f(3.5f);
  gget(a, "f").s("Hello").call("sctextobject", "create", 5);
  gset(a, "t").nothing().ret();
  Run r(a);
  REQUIRE_MESSAGE(r.res.ok, r.err());
  REQUIRE(r.log.lines.size() == 3);
  CHECK(r.log.lines[1] == "H 0042:00000001 +1");
  CHECK(r.log.lines[2] == "00000001 sctextobject.create 5 -> 002e:00000002 G 0");
  ScTextObject* t = dynamic_cast<ScTextObject*>(r.ctx().object(2));
  REQUIRE(t);
  CHECK(t->text() == "Hello");
  CHECK(t->x() == 1.5f);
  CHECK(t->y() == 2.5f);
  CHECK(t->z() == 3.5f);
  CHECK(t->font() == r.ctx().object(1));
  CHECK(r.ctx().object(1)->engine_refs() == 1);
  CHECK(r.host->heap().refs(1) == 2);  // the global + the text object
  CHECK(r.ctx().check_engine_refs().empty());
}

TEST_CASE("osi objects sctextobject.create: a nothing font never reaches the body; a freed text object releases its font") {
  {
    Asm a;
    a.fn("setup", 0).f(0).f(0).f(0).nothing().s("x").call("sctextobject", "create", 5).pop().nothing().ret();
    Run r(a);
    CHECK_FALSE(r.res.ok);  // ConvertVariant(nothing, type:ScTileFont) errors in 0x82
    CHECK_FALSE(r.log_has("sctextobject.create"));
  }
  {
    // the text object is popped at once: F g of the text object, then ~ScTextObject's release of the font (H -1); the
    // font keeps the global's variant
    Asm a;
    a.fn("setup", 0).s("F").call("sctilefont", "create", 1);
    gset(a, "f").f(0).f(0).f(0);
    gget(a, "f").s("x").call("sctextobject", "create", 5).pop().nothing().ret();
    Run r(a);
    REQUIRE_MESSAGE(r.res.ok, r.err());
    CHECK(r.h_lines() == std::vector<std::string>{"H 0042:00000001 +1", "H 0042:00000001 -1"});
    CHECK(r.trace_has("F g 00000002"));
    CHECK_FALSE(r.trace_has("F g 00000001"));
    REQUIRE(r.ctx().object(1));
    CHECK(r.ctx().object(1)->engine_refs() == 0);
    CHECK(r.host->heap().refs(1) == 1);
  }
  {
    // the font's last script variant goes first; the text object keeps it alive; dropping the text object frees both
    Asm a;
    a.fn("setup", 0).f(0).f(0).f(0).s("F").call("sctilefont", "create", 1).s("x").call("sctextobject", "create", 5);
    a.pop().nothing().ret();
    Run r(a);
    REQUIRE_MESSAGE(r.res.ok, r.err());
    const long ft = r.trace_index("F g 00000002"), ff = r.trace_index("F g 00000001");
    CHECK(ft >= 0);
    CHECK(ff > ft);  // the text object first, then the font its destructor released
    CHECK(r.ctx().object(1) == nullptr);
    CHECK(r.ctx().object(2) == nullptr);
  }
}

// ============================== gcbigsprite.loadhudsprite ==============================

TEST_CASE("osi objects gcbigsprite.loadhudsprite asm pin: LoadHudSprite 0x440460 always news a GcBigSprite") {
  const Pe* im = exe();
  SKIP_WITHOUT(im);
  PIN(im, 0x43daf4, "8a 02");           // w6 : its low byte
  PIN(im, 0x43daf7, "8a 03");           // w5 : its low byte
  PIN(im, 0x43db09, "e8 52 29 00 00");  // call GcBigSprite::LoadHudSprite 0x440460
  PIN(im, 0x43db16, "e8 05 ff ff ff");  // CreateVariant(GcBigSprite*) 0x43da20
  PIN(im, 0x43da33, "66 c7 06 0f 00");  // null -> nothing
  PIN(im, 0x43da50, "ff 47 04");        // the AddRef
  PIN(im, 0x440468, "e8 c3 d6 10 00");  // saves the current block file ...
  PIN(im, 0x440471, "e8 aa d6 10 00");  // ... sets it to 0 (push 0 at 0x44046f) ...
  PIN(im, 0x44048a, "e8 31 42 0a 00");  // operator_new 0x21c
  PIN(im, 0x440494, "74 1f");           // a null new skips the constructor
  PIN(im, 0x4404b0, "e8 5b ed ff ff");  // GcBigSprite::GcBigSprite 0x43f210
  PIN(im, 0x4404be, "e8 5d d6 10 00");  // ... restores the block file
  PIN(im, 0x4404c4, "89 d8");           // returns the new object: null only when new failed
}

TEST_CASE("osi objects gcbigsprite.loadhudsprite: a new sprite each call, G 0, the arguments recorded (w5/w6 low bytes)") {
  Asm a;
  a.fn("setup", 0).f(1).f(2).f(3).f(4).s("Hud/Sprite").i32(0x1ff).i32(0x102).call("gcbigsprite", "loadhudsprite", 7);
  gset(a, "s").f(0).f(0).f(0).f(0).s("b").i8(1).call("gcbigsprite", "loadhudsprite", 6);
  gset(a, "s2").nothing().ret();
  Run r(a);
  REQUIRE_MESSAGE(r.res.ok, r.err());
  CHECK(r.log.lines.at(0) == "00000000 gcbigsprite.loadhudsprite 7 -> 0026:00000001 G 0");
  CHECK(r.log.lines.at(1) == "00000001 gcbigsprite.loadhudsprite 6 -> 0026:00000002 G 0");
  GcBigSprite* s = dynamic_cast<GcBigSprite*>(r.ctx().object(1));
  REQUIRE(s);
  CHECK(s->name() == "Hud/Sprite");
  CHECK(s->x0() == 1.0f);
  CHECK(s->y1() == 4.0f);
  CHECK(s->b5() == 0xff);
  CHECK(s->b6() == 0x02);
  GcBigSprite* s2 = dynamic_cast<GcBigSprite*>(r.ctx().object(2));
  REQUIRE(s2);
  CHECK(s2->b5() == 1);
  CHECK(s2->b6() == 0);  // an absent argument is w = 0 (0x82: wi = args[i] ? ... : 0)
}

// ============================== scslosislkcubicspline.create ==============================

TEST_CASE("osi objects scslosislkcubicspline.create asm pin: ScSLOSISLKCubicSpline 0x61a4d0, no null after a new") {
  const Pe* im = exe();
  SKIP_WITHOUT(im);
  PIN(im, 0x61a2e8, "6a 0c");           // push 0xc
  PIN(im, 0x61a2f4, "74 08");           // a null new skips the constructor
  PIN(im, 0x61a2f9, "e8 d2 01 00 00");  // ScSLOSISLKCubicSpline::ScSLOSISLKCubicSpline 0x61a4d0
  PIN(im, 0x61a303, "e8 18 00 00 00");  // CreateVariant 0x61a320
  PIN(im, 0x61a333, "66 c7 06 0f 00");
  PIN(im, 0x61a350, "ff 47 04");
  PIN(im, 0x61a4de, "e8 9d 86 ed ff");  // SxReferenceCountable ctor (count 0)
  PIN(im, 0x61a514, "e8 67 00 00 00");  // GetSLKFile 0x61a580: a missing file leaves [this+8] null, the object stays
}

TEST_CASE("osi objects scslosislkcubicspline.create: a new spline each call, G 0, its name recorded") {
  Asm a;
  a.fn("setup", 0).s("cam_spline").call("scslosislkcubicspline", "create", 1);
  gset(a, "s").nothing().ret();
  Run r(a);
  REQUIRE_MESSAGE(r.res.ok, r.err());
  CHECK(r.log.lines.at(0) == "00000000 scslosislkcubicspline.create 1 -> 0023:00000001 G 0");
  auto* sp = dynamic_cast<ScSLOSISLKCubicSpline*>(r.ctx().object(1));
  REQUIRE(sp);
  CHECK(sp->name() == "cam_spline");
}

// ============================== gcmodeldirector.addtoa ==============================

TEST_CASE("osi objects gcmodeldirector.addtoa asm pin: AddToa 0x47e170 -> GcToa::GcToa -> Call(OnEvent, 0x32, 250) at 0x568a59") {
  const Pe* im = exe();
  SKIP_WITHOUT(im);
  PIN(im, 0x47cc36, "6a 00");           // the wrapper's second argument 0
  PIN(im, 0x47cc39, "e8 32 15 00 00");  // call AddToa 0x47e170
  PIN(im, 0x47cc45, "e8 e6 b2 f8 ff");  // CreateVariant 0x407f30 ...
  PIN(im, 0x407f35, "66 c7 03 10 00");  // ... {0x10,
  PIN(im, 0x407f3a, "0f b6 54 24 0c");  //  movzx byte}
  PIN(im, 0x47e194, "e8 77 96 19 00");  // the Toa's id: StringToIdentifier(name)
  PIN(im, 0x47e1d0, "e8 3b 9b 0e 00");  // GcToa::GcToa 0x567d10
  PIN(im, 0x567d7b, "e8 a0 37 ea ff");  // -> GcCharacter::GcCharacter 0x40b520
  PIN(im, 0x40b728, "89 93 fc 00 00 00");  // character id at +0xfc
  PIN(im, 0x40b73e, "89 93 00 01 00 00");  // controller id at +0x100 (the 5th argument, [0x7cb1cc])
  PIN(im, 0x570be3, "c7 05 cc b1 7c 00 31 74 6e 63");  // [0x7cb1cc] = 'cnt1'
  PIN(im, 0x567e27, "66 c7 82 80 03 00 00 fa 00");     // [toa+0x380] = 250
  PIN(im, 0x56898b, "66 89 15 c8 74 74 00");           // the ammo [0x7474c8] = [toa+0x380]
  PIN(im, 0x568992, "6a 32");           // OnEvent a0: 0x32 ...
  PIN(im, 0x56899b, "e8 30 b5 ea ff");  // ... through CreateVariant(byte) 0x413ed0
  PIN(im, 0x413eda, "0f b6 54 24 0c");
  PIN(im, 0x5689a8, "66 8b 82 80 03 00 00");  // a1: the u16 [toa+0x380] ...
  PIN(im, 0x5689b4, "e8 37 fa ea ff");        // ... through CreateVariant(u16) 0x4183f0
  PIN(im, 0x4183fa, "0f b7 54 24 0c");
  PIN(im, 0x5689bb, "66 c7 45 88 0f 00");     // a2..a9 nothing
  PIN(im, 0x568a54, "68 64 b9 73 00");        // the name ...
  CHECK(im->cstr(0x73b964) == "OnEvent");
  PIN(im, 0x568a59, "e8 c2 28 0a 00");        // Call 0x60b320: unconditional in the constructor
  PIN(im, 0x47e1d5, "8b 97 00 01 00 00");     // the node id [+0x10] = [+0x100]
  PIN(im, 0x47e1eb, "89 57 10");
  PIN(im, 0x47e221, "89 15 9c 74 74 00");     // GcSaver::sSaveToa = the id (both branches)
  CHECK(im->u32(0x74749c) == 0x3f3f3f3fu);
  CHECK(im->u32(0x7474c8) == 0u);
  PIN(im, 0x47e4af, "8b 15 84 28 78 00");     // Add(toa, [0x782884] = 'cnt1')
  PIN(im, 0x482d43, "c7 05 84 28 78 00 31 74 6e 63");
  PIN(im, 0x47e4bd, "e8 ee 29 00 00");
  PIN(im, 0x47e4c8, "75 46");                 // Add false -> return 0 (0x47e4ca) ...
  PIN(im, 0x47e4ca, "30 db");
  PIN(im, 0x47e527, "89 3d b8 32 70 00");     // ... true -> GcGame::sToa = the Toa ...
  PIN(im, 0x47e657, "88 d8");                 // ... return 1 (bl from Add)
  // GcModelDirector::Add 0x480eb0: a temporary +1, the map insert (copies on success), the temporary -1
  PIN(im, 0x480ec7, "ff 42 04");
  PIN(im, 0x480ed9, "b9 04 2a 78 00");
  PIN(im, 0x480ede, "e8 8d 13 00 00");
  PIN(im, 0x480f08, "ff 4a 04");
  PIN(im, 0x480f19, "8a 45 dc");              // returns the insert's bool
}

TEST_CASE("osi objects gcmodeldirector.addtoa: OnEvent(0x32, 250) inside the native (C N first), int 1; the Toa enters the map") {
  Asm a;
  a.fn("setup", 0).s("onua").call("gcmodeldirector", "addtoa", 1).ret();
  on_event(a);
  Run r(a);
  REQUIRE_MESSAGE(r.res.ok, r.err());
  CHECK(r.res.result == make_int(1));
  REQUIRE(r.log.lines.size() == 3);
  CHECK(r.log.lines[0] == std::string("C 00000000 ") + kOnEvent0x32);
  CHECK(r.log.lines[1] == "00000001 t9.mark 2 -> 000f:00000000");
  CHECK(r.log.lines[2] == "00000000 gcmodeldirector.addtoa 1 -> 0010:00000001");
  CHECK(r.host->stats().max_nesting == 2);
  ModelDirector& md = r.ctx().subsystem<ModelDirector>();
  GcCharacter* c = md.get(kIdCnt1);
  REQUIRE(c);
  auto* toa = dynamic_cast<GcToa*>(c);
  REQUIRE(toa);
  CHECK(toa->name() == "onua");
  CHECK(toa->character_id() == 0x6f6e7561u);
  CHECK(toa->controller_id() == kIdCnt1);
  CHECK(toa->node_id() == kIdCnt1);
  CHECK(toa->engine_refs() == 1);  // the map's copy (temporary +1/-1 cancel, no handle yet: no H line)
  CHECK(toa->handle() == 0);
  CHECK(md.s_toa == toa);
  CHECK(md.save_toa == 0x6f6e7561u);
  CHECK(md.ammo == 250);
  CHECK(md.main_character_id() == kIdCnt1);
}

TEST_CASE("osi objects gcmodeldirector.addtoa: a second Toa still fires OnEvent, but Add fails: int 0, the map keeps the first") {
  Asm a;
  a.fn("setup", 0).s("onua").call("gcmodeldirector", "addtoa", 1).pop().s("tahu").call("gcmodeldirector", "addtoa", 1).ret();
  on_event(a);
  Run r(a);
  REQUIRE_MESSAGE(r.res.ok, r.err());
  CHECK(r.res.result == make_int(0));
  CHECK(r.log_has("00000002 gcmodeldirector.addtoa 1 -> 0010:00000000"));
  CHECK(r.log_has(std::string("C 00000002 ") + kOnEvent0x32));
  ModelDirector& md = r.ctx().subsystem<ModelDirector>();
  REQUIRE(md.map.size() == 1);
  auto* toa = dynamic_cast<GcToa*>(md.get(kIdCnt1));
  REQUIRE(toa);
  CHECK(toa->name() == "onua");
  CHECK(md.s_toa == toa);              // 0x47e527 is on the success branch only
  CHECK(md.save_toa == 0x74616875u);  // sSaveToa is written before Add (0x47e221): the second name's id
}

TEST_CASE("osi objects gcmodeldirector.addtoa: an error in OnEvent propagates out of the native (no swallow)") {
  Asm a;
  a.fn("setup", 0).s("onua").call("gcmodeldirector", "addtoa", 1).ret();
  a.fn("onevent", 10).op(0x02).ret();  // 0x02 always errors
  Run r(a);
  CHECK_FALSE(r.res.ok);
  CHECK_FALSE(r.log_has("gcmodeldirector.addtoa 1 ->"));  // the native writes no line of its own (natives.md 3.2)
  CHECK(r.ctx().subsystem<ModelDirector>().map.empty());
}

// ============================== gcmodeldirector.gettoa / getcharacter ==============================

TEST_CASE("osi objects gcmodeldirector.gettoa/getcharacter asm pin: Get 0x480f70 on map 0x782a04; the 'cnt1' key; the overloads") {
  const Pe* im = exe();
  SKIP_WITHOUT(im);
  PIN(im, 0x47cf86, "8b 15 84 28 78 00");  // gettoa: key [0x782884] = 'cnt1' (0x482d43)
  PIN(im, 0x47cf93, "e8 d8 3f 00 00");     // Get 0x480f70
  PIN(im, 0x47cf9e, "e8 0d 00 00 00");     // CreateVariant(GcToa*) 0x47cfb0
  PIN(im, 0x47cfc3, "66 c7 06 0f 00");
  PIN(im, 0x47cfe0, "ff 47 04");
  PIN(im, 0x47ce9e, "8b 13");              // getcharacter: w1 loaded and overwritten ...
  PIN(im, 0x47cea0, "8b 16");              // ... by w0, the key
  PIN(im, 0x47ceb4, "e8 b7 40 00 00");     // Get
  PIN(im, 0x47cebf, "e8 0c 00 00 00");     // CreateVariant(GcCharacter*) 0x47ced0
  PIN(im, 0x47cee3, "66 c7 06 0f 00");
  PIN(im, 0x47cf00, "ff 47 04");
  PIN(im, 0x480f8b, "b9 04 2a 78 00");     // Get: find in 0x782a04 ...
  PIN(im, 0x480f7c, "31 ff");              // ... edi = 0: not found -> null ...
  PIN(im, 0x480ff7, "8b 3a");              // ... else the stored pointer, no reference taken
}

TEST_CASE("osi objects gcmodeldirector.gettoa/getcharacter: nothing before addtoa; then the same Toa, two type tags, G 1") {
  Asm a;
  a.fn("setup", 0).call("gcmodeldirector", "gettoa", 0).pop();
  a.i32(static_cast<int32_t>(kIdCnt1)).call("gcmodeldirector", "getcharacter", 1).pop();
  a.s("onua").call("gcmodeldirector", "addtoa", 1).pop();
  a.call("gcmodeldirector", "gettoa", 0);
  gset(a, "t").i32(static_cast<int32_t>(kIdCnt1)).i8(7).call("gcmodeldirector", "getcharacter", 2);
  gset(a, "c").i32(0x6f6e7561).call("gcmodeldirector", "getcharacter", 1).ret();  // 'onua' is no key: nothing
  on_event(a);
  Run r(a);
  REQUIRE_MESSAGE(r.res.ok, r.err());
  CHECK(r.log_has("00000000 gcmodeldirector.gettoa 0 -> 000f:00000000"));
  CHECK(r.log_has("00000001 gcmodeldirector.getcharacter 1 -> 000f:00000000"));
  CHECK(r.log_has("00000004 gcmodeldirector.gettoa 0 -> 0037:00000001 G 1"));
  CHECK(r.log_has("00000005 gcmodeldirector.getcharacter 2 -> 0022:00000001 G 1"));
  CHECK(r.log_has("00000006 gcmodeldirector.getcharacter 1 -> 000f:00000000"));
  CHECK(r.trace_has("A g 00000001 37"));  // minted under GcToa, the object's class
  CHECK(r.host->heap().refs(1) == 3);      // map + two globals
  CHECK(r.h_lines().empty());
}

// ============================== gcmodeldirector.show ==============================

TEST_CASE("osi objects gcmodeldirector.show asm pin: Show 0x47f830, the Toa node's copy (+1) and GcBAGroup::SetCharacter (+1)") {
  const Pe* im = exe();
  SKIP_WITHOUT(im);
  PIN(im, 0x47cd82, "8b 12");              // the wrapper passes &w0, &w1 ...
  PIN(im, 0x47cd94, "e8 97 2a 00 00");     // ... to Show 0x47f830
  PIN(im, 0x47cd9b, "66 c7 07 0f 00");     // and returns nothing
  PIN(im, 0x47f97c, "e8 ef 15 00 00");     // Get(id)
  PIN(im, 0x47f98a, "8b 15 84 28 78 00");  // id == [0x782884] ('cnt1') ...
  PIN(im, 0x47f992, "0f 85 98 00 00 00");  // ... else the main-guy branch 0x47fa30
  PIN(im, 0x47f99b, "8b 50 10");           // [obj+0x10]: a null object (an id not in the map) faults here ...
  PIN(im, 0x47fa33, "8b 50 10");           // ... or here
  PIN(im, 0x47f9a9, "8b 0d a8 90 83 00");  // the Toa node [0x8390a8] ...
  PIN(im, 0x47f9b6, "ff 56 40");           // ... FindChild(id) (vtable +0x40) ...
  PIN(im, 0x47f9bb, "74 08");              // ... found -> return: no reference, no SetCharacter
  PIN(im, 0x47f9d3, "ff 56 28");           // SetParentNode (0x40fc00: stores the parent, no reference)
  PIN(im, 0x40fc07, "89 53 20");
  PIN(im, 0x47f9df, "ff 42 04");           // the temporary ScAutoRefPtr: +1
  PIN(im, 0x47f9eb, "e8 a0 9e f9 ff");     // vector push_back 0x419890: the copy ...
  PIN(im, 0x41992b, "e8 d0 01 00 00");     // ... ScAutoRefPtr copy ctor 0x419b00 (no reallocation) ...
  PIN(im, 0x419a29, "e8 d2 00 00 00");     // ... (after a reallocation) ...
  PIN(im, 0x419b12, "ff 42 04");           // ... +1, kept by the node
  PIN(im, 0x47f9f9, "ff 49 04");           // the temporary: -1
  PIN(im, 0x47fa41, "8b 0d 4c aa 83 00");  // main-guy branch: the main-guy node [0x83aa4c], same FindChild / +1 / copy / -1
  PIN(im, 0x47fa7a, "ff 42 04");
  PIN(im, 0x47fa86, "e8 05 9e f9 ff");
  PIN(im, 0x47fa94, "ff 49 04");
  PIN(im, 0x47fb35, "8b 15 84 28 78 00");  // 'cnt1' again ...
  PIN(im, 0x47fb3d, "75 09");
  PIN(im, 0x47fb42, "e8 e9 b7 0c 00");     // ... -> GcBAGroup::SetCharacter 0x54b330
  PIN(im, 0x54b338, "39 3d 3c 90 83 00");  // the same character -> no change
  PIN(im, 0x54b33e, "74 2a");
  PIN(im, 0x54b344, "ff 47 04");           // +1 on the new one
  PIN(im, 0x54b356, "ff 49 04");           // -1 on the old one
  PIN(im, 0x54b364, "89 3d 3c 90 83 00");  // [0x83903c] = the character
}

TEST_CASE("osi objects gcmodeldirector.show: 'cnt1' first time H +1 +1 -1 +1 on the Toa (G 3 after), a second show changes nothing") {
  Asm a;
  a.fn("setup", 0).s("onua").call("gcmodeldirector", "addtoa", 1).pop().call("gcmodeldirector", "gettoa", 0);
  gset(a, "t").i32(static_cast<int32_t>(kIdCnt1)).i8(0).call("gcmodeldirector", "show", 2).pop();
  a.i32(static_cast<int32_t>(kIdCnt1)).i8(0).call("gcmodeldirector", "show", 2).pop();
  a.i32(static_cast<int32_t>(kIdCnt1)).call("gcmodeldirector", "getcharacter", 1).ret();
  on_event(a);
  Run r(a);
  REQUIRE_MESSAGE(r.res.ok, r.err());
  const long show1 = r.log_index("gcmodeldirector.show 2 ->");
  REQUIRE(show1 >= 4);
  CHECK(r.log.lines[show1 - 4] == "H 0037:00000001 +1");  // the temporary (0x47f9df)
  CHECK(r.log.lines[show1 - 3] == "H 0037:00000001 +1");  // the node's copy (0x419b12)
  CHECK(r.log.lines[show1 - 2] == "H 0037:00000001 -1");  // the temporary released (0x47f9f9)
  CHECK(r.log.lines[show1 - 1] == "H 0037:00000001 +1");  // GcBAGroup::SetCharacter (0x54b344)
  CHECK(r.h_lines().size() == 4);                         // the second show: FindChild finds it, returns
  CHECK(r.log_has("gcmodeldirector.getcharacter 1 -> 0022:00000001 G 3"));
  ModelDirector& md = r.ctx().subsystem<ModelDirector>();
  CHECK(md.ba_character == md.get(kIdCnt1));
  CHECK(md.toa_node.count(kIdCnt1) == 1);
  CHECK(r.ctx().check_engine_refs().empty());
}

TEST_CASE("osi objects gcmodeldirector.show: an id not in the map faults in the original (Undefined); another character takes the main-guy node") {
  {
    Asm a;
    a.fn("setup", 0).i32(static_cast<int32_t>(kIdCnt1)).i8(0).call("gcmodeldirector", "show", 2).ret();
    Run r(a);
    CHECK_FALSE(r.res.ok);
    CHECK(r.err().find("Script is calling") != std::string::npos);
  }
  {
    // a character under another id (inserted as the map does, one reference): +1 +1 -1 into the main-guy node, no
    // SetCharacter
    GcCharacter* other = nullptr;
    Asm a;
    a.fn("setup", 0).i32(0x6f746865).call("gcmodeldirector", "getcharacter", 1);
    gset(a, "c").i32(0x6f746865).i8(0).call("gcmodeldirector", "show", 2).pop();
    a.i32(0x6f746865).i8(0).call("gcmodeldirector", "show", 2).ret();
    Run r(a, [&](ScriptHost& h) {
      HostContext& ctx = h.context();
      other = ctx.make<GcCharacter>(34, 0x6f746865u, 0x6f746865u);
      ctx.add_ref(other);
      ctx.subsystem<ModelDirector>().map[0x6f746865u] = other;
    });
    REQUIRE_MESSAGE(r.res.ok, r.err());
    CHECK(r.h_lines() == std::vector<std::string>{"H 0022:00000001 +1", "H 0022:00000001 +1", "H 0022:00000001 -1"});
    ModelDirector& md = r.ctx().subsystem<ModelDirector>();
    CHECK(md.mainguy_node.count(0x6f746865u) == 1);
    CHECK(md.ba_character == nullptr);
    CHECK(other->engine_refs() == 2);
  }
}

// ============================== gccharacter.setupmoveable ==============================

TEST_CASE("osi objects gccharacter.setupmoveable asm pin: SetupMoveable -> the MainCharacterID guard -> SetUnderWater's OnEvent(0x36, b)") {
  const Pe* im = exe();
  SKIP_WITHOUT(im);
  PIN(im, 0x4093f9, "8b 08");              // this = w0 (the character)
  PIN(im, 0x4093fb, "e8 50 f4 00 00");     // GcCharacter::SetupMoveable 0x418850
  PIN(im, 0x409403, "66 c7 00 0f 00");     // returns nothing
  PIN(im, 0x418856, "83 bb 24 01 00 00 00");  // [char+0x124] (the motion system) null -> nothing; a null char faults
  PIN(im, 0x41885d, "0f 84 60 01 00 00");
  PIN(im, 0x41887e, "e8 fd c2 15 00");     // GcMotionSystem::SetupMoveable 0x574b80
  PIN(im, 0x40beeb, "e8 10 07 00 00");     // every GcCharacter builds its motion system (InitMotionSystem 0x40c600) ...
  PIN(im, 0x40c60b, "8b 9f 00 01 00 00");  // ... with the controller id [char+0x100] ...
  PIN(im, 0x40c62f, "e8 6c 7c 16 00");     // ... GcMotionSystem::GcMotionSystem 0x5742a0 ...
  PIN(im, 0x5748ec, "89 53 14");           // ... -> [ms+0x14]
  PIN(im, 0x574ee5, "e8 a6 60 ec ff");     // MainCharacterID 0x43af90
  PIN(im, 0x574eeb, "8b 4e 14");           // [ms+0x14] ...
  PIN(im, 0x574ef1, "39 d1");              // ... == MainCharacterID ...
  PIN(im, 0x574ef3, "75 0d");
  PIN(im, 0x574ef5, "a0 a8 c5 73 00");     // ... -> SetUnderWater(byte [0x73c5a8])
  CHECK(im->u8(0x73c5a8) == 0);
  PIN(im, 0x574efd, "e8 4e 41 01 00");
  PIN(im, 0x58905c, "80 7d 08 00");        // b != 0 ...
  PIN(im, 0x58907b, "6a 36");              // ... OnEvent(0x36, 1) ...
  PIN(im, 0x58908b, "6a 01");
  PIN(im, 0x589094, "e8 97 ee e7 ff");     // (CreateVariant 0x407f30)
  PIN(im, 0x589154, "e8 c7 21 08 00");
  PIN(im, 0x589194, "6a 36");              // ... else OnEvent(0x36, 0)
  PIN(im, 0x5891a1, "6a 00");
  PIN(im, 0x589249, "e8 d2 20 08 00");
  PIN(im, 0x589244, "68 20 df 73 00");
  CHECK(im->cstr(0x73df20) == "OnEvent");
  // MainCharacterID 0x43af90: sToa null -> [0x7796f4] ('cnt1', 0x43c193); else [sToa+0x1d0] null -> the same; else its
  // [+0x100]. [char+0x1d0] = 0 in the GcCharacter constructor (0x40bbba).
  PIN(im, 0x43af91, "83 3d b8 32 70 00 00");
  PIN(im, 0x43af9e, "8b 15 f4 96 77 00");
  PIN(im, 0x43afb8, "83 ba d0 01 00 00 00");
  PIN(im, 0x43afcb, "8b 92 00 01 00 00");
  PIN(im, 0x43c193, "c7 05 f4 96 77 00 31 74 6e 63");
  PIN(im, 0x40bbba, "c7 82 d0 01 00 00 00 00 00 00");
}

TEST_CASE("osi objects gccharacter.setupmoveable: on the Toa it calls OnEvent(0x36, 0); off the main character nothing") {
  Asm a;
  a.fn("setup", 0).s("onua").call("gcmodeldirector", "addtoa", 1).pop();
  a.i32(static_cast<int32_t>(kIdCnt1)).call("gcmodeldirector", "getcharacter", 1);
  gset(a, "c");
  gget(a, "c").f(1).f(2).i8(1).i8(0).i8(1).call("gccharacter", "setupmoveable", 6).ret();
  on_event(a);
  Run r(a);
  REQUIRE_MESSAGE(r.res.ok, r.err());
  const long c = r.log_index("C 00000003 OnEvent 0010:00000036 0010:00000000 000f:00000000");
  REQUIRE(c >= 0);
  CHECK(r.log.lines.at(c + 1) == "00000004 t9.mark 2 -> 000f:00000000");
  CHECK(r.log.lines.at(c + 2) == "00000003 gccharacter.setupmoveable 6 -> 000f:00000000");
  // [0x73c5a8] != 0 would pass 1 (the other branch of SetUnderWater)
  {
    Asm b;
    b.fn("setup", 0).s("onua").call("gcmodeldirector", "addtoa", 1).pop();
    b.i32(static_cast<int32_t>(kIdCnt1)).call("gcmodeldirector", "getcharacter", 1);
    b.f(0).f(0).i8(0).i8(0).i8(0).call("gccharacter", "setupmoveable", 6).ret();
    on_event(b);
    Run r2(b, [](ScriptHost& h) { h.context().subsystem<ModelDirector>().global_underwater = 1; });
    REQUIRE_MESSAGE(r2.res.ok, r2.err());
    CHECK(r2.log_has("C 00000003 OnEvent 0010:00000036 0010:00000001 000f:00000000"));
  }
  // a character whose motion id is not MainCharacterID: no call
  {
    Asm b;
    b.fn("setup", 0).i32(0x6f746865).call("gcmodeldirector", "getcharacter", 1);
    b.f(0).f(0).i8(0).i8(0).i8(0).call("gccharacter", "setupmoveable", 6).ret();
    on_event(b);
    Run r3(b, [](ScriptHost& h) {
      HostContext& ctx = h.context();
      GcCharacter* o = ctx.make<GcCharacter>(34, 0x6f746865u, 0x6f746865u);
      ctx.add_ref(o);
      ctx.subsystem<ModelDirector>().map[0x6f746865u] = o;
    });
    REQUIRE_MESSAGE(r3.res.ok, r3.err());
    CHECK_FALSE(r3.log_has("OnEvent"));
    CHECK(r3.log_has("00000001 gccharacter.setupmoveable 6 -> 000f:00000000"));
  }
  // a nothing character never reaches the body (0x82's ConvertVariant refuses it); the body's null test is defensive
  {
    Asm b;
    b.fn("setup", 0).nothing().f(0).f(0).i8(0).i8(0).i8(0).call("gccharacter", "setupmoveable", 6).ret();
    Run r4(b);
    CHECK_FALSE(r4.res.ok);
  }
}

TEST_CASE("osi objects MainCharacterID: 'cnt1' before and after addtoa (the Toa's [+0x1d0] stays null)") {
  Asm a;
  a.fn("setup", 0).nothing().ret();
  Run r(a);
  ModelDirector& md = r.ctx().subsystem<ModelDirector>();
  CHECK(md.main_character_id() == kIdCnt1);
}

// ============================== gclegocamera.getcameraaimode / setcameraaimode ==============================

TEST_CASE("osi objects gclegocamera ai mode asm pin: the byte [cam+0x3a1], 0 from the constructor; the spline release on leaving 2/3") {
  const Pe* im = exe();
  SKIP_WITHOUT(im);
  PIN(im, 0x44ed18, "8a 82 a1 03 00 00");  // getcameraaimode: al = [cam+0x3a1] ...
  PIN(im, 0x44ed23, "e8 18 00 00 00");     // ... CreateVariant 0x44ed40 ...
  PIN(im, 0x44ed45, "66 c7 03 10 00");     // ... {0x10,
  PIN(im, 0x44ed4a, "0f b6 54 24 0c");     //  movzx byte}
  PIN(im, 0x44ecee, "8a 02");              // setcameraaimode: the low byte of w1 ...
  PIN(im, 0x44ecf3, "e8 98 c7 00 00");     // ... SetCameraAIMode 0x45b490
  PIN(im, 0x44ecf8, "66 c7 06 0f 00");
  PIN(im, 0x45b49b, "8a 87 a1 03 00 00");  // old = [cam+0x3a1]
  PIN(im, 0x45b4a7, "88 97 a1 03 00 00");  // [cam+0x3a1] = new, unconditionally
  PIN(im, 0x45b558, "80 45 f0 fe");        // old - 2 ...
  PIN(im, 0x45b55c, "80 7d f0 01");        // ... <= 1 (old is 2 or 3) ...
  PIN(im, 0x45b560, "77 64");
  PIN(im, 0x45b56a, "8a 15 dc 6f 70 00");  // ... and new != [0x706fdc] ...
  PIN(im, 0x45b581, "8a 15 e0 6f 70 00");  // ... and new != [0x706fe0] ...
  CHECK(im->u8(0x706fdc) == 2);
  CHECK(im->u8(0x706fe0) == 3);
  PIN(im, 0x45b590, "83 bf a4 03 00 00 00");  // ... and a spline held ...
  PIN(im, 0x45b5a8, "ff 49 04");              // ... -> released ...
  PIN(im, 0x45b5b6, "c7 87 a4 03 00 00 00 00 00 00");  // ... and dropped
  PIN(im, 0x44f6a0, "c6 82 a1 03 00 00 00");  // GcLegoCamera::GcLegoCamera 0x44f3e0: mode 0 ...
  PIN(im, 0x44f6ad, "c7 82 a4 03 00 00 00 00 00 00");  // ... no spline
}

TEST_CASE("osi objects gclegocamera ai mode: 0 at first, set -> get reads the low byte back; a nothing camera is refused") {
  Asm a;
  a.fn("setup", 0).call("gcgame", "getmainview", 0).call("gcviewport", "getcamera", 1);
  gset(a, "cam");
  gget(a, "cam").call("gclegocamera", "getcameraaimode", 1).pop();
  gget(a, "cam").i32(0x10b).call("gclegocamera", "setcameraaimode", 2).pop();
  gget(a, "cam").call("gclegocamera", "getcameraaimode", 1).ret();
  Run r(a);
  REQUIRE_MESSAGE(r.res.ok, r.err());
  // seq 0 getmainview, 1 getcamera (typed stubs of another group: fresh handles)
  CHECK(r.log_has("00000002 gclegocamera.getcameraaimode 1 -> 0010:00000000"));
  CHECK(r.log_has("00000003 gclegocamera.setcameraaimode 2 -> 000f:00000000"));
  CHECK(r.log_has("00000004 gclegocamera.getcameraaimode 1 -> 0010:0000000b"));
  CHECK(r.res.result == make_int(0x0b));
  {
    Asm b;
    b.fn("setup", 0).nothing().call("gclegocamera", "getcameraaimode", 1).ret();
    Run r2(b);
    CHECK_FALSE(r2.res.ok);
  }
  {
    Asm b;
    b.fn("setup", 0).nothing().i8(3).call("gclegocamera", "setcameraaimode", 2).ret();
    Run r2(b);
    CHECK_FALSE(r2.res.ok);
  }
}

// ============================== gclegocamera.setcameraspline / settargetspline ==============================

TEST_CASE("osi objects gclegocamera spline asm pin: SetCameraSpline 0x45b380 / SetTargetSpline 0x45b400 share [cam+0x3a4]") {
  const Pe* im = exe();
  SKIP_WITHOUT(im);
  PIN(im, 0x44f1af, "8b 08");           // this = w0
  PIN(im, 0x44f1b1, "e8 ca c1 00 00");  // SetCameraSpline
  PIN(im, 0x44f1b9, "66 c7 00 0f 00");
  PIN(im, 0x44f21b, "8b 08");
  PIN(im, 0x44f21d, "e8 de c1 00 00");  // SetTargetSpline
  PIN(im, 0x45b38b, "39 9f a4 03 00 00");  // the same spline -> no change
  PIN(im, 0x45b391, "74 2a");
  PIN(im, 0x45b397, "ff 43 04");           // +1 on the new (skipped for null, 0x45b395)
  PIN(im, 0x45b3a9, "ff 49 04");           // -1 on the old
  PIN(im, 0x45b3b7, "89 9f a4 03 00 00");
  PIN(im, 0x45b40b, "39 9f a4 03 00 00");  // SetTargetSpline: the same field
  PIN(im, 0x45b417, "ff 43 04");
  PIN(im, 0x45b429, "ff 49 04");
  PIN(im, 0x45b437, "89 9f a4 03 00 00");
}

TEST_CASE("osi objects gclegocamera spline: held with +1, the same spline again changes nothing, a new one +1 then -1 the old") {
  Asm a;
  a.fn("setup", 0).call("gcgame", "getmainview", 0).call("gcviewport", "getcamera", 1);
  gset(a, "cam").s("a").call("scslosislkcubicspline", "create", 1);
  gset(a, "s1").s("b").call("scslosislkcubicspline", "create", 1);
  gset(a, "s2");
  gget(a, "cam");
  gget(a, "s1").i8(0).f(1).i8(1).i8(0).call("gclegocamera", "setcameraspline", 6).pop();  // +1 s1
  gget(a, "cam");
  gget(a, "s1").i8(0).i8(0).f(1).i8(1).i8(0).call("gclegocamera", "settargetspline", 7).pop();  // same: nothing
  gget(a, "cam");
  gget(a, "s2").i8(0).i8(0).f(1).i8(1).i8(0).call("gclegocamera", "settargetspline", 7).pop();  // +1 s2, -1 s1
  gget(a, "cam");
  gget(a, "s1").i8(0).f(1).i8(1).i8(0).call("gclegocamera", "setcameraspline", 6).pop();  // back to s1: +1 s1, -1 s2
  a.nothing().ret();
  Run r(a);
  REQUIRE_MESSAGE(r.res.ok, r.err());
  CHECK(r.h_lines() == std::vector<std::string>{"H 0023:00000003 +1", "H 0023:00000004 +1", "H 0023:00000003 -1",
                                                "H 0023:00000003 +1", "H 0023:00000004 -1"});
  const long l = r.log_index("H 0023:00000003 +1");
  REQUIRE(l >= 0);
  CHECK(r.log.lines.at(l + 1) == "00000004 gclegocamera.setcameraspline 6 -> 000f:00000000");  // seqs 0-3: view, camera, 2 splines
  CHECK(r.ctx().check_engine_refs().empty());
}

TEST_CASE("osi objects gclegocamera ai mode: leaving 2/3 for another mode releases the spline; 3 -> 2 keeps it") {
  auto run = [](int from, int to) {
    Asm a;
    a.fn("setup", 0).call("gcgame", "getmainview", 0).call("gcviewport", "getcamera", 1);
    gset(a, "cam").s("a").call("scslosislkcubicspline", "create", 1);
    gset(a, "s");
    gget(a, "cam");
    gget(a, "s").i8(0).f(1).i8(1).i8(0).call("gclegocamera", "setcameraspline", 6).pop();
    gget(a, "cam").i32(from).call("gclegocamera", "setcameraaimode", 2).pop();
    gget(a, "cam").i32(to).call("gclegocamera", "setcameraaimode", 2).pop();
    a.nothing().ret();
    auto r = std::make_unique<Run>(a);
    REQUIRE_MESSAGE(r->res.ok, r->err());
    return r->h_lines();
  };
  CHECK(run(3, 0xb) == std::vector<std::string>{"H 0023:00000003 +1", "H 0023:00000003 -1"});
  CHECK(run(2, 0) == std::vector<std::string>{"H 0023:00000003 +1", "H 0023:00000003 -1"});
  CHECK(run(3, 2) == std::vector<std::string>{"H 0023:00000003 +1"});
  CHECK(run(2, 3) == std::vector<std::string>{"H 0023:00000003 +1"});
  CHECK(run(0xb, 3) == std::vector<std::string>{"H 0023:00000003 +1"});
  CHECK(run(3, 3) == std::vector<std::string>{"H 0023:00000003 +1"});
  CHECK(run(0x103, 0x0b) == std::vector<std::string>{"H 0023:00000003 +1", "H 0023:00000003 -1"});  // the byte is 3
}

// ============================== gchuddirector.addbigsprite / addtextobject ==============================

TEST_CASE("osi objects gchuddirector.addbigsprite/addtextobject asm pin: a render node holds the object (+1); the id counter") {
  const Pe* im = exe();
  SKIP_WITHOUT(im);
  PIN(im, 0x47afee, "e8 3d 07 00 00");     // AddBigSprite 0x47b730
  PIN(im, 0x47affd, "e8 ee cd f8 ff");     // CreateVariant 0x407df0: {0x10, the id}
  PIN(im, 0x407dfb, "66 c7 03 10 00");
  PIN(im, 0x407e00, "89 53 04");
  PIN(im, 0x47b01e, "e8 5d 07 00 00");     // AddTextObject 0x47b780
  PIN(im, 0x47b02d, "e8 be cd f8 ff");
  PIN(im, 0x47b75a, "e8 f1 0f 03 00");     // ScRenderNode::ScRenderNode 0x4ac750 (object = w0) ...
  PIN(im, 0x47b7aa, "e8 a1 0f 03 00");
  PIN(im, 0x4ac7c4, "89 53 1c");           // ... [node+0x1c] = the object ...
  PIN(im, 0x4ac7ca, "83 7a 1c 00");        // ... null skips ...
  PIN(im, 0x4ac7d3, "ff 42 04");           // ... +1
  PIN(im, 0x47b764, "e8 d7 01 00 00");     // GcHudDirector::Add 0x47b940
  PIN(im, 0x47b7b4, "e8 87 01 00 00");
  PIN(im, 0x47b94f, "8b 35 3c aa 83 00");  // id = sNextHudElementID [0x83aa3c] ...
  PIN(im, 0x47b957, "42");                 // ... + 1 ...
  PIN(im, 0x47b958, "89 15 3c aa 83 00");  // ... stored
  PIN(im, 0x47cb54, "c7 05 24 91 83 00 00 00 00 00");  // static init: [0x839124] = 0 ...
  PIN(im, 0x47cb64, "89 15 3c aa 83 00");              // ... [0x83aa3c] = [0x839124]
  PIN(im, 0x47b989, "e8 42 00 00 00");     // the map insert (0x47b9d0 -> GcIdentifierMap::insert 0x650500)
  PIN(im, 0x47b995, "c7 07 00 00 00 00");  // failure -> 0
  PIN(im, 0x47b9b0, "89 37");              // success -> the id
}

TEST_CASE("osi objects gchuddirector.addbigsprite/addtextobject: ids 0, 1, 2 from one counter; +1 on the object (H)") {
  Asm a;
  a.fn("setup", 0).f(0).f(0).f(1).f(1).s("s").call("gcbigsprite", "loadhudsprite", 5);
  gset(a, "s");
  gget(a, "s").call("gchuddirector", "addbigsprite", 1).pop();
  a.f(0).f(0).f(0).s("F").call("sctilefont", "create", 1).s("t").call("sctextobject", "create", 5);
  gset(a, "t");
  gget(a, "t").call("gchuddirector", "addtextobject", 1).pop();
  gget(a, "s").call("gchuddirector", "addbigsprite", 1).ret();  // the same sprite again: a second node, a second +1
  Run r(a);
  REQUIRE_MESSAGE(r.res.ok, r.err());
  CHECK(r.log.lines.at(1) == "H 0026:00000001 +1");
  CHECK(r.log.lines.at(2) == "00000001 gchuddirector.addbigsprite 1 -> 0010:00000000");
  CHECK(r.log.lines.at(6) == "H 002e:00000003 +1");
  CHECK(r.log.lines.at(7) == "00000004 gchuddirector.addtextobject 1 -> 0010:00000001");
  CHECK(r.log.lines.at(8) == "H 0026:00000001 +1");
  CHECK(r.log.lines.at(9) == "00000005 gchuddirector.addbigsprite 1 -> 0010:00000002");
  CHECK(r.res.result == make_int(2));
  HudDirector& hud = r.ctx().subsystem<HudDirector>();
  CHECK(hud.next_id == 3);
  CHECK(hud.elements.at(0) == r.ctx().object(1));
  CHECK(hud.elements.at(2) == r.ctx().object(1));
  CHECK(r.ctx().object(1)->engine_refs() == 2);
  CHECK(r.ctx().check_engine_refs().empty());
  // a nothing argument never reaches the body (0x82's ConvertVariant)
  Asm b;
  b.fn("setup", 0).nothing().call("gchuddirector", "addbigsprite", 1).ret();
  Run r2(b);
  CHECK_FALSE(r2.res.ok);
  CHECK(r2.ctx().subsystem<HudDirector>().next_id == 0);
}

TEST_CASE("osi objects gchuddirector.addbigsprite: the HUD keeps a sprite alive after the script's last variant") {
  Asm a;
  a.fn("setup", 0).f(0).f(0).f(1).f(1).s("s").call("gcbigsprite", "loadhudsprite", 5);
  a.call("gchuddirector", "addbigsprite", 1).pop().nothing().ret();
  Run r(a);
  REQUIRE_MESSAGE(r.res.ok, r.err());
  CHECK_FALSE(r.trace_has("F g 00000001"));
  CHECK(r.host->heap().refs(1) == 1);
  CHECK(r.ctx().object(1) != nullptr);
}
