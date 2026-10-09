// M2a-2 T3: unit tests of the native framework (docs/osi/natives.md §1.2, §2 = D13, §3 = D14, §5.4 = D16) and of
// ScriptHost. Expected values are taken from natives.md (the §3.4 worked example byte for byte), the corpus notes of
// tests/osi/conformance/n/ (hand-derived from the asm by Task 2b) and integer arithmetic stated next to them.
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "doctest.h"
#include "lomn/osi/heap.h"
#include "lomn/osi/host.h"
#include "lomn/osi/natives.h"
#include "lomn/osi/registry.h"
#include "lomn/osi/trace.h"
#include "lomn/osi/vm.h"
#include "osi_test_asm.h"

using namespace lomn;
using namespace lomn::osi;
using lomn::test::Asm;

namespace {

NativeEntry nentry(const std::string& cls, const std::string& name, uint8_t mn, uint8_t mx, std::vector<uint16_t> types,
                   NativeFn fn, void* user) {
  NativeEntry e;
  e.cls = cls;
  e.name = name;
  e.key = NativeRegistry::fold(cls + "." + name);
  e.min = mn;
  e.max = mx;
  e.types.fill(kNothing);
  for (size_t i = 0; i < types.size(); ++i) e.types[i] = types[i];
  e.fn = fn;
  e.user = user;
  e.tier = Tier::Implemented;
  return e;
}

// A VM with a host context: trace and natives.log kept in memory.
struct Rig {
  sage::OsiFile file;
  NativeRegistry reg;
  StringTrace tr, log;
  Heap heap{&tr};
  std::unique_ptr<Vm> vm;
  std::unique_ptr<HostContext> ctx;
  void load(Asm& a) {
    std::string err;
    REQUIRE_MESSAGE(file.parse(a.build(), &err), err);
    reg.set_return_log(&log);
    vm = std::make_unique<Vm>(file, reg, heap, &tr);
    ctx = std::make_unique<HostContext>(*vm, heap, nullptr, HostFlags{}, &log);
  }
  std::vector<std::string> starting(const std::string& p) const {
    std::vector<std::string> out;
    for (const auto& x : tr.lines)
      if (x.rfind(p, 0) == 0) out.push_back(x);
    return out;
  }
  size_t index_of(const std::string& line) const {
    for (size_t i = 0; i < tr.lines.size(); ++i)
      if (tr.lines[i] == line) return i;
    return std::string::npos;
  }
};

// A host object that reports its deletion into the trace (to pin "F g, then delete").
struct Probe : GameObject {
  Probe(uint16_t t, int32_t k, TraceSink* s) : GameObject(t, k), sink(s) {}
  ~Probe() override {
    if (sink) sink->line("~probe");
  }
  TraceSink* sink;
};

}  // namespace

// ============================== step 1: the heap's engine references ==============================

TEST_CASE("osi natives T3-1: Heap engine references (D13: total = engine_refs + 1; add_ref / release; free hook)") {
  StringTrace tr;
  Heap heap(&tr);
  std::vector<uint32_t> freed;
  heap.set_free_hook([&](uint32_t h) { freed.push_back(h); });
  // engine_refs 1 (natives.md §2.4: an engine-held object such as gcgame.getkeymapper's, G 1): total 2
  const uint32_t h = heap.new_game(0x3e, 1);
  CHECK(h == 1);
  CHECK(heap.refs(h) == 2);
  CHECK(heap.engine_refs(h) == 1);
  CHECK(tr.lines.back() == "A g 00000001 3e");
  heap.dispose(Variant{0x3e, h});  // the script drops its variant: 1 left, the engine's
  CHECK(heap.live(kKindGame, h));
  CHECK(heap.refs(h) == 1);
  heap.add_ref(h);  // e.g. GcBAGroup::SetCharacter +1 (0x54b344)
  CHECK(heap.refs(h) == 2);
  CHECK(heap.engine_refs(h) == 2);
  heap.release(h);
  CHECK(heap.refs(h) == 1);
  CHECK(heap.engine_refs(h) == 1);
  CHECK(freed.empty());
  heap.release(h);  // the engine's last reference: total 0 -> F g, then the hook
  CHECK_FALSE(heap.live(kKindGame, h));
  CHECK(tr.lines.back() == "F g 00000001");
  REQUIRE(freed.size() == 1);
  CHECK(freed[0] == 1);
  CHECK(heap.stats().live[kKindGame] == 0);
  // engine_refs 0 is today's new_game(type) (m4): total 1, freed at the variant's dispose
  const uint32_t z = heap.new_game(0x27);
  CHECK(heap.refs(z) == 1);
  CHECK(heap.engine_refs(z) == 0);
  bool threw = false;
  try {
    heap.release(z);  // the engine holds none
  } catch (const VmError& e) {
    threw = e.message == "Undefined: release of an engine reference the engine does not hold";
  }
  CHECK(threw);
  CHECK(heap.refs(z) == 1);
  heap.dispose(Variant{0x27, z});
  CHECK(tr.lines.back() == "F g 00000002");
  CHECK(freed.size() == 2);
  // Duplicate counts as a variant reference only
  const uint32_t d = heap.new_game(0x27, 1);
  heap.duplicate(Variant{0x27, d});
  CHECK(heap.refs(d) == 3);
  CHECK(heap.engine_refs(d) == 1);
}

// ============================== step 2: create_variant ==============================

TEST_CASE("osi natives T3-2: create_variant (CreateVariant 0x445550: null -> nothing; +1 at 0x445580; A g at first use; one handle per object)") {
  Rig r;
  Asm a;
  a.fn("main", 0).nothing().ret();
  r.load(a);
  HostContext& ctx = *r.ctx;
  const size_t lines0 = r.tr.lines.size();
  CHECK(ctx.create_variant(nullptr) == nothing());  // the overload's null branch {0x0f, 0} (0x445563)
  CHECK(r.tr.lines.size() == lines0);               // no A g
  GameObject* toa = ctx.make<GameObject>(0x37, 1);  // the model-director map holds it (natives.md §2.4, G 1)
  CHECK(toa->handle() == 0);                        // not minted at construction
  CHECK(r.heap.stats().allocs[kKindGame] == 0);
  const Variant v1 = ctx.create_variant(toa);
  CHECK(v1 == Variant{0x37, 1});
  CHECK(toa->handle() == 1);
  CHECK(r.tr.lines.back() == "A g 00000001 37");
  CHECK(r.heap.refs(1) == 2);  // engine 1 + this variant
  CHECK(ctx.object(1) == toa);
  // the same live object again, as GcCharacter (natives.md §2.3): the same handle, one more reference, no A g
  const size_t lines1 = r.tr.lines.size();
  const Variant v2 = ctx.create_variant(toa, 0x22);
  CHECK(v2 == Variant{0x22, 1});
  CHECK(r.tr.lines.size() == lines1);
  CHECK(r.heap.refs(1) == 3);
  CHECK(r.heap.engine_refs(1) == 1);
  CHECK(r.heap.stats().allocs[kKindGame] == 1);
  // a second object gets the next handle
  GameObject* t2 = ctx.make<GameObject>(0x27, 0);
  CHECK(ctx.create_variant(t2) == Variant{0x27, 2});
  CHECK(r.heap.refs(2) == 1);
}

// ============================== step 3: release to 0 ==============================

TEST_CASE("osi natives T3-3: the last reference frees the handle (F g) and then deletes the host object (Dispose 0x60a94c / Free 0x60a957)") {
  Rig r;
  Asm a;
  a.fn("main", 0).nothing().ret();
  r.load(a);
  HostContext& ctx = *r.ctx;
  // (a) the engine releases last: H -1 in natives.log, F g, then ~probe
  Probe* p = ctx.make<Probe>(0x26, 1, &r.tr);
  const Variant v = ctx.create_variant(p);
  CHECK(v == Variant{0x26, 1});
  r.heap.dispose(v);  // the script's variant gone: the engine's 1 left
  CHECK(r.index_of("~probe") == std::string::npos);
  ctx.release(p);
  const size_t f = r.index_of("F g 00000001"), d = r.index_of("~probe");
  REQUIRE(f != std::string::npos);
  REQUIRE(d != std::string::npos);
  CHECK(f < d);
  CHECK(ctx.object(1) == nullptr);
  REQUIRE(!r.log.lines.empty());
  CHECK(r.log.lines.back() == "H 0026:00000001 -1");
  // (b) the script drops the last reference: F g, then ~probe; the engine had none
  Probe* q = ctx.make<Probe>(0x27, 0, &r.tr);
  r.tr.lines.clear();
  r.heap.dispose(ctx.create_variant(q));
  REQUIRE(r.tr.lines.size() == 3);
  CHECK(r.tr.lines[0] == "A g 00000002 27");
  CHECK(r.tr.lines[1] == "F g 00000002");
  CHECK(r.tr.lines[2] == "~probe");
  // (c) an object never minted: the engine's last release deletes it, with no F g and no H (not script-visible)
  Probe* u = ctx.make<Probe>(0x27, 1, &r.tr);
  r.tr.lines.clear();
  const size_t log_lines = r.log.lines.size();
  ctx.add_ref(u);
  ctx.release(u);
  CHECK(r.tr.lines.empty());
  ctx.release(u);
  REQUIRE(r.tr.lines.size() == 1);
  CHECK(r.tr.lines[0] == "~probe");
  CHECK(r.log.lines.size() == log_lines);
}

// ============================== step 4: natives.log v2, the worked example ==============================

namespace {
struct Example {
  HostContext* ctx = nullptr;
  GameObject* km = nullptr;
  GameObject* toa = nullptr;
};
Example g_ex;
Variant ex_keymapper(NativeCall&, void*) {
  if (!g_ex.km) g_ex.km = g_ex.ctx->make<GameObject>(0x3e, 1);  // the global ScAutoRefPtr (0x439583): G 1
  return g_ex.ctx->create_variant(g_ex.km);
}
Variant ex_timer(NativeCall&, void*) { return g_ex.ctx->create_variant(g_ex.ctx->make<GameObject>(0x27, 0)); }
Variant ex_addtoa(NativeCall&, void*) {
  g_ex.toa = g_ex.ctx->make<GameObject>(0x37, 1);  // the model-director map
  std::vector<Variant> args = {make_int(0x32), make_int(250)};
  args.resize(10, nothing());
  g_ex.ctx->engine_call("OnEvent", args);
  return make_int(1);
}
Variant ex_nothing(NativeCall&, void*) { return nothing(); }
Variant ex_gettoa(NativeCall&, void*) { return g_ex.ctx->create_variant(g_ex.toa); }
Variant ex_show(NativeCall&, void*) {
  g_ex.ctx->add_ref(g_ex.toa);  // GcBAGroup::SetCharacter (0x54b344)
  return nothing();
}
Variant ex_getcharacter(NativeCall&, void*) { return g_ex.ctx->create_variant(g_ex.toa, 0x22); }
Variant ex_loadhudsprite(NativeCall&, void*) { return g_ex.ctx->create_variant(g_ex.ctx->make<GameObject>(0x26, 0)); }
Variant ex_addbigsprite(NativeCall& c, void*) {
  g_ex.ctx->add_ref(g_ex.ctx->object_arg<GameObject>(c, 0));
  return nothing();
}
Variant ex_getmask(NativeCall&, void*) { return make_int(0x3f3f3f3f); }

const char* const kExampleNatives = R"([
 {"cls": "GcGame", "name": "GetKeyMapper", "min": 0, "max": 0, "types": [], "ret": "game", "ret_class": "GcKeyMapper", "winner": true},
 {"cls": "GcTimer", "name": "Create", "min": 3, "max": 3, "types": ["int", "int", "float"], "ret": "game", "ret_class": "GcTimer", "winner": true},
 {"cls": "GcModelDirector", "name": "AddToa", "min": 1, "max": 1, "types": ["string"], "ret": "int", "winner": true},
 {"cls": "ScTextObject", "name": "SetString", "min": 2, "max": 2, "types": ["int", "int"], "ret": "none", "winner": true},
 {"cls": "GcModelDirector", "name": "GetToa", "min": 0, "max": 0, "types": [], "ret": "game", "ret_class": "GcToa", "winner": true},
 {"cls": "GcModelDirector", "name": "Show", "min": 2, "max": 2, "types": ["int", "int"], "ret": "none", "winner": true},
 {"cls": "GcModelDirector", "name": "GetCharacter", "min": 1, "max": 2, "types": ["int", "int"], "ret": "game", "ret_class": "GcCharacter", "winner": true},
 {"cls": "GcBigSprite", "name": "LoadHudSprite", "min": 5, "max": 7, "types": ["int", "int", "int", "int", "int"], "ret": "game", "ret_class": "GcBigSprite", "winner": true},
 {"cls": "GcHudDirector", "name": "AddBigSprite", "min": 1, "max": 1, "types": ["type:GcBigSprite"], "ret": "none", "winner": true},
 {"cls": "GcSaver", "name": "GetMask", "min": 0, "max": 0, "types": [], "ret": "int", "winner": true}
])";
const char* const kExampleTypes = R"({"types": [
 {"name": "type:GcCharacter", "id": 34}, {"name": "type:GcBigSprite", "id": 38}, {"name": "type:GcTimer", "id": 39},
 {"name": "type:GcToa", "id": 55}, {"name": "type:GcKeyMapper", "id": 62}]})";

}  // namespace

TEST_CASE("osi natives T3-4: natives.log v2 records G, C N and K byte for byte as natives.md §3.4 (no C -1, no host H)") {
  Asm a;
  a.fn("setup", 0);
  a.call("gcgame", "getkeymapper", 0).pop();
  a.i8(0).i8(0).f(5.0f).call("gctimer", "create", 3).pop();
  a.s("onua").call("gcmodeldirector", "addtoa", 1).pop();
  a.call("gcmodeldirector", "gettoa", 0).pop();
  a.i32(0x636e7431).i8(0).call("gcmodeldirector", "show", 2).pop();
  a.i32(0x636e7431).call("gcmodeldirector", "getcharacter", 1).pop();
  a.i8(0).i8(0).i8(0).i8(0).i8(0).call("gcbigsprite", "loadhudsprite", 5);
  a.op(0x35).call("gchuddirector", "addbigsprite", 1).pop().pop();
  a.nothing().ret();
  a.fn("onevent", 10).i8(1).i8(2).call("sctextobject", "setstring", 2).pop().nothing().ret();
  a.fn("process", 3).call("gcsaver", "getmask", 0).pop().i8(1).ret();
  sage::OsiFile file;
  std::string err;
  REQUIRE_MESSAGE(file.parse(a.build(), &err), err);
  StringTrace tr, log;
  ScriptHost::Options opt;
  ScriptHost host(file, kExampleNatives, "", kExampleTypes, "", nullptr, &tr, &log, opt);
  g_ex = Example{};
  g_ex.ctx = &host.context();
  NativeRegistry& reg = host.registry();
  reg.bind("gcgame.getkeymapper", ex_keymapper, nullptr, Tier::Implemented);
  reg.bind("gctimer.create", ex_timer, nullptr, Tier::Implemented);
  reg.bind("gcmodeldirector.addtoa", ex_addtoa, nullptr, Tier::Implemented);
  reg.bind("sctextobject.setstring", ex_nothing, nullptr, Tier::Implemented);
  reg.bind("gcmodeldirector.gettoa", ex_gettoa, nullptr, Tier::Implemented);
  reg.bind("gcmodeldirector.show", ex_show, nullptr, Tier::Implemented);
  reg.bind("gcmodeldirector.getcharacter", ex_getcharacter, nullptr, Tier::Implemented);
  reg.bind("gcbigsprite.loadhudsprite", ex_loadhudsprite, nullptr, Tier::Implemented);
  reg.bind("gchuddirector.addbigsprite", ex_addbigsprite, nullptr, Tier::Implemented);
  reg.bind("gcsaver.getmask", ex_getmask, nullptr, Tier::Implemented);
  CallResult s = host.setup();
  REQUIRE_MESSAGE(s.ok, (host.vm().last_error() ? host.vm().last_error()->message : std::string("?")));
  CHECK(host.frame());
  const std::vector<std::string> want = {
      "00000000 gcgame.getkeymapper 0 -> 003e:00000001 G 1",
      "00000001 gctimer.create 3 -> 0027:00000002 G 0",
      "C 00000002 OnEvent 0010:00000032 0010:000000fa 000f:00000000 000f:00000000 000f:00000000 000f:00000000 "
      "000f:00000000 000f:00000000 000f:00000000 000f:00000000",
      "00000003 sctextobject.setstring 2 -> 000f:00000000",
      "00000002 gcmodeldirector.addtoa 1 -> 0010:00000001",
      "00000004 gcmodeldirector.gettoa 0 -> 0037:00000003 G 1",
      "H 0037:00000003 +1",
      "00000005 gcmodeldirector.show 2 -> 000f:00000000",
      "00000006 gcmodeldirector.getcharacter 1 -> 0022:00000003 G 2",
      "00000007 gcbigsprite.loadhudsprite 5 -> 0026:00000004 G 0",
      "H 0026:00000004 +1",
      "00000008 gchuddirector.addbigsprite 1 -> 000f:00000000",
      "K      0",
      "00000009 gcsaver.getmask 0 -> 0010:3f3f3f3f",
  };
  CHECK(log.text() == [&] {
    std::string t;
    for (const auto& l : want) t += l + "\n";
    return t;
  }());
  // the sprite survives the script's last variant: the engine holds it (natives.md §3.4, lines 10-12)
  CHECK(host.heap().live(kKindGame, 4));
  CHECK(host.heap().refs(4) == 1);
  // ScriptHost's coverage counts (HostStats)
  const HostStats& st = host.stats();
  CHECK(st.natives == 10);
  CHECK(st.max_nesting == 2);  // Setup + the nested OnEvent
  REQUIRE(st.hits.count("gcsaver.getmask"));
  CHECK(st.hits.at("gcsaver.getmask").first_frame == 0);
  CHECK(st.hits.at("gcsaver.getmask").first_seq == 9);
  CHECK(st.hits.at("gcmodeldirector.addtoa").first_frame == -1);
  CHECK(st.hits.at("gcmodeldirector.addtoa").tier == Tier::Implemented);
  CHECK(st.frames == 1);
}

TEST_CASE("osi natives T3-4b: a stub game result prints G 0; a by-ref game write prints its G; K is written for every frame") {
  Asm a;
  a.fn("setup", 0).call("gctimer", "create", 0).pop().nothing().ret();
  a.fn("process", 3).i8(1).ret();
  // with no bodies bound every native is its typed stub (D11)
  const char* nat = R"([{"cls": "GcTimer", "name": "Create", "min": 0, "max": 3, "types": [], "ret": "game", "ret_class": "GcTimer", "winner": true}])";
  const char* typ = R"({"types": [{"name": "type:GcTimer", "id": 39}]})";
  sage::OsiFile file;
  std::string err;
  REQUIRE_MESSAGE(file.parse(a.build(), &err), err);
  StringTrace tr, log;
  ScriptHost host(file, nat, "", typ, "", nullptr, &tr, &log, ScriptHost::Options{});
  REQUIRE(host.setup().ok);
  CHECK(host.frame());
  CHECK(host.frame());
  REQUIRE(log.lines.size() == 3);
  CHECK(log.lines[0] == "00000000 gctimer.create 0 -> 0027:00000001 G 0");
  CHECK(log.lines[1] == "K      0");
  CHECK(log.lines[2] == "K      1");
}

// ============================== step 5: the engine_call nesting high-water ==============================

namespace {
HostContext* g_ctx = nullptr;
int g_reenter_left = 0;
Variant nat_nest(NativeCall&, void*) {
  if (g_reenter_left-- > 0) g_ctx->engine_call("main", {});
  return make_int(1);
}
}  // namespace

TEST_CASE("osi natives T3-5: engine_call writes C <the calling native's seq> and the VM records the nesting high-water (D15)") {
  Rig r;
  Asm a;
  a.fn("main", 0).call("t", "nest", 0).ret();
  r.load(a);
  r.reg.add(nentry("t", "nest", 0, 0, {}, nat_nest, nullptr));
  g_ctx = r.ctx.get();
  g_reenter_left = 2;  // main -> t.nest -> main -> t.nest -> main -> t.nest (stops): 3 calls in flight at most
  CallResult c = r.vm->call("main", {});
  REQUIRE(c.ok);
  CHECK(r.vm->max_nesting() == 3);
  const std::string pad = " 000f:00000000 000f:00000000 000f:00000000 000f:00000000 000f:00000000 000f:00000000 "
                          "000f:00000000 000f:00000000 000f:00000000 000f:00000000";
  const std::vector<std::string> want = {"C 00000000 main" + pad, "C 00000001 main" + pad,
                                         "00000002 t.nest 0 -> 0010:00000001", "00000001 t.nest 0 -> 0010:00000001",
                                         "00000000 t.nest 0 -> 0010:00000001"};
  CHECK(r.log.lines == want);
  // a second, flatter call does not lower the high-water mark
  g_reenter_left = 0;
  REQUIRE(r.vm->call("main", {}).ok);
  CHECK(r.vm->max_nesting() == 3);
  // the cap (D15 keeps 64 while 64 >= 4 x the measured maximum): unchanged
  CHECK(Vm::kMaxEngineNesting == 64);
}

// ============================== step 6: D16, n-reenter-nested-error-swallowed ==============================

namespace {
Variant nat_swallow_mid(NativeCall&, void*) {
  g_ctx->engine_call_swallowing("mid", {});
  return make_int(5);
}
Variant nat_inner_deep(NativeCall&, void*) {
  g_ctx->engine_call("deep", {});
  return make_int(6);
}
}  // namespace

TEST_CASE("osi natives T3-6: D16 n-reenter-nested-error-swallowed (the swallowed E line is the 0x82 re-raise; dispatch goes on in deep)") {
  Rig r;
  Asm a;  // the corpus program n-reenter-nested-error-swallowed.asm, entry order kept
  a.fn("main2", 0);
  a.fns.back().label = "m2";
  a.fn("mid", 0);
  a.fns.back().label = "mid";
  a.fn("deep", 0);
  a.fns.back().label = "deep";
  a.fn("main", 0).i8(11).label("c").call("t", "sw", 0).pop().i8(1).ret();
  a.label("mid").i8(22).label("ci").call("t", "inner", 0).label("after_ci").pop().i8(2).ret();
  a.label("deep").i8(33).label("bad").op(0x02).ret();
  a.label("m2").i8(9).ret();
  r.load(a);
  r.reg.add(nentry("t", "sw", 0, 0, {}, nat_swallow_mid, nullptr));
  r.reg.add(nentry("t", "inner", 0, 0, {}, nat_inner_deep, nullptr));
  g_ctx = r.ctx.get();
  CallResult c = r.vm->call("main", {});
  CHECK(c.ok);
  CHECK(r.vm->last_error() == nullptr);
  CHECK(c.result == make_int(5));
  VmState st = r.vm->snapshot();
  CHECK(st.depth == 4);
  CHECK(st.top == make_int(22));
  CHECK(st.context_depth == 2);
  CHECK(st.fpu_pc == 64);
  CHECK(st.pc == a.at("after_ci"));
  REQUIRE(r.vm->swallowed().size() == 1);
  const Vm::Swallowed& s = r.vm->swallowed()[0];
  CHECK(s.error.message == "Script is calling t.inner");
  CHECK(s.error.pc == a.at("ci"));
  CHECK(s.error.op == 0x82);
  CHECK(s.error.depth == 6);
  CHECK(s.has_top);
  CHECK(s.top == make_int(33));
  // exactly one E line, the swallowed one, then P (24 -> 64)
  const auto e = r.starting("E ");
  REQUIRE(e.size() == 1);
  char b[64];
  std::snprintf(b, sizeof b, "E %08x 82    6 \"Script is calling t.inner\"", a.at("ci"));
  CHECK(e[0] == b);
  const size_t ei = r.index_of(b);
  REQUIRE(ei + 1 < r.tr.lines.size());
  std::snprintf(b, sizeof b, "P %08x 64", a.at("ci"));
  CHECK(r.tr.lines[ei + 1] == b);
  // natives.log: t.sw is seq 0, t.inner seq 1 (entry order); t.inner raised, so it has no line; X 0 at the swallow
  const std::string pad = " 000f:00000000 000f:00000000 000f:00000000 000f:00000000 000f:00000000 000f:00000000 "
                          "000f:00000000 000f:00000000 000f:00000000 000f:00000000";
  const std::vector<std::string> want = {"C 00000000 mid" + pad, "C 00000001 deep" + pad, "X 00000000",
                                         "00000000 t.sw 0 -> 0010:00000005"};
  CHECK(r.log.lines == want);
  // then main2 above the leftovers (rule 19: no reset after a call that did not raise)
  CallResult m2 = r.vm->call("main2", {});
  CHECK(m2.ok);
  CHECK(m2.result == make_int(9));
  st = r.vm->snapshot();
  CHECK(st.depth == 4);
  CHECK(st.context_depth == 2);
  CHECK(st.pc == a.at("after_ci"));
  CHECK(r.vm->swallowed().empty());  // cleared at the next outermost call
}

// ============================== step 7: parked N2 ==============================

TEST_CASE("osi natives T3-7: FindMemberID with an empty symbol table (0x609c57 xor esi, 0x609cb0): a NULL entry, then undefined") {
  Rig r;
  Asm a;  // no class, no symbol
  a.fn("main", 0).nothing().ret();
  r.load(a);
  REQUIRE(r.file.symbols().empty());
  CHECK(r.vm->member_id("hp") == 0xffff);  // the miss caches {NULL, 0xffff}
  bool threw = false;
  try {
    r.vm->member_id("hp");  // strcmp against the NULL symbol: a crash in the original
  } catch (const VmError& e) {
    threw = e.message == "Undefined behaviour: FindMemberID cache entry without a symbol";
  }
  CHECK(threw);
  // member_id shares the 0x18/0x19/0x1a cache (N3): a symbol table with one entry, a miss poisons it for 0x18 too
  Rig u;
  Asm b;
  b.cls("k", {"hp"});
  b.fn("get", 0).op(0x13).u16(0).s("hp").op(0x18).ret();
  u.load(b);
  CHECK(u.vm->member_id("nope") == 0xffff);  // caches {"hp", 0xffff}
  CHECK(u.vm->member_id("hp") == 0xffff);    // poisoned
  CHECK_FALSE(u.vm->call("get", {}).ok);
  CHECK(u.vm->last_error()->message == "Reference to non-existant member property \"hp\"");
}

// ============================== step 8: the wrap guard (parked N4) ==============================

TEST_CASE("osi natives T3-8: next_[k] == UINT32_MAX -> Undefined: handle space exhausted, for every kind") {
  StringTrace tr;
  Heap heap(&tr);
  for (int k = 0; k < 4; ++k) heap.set_next_for_test(static_cast<HeapKind>(k), 0xfffffffeu);
  CHECK(heap.new_object(0, 0) == 0xfffffffeu);
  CHECK(heap.new_array() == 0xfffffffeu);
  CHECK(heap.new_string("x") == 0xfffffffeu);
  CHECK(heap.new_game(0x27, 0) == 0xfffffffeu);
  const size_t lines = tr.lines.size();
  int threw = 0;
  auto expect_exhausted = [&](auto f) {
    try {
      f();
    } catch (const VmError& e) {
      if (e.message == "Undefined: handle space exhausted") ++threw;
    }
  };
  expect_exhausted([&] { heap.new_object(0, 0); });
  expect_exhausted([&] { heap.new_array(3); });
  expect_exhausted([&] { heap.new_string("y"); });
  expect_exhausted([&] { heap.new_game(0x27, 1); });
  CHECK(threw == 4);
  CHECK(tr.lines.size() == lines);  // no A record for a refused allocation
  CHECK(heap.stats().allocs[kKindGame] == 1);
  CHECK(heap.stored() == 4);
}

// ============================== the registry's tier column ==============================

TEST_CASE("osi natives T3: the registry reads each row's merged tier (natives.md §7.1-7.2); no key = stub") {
  const char* nat = R"([
 {"cls": "a", "name": "one", "min": 0, "max": 0, "types": [], "ret": "int", "winner": true, "tier": "tier1"},
 {"cls": "a", "name": "two", "min": 0, "max": 0, "types": [], "ret": "int", "winner": true, "tier": "tier2"},
 {"cls": "a", "name": "three", "min": 0, "max": 0, "types": [], "ret": "int", "winner": true, "tier": "tier3"},
 {"cls": "a", "name": "plain", "min": 0, "max": 0, "types": [], "ret": "int", "winner": true}])";
  const char* mods = R"([{"cls": "m", "name": "x", "min": 0, "max": 0, "types": [], "ret": "none", "winner": true, "tier": "mod"}])";
  TypeTable types = TypeTable::load(R"({"types": []})");
  NativeRegistry reg = NativeRegistry::load(nat, mods, types);
  CHECK(reg.find("a", "one")->tier == Tier::Implemented);
  CHECK(reg.find("a", "two")->tier == Tier::Tier2);
  CHECK(reg.find("a", "three")->tier == Tier::Tier3);
  CHECK(reg.find("a", "plain")->tier == Tier::Stub);
  CHECK(reg.find("m", "x")->tier == Tier::Mod);
  CHECK(std::string(tier_name(Tier::Tier2)) == "tier2");
  bool threw = false;
  try {
    NativeRegistry::load(R"([{"cls": "a", "name": "b", "min": 0, "max": 0, "types": [], "ret": "int", "winner": true, "tier": "tier9"}])",
                         "", types);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  CHECK(threw);
}

// ============================== bind_natives ==============================

namespace {
Variant grp_getmask(NativeCall&, void*) { return make_int(0x3f3f3f3f); }
void bind_test_group(NativeRegistry& reg, HostContext& ctx) { reg.bind("gcsaver.getmask", grp_getmask, &ctx, Tier::Stub); }
const bool g_registered = (register_native_group("t3-test", bind_test_group), true);
}  // namespace

TEST_CASE("osi natives T3: register_native_group / bind_natives run every group and mark the rows they bind tier1") {
  CHECK(g_registered);
  CHECK(native_group_count() >= 1);
  Asm a;
  a.fn("setup", 0).call("gcsaver", "getmask", 0).ret();
  sage::OsiFile file;
  std::string err;
  REQUIRE_MESSAGE(file.parse(a.build(), &err), err);
  const char* nat = R"([{"cls": "GcSaver", "name": "GetMask", "min": 0, "max": 0, "types": [], "ret": "int", "winner": true}])";
  StringTrace tr;
  ScriptHost::Options stub_opt;
  ScriptHost stubs(file, nat, "", R"({"types": []})", "", nullptr, &tr, nullptr, stub_opt);
  CHECK(stubs.setup().ok);
  CHECK(stubs.registry().find("gcsaver", "getmask")->tier == Tier::Stub);
  ScriptHost::Options real;
  real.real_natives = true;
  ScriptHost host(file, nat, "", R"({"types": []})", "", nullptr, &tr, nullptr, real);
  const NativeEntry* e = host.registry().find("gcsaver", "getmask");
  REQUIRE(e);
  CHECK(e->fn == grp_getmask);
  CHECK(e->user == &host.context());
  CHECK(e->tier == Tier::Implemented);
  // an override row must agree with the merged tier of its row
  bool threw = false;
  try {
    ScriptHost bad(file, nat, "", R"({"types": []})", R"([{"key": "gcsaver.getmask", "tier": "tier2", "cite": "body 0x5d36d0", "note": "x"}])",
                   nullptr, &tr, nullptr, stub_opt);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  CHECK(threw);
}

// ============================== subsystem<T>() and the flags ==============================

namespace {
struct Counter {
  explicit Counter(HostContext& c) : ctx(&c) { ++made; }
  HostContext* ctx;
  int n = 0;
  static int made;
};
int Counter::made = 0;
}  // namespace

TEST_CASE("osi natives T3: subsystem<T>() is one lazily built instance per context; HostFlags default to D1/D3 on") {
  Rig r, s;
  Asm a;
  a.fn("main", 0).nothing().ret();
  r.load(a);
  s.load(a);
  Counter::made = 0;
  r.ctx->subsystem<Counter>().n = 7;
  CHECK(r.ctx->subsystem<Counter>().n == 7);
  CHECK(r.ctx->subsystem<Counter>().ctx == r.ctx.get());
  CHECK(s.ctx->subsystem<Counter>().n == 0);
  CHECK(Counter::made == 2);
  CHECK(r.ctx->flags().deterministic);
  CHECK(r.ctx->flags().skip_video);
  CHECK(r.ctx->vfs() == nullptr);
}

// ============================== D15: the cap counts engine calls in flight, not context_depth ==============================

namespace {
int g_cap_left = 0;
uint32_t g_cap_max_context = 0;
std::string g_cap_inner_err;
Variant nat_cap_sw(NativeCall&, void*) {
  g_ctx->engine_call_swallowing("bad", {});
  return make_int(5);
}
Variant nat_cap_deep(NativeCall& c, void*) {
  if (c.vm.snapshot().context_depth > g_cap_max_context) g_cap_max_context = c.vm.snapshot().context_depth;
  if (g_cap_left-- > 0) {
    try {
      g_ctx->engine_call("deep", {});
    } catch (const VmError& e) {
      if (g_cap_inner_err.empty()) g_cap_inner_err = e.message;
      throw;
    }
  }
  return make_int(1);
}
}  // namespace

TEST_CASE("osi natives T3 D15 (controller ruling): after a swallow the cap of 64 counts calls in flight, not context_depth") {
  // main -> t.sw swallows bad's DebugOn: context_depth stays 2 while 1 call is in flight; dispatch goes on in bad, whose
  // t.deep re-enters `deep` g_cap_left times. Calls in flight = 1 + the nested deep calls; context_depth is one higher.
  auto run = [](int left, Rig& r, Asm& a) {
    a.fn("main", 0).call("t", "sw", 0).ret();
    a.fn("bad", 0).op(0x02).call("t", "deep", 0).ret();
    a.fn("deep", 0).call("t", "deep", 0).ret();
    r.load(a);
    r.reg.add(nentry("t", "sw", 0, 0, {}, nat_cap_sw, nullptr));
    r.reg.add(nentry("t", "deep", 0, 0, {}, nat_cap_deep, nullptr));
    g_ctx = r.ctx.get();
    g_cap_left = left;
    g_cap_max_context = 0;
    g_cap_inner_err.clear();
    return r.vm->call("main", {});
  };
  {
    Rig r;
    Asm a;
    CallResult c = run(63, r, a);  // in flight: main + 63 deep = 64, the cap; context_depth reaches 65
    CHECK_MESSAGE(c.ok, (r.vm->last_error() ? r.vm->last_error()->message : std::string()));
    CHECK(g_cap_inner_err.empty());
    CHECK(r.vm->swallowed().size() == 1);
    CHECK(r.vm->max_nesting() == 64);
    CHECK(g_cap_max_context == 65);  // a context_depth cap would have refused this run
  }
  Rig r;
  Asm a;
  CallResult c = run(64, r, a);  // the 65th call in flight is refused
  CHECK_FALSE(c.ok);
  CHECK(g_cap_inner_err == "Undefined: engine-call nesting limit (64)");
  CHECK(r.vm->last_error()->message == "Script is calling t.deep");
  CHECK(r.vm->max_nesting() == 64);
}
