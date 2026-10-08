// M2a-1 T1: the native registry, the type table and the JSON reader they load from.
#include "lomn/osi/registry.h"

#include <cctype>
#include <cstdlib>
#include <stdexcept>

#include "lomn/osi/heap.h"
#include "lomn/osi/json.h"


namespace lomn::osi {

// ---------------- JSON ----------------
const Json* Json::get(std::string_view key) const {
  if (type != Object) return nullptr;
  for (const auto& kv : obj)
    if (kv.first == key) return &kv.second;
  return nullptr;
}
double Json::num() const { return type == Number ? std::strtod(str.c_str(), nullptr) : 0.0; }
bool Json::as_i64(int64_t& out) const {
  if (type != Number || str.empty()) return false;
  for (char c : str)
    if (c == '.' || c == 'e' || c == 'E') return false;
  char* end = nullptr;
  out = std::strtoll(str.c_str(), &end, 10);
  return end && *end == 0;
}

namespace {
struct Parser {
  std::string_view s;
  size_t i = 0;
  std::string err;
  void ws() {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
  }
  bool fail(const char* m) {
    if (err.empty()) err = std::string(m) + " at byte " + std::to_string(i);
    return false;
  }
  bool lit(const char* w) {
    size_t n = std::char_traits<char>::length(w);
    if (s.substr(i, n) != w) return fail("bad literal");
    i += n;
    return true;
  }
  static void utf8(std::string& o, uint32_t cp) {
    if (cp < 0x80) {
      o += static_cast<char>(cp);
    } else if (cp < 0x800) {
      o += static_cast<char>(0xc0 | (cp >> 6));
      o += static_cast<char>(0x80 | (cp & 0x3f));
    } else {
      o += static_cast<char>(0xe0 | (cp >> 12));
      o += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
      o += static_cast<char>(0x80 | (cp & 0x3f));
    }
  }
  bool string(std::string& o) {
    if (i >= s.size() || s[i] != '"') return fail("expected a string");
    ++i;
    while (i < s.size() && s[i] != '"') {
      char c = s[i++];
      if (c != '\\') {
        o += c;
        continue;
      }
      if (i >= s.size()) return fail("bad escape");
      char e = s[i++];
      switch (e) {
        case '"': o += '"'; break;
        case '\\': o += '\\'; break;
        case '/': o += '/'; break;
        case 'b': o += '\b'; break;
        case 'f': o += '\f'; break;
        case 'n': o += '\n'; break;
        case 'r': o += '\r'; break;
        case 't': o += '\t'; break;
        case 'u': {
          if (i + 4 > s.size()) return fail("bad \\u");
          uint32_t cp = static_cast<uint32_t>(std::strtoul(std::string(s.substr(i, 4)).c_str(), nullptr, 16));
          i += 4;
          utf8(o, cp);
          break;
        }
        default: return fail("bad escape");
      }
    }
    if (i >= s.size()) return fail("unterminated string");
    ++i;
    return true;
  }
  bool value(Json& v, int depth) {
    if (depth > 64) return fail("nesting too deep");
    ws();
    if (i >= s.size()) return fail("unexpected end");
    char c = s[i];
    if (c == '{') {
      v.type = Json::Object;
      ++i;
      ws();
      if (i < s.size() && s[i] == '}') return ++i, true;
      for (;;) {
        ws();
        std::string k;
        if (!string(k)) return false;
        ws();
        if (i >= s.size() || s[i] != ':') return fail("expected ':'");
        ++i;
        Json m;
        if (!value(m, depth + 1)) return false;
        v.obj.emplace_back(std::move(k), std::move(m));
        ws();
        if (i < s.size() && s[i] == ',') {
          ++i;
          continue;
        }
        if (i < s.size() && s[i] == '}') return ++i, true;
        return fail("expected ',' or '}'");
      }
    }
    if (c == '[') {
      v.type = Json::Array;
      ++i;
      ws();
      if (i < s.size() && s[i] == ']') return ++i, true;
      for (;;) {
        Json m;
        if (!value(m, depth + 1)) return false;
        v.arr.push_back(std::move(m));
        ws();
        if (i < s.size() && s[i] == ',') {
          ++i;
          continue;
        }
        if (i < s.size() && s[i] == ']') return ++i, true;
        return fail("expected ',' or ']'");
      }
    }
    if (c == '"') {
      v.type = Json::String;
      return string(v.str);
    }
    if (c == 't') {
      v.type = Json::Bool;
      v.b = true;
      return lit("true");
    }
    if (c == 'f') {
      v.type = Json::Bool;
      return lit("false");
    }
    if (c == 'n') {
      v.type = Json::Null;
      return lit("null");
    }
    size_t st = i;
    if (s[i] == '-') ++i;
    while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.' || s[i] == 'e' || s[i] == 'E' ||
                            s[i] == '+' || s[i] == '-'))
      ++i;
    if (i == st) return fail("unexpected character");
    v.type = Json::Number;
    v.str = std::string(s.substr(st, i - st));
    return true;
  }
};
}  // namespace

bool parse_json(std::string_view text, Json& out, std::string* err) {
  Parser p;
  p.s = text;
  out = Json();
  if (!p.value(out, 0)) {
    if (err) *err = p.err;
    return false;
  }
  p.ws();
  if (p.i != text.size()) {
    if (err) *err = "trailing data at byte " + std::to_string(p.i);
    return false;
  }
  return true;
}

// ---------------- types ----------------
TypeTable TypeTable::load(std::string_view json, std::string* err) {
  TypeTable t;
  Json j;
  std::string e;
  if (!parse_json(json, j, &e)) {
    if (err) *err = e;
    return t;
  }
  const Json* types = j.get("types");
  if (!types) return t;
  for (const Json& x : types->arr) {
    const Json* n = x.get("name");
    const Json* id = x.get("id");
    int64_t v = 0;
    if (!n || !id || !id->as_i64(v)) continue;
    t.ids_[n->str] = static_cast<uint16_t>(v);
    if (!t.names_.count(static_cast<uint16_t>(v))) t.names_[static_cast<uint16_t>(v)] = n->str;
  }
  return t;
}

uint16_t TypeTable::id(std::string_view name) const {
  auto it = ids_.find(name);
  if (it != ids_.end()) return it->second;
  auto jt = ids_.find("type:" + std::string(name));
  return jt != ids_.end() ? jt->second : 0xffff;
}

const std::string& TypeTable::name(uint16_t id) const {
  static const std::string empty;
  auto it = names_.find(id);
  return it != names_.end() ? it->second : empty;
}

uint16_t type_id_of(std::string_view name, const TypeTable& types) {
  if (name == "int") return kInt;
  if (name == "float") return kFloat;
  if (name == "string") return kString;
  if (name == "colour") return kColour;
  if (name == "colour16") return kColour16;
  if (name == "string-id") return kStringId;
  if (name == "byref") return kByRef;
  if (name == "nothing") return kNothing;
  if (name == "unknown:0x0") return 0;
  return types.id(name);
}

// ---------------- registry ----------------
std::string NativeRegistry::fold(std::string_view s) {
  std::string o(s);
  for (char& c : o)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return o;
}

namespace {
RetKind ret_kind(const std::string& s) {
  if (s == "int") return RetKind::Int;
  if (s == "float") return RetKind::Float;
  if (s == "string") return RetKind::String;
  if (s == "colour") return RetKind::Colour;
  if (s == "game") return RetKind::Game;
  if (s == "copy") return RetKind::Copy;
  return RetKind::None;
}
uint32_t parse_u32(const Json& j) {
  int64_t v = 0;
  if (j.as_i64(v)) return static_cast<uint32_t>(v);
  if (j.type == Json::String) return static_cast<uint32_t>(std::strtoull(j.str.c_str(), nullptr, 0));
  return 0;
}
NativeEntry entry_from(const Json& r, const TypeTable& types, Tier tier) {
  NativeEntry e;
  auto str = [&](const char* k) {
    const Json* x = r.get(k);
    return x && x->type == Json::String ? x->str : std::string();
  };
  e.cls = str("cls");
  e.name = str("name");
  e.key = NativeRegistry::fold(e.cls + "." + e.name);
  if (const Json* x = r.get("min")) e.min = static_cast<uint8_t>(parse_u32(*x));
  if (const Json* x = r.get("max")) e.max = static_cast<uint8_t>(parse_u32(*x));
  e.types.fill(kNothing);
  if (const Json* t = r.get("types"))
    for (size_t i = 0; i < t->arr.size() && i < 10; ++i)
      if (t->arr[i].type == Json::String) {
        uint16_t id = type_id_of(t->arr[i].str, types);
        if (id == 0xffff) throw std::runtime_error("natives: unknown type " + t->arr[i].str + " in " + e.key);
        e.types[i] = id;
      }
  e.ret = ret_kind(str("ret"));
  if (e.ret == RetKind::Game) {
    uint16_t id = types.id("type:" + str("ret_class"));
    if (id == 0xffff) throw std::runtime_error("natives: unknown ret_class in " + e.key);
    e.ret_type = id;
  }
  if (const Json* d = r.get("default")) {
    if (d->type == Json::String && d->str == "nothing") {
      e.default_ret = nothing();
    } else if (d->type == Json::Object) {
      Variant v;
      if (const Json* t = d->get("type")) v.type = static_cast<uint16_t>(parse_u32(*t));
      if (const Json* x = d->get("value")) v.value = parse_u32(*x);
      if (const Json* x = d->get("bits")) v.value = static_cast<uint32_t>(std::strtoul(x->str.c_str(), nullptr, 16));
      e.default_ret = v;
    }
  }
  if (const Json* f = r.get("sets_fpu_pc64")) e.sets_fpu_pc64 = f->type == Json::Bool && f->b;
  e.tier = tier;
  return e;
}
}  // namespace

NativeRegistry NativeRegistry::load(std::string_view natives_json, std::string_view mods_json, const TypeTable& types) {
  NativeRegistry reg;
  for (int pass = 0; pass < 2; ++pass) {
    std::string_view text = pass == 0 ? natives_json : mods_json;
    if (text.empty()) continue;
    Json j;
    std::string err;
    if (!parse_json(text, j, &err)) throw std::runtime_error("natives: " + err);
    if (j.type != Json::Array) throw std::runtime_error("natives: not an array");
    for (const Json& r : j.arr) {
      const Json* w = r.get("winner");
      if (w && w->type == Json::Bool && !w->b) continue;  // the map keeps the first registration (types.md)
      NativeEntry e = entry_from(r, types, pass == 0 ? Tier::Stub : Tier::Mod);
      if (reg.index_.count(e.key)) continue;              // an exe key wins over a mod row
      reg.index_[e.key] = reg.entries_.size();
      reg.entries_.push_back(std::move(e));
    }
  }
  return reg;
}

const NativeEntry* NativeRegistry::find(std::string_view cls, std::string_view fn) const {
  auto it = index_.find(fold(std::string(cls) + "." + std::string(fn)));
  return it == index_.end() ? nullptr : &entries_[it->second];
}

void NativeRegistry::bind(std::string_view key, NativeFn fn, void* user, Tier tier) {
  auto it = index_.find(fold(key));
  if (it == index_.end()) throw std::logic_error("bind: unknown native " + std::string(key));
  NativeEntry& e = entries_[it->second];
  e.fn = fn;
  e.user = user;
  e.tier = tier;
}

void NativeRegistry::add(NativeEntry e) {
  if (e.key.empty()) e.key = fold(e.cls + "." + e.name);
  auto it = index_.find(e.key);
  if (it != index_.end()) {
    entries_[it->second] = std::move(e);
    return;
  }
  index_[e.key] = entries_.size();
  entries_.push_back(std::move(e));
}

Variant NativeRegistry::stub_return(const NativeEntry& e, Heap& heap, NativeCall& call) {
  if (e.default_ret) return *e.default_ret;
  switch (e.ret) {
    case RetKind::Int: return make_int(0);
    case RetKind::Float: return make_float_bits(0);
    case RetKind::String: return Variant{kString, heap.new_string("")};
    case RetKind::Colour: return make_colour(0);
    case RetKind::Game: return Variant{e.ret_type, heap.new_game(e.ret_type)};
    case RetKind::Copy: return call.args.empty() ? nothing() : heap.duplicate(*call.args[0]);
    default: return nothing();
  }
}

}  // namespace lomn::osi
