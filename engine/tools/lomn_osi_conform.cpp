// M2a-1 T1: lomn_osi_conform, the C++ conformance runner (docs/osi/conformance.md §1, §4, §6 amendments 1-14).
//
//   lomn_osi_conform CORPUS_DIR [--osi-dir DIR] [--data-dir data/osi] [--trace-dir DIR]
//   lomn_osi_conform --list-keys     (the compared and ignored expectation key paths, one per line)
//
// Every `<id>.expect.json` under CORPUS_DIR (recursively, sorted by path) is one program. Its image `<id>.osi` and label
// table `<id>.labels` come from --osi-dir (the same relative directory; written at build time by
// tools/osi/conform_assemble.py from `<id>.asm`), or from next to the expectation. Prints one `FAIL <path>: <field>: ...`
// line per differing field and `conformance: N/M pass, P pending`; exit 0 when all pass, 1 on any failure, 77 when there is
// no program (ctest's skip code: an empty corpus is not a pass).
//
// Pending programs: an expectation with a top-level non-empty string `"pending": "<task id>"` (any other value is ignored,
// so the program runs) is not run. The runner prints `PENDING <group>/<id> (<task>)` in the place of its PASS/FAIL line, reads
// no .osi, writes no trace, and counts it only in P (M excludes it). It never fails the run. An all-pending corpus exits 0.
// Unknown keys fail closed: a non-pending program with a key path no runner compares (and the ignored list does not allow)
// FAILS with `FAIL <path>: uncompared key <key path>` before it runs; see compared_keys() / ignored_keys() below.
//
// Test natives are declarative (conformance.md §1, §6.1): types in the registry vocabulary; `returns` (a variant,
// {"game": C}, {"text": t}, {"hex": h}, {"arg": i}); `writes` (by-ref slots overwritten WITHOUT dispose, §6.14d);
// `calls` ({"entry", "args"}, a nested vm.call before the native returns, §6.1); `sets_fpu_pc64`. Inside a native the
// order is: nested calls, then writes, then the return value.
// M2a-2 T3 (conformance.md §1 "Native-framework fields", rules 15 and 17-19), through the native framework
// (lomn/osi/natives.h): `returns.engine_refs` k (a host GameObject held k times, minted by HostContext::create_variant),
// `returns.object` (one host object per name while it is live: the same handle, no new A g), `calls.depth` d (the
// nested call only while the VM's context_depth < d: conformance.md's literal meaning, which a D16 swallow leaves above
// the engine calls in flight; controller ruling, fix 1), `swallow` (D16: HostContext::engine_call_swallowing). Compared
// after the call: `pc_after` (snapshot().pc, "@L+k" allowed), `expect.top`, `swallowed` (exactly the swallowed errors;
// no key = none), and `then` (a second call on the same VM; after an error the runner resets first, rule 19).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "lomn/osi/heap.h"
#include "lomn/osi/json.h"
#include "lomn/osi/natives.h"
#include "lomn/osi/registry.h"
#include "lomn/osi/trace.h"
#include "lomn/osi/vm.h"
#include "lomn/sage/osi.h"

#ifndef LOMN_OSI_DATA_DIR
#define LOMN_OSI_DATA_DIR "data/osi"
#endif

namespace sfs = std::filesystem;  // not `fs`: lomn::fs (the VFS) is visible through `using namespace lomn`
using namespace lomn;
using namespace lomn::osi;

namespace {

bool slurp(const sfs::path& p, std::string& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

std::string hex2bytes(const std::string& h) {
  std::string o;
  for (size_t i = 0; i + 1 < h.size(); i += 2) o += static_cast<char>(std::strtoul(h.substr(i, 2).c_str(), nullptr, 16));
  return o;
}

uint32_t num_u32(const Json& j) {
  int64_t v = 0;
  if (j.as_i64(v)) return static_cast<uint32_t>(v);
  if (j.type == Json::String) return static_cast<uint32_t>(std::strtoull(j.str.c_str(), nullptr, 0));
  return static_cast<uint32_t>(j.num());
}

// A type field: a number, "0x.." / decimal text, or a type name ("type:GcTimer", "GcTimer", "int", ...).
bool type_of(const Json& j, const TypeTable& types, uint16_t& out) {
  if (j.type == Json::Number) {
    out = static_cast<uint16_t>(num_u32(j));
    return true;
  }
  if (j.type != Json::String || j.str.empty()) return false;
  if (j.str[0] >= '0' && j.str[0] <= '9') {
    out = static_cast<uint16_t>(std::strtoul(j.str.c_str(), nullptr, 0));
    return true;
  }
  const uint16_t id = type_id_of(j.str, types);
  if (id == 0xffff) return false;
  out = id;
  return true;
}

// A value written in a program (args, returns, writes): a variant or a heap-allocating form.
bool make_value(const Json& j, Heap& heap, const TypeTable& types, Variant& out, std::string* err) {
  if (j.type != Json::Object) {
    *err = "value is not an object";
    return false;
  }
  if (const Json* t = j.get("text")) {
    out = Variant{kString, heap.new_string(t->str)};
    return true;
  }
  if (const Json* h = j.get("hex")) {
    out = Variant{kString, heap.new_string(hex2bytes(h->str))};
    return true;
  }
  if (const Json* g = j.get("game")) {
    const uint16_t id = types.id(g->str);
    if (id == 0xffff) {
      *err = "unknown game type " + g->str;
      return false;
    }
    out = Variant{id, heap.new_game(id)};
    return true;
  }
  Variant v;
  const Json* t = j.get("type");
  if (!t || !type_of(*t, types, v.type)) {
    *err = "bad or missing type";
    return false;
  }
  if (const Json* x = j.get("value")) v.value = num_u32(*x);
  if (const Json* x = j.get("bits")) v.value = static_cast<uint32_t>(std::strtoul(x->str.c_str(), nullptr, 16));
  out = v;
  return true;
}

// Fail-closed expectation keys (docs/osi/conformance.md amendment 18). A key path is the chain of object keys from the top of
// the .expect.json, `[*]` after a key whose value is an array of objects, and `*` for the name of a global under
// expect.globals / expect_error.globals. A non-pending program with a key path in neither set FAILS before it runs
// (`FAIL <path>: uncompared key <key path>`, no trace). The Python oracle keeps its own list (tools/research/osi_vm.py
// COMPARED_KEYS / IGNORED_KEYS); `--list-keys` prints both as sorted `compared|ignored <path>` lines and
// tools/tests/test_osi_conform_keys.py asserts the two are the same set.
const char* const kValueForms[] = {"text", "hex", "game", "type", "value", "bits"};  // make_value / check_value

void add_value(std::set<std::string>& s, const std::string& prefix, bool container) {
  if (container) s.insert(prefix);
  for (const char* f : kValueForms) s.insert(prefix + "." + f);
}

// the post-call state fields a program may check: context_depth, fpu_pc, globals, heap (check_state)
void add_state(std::set<std::string>& s, const std::string& prefix) {
  for (const char* f : {".context_depth", ".fpu_pc", ".globals", ".heap", ".heap.live", ".heap.allocs"}) s.insert(prefix + f);
  add_value(s, prefix + ".globals.*", true);
  for (const char* part : {"live", "allocs"})
    for (const char* kind : {"o", "a", "s", "g"}) s.insert(prefix + ".heap." + part + "." + kind);
}

std::set<std::string> compared_keys() {
  std::set<std::string> s = {"registry", "entry", "args", "natives", "expect", "expect_error"};
  add_value(s, "args[*]", false);
  for (const char* f : {"cls", "name", "min", "max", "types", "sets_fpu_pc64", "calls", "writes", "returns"})
    s.insert(std::string("natives[*].") + f);
  s.insert("natives[*].calls.entry");
  s.insert("natives[*].calls.args");
  add_value(s, "natives[*].calls.args[*]", false);
  s.insert("natives[*].writes[*].arg");
  add_value(s, "natives[*].writes[*]", false);
  s.insert("natives[*].returns.arg");
  add_value(s, "natives[*].returns", false);
  s.insert("expect.ok");
  s.insert("expect.depth");
  add_value(s, "expect.result", true);
  add_state(s, "expect");
  for (const char* f : {"message", "pc", "op", "depth"}) s.insert(std::string("expect_error.") + f);
  add_value(s, "expect_error.top", true);
  add_state(s, "expect_error");
  // M2a-2 T3 (controller ruling): exactly the 38 key paths the group-n programs use (conformance.md §1 "Native-framework
  // fields"), no value-form variants beyond them; the oracle's list is the same set (--list-keys must be byte-identical).
  for (const char* k : {"natives[*].calls.depth", "natives[*].returns.engine_refs", "natives[*].returns.object",
                        "natives[*].swallow", "expect.pc_after", "expect.top", "expect.top.type", "expect.top.value",
                        "expect_error.pc_after", "then", "then.args", "then.context_depth", "then.depth", "then.entry",
                        "then.ok", "then.pc_after", "then.result", "then.result.type", "then.result.value", "then.top",
                        "then.top.type", "then.top.value"})
    s.insert(k);
  for (const char* x : {"expect", "expect_error"})
    for (const char* f : {".swallowed", ".swallowed[*].depth", ".swallowed[*].message", ".swallowed[*].op", ".swallowed[*].pc",
                          ".swallowed[*].top", ".swallowed[*].top.type", ".swallowed[*].top.value"})
      s.insert(std::string(x) + f);
  return s;
}

// descriptive keys no runner compares (the coverage report reads some of them)
std::set<std::string> ignored_keys() {
  return {"id", "rows", "note", "alternative", "falsifier", "falsifier_id", "argc_violation", "pending"};
}

// every object key under `n` as (real path, pattern path); arrays of objects add `[*]`, scalars add nothing
void key_paths(const Json& n, const std::string& path, const std::string& pat,
               std::vector<std::pair<std::string, std::string>>& out) {
  if (n.type == Json::Array) {
    for (const Json& v : n.arr) key_paths(v, path + "[*]", pat + "[*]", out);
  } else if (n.type == Json::Object) {
    for (const auto& kv : n.obj) {
      const std::string wild = (pat == "expect.globals" || pat == "expect_error.globals") ? "*" : kv.first;
      const std::string rp = path.empty() ? kv.first : path + "." + kv.first;
      const std::string pp = pat.empty() ? wild : pat + "." + wild;
      out.emplace_back(rp, pp);
      key_paths(kv.second, rp, pp, out);
    }
  }
}

std::vector<std::string> uncompared_keys(const Json& ex) {
  static const std::set<std::string> compared = compared_keys(), ignored = ignored_keys();
  std::vector<std::pair<std::string, std::string>> all;
  key_paths(ex, "", "", all);
  std::vector<std::string> bad;
  for (const auto& kp : all)
    if (!compared.count(kp.second) && !ignored.count(kp.second)) bad.push_back(kp.first);
  return bad;
}

struct TestNative {
  const Json* spec = nullptr;
  const TypeTable* types = nullptr;
  HostContext* ctx = nullptr;
  std::map<std::string, uint32_t>* objects = nullptr;  // returns.object: name -> its handle (0 or freed: make a new one)
};

// a `returns` game value through the framework: a host object held engine_refs times (D13); `object` names one
struct TestObject : GameObject {
  using GameObject::GameObject;
};
Variant test_game_return(TestNative& tn, const Json& r, uint16_t id) {
  int32_t k = 0;
  if (const Json* e = r.get("engine_refs")) k = static_cast<int32_t>(num_u32(*e));
  GameObject* o = nullptr;
  const Json* name = r.get("object");
  if (name) {
    auto it = tn.objects->find(name->str);
    if (it != tn.objects->end()) o = tn.ctx->object(it->second);  // nullptr once freed: "creates a new object"
  }
  if (!o) o = tn.ctx->make<TestObject>(id, k);
  const Variant v = tn.ctx->create_variant(o, id);
  if (name) (*tn.objects)[name->str] = o->handle();
  return v;
}

Variant test_native(NativeCall& c, void* user) {
  TestNative& tn = *static_cast<TestNative*>(user);
  Heap& heap = c.vm.heap();
  std::string err;
  const Json* k = tn.spec->get("calls");  // one nested call: {"entry", "args", "depth"}
  const Json* d = k ? k->get("depth") : nullptr;
  if (k && (!d || c.vm.snapshot().context_depth < num_u32(*d))) {  // calls.depth: only while context_depth < d
    std::vector<Variant> args;
    if (const Json* a = k->get("args"))
      for (const Json& x : a->arr) {
        Variant v;
        if (!make_value(x, heap, *tn.types, v, &err)) throw VmError("Undefined: test native calls: " + err);
        args.push_back(v);
      }
    const Json* e = k->get("entry");
    const std::string entry = e ? e->str : std::string("inner");
    const Json* sw = tn.spec->get("swallow");
    if (sw && sw->type == Json::Bool && sw->b) tn.ctx->engine_call_swallowing(entry, args);  // D16, rule 17
    else tn.ctx->engine_call(entry, args);  // the host disposes the nested result after its R (§6.1)
  }
  if (const Json* ws = tn.spec->get("writes"))
    for (const Json& w : ws->arr) {
      const Json* a = w.get("arg");
      const uint32_t i = a ? num_u32(*a) : 0;
      if (i >= c.args.size()) throw VmError("Undefined: test native writes past argc");
      Variant v;
      if (!make_value(w, heap, *tn.types, v, &err)) throw VmError("Undefined: test native writes: " + err);
      *c.args[i] = v;  // overwrite without dispose (§6.14d); the following Pop(true) disposes it
    }
  const Json* r = tn.spec->get("returns");
  if (!r) return nothing();
  if (const Json* a = r->get("arg")) {
    const uint32_t i = num_u32(*a);
    if (i >= c.args.size()) throw VmError("Undefined: test native returns an absent argument");
    return heap.duplicate(*c.args[i]);
  }
  if (const Json* g = r->get("game")) {
    const uint16_t id = tn.types->id(g->str);
    if (id == 0xffff) throw VmError("Undefined: test native returns: unknown game type " + g->str);
    return test_game_return(tn, *r, id);
  }
  Variant v;
  if (!make_value(*r, heap, *tn.types, v, &err)) throw VmError("Undefined: test native returns: " + err);
  return v;
}

struct Runner {
  sfs::path corpus, osi_dir, data_dir, trace_dir;
  TypeTable types;
  NativeRegistry base;
  std::string sha_natives, sha_mods, sha_types;
  int fails_in_program = 0;
  std::string cur;

  void fail(const std::string& field, const std::string& want, const std::string& got) {
    ++fails_in_program;
    std::printf("FAIL %s: %s: expected %s, got %s\n", cur.c_str(), field.c_str(), want.c_str(), got.c_str());
  }
  void fail_msg(const std::string& field, const std::string& msg) {
    ++fails_in_program;
    std::printf("FAIL %s: %s: %s\n", cur.c_str(), field.c_str(), msg.c_str());
  }

  // compares a variant expectation; heap values compare by D7 handle id (§6.10); {"text"/"hex"} compares the text
  void check_value(const std::string& field, const Json& want, Variant got, const std::string* got_text) {
    if (want.type == Json::Null) {
      fail_msg(field, "null is only allowed for top");
      return;
    }
    if (const Json* t = want.get("text")) {
      if (got.type != kString) return fail(field, "a 0x16 string", fmt_variant(got));
      if (got_text && *got_text != t->str) fail(field + ".text", "\"" + fmt_message(t->str) + "\"", "\"" + fmt_message(*got_text) + "\"");
      return;
    }
    if (const Json* h = want.get("hex")) {
      if (got.type != kString) return fail(field, "a 0x16 string", fmt_variant(got));
      if (got_text && *got_text != hex2bytes(h->str)) fail(field + ".hex", h->str, fmt_hex(*got_text));
      return;
    }
    if (const Json* g = want.get("game")) {
      const uint16_t id = types.id(g->str);
      if (got.type != id) fail(field + ".type", g->str, fmt_variant(got));
      return;
    }
    Variant w;
    const Json* t = want.get("type");
    if (!t || !type_of(*t, types, w.type)) return fail_msg(field, "bad expectation (type)");
    if (const Json* x = want.get("value")) w.value = num_u32(*x);
    if (const Json* x = want.get("bits")) w.value = static_cast<uint32_t>(std::strtoul(x->str.c_str(), nullptr, 16));
    if (w != got) fail(field, fmt_variant(w), fmt_variant(got));
  }

  void check_u32(const Json* want, uint32_t got, const std::string& field) {
    if (!want) return;
    const uint32_t w = num_u32(*want);
    if (w != got) fail(field, std::to_string(w), std::to_string(got));
  }

  // a PC expectation: an integer, "@L" or "@L+k" (a label plus a decimal byte count), as an absolute file offset
  const std::map<std::string, uint32_t>* labels = nullptr;
  bool resolve_pc(const Json& j, uint32_t& out, const std::string& field) {
    if (j.type == Json::String && !j.str.empty() && j.str[0] == '@') {
      std::string name = j.str.substr(1);
      uint32_t add = 0;
      const size_t plus = name.find('+');
      if (plus != std::string::npos) {
        add = static_cast<uint32_t>(std::strtoul(name.c_str() + plus + 1, nullptr, 10));
        name.resize(plus);
      }
      auto it = labels->find(name);
      if (it == labels->end()) {
        fail_msg(field, "unknown label " + j.str);
        return false;
      }
      out = it->second + add;
      return true;
    }
    out = num_u32(j);
    return true;
  }
  void check_pc(const Json* want, uint32_t got, const std::string& field) {
    if (!want) return;
    uint32_t w = 0;
    if (!resolve_pc(*want, w, field)) return;
    char a[16], b[16];
    std::snprintf(a, sizeof a, "%08x", w);
    std::snprintf(b, sizeof b, "%08x", got);
    if (w != got) fail(field, a, b);
  }
  // `top`: null = depth 0, else the top slot's value
  void check_top(const Json* t, bool has_top, Variant top, const std::string& field) {
    if (!t) return;
    if (t->type == Json::Null) {
      if (has_top) fail(field, "null (depth 0)", fmt_variant(top));
    } else if (!has_top) {
      fail(field, "a value", "depth 0");
    } else {
      check_value(field, *t, top, nullptr);
    }
  }
  // `swallowed`: exactly the errors swallowed during the call, in order (no key = none)
  void check_swallowed(const Json* want, const std::vector<Vm::Swallowed>& got, const std::string& field) {
    const size_t n = want ? want->arr.size() : 0;
    if (n != got.size()) {
      fail(field, std::to_string(n) + " swallowed error(s)", std::to_string(got.size()));
      return;
    }
    for (size_t i = 0; i < n; ++i) {
      const Json& w = want->arr[i];
      const Vm::Swallowed& g = got[i];
      const std::string f = field + "[" + std::to_string(i) + "]";
      if (const Json* m = w.get("message"))
        if (m->str != g.error.message)
          fail(f + ".message", "\"" + fmt_message(m->str) + "\"", "\"" + fmt_message(g.error.message) + "\"");
      check_pc(w.get("pc"), g.error.pc, f + ".pc");
      check_u32(w.get("op"), g.error.op, f + ".op");
      check_u32(w.get("depth"), g.error.depth, f + ".depth");
      check_top(w.get("top"), g.has_top, g.top, f + ".top");
    }
  }

  enum class Outcome { Pass, Fail, Pending };

  // The task id when the expectation is pending (a non-empty string `pending`), else empty.
  static std::string pending_task(const Json& ex) {
    const Json* p = ex.get("pending");
    return p && p->type == Json::String ? p->str : std::string();
  }

  Outcome run_program(const sfs::path& expect_path) {
    cur = sfs::relative(expect_path, corpus).generic_string();
    std::string text, err;
    Json ex;
    if (slurp(expect_path, text) && parse_json(text, ex, &err)) {
      const std::string task = pending_task(ex);
      if (!task.empty()) {
        std::string id = cur.substr(0, cur.size() - std::string(".expect.json").size());
        std::printf("PENDING %s (%s)\n", id.c_str(), task.c_str());
        return Outcome::Pending;
      }
    }
    return run_one(expect_path) ? Outcome::Pass : Outcome::Fail;
  }

  bool run_one(const sfs::path& expect_path) {
    fails_in_program = 0;
    cur = sfs::relative(expect_path, corpus).generic_string();
    std::string text, err;
    Json ex;
    if (!slurp(expect_path, text) || !parse_json(text, ex, &err)) {
      fail_msg("expect.json", "unreadable: " + err);
      return false;
    }
    if (const auto bad = uncompared_keys(ex); !bad.empty()) {  // fail closed: nothing runs, no trace
      for (const std::string& k : bad) {
        ++fails_in_program;
        std::printf("FAIL %s: uncompared key %s\n", cur.c_str(), k.c_str());
      }
      return false;
    }
    std::string stem = expect_path.filename().string();
    stem = stem.substr(0, stem.size() - std::string(".expect.json").size());
    const sfs::path rel_dir = sfs::relative(expect_path.parent_path(), corpus);
    sfs::path img_dir = osi_dir.empty() ? expect_path.parent_path() : osi_dir / rel_dir;
    std::string image;
    if (!slurp(img_dir / (stem + ".osi"), image)) {
      fail_msg("osi", "missing " + (img_dir / (stem + ".osi")).string());
      return false;
    }
    std::map<std::string, uint32_t> labels;
    {
      std::string lt;
      if (slurp(img_dir / (stem + ".labels"), lt)) {
        std::istringstream ls(lt);
        std::string name;
        uint32_t off;
        while (ls >> name >> off) labels[name] = off;
      }
    }
    sage::OsiFile file;
    if (!file.parse(std::vector<uint8_t>(image.begin(), image.end()), &err)) {
      fail_msg("osi", "parse: " + err);
      return false;
    }
    NativeRegistry reg;
    const Json* rj = ex.get("registry");
    if (rj && rj->str == "natives.json") reg = base;
    std::vector<std::unique_ptr<TestNative>> tns;
    std::map<std::string, uint32_t> objects;
    if (const Json* ns = ex.get("natives"))
      for (const Json& n : ns->arr) {
        NativeEntry e;
        e.cls = n.get("cls") ? n.get("cls")->str : "";
        e.name = n.get("name") ? n.get("name")->str : "";
        e.key = NativeRegistry::fold(e.cls + "." + e.name);
        e.min = static_cast<uint8_t>(n.get("min") ? num_u32(*n.get("min")) : 0);
        e.max = static_cast<uint8_t>(n.get("max") ? num_u32(*n.get("max")) : 0);
        e.types.fill(kNothing);
        if (const Json* ts = n.get("types"))
          for (size_t i = 0; i < ts->arr.size() && i < 10; ++i) {
            const uint16_t id = ts->arr[i].type == Json::String ? type_id_of(ts->arr[i].str, types) : 0xffff;
            if (id == 0xffff) {
              fail_msg("natives", "unknown type in " + e.key);
              return false;
            }
            e.types[i] = id;
          }
        if (const Json* f = n.get("sets_fpu_pc64")) e.sets_fpu_pc64 = f->type == Json::Bool && f->b;
        tns.push_back(std::make_unique<TestNative>());
        tns.back()->spec = &n;
        tns.back()->types = &types;
        tns.back()->objects = &objects;
        e.fn = test_native;
        e.user = tns.back().get();
        e.tier = Tier::Implemented;
        reg.add(std::move(e));
      }
    StringTrace tr;
    Heap heap(&tr);
    Vm vm(file, reg, heap, &tr);
    HostContext ctx(vm, heap, nullptr, HostFlags{}, nullptr);
    for (auto& tn : tns) tn->ctx = &ctx;
    this->labels = &labels;
    std::vector<Variant> args;
    if (const Json* a = ex.get("args"))
      for (const Json& x : a->arr) {  // host strings are allocated in index order before the call (§6.2)
        Variant v;
        if (!make_value(x, heap, types, v, &err)) {
          fail_msg("args", err);
          return false;
        }
        args.push_back(v);
      }
    const std::string entry = ex.get("entry") ? ex.get("entry")->str : std::string("main");
    CallResult r = vm.call(entry, args);
    const VmState st = vm.snapshot();
    const VmError* e = vm.last_error();
    const std::vector<Vm::Swallowed> swallowed = vm.swallowed();
    // rule 19: a second engine call on the same VM, after the first call has been compared
    auto run_then = [&](const Json& th) {
      std::vector<Variant> targs;
      if (const Json* a = th.get("args"))
        for (const Json& x : a->arr) {
          Variant v;
          if (!make_value(x, heap, types, v, &err)) return fail_msg("then.args", err);
          targs.push_back(v);
        }
      const std::string te = th.get("entry") ? th.get("entry")->str : std::string("main2");
      CallResult r2 = vm.call(te, targs);
      const VmState s2 = vm.snapshot();
      if (const Json* x = th.get("ok"))
        if (x->type == Json::Bool && x->b != (r2.ok && !vm.last_error()))
          fail("then.ok", x->b ? "true" : "false",
               vm.last_error() ? "error \"" + fmt_message(vm.last_error()->message) + "\"" : (r2.ok ? "true" : "false"));
      if (const Json* x = th.get("result")) check_value("then.result", *x, r2.result, &r2.result_text);
      check_u32(th.get("depth"), s2.depth, "then.depth");
      check_top(th.get("top"), s2.depth > 0, s2.top, "then.top");
      check_u32(th.get("context_depth"), s2.context_depth, "then.context_depth");
      check_pc(th.get("pc_after"), s2.pc, "then.pc_after");
    };
    const Json* then = ex.get("then");
    // the post-call state fields shared by expect and expect_error (the oracle's _cmp_state): context_depth, fpu_pc,
    // globals.<name>, heap.{live,allocs}.{o,a,s,g}; only the fields present are compared (§6.7)
    auto check_state = [&](const Json& x) {
      check_u32(x.get("context_depth"), st.context_depth, "context_depth");
      check_u32(x.get("fpu_pc"), static_cast<uint32_t>(st.fpu_pc), "fpu_pc");
      if (const Json* g = x.get("globals"))
        for (const auto& kv : g->obj) {
          const Variant* v = vm.find_global(kv.first);
          const Variant got = v ? *v : nothing();
          std::string t = got.type == kString && heap.live(kKindString, got.value) ? heap.text(got.value) : std::string();
          check_value("globals." + kv.first, kv.second, got, &t);
        }
      if (const Json* h = x.get("heap")) {
        const HeapStats hs = heap.stats();
        const char* kinds[4] = {"o", "a", "s", "g"};
        for (const char* part : {"live", "allocs"})
          if (const Json* p = h->get(part))
            for (int k = 0; k < 4; ++k)
              if (const Json* v = p->get(kinds[k]))
                check_u32(v, static_cast<uint32_t>(std::string(part) == "live" ? hs.live[k] : hs.allocs[k]),
                          std::string("heap.") + part + "." + kinds[k]);
      }
    };
    if (const Json* ee = ex.get("expect_error")) {
      if (!e) {
        fail("expect_error", "an error", r.found ? "ok" : "function not found");
      } else {
        if (const Json* m = ee->get("message"))
          if (m->str != e->message) fail("message", "\"" + fmt_message(m->str) + "\"", "\"" + fmt_message(e->message) + "\"");
        check_pc(ee->get("pc"), e->pc, "pc");
        if (const Json* op = ee->get("op")) check_u32(op, e->op, "op");
        check_u32(ee->get("depth"), e->depth, "depth");
        check_top(ee->get("top"), st.depth > 0, st.top, "top");
        check_pc(ee->get("pc_after"), st.pc, "pc_after");  // rule 15: snapshot().pc after the error
        check_swallowed(ee->get("swallowed"), swallowed, "swallowed");
        check_state(*ee);  // before the reset: the snapshot's context_depth, fpu_pc, the globals and the heap
        // our host behaviour (not the original's): reset, then the second entry point must run
        vm.reset_after_error();
        bool has_main2 = false;
        for (const auto& f : file.functions())
          if (NativeRegistry::fold(f.name) == "main2") has_main2 = true;
        if (then) {
          run_then(*then);  // `then` replaces the implicit main2 check (rule 19)
        } else if (has_main2) {
          CallResult r2 = vm.call("main2", {});
          if (!r2.found || !r2.ok)
            fail("main2", "ok after reset_after_error", vm.last_error() ? "\"" + fmt_message(vm.last_error()->message) + "\"" : "not found");
        }
      }
    } else if (const Json* ok = ex.get("expect")) {
      if (e) fail("ok", "true", "error \"" + fmt_message(e->message) + "\"");
      if (const Json* x = ok->get("ok"))
        if (x->type == Json::Bool && x->b != r.ok) fail("ok", x->b ? "true" : "false", r.ok ? "true" : "false");  // M: ok (§6.14a)
      if (const Json* x = ok->get("result")) check_value("result", *x, r.result, &r.result_text);
      check_u32(ok->get("depth"), st.depth, "depth");
      check_top(ok->get("top"), st.depth > 0, st.top, "top");
      check_pc(ok->get("pc_after"), st.pc, "pc_after");
      check_swallowed(ok->get("swallowed"), swallowed, "swallowed");
      check_state(*ok);
      if (then) {  // rule 19: reset first if the first call raised; otherwise run above its leftovers (D16 rule 5)
        if (e) vm.reset_after_error();
        run_then(*then);
      }
    } else {
      fail_msg("expect.json", "neither expect nor expect_error");
    }
    if (!trace_dir.empty()) {
      const sfs::path d = trace_dir / rel_dir;
      sfs::create_directories(d);
      FileTrace out((d / (stem + ".trace")).string());
      out.line(header_line(sha256_hex(reinterpret_cast<const uint8_t*>(image.data()), image.size()), sha_natives, sha_mods, sha_types));
      for (const auto& l : tr.lines) out.line(l);
    }
    return fails_in_program == 0;
  }
};

std::string file_sha(const sfs::path& p) {
  std::string s;
  if (!slurp(p, s)) return std::string(64, '0');
  return sha256_hex(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

}  // namespace

int main(int argc, char** argv) {
  Runner rn;
  rn.data_dir = LOMN_OSI_DATA_DIR;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "lomn_osi_conform: %s needs a value\n", a.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--osi-dir") rn.osi_dir = next();
    else if (a == "--data-dir") rn.data_dir = next();
    else if (a == "--trace-dir") rn.trace_dir = next();
    else if (a == "--list-keys") {
      for (const auto& k : compared_keys()) std::printf("compared %s\n", k.c_str());
      for (const auto& k : ignored_keys()) std::printf("ignored %s\n", k.c_str());
      return 0;
    }
    else if (!a.empty() && a[0] == '-') {
      std::fprintf(stderr, "usage: lomn_osi_conform CORPUS_DIR [--osi-dir DIR] [--data-dir DIR] [--trace-dir DIR]\n");
      return 2;
    } else rn.corpus = a;
  }
  if (rn.corpus.empty()) {
    std::fprintf(stderr, "usage: lomn_osi_conform CORPUS_DIR [--osi-dir DIR] [--data-dir DIR] [--trace-dir DIR]\n");
    return 2;
  }
  std::string tj, nj, mj;
  if (!slurp(rn.data_dir / "types.json", tj) || !slurp(rn.data_dir / "natives.json", nj) ||
      !slurp(rn.data_dir / "natives_beta150.json", mj)) {
    std::fprintf(stderr, "lomn_osi_conform: cannot read the tables in %s\n", rn.data_dir.string().c_str());
    return 2;
  }
  rn.types = TypeTable::load(tj);
  try {
    rn.base = NativeRegistry::load(nj, mj, rn.types);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "lomn_osi_conform: %s\n", e.what());
    return 2;
  }
  rn.sha_natives = file_sha(rn.data_dir / "natives.json");
  rn.sha_mods = file_sha(rn.data_dir / "natives_beta150.json");
  rn.sha_types = file_sha(rn.data_dir / "types.json");
  std::vector<sfs::path> progs;
  std::error_code ec;
  for (sfs::recursive_directory_iterator it(rn.corpus, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string n = it->path().filename().string();
    if (n.size() > 12 && n.compare(n.size() - 12, 12, ".expect.json") == 0) progs.push_back(it->path());
  }
  std::sort(progs.begin(), progs.end());
  int pass = 0, pending = 0;
  for (const auto& p : progs) {
    const Runner::Outcome o = rn.run_program(p);
    pass += o == Runner::Outcome::Pass ? 1 : 0;
    pending += o == Runner::Outcome::Pending ? 1 : 0;
  }
  const int total = static_cast<int>(progs.size()) - pending;
  std::printf("conformance: %d/%d pass, %d pending\n", pass, total, pending);
  if (progs.empty()) return 77;
  return pass == total ? 0 : 1;
}
