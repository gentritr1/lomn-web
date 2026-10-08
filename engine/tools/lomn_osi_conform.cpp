// M2a-1 T1: lomn_osi_conform, the C++ conformance runner (docs/osi/conformance.md §1, §4, §6 amendments 1-14).
//
//   lomn_osi_conform CORPUS_DIR [--osi-dir DIR] [--data-dir data/osi] [--trace-dir DIR]
//
// Every `<id>.expect.json` under CORPUS_DIR (recursively, sorted by path) is one program. Its image `<id>.osi` and label
// table `<id>.labels` come from --osi-dir (the same relative directory; written at build time by
// tools/osi/conform_assemble.py from `<id>.asm`), or from next to the expectation. Prints one `FAIL <path>: <field>: ...`
// line per differing field and `conformance: N/M pass`; exit 0 when all pass, 1 on any failure, 77 when there is no
// program (ctest's skip code: an empty corpus is not a pass).
//
// Test natives are declarative (conformance.md §1, §6.1): types in the registry vocabulary; `returns` (a variant,
// {"game": C}, {"text": t}, {"hex": h}, {"arg": i}); `writes` (by-ref slots overwritten WITHOUT dispose, §6.14d);
// `calls` ({"entry", "args"}, a nested vm.call before the native returns, §6.1); `sets_fpu_pc64`. Inside a native the
// order is: nested calls, then writes, then the return value.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "lomn/osi/heap.h"
#include "lomn/osi/json.h"
#include "lomn/osi/registry.h"
#include "lomn/osi/trace.h"
#include "lomn/osi/vm.h"
#include "lomn/sage/osi.h"

#ifndef LOMN_OSI_DATA_DIR
#define LOMN_OSI_DATA_DIR "data/osi"
#endif

namespace fs = std::filesystem;
using namespace lomn;
using namespace lomn::osi;

namespace {

bool slurp(const fs::path& p, std::string& out) {
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

struct Ctx {
  const TypeTable* types = nullptr;
};

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

struct TestNative {
  const Json* spec = nullptr;
  const TypeTable* types = nullptr;
};

Variant test_native(NativeCall& c, void* user) {
  const TestNative& tn = *static_cast<TestNative*>(user);
  Heap& heap = c.vm.heap();
  std::string err;
  if (const Json* calls = tn.spec->get("calls")) {
    std::vector<const Json*> list;
    if (calls->type == Json::Array)
      for (const Json& x : calls->arr) list.push_back(&x);
    else
      list.push_back(calls);
    for (const Json* k : list) {
      std::vector<Variant> args;
      if (const Json* a = k->get("args"))
        for (const Json& x : a->arr) {
          Variant v;
          if (!make_value(x, heap, *tn.types, v, &err)) throw VmError("Undefined: test native calls: " + err);
          args.push_back(v);
        }
      const Json* e = k->get("entry");
      c.vm.call(e ? e->str : std::string("inner"), args);  // the host disposes the nested result after its R (§6.1)
    }
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
  Variant v;
  if (!make_value(*r, heap, *tn.types, v, &err)) throw VmError("Undefined: test native returns: " + err);
  return v;
}

struct Runner {
  fs::path corpus, osi_dir, data_dir, trace_dir;
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

  bool run_one(const fs::path& expect_path) {
    fails_in_program = 0;
    cur = fs::relative(expect_path, corpus).generic_string();
    std::string text, err;
    Json ex;
    if (!slurp(expect_path, text) || !parse_json(text, ex, &err)) {
      fail_msg("expect.json", "unreadable: " + err);
      return false;
    }
    std::string stem = expect_path.filename().string();
    stem = stem.substr(0, stem.size() - std::string(".expect.json").size());
    const fs::path rel_dir = fs::relative(expect_path.parent_path(), corpus);
    fs::path img_dir = osi_dir.empty() ? expect_path.parent_path() : osi_dir / rel_dir;
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
        e.fn = test_native;
        e.user = tns.back().get();
        e.tier = Tier::Implemented;
        reg.add(std::move(e));
      }
    StringTrace tr;
    Heap heap(&tr);
    Vm vm(file, reg, heap, &tr);
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
    if (const Json* ee = ex.get("expect_error")) {
      if (!e) {
        fail("expect_error", "an error", r.found ? "ok" : "function not found");
      } else {
        if (const Json* m = ee->get("message"))
          if (m->str != e->message) fail("message", "\"" + fmt_message(m->str) + "\"", "\"" + fmt_message(e->message) + "\"");
        if (const Json* pc = ee->get("pc")) {
          uint32_t want = 0;
          if (pc->type == Json::String && !pc->str.empty() && pc->str[0] == '@') {
            auto it = labels.find(pc->str.substr(1));
            if (it == labels.end()) fail_msg("pc", "unknown label " + pc->str);
            else want = it->second;
          } else {
            want = num_u32(*pc);
          }
          char a[16], b[16];
          std::snprintf(a, sizeof a, "%08x", want);
          std::snprintf(b, sizeof b, "%08x", e->pc);
          if (want != e->pc) fail("pc", a, b);
        }
        if (const Json* op = ee->get("op")) check_u32(op, e->op, "op");
        check_u32(ee->get("depth"), e->depth, "depth");
        if (const Json* t = ee->get("top")) {
          if (t->type == Json::Null) {
            if (st.depth != 0) fail("top", "null (depth 0)", fmt_variant(st.top));
          } else if (st.depth == 0) {
            fail("top", "a value", "depth 0");
          } else {
            check_value("top", *t, st.top, nullptr);
          }
        }
        check_u32(ee->get("fpu_pc"), static_cast<uint32_t>(st.fpu_pc), "fpu_pc");
        // our host behaviour (not the original's): reset, then the second entry point must run
        vm.reset_after_error();
        bool has_main2 = false;
        for (const auto& f : file.functions())
          if (NativeRegistry::fold(f.name) == "main2") has_main2 = true;
        if (has_main2) {
          CallResult r2 = vm.call("main2", {});
          if (!r2.found || !r2.ok)
            fail("main2", "ok after reset_after_error", vm.last_error() ? "\"" + fmt_message(vm.last_error()->message) + "\"" : "not found");
        }
      }
    } else if (const Json* ok = ex.get("expect")) {
      if (e) fail("ok", "true", "error \"" + fmt_message(e->message) + "\"");
      if (const Json* x = ok->get("ok"))
        if (x->type == Json::Bool && x->b != r.ok) fail("ok", x->b ? "true" : "false", r.ok ? "true" : "false");  // M: ok (§6.14a)
      if (const Json* x = ok->get("found"))
        if (x->type == Json::Bool && x->b != r.found) fail("found", x->b ? "true" : "false", r.found ? "true" : "false");
      if (const Json* x = ok->get("result")) check_value("result", *x, r.result, &r.result_text);
      check_u32(ok->get("depth"), st.depth, "depth");
      check_u32(ok->get("context_depth"), st.context_depth, "context_depth");
      check_u32(ok->get("fpu_pc"), static_cast<uint32_t>(st.fpu_pc), "fpu_pc");
      if (const Json* g = ok->get("globals"))
        for (const auto& kv : g->obj) {
          const Variant* v = vm.find_global(kv.first);
          const Variant got = v ? *v : nothing();
          std::string t = got.type == kString && heap.live(kKindString, got.value) ? heap.text(got.value) : std::string();
          check_value("globals." + kv.first, kv.second, got, &t);
        }
      if (const Json* h = ok->get("heap")) {  // optional; only the fields present (§6.7)
        const HeapStats s = heap.stats();
        const char* kinds[4] = {"o", "a", "s", "g"};
        for (const char* part : {"live", "allocs"})
          if (const Json* p = h->get(part))
            for (int k = 0; k < 4; ++k)
              if (const Json* x = p->get(kinds[k]))
                check_u32(x, static_cast<uint32_t>(std::string(part) == "live" ? s.live[k] : s.allocs[k]),
                          std::string("heap.") + part + "." + kinds[k]);
      }
    } else {
      fail_msg("expect.json", "neither expect nor expect_error");
    }
    if (!trace_dir.empty()) {
      const fs::path d = trace_dir / rel_dir;
      fs::create_directories(d);
      FileTrace out((d / (stem + ".trace")).string());
      out.line(header_line(sha256_hex(reinterpret_cast<const uint8_t*>(image.data()), image.size()), sha_natives, sha_mods, sha_types));
      for (const auto& l : tr.lines) out.line(l);
    }
    return fails_in_program == 0;
  }
};

std::string file_sha(const fs::path& p) {
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
  std::vector<fs::path> progs;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(rn.corpus, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string n = it->path().filename().string();
    if (n.size() > 12 && n.compare(n.size() - 12, 12, ".expect.json") == 0) progs.push_back(it->path());
  }
  std::sort(progs.begin(), progs.end());
  int pass = 0;
  for (const auto& p : progs) pass += rn.run_one(p) ? 1 : 0;
  std::printf("conformance: %d/%d pass\n", pass, static_cast<int>(progs.size()));
  if (progs.empty()) return 77;
  return pass == static_cast<int>(progs.size()) ? 0 : 1;
}
