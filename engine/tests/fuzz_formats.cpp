// M1a-4 A11: one deterministic mutation-fuzz driver over every real file format the engine parses (spec M1 §4 A11, §5):
//   blk        BlockFile::open_bytes + is_sorted / find / read (stored and LZSS entries)
//   lzss       decode_lzss on [u32 declared size][stream]
//   slb.obj / slb.pos / slb.light / slb.tex
//              Slb::parse, then parse_obj / parse_pos / parse_light / parse_spec_textures (all four views on every
//              input; the format's own view decides accepted / deep)
//   x          parse_x (binary .x)
//   dds, tga   decode_dds, decode_tga
//   osi        OsiFile::parse + check_all + sampled eval_calls
// Seeds: each format's synthetic unit-test inputs (rebuilt here from the builders in tests/test_sage_*.cpp,
// sage_test_util.h and osi_builder.h), plus, with --data <install>, real files sampled per format with a deterministic
// RNG (read only; no game bytes are ever written except a finding's input, into --crash-dir, which must lie under
// build/). The Beta install has no compressed BLK entry, so the real LZSS seeds are real entry prefixes encoded by the
// independent test encoder ("real-derived").
// Mutations (1..4 per input): truncation at a random prefix, bit flips, length-field inflation (u32/u16 fields set to
// size+-k, remaining+-k, 0, 0xffffffff/0xffff, 0x80000000/0x8000, 2^k; at the seed's known count/size fields or at a
// random aligned offset), chunk duplication, chunk deletion, interesting bytes, runs of one non-NUL byte, byte inserts.
// Invariants on EVERY input: no sanitizer report (ASan+UBSan builds abort; the death callback names the input); an
// accepted parse satisfies the format invariants (sizes consistent, indices in range, caps hold, Image rgba == w*h*4);
// a rejected parse leaves its output default and has a non-empty error string; per-input CPU time < 2 s (see
// run_measured: wall time on a shared host measures the other load); per-input
// heap growth (peak live operator-new bytes) and process-memory growth (ru_maxrss natively, wasm linear memory) under
// 64 MiB natively / 256 MiB on wasm32; every byte the input allocated is released afterwards.
// Deep reach: an input whose parse got past the header checks into the format's body loop (BLK entry table, LZSS
// decode loop, the SLB view's record loop, .x mesh decoding (decode_mesh entered; the token stream is printed apart as
// token_stream=), the DDS block loop, TGA: the RLE packet loop OR an accepted image (so accepted raw type-2 TGAs, which
// have no packet loop, count as deep too), the OSI tables + bytecode walk). Determined from the parser's result and error string (see deep_* below), not from the input.
// Output: per format "inputs accepted deep findings" (deterministic: identical natively and as wasm32), then a
// separate "resources" block (timings and memory, not deterministic). Exit 1 on any finding or a deep-reach fraction
// below --min-deep (default 0.05), 2 on usage errors, 3 when --data yields no real seed for a format.
// Usage: lomn_fuzz_formats [--n N] [--seed S] [--data INSTALL] [--formats a,b] [--real-per-format K] [--crash-dir D]
//                          [--min-deep F] [--smoke] [--replay FMT ITER --dump FILE] [--last-input FILE]
//                          [--test-hang-seed K]
// How to run: tools/research/fuzz_formats.md (make fuzz-formats; ctest fuzz_formats_smoke).
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "fuzz_fragments.h"
#include "lomn/sage/blockfile.h"
#include "lomn/sage/image.h"
#include "lomn/sage/osi.h"
#include "lomn/sage/slb.h"
#include "lomn/sage/xfile.h"
#include "osi_builder.h"
#include "sage_test_util.h"

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define LOMN_FUZZ_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) && !defined(LOMN_FUZZ_ASAN)
#define LOMN_FUZZ_ASAN 1
#endif
#if defined(LOMN_FUZZ_ASAN)
#include <sanitizer/allocator_interface.h>
#include <sanitizer/common_interface_defs.h>
#elif defined(__APPLE__)
#include <malloc/malloc.h>
#else
#include <malloc.h>
#endif
#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#else
#include <csignal>
#include <ctime>
#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>
#endif

// ASan's quarantine on wasm32 (M1a-4, measured): with the default 256 MiB quarantine the ASan allocator's per-size-class
// regions grow at every format change and never shrink, and a 2,000,000-input run hit the 2 GB cap ("allocator is out
// of memory", x input 442 after 1.4 M inputs; that input alone is benign). With 32 MiB the same formats stay far below
// it (build/asan exp logs, tools/research/fuzz_formats.md). 32 MiB still holds many inputs' worth of freed memory, so a
// use-after-free inside one input is still caught. Native keeps ASan's default unless LOMN_FUZZ_QUARANTINE_MB is set.
#if defined(LOMN_FUZZ_ASAN) && defined(__EMSCRIPTEN__) && !defined(LOMN_FUZZ_QUARANTINE_MB)
#define LOMN_FUZZ_QUARANTINE_MB 32
#endif
#if defined(LOMN_FUZZ_ASAN) && defined(LOMN_FUZZ_QUARANTINE_MB)
#define LOMN_STR2(x) #x
#define LOMN_STR(x) LOMN_STR2(x)
extern "C" const char* __asan_default_options() { return "quarantine_size_mb=" LOMN_STR(LOMN_FUZZ_QUARANTINE_MB); }
#endif

using namespace lomn::sage;
using sage_test::Bytes;
using sage_test::put16;
using sage_test::put32;
using sage_test::put8;
using sage_test::putf;
using sage_test::putstr;

// ---------------------------------------------------------------------------------------------------------------------
// Heap accounting: every operator new/delete of the process goes through these counters, so the per-input peak of live
// heap bytes is exact (parsers allocate only through std::vector / std::string). Sizes come from the allocator itself.
namespace heap {
size_t live = 0, peak = 0;
inline size_t size_of(void* p) {
#if defined(LOMN_FUZZ_ASAN)
  return __sanitizer_get_allocated_size(p);
#elif defined(__APPLE__)
  return malloc_size(p);
#else
  return malloc_usable_size(p);
#endif
}
inline void* alloc(size_t n) {
  void* p = std::malloc(n ? n : 1);
  if (!p) {
    std::fputs("fuzz_formats: out of memory\n", stderr);
    std::abort();
  }
  live += size_of(p);
  if (live > peak) peak = live;
  return p;
}
inline void release(void* p) {
  if (!p) return;
  live -= size_of(p);
  std::free(p);
}
}  // namespace heap

void* operator new(size_t n) { return heap::alloc(n); }
void* operator new[](size_t n) { return heap::alloc(n); }
void* operator new(size_t n, const std::nothrow_t&) noexcept { return heap::alloc(n); }
void* operator new[](size_t n, const std::nothrow_t&) noexcept { return heap::alloc(n); }
void operator delete(void* p) noexcept { heap::release(p); }
void operator delete[](void* p) noexcept { heap::release(p); }
void operator delete(void* p, size_t) noexcept { heap::release(p); }
void operator delete[](void* p, size_t) noexcept { heap::release(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { heap::release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { heap::release(p); }

namespace {

// Process memory high-water mark in bytes: ru_maxrss natively, the wasm32 linear memory size under Node.
uint64_t process_peak_bytes() {
#if defined(__EMSCRIPTEN__)
  return static_cast<uint64_t>(__builtin_wasm_memory_size(0)) * 65536u;
#else
  rusage u{};
  getrusage(RUSAGE_SELF, &u);
#if defined(__APPLE__)
  return static_cast<uint64_t>(u.ru_maxrss);
#else
  return static_cast<uint64_t>(u.ru_maxrss) * 1024u;
#endif
#endif
}

#if defined(__EMSCRIPTEN__)
constexpr uint64_t kMaxGrowth = 256ull << 20;  // wasm32 heap growth per input
#else
constexpr uint64_t kMaxGrowth = 64ull << 20;  // native RSS / heap growth per input
#endif
constexpr double kMaxInputSeconds = 2.0;
constexpr size_t kSeedMax = 256u << 10;  // real seeds above this are not sampled (per-input copy cost)
constexpr size_t kMaxSaved = 8;          // finding inputs written per format

uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint32_t rd32(const Bytes& b, size_t at) { return at + 4 <= b.size() ? rd32(b.data() + at) : 0; }

struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed) {}
  uint64_t next() {  // splitmix64
    uint64_t z = (s += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
  }
  uint32_t below(uint64_t n) { return n ? static_cast<uint32_t>(next() % n) : 0; }
};
uint64_t mix(uint64_t a, uint64_t b) {
  Rng r(a * 0x100000001b3ull ^ b);
  return r.next();
}
uint64_t hash_str(const std::string& s) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (unsigned char c : s) h = (h ^ c) * 0x100000001b3ull;
  return h;
}

struct Field {
  uint32_t off;
  uint8_t width;  // 2 or 4
};
struct Seed {
  Bytes bytes;
  std::vector<Field> fields;  // known count / size / offset fields of this seed (length-inflation targets)
  std::string label;
};

// ------------------------------------------------------------------------------------------------- findings / context
struct Format;
const Format* g_fmt = nullptr;
uint64_t g_iter = 0;
const Bytes* g_input = nullptr;
std::string g_crash_dir;
std::string g_replay_args;  // --seed / --data part of the replay command
bool g_warmup = false;      // findings name the seed (index g_iter in the format's synthetic / real pool), not an input
bool g_quiet = false;       // re-timing run: invariant findings were already checked on the first run
// --last-input FILE: the format and index of the input about to run, overwritten before every input and before every
// warm-up seed ("x         seed #1 (synthetic, unmutated)"). wasm32 has no per-input watchdog (a synchronous wasm loop
// cannot be interrupted), so after an outer timeout this names the input.
std::FILE* g_last = nullptr;
// --test-hang-seed K (ctest fuzz_formats_warmup_hang only): synthetic seed K spins forever in the warm-up, after
// --last-input names it, as a parser hang there would. UINT64_MAX = off.
uint64_t g_test_hang_seed = UINT64_MAX;

// Rewrites the --last-input line in place. Every line is padded to one width, so a shorter line never leaves the tail
// of a longer one behind.
void note_last(const char* line) {
  if (!g_last) return;
  std::rewind(g_last);
  std::fprintf(g_last, "%-47.47s\n", line);
  std::fflush(g_last);
}
void note_last_seed(const char* fmt, uint64_t index, bool real) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%-9s seed #%llu (%s, unmutated)", fmt, static_cast<unsigned long long>(index), real ? "real" : "synthetic");
  note_last(buf);
}

struct Format {
  const char* name;
  std::vector<Seed> syn, real;
  uint32_t real_den;  // a real seed is used for 1 in real_den inputs (when real seeds exist)
  uint64_t inputs = 0, accepted = 0, deep = 0, findings = 0, aux = 0;
  double max_seconds = 0, total_seconds = 0, max_cpu = 0;  // wall (first run), wall sum, CPU (minimum of the runs)
  uint64_t max_heap = 0, max_rss = 0;
  uint64_t max_input = 0;
  uint64_t retimed = 0;  // inputs whose first timing reached the limit and were timed again
};

void finding(const std::string& msg) {
  if (g_quiet) return;
  Format* f = const_cast<Format*>(g_fmt);
  ++f->findings;
  if (g_warmup) std::printf("FINDING %s seed #%llu (unmutated): %s\n", f->name, static_cast<unsigned long long>(g_iter), msg.c_str());
  else if (f->findings <= 5)
    std::printf("FINDING %s input %llu: %s (replay: --replay %s %llu%s)\n", f->name, static_cast<unsigned long long>(g_iter), msg.c_str(),
                f->name, static_cast<unsigned long long>(g_iter), g_replay_args.c_str());
  if (f->findings <= kMaxSaved && !g_crash_dir.empty() && g_input) {
    std::error_code ec;
    std::filesystem::create_directories(g_crash_dir, ec);
    const std::string p = g_crash_dir + "/" + f->name + "-" + std::to_string(g_iter) + ".bin";
    std::ofstream o(p, std::ios::binary);
    o.write(reinterpret_cast<const char*>(g_input->data()), static_cast<std::streamsize>(g_input->size()));
  }
}

#if defined(LOMN_FUZZ_ASAN)
void on_sanitizer_death() {
  if (g_fmt)
    std::fprintf(stdout, "FINDING %s input %llu: sanitizer report above (replay: --replay %s %llu%s --dump FILE)\n", g_fmt->name,
                 static_cast<unsigned long long>(g_iter), g_fmt->name, static_cast<unsigned long long>(g_iter), g_replay_args.c_str());
  std::fflush(stdout);
}
#endif
#if !defined(__EMSCRIPTEN__)
extern "C" void on_alarm(int) {
  char buf[256];
  int n = std::snprintf(buf, sizeof buf, "FINDING %s %s%llu: no result after 10 s of CPU time (hang)\n", g_fmt ? g_fmt->name : "?",
                        g_warmup ? "seed #" : "input ", static_cast<unsigned long long>(g_iter));
  if (n > 0) (void)!write(1, buf, static_cast<size_t>(n));
  _exit(4);
}
#endif

// An exactly-sized heap copy, so a read one byte past the input is a heap-buffer-overflow under ASan (a vector's
// capacity may exceed its size).
struct Exact {
  std::unique_ptr<uint8_t[]> p;
  size_t n;
  explicit Exact(const Bytes& b) : p(new uint8_t[b.size() ? b.size() : 1]), n(b.size()) {
    if (n) std::memcpy(p.get(), b.data(), n);
  }
};

struct Result {
  bool accepted = false, deep = false;
  bool aux = false;  // .x only: the token stream was entered (past the 16-byte header), printed as token_stream=
};
#define REQUIRE_INV(cond, msg) \
  do {                         \
    if (!(cond)) {             \
      finding(msg);            \
      return r;                \
    }                          \
  } while (0)

// Takes only a fuzz_frag::Fragment (fuzz_fragments.h): a raw string literal here does not compile (cleanup S3).
bool has(const std::string& s, const fuzz_frag::Fragment& f) { return fuzz_frag::matches(s, f); }

// ------------------------------------------------------------------------------------------------------------- BLK
Result run_blk(const Bytes& in, Rng& rng) {
  Result r;
  BlockFile bf;
  std::string err;
  const uint32_t count_field = rd32(in, 8);
  if (!bf.open_bytes(Bytes(in.begin(), in.end()), &err)) {
    REQUIRE_INV(!bf.is_open() && bf.entries().empty() && bf.file_size() == 0, "blk: rejected open left state");
    REQUIRE_INV(!err.empty(), "blk: rejected open without an error");
    r.deep = has(err, fuzz_frag::kBlkNameRun);  // the entry loop's own error
    return r;
  }
  r.accepted = true;
  const std::vector<BlockEntry>& es = bf.entries();
  REQUIRE_INV(es.size() == count_field, "blk: entry count differs from the header");
  REQUIRE_INV(kBlockHeaderSize + uint64_t(es.size()) * kBlockEntrySize <= in.size(), "blk: table larger than the file");
  REQUIRE_INV(bf.file_size() == in.size(), "blk: file_size differs from the input");
  r.deep = !es.empty();
  for (const BlockEntry& e : es) {
    REQUIRE_INV(e.name.size() <= kBlockNameSize && e.name.find('\0') == std::string::npos, "blk: name longer than 40 or with a NUL");
    REQUIRE_INV(e.name_terminated == (e.name.size() < kBlockNameSize), "blk: name_terminated inconsistent");
    REQUIRE_INV(e.lookup_name.compare(0, e.name.size(), e.name) == 0, "blk: lookup_name does not start with the name");
    REQUIRE_INV(!e.name_terminated || e.lookup_name == e.name, "blk: terminated name with a longer lookup_name");
    REQUIRE_INV(e.lookup_name.size() <= kBlockNameSize + kMaxNameRun, "blk: lookup_name exceeds 40 + kMaxNameRun");
  }
  if (es.empty()) return r;
  const bool sorted = bf.is_sorted();
  for (int k = 0; k < 6; ++k) {
    const size_t i = k == 0 ? 0 : k == 1 ? es.size() - 1 : rng.below(es.size());
    const int got = bf.find(es[i].name);
    REQUIRE_INV(got >= -1 && got < static_cast<int64_t>(es.size()), "blk: find index out of range");
    if (got >= 0)
      REQUIRE_INV(block_strcmp(block_basename(es[i].name), es[static_cast<size_t>(got)].lookup_name) == 0, "blk: find hit a different name");
    const bool plain = es[i].name_terminated && es[i].name.find_first_of("/\\") == std::string::npos;
    if (sorted && plain) REQUIRE_INV(got == static_cast<int>(i), "blk: sorted table, find(name) missed its entry");
  }
  for (int k = 0; k < 4; ++k) {
    const size_t i = k == 0 ? 0 : rng.below(es.size());
    const BlockEntry& e = es[i];
    std::vector<uint8_t> out;
    std::string rerr;
    if (!bf.read(i, out, &rerr)) {
      REQUIRE_INV(!rerr.empty(), "blk: rejected read without an error");
      REQUIRE_INV(out.empty(), "blk: rejected read left partial output");
      continue;
    }
    REQUIRE_INV(out.size() == e.full_size && e.full_size <= kMaxEntrySize, "blk: read size differs from full_size");
    if (e.flags & 1) {
      REQUIRE_INV(uint64_t(e.offset) + e.stored_size <= in.size(), "blk: compressed entry outside the file");
    } else {
      REQUIRE_INV(uint64_t(e.offset) + e.full_size <= in.size(), "blk: stored entry outside the file");
      REQUIRE_INV(std::equal(out.begin(), out.end(), in.begin() + e.offset), "blk: stored entry bytes differ from the file");
    }
  }
  return r;
}

// ------------------------------------------------------------------------------------------------------------ LZSS
Result run_lzss(const Bytes& in, Rng&) {
  Result r;
  const uint32_t out_size = rd32(in, 0);
  Bytes stream(in.size() >= 4 ? in.begin() + 4 : in.end(), in.end());
  Exact s(stream);
  std::vector<uint8_t> out;
  std::string err;
  if (!decode_lzss(s.p.get(), s.n, out_size, out, &err)) {
    REQUIRE_INV(!err.empty(), "lzss: rejected without an error");
    r.deep = s.n > 0 && !has(err, fuzz_frag::kLzssRatio);  // past the ratio check, into the decode loop
    REQUIRE_INV(out.empty(), "lzss: rejected stream left partial output");
    return r;
  }
  r.accepted = true;
  r.deep = s.n > 0;
  REQUIRE_INV(out.size() == out_size, "lzss: output size differs from the declared size");
  REQUIRE_INV(uint64_t(out_size) <= uint64_t(s.n) * kLzssMaxRatio, "lzss: accepted above the ratio cap");
  return r;
}

// ------------------------------------------------------------------------------------------------------------- SLB
enum class SlbKind { Obj, Pos, Light, Tex };

// Checks one typed view's invariants; returns true when it accepted with at least one record: the record loop ran.
template <typename Rec, typename Fn>
bool slb_view(const Slb& s, uint32_t count_off, uint32_t ptr_off, size_t stride, Fn parse, const char* what, bool& accepted, bool& bad) {
  std::vector<Rec> out;
  std::string err;
  accepted = parse(s, out, &err);
  if (!accepted) {
    if (err.empty()) {
      finding(std::string(what) + ": rejected without an error");
      bad = true;
    } else if (!out.empty()) {
      finding(std::string(what) + ": rejected view left records");
      bad = true;
    }
    return false;  // obj / pos / light fail only in the header (table()); their record loops cannot fail
  }
  uint32_t n = 0, base = 0;
  if (!s.read_u32(count_off, n) || !s.read_ptr(ptr_off, base) || out.size() != n || uint64_t(base) + uint64_t(n) * stride > s.data_end()) {
    finding(std::string(what) + ": accepted view inconsistent with its header (count / records pointer / data_end)");
    bad = true;
  }
  return n > 0;
}

Result run_slb(const Bytes& in, SlbKind kind) {
  Result r;
  Slb s;
  std::string err;
  if (!s.parse(Bytes(in.begin(), in.end()), &err)) {
    REQUIRE_INV(!err.empty(), "slb: rejected without an error");
    REQUIRE_INV(s.bytes().empty() && s.pointer_fields().empty() && !s.has_trailer() && s.data_end() == 0, "slb: rejected parse left state");
    return r;
  }
  REQUIRE_INV(s.bytes().size() == in.size() && s.data_end() <= in.size(), "slb: data_end past the buffer");
  const std::vector<uint32_t>& f = s.pointer_fields();
  REQUIRE_INV(std::adjacent_find(f.begin(), f.end(), [](uint32_t a, uint32_t b) { return a >= b; }) == f.end(), "slb: fields not strictly ascending");
  for (uint32_t x : f) {
    REQUIRE_INV(x % 4 == 0 && uint64_t(x) + 4 <= s.data_end(), "slb: pointer field outside the data area");
    REQUIRE_INV(rd32(in, x) < in.size(), "slb: fix-up target outside the buffer");
  }
  REQUIRE_INV(s.has_trailer() || (f.empty() && s.data_end() == in.size()), "slb: no trailer but fields / data_end set");
  bool bad = false, ok[4] = {}, deep[4] = {};
  deep[0] = slb_view<ObjRecord>(s, 8, 12, kObjRecordSize, parse_obj, "slb.obj", ok[0], bad);
  deep[1] = slb_view<PosRecord>(s, 4, 8, kPosRecordSize, parse_pos, "slb.pos", ok[1], bad);
  deep[2] = slb_view<LightRecord>(s, 4, 8, kLightRecordSize, parse_light, "slb.light", ok[2], bad);
  {
    std::vector<SpecTextureRecord> t;
    std::string terr;
    ok[3] = parse_spec_textures(s, t, &terr);
    if (ok[3]) {
      uint32_t n = 0, base = 0;
      REQUIRE_INV(s.read_u32(4, n) && s.read_ptr(8, base) && t.size() == n, "slb.tex: accepted view inconsistent with its header");
      REQUIRE_INV(n <= kMaxSpecTextures, "slb.tex: more than kMaxSpecTextures records accepted");
      REQUIRE_INV(uint64_t(base) + uint64_t(n) * kSpecTextureRecordSize <= s.data_end(), "slb.tex: records past data_end");
      for (uint32_t i = 0; i < n; ++i) {
        uint32_t name = 0;
        REQUIRE_INV(s.read_ptr(base + uint64_t(i) * kSpecTextureRecordSize, name), "slb.tex: accepted record name is not a fix-up slot");
        REQUIRE_INV(t[i].name.size() <= kMaxSpecTextureName && t[i].name.find('\0') == std::string::npos,
                    "slb.tex: name longer than kMaxSpecTextureName accepted");
      }
      deep[3] = n > 0;
    } else {
      REQUIRE_INV(!terr.empty() && t.empty(), "slb.tex: rejected view left records or no error");
      deep[3] = has(terr, fuzz_frag::kSlbTexFixup) || has(terr, fuzz_frag::kSlbTexNameLong);  // errors of the record loop
    }
  }
  if (bad) return r;
  const int k = static_cast<int>(kind);
  r.accepted = ok[k];
  r.deep = deep[k];
  return r;
}

// --------------------------------------------------------------------------------------------------------------- .x
bool x_default(const XFile& f) {
  const XMesh& m = f.mesh;
  return f.mesh_count == 0 && m.positions.empty() && m.normals.empty() && m.colors.empty() && m.uvs.empty() && m.source_vertex.empty() &&
         m.indices.empty() && m.triangle_attrib.empty() && m.subsets.empty() && m.materials.empty() && !m.has_colors && m.file_vertices == 0 &&
         m.file_faces == 0;
}

// Every error decode_mesh (xfile.cpp) can return starts with one of these; the token parser's errors end in
// " at byte N" and parse_x's header errors are "file too large" / "not a (binary) .x file" / "unsupported float size".
// Pinned against a counter at decode_mesh's entry in fix round 1 (0 mismatches; tools/research/fuzz_formats.md).
bool decode_mesh_error(const std::string& err) {
  for (const fuzz_frag::Fragment& p : fuzz_frag::kXDecodeMeshPrefixes)
    if (fuzz_frag::matches(err, p)) return true;
  return false;
}

Result run_x(const Bytes& in, Rng&) {
  Result r;
  Exact e(in);
  XFile xf;
  std::string err;
  if (!parse_x(e.p.get(), e.n, xf, &err)) {
    REQUIRE_INV(!err.empty(), "x: rejected without an error");
    REQUIRE_INV(x_default(xf), "x: rejected parse left a partial XFile");
    // aux: past parse_x's header checks (into the token stream). deep: decode_mesh was entered, i.e. the error is one of
    // decode_mesh's own (fix round 1, review I-1: "past the header" read 90-95 %, so its 5 % floor could not fail).
    r.aux = !(has(err, fuzz_frag::kXTooLarge) || has(err, fuzz_frag::kXNotX) || has(err, fuzz_frag::kXNotBinary) || has(err, fuzz_frag::kXFloatSize));
    r.deep = decode_mesh_error(err);
    return r;
  }
  r.accepted = r.aux = true;
  r.deep = xf.mesh_count > 0;  // accepted with a mesh: decode_mesh ran to the end
  const XMesh& m = xf.mesh;
  if (xf.mesh_count == 0) {
    REQUIRE_INV(x_default(xf), "x: no mesh but mesh data");
    return r;
  }
  const size_t nv = m.source_vertex.size(), ntri = m.triangle_attrib.size();
  REQUIRE_INV(nv >= m.file_vertices, "x: fewer output vertices than file vertices");
  REQUIRE_INV(m.positions.size() == 3 * nv && m.normals.size() == 3 * nv, "x: positions / normals size != 3 * vertices");
  REQUIRE_INV(m.uvs.empty() || m.uvs.size() == 2 * nv, "x: uvs size != 2 * vertices");
  REQUIRE_INV(m.colors.size() == (m.has_colors ? 4 * nv : 0), "x: colors size inconsistent with has_colors");
  for (size_t v = 0; v < nv; ++v) {
    REQUIRE_INV(m.source_vertex[v] < m.file_vertices, "x: source_vertex out of range");
    if (v < m.file_vertices) REQUIRE_INV(m.source_vertex[v] == v, "x: first file_vertices outputs are not the identity");
  }
  REQUIRE_INV(m.indices.size() == 3 * ntri, "x: indices size != 3 * triangles");
  for (uint32_t i : m.indices) REQUIRE_INV(i < nv, "x: index out of range");
  for (size_t t = 1; t < ntri; ++t) REQUIRE_INV(m.triangle_attrib[t - 1] <= m.triangle_attrib[t], "x: triangles not sorted by attribute");
  REQUIRE_INV(m.materials.size() <= kXMaxMaterials, "x: more than kXMaxMaterials materials");
  for (const XMaterial& mat : m.materials) REQUIRE_INV(mat.texture.size() <= kXMaxTextureName, "x: texture name above kXMaxTextureName");
  size_t next = 0;
  for (size_t k = 0; k < m.subsets.size(); ++k) {
    const XSubset& s = m.subsets[k];
    REQUIRE_INV(s.face_start == next && s.face_count >= 1 && uint64_t(s.face_start) + s.face_count <= ntri, "x: subset face ranges not contiguous");
    REQUIRE_INV(k == 0 || m.subsets[k - 1].attrib_id < s.attrib_id, "x: subset attrib ids not ascending");
    REQUIRE_INV(m.materials.empty() ? s.attrib_id == 0 : s.attrib_id < m.materials.size(), "x: subset attrib id without a material");
    REQUIRE_INV(uint64_t(s.vertex_start) + s.vertex_count <= nv, "x: subset vertex range out of range");
    for (size_t t = s.face_start; t < s.face_start + s.face_count; ++t) {
      REQUIRE_INV(m.triangle_attrib[t] == s.attrib_id, "x: triangle attribute differs from its subset");
      for (int j = 0; j < 3; ++j) {
        const uint32_t i = m.indices[3 * t + j];
        REQUIRE_INV(i >= s.vertex_start && i < uint64_t(s.vertex_start) + s.vertex_count, "x: index outside its subset's vertex range");
      }
    }
    next = s.face_start + s.face_count;
  }
  REQUIRE_INV(next == ntri, "x: subsets do not cover every triangle");
  return r;
}

// ---------------------------------------------------------------------------------------------------------- DDS / TGA
bool image_default(const Image& img) { return img.width == 0 && img.height == 0 && img.rgba.empty(); }

Result run_dds(const Bytes& in, Rng&) {
  Result r;
  Exact e(in);
  Image img;
  DdsInfo di;
  std::string err;
  if (!decode_dds(e.p.get(), e.n, img, &di, &err)) {
    const DdsInfo d0;
    REQUIRE_INV(!err.empty(), "dds: rejected without an error");
    REQUIRE_INV(image_default(img), "dds: rejected decode left an image");
    REQUIRE_INV(di.width == d0.width && di.height == d0.height && di.mip_count == d0.mip_count && di.format == d0.format && di.alpha_on == d0.alpha_on,
                "dds: rejected decode wrote DdsInfo");
    return r;  // every rejection happens before the block loop
  }
  r.accepted = r.deep = true;
  REQUIRE_INV(img.width == di.width && img.height == di.height, "dds: image and info dimensions differ");
  REQUIRE_INV(img.width >= 1 && img.height >= 1 && img.width <= kMaxTextureDim && img.height <= kMaxTextureDim, "dds: dimensions outside 1..4096");
  REQUIRE_INV(img.rgba.size() == uint64_t(img.width) * img.height * 4, "dds: rgba size != w*h*4");
  uint32_t max_mips = 1;
  for (uint32_t s = std::max(img.width, img.height); s > 1; s >>= 1) ++max_mips;
  REQUIRE_INV(di.mip_count <= max_mips, "dds: mip count above log2(size)+1");
  REQUIRE_INV(di.alpha_on == (di.format == DdsFormat::DXT3 || di.format == DdsFormat::A8R8G8B8), "dds: alpha_on rule");
  return r;
}

Result run_tga(const Bytes& in, Rng&) {
  Result r;
  Exact e(in);
  Image img;
  TgaInfo ti;
  std::string err;
  if (!decode_tga(e.p.get(), e.n, img, &ti, &err)) {
    REQUIRE_INV(!err.empty(), "tga: rejected without an error");
    REQUIRE_INV(image_default(img), "tga: rejected decode left an image");
    REQUIRE_INV(ti.width == 0 && ti.height == 0 && ti.image_type == 0 && ti.bits == 0, "tga: rejected decode wrote TgaInfo");
    r.deep = has(err, fuzz_frag::kTgaRleTruncated) || has(err, fuzz_frag::kTgaRleRunsPast);  // the RLE packet loop (accepted: below)
    return r;
  }
  r.accepted = r.deep = true;  // NOTE: includes accepted raw (type 2) TGAs, which never enter the RLE packet loop
  REQUIRE_INV(img.width == ti.width && img.height == ti.height, "tga: image and info dimensions differ");
  REQUIRE_INV(img.width >= 1 && img.height >= 1 && img.width <= kMaxTextureDim && img.height <= kMaxTextureDim, "tga: dimensions outside 1..4096");
  REQUIRE_INV(img.rgba.size() == uint64_t(img.width) * img.height * 4, "tga: rgba size != w*h*4");
  REQUIRE_INV((ti.image_type == 2 || ti.image_type == 10) && (ti.bits == 24 || ti.bits == 32), "tga: accepted type / bpp outside 2,10 / 24,32");
  if (ti.bits == 24)
    for (size_t i = 3; i < img.rgba.size(); i += 4) REQUIRE_INV(img.rgba[i] == 255, "tga: 24 bpp pixel with alpha != 255");
  return r;
}

// ------------------------------------------------------------------------------------------------------------- OSI
Result run_osi(const Bytes& in, Rng& rng, bool real) {
  Result r;
  OsiFile o;
  std::string err;
  if (!o.parse(Bytes(in.begin(), in.end()), &err)) {
    REQUIRE_INV(!err.empty(), "osi: rejected without an error");
    REQUIRE_INV(o.classes().empty() && o.strings().empty() && o.major() == 0 && o.minor() == 0, "osi: rejected parse left tables");
    return r;
  }
  r.accepted = r.deep = true;  // every table parsed and every method offset / symbol checked
  REQUIRE_INV(o.major() == 4, "osi: accepted a major version other than 4");
  uint64_t methods = 0;
  for (const OsiClass& k : o.classes()) {
    methods += k.methods.size();
    for (const OsiMethod& m : k.methods) REQUIRE_INV(m.offset < in.size(), "osi: method offset outside the file");
  }
  REQUIRE_INV(o.classes().size() <= kMaxOsiClasses && methods <= kMaxOsiMethods, "osi: class / method caps");
  uint64_t n = 0, nc = 0;
  std::string cerr;
  if (o.check_all(&n, &cerr, &nc)) REQUIRE_INV(n <= kMaxOsiTotalInstructions && nc <= kMaxOsiTotalCalls, "osi: check_all over its caps");
  else REQUIRE_INV(!cerr.empty(), "osi: check_all failed without an error");
  uint32_t budget = 16;
  for (const OsiClass& k : o.classes())
    for (const OsiMethod& m : k.methods) {
      if (real && rng.below(512) != 0) continue;  // the real file has 8,406 methods: sample a few per input
      if (budget == 0) return r;
      --budget;
      std::vector<OsiCall> calls(2);
      std::string eerr;
      if (!o.eval_calls(k.name, o.method_name(m), calls, &eerr)) {
        REQUIRE_INV(calls.empty() && !eerr.empty(), "osi: failed eval left calls or no error");
        continue;
      }
      REQUIRE_INV(calls.size() <= kMaxOsiCallsPerMethod, "osi: calls over kMaxOsiCallsPerMethod");
      uint64_t args = 0, sb = 0;
      for (const OsiCall& c : calls) {
        REQUIRE_INV(c.args.size() <= kMaxOsiArgc && c.offset < in.size(), "osi: argc over cap or call offset outside the file");
        args += c.args.size();
        for (const OsiValue& v : c.args) sb += v.s.size();
      }
      REQUIRE_INV(args <= kMaxOsiArgsPerMethod && sb <= kMaxOsiStringArgBytes, "osi: args / string bytes over caps");
    }
  return r;
}

// ---------------------------------------------------------------------------------------------------- seed builders
// The synthetic inputs of the unit tests (tests/test_sage_*.cpp builders, reproduced; osi_builder.h shared).
uint32_t id4(const char* s) {
  return uint32_t(uint8_t(s[0])) << 24 | uint32_t(uint8_t(s[1])) << 16 | uint32_t(uint8_t(s[2])) << 8 | uint32_t(uint8_t(s[3]));
}

// test_sage_blockfile.cpp: independent greedy Okumura LZSS encoder (brute-force match search: small inputs only).
Bytes lzss_encode(const Bytes& in) {
  uint8_t ring[4096];
  for (int i = 0; i < 4096; ++i) ring[i] = i < 0xfee ? 0x20 : 0;
  uint32_t r = 0xfee;
  Bytes out;
  size_t i = 0;
  while (i < in.size()) {
    size_t flag_at = out.size();
    out.push_back(0);
    for (int bit = 0; bit < 8 && i < in.size(); ++bit) {
      size_t best_len = 0;
      uint32_t best_pos = 0;
      for (uint32_t p = 0; p < 4096; ++p) {
        size_t k = 0;
        uint8_t emitted[18];
        while (k < 18 && i + k < in.size()) {
          uint32_t idx = (p + k) & 0xfff;
          uint32_t d = (idx - r) & 0xfff;
          uint8_t v = d < k ? emitted[d] : ring[idx];
          if (v != in[i + k]) break;
          emitted[k] = v;
          ++k;
        }
        if (k > best_len) {
          best_len = k;
          best_pos = p;
        }
      }
      if (best_len >= 3) {
        out.push_back(static_cast<uint8_t>(best_pos & 0xff));
        out.push_back(static_cast<uint8_t>(((best_pos >> 4) & 0xf0) | (best_len - 3)));
        for (size_t k = 0; k < best_len; ++k) {
          ring[r] = in[i + k];
          r = (r + 1) & 0xfff;
        }
        i += best_len;
      } else {
        out[flag_at] |= static_cast<uint8_t>(1u << bit);
        out.push_back(in[i]);
        ring[r] = in[i];
        r = (r + 1) & 0xfff;
        ++i;
      }
    }
  }
  return out;
}

Bytes fox(size_t n) {  // test_sage_blockfile.cpp round-trip data
  Bytes data;
  uint32_t seed = 12345;
  for (size_t i = 0; i < n; ++i) {
    seed = seed * 1103515245u + 12345u;
    data.push_back(i % 97 < 60 ? static_cast<uint8_t>("the quick brown fox "[i % 20]) : static_cast<uint8_t>(seed >> 24));
  }
  return data;
}

Bytes lzss_seed(uint32_t declared, const Bytes& stream) {
  Bytes b;
  put32(b, declared);
  b.insert(b.end(), stream.begin(), stream.end());
  return b;
}

// Known length fields of a seed, found by walking its structure (bounded; best effort).
std::vector<Field> blk_fields(const Bytes& b) {
  std::vector<Field> f = {{8, 4}};
  const uint32_t n = std::min<uint32_t>(rd32(b, 8), 64);
  for (uint32_t i = 0; i < n && 12 + 56 * (i + 1) <= b.size(); ++i)
    for (uint32_t k : {40u, 44u, 48u, 52u}) f.push_back({12 + 56 * i + k, 4});
  return f;
}
std::vector<Field> slb_fields(const Bytes& b, SlbKind kind) {
  std::vector<Field> f;
  for (uint32_t o : kind == SlbKind::Obj ? std::vector<uint32_t>{4, 8, 12} : std::vector<uint32_t>{0, 4, 8})
    if (o + 4 <= b.size()) f.push_back({o, 4});
  if (b.size() >= 12) {
    const uint32_t m = (static_cast<uint32_t>(b.size()) - 4) & ~3u;
    if (rd32(b, m) == kSlbMagic) {
      const uint32_t count = rd32(b, m - 4);
      f.push_back({m - 4, 4});
      for (uint32_t k = 1; k <= count && k <= 64 && m >= 4 + 4 * k + 4; ++k) {
        f.push_back({m - 4 - 4 * k, 4});
        const uint32_t field = rd32(b, m - 4 - 4 * k) & ~3u;
        if (uint64_t(field) + 4 <= b.size()) f.push_back({field, 4});
      }
    }
  }
  return f;
}
std::vector<Field> x_fields(const Bytes& b) {
  std::vector<Field> f;
  size_t p = 16;
  const size_t fs = b.size() >= 16 && std::memcmp(b.data() + 12, "0064", 4) == 0 ? 8 : 4;
  while (p + 2 <= b.size() && f.size() < 256) {
    const uint16_t t = static_cast<uint16_t>(b[p] | b[p + 1] << 8);
    p += 2;
    uint64_t skip = 0;
    if (t == 1 || t == 2 || t == 6 || t == 7) {
      if (p + 4 > b.size()) break;
      f.push_back({static_cast<uint32_t>(p), 4});
      const uint64_t n = rd32(b, p);
      skip = 4 + (t == 1 ? n : t == 2 ? n + 2 : t == 6 ? 4 * n : fs * n);
    } else if (t == 3) {
      skip = 4;
    } else if (t == 5) {
      skip = 16;
    }
    if (skip > b.size() - p) break;
    p += static_cast<size_t>(skip);
  }
  return f;
}
std::vector<Field> osi_fields(const Bytes& b) {
  std::vector<Field> f = {{4, 2}, {6, 2}};
  size_t p = 8;
  for (int table = 0; table < 2; ++table) {  // strings, globals: u16 count, {u8 len, chars, NUL}
    if (p + 2 > b.size()) return f;
    f.push_back({static_cast<uint32_t>(p), 2});
    const uint32_t n = b[p] | b[p + 1] << 8;
    p += 2;
    for (uint32_t i = 0; i < n; ++i) {
      if (p >= b.size()) return f;
      p += 2u + b[p];
    }
  }
  if (p + 2 > b.size()) return f;
  f.push_back({static_cast<uint32_t>(p), 2});  // function table: u16 count, {u8 len, name, u32 offset, u16 params}
  const uint32_t nf = b[p] | b[p + 1] << 8;
  p += 2;
  for (uint32_t i = 0; i < nf && f.size() < 128; ++i) {
    if (p >= b.size()) return f;
    p += 1u + b[p];
    if (p + 6 > b.size()) return f;
    f.push_back({static_cast<uint32_t>(p), 4});
    f.push_back({static_cast<uint32_t>(p + 4), 2});
    p += 6;
  }
  if (p + 2 <= b.size()) f.push_back({static_cast<uint32_t>(p), 2});  // class count
  return f;
}
std::vector<Field> dds_fields(const Bytes&) { return {{12, 4}, {16, 4}, {24, 4}, {28, 4}, {88, 4}}; }
std::vector<Field> tga_fields(const Bytes&) { return {{5, 2}, {12, 2}, {14, 2}}; }

// --- synthetic seeds per format
std::vector<Bytes> syn_blk() {
  using sage_test::BlkEntrySpec;
  using sage_test::make_blk;
  using sage_test::stored;
  std::vector<Bytes> v;
  v.push_back(make_blk({stored("a.dds", "alpha"), stored("b.x", "bravo-bravo"), stored("lev1/c.tga", "charlie")}));
  const Bytes data = fox(300);
  v.push_back(make_blk({stored("a.bin", "plain"), BlkEntrySpec{"c.bin", lzss_encode(data), static_cast<uint32_t>(data.size()), 3}}));
  std::string longname(36, 'q');
  longname += ".dds";  // exactly 40 characters: unterminated, runs on into mBlkSize
  v.push_back(make_blk({stored("a.dds", "1"), BlkEntrySpec{longname, Bytes(0x80, 7), 0x80, 2}, stored("zz.dds", "2")}));
  std::vector<BlkEntrySpec> many;  // a 16-entry sorted table (896-byte table)
  for (int i = 0; i < 16; ++i) many.push_back(stored("e" + std::to_string(10 + i) + ".slb", std::string(static_cast<size_t>(4 + i), 'a' + i)));
  v.push_back(make_blk(many));
  return v;
}
std::vector<Bytes> syn_lzss() {
  std::vector<Bytes> v;
  v.push_back(lzss_seed(8, {0xff, 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'}));
  v.push_back(lzss_seed(3, {0x00, 0x00, 0x00}));
  v.push_back(lzss_seed(6, {0x03, 'a', 'b', 0xee, 0xf1}));
  v.push_back(lzss_seed(18, {0x00, 0x00, 0x0f}));
  const Bytes data = fox(600);
  v.push_back(lzss_seed(static_cast<uint32_t>(data.size()), lzss_encode(data)));
  return v;
}
Bytes make_obj(const std::vector<std::pair<const char*, uint32_t>>& recs) {
  Bytes b;
  put32(b, id4("????"));
  put32(b, static_cast<uint32_t>(recs.size()));
  put32(b, static_cast<uint32_t>(recs.size()));
  put32(b, 0x16);
  put16(b, 0);
  put32(b, 0);
  for (size_t i = 0; i < recs.size(); ++i) {
    put32(b, id4(recs[i].first));
    putf(b, 1.0f + static_cast<float>(i));
    putf(b, 2.0f);
    putf(b, 3.0f);
    putf(b, 0.5f);
    putf(b, 0.25f);
    putf(b, 0.125f);
    put32(b, 0xdeadbeefu);
    for (int k = 0; k < 6; ++k) putf(b, 10.0f + k);
    put32(b, recs[i].second);
  }
  put16(b, 0);
  put32(b, 12);
  put32(b, 1);
  put32(b, kSlbMagic);
  return b;
}
Bytes make_pos() {
  Bytes p;
  put32(p, 0);
  put32(p, 2);
  put32(p, 12);
  for (uint32_t i = 0; i < 2; ++i) {
    put32(p, id4(i ? "strt" : "pkup"));
    putf(p, 1.5f);
    putf(p, -2.5f);
    putf(p, 3.5f);
    put32(p, i ? 0x1000u : 0x60u);
  }
  put32(p, 8);
  put32(p, 1);
  put32(p, kSlbMagic);
  return p;
}
Bytes make_light(int n) {
  Bytes l;
  put32(l, 0);
  put32(l, static_cast<uint32_t>(n));
  put32(l, 12);
  for (int i = 0; i < n; ++i) {
    put32(l, id4(i ? "lmp1" : "sun0"));
    for (float f : {0.f, -1.f, 0.f, 500.f, 2.f, 0.9f, 0.8f, 0.7f, 1.f}) putf(l, f);
    put32(l, i ? 0x40u : 0x80u);
  }
  put32(l, 8);
  put32(l, 1);
  put32(l, kSlbMagic);
  return l;
}
Bytes make_textures(const std::vector<std::pair<std::string, float>>& recs) {
  Bytes b;
  put32(b, static_cast<uint32_t>(recs.size()));
  put32(b, static_cast<uint32_t>(recs.size()));
  put32(b, 12);
  uint32_t str = static_cast<uint32_t>(12 + 20 * recs.size());
  std::vector<uint32_t> slots = {8};
  for (const auto& r : recs) {
    slots.push_back(static_cast<uint32_t>(b.size()));
    put32(b, str);
    putf(b, r.second);
    putf(b, 0.25f);
    putf(b, 0.0f);
    put32(b, 3);
    str += static_cast<uint32_t>(r.first.size()) + 2;
  }
  for (const auto& r : recs) {
    put8(b, static_cast<uint8_t>(r.first.size()));
    putstr(b, r.first);
    put8(b, 0);
  }
  while (b.size() % 4) put8(b, 0);
  for (size_t i = slots.size(); i-- > 0;) put32(b, slots[i]);
  put32(b, static_cast<uint32_t>(slots.size()));
  put32(b, kSlbMagic);
  return b;
}

struct X {  // test_sage_xfile.cpp binary .x token writer
  Bytes b;
  X() { putstr(b, "xof 0303bin 0032"); }
  X& name(const std::string& s) { put16(b, 1); put32(b, static_cast<uint32_t>(s.size())); putstr(b, s); return *this; }
  X& str(const std::string& s) { put16(b, 2); put32(b, static_cast<uint32_t>(s.size())); putstr(b, s); put16(b, 20); return *this; }
  X& guid() { put16(b, 5); for (int i = 0; i < 16; ++i) put8(b, static_cast<uint8_t>(i)); return *this; }
  X& ints(std::initializer_list<uint32_t> v) { put16(b, 6); put32(b, static_cast<uint32_t>(v.size())); for (uint32_t x : v) put32(b, x); return *this; }
  X& floats(std::initializer_list<float> v) { put16(b, 7); put32(b, static_cast<uint32_t>(v.size())); for (float x : v) putf(b, x); return *this; }
  X& open() { put16(b, 10); return *this; }
  X& close() { put16(b, 11); return *this; }
  X& tok(uint16_t t) { put16(b, t); return *this; }
  X& obj(const std::string& type) { return name(type).open(); }
  X& ref(const std::string& n) { return open().name(n).close(); }
  X& templ() {
    tok(31).name("Mesh").open().guid();
    tok(41).name("nVertices").tok(20);
    tok(52).name("Vector").name("vertices").tok(14).name("nVertices").tok(15).tok(20);
    tok(14).tok(18).tok(18).tok(18).tok(15);
    return close();
  }
};
void x_mesh(X& x, bool with_colors) {
  x.name("Mesh").name("body").guid().open();
  x.ints({5}).floats({0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 0.5f, 0.5f, 1});
  x.ints({3, 3, 0, 1, 2, 4, 0, 2, 3, 4, 3, 1, 3, 2});
  x.obj("MeshNormals").ints({2}).floats({0, 0, 1, 0, 1, 0}).ints({3, 3, 0, 0, 0, 4, 1, 0, 0, 0, 3, 0, 0, 0}).close();
  x.obj("MeshTextureCoords").ints({5}).floats({0, 0, 1, 0, 1, 1, 0, 1, 0.5f, 0.5f}).close();
  if (with_colors) {
    x.obj("MeshVertexColors").ints({5});
    for (uint32_t i = 0; i < 5; ++i) x.ints({i}).floats({0.1f * i, 0.2f, 0.3f, 1});
    x.close();
  }
  x.obj("MeshMaterialList").ints({2, 3, 1, 0, 1});
  x.obj("Material").floats({1, 0, 0, 1, 8, 0.5f, 0.5f, 0.5f, 0, 0, 0}).close();
  x.obj("Material").floats({0, 1, 0, 0.5f, 4, 0, 0, 0, 0.1f, 0.2f, 0.3f});
  x.obj("TextureFilename").str("on_lev_20xx_ocean").close();
  x.close();
  x.close();
  x.close();
}
std::vector<Bytes> syn_x() {
  std::vector<Bytes> v;
  {
    X x;  // sample(): template, Frame, FrameTransformMatrix, the full mesh
    x.templ();
    x.name("Frame").name("root").open();
    x.obj("FrameTransformMatrix").floats({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}).close();
    x_mesh(x, true);
    x.close();
    v.push_back(x.b);
  }
  {
    X x;  // two meshes (the last wins), the second without vertex colours
    x_mesh(x, true);
    x_mesh(x, false);
    v.push_back(x.b);
  }
  {
    X x;  // material references (many_refs(2, 1, 3))
    x.obj("Mesh").ints({3}).floats({0, 0, 0, 1, 0, 0, 1, 1, 0}).ints({1, 3, 0, 1, 2});
    x.name("Material").name("M").open().floats({1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0});
    x.obj("A").close().obj("A").close();
    x.close();
    x.obj("MeshMaterialList").ints({1, 1, 0}).ref("M").ref("M").ref("M").close();
    x.close();
    v.push_back(x.b);
  }
  {
    X x;  // a Mesh without MeshMaterialList
    x.obj("Mesh").ints({3}).floats({0, 0, 0, 1, 0, 0, 0, 1, 0}).ints({1, 3, 0, 1, 2}).close();
    v.push_back(x.b);
  }
  return v;
}
Bytes dds_header(uint32_t w, uint32_t h, const char* fourcc) {
  Bytes b;
  putstr(b, "DDS ");
  put32(b, 124);
  put32(b, 0x1007);
  put32(b, h);
  put32(b, w);
  put32(b, 0);
  put32(b, 0);
  put32(b, 0);
  for (int i = 0; i < 11; ++i) put32(b, 0);
  put32(b, 32);
  if (fourcc) {
    put32(b, 0x4);
    putstr(b, fourcc);
    for (int i = 0; i < 5; ++i) put32(b, 0);
  } else {
    put32(b, 0x41);
    put32(b, 0);
    put32(b, 32);
    put32(b, 0x00ff0000u);
    put32(b, 0x0000ff00u);
    put32(b, 0x000000ffu);
    put32(b, 0xff000000u);
  }
  put32(b, 0x1000);
  for (int i = 0; i < 4; ++i) put32(b, 0);
  return b;
}
std::vector<Bytes> syn_dds() {
  std::vector<Bytes> v;
  Bytes d1 = dds_header(5, 3, "DXT1");
  for (int i = 0; i < 2; ++i) {
    uint8_t blk[8] = {0x00, 0xf8, 0x1f, 0x00, 0, 0, 0, 0};
    if (i == 1) blk[4] = 0x01;
    d1.insert(d1.end(), blk, blk + 8);
  }
  v.push_back(d1);
  Bytes d3 = dds_header(4, 4, "DXT3");
  d3.resize(d3.size() + 16, 0xff);
  v.push_back(d3);
  Bytes d5 = dds_header(8, 4, "DXT5");
  for (uint8_t k = 0; k < 32; ++k) d5.push_back(static_cast<uint8_t>(k * 37));
  v.push_back(d5);
  Bytes argb = dds_header(2, 1, nullptr);
  for (uint8_t x : {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88}) argb.push_back(x);
  v.push_back(argb);
  return v;
}
Bytes tga_header(uint8_t type, uint16_t w, uint16_t h, uint8_t bpp, uint8_t desc) {
  Bytes b = {3, 0, type, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  put16(b, w);
  put16(b, h);
  put8(b, bpp);
  put8(b, desc);
  putstr(b, "abc");
  return b;
}
std::vector<Bytes> syn_tga() {
  std::vector<Bytes> v;
  Bytes t = tga_header(2, 2, 2, 24, 0);
  for (uint8_t x : {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}) t.push_back(x);
  v.push_back(t);
  Bytes u = tga_header(2, 2, 1, 32, 0x28);
  for (uint8_t x : {1, 2, 3, 4, 5, 6, 7, 8}) u.push_back(x);
  v.push_back(u);
  Bytes r = tga_header(10, 4, 1, 32, 0x28);
  for (uint8_t x : {0x82, 10, 20, 30, 40, 0x00, 1, 2, 3, 4}) r.push_back(x);
  v.push_back(r);
  Bytes r24 = tga_header(10, 3, 2, 24, 0x10);  // RLE 24 bpp, right-to-left
  for (uint8_t x : {0x81, 1, 2, 3, 0x01, 4, 5, 6, 7, 8, 9, 0x80, 9, 9, 9}) r24.push_back(x);
  v.push_back(r24);
  // M1a-4 finding (real-seeded input tga 49594, seed 1): a 4096x4096x32 RLE header over a few bytes of packets. The
  // decoder must reject it before allocating the 64 MiB pixel buffer.
  Bytes big = tga_header(10, 4096, 4096, 32, 0x28);
  for (uint8_t x : {0xff, 1, 2, 3, 4, 0x00, 5, 6, 7, 8}) big.push_back(x);
  v.push_back(big);
  return v;
}
std::vector<Bytes> syn_osi() {
  std::vector<Bytes> v = {lomn::test::statt_osi()};
  lomn::test::OsiSpec s;  // fuzz_osi.cpp's cap-edge program
  s.strings.push_back(std::string(255, 'x'));
  s.code = {0x46, 4, 0, 0x46, 4, 0, 0x82, 0, 0, 1, 0, 2, 0x30, 0x53, 2, 0, 0x45, 0x45, 0x31, 2, 0x52};
  s.functions = 3;
  s.function_stride = 3;
  v.push_back(lomn::test::build_osi(s));
  return v;
}

// --------------------------------------------------------------------------------------------------------- mutation
void mutate(Rng& r, const Seed& seed, bool real, Bytes& b) {
  static const uint8_t interesting[] = {0, 1, 2, 3, 4, 0x10, 0x1f, 0x20, 0x3f, 0x40, 0x7f, 0x80, 0xc0, 0xee, 0xfe, 0xff};
  // Weights out of 16: truncate 1, flip 4, length field 3, duplicate 2, delete 2, interesting 2, run 1, insert 1.
  static const uint8_t kinds[16] = {0, 1, 1, 1, 1, 2, 2, 2, 3, 3, 4, 4, 5, 5, 6, 7};
  b = seed.bytes;
  const uint32_t nmut = 1 + r.below(real ? 3 : 4);
  const size_t limit = seed.bytes.size() * 2 + 65536;
  for (uint32_t m = 0; m < nmut; ++m) {
    const uint8_t kind = kinds[r.below(16)];
    if (b.empty()) {
      b.push_back(static_cast<uint8_t>(r.below(256)));
      continue;
    }
    const size_t at = r.below(b.size());
    switch (kind) {
      case 0:  // truncation at a random prefix
        b.resize(at);
        break;
      case 1:  // bit flip
        b[at] ^= static_cast<uint8_t>(1u << r.below(8));
        break;
      case 2: {  // length-field inflation
        Field f{static_cast<uint32_t>(at & ~size_t(3)), static_cast<uint8_t>(r.below(2) ? 4 : 2)};
        if (!seed.fields.empty() && r.below(4) != 0) f = seed.fields[r.below(seed.fields.size())];
        if (uint64_t(f.off) + f.width > b.size()) break;
        const uint64_t size = b.size(), rem = size - f.off, k = r.below(9);
        uint64_t v = 0;
        switch (r.below(8)) {
          case 0: v = size + k; break;
          case 1: v = size - std::min(size, k); break;
          case 2: v = rem + k; break;
          case 3: v = rem - std::min(rem, k); break;
          case 4: v = 0; break;
          case 5: v = 0xffffffffull; break;
          case 6: v = f.width == 4 ? 0x80000000ull : 0x8000; break;
          default: v = 1ull << r.below(f.width * 8); break;
        }
        for (uint32_t i = 0; i < f.width; ++i) b[f.off + i] = static_cast<uint8_t>(v >> (8 * i));
        break;
      }
      case 3: {  // chunk duplication
        if (b.size() > limit) break;
        const size_t len = std::min<size_t>(1 + r.below(64), b.size() - at);
        const Bytes chunk(b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(at + len));
        for (uint32_t k = 1 + r.below(16); k > 0; --k) b.insert(b.begin() + static_cast<std::ptrdiff_t>(at), chunk.begin(), chunk.end());
        break;
      }
      case 4: {  // chunk deletion
        const size_t len = std::min<size_t>(1 + r.below(r.below(4) ? 64 : 1 + b.size() / 4), b.size() - at);
        b.erase(b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(at + len));
        break;
      }
      case 5:  // interesting byte
        b[at] = interesting[r.below(sizeof interesting)];
        break;
      case 6: {  // a run of one non-NUL byte (unterminated names, strings without NUL)
        const size_t len = std::min<size_t>(1 + r.below(1024), b.size() - at);
        std::memset(b.data() + at, 1 + r.below(255), len);
        break;
      }
      default:  // random byte insert
        if (b.size() <= limit) b.insert(b.begin() + static_cast<std::ptrdiff_t>(at), static_cast<uint8_t>(r.below(256)));
        break;
    }
  }
}

// ---------------------------------------------------------------------------------------------------- real seeds
std::string lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return s;
}
bool ends_with(const std::string& s, const char* suf) {
  const size_t n = std::strlen(suf);
  return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}
bool read_range(const std::string& path, uint64_t off, uint64_t n, Bytes& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  f.seekg(static_cast<std::streamoff>(off));
  out.resize(static_cast<size_t>(n));
  f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(n));
  return static_cast<uint64_t>(f.gcount()) == n;
}

struct Candidate {
  std::string path;  // relative to the install (for labels) and absolute (for reads)
  std::string abs;
  int entry = -1;    // BLK entry index, -1 = a loose file
  uint64_t size = 0;
};

// Picks k of the (sorted) candidates with a deterministic RNG.
std::vector<Candidate> sample(std::vector<Candidate> c, size_t k, uint64_t seed) {
  Rng r(seed);
  for (size_t i = 0; i < c.size() && i < k; ++i) std::swap(c[i], c[i + r.below(c.size() - i)]);
  if (c.size() > k) c.resize(k);
  return c;
}

bool read_candidate(const Candidate& c, Bytes& out) {
  if (c.entry < 0) return read_range(c.abs, 0, c.size, out);
  BlockFile bf;
  std::string err;
  std::error_code ec;
  const uint64_t fsz = std::filesystem::file_size(c.abs, ec);
  if (ec) return false;
  const std::string p = c.abs;
  if (!bf.open(fsz, [p](uint64_t off, uint64_t n, std::vector<uint8_t>& o) { return read_range(p, off, n, o); }, &err)) return false;
  return bf.read(static_cast<size_t>(c.entry), out, &err);
}

// Returns false (with a message) when the install cannot be walked.
bool load_real(const std::string& install, uint64_t seed, size_t per_format, std::map<std::string, std::vector<Seed>>& out) {
  std::error_code ec;
  const std::filesystem::path root = std::filesystem::canonical(install, ec);
  if (ec || !std::filesystem::is_directory(root, ec)) {
    std::printf("fuzz_formats: --data %s is not a directory\n", install.c_str());
    return false;
  }
  std::vector<std::pair<std::string, std::string>> files;  // relative, absolute
  for (auto it = std::filesystem::recursive_directory_iterator(root, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    std::error_code e2;
    if (!it->is_regular_file(e2)) continue;
    files.emplace_back(std::filesystem::relative(it->path(), root, e2).generic_string(), it->path().string());
  }
  if (ec) {
    std::printf("fuzz_formats: walking %s failed: %s\n", install.c_str(), ec.message().c_str());
    return false;
  }
  std::sort(files.begin(), files.end());
  std::map<std::string, std::vector<Candidate>> cand;
  for (const auto& f : files) {
    const std::string rel = lower(f.first);
    std::error_code e2;
    const uint64_t size = std::filesystem::file_size(f.second, e2);
    if (e2) continue;
    if (ends_with(rel, ".blk")) {
      if (size <= kSeedMax) cand["blk"].push_back({f.first, f.second, -1, size});
      BlockFile bf;
      std::string err;
      const std::string p = f.second;
      if (!bf.open(size, [p](uint64_t off, uint64_t n, std::vector<uint8_t>& o) { return read_range(p, off, n, o); }, &err)) continue;
      for (size_t i = 0; i < bf.entries().size(); ++i) {
        const BlockEntry& e = bf.entries()[i];
        const std::string n = lower(e.name);
        const uint64_t esz = (e.flags & 1) ? e.stored_size : e.full_size;
        if (esz == 0 || esz > kSeedMax) continue;
        Candidate c{f.first + ":" + e.name, f.second, static_cast<int>(i), esz};
        if (ends_with(n, ".x")) cand["x"].push_back(c);
        else if (ends_with(n, ".dds")) cand["dds"].push_back(c);
        else if (ends_with(n, ".tga")) cand["tga"].push_back(c);
        else if (n == "textures.slb") cand["slb.tex"].push_back(c);
        else if (ends_with(n, "_obj.slb") && !is_cinematic_obj(n)) cand["slb.obj"].push_back(c);
        else if (ends_with(n, "_pos.slb")) cand["slb.pos"].push_back(c);
        else if (ends_with(n, "_light.slb")) cand["slb.light"].push_back(c);
        if (ends_with(n, ".x") || ends_with(n, ".dds") || ends_with(n, ".tga") || ends_with(n, ".slb")) cand["lzss-src"].push_back(c);
      }
    } else if (ends_with(rel, ".tga") && size <= kSeedMax) {
      cand["tga"].push_back({f.first, f.second, -1, size});
    } else if (ends_with(rel, ".osi") && size <= kMaxOsiBytes) {
      cand["osi"].push_back({f.first, f.second, -1, size});
    }
  }
  for (auto& kv : cand) {
    const bool lz = kv.first == "lzss-src";
    const size_t k = lz ? std::min<size_t>(per_format, 8) : per_format;
    for (const Candidate& c : sample(kv.second, k, mix(seed, hash_str(kv.first)))) {
      Seed s;
      if (!read_candidate(c, s.bytes)) continue;
      if (lz) {  // the Beta install has no compressed entry: encode a real prefix with the independent encoder
        Bytes prefix(s.bytes.begin(), s.bytes.begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(s.bytes.size(), 768)));
        s.bytes = lzss_seed(static_cast<uint32_t>(prefix.size()), lzss_encode(prefix));
        s.label = "lzss(" + c.path + "[0:" + std::to_string(prefix.size()) + "])";
        out["lzss"].push_back(std::move(s));
      } else {
        s.label = c.path;
        out[kv.first].push_back(std::move(s));
      }
    }
  }
  return true;
}

void add_fields(const std::string& fmt, Seed& s) {
  if (fmt == "blk") s.fields = blk_fields(s.bytes);
  else if (fmt == "lzss") s.fields = {{0, 4}};
  else if (fmt == "slb.obj") s.fields = slb_fields(s.bytes, SlbKind::Obj);
  else if (fmt == "slb.pos") s.fields = slb_fields(s.bytes, SlbKind::Pos);
  else if (fmt == "slb.light") s.fields = slb_fields(s.bytes, SlbKind::Light);
  else if (fmt == "slb.tex") s.fields = slb_fields(s.bytes, SlbKind::Tex);
  else if (fmt == "x") s.fields = x_fields(s.bytes);
  else if (fmt == "dds") s.fields = dds_fields(s.bytes);
  else if (fmt == "tga") s.fields = tga_fields(s.bytes);
  else if (fmt == "osi") s.fields = osi_fields(s.bytes);
}

Result run_one(const std::string& fmt, const Bytes& in, Rng& r, bool real) {
  if (fmt == "blk") return run_blk(in, r);
  if (fmt == "lzss") return run_lzss(in, r);
  if (fmt == "slb.obj") return run_slb(in, SlbKind::Obj);
  if (fmt == "slb.pos") return run_slb(in, SlbKind::Pos);
  if (fmt == "slb.light") return run_slb(in, SlbKind::Light);
  if (fmt == "slb.tex") return run_slb(in, SlbKind::Tex);
  if (fmt == "x") return run_x(in, r);
  if (fmt == "dds") return run_dds(in, r);
  if (fmt == "tga") return run_tga(in, r);
  return run_osi(in, r, real);
}

// CPU time used so far: the calling thread natively, the Node process as wasm32 (Node 20 has no per-thread counter).
double cpu_seconds() {
#if defined(__EMSCRIPTEN__)
  return EM_ASM_DOUBLE({
    const u = process.cpuUsage();
    return (u.user + u.system) / 1e6;
  });
#else
  timespec ts{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
#endif
}

#if !defined(__EMSCRIPTEN__)
void watchdog(int seconds) {  // CPU-time watchdog (ITIMER_PROF / SIGPROF: user + system CPU, so a loop spinning in the
  itimerval t{};               // kernel trips it too); a hang burns CPU, a busy host does not trip it
  t.it_value.tv_sec = seconds;
  setitimer(ITIMER_PROF, &t, nullptr);
}
#endif

// Runs one input with the resource invariants (time, heap and process-memory growth, heap released): shared by the
// main loop and --replay, so a replayed input reports the same findings.
// Time is checked as CPU time (M1a-4, measured): wall time on a shared host measures the other load. One real-seeded
// OSI input took 0.3-0.6 s as ASan wasm32 at loadavg ~7 and 2.04-2.34 s at loadavg ~27; at loadavg 35-73 on 8 cores
// even native OSI inputs reached 1.94 s of wall time. A hang or a quadratic path burns CPU on every run; an input at
// or over the limit is still re-run up to twice (same bytes, same RNG, findings muted) and the minimum counts, so a
// one-off CPU spike (e.g. V8 background compilation, counted in Node's process CPU) is not a finding either.
Result run_measured(Format& f, const Bytes& b, Rng& r, bool is_real) {
  if (b.size() > f.max_input) f.max_input = b.size();
  const size_t live0 = heap::live;
  heap::peak = live0;
  const uint64_t rss0 = process_peak_bytes();
  const Rng r0 = r;  // a re-timing run repeats exactly this input's work
#if !defined(__EMSCRIPTEN__)
  watchdog(10);
#endif
  const auto w0 = std::chrono::steady_clock::now();
  double c0 = cpu_seconds();
  const Result res = run_one(f.name, b, r, is_real);
  double cpu = cpu_seconds() - c0;
  const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
  for (int k = 0; k < 2 && cpu >= kMaxInputSeconds; ++k) {
    if (k == 0) ++f.retimed;
    Rng r1 = r0;
    g_quiet = true;
#if !defined(__EMSCRIPTEN__)
    watchdog(10);
#endif
    c0 = cpu_seconds();
    (void)run_one(f.name, b, r1, is_real);
    cpu = std::min(cpu, cpu_seconds() - c0);
    g_quiet = false;
  }
#if !defined(__EMSCRIPTEN__)
  watchdog(0);
#endif
  const uint64_t grow = heap::peak - live0, rss = process_peak_bytes() - rss0;
  ++f.inputs;
  f.accepted += res.accepted;
  f.deep += res.deep;
  f.aux += res.aux;
  f.total_seconds += wall;
  f.max_seconds = std::max(f.max_seconds, wall);
  f.max_cpu = std::max(f.max_cpu, cpu);
  f.max_heap = std::max<uint64_t>(f.max_heap, grow);
  f.max_rss = std::max(f.max_rss, rss);
  if (cpu >= kMaxInputSeconds) finding("CPU time " + std::to_string(cpu) + " s >= 2 s on 3 runs (minimum shown)");
  if (grow > kMaxGrowth) finding("heap growth " + std::to_string(grow) + " bytes above the per-input limit");
  if (rss > kMaxGrowth) finding("process memory growth " + std::to_string(rss) + " bytes above the per-input limit");
  if (heap::live != live0) finding("heap not released after the input (" + std::to_string(heap::live - live0) + " bytes)");
  return res;
}

// The input `iter` of format `f`: a pure function of (--seed, format, iter, seed pool), so any input can be replayed.
void make_input(const Format& f, uint64_t seed, uint64_t iter, Rng& r, Bytes& b, bool& real) {
  r = Rng(mix(mix(seed, hash_str(f.name)), iter));
  real = !f.real.empty() && r.below(f.real_den) == 0;
  const std::vector<Seed>& pool = real ? f.real : f.syn;
  mutate(r, pool[r.below(pool.size())], real, b);
}

int usage() {
  std::printf(
      "usage: lomn_fuzz_formats [--n N] [--seed S] [--data INSTALL] [--formats a,b] [--real-per-format K] [--crash-dir D]\n"
      "                         [--min-deep F] [--smoke] [--replay FMT ITER --dump FILE] [--last-input FILE]\n"
      "                         [--test-hang-seed K]   (test only: synthetic seed K hangs in the warm-up)\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  uint64_t n = 20000, seed = 1, replay_iter = 0;
  size_t per_format = 16;
  double min_deep = 0.05;
  bool smoke = false;
  std::string data, only, replay_fmt, dump;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto val = [&](std::string& out) {
      if (i + 1 >= argc) return false;
      out = argv[++i];
      return true;
    };
    std::string v;
    if (a == "--smoke") smoke = true;
    else if (a == "--n" && val(v)) n = std::strtoull(v.c_str(), nullptr, 10);
    else if (a == "--seed" && val(v)) seed = std::strtoull(v.c_str(), nullptr, 10);
    else if (a == "--data" && val(v)) data = v;
    else if (a == "--formats" && val(v)) only = "," + v + ",";
    else if (a == "--real-per-format" && val(v)) per_format = std::strtoull(v.c_str(), nullptr, 10);
    else if (a == "--crash-dir" && val(v)) g_crash_dir = v;
    else if (a == "--min-deep" && val(v)) min_deep = std::strtod(v.c_str(), nullptr);
    else if (a == "--replay" && val(replay_fmt) && val(v)) replay_iter = std::strtoull(v.c_str(), nullptr, 10);
    else if (a == "--dump" && val(v)) dump = v;
    else if (a == "--last-input" && val(v)) {
      g_last = std::fopen(v.c_str(), "w");
      if (!g_last) return usage();
    }
    else if (a == "--test-hang-seed" && val(v)) g_test_hang_seed = std::strtoull(v.c_str(), nullptr, 10);
    else return usage();
  }
  if (smoke) data.clear();  // the ctest subset uses synthetic seeds only
  g_replay_args = " --seed " + std::to_string(seed) + (data.empty() ? "" : " --data <install>");

  std::vector<Format> formats = {
      {"blk", {}, {}, 2},     {"lzss", {}, {}, 2},      {"slb.obj", {}, {}, 2}, {"slb.pos", {}, {}, 2}, {"slb.light", {}, {}, 2},
      {"slb.tex", {}, {}, 2}, {"x", {}, {}, 2},         {"dds", {}, {}, 2},     {"tga", {}, {}, 2},     {"osi", {}, {}, 16}};
  std::map<std::string, std::vector<Bytes>> syn = {{"blk", syn_blk()},
                                                   {"lzss", syn_lzss()},
                                                   {"slb.obj", {make_obj({{"wllv", 0x10}, {"ehcr", 0x110}, {"bmov", 0x30}})}},
                                                   {"slb.pos", {make_pos()}},
                                                   {"slb.light", {make_light(1), make_light(3)}},
                                                   {"slb.tex", {make_textures({{"on_lev_21xx_river", 0.5f}, {"glow", 1.0f}})}},
                                                   {"x", syn_x()},
                                                   {"dds", syn_dds()},
                                                   {"tga", syn_tga()},
                                                   {"osi", syn_osi()}};
  std::map<std::string, std::vector<Seed>> real;
  if (!data.empty() && !load_real(data, seed, per_format, real)) return 3;
  for (Format& f : formats) {
    for (Bytes& b : syn[f.name]) {
      Seed s;
      s.bytes = std::move(b);
      s.label = "synthetic";
      f.syn.push_back(std::move(s));
    }
    f.real = std::move(real[f.name]);
    for (Seed& s : f.syn) add_fields(f.name, s);
    for (Seed& s : f.real) add_fields(f.name, s);
  }

#if !defined(__EMSCRIPTEN__)
  std::signal(SIGPROF, on_alarm);  // before the warm-up and --replay: both run parsers under the CPU watchdog
#endif
  if (!replay_fmt.empty()) {
    for (Format& f : formats)
      if (replay_fmt == f.name) {
        Rng r(0);
        Bytes b;
        bool is_real = false;
        make_input(f, seed, replay_iter, r, b, is_real);
        if (!dump.empty()) {
          std::ofstream o(dump, std::ios::binary);
          o.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
        }
        g_fmt = &f;
        g_warmup = true;  // warm-up: one-time statics, as in a normal run
        for (const Seed& sd : f.syn) {
          g_iter = static_cast<uint64_t>(&sd - f.syn.data());
          note_last_seed(f.name, g_iter, false);
#if !defined(__EMSCRIPTEN__)
          watchdog(10);
#endif
          (void)run_one(f.name, sd.bytes, r, false);
        }
        g_warmup = false;
        f.findings = 0;
        g_iter = replay_iter;
        g_input = &b;
        const Result res = run_measured(f, b, r, is_real);
        std::printf("replay %s %llu: %zu bytes (%s seed), accepted=%d deep=%d findings=%llu\n", f.name,
                    static_cast<unsigned long long>(replay_iter), b.size(), is_real ? "real" : "synthetic", res.accepted, res.deep,
                    static_cast<unsigned long long>(f.findings));
        return f.findings ? 1 : 0;
      }
    return usage();
  }

#if defined(LOMN_FUZZ_ASAN)
  __sanitizer_set_death_callback(on_sanitizer_death);
#endif

  std::printf("fuzz_formats: seed=%llu n=%llu per format, real seeds %s\n", static_cast<unsigned long long>(seed),
              static_cast<unsigned long long>(n), data.empty() ? "off (synthetic only)" : "on");
  int missing_real = 0;
  for (const Format& f : formats) {
    if (!only.empty() && only.find("," + std::string(f.name) + ",") == std::string::npos) continue;
    std::printf("fuzz_formats: seeds %-9s synthetic=%zu real=%zu\n", f.name, f.syn.size(), f.real.size());
    if (!data.empty() && f.real.empty()) ++missing_real;
  }
  if (missing_real) {
    std::printf("fuzz_formats: --data gave no real seed for %d format(s)\n", missing_real);
    return 3;
  }

  const auto t_all = std::chrono::steady_clock::now();
  bool ok = true;
  for (Format& f : formats) {
    if (!only.empty() && only.find("," + std::string(f.name) + ",") == std::string::npos) continue;
    g_fmt = &f;
    Bytes b;
    Rng r(0);
    // Warm-up: each seed once, unmeasured and uncounted, so one-time allocations (function-local statics such as the
    // OSI opcode table) are not reported as a per-input leak. A seed that breaks an invariant is still a finding.
    g_warmup = true;
    for (const std::vector<Seed>* pool : {&f.syn, &f.real})
      for (const Seed& sd : *pool) {
        g_iter = static_cast<uint64_t>(&sd - pool->data());
        g_input = &sd.bytes;
        note_last_seed(f.name, g_iter, pool == &f.real);  // a seed that hangs on wasm32 is named too (cleanup C4)
#if !defined(__EMSCRIPTEN__)
        watchdog(10);  // a seed that hangs is a finding too
#endif
        if (pool == &f.syn && g_iter == g_test_hang_seed)
          for (volatile uint64_t spin = 0;; spin = spin + 1) {
          }
        (void)run_one(f.name, sd.bytes, r, pool == &f.real);
      }
#if !defined(__EMSCRIPTEN__)
    watchdog(0);
#endif
    g_warmup = false;
    for (uint64_t it = 0; it < n; ++it) {
      bool is_real = false;
      make_input(f, seed, it, r, b, is_real);
      g_iter = it;
      g_input = &b;
      if (g_last) {
        char line[64];
        std::snprintf(line, sizeof line, "%-9s %12llu", f.name, static_cast<unsigned long long>(it));
        note_last(line);
      }
      (void)run_measured(f, b, r, is_real);
      if ((it + 1) % (n / 10 ? n / 10 : 1) == 0)  // progress, with the process memory (wasm32: linear memory size)
        std::fprintf(stderr, "fuzz_formats: %s %llu/%llu mem=%.0fMiB\n", f.name, static_cast<unsigned long long>(it + 1),
                     static_cast<unsigned long long>(n), process_peak_bytes() / 1048576.0);
    }
    g_input = nullptr;
  }
  g_fmt = nullptr;
  const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_all).count();

  uint64_t total = 0, total_findings = 0;
  for (const Format& f : formats) {
    if (!f.inputs) continue;
    const double frac = static_cast<double>(f.deep) / static_cast<double>(f.inputs);
    char extra[64] = "";
    if (std::strcmp(f.name, "x") == 0)  // the pre-fix-round-1 .x "deep" (past the header), kept as a second column
      std::snprintf(extra, sizeof extra, " token_stream=%llu (%.1f%%)", static_cast<unsigned long long>(f.aux),
                    100.0 * static_cast<double>(f.aux) / static_cast<double>(f.inputs));
    std::printf("fuzz_formats: %-9s inputs=%llu accepted=%llu deep=%llu (%.1f%%) findings=%llu%s\n", f.name,
                static_cast<unsigned long long>(f.inputs), static_cast<unsigned long long>(f.accepted), static_cast<unsigned long long>(f.deep),
                100.0 * frac, static_cast<unsigned long long>(f.findings), extra);
    total += f.inputs;
    total_findings += f.findings;
    if (f.findings) ok = false;
    if (frac < min_deep) {
      std::printf("fuzz_formats: %s deep reach %.1f%% is below the %.1f%% minimum\n", f.name, 100.0 * frac, 100.0 * min_deep);
      ok = false;
    }
  }
  std::printf("fuzz_formats: total inputs=%llu findings=%llu -> %s\n", static_cast<unsigned long long>(total),
              static_cast<unsigned long long>(total_findings), ok ? "PASS" : "FAIL");
  // Not deterministic (timing, memory): kept apart from the totals above so native and wasm32 totals compare equal.
  for (const Format& f : formats)
    if (f.inputs)
      std::printf("resources %-9s max_input=%lluB max_cpu_ms=%.2f max_wall_ms=%.2f mean_wall_us=%.1f max_heap_growth=%.2fMiB "
                  "max_mem_growth=%.2fMiB retimed=%llu\n",
                  f.name, static_cast<unsigned long long>(f.max_input), 1000.0 * f.max_cpu, 1000.0 * f.max_seconds,
                  1e6 * f.total_seconds / static_cast<double>(f.inputs), f.max_heap / 1048576.0, f.max_rss / 1048576.0,
                  static_cast<unsigned long long>(f.retimed));
  std::printf("resources wall=%.1fs limits: per-input CPU time < %.0f s (minimum of up to 3 runs), growth <= %llu MiB\n", wall,
              kMaxInputSeconds,
              static_cast<unsigned long long>(kMaxGrowth >> 20));
  return ok ? 0 : 1;
}
