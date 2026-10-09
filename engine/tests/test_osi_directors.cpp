// M2a-2 T8a: the "directors, saver and Beta 1.5.0 mods" native group (engine/src/osi/natives/directors.cpp), one asm-pin
// test and one behaviour test per native, written before the bindings (RED, then GREEN). Every pin reads the canonical
// exe (or BetterSaver.dll) as DATA through $LOMN_DATA; without it the pin tests report SKIPPED (no LOMN_DATA) and pass.
// The bytes below were read with `objdump -d --x86-asm-syntax=intel` of LEGOBionicle.exe and BetterSaver.dll on
// 2026-10-09; each pin names its instruction address.
#include <algorithm>
#include <array>
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
#include <utility>
#include <vector>

#include "doctest.h"
#include "lomn/fs.h"
#include "lomn/osi/directors.h"
#include "lomn/osi/heap.h"
#include "lomn/osi/host.h"
#include "lomn/osi/json.h"
#include "lomn/osi/natives.h"
#include "lomn/osi/registry.h"
#include "lomn/osi/services.h"
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
    import_rva = le32(pe + 24 + 104);
    for (size_t i = 0; i < n; ++i) {
      const size_t o = pe + 24 + opt + i * 40;
      if (o + 40 > d.size()) return false;
      secs.push_back({le32(o + 12), le32(o + 8), le32(o + 16), le32(o + 20)});
    }
    return true;
  }
  uint32_t import_rva = 0;
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
  // The imported name behind an IAT slot (absolute address): walks the import directory (FirstThunk arrays).
  std::string import_name(uint32_t slot) const {
    for (uint32_t desc = base + import_rva;; desc += 20) {
      const uint32_t oft = u32(desc), name = u32(desc + 12), ft = u32(desc + 16);
      if (name == 0) return "";
      for (uint32_t i = 0;; ++i) {
        const uint32_t thunk = u32(base + (oft ? oft : ft) + 4 * i);
        if (thunk == 0) break;
        if (base + ft + 4 * i == slot) return cstr(base + name) + "!" + ((thunk & 0x80000000u) ? "#" : cstr(base + thunk + 2));
      }
    }
  }
};

const Pe* image(const char* rel) {
  static std::map<std::string, std::unique_ptr<Pe>> cache;
  const char* ld = std::getenv("LOMN_DATA");
  if (!ld || !*ld) return nullptr;
  auto it = cache.find(rel);
  if (it == cache.end()) {
    auto im = std::make_unique<Pe>();
    if (!im->load(std::string(ld) + "/" + rel)) im.reset();
    it = cache.emplace(rel, std::move(im)).first;
  }
  return it->second.get();
}
const Pe* exe() { return image("reference/LEGOBionicle.exe"); }
const Pe* bettersaver() { return image("install/mods/BetterSaver.dll"); }

#define SKIP_WITHOUT(img)              \
  if (!(img)) {                        \
    MESSAGE("SKIPPED (no LOMN_DATA)"); \
    return;                            \
  }
#define PIN(img, va, bytes) CHECK_MESSAGE((img)->hex((va), (sizeof(bytes) + 1) / 3) == bytes, "pin at " #va)

// ---------------------------------------------------------------------------------------------- the registry rows
const char* const kRows = R"([
 {"cls": "GcGame", "name": "GetMainView", "min": 0, "max": 0, "types": [], "ret": "game", "ret_class": "GcViewPort", "winner": true},
 {"cls": "GcGame", "name": "GetKeyMapper", "min": 0, "max": 0, "types": [], "ret": "game", "ret_class": "GcKeyMapper", "winner": true},
 {"cls": "GcGame", "name": "GetMP3Player", "min": 0, "max": 0, "types": [], "ret": "game", "ret_class": "GcMP3Player", "winner": true},
 {"cls": "GcViewPort", "name": "GetCamera", "min": 1, "max": 1, "types": ["type:GcViewPort"], "ret": "game", "ret_class": "GcLegoCamera", "winner": true},
 {"cls": "GcAreaDirector", "name": "GetCurrentLevel", "min": 0, "max": 0, "types": [], "ret": "int", "winner": true},
 {"cls": "GcAreaDirector", "name": "SetCurrentLevel", "min": 1, "max": 1, "types": ["string"], "ret": "none", "winner": true},
 {"cls": "GcAreaDirector", "name": "SetCurrentArea", "min": 1, "max": 1, "types": ["string"], "ret": "none", "winner": true},
 {"cls": "GcAreaDirector", "name": "GetScriptFluffString", "min": 3, "max": 3, "types": ["int", "int", "int"], "ret": "string", "winner": true},
 {"cls": "GcSaver", "name": "CheatMenu", "min": 0, "max": 0, "types": [], "ret": "int", "winner": true},
 {"cls": "GcSaver", "name": "GetAmmo", "min": 0, "max": 0, "types": [], "ret": "int", "winner": true},
 {"cls": "GcHudDirector", "name": "PlayCinematic", "min": 2, "max": 2, "types": ["string", "int"], "ret": "none", "winner": true},
 {"cls": "ScSLOSIFileIO", "name": "DoesFileExist", "min": 1, "max": 1, "types": ["string"], "ret": "int", "winner": true}
])";
const char* const kMods = R"([
 {"cls": "BetterSaver", "name": "GetBooleanValue", "min": 1, "max": 1, "types": ["int"], "ret": "int", "tier": "mod", "winner": true},
 {"cls": "BetterSaver", "name": "SetBooleanValue", "min": 2, "max": 2, "types": ["int", "int"], "ret": "none", "tier": "mod", "winner": true},
 {"cls": "BetterSaver", "name": "Save", "min": 0, "max": 0, "types": [], "ret": "none", "tier": "mod", "winner": true}
])";
// the type ids of data/osi/types.json (the A9 trace mints 0028 / 002a / 003e / 003f)
const char* const kTypes = R"({"types": [{"name": "type:GcViewPort", "id": 40}, {"name": "type:GcLegoCamera", "id": 42},
 {"name": "type:GcKeyMapper", "id": 62}, {"name": "type:GcMP3Player", "id": 63}]})";

struct Run {
  sage::OsiFile file;
  StringTrace tr, log;
  std::unique_ptr<ScriptHost> host;
  CallResult res;
  Run(Asm& a, const fs::FileSystem* vfs = nullptr, HostFlags flags = HostFlags{},
      std::function<void(ScriptHost&)> pre = {}) {
    std::string err;
    REQUIRE_MESSAGE(file.parse(a.build(), &err), err);
    ScriptHost::Options opt;
    opt.real_natives = true;
    opt.flags = flags;
    host = std::make_unique<ScriptHost>(file, kRows, kMods, kTypes, "", vfs, &tr, &log, opt);
    if (pre) pre(*host);
    res = host->setup();
  }
  HostContext& ctx() { return host->context(); }
  Variant value() {
    REQUIRE_MESSAGE(res.ok, (host->vm().last_error() ? host->vm().last_error()->message : std::string("?")));
    return res.result;
  }
  CallResult call(const char* fn) { return host->vm().call(fn, {}); }
  bool log_has(const std::string& p) const {
    for (const auto& l : log.lines)
      if (l.find(p) != std::string::npos) return true;
    return false;
  }
  size_t log_count(const std::string& prefix) const {
    size_t n = 0;
    for (const auto& l : log.lines) n += l.rfind(prefix, 0) == 0 ? 1 : 0;
    return n;
  }
  bool trace_has(const std::string& p) const {
    for (const auto& l : tr.lines)
      if (l.rfind(p, 0) == 0) return true;
    return false;
  }
};

uint32_t id4(const char* s) { return convert_string_to_id(std::string(s, 4)); }

// ---- byte builders: a BLKF container with stored entries, an SLB with its fix-up trailer ----
using Bytes = std::vector<uint8_t>;
void put32(Bytes& b, uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void set32(Bytes& b, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i) b[at + i] = static_cast<uint8_t>(v >> (8 * i));
}
Bytes blk(std::vector<std::pair<std::string, Bytes>> entries) {
  auto fold = [](std::string s) {
    for (char& c : s)
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c | 0x20);
    return s;
  };
  std::sort(entries.begin(), entries.end(), [&](const auto& a, const auto& b) { return fold(a.first) < fold(b.first); });
  Bytes b = {'B', 'L', 'K', 'F'};
  put32(b, 1);
  put32(b, static_cast<uint32_t>(entries.size()));
  uint32_t off = static_cast<uint32_t>(12 + 56 * entries.size());
  for (const auto& e : entries) {
    Bytes rec(56, 0);
    std::memcpy(rec.data(), e.first.data(), std::min<size_t>(e.first.size(), 39));
    set32(rec, 40, static_cast<uint32_t>(e.second.size()));
    set32(rec, 44, static_cast<uint32_t>(e.second.size()));
    set32(rec, 48, 2);  // stored, f = file offset
    set32(rec, 52, off);
    b.insert(b.end(), rec.begin(), rec.end());
    off += static_cast<uint32_t>(e.second.size());
  }
  for (const auto& e : entries) b.insert(b.end(), e.second.begin(), e.second.end());
  return b;
}
// data (padded to 4) + the pointer slots + count + 0x00C0FFEE (the layout GetSLKFile 0x4032e0 reads, 0x40342d-0x40346f)
Bytes slb(Bytes data, const std::vector<uint32_t>& ptr_fields) {
  while (data.size() % 4) data.push_back(0);
  for (size_t i = ptr_fields.size(); i-- > 0;) put32(data, ptr_fields[i]);
  put32(data, static_cast<uint32_t>(ptr_fields.size()));
  put32(data, 0x00C0FFEEu);
  return data;
}
// <area>_obj.slb: {slot, n, n2, ptr = 0x10} + 60-byte records {id, ..., flags +0x38}
Bytes obj_slb(const std::vector<std::pair<const char*, uint32_t>>& recs) {
  Bytes d;
  put32(d, 0);
  put32(d, static_cast<uint32_t>(recs.size()));
  put32(d, static_cast<uint32_t>(recs.size()));
  put32(d, 0x10);
  for (const auto& r : recs) {
    Bytes rec(60, 0);
    set32(rec, 0, id4(r.first));
    set32(rec, 0x38, r.second);
    d.insert(d.end(), rec.begin(), rec.end());
  }
  return slb(d, {0x0c});
}
// <area>_char.slb / <area>_hive.slb: {u32, n, ptr = 0xc} + records of `stride` bytes {id, ..., dword +0x2c (hives: 0 takes
// the GcHive ctor 0x6239b0 that never reaches the VM)}
Bytes id_slb(const std::vector<const char*>& ids, size_t stride, uint32_t at2c = 1) {
  Bytes d;
  put32(d, 0);
  put32(d, static_cast<uint32_t>(ids.size()));
  put32(d, 0x0c);
  for (const char* id : ids) {
    Bytes rec(stride, 0);
    set32(rec, 0, id4(id));
    if (stride > 0x2c + 4) set32(rec, 0x2c, at2c);
    d.insert(d.end(), rec.begin(), rec.end());
  }
  return slb(d, {0x08});
}

// A fluff string table as GcStringTableLoader reads it: {count, ptr entries}; entries {key0, key1, n, ptr strings};
// strings: n pointer slots, each to a length-prefixed string (InitStrings 0x5e8600 steps over the prefix byte).
struct FluffEntry {
  uint32_t k0, k1;
  std::vector<std::string> s;
  int share_with = -1;  // reuse an earlier entry's pointer array (its slots are then advanced twice)
};
Bytes fluff_slb(const std::vector<FluffEntry>& es) {
  Bytes d;
  std::vector<uint32_t> ptrs;
  put32(d, static_cast<uint32_t>(es.size()));
  put32(d, 8);
  ptrs.push_back(4);
  const size_t entries_at = d.size();
  d.resize(d.size() + 16 * es.size(), 0);
  std::vector<uint32_t> arrays(es.size(), 0);
  for (size_t i = 0; i < es.size(); ++i) {
    const FluffEntry& e = es[i];
    set32(d, entries_at + 16 * i + 0, e.k0);
    set32(d, entries_at + 16 * i + 4, e.k1);
    set32(d, entries_at + 16 * i + 8, static_cast<uint32_t>(e.share_with >= 0 ? es[e.share_with].s.size() : e.s.size()));
    ptrs.push_back(static_cast<uint32_t>(entries_at + 16 * i + 12));
    if (e.share_with >= 0) {
      set32(d, entries_at + 16 * i + 12, arrays[e.share_with]);
      continue;
    }
    while (d.size() % 4) d.push_back(0);  // the trailer masks a field to 4 (slb.h): slot arrays are aligned
    arrays[i] = static_cast<uint32_t>(d.size());
    set32(d, entries_at + 16 * i + 12, arrays[i]);
    const size_t slots = d.size();
    d.resize(d.size() + 4 * e.s.size(), 0);
    for (size_t j = 0; j < e.s.size(); ++j) {
      ptrs.push_back(static_cast<uint32_t>(slots + 4 * j));
      set32(d, slots + 4 * j, static_cast<uint32_t>(d.size()));
      d.push_back(static_cast<uint8_t>(e.s[j].size()));  // the length prefix (skipped)
      d.insert(d.end(), e.s[j].begin(), e.s[j].end());
      d.push_back(0);
    }
  }
  return slb(d, ptrs);
}

std::string slurp(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// An `onevent(a0..a9)` script function that records its first two arguments in globals; `bad` variants raise.
void add_onevent(Asm& a, bool raise = false) {
  a.fn("onevent", 10);
  if (raise) a.op(0x02);  // 0x02 with nothing to do raises (advances PC by 1 first, 0x6038a6)
  a.nothing().ret();
}

}  // namespace

// ============================== the group is listed (fix 1 I-1 rule) ==============================

TEST_CASE("osi directors: the group is in the explicit list and binds every tier1 native of the brief") {
  bool listed = false;
  for (const NativeGroup& g : native_groups()) listed |= std::string(g.name) == "directors";
  CHECK(listed);
  Asm a;
  a.fn("setup", 0).nothing().ret();
  Run r(a);
  for (const char* k : {"gcgame.getmainview", "gcgame.getkeymapper", "gcgame.getmp3player", "gcviewport.getcamera",
                        "gcareadirector.getcurrentlevel", "gcareadirector.setcurrentlevel", "gcareadirector.setcurrentarea",
                        "gcareadirector.getscriptfluffstring", "gcsaver.cheatmenu", "gcsaver.getammo",
                        "gchuddirector.playcinematic", "bettersaver.getbooleanvalue", "bettersaver.setbooleanvalue",
                        "bettersaver.save"}) {
    const std::string key(k);
    const size_t dot = key.find('.');
    const NativeEntry* e = r.host->registry().find(key.substr(0, dot), key.substr(dot + 1));
    REQUIRE_MESSAGE(e, key);
    CHECK_MESSAGE(e->fn != nullptr, key);
    CHECK_MESSAGE(e->tier == Tier::Implemented, key);
  }
}

// ============================== the singletons: getmainview / getkeymapper / getmp3player / getcamera ==============================

TEST_CASE("osi directors singletons asm pin: GetMainView 0x43ae20, GetKeyMapper 0x43ae90, GetMP3Player 0x43aea0, GetCamera 0x448080") {
  const Pe* im = exe();
  SKIP_WITHOUT(im);
  PIN(im, 0x4366e3, "e8 38 47 00 00");         // body 0x4366e0 -> GcGame::GetMainView 0x43ae20
  PIN(im, 0x4366ed, "e8 0e 00 00 00");         // -> CreateVariant(GcViewPort*) 0x436700
  PIN(im, 0x43ae26, "8b 15 b0 91 83 00");      // the main view's key [0x8391b0] ...
  PIN(im, 0x43ae32, "8b 0d 64 fa 70 00");      // ... looked up in the screen [0x70fa64] ...
  PIN(im, 0x43ae39, "e8 82 25 06 00");         // ... by ScScreen::GetViewPort 0x49d3c0
  PIN(im, 0x49d3ea, "8b 5e 08");               // GetViewPort: the view's key [vp+8] ...
  PIN(im, 0x49d3f2, "39 d3");                  // ... compared with the request; the view itself is returned (0x49d3f6)
  PIN(im, 0x4367f3, "e8 98 46 00 00");         // body 0x4367f0 -> GetKeyMapper 0x43ae90
  PIN(im, 0x4367fd, "e8 0e 00 00 00");         // -> CreateVariant(GcKeyMapper*) 0x436810
  PIN(im, 0x43ae90, "a1 98 aa 83 00 c3");      // mov eax, [0x83aa98] ; ret
  PIN(im, 0x436943, "e8 58 45 00 00");         // body 0x436940 -> GetMP3Player 0x43aea0
  PIN(im, 0x43694d, "e8 0e 00 00 00");         // -> CreateVariant(GcMP3Player*) 0x436960
  PIN(im, 0x43aea0, "a1 9c 87 83 00 c3");      // mov eax, [0x83879c] ; ret
  PIN(im, 0x4474e8, "e8 93 0b 00 00");         // body 0x4474e0 -> GcViewPort::GetCamera 0x448080 (this = w0, 0x4474e6)
  PIN(im, 0x4474f2, "e8 09 00 00 00");         // -> CreateVariant(GcLegoCamera*) 0x447500
  PIN(im, 0x44809e, "8b 7b 30");               // GetCamera: the camera node [vp+0x30] (no null test of vp) ...
  PIN(im, 0x4480aa, "ff 47 04");               // ... a temporary reference on the NODE ...
  PIN(im, 0x4480ca, "8b 7a 1c");               // ... the camera is [node+0x1c] ...
  PIN(im, 0x4480d6, "ff 49 04");               // ... and the node's temporary is released: no change on the camera
  // the overloads: the AddRef at +4 and the null route (natives.md §2.1)
  PIN(im, 0x436730, "ff 47 04");
  PIN(im, 0x436840, "ff 47 04");
  PIN(im, 0x436990, "ff 47 04");
  PIN(im, 0x447530, "ff 47 04");
  PIN(im, 0x436713, "66 c7 06 0f 00");
  PIN(im, 0x436973, "66 c7 06 0f 00");
  // the engine's holders (G 1): the key mapper and the MP3 player are global ScAutoRefPtrs, set before Setup
  PIN(im, 0x439583, "ff 47 04");               // SetUpControllers: +1 ...
  PIN(im, 0x4395a3, "89 3d 98 aa 83 00");      // ... into [0x83aa98]
  PIN(im, 0x43a3e8, "68 68 02 00 00");         // SetUpDatabase: new GcMP3Player (0x268 bytes, no condition) ...
  PIN(im, 0x43a40c, "ff 47 04");               // ... +1 ...
  PIN(im, 0x43a42c, "89 3d 9c 87 83 00");      // ... into [0x83879c]: getmp3player is never null on the path
  PIN(im, 0x43a945, "a3 bc 32 70 00");         // the camera pointer [0x7032bc] is a raw copy (no reference)
}

TEST_CASE("osi directors singletons: one handle per object while it lives; G 1; the script's last variant never frees it") {
  Asm a;
  a.fn("setup", 0);
  a.call("gcgame", "getmainview", 0).pop().call("gcgame", "getmainview", 0).pop();
  a.call("gcgame", "getkeymapper", 0).pop().call("gcgame", "getkeymapper", 0).pop();
  a.call("gcgame", "getmp3player", 0).pop().call("gcgame", "getmp3player", 0).pop();
  a.call("gcgame", "getmainview", 0).call("gcviewport", "getcamera", 1).pop();
  a.call("gcgame", "getmainview", 0).call("gcviewport", "getcamera", 1).ret();
  Run r(a);
  CHECK(r.value() == Variant{42, 4});  // GcLegoCamera, the 4th handle minted
  CHECK(r.log_has("gcgame.getmainview 0 -> 0028:00000001 G 1"));
  CHECK(r.log_count("00000001 gcgame.getmainview 0 -> 0028:00000001 G 1") == 1);  // the second call: the same handle
  CHECK(r.log_has("00000002 gcgame.getkeymapper 0 -> 003e:00000002 G 1"));
  CHECK(r.log_has("00000003 gcgame.getkeymapper 0 -> 003e:00000002 G 1"));
  CHECK(r.log_has("00000004 gcgame.getmp3player 0 -> 003f:00000003 G 1"));
  CHECK(r.log_has("00000005 gcgame.getmp3player 0 -> 003f:00000003 G 1"));
  CHECK(r.log_has("00000007 gcviewport.getcamera 1 -> 002a:00000004 G 1"));
  CHECK(r.log_has("00000009 gcviewport.getcamera 1 -> 002a:00000004 G 1"));
  CHECK(r.trace_has("A g 00000001 28"));
  CHECK_FALSE(r.trace_has("A g 00000005"));  // no fresh handle per call (a stub would mint one each time)
  CHECK_FALSE(r.trace_has("F g"));           // the engine's holder keeps every one (total 1 after the script lets go)
  for (uint32_t h = 1; h <= 4; ++h) {
    CHECK(r.ctx().object(h) != nullptr);
    CHECK(r.ctx().heap().engine_refs(h) == 1);
  }
  CHECK(r.ctx().check_engine_refs().empty());
  for (const auto& l : r.log.lines) CHECK(l.rfind("H ", 0) != 0);  // no engine reference changes on these objects
  // the camera is the main view's: the object behind both getcamera handles
  GcViewPort* vp = dynamic_cast<GcViewPort*>(r.ctx().object(1));
  REQUIRE(vp);
  CHECK(vp->camera == r.ctx().object(4));
  CHECK(dynamic_cast<GcKeyMapper*>(r.ctx().object(2)) != nullptr);
  CHECK(dynamic_cast<GcMP3Player*>(r.ctx().object(3)) != nullptr);
}

TEST_CASE("osi directors getcamera: nothing as the view is a VM error (the original reads [this+0x30] of null, 0x44809e)") {
  Asm a;
  a.fn("setup", 0).nothing().call("gcviewport", "getcamera", 1).ret();
  Run r(a);
  CHECK_FALSE(r.res.ok);
}

// ============================== getcurrentlevel / setcurrentlevel ==============================

TEST_CASE("osi directors getcurrentlevel/setcurrentlevel asm pin: [0x83902c], the RemoveArea guard, the level block header") {
  const Pe* im = exe();
  SKIP_WITHOUT(im);
  PIN(im, 0x486c46, "8b 15 2c 90 83 00");      // getcurrentlevel: mov edx, [0x83902c] ...
  PIN(im, 0x486c5c, "e8 8f 11 f8 ff");         // ... CreateVariant(ScIdentifier) 0x407df0 ...
  PIN(im, 0x407dfb, "66 c7 03 10 00");         // ... = {0x10, the dword}
  PIN(im, 0x407df9, "8b 12");
  PIN(im, 0x48db4b, "c7 05 2c 90 83 00 3f 3f 3f 3f");  // the static init: the null id (before Setup, INFERRED)
  CHECK(im->u32(0x70b3bc) == kNullId);         // the null ids RemoveArea's guard compares with
  CHECK(im->u32(0x70b3c0) == kNullId);
  PIN(im, 0x486c1b, "e8 40 2e 00 00");         // body 0x486c10 -> GcAreaDirector::SetCurrentLevel 0x489a60 (w0: the C string)
  PIN(im, 0x489a96, "e8 75 dd 18 00");         // the name -> StringToIdentifier 0x617810 (as scslosifoundation's)
  PIN(im, 0x489aba, "39 15 bc b3 70 00");      // cmp [0x70b3bc], edx (the level) ...
  PIN(im, 0x489ac8, "39 15 c0 b3 70 00");      // ... cmp [0x70b3c0], edx (the area) ...
  PIN(im, 0x489add, "e8 7e fa ff ff");         // ... both set: RemoveArea 0x489560
  PIN(im, 0x489af6, "89 15 2c 90 83 00");      // the store [0x83902c] = id (the brief's "table walk" 0x489ab4-0x489af6 is this guard)
  PIN(im, 0x489b7f, "b9 38 92 83 00 e8 e7 63 18 00");  // Close the level header 0x839238 ...
  PIN(im, 0x489b89, "b9 dc ae 83 00 e8 dd 63 18 00");  // ... and the area header 0x83aedc
  PIN(im, 0x489b93, "6a 00 e8 76 21 00 00");   // LoadLevelBlockHeader(0) ...
  PIN(im, 0x48be10, "68 38 92 83 00 e8 06 1d 0c 00");  // ... arg 0: SrSetCurBlockFile(0x839238) (the CURRENT slot)
  PIN(im, 0x48be01, "68 38 92 83 00 e8 35 1d 0c 00");  // ... arg 1: SrSetCurSecondBlockFile
  PIN(im, 0x48bd1f, "e8 cc 42 18 00");         // IsHeaderLoaded: a loaded header is not reopened (0x48bd26 jne)
  CHECK(im->cstr(0x70b7ec) == "Root/Data/Levels/%.4s/%.4s.blk");
  PIN(im, 0x54db24, "89 15 28 61 73 00");      // SrSetCurBlockFile: [0x736128] = arg
  PIN(im, 0x60ffdb, "c7 46 08 00 00 00 00");   // Close: the entry count = 0 ...
  PIN(im, 0x60fedb, "83 7f 08 00");            // ... so Find on a closed header gives -1 (0x60fee1)
  PIN(im, 0x48a25b, "e8 d0 38 0c 00");         // the language load saves the current slot ...
  PIN(im, 0x48a449, "e8 d2 36 0c 00");         // ... and restores it
  PIN(im, 0x48a455, "0f b6 40 03");            // GcSaver::SetLevel(name[3] - '0') ...
  PIN(im, 0x48a459, "05 d0 ff ff ff");
  PIN(im, 0x5d5744, "88 15 8c 74 74 00");      // ... byte [0x74748c]
  // the model starts where the exe does
  Asm a;
  a.fn("setup", 0).nothing().ret();
  Run r(a);
  CHECK(r.ctx().subsystem<AreaDirector>().current_level() == im->u32(0x70b3bc));
}

TEST_CASE("osi directors getcurrentlevel: the null id until set; set -> get round trip; StringToIdentifier's rule") {
  Asm a;
  a.fn("setup", 0).call("gcareadirector", "getcurrentlevel", 0).ret();
  a.fn("set_lev0", 0).s("lev0").call("gcareadirector", "setcurrentlevel", 1).ret();
  a.fn("set_x", 0).s("x").call("gcareadirector", "setcurrentlevel", 1).ret();
  a.fn("get", 0).call("gcareadirector", "getcurrentlevel", 0).ret();
  Run r(a);
  CHECK(r.value() == make_int(static_cast<int32_t>(kNullId)));
  CHECK(r.log_has("gcareadirector.getcurrentlevel 0 -> 0010:3f3f3f3f"));
  REQUIRE(r.call("set_lev0").ok);
  CHECK(r.call("get").result == make_int(0x6c657630));  // 'lev0'
  REQUIRE(r.call("set_x").ok);
  CHECK(r.call("get").result == make_int(-1));  // length != 4 -> 0xffffffff
  CHECK(r.ctx().subsystem<SaverState>().level == static_cast<uint8_t>(0 - 0x30));  // "x": the byte at [3] is past the NUL (INFERRED 0)
  REQUIRE(r.call("set_lev0").ok);
  CHECK(r.ctx().subsystem<SaverState>().level == 0);  // '0' - '0'
  for (const auto& l : r.log.lines) CHECK(l.rfind("C ", 0) != 0);  // no engine call: no area was set (guard 0x489ac8)
}

TEST_CASE("osi directors setcurrentlevel: the level block file becomes the CURRENT slot (set even when it fails to open)") {
  fs::MemoryFileSystem mem;
  REQUIRE(mem.mount("Root/Data/Art/Art.blk", blk({{"art.tga", {1}}})));
  REQUIRE(mem.mount("Root/Data/Levels/lev0/lev0.blk", blk({{"inlev.txt", {1}}})));
  REQUIRE(mem.mount("Root/Data/loose.txt", {'x'}));
  Asm a;
  a.fn("setup", 0).nothing().ret();
  a.fn("lev", 0).s("lev0").call("gcareadirector", "setcurrentlevel", 1).ret();
  a.fn("nolev", 0).s("zzzz").call("gcareadirector", "setcurrentlevel", 1).ret();
  a.fn("exists_lev", 0).s("x/inlev.txt").call("scslosifileio", "doesfileexist", 1).ret();
  a.fn("exists_loose", 0).s("Root/Data/loose.txt").call("scslosifileio", "doesfileexist", 1).ret();
  Run r(a, &mem);
  CHECK(r.call("exists_loose").result == make_int(1));  // no slot set: FileExists
  REQUIRE(r.call("lev").ok);
  sage::BlockChain* chain = r.ctx().block_chain();
  REQUIRE(chain);
  CHECK(chain->path(sage::Source::Current) == "Root/Data/Levels/lev0/lev0.blk");
  CHECK(chain->blockfile(sage::Source::Second) == nullptr);
  CHECK(r.call("exists_lev").result == make_int(1));
  CHECK(r.call("exists_loose").result == make_int(0));  // [0x736128] set: the FileExists fallback is suppressed (0x54ddf7)
  REQUIRE(r.call("nolev").ok);                          // zzzz.blk does not exist: the slot still points at 0x839238
  CHECK(chain->blockfile(sage::Source::Current) != nullptr);
  CHECK(r.call("exists_lev").result == make_int(0));    // closed header: Find -1
  CHECK(r.call("exists_loose").result == make_int(0));  // still no fallback
  CHECK_FALSE(r.ctx().subsystem<AreaDirector>().level_header().loaded);
}

// ============================== setcurrentarea (the LoadArea chain, D16) ==============================

TEST_CASE("osi directors setcurrentarea asm pin: SetCurrentArea 0x48a540, LoadArea's blocks, SetUnderWater, CleanupLoad") {
  const Pe* im = exe();
  SKIP_WITHOUT(im);
  PIN(im, 0x486efb, "e8 40 36 00 00");         // body 0x486ef0 -> GcAreaDirector::SetCurrentArea 0x48a540
  PIN(im, 0x48a55d, "e8 ae d2 18 00");         // the name -> StringToIdentifier 0x617810 ...
  PIN(im, 0x48a57f, "89 15 b4 b0 83 00");      // ... [0x83b0b4] = id ...
  PIN(im, 0x48a585, "6a 01 53 e8 63 ed ff ff");  // ... GcAreaDirector::LoadArea(name, 1) 0x4892f0 ...
  PIN(im, 0x48a593, "e8 68 af 14 00");         // ... then GcSaver::SetCurrentArea(id) 0x5d5500
  PIN(im, 0x5d5506, "89 15 90 74 74 00");      // [0x747490] = id ...
  PIN(im, 0x5d550c, "e8 bf f0 ff ff");         // ... CleanupLoad 0x5d45d0 (BetterSaver replaces it: 0x10001ae0)
  PIN(im, 0x489305, "c7 05 88 2d 71 00 00 00 00 00");  // GcAreaDirector::LoadArea: the real-time frame count = 0
  PIN(im, 0x48935a, "e8 71 5a 07 00");         // -> GcAreaLoader::LoadArea 0x4fedd0
  PIN(im, 0x4ff62c, "6a 01 e8 dd c6 f8 ff");   // LoadLevelBlockHeader(1): the level header in the SECOND slot ...
  PIN(im, 0x4ff63b, "e8 d0 c5 f8 ff");         // ... LoadAreaBlockHeader 0x48bc10 ...
  PIN(im, 0x48bcfb, "68 dc ae 83 00 e8 1b 1e 0c 00");  // ... the area header in the CURRENT slot (0x48bd00)
  PIN(im, 0x48bc1f, "e8 cc 43 18 00");         // ... and a loaded area header is not reopened (0x48bc26 jne)
  CHECK(im->cstr(0x72990f).find("_obj.slb") != std::string::npos);
  CHECK(im->cstr(0x7299b5) == "Root/Data/Levels/%.4s/%.4s/%.4s_char.slb");
  CHECK(im->cstr(0x729a88) == "Root/Data/Levels/%.4s/%.4s/%.4s_hive.slb");
  // the three swallowing blocks (natives.md §5.2) and their guards
  PIN(im, 0x4ff8f2, "83 bd f0 fc ff ff 00");   // objects: the SLK object non-null (natives.md §5.2 lists no guard)
  PIN(im, 0x4ff93e, "e8 1d 03 04 00");         // LoadObjects 0x53fc60
  PIN(im, 0x5002ec, "e8 ff f0 03 00");         // LoadCharacters 0x53f3f0
  PIN(im, 0x500c0b, "e8 b0 55 12 00");         // LoadHives 0x6261c0
  PIN(im, 0x4033b2, "e8 39 5e 0d 00");         // a missing SLK: SrThrow 0x4d91f0 inside the block's try: the block is skipped
  // the loaders' tables: objects {n2 at +8, ptr +0xc} x 0x3c, characters {n +4, ptr +8} x 0x34, hives {n +4, ptr +8} x 0x3c
  PIN(im, 0x5406fd, "83 85 d0 fd ff ff 3c");
  PIN(im, 0x54070a, "8b 90 a0 00 00 00");      // [obj+0xa0] = data+8 (GetSLKFile puts the data at +0x98, 0x4033e2)
  PIN(im, 0x540713, "03 90 a4 00 00 00");
  PIN(im, 0x53f53c, "83 45 ac 34");
  PIN(im, 0x53f543, "8b 90 9c 00 00 00");
  PIN(im, 0x626408, "83 c7 3c");
  PIN(im, 0x626411, "8b 90 9c 00 00 00");
  PIN(im, 0x4033e2, "05 98 00 00 00");
  // objects: only flag 0x20 reaches a moveable (0x10 set: GcAreaDirector::AddMoveableObj; clear: GcNoLightDirector's)
  PIN(im, 0x53fe03, "25 20 00 00 00");
  PIN(im, 0x5403a9, "25 20 00 00 00");
  PIN(im, 0x53fe36, "8b 10");                  // the record id (+0) passed on (INFERRED: it becomes the motion id)
  // GcMotionSystem::SetupMoveable: SetUnderWater only for the main character
  PIN(im, 0x574ee5, "e8 a6 60 ec ff");         // MainCharacterID 0x43af90 ...
  PIN(im, 0x574eeb, "8b 4e 14 8b 55 ec 39 d1 75 0d");  // ... == [ms+0x14] ...
  PIN(im, 0x574ef5, "a0 a8 c5 73 00");         // ... the flag byte [0x73c5a8] ...
  PIN(im, 0x574efd, "e8 4e 41 01 00");         // ... SetUnderWater 0x589050
  CHECK(im->u8(0x73c5a8) == 0);
  PIN(im, 0x43af9e, "8b 15 f4 96 77 00");      // MainCharacterID: [0x7796f4] while [0x7032b8] is 0 ...
  PIN(im, 0x43c193, "c7 05 f4 96 77 00 31 74 6e 63");  // ... = 'cnt1'
  CHECK(im->u32(0x7032b8) == 0);
  // SetUnderWater: Call("OnEvent", byte 0x36, int b) on both branches
  PIN(im, 0x58905c, "80 7d 08 00");
  PIN(im, 0x58907b, "6a 36");
  PIN(im, 0x58908b, "6a 01");
  PIN(im, 0x589194, "6a 36");
  PIN(im, 0x5891a1, "6a 00");
  PIN(im, 0x58914f, "68 20 df 73 00 e8 c7 21 08 00");
  PIN(im, 0x589244, "68 20 df 73 00 e8 d2 20 08 00");
  CHECK(im->cstr(0x73df20) == "OnEvent");
  PIN(im, 0x413ed5, "66 c7 03 10 00");         // CreateVariant(byte) 0x413ed0: {0x10, v}
  // BetterSaver's CleanupLoad: the token vector and the exe lists, all empty without pickups
  const Pe* bs = bettersaver();
  REQUIRE(bs);
  PIN(bs, 0x10001ae7, "b8 10 a8 48 00");       // GetAreaLoader 0x48a810 (null: nothing)
  PIN(bs, 0x10001b0d, "c1 fa 03");             // the token vector's size (8-byte elements) ...
  PIN(bs, 0x10001b38, "b8 c0 1d 50 00");       // ... RemoveObject 0x501dc0 per match
  CHECK(bs->u32(0x1000a054) == 0x83b401);      // the exe list counts (ResetData zeroes them, 0x5d54ab / 0x5d54b2)
  CHECK(bs->u32(0x1000a050) == 0x83b3fa);
}

namespace {
// the frnt-like fixture of the A9 path: lev0.blk + frnt.blk (obj records with no 0x20 flag, no char or hive file)
void mount_area(fs::MemoryFileSystem& mem, std::vector<std::pair<std::string, Bytes>> area_entries,
                const std::string& area = "frnt") {
  REQUIRE(mem.mount("Root/Data/Art/Art.blk", blk({{"art.tga", {1}}})));
  REQUIRE(mem.mount("Root/Data/Levels/lev0/lev0.blk", blk({{"inlev.txt", {1}}})));
  REQUIRE(mem.mount("Root/Data/Levels/lev0/" + area + ".blk", blk(std::move(area_entries))));
}
void area_script(Asm& a, const std::string& area = "frnt", bool raise = false) {
  a.fn("setup", 0).s("lev0").call("gcareadirector", "setcurrentlevel", 1).pop();
  a.s(area).call("gcareadirector", "setcurrentarea", 1).ret();
  add_onevent(a, raise);
}
const char* const kOnEvent36_0 =
    "OnEvent 0010:00000036 0010:00000000 000f:00000000 000f:00000000 000f:00000000 000f:00000000 000f:00000000 "
    "000f:00000000 000f:00000000 000f:00000000";
}  // namespace

TEST_CASE("osi directors setcurrentarea: the A9 shape (frnt: no 0x20 object, no character or hive file) makes no engine call") {
  fs::MemoryFileSystem mem;
  mount_area(mem, {{"frnt_obj.slb", obj_slb({{"sky1", 0x110}, {"tre1", 0x08}, {"plnt", 0x08}, {"main", 0x10},
                                             {"watr", 0x110}, {"lava", 0x10}})},
                   {"inarea.txt", {1}}});
  Asm a;
  area_script(a);
  a.fn("ex_area", 0).s("x/inarea.txt").call("scslosifileio", "doesfileexist", 1).ret();
  a.fn("ex_lev", 0).s("x/inlev.txt").call("scslosifileio", "doesfileexist", 1).ret();
  Run r(a, &mem, HostFlags{}, [](ScriptHost& h) { h.context().subsystem<RealDeltaTime>().count = 7; });
  REQUIRE(r.res.ok);
  for (const auto& l : r.log.lines) CHECK(l.rfind("C ", 0) != 0);
  AreaDirector& ad = r.ctx().subsystem<AreaDirector>();
  CHECK(ad.current_area() == id4("frnt"));
  CHECK(ad.block_stats(AreaDirector::kObjects).runs == 1);
  CHECK(ad.block_stats(AreaDirector::kObjects).nested_calls == 0);
  CHECK(ad.block_stats(AreaDirector::kCharacters).runs == 0);  // no frnt_char.slb: skipped
  CHECK(ad.block_stats(AreaDirector::kHives).runs == 0);
  CHECK(r.ctx().subsystem<SaverState>().area == id4("frnt"));  // GcSaver::SetCurrentArea 0x5d5506
  CHECK(r.ctx().subsystem<RealDeltaTime>().count == 0);       // 0x489305
  sage::BlockChain* chain = r.ctx().block_chain();
  REQUIRE(chain);
  CHECK(chain->path(sage::Source::Current) == "Root/Data/Levels/lev0/frnt.blk");
  CHECK(chain->path(sage::Source::Second) == "Root/Data/Levels/lev0/lev0.blk");
  CHECK(r.call("ex_area").result == make_int(1));
  CHECK(r.call("ex_lev").result == make_int(1));  // the second slot after a current-slot miss
}

namespace {
void main_is(ScriptHost& h, const char* id) { h.context().subsystem<GameGlobals>().main_character_id = id4(id); }
}  // namespace

TEST_CASE("osi directors setcurrentarea asm pin: the GcMoveableObj 'lbld' guard and the hive +0x2c branch (review C-1)") {
  const Pe* im = exe();
  SKIP_WITHOUT(im);
  CHECK(im->u32(0x734f10) == id4("lbld"));  // .data bytes 64 6c 62 6c
  CHECK(kLbld == id4("lbld"));
  PIN(im, 0x54332a, "8b 16 89 93 80 00 00 00");  // the ctor stores its id at [this+0x80] ...
  PIN(im, 0x54405f, "8b 92 80 00 00 00");        // ... and compares it with [0x734f10] ...
  PIN(im, 0x544065, "39 15 10 4f 73 00 75 66");  // ... jne 0x5440d3 past ...
  PIN(im, 0x54408b, "e8 70 1b 00 00");           // ... GcMoveableObj::SetupMoveable 0x545c00
  PIN(im, 0x489937, "e8 64 99 0b 00");           // AddMoveableObj -> the ctor 0x5432a0
  PIN(im, 0x6236d4, "e8 c7 fb f1 ff");           // GcHive::GcHive 0x6236a0 -> the ctor 0x5432a0 ...
  PIN(im, 0x6236bf, "8b 10 89 55 d0");           // ... with the record id
  PIN(im, 0x62623c, "83 7f 2c 00 74 7e");        // LoadHives: [rec+0x2c] == 0 ...
  PIN(im, 0x6262fa, "e8 b1 d6 ff ff");           // ... -> GcHive::GcHive 0x6239b0 (no VM)
  PIN(im, 0x501830, "e8 eb da f7 ff");           // (I-2, deferred) CleanUpArea -> GcModelDirector::Remove 0x47f320
}

TEST_CASE("osi directors setcurrentarea: objects reach OnEvent(0x36, b) only for an 'lbld' record when 'lbld' is the main character") {
  for (uint32_t flags : {0x30u, 0x20u}) {  // AddMoveableObj (0x10 set) and GcNoLightDirector's (0x10 clear)
    fs::MemoryFileSystem mem;
    mount_area(mem, {{"frnt_obj.slb", obj_slb({{"sky1", 0x110}, {"lbld", flags}, {"cnt2", 0x30}})}});
    Asm a;
    area_script(a);
    Run r(a, &mem, HostFlags{}, [](ScriptHost& h) { main_is(h, "lbld"); });
    REQUIRE(r.res.ok);
    CHECK(r.log_has(std::string("C 00000001 ") + kOnEvent36_0));  // setcurrentarea is native seq 1
    CHECK(r.log_count("C ") == 1);
    CHECK(r.ctx().subsystem<AreaDirector>().block_stats(AreaDirector::kObjects).nested_calls == 1);
  }
  {
    fs::MemoryFileSystem mem;  // the main character 'cnt1' as a 0x20 object: the ctor's 'lbld' test fails, no call
    mount_area(mem, {{"frnt_obj.slb", obj_slb({{"cnt1", 0x20}, {"cnt1", 0x30}})}, {"frnt_hive.slb", id_slb({"cnt1"}, 0x3c)}});
    Asm a;
    area_script(a);
    Run r(a, &mem);
    REQUIRE(r.res.ok);
    CHECK(r.log_count("C ") == 0);
    CHECK(r.ctx().subsystem<AreaDirector>().block_stats(AreaDirector::kObjects).runs == 1);
    CHECK(r.ctx().subsystem<AreaDirector>().block_stats(AreaDirector::kHives).runs == 1);
  }
  {
    fs::MemoryFileSystem mem;  // 'lbld' records while the main character is 'cnt1': the 0x574ef1 guard fails
    mount_area(mem, {{"frnt_obj.slb", obj_slb({{"lbld", 0x20}})}, {"frnt_hive.slb", id_slb({"lbld"}, 0x3c)}});
    Asm a;
    area_script(a);
    Run r(a, &mem);
    REQUIRE(r.res.ok);
    CHECK(r.log_count("C ") == 0);
  }
  {
    fs::MemoryFileSystem mem;  // 0x10 or 0x08 without 0x20: not a moveable
    mount_area(mem, {{"frnt_obj.slb", obj_slb({{"lbld", 0x10}, {"lbld", 0x108}})}});
    Asm a;
    area_script(a);
    Run r(a, &mem, HostFlags{}, [](ScriptHost& h) { main_is(h, "lbld"); });
    REQUIRE(r.res.ok);
    CHECK(r.log_count("C ") == 0);
  }
  {
    fs::MemoryFileSystem mem;  // the flag byte [0x73c5a8] set: the other branch, (0x36, 1)
    mount_area(mem, {{"frnt_obj.slb", obj_slb({{"lbld", 0x20}})}});
    Asm a;
    area_script(a);
    Run r(a, &mem, HostFlags{}, [](ScriptHost& h) {
      main_is(h, "lbld");
      h.context().subsystem<GameGlobals>().global_under_water = true;
    });
    REQUIRE(r.res.ok);
    CHECK(r.log_has("C 00000001 OnEvent 0010:00000036 0010:00000001 "));
  }
}

TEST_CASE("osi directors setcurrentarea: characters fire for the main character id; hives need +0x2c != 0") {
  {
    fs::MemoryFileSystem mem;  // characters have no 'lbld' guard: 'cnt1' fires, 'tahu' does not
    mount_area(mem, {{"frnt_char.slb", id_slb({"tahu", "cnt1"}, 0x34)}});
    Asm a;
    area_script(a);
    Run r(a, &mem);
    REQUIRE(r.res.ok);
    CHECK(r.log_count("C 00000001 OnEvent") == 1);
    CHECK(r.ctx().subsystem<AreaDirector>().block_stats(AreaDirector::kCharacters).nested_calls == 1);
  }
  {
    fs::MemoryFileSystem mem;  // another main character id (the model-director group sets it after AddToa)
    mount_area(mem, {{"frnt_char.slb", id_slb({"cnt1", "onua"}, 0x34)}});
    Asm a;
    area_script(a);
    Run r(a, &mem, HostFlags{}, [](ScriptHost& h) { main_is(h, "onua"); });
    REQUIRE(r.res.ok);
    CHECK(r.log_count("C ") == 1);
  }
  for (uint32_t at2c : {0u, 1u}) {  // an 'lbld' hive while 'lbld' is the main character: only +0x2c != 0 fires
    fs::MemoryFileSystem mem;
    mount_area(mem, {{"frnt_hive.slb", id_slb({"lbld"}, 0x3c, at2c)}});
    Asm a;
    area_script(a);
    Run r(a, &mem, HostFlags{}, [](ScriptHost& h) { main_is(h, "lbld"); });
    REQUIRE(r.res.ok);
    CHECK(r.log_count("C ") == (at2c ? 1u : 0u));
  }
}

TEST_CASE("osi directors setcurrentarea: the order objects -> characters -> hives") {
  fs::MemoryFileSystem mem;
  mount_area(mem, {{"frnt_obj.slb", obj_slb({{"lbld", 0x20}})},
                   {"frnt_char.slb", id_slb({"tahu", "lbld"}, 0x34)},
                   {"frnt_hive.slb", id_slb({"lbld", "hive"}, 0x3c)}});
  Asm a;
  a.fn("setup", 0).s("lev0").call("gcareadirector", "setcurrentlevel", 1).pop();
  a.s("frnt").call("gcareadirector", "setcurrentarea", 1).ret();
  a.fn("onevent", 10).call("gcsaver", "getammo", 0).pop().nothing().ret();  // the probe below runs inside each OnEvent
  std::vector<int> order;
  Run r(a, &mem, HostFlags{}, [&](ScriptHost& h) {
    main_is(h, "lbld");
    static std::vector<int>* out = nullptr;
    static HostContext* ctx = nullptr;
    static std::array<uint64_t, 3> seen{};
    out = &order;
    ctx = &h.context();
    seen = {};
    h.registry().bind("gcsaver.getammo", [](NativeCall&, void*) {
      AreaDirector& ad = ctx->subsystem<AreaDirector>();
      for (int b = 0; b < 3; ++b) {
        const uint64_t n = ad.block_stats(static_cast<AreaDirector::Block>(b)).nested_calls;
        if (n != seen[static_cast<size_t>(b)]) out->push_back(b);
        seen[static_cast<size_t>(b)] = n;
      }
      return make_int(0);
    }, &h.context(), Tier::Implemented);
  });
  REQUIRE(r.res.ok);
  CHECK(r.log_count("C 00000001 OnEvent") == 3);
  CHECK(order == std::vector<int>{AreaDirector::kObjects, AreaDirector::kCharacters, AreaDirector::kHives});
  AreaDirector& ad = r.ctx().subsystem<AreaDirector>();
  for (auto b : {AreaDirector::kObjects, AreaDirector::kCharacters, AreaDirector::kHives}) {
    CHECK(ad.block_stats(b).runs == 1);
    CHECK(ad.block_stats(b).nested_calls == 1);
    CHECK(ad.block_stats(b).errors == 0);
  }
}

TEST_CASE("osi directors setcurrentarea D16: a raising OnEvent is swallowed in its block (X, E); the block stops, the next runs") {
  fs::MemoryFileSystem mem;
  mount_area(mem, {{"frnt_obj.slb", obj_slb({{"lbld", 0x20}, {"lbld", 0x20}})},
                   {"frnt_char.slb", id_slb({"lbld", "lbld"}, 0x34)}});
  Asm a;
  area_script(a, "frnt", /*raise=*/true);
  Run r(a, &mem, HostFlags{}, [](ScriptHost& h) { main_is(h, "lbld"); });
  // the swallowed error leaves the VM as at the throw (D16 rule 1); the outermost call continues at the error PC and ends
  // with that inner function's value, so Setup still returns ok
  REQUIRE(r.res.ok);
  CHECK(r.log_count("X 00000001") == 2);  // one per block: each block's second record never runs
  CHECK(r.log_count("C 00000001 OnEvent") == 2);
  AreaDirector& ad = r.ctx().subsystem<AreaDirector>();
  CHECK(ad.block_stats(AreaDirector::kObjects).nested_calls == 1);
  CHECK(ad.block_stats(AreaDirector::kObjects).errors == 1);
  CHECK(ad.block_stats(AreaDirector::kCharacters).nested_calls == 1);
  CHECK(ad.block_stats(AreaDirector::kCharacters).errors == 1);
  CHECK(r.trace_has("E "));
}

TEST_CASE("osi directors setcurrentarea: a loaded area header is kept; setcurrentlevel then removes the area") {
  fs::MemoryFileSystem mem;
  mount_area(mem, {{"frnt_obj.slb", obj_slb({})}, {"infrnt.txt", {1}}});
  REQUIRE(mem.mount("Root/Data/Levels/lev0/abcd.blk", blk({{"inabcd.txt", {1}}})));
  REQUIRE(mem.mount("Root/Data/Levels/lev1/lev1.blk", blk({{"inlev1.txt", {1}}})));
  Asm a;
  area_script(a);
  a.fn("abcd", 0).s("abcd").call("gcareadirector", "setcurrentarea", 1).ret();
  a.fn("lev1", 0).s("lev1").call("gcareadirector", "setcurrentlevel", 1).ret();
  a.fn("ex", 1).op(0x21).u16(0).call("scslosifileio", "doesfileexist", 1).ret();  // 0x21: push argument 0
  Run r(a, &mem);
  REQUIRE(r.res.ok);
  REQUIRE(r.call("abcd").ok);
  AreaDirector& ad = r.ctx().subsystem<AreaDirector>();
  CHECK(ad.current_area() == id4("abcd"));
  // LoadAreaBlockHeader skips the open while 0x83aedc is loaded (0x48bc26): the slot still holds frnt.blk
  sage::BlockChain* chain = r.ctx().block_chain();
  CHECK(chain->path(sage::Source::Current) == "Root/Data/Levels/lev0/frnt.blk");
  // setcurrentlevel with a level and an area set: RemoveArea (area -> null, both headers closed), then the new level header
  REQUIRE(r.call("lev1").ok);
  CHECK(ad.current_area() == kNullId);
  CHECK(ad.current_level() == id4("lev1"));
  CHECK(chain->path(sage::Source::Current) == "Root/Data/Levels/lev1/lev1.blk");
  // the second slot pointed at the level header object 0x839238, which is now lev1's
  CHECK(chain->path(sage::Source::Second) == "Root/Data/Levels/lev1/lev1.blk");
  CHECK_FALSE(ad.area_header().loaded);
  for (const auto& l : r.log.lines) CHECK(l.rfind("C ", 0) != 0);  // RemoveArea makes no call: no character was created
}

TEST_CASE("osi directors setcurrentarea: no VFS is not a crash (no block file, no area file)") {
  Asm a;
  area_script(a);
  Run r(a, nullptr);
  REQUIRE(r.res.ok);
  CHECK(r.ctx().subsystem<AreaDirector>().current_area() == id4("frnt"));
  CHECK(r.ctx().subsystem<AreaDirector>().block_stats(AreaDirector::kObjects).runs == 0);
}

// ============================== getscriptfluffstring ==============================

TEST_CASE("osi directors getscriptfluffstring asm pin: GetScriptFluffString 0x48acc0 -> GetString 0x5e86e0 on [0x8391c0]") {
  const Pe* im = exe();
  SKIP_WITHOUT(im);
  PIN(im, 0x48725b, "8d 75 10 8d 5d 14 8d 55 18");  // w0, w1, w2 ...
  PIN(im, 0x487264, "8b 02 50 8b 03 50 8b 06 50");  // ... pushed w2, w1, w0: GetScriptFluffString(out, w0, w1, w2)
  PIN(im, 0x487274, "e8 47 3a 00 00");         // -> 0x48acc0
  PIN(im, 0x487287, "e8 74 6b fe ff");         // -> CreateVariant(string) 0x46de00
  PIN(im, 0x48ad29, "83 3d c0 91 83 00 00");   // no table ...
  PIN(im, 0x48ad32, "85 ff 74 04 85 f6 75 26"); // ... w1 == 0 or w0 == 0 ...
  PIN(im, 0x48ad3a, "68 e7 b7 70 00");         // ... -> "XXX"
  CHECK(im->cstr(0x70b7e7) == "XXX");
  PIN(im, 0x48ad73, "e8 68 d9 15 00");         // GetString(w0, w1, w2, &s)
  PIN(im, 0x48ad7a, "68 eb b7 70 00");         // s compared with "" ...
  CHECK(im->cstr(0x70b7eb).empty());
  PIN(im, 0x48ad97, "68 e7 b7 70 00");         // ... empty -> "XXX"
  PIN(im, 0x48addb, "c7 42 08 ff 00 00 00");   // the result's capacity 0xff (Init 0x401850: n >= 255 throws)
  // GetString: the 16-byte entries {key0, key1, n, strings}; index <= n - 1 (unsigned); "" (0x74ae14) otherwise
  PIN(im, 0x5e86ee, "8b 9e 9c 00 00 00");      // entries [this+0x9c] = data+4
  PIN(im, 0x5e8768, "8b 96 98 00 00 00");      // count [this+0x98] = data+0
  PIN(im, 0x5e8765, "83 c3 10");
  PIN(im, 0x5e86f6, "8b 0b 8b 55 08 39 d1");   // key0 == w0
  PIN(im, 0x5e86ff, "8b 4b 04 8b 55 0c 39 d1"); // key1 == w1
  PIN(im, 0x5e8709, "8b 53 08 4a 39 d7 76 1f"); // w2 <= n - 1 (jbe)
  PIN(im, 0x5e8730, "89 fa c1 e2 02 03 53 0c"); // strings[w2]
  PIN(im, 0x5e8711, "68 14 ae 74 00");
  PIN(im, 0x5e877f, "68 14 ae 74 00");         // no entry: ""
  CHECK(im->cstr(0x74ae14).empty());
  PIN(im, 0x5e8675, "ff 03");                  // InitStrings 0x5e8600: inc [slot] (the length prefix is stepped over)
  PIN(im, 0x487cb0, "e8 4b 09 16 00");         // ... run once, after the load in GcAreaDirector::Init
  PIN(im, 0x487c74, "89 15 c0 91 83 00");      // Init stores the table in [0x8391c0]
  PIN(im, 0x48784c, "80 3d 84 b3 70 00 00");   // language byte [0x70b384] == 0 ...
  PIN(im, 0x487855, "68 f4 b3 70 00");         // ... the English table ...
  PIN(im, 0x487a42, "68 14 b5 70 00");         // ... from the English language block file
  CHECK(im->u8(0x70b384) == 0);
  CHECK(im->cstr(0x70b3f4) == kFluffTable);
  CHECK(im->cstr(0x70b514) == kFluffLangBlk);
  PIN(im, 0x487bdf, "e8 3c 5f 0c 00");         // the language block file as the current slot while loading ...
  PIN(im, 0x487daa, "e8 71 5d 0c 00");         // ... and the saved slot restored
}

TEST_CASE("osi directors getscriptfluffstring: hits, the XXX fallbacks, the shared slots, the 255-byte bound") {
  fs::MemoryFileSystem mem;
  const std::string long254(254, 'L'), long255(255, 'M');
  const uint32_t fend = id4("fend"), door = id4("door");
  REQUIRE(mem.mount("Root/Data/Art/Art.blk", blk({{"art.tga", {1}}})));
  REQUIRE(mem.mount(kFluffLangBlk, blk({{"Fluff_eng.slb", fluff_slb({{fend, 1, {"16-bit color", "", "Third"}},
                                                                       {door, 1, {"door text"}},
                                                                       {fend, 2, {"second fend"}},
                                                                       {fend, 1, {"shadowed"}},
                                                                       {id4("shar"), 1, {}, 1},
                                                                       {id4("long"), 1, {long254, long255}},
                                                                       {0, 1, {"zero key0"}},
                                                                       {fend, 0, {"zero key1"}}})}})));
  auto get = [&](uint32_t k0, uint32_t k1, uint32_t idx) {
    Asm a;
    a.fn("setup", 0).i32(static_cast<int32_t>(k0)).i32(static_cast<int32_t>(k1)).i32(static_cast<int32_t>(idx));
    a.call("gcareadirector", "getscriptfluffstring", 3).ret();
    Run r(a, &mem);
    REQUIRE_MESSAGE(r.res.ok, (r.host->vm().last_error() ? r.host->vm().last_error()->message : std::string("?")));
    REQUIRE(r.res.result.type == kString);
    return r.res.result_text;
  };
  CHECK(get(fend, 1, 0) == "16-bit color");
  CHECK(get(fend, 1, 2) == "Third");
  CHECK(get(fend, 1, 1) == "XXX");         // an empty string -> XXX (0x48ad8e)
  CHECK(get(fend, 1, 3) == "XXX");         // past n - 1
  CHECK(get(fend, 1, 0xffffffffu) == "XXX");  // unsigned compare: -1 is past n - 1
  CHECK(get(fend, 2, 0) == "second fend");
  CHECK(get(door, 3, 0) == "XXX");         // no entry
  CHECK(get(0, 1, 0) == "XXX");            // w0 == 0 (0x48ad36), although an entry {0, 1} holds "zero key0"
  CHECK(get(fend, 0, 0) == "XXX");         // w1 == 0 (0x48ad32), although an entry {fend, 0} holds "zero key1"
  CHECK(get(id4("long"), 1, 0) == long254);
  // the entry "shar" reuses door's pointer array: InitStrings advances those slots twice: one step past the length prefix, one into the text ("door text" -> "oor text"), for both keys
  CHECK(get(id4("shar"), 1, 0) == "oor text");
  CHECK(get(door, 1, 0) == "oor text");
  {
    Asm a;  // a 255-byte string: kOutOfRange at the copy (0x401850, n >= 255)
    a.fn("setup", 0).i32(static_cast<int32_t>(id4("long"))).i8(1).i8(1).call("gcareadirector", "getscriptfluffstring", 3).ret();
    Run r(a, &mem);
    CHECK_FALSE(r.res.ok);
  }
  {
    fs::MemoryFileSystem none;  // no table at all ([0x8391c0] = 0): XXX
    Asm a;
    a.fn("setup", 0).i32(static_cast<int32_t>(fend)).i8(1).i8(0).call("gcareadirector", "getscriptfluffstring", 3).ret();
    Run r(a, &none);
    REQUIRE(r.res.ok);
    CHECK(r.res.result_text == "XXX");
  }
}

// native only: the disk file system (fs::open_disk) is not linked into the wasm32 unit tests
#ifndef __EMSCRIPTEN__
TEST_CASE("osi directors getscriptfluffstring: an independent 10-line reader of the install's Lang_eng.blk agrees on every slot") {
  const char* ld = std::getenv("LOMN_DATA");
  if (!ld || !*ld) {
    MESSAGE("SKIPPED (no LOMN_DATA)");
    return;
  }
  std::unique_ptr<fs::FileSystem> disk = fs::open_disk(std::string(ld) + "/install", "");
  REQUIRE(disk);
  // the independent reader: raw bytes of the .blk at the entry's offset, then the table walked as the asm reads it
  const std::string blkb = slurp(std::string(ld) + "/install/data/Languages/lang_eng.blk");
  REQUIRE(blkb.size() > 12);
  auto u32 = [](const std::string& s, size_t o) {
    return static_cast<uint32_t>(static_cast<uint8_t>(s[o])) | static_cast<uint32_t>(static_cast<uint8_t>(s[o + 1])) << 8 |
           static_cast<uint32_t>(static_cast<uint8_t>(s[o + 2])) << 16 | static_cast<uint32_t>(static_cast<uint8_t>(s[o + 3])) << 24;
  };
  std::string t;  // the entry Fluff_eng.slb, stored (flags 2)
  for (uint32_t i = 0, n = u32(blkb, 8); i < n; ++i)
    if (blkb.compare(12 + 56 * i, 14, std::string("Fluff_eng.slb\0", 14)) == 0) t = blkb.substr(u32(blkb, 12 + 56 * i + 52), u32(blkb, 12 + 56 * i + 44));
  REQUIRE(!t.empty());
  size_t checked = 0;
  for (uint32_t e = 0, ne = u32(t, 0), ents = u32(t, 4); e < ne; ++e) {
    const uint32_t k0 = u32(t, ents + 16 * e), k1 = u32(t, ents + 16 * e + 4), n = u32(t, ents + 16 * e + 8), sp = u32(t, ents + 16 * e + 12);
    for (uint32_t j = 0; j < n; ++j) {
      const std::string want = t.c_str() + u32(t, sp + 4 * j) + 1;  // step over the length byte (InitStrings)
      Asm a;
      a.fn("setup", 0).i32(static_cast<int32_t>(k0)).i32(static_cast<int32_t>(k1)).i32(static_cast<int32_t>(j));
      a.call("gcareadirector", "getscriptfluffstring", 3).ret();
      Run r(a, disk.get());
      REQUIRE(r.res.ok);
      bool first = true;  // only the first entry with this key pair is reachable (GetString returns on the first match)
      for (uint32_t p = 0; p < e; ++p) first &= !(u32(t, ents + 16 * p) == k0 && u32(t, ents + 16 * p + 4) == k1);
      if (first) CHECK_MESSAGE(r.res.result_text == (want.empty() ? std::string("XXX") : want), "entry " << e << " index " << j);
      ++checked;
    }
  }
  MESSAGE("fluff strings checked: " << checked);
  CHECK(checked > 100);
  // the A9 path's calls: ('fend', 1, 0x23) is the first of Setup
  Asm a;
  a.fn("setup", 0).i32(0x66656e64).i8(1).i8(0x23).call("gcareadirector", "getscriptfluffstring", 3).ret();
  Run r(a, disk.get());
  REQUIRE(r.res.ok);
  CHECK(r.res.result_text != "XXX");
}
#endif

// ============================== cheatmenu ==============================

TEST_CASE("osi directors cheatmenu asm pin: GetPrivateProfileStringA(\"Misc\", \"CheatMenu\", \"\") == \"LEGOTester\"") {
  const Pe* im = exe();
  SKIP_WITHOUT(im);
  PIN(im, 0x5d35b3, "e8 08 1a 00 00");         // body 0x5d35b0 -> GcSaver::CheatMenu 0x5d4fc0
  PIN(im, 0x5d35bd, "e8 6e 49 e3 ff");         // -> CreateVariant 0x407f30 ({0x10, the byte})
  PIN(im, 0x407f35, "66 c7 03 10 00");
  PIN(im, 0x5d4fca, "68 10 b4 83 00");         // the INI path [0x83b410] ...
  PIN(im, 0x5d4fcf, "68 ff 00 00 00");         // ... 0xff bytes ...
  PIN(im, 0x5d4fdb, "68 4b 76 74 00");         // ... default "" ...
  PIN(im, 0x5d4fe0, "68 79 76 74 00");         // ... "CheatMenu" ...
  PIN(im, 0x5d4fe5, "68 1d 76 74 00");         // ... "Misc"
  PIN(im, 0x5d4fea, "ff 15 24 14 77 00");
  CHECK(im->import_name(0x771424) == "KERNEL32.dll!GetPrivateProfileStringA");
  CHECK(im->cstr(0x74761d) == "Misc");
  CHECK(im->cstr(0x747679) == "CheatMenu");
  CHECK(im->cstr(0x74764b).empty());
  CHECK(im->cstr(0x74765f) == "LEGOTester");
  PIN(im, 0x5d502c, "68 5f 76 74 00");         // compared ...
  PIN(im, 0x5d5037, "e8 54 09 f2 ff");         // ... by ScFixedString::compare 0x4f5990: memcmp 0x657820 (case sensitive)
  PIN(im, 0x5d503c, "85 c0 75 05 bb 01 00 00 00");  // equal -> 1
  PIN(im, 0x4f59d4, "8b 43 04 29 f0");         // compare: a common prefix gives len - strlen("LEGOTester")
  PIN(im, 0x657834, "f3 a6");                  // repe cmpsb
  // the model's default: the INI value is absent, the call yields "" (int 0)
  Asm a;
  a.fn("setup", 0).nothing().ret();
  Run r(a);
  CHECK(r.ctx().subsystem<SaverState>().ini_cheat_menu == im->cstr(0x74764b));
}

TEST_CASE("osi directors cheatmenu: int 0 for this install; exactly \"LEGOTester\" gives 1") {
  auto run = [](const std::string& ini) {
    Asm a;
    a.fn("setup", 0).call("gcsaver", "cheatmenu", 0).ret();
    Run r(a, nullptr, HostFlags{}, [&](ScriptHost& h) { h.context().subsystem<SaverState>().ini_cheat_menu = ini; });
    return r.value();
  };
  CHECK(run("") == make_int(0));
  CHECK(run("LEGOTester") == make_int(1));
  CHECK(run("legotester") == make_int(0));   // memcmp: case sensitive
  CHECK(run("LEGOTester2") == make_int(0));  // the length is compared too
  CHECK(run("LEGOTeste") == make_int(0));
}

TEST_CASE("osi directors cheatmenu: the owner's Bionicle.ini has [Misc] but no CheatMenu key") {
  const char* ld = std::getenv("LOMN_DATA");
  if (!ld || !*ld) {
    MESSAGE("SKIPPED (no LOMN_DATA)");
    return;
  }
  const std::string ini = slurp(std::string(ld) + "/install/Bionicle.ini");
  REQUIRE(!ini.empty());
  // GetPrivateProfileString matches section and key names case-insensitively
  std::string section;
  bool misc = false, key = false;
  size_t pos = 0;
  while (pos < ini.size()) {
    size_t nl = ini.find('\n', pos);
    if (nl == std::string::npos) nl = ini.size();
    std::string line = ini.substr(pos, nl - pos);
    pos = nl + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    for (char& c : line)
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c | 0x20);
    if (!line.empty() && line[0] == '[') {
      section = line;
      misc |= section == "[misc]";
      continue;
    }
    if (section == "[misc]" && line.rfind("cheatmenu", 0) == 0) key = true;
  }
  CHECK(misc);
  CHECK_FALSE(key);
}

// ============================== getammo (and the tier2 setters' .data) ==============================

TEST_CASE("osi directors gcsaver asm pin: getammo u16 [0x7474c8] (0 in .data); the setters' fields and defaults") {
  const Pe* im = exe();
  SKIP_WITHOUT(im);
  PIN(im, 0x5d3913, "66 a1 c8 74 74 00");      // getammo: mov ax, [0x7474c8] ...
  PIN(im, 0x5d391e, "e8 cd 4a e4 ff");         // ... CreateVariant(u16) 0x4183f0 ...
  PIN(im, 0x4183f5, "66 c7 03 10 00");         // ... {0x10, movzx word} (no sign extension)
  PIN(im, 0x4183fa, "0f b7 54 24 0c");
  CHECK(im->u32(0x7474c8) == 0);
  PIN(im, 0x56898b, "66 89 15 c8 74 74 00");   // the GcToa ctor's write (after getammo on the A9 path)
  PIN(im, 0x5d35fb, "89 15 a0 74 74 00");      // setmask: dword [0x7474a0]
  PIN(im, 0x5d365b, "88 15 a8 74 74 00");      // sethealth: byte [0x7474a8]
  PIN(im, 0x5d368b, "88 15 ac 74 74 00");      // setenergy: byte [0x7474ac]
  CHECK(im->u32(0x7474a0) == kNullId);
  CHECK(im->u8(0x7474a8) == 0);
  CHECK(im->u8(0x7474ac) == 0);
  PIN(im, 0x5d5493, "c6 05 8c 74 74 00 00");   // ResetData: the level byte ...
  PIN(im, 0x5d549a, "c7 05 90 74 74 00 3f 3f 3f 3f");  // ... the area ...
  PIN(im, 0x5d54ec, "c7 05 a0 91 83 00 00 00 00 00");  // ... the legacy condition bits
  Asm a;
  a.fn("setup", 0).nothing().ret();
  Run r(a);
  const SaverState& s = r.ctx().subsystem<SaverState>();
  CHECK(s.ammo == im->u32(0x7474c8));
  CHECK(s.mask == im->u32(0x7474a0));
  CHECK(s.health == im->u8(0x7474a8));
  CHECK(s.energy == im->u8(0x7474ac));
  CHECK(s.area == kNullId);
}

TEST_CASE("osi directors getammo: 0 at Setup; reads back what the saver holds, as an unsigned 16-bit value") {
  Asm a;
  a.fn("setup", 0).call("gcsaver", "getammo", 0).ret();
  a.fn("get", 0).call("gcsaver", "getammo", 0).ret();
  Run r(a);
  CHECK(r.value() == make_int(0));
  CHECK(r.log_has("gcsaver.getammo 0 -> 0010:00000000"));
  r.ctx().subsystem<SaverState>().ammo = 250;  // what the GcToa ctor stores (0x567e27 -> 0x56898b)
  CHECK(r.call("get").result == make_int(250));
  r.ctx().subsystem<SaverState>().ammo = 0xffff;
  CHECK(r.call("get").result == make_int(65535));
}

// ============================== bettersaver.getbooleanvalue / setbooleanvalue / save ==============================

TEST_CASE("osi directors bettersaver asm pin: the u16 key, (al != 0), set's erase / legacy bits / quests, save") {
  const Pe* bs = bettersaver();
  SKIP_WITHOUT(bs);
  PIN(bs, 0x10001f77, "0f b7 75 10");          // get: the key is a u16
  PIN(bs, 0x10001f88, "e8 23 3b 00 00");       // lower_bound 0x10005ab0 on the map 0x1000a80c ...
  PIN(bs, 0x10005ab3, "8b 0d 0c a8 00 10");
  PIN(bs, 0x10001f96, "66 3b 70 0e");          // ... found when !(key < node.key) ...
  PIN(bs, 0x10001fb8, "8a 40 10");             // ... the byte at node+0x10
  PIN(bs, 0x10001fc8, "32 c0");                // not found: 0
  PIN(bs, 0x10001fd1, "0f 95 c1");             // setne: 0 or 1 ...
  PIN(bs, 0x10001fd8, "b9 10 00 00 00");       // ... type 0x10
  PIN(bs, 0x10001ff4, "83 7d 14 00");          // set: w1 compared as a whole dword ...
  PIN(bs, 0x10002005, "0f 95 45 13");          // ... v = (w1 != 0)
  PIN(bs, 0x10001ffb, "0f b7 45 10");          // ... u16 key
  PIN(bs, 0x10002009, "e8 22 27 00 00");       // -> 0x10004730
  PIN(bs, 0x10002011, "b9 0f 00 00 00");       // returns nothing
  PIN(bs, 0x10004762, "80 3b 00");             // v == 0 ...
  PIN(bs, 0x1000476f, "0f 84 d7 00 00 00");    // ... -> the removal branch 0x1000484c ...
  PIN(bs, 0x10004889, "e8 32 08 00 00");       // ... which erases a present key
  PIN(bs, 0x100047c9, "88 41 10");             // v != 0: a present key gets the byte, else a new node (0x1000481c)
  PIN(bs, 0x1000488e, "66 83 3f 20 73 52");    // key < 32: the legacy condition dword too ...
  CHECK(bs->u32(0x1000a040) == 0x8391a0);
  PIN(bs, 0x100048ab, "0f ab d0");             // ... bts (v != 0) ...
  PIN(bs, 0x100048e1, "0f b3 d0");             // ... btr (v == 0)
  PIN(bs, 0x100048b3, "83 f8 0e 75 04 6a 01"); // keys 0xe / 0x10 / 0x12 set: GcSaver::SetQuest(level, 1 / 2 / 3)
  PIN(bs, 0x100048bc, "83 f8 10 75 04 6a 02");
  PIN(bs, 0x100048c5, "83 f8 12 75 1c 6a 03");
  CHECK(bs->u32(0x1000a07c) == 0x74748c);      // the saver's level byte
  PIN(bs, 0x100048d5, "b8 20 5e 5d 00");
  PIN(bs, 0x10001f43, "e8 b8 0b 00 00");       // save: the save routine 0x10002b00 (the file write: M4) ...
  PIN(bs, 0x10001f56, "b9 0f 00 00 00");       // ... returns nothing
  CHECK(bs->import_name(0x10008064) == "KERNEL32.dll!OutputDebugStringW");
  const Pe* im = exe();
  REQUIRE(im);
  PIN(im, 0x5d5e24, "80 f9 01 72 41");         // SetQuest: level >= 1 ...
  PIN(im, 0x5d5e29, "80 f9 06 77 3c");         // ... and <= 6 ...
  PIN(im, 0x5d5e33, "83 ea 01 83 fa 02 77 2f");  // ... quest 1..3 ...
  PIN(im, 0x5d5e45, "80 8a 7f b3 83 00 01");   // ... |= 1 / 2 / 4 into [0x83b37f + level]
  PIN(im, 0x5d5e53, "80 8a 7f b3 83 00 02");
  PIN(im, 0x5d5e63, "80 8a 7f b3 83 00 04");
}

TEST_CASE("osi directors bettersaver: empty map -> 0; set -> get; u16 keys; 0 erases; legacy bits and quests; save writes nothing") {
  Asm a;
  a.fn("setup", 0).i8(100).call("bettersaver", "getbooleanvalue", 1).ret();
  a.fn("set", 2).op(0x21).u16(0).op(0x21).u16(1).call("bettersaver", "setbooleanvalue", 2).ret();
  a.fn("get", 1).op(0x21).u16(0).call("bettersaver", "getbooleanvalue", 1).ret();
  a.fn("save", 0).call("bettersaver", "save", 0).ret();
  Run r(a);
  CHECK(r.value() == make_int(0));
  CHECK(r.log_has("bettersaver.getbooleanvalue 1 -> 0010:00000000"));
  auto set = [&](int32_t k, int32_t v) { REQUIRE(r.host->vm().call("set", {make_int(k), make_int(v)}).ok); };
  auto get = [&](int32_t k) { return r.host->vm().call("get", {make_int(k)}).result; };
  set(100, 7);
  CHECK(get(100) == make_int(1));
  CHECK(get(0x10064) == make_int(1));   // the key is u16 ...
  CHECK(get(0x164) == make_int(0));     // ... not u8 (0x164 != 0x64)
  set(0x20064, 0);
  CHECK(get(100) == make_int(0));
  CHECK(r.ctx().subsystem<BetterSaverState>().values().count(100) == 0);  // erased, not stored as false
  set(101, 0x100);                       // the whole dword is tested: 0x100 != 0
  CHECK(get(101) == make_int(1));
  SaverState& sv = r.ctx().subsystem<SaverState>();
  set(3, 1);
  CHECK(sv.condition == 0x8u);
  set(31, 1);
  CHECK(sv.condition == 0x80000008u);
  set(3, 0);
  CHECK(sv.condition == 0x80000000u);
  set(32, 1);                            // >= 32: no legacy bit
  CHECK(sv.condition == 0x80000000u);
  sv.level = 0;                          // SetQuest ignores level 0 (cmp cl, 1 ; jb at 0x5d5e24)
  set(14, 1);
  set(16, 1);
  set(18, 1);
  for (int l = 0; l < 7; ++l) CHECK(sv.quests[static_cast<size_t>(l)] == 0);
  sv.level = 4;                          // each key's own quest bit: 0xe -> 1, 0x10 -> 2, 0x12 -> 4
  set(16, 1);
  CHECK(sv.quests[4] == 2);
  sv.level = 5;
  set(18, 1);
  CHECK(sv.quests[5] == 4);
  sv.level = 2;
  set(14, 1);
  CHECK(sv.quests[2] == 1);
  set(16, 1);
  set(18, 1);
  CHECK(sv.quests[2] == 7);
  set(14, 0);                            // clearing does not touch the quests
  CHECK(sv.quests[2] == 7);
  sv.level = 7;                          // SetQuest ignores a level above 6 (and never writes past the array)
  set(16, 1);
  sv.level = 0xff;
  set(16, 1);
  sv.level = 6;
  set(14, 1);
  for (int l = 0; l < 7; ++l)
    CHECK(sv.quests[static_cast<size_t>(l)] == (l == 2 ? 7 : l == 4 ? 2 : l == 5 ? 4 : l == 6 ? 1 : 0));
  const auto before = r.ctx().subsystem<BetterSaverState>().values();
  CHECK(r.call("save").result == nothing());
  CHECK(r.ctx().subsystem<BetterSaverState>().values() == before);
}

// ============================== playcinematic (D1) ==============================

TEST_CASE("osi directors playcinematic asm pin: PlayCinematic 0x47b820 plays only while [0x70a3b0] is set and kills at once") {
  const Pe* im = exe();
  SKIP_WITHOUT(im);
  PIN(im, 0x47b10e, "8a 02");                  // the low byte of w1 ...
  PIN(im, 0x47b112, "e8 09 07 00 00");         // ... PlayCinematic(name, byte) 0x47b820
  PIN(im, 0x47b119, "66 c7 06 0f 00");         // returns nothing
  PIN(im, 0x47b849, "c7 42 08 ff 00 00 00");   // the name copy's capacity 0xff
  PIN(im, 0x47b867, "80 3d b0 a3 70 00 00");   // the video switch [0x70a3b0] ...
  CHECK(im->u8(0x70a3b0) == 1);                // ... on in .data ...
  PIN(im, 0x5380bb, "c6 05 b0 a3 70 00 00");   // ... its only writer turns it off
  PIN(im, 0x47b8ca, "e8 71 76 16 00");         // LoadCinematic 0x5e2f40 ...
  PIN(im, 0x47b8fa, "ff 56 08");               // ... play ...
  PIN(im, 0x47b8fd, "e8 1e 00 00 00");         // ... KillCinematic 0x47b920 before the return
  PIN(im, 0x47b929, "e8 22 77 16 00");         // UnLoadCinematic 0x5e3050
}

TEST_CASE("osi directors playcinematic: complete when the native returns (D1); no engine call; skip-video on and off") {
  for (bool skip : {true, false}) {
    Asm a;
    a.fn("setup", 0).s("Root/Data/cinematics/New_LOGO8015.bik").i8(1).call("gchuddirector", "playcinematic", 2).pop();
    a.call("gcsaver", "getammo", 0).pop();  // the next native sees nothing in flight
    a.s("Root/Data/cinematics/BIONICLEFMV3.bik").i32(0x101).call("gchuddirector", "playcinematic", 2).ret();
    a.fn("process", 3).i8(1).ret();
    HostFlags f;
    f.skip_video = skip;
    bool in_flight_at_next_native = true;
    Run r(a, nullptr, f, [&](ScriptHost& h) {
      // a probe wrapped around getammo: is a cinematic still playing when the next native runs?
      static bool* out = nullptr;
      static HostContext* ctx = nullptr;
      static NativeFn real = nullptr;
      static void* real_user = nullptr;
      out = &in_flight_at_next_native;
      ctx = &h.context();
      const NativeEntry* e = h.registry().find("gcsaver", "getammo");
      REQUIRE(e);
      real = e->fn;
      real_user = e->user;
      h.registry().bind("gcsaver.getammo", [](NativeCall& c, void*) {
        *out = ctx->subsystem<CinematicState>().playing;
        return real ? real(c, real_user) : make_int(0);  // (RED: the unbound body)
      }, &h.context(), Tier::Implemented);
    });
    CHECK(r.value() == nothing());
    CHECK_FALSE(in_flight_at_next_native);
    const CinematicState& cs = r.ctx().subsystem<CinematicState>();
    CHECK(cs.completed == 2);
    CHECK(cs.skipped == (skip ? 2u : 0u));
    CHECK(cs.last == "Root/Data/cinematics/BIONICLEFMV3.bik");
    CHECK_FALSE(cs.playing);
    for (const auto& l : r.log.lines) CHECK(l.rfind("C ", 0) != 0);  // PlayCinematic reaches no Call (calls.jsonl closure)
    REQUIRE(r.host->frame());
    CHECK(r.trace_has("K      0 "));  // the frame after Setup is frame 0: no frame was spent waiting
  }
}

// ============================== the real tables ==============================

#ifdef LOMN_OSI_DATA_DIR
TEST_CASE("osi directors overrides rows: pure ASCII; each key once with its exact tier, no default, the body and method cited") {
  const std::string ov = slurp(std::string(LOMN_OSI_DATA_DIR) + "/native_overrides.json");
  REQUIRE(!ov.empty());
  CHECK(std::count_if(ov.begin(), ov.end(), [](char ch) { return static_cast<unsigned char>(ch) >= 0x80; }) == 0);  // pure ASCII
  Json j;
  std::string err;
  REQUIRE_MESSAGE(parse_json(ov, j, &err), err);
  struct Want {
    const char* key;
    const char* tier;
    std::vector<const char*> addrs;  // the body first, then the method(s) the brief and the bindings cite
  };
  const std::vector<Want> want = {
      {"bettersaver.getbooleanvalue", "tier1", {"0x10001f70", "0x10005ab0", "0x10001fc8", "0x10001fd8"}},
      {"bettersaver.save", "tier1", {"0x10001f40", "0x10002b00"}},
      {"bettersaver.setbooleanvalue", "tier1", {"0x10001ff0", "0x10004730", "0x10002005"}},
      {"gcareadirector.getcurrentlevel", "tier1", {"0x486c40", "0x83902c", "0x486c46"}},
      {"gcareadirector.getscriptfluffstring", "tier1", {"0x487250", "0x48acc0", "0x5e86e0"}},
      {"gcareadirector.setcurrentarea", "tier1", {"0x486ef0", "0x48a540", "0x4fedd0"}},
      {"gcareadirector.setcurrentlevel", "tier1", {"0x486c10", "0x489a60", "0x489af6"}},
      {"gcgame.getkeymapper", "tier1", {"0x4367f0", "0x43ae90", "0x83aa98"}},
      {"gcgame.getmainview", "tier1", {"0x4366e0", "0x43ae20", "0x49d3c0"}},
      {"gcgame.getmp3player", "tier1", {"0x436940", "0x43aea0", "0x436960", "0x83879c"}},
      {"gchuddirector.playcinematic", "tier1", {"0x47b100", "0x47b820", "0x70a3b0"}},
      {"gckeymapper.setusecontext", "tier2", {"0x5ceed0"}},
      {"gcmp3player.play", "tier2", {"0x5dc970"}},
      {"gcmp3player.setupforplay", "tier2", {"0x5dc840"}},
      {"gcmp3player.stop", "tier2", {"0x5dc9f0"}},
      {"gcsaver.cheatmenu", "tier1", {"0x5d35b0", "0x5d4fc0", "0x74761d", "0x747679", "0x74764b", "0x74765f", "0x771424"}},
      {"gcsaver.getammo", "tier1", {"0x5d3910", "0x7474c8"}},
      {"gcsaver.setenergy", "tier2", {"0x5d3680", "0x7474ac"}},
      {"gcsaver.sethealth", "tier2", {"0x5d3650", "0x7474a8"}},
      {"gcsaver.setmask", "tier2", {"0x5d35f0", "0x7474a0"}},
      {"gcviewport.getcamera", "tier1", {"0x4474e0", "0x448080"}},
  };
  std::string prev;
  for (const Json& row : j.arr) {  // sorted by key, each key once (natives.md §7.2)
    const std::string k = row.get("key")->str;
    CHECK_MESSAGE(prev < k, k);
    prev = k;
  }
  for (const Want& w : want) {
    const Json* row = nullptr;
    for (const Json& r : j.arr)
      if (r.get("key")->str == w.key) row = &r;
    REQUIRE_MESSAGE(row, w.key);
    CHECK_MESSAGE(row->get("tier")->str == w.tier, w.key);
    CHECK_MESSAGE(row->get("default") == nullptr, w.key);  // tier1: the body decides; tier2: the typed stub (nothing)
    REQUIRE_MESSAGE(row->get("note"), w.key);
    const std::string cite = row->get("cite")->str;
    // natives.md §1.3: a mod body names its DLL first
    const std::string dll = std::string(w.key).rfind("bettersaver.", 0) == 0 ? "BetterSaver.dll " : "";
    if (std::string(w.tier) == "tier1") CHECK_MESSAGE(cite.rfind("// body " + dll + w.addrs[0] + ", method 0x", 0) == 0, w.key);
    else CHECK_MESSAGE(cite.rfind("body " + std::string(w.addrs[0]), 0) == 0, w.key);
    for (const char* a : w.addrs) CHECK_MESSAGE(cite.find(a) != std::string::npos, w.key << " cites " << a);
  }
}

TEST_CASE("osi directors real tables: the group's tier1 overrides are bound; the tier2 rows stay stubs") {
  const std::string dir = LOMN_OSI_DATA_DIR;
  const std::string nj = slurp(dir + "/natives.json"), mj = slurp(dir + "/natives_beta150.json"),
                    tj = slurp(dir + "/types.json"), ov = slurp(dir + "/native_overrides.json");
  REQUIRE(!nj.empty());
  Asm a;
  a.fn("setup", 0).call("gcgame", "getmp3player", 0).ret();
  sage::OsiFile file;
  std::string err;
  REQUIRE_MESSAGE(file.parse(a.build(), &err), err);
  StringTrace tr, log;
  ScriptHost::Options real;
  real.real_natives = true;
  ScriptHost host(file, nj, mj, tj, ov, nullptr, &tr, &log, real);
  for (const char* k : {"gcgame.getmainview", "gcgame.getkeymapper", "gcgame.getmp3player", "gcviewport.getcamera",
                        "gcareadirector.getcurrentlevel", "gcareadirector.setcurrentlevel", "gcareadirector.setcurrentarea",
                        "gcareadirector.getscriptfluffstring", "gcsaver.cheatmenu", "gcsaver.getammo",
                        "gchuddirector.playcinematic", "bettersaver.getbooleanvalue", "bettersaver.setbooleanvalue",
                        "bettersaver.save"}) {
    const std::string key(k);
    const size_t dot = key.find('.');
    const NativeEntry* e = host.registry().find(key.substr(0, dot), key.substr(dot + 1));
    REQUIRE_MESSAGE(e, key);
    CHECK_MESSAGE(e->tier == Tier::Implemented, key);
    CHECK_MESSAGE(e->fn != nullptr, key);
  }
  for (const char* k : {"gcsaver.setmask", "gcsaver.sethealth", "gcsaver.setenergy", "gckeymapper.setusecontext",
                        "gcmp3player.stop", "gcmp3player.setupforplay", "gcmp3player.play"}) {
    const std::string key(k);
    const size_t dot = key.find('.');
    const NativeEntry* e = host.registry().find(key.substr(0, dot), key.substr(dot + 1));
    REQUIRE_MESSAGE(e, key);
    CHECK_MESSAGE(e->tier == Tier::Tier2, key);
    CHECK_MESSAGE(e->fn == nullptr, key);
  }
  CallResult r = host.setup();
  REQUIRE(r.ok);
  CHECK(is_game_type(r.result.type));
  bool g1 = false;
  for (const auto& l : log.lines) g1 |= l.find("gcgame.getmp3player 0 -> ") != std::string::npos && l.find(" G 1") != std::string::npos;
  CHECK(g1);
}
#endif
