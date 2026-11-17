// M2a-3 T6a: the HUD setters, show/hide and the Z hud/sprite/text/font lines (hud.md §4, §9.2) on the real native tables
// (data/osi). The expected values come from hud.md; the colour constant is computed here from its bits.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "doctest.h"
#include "lomn/fs.h"
#include "lomn/osi/host.h"
#include "lomn/osi/trace.h"
#include "osi_test_asm.h"

using namespace lomn;
using namespace lomn::osi;
using lomn::test::Asm;

namespace {
std::string slurp(const char* name) {
  std::ifstream f(std::string(LOMN_OSI_DATA_DIR) + "/" + name, std::ios::binary);
  std::ostringstream s;
  s << f.rdbuf();
  return s.str();
}
struct Lines : TraceSink {
  std::vector<std::string> lines;
  void line(std::string_view s) override { lines.emplace_back(s); }
};
std::string bits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  char b[12];
  std::snprintf(b, sizeof b, "%08x", u);
  return b;
}
std::string colour(double x) {  // f32(x * f32 0x3b808081): the exact product rounded once
  const uint32_t kb = 0x3b808081u;
  float k;
  std::memcpy(&k, &kb, 4);
  return bits(static_cast<float>(x * static_cast<double>(k)));
}
Asm& gset(Asm& a, const char* g) { return a.op(0x22).u16(a.glob(g)); }
Asm& gget(Asm& a, const char* g) { return a.op(0x21).u16(a.glob(g)); }

// Setup: f, g = two fonts; t = a text object (400, 320, 0, f, "Play"); s = a sprite (1, 2, 3, 4, "Spr", 1, 0);
// i, j = their HUD ids. `body` writes frame 0's process.
struct Rig {
  sage::OsiFile file;
  fs::MemoryFileSystem vfs;
  Lines tr, log;
  std::unique_ptr<ScriptHost> host;
  bool ok = false;
  explicit Rig(void (*body)(Asm&)) {
    Asm a;
    a.fn("setup", 0);
    a.s("Fnt").call("sctilefont", "create", 1);
    gset(a, "f").s("Fnt2").call("sctilefont", "create", 1);
    gset(a, "g").f(400).f(320).f(0);
    gget(a, "f").s("Play").call("sctextobject", "create", 5);
    gset(a, "t").f(1).f(2).f(3).f(4).s("Spr").i8(1).i8(0).call("gcbigsprite", "loadhudsprite", 7);
    gset(a, "s");
    gget(a, "s").call("gchuddirector", "addbigsprite", 1);
    gset(a, "i");
    gget(a, "t").call("gchuddirector", "addtextobject", 1);
    gset(a, "j").i8(1).ret();
    a.fn("process", 3);
    body(a);
    a.i8(1).ret();
    std::string err;
    REQUIRE_MESSAGE(file.parse(a.build(), &err), err);
    ScriptHost::Options opt;
    opt.real_natives = true;
    host = std::make_unique<ScriptHost>(file, slurp("natives.json"), slurp("natives_beta150.json"), slurp("types.json"),
                                        slurp("native_overrides.json"), &vfs, &tr, &log, opt);
    REQUIRE(host->setup().ok);
    ok = host->frame();
  }
  std::vector<std::string> z(const std::string& prefix) const {
    std::vector<std::string> v;
    for (const auto& l : tr.lines)
      if (l.rfind(prefix, 0) == 0) v.push_back(l);
    return v;
  }
  bool has(const std::string& line) const {
    for (const auto& l : tr.lines)
      if (l == line) return true;
    return false;
  }
  std::string error() const {
    const auto& e = host->vm().last_error();
    return e ? e->message : "";
  }
  std::string h(const char* cls, const char* fn, uint32_t handle) {
    char b[24];
    std::snprintf(b, sizeof b, "%04x:%08x", host->registry().find(cls, fn)->ret_type, handle);
    return b;
  }
};
}  // namespace

TEST_CASE("hud: the constructors' fields, `-` for every field no native wrote (hud.md §9.2)") {
  Rig r([](Asm&) {});
  REQUIRE(r.ok);
  const std::string f = r.h("sctilefont", "create", 1), t = r.h("sctextobject", "create", 3),
                    s = r.h("gcbigsprite", "loadhudsprite", 4);
  for (const std::string& l : {"Z font.file " + f + " 3 466e74", "Z font.color " + f + " -", "Z font.transp " + f + " -",
                               "Z text.pos " + t + " " + bits(400) + " " + bits(320) + " " + bits(0),
                               "Z text.font " + t + " " + f, "Z text.string " + t + " 4 506c6179", "Z text.scale " + t + " -",
                               "Z text.just " + t + " -", "Z text.juston " + t + " -",
                               "Z sprite.rect " + s + " " + bits(1) + " " + bits(2) + " " + bits(3) + " " + bits(4),
                               "Z sprite.file " + s + " 3 537072", "Z sprite.b6 " + s + " 01", "Z sprite.alpha " + s + " -",
                               "Z sprite.color " + s + " -", "Z sprite.zdepth " + s + " -",
                               "Z hud.node 00000000 sprite " + s + " -", "Z hud.node 00000001 text " + t + " -"})
    CHECK_MESSAGE(r.has(l), l);
}

TEST_CASE("hud: every setter stores what hud.md §4 says") {
  Rig r([](Asm& a) {
    gget(a, "s").f(-5).call("gcbigsprite", "setzdepth", 2).pop();
    gget(a, "s").i32(0x1ff).call("gcbigsprite", "enablealphachannel", 2).pop();
    gget(a, "s").f(5).f(6).f(7).f(8).call("gcbigsprite", "sethudposition", 5).pop();
    gget(a, "s").i32(0x1ff).i32(128).i32(0).i32(1).call("gcbigsprite", "setcolor", 5).pop();
    gget(a, "t").f(1.5f).f(2.5f).f(3.5f).call("sctextobject", "setposition", 4).pop();
    gget(a, "t").f(0.5f).f(0.25f).call("sctextobject", "setscale", 3).pop();
    gget(a, "t").i32(0x10002).call("sctextobject", "setjustification", 2).pop();
    gget(a, "t").i32(0x101).call("sctextobject", "enablejustification", 2).pop();
    gget(a, "t").s("").call("sctextobject", "setstring", 2).pop();
    gget(a, "f").i32(255).i32(-1).i32(7).i32(300).call("sctilefont", "setcolor", 5).pop();
    gget(a, "f").i8(1).call("sctilefont", "enabletransparency", 2).pop();
  });
  REQUIRE_MESSAGE(r.ok, r.error());
  const std::string f = r.h("sctilefont", "create", 1), t = r.h("sctextobject", "create", 3),
                    s = r.h("gcbigsprite", "loadhudsprite", 4);
  for (const std::string& l :
       {"Z sprite.zdepth " + s + " " + bits(-5), "Z sprite.alpha " + s + " ff",
        "Z sprite.rect " + s + " " + bits(5) + " " + bits(6) + " " + bits(7) + " " + bits(8),
        "Z sprite.color " + s + " " + colour(255) + " " + colour(128) + " " + colour(0) + " " + colour(1),
        "Z text.pos " + t + " " + bits(1.5f) + " " + bits(2.5f) + " " + bits(3.5f),
        "Z text.scale " + t + " " + bits(0.5f) + " " + bits(0.25f), "Z text.just " + t + " 0002", "Z text.juston " + t + " 01",
        "Z text.string " + t + " 0", "Z font.color " + f + " " + colour(255) + " " + colour(-1) + " " + colour(7) + " " + colour(300),
        "Z font.transp " + f + " 01"})
    CHECK_MESSAGE(r.has(l), l);
}

TEST_CASE("hud: show appends once per object, hide erases and keeps the order (0x47b36f, 0x47b3a6, 0x47b4a7)") {
  Rig r([](Asm& a) {
    for (const char* g : {"i", "j", "i"}) gget(a, g).call("gchuddirector", "show", 1).pop();
    gget(a, "i").call("gchuddirector", "hide", 1).pop();
    gget(a, "i").call("gchuddirector", "show", 1).pop();
    gget(a, "j").call("gchuddirector", "hide", 1).pop();
    gget(a, "j").call("gchuddirector", "hide", 1).pop();  // hiding a hidden node: nothing
    gget(a, "j").call("gchuddirector", "show", 1).pop();
  });
  REQUIRE_MESSAGE(r.ok, r.error());
  CHECK(r.has("Z hud.node 00000000 sprite " + r.h("gcbigsprite", "loadhudsprite", 4) + " 0"));
  CHECK(r.has("Z hud.node 00000001 text " + r.h("sctextobject", "create", 3) + " 1"));
}

TEST_CASE("hud: setfont adds the new font, then releases the old one; the same font is nothing (0x611fa9-0x611fce)") {
  Rig same([](Asm& a) {
    gget(a, "t");
    gget(a, "f").call("sctextobject", "setfont", 2).pop();
  });
  REQUIRE_MESSAGE(same.ok, same.error());
  int h = 0;
  for (const auto& l : same.log.lines) h += l.rfind("H ", 0) == 0;
  Rig other([](Asm& a) {
    gget(a, "t");
    gget(a, "g").call("sctextobject", "setfont", 2).pop();
  });
  REQUIRE_MESSAGE(other.ok, other.error());
  std::vector<std::string> hl;
  for (const auto& l : other.log.lines)
    if (l.rfind("H ", 0) == 0) hl.push_back(l);
  REQUIRE(hl.size() == static_cast<size_t>(h) + 2);
  CHECK(hl[hl.size() - 2] == "H " + other.h("sctilefont", "create", 2) + " +1");
  CHECK(hl.back() == "H " + other.h("sctilefont", "create", 1) + " -1");
  CHECK(other.has("Z text.font " + other.h("sctextobject", "create", 3) + " " + other.h("sctilefont", "create", 2)));
}

TEST_CASE("hud: an unknown HUD id, a null object and a font colour past 2^29 stop the run with their messages") {
  Rig unknown([](Asm& a) { a.i32(99).call("gchuddirector", "show", 1).pop(); });
  CHECK_FALSE(unknown.ok);
  Rig null_this([](Asm& a) { a.nothing().f(1).f(1).call("sctextobject", "setscale", 3).pop(); });
  CHECK_FALSE(null_this.ok);
  Rig huge([](Asm& a) { gget(a, "f").i32(1 << 29).i8(0).i8(0).i8(0).call("sctilefont", "setcolor", 5).pop(); });
  CHECK_FALSE(huge.ok);
  bool saw[3] = {};
  for (const Rig* r : {&unknown, &null_this, &huge})
    for (const auto& l : r->tr.lines) {
      saw[0] |= l.find("Script is calling gchuddirector.show") != std::string::npos;
      saw[1] |= l.find("Script is calling sctextobject.setscale") != std::string::npos;
      saw[2] |= l.find("Script is calling sctilefont.setcolor") != std::string::npos;
    }
  CHECK(saw[0]);
  CHECK(saw[1]);
  CHECK(saw[2]);
}
