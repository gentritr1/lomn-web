// M1b: PrefetchFileSystem (the browser's file system) and the read-driven prefetch ("discovery"). Runs natively and
// as wasm32 under Node.
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "doctest.h"
#include "lomn/fs.h"
#include "lomn/prefetch_fs.h"
#include "lomn/sage/blockchain.h"
#include "lomn/view/area.h"
#include "lomn/view/prefetch.h"
#include "lomn/view/synthetic.h"
#include "sage_test_util.h"

using namespace lomn;
using fs::ByteRange;
using fs::Error;
using fs::PrefetchFileSystem;

namespace {

// Makes [off, off+len) of `path` resident with bytes value(i) = (off + i) & 0xff.
bool fill(PrefetchFileSystem& p, const char* path, uint64_t off, uint64_t len) {
  uint8_t* dst = p.reserve(path, off, len);
  if (!dst) return false;
  for (uint64_t i = 0; i < len; ++i) dst[i] = static_cast<uint8_t>((off + i) & 0xff);
  return p.commit(dst);
}

// The browser's loop, natively: a round runs `load`; its misses are copied from `src` (the player's files) into the
// prefetch fs; repeat until a round has no miss. Returns the number of rounds (0 = gave up after `max_rounds`).
template <typename Load>
int discover(PrefetchFileSystem& p, const fs::FileSystem& src, Load load, int max_rounds = 16) {
  for (int round = 1; round <= max_rounds; ++round) {
    p.next_generation();
    load();
    if (p.misses().empty() && p.misses_dropped() == 0) return round;
    for (const ByteRange& m : std::vector<ByteRange>(p.misses())) {
      std::vector<uint8_t> bytes;
      if (src.read_range(m.path, m.offset, m.length, bytes) != Error::None) return 0;
      uint8_t* dst = p.reserve(m.path, m.offset, m.length);
      if (!dst) return 0;
      std::memcpy(dst, bytes.data(), bytes.size());
      p.commit(dst);
    }
  }
  return 0;
}

// Registers every file of a MemoryFileSystem holding the synthetic install in the prefetch index.
void index_synthetic(PrefetchFileSystem& p, const fs::MemoryFileSystem& m) {
  for (const char* f : {"Root/Data/Levels/levt/levt.blk", "Root/Data/Levels/levt/test.blk", "Root/Data/Levels/levt/stat.blk",
                        "Root/Data/Base.osi"}) {
    uint64_t size = 0;
    REQUIRE(m.size(f, size) == Error::None);
    REQUIRE(p.add_file(f, size));
  }
}

}  // namespace

TEST_CASE("prefetch fs: the index answers exists/size without resident bytes; bad registrations are rejected") {
  PrefetchFileSystem p;
  std::string err;
  CHECK(p.add_file("Root\\Data\\Main.BLK", 100, &err));
  CHECK_FALSE(p.add_file("root/data/main.blk", 5, &err));  // duplicate after normalisation
  CHECK(err.find("duplicate") != std::string::npos);
  CHECK_FALSE(p.add_file("User/save0.sav", 5, &err));      // only root/ is indexed
  CHECK_FALSE(p.add_file("Root", 5, &err));
  CHECK_FALSE(p.add_file("Root/x", (1ull << 53), &err));    // beyond a JS safe integer
  CHECK_FALSE(p.add_file(std::string(PrefetchFileSystem::kMaxPathBytes + 1, 'a'), 1, &err));
  CHECK(p.file_count() == 1);
  CHECK(p.exists("ROOT/data/main.blk"));
  // final I-2 (defence in depth): the engine<->JS protocols are tab/newline separated ("lev\tarea\n", "path\toffset\tlength\n"),
  // so a path with a control character (< 0x20 or 0x7f) is never indexed, whatever the page let through.
  for (const char c : {'\t', '\n', '\r', '\x01', '\x1f', '\x7f'}) {
    const std::string bad = std::string("Root/Data/Levels/levt/a") + c + "b.blk";
    CHECK_FALSE(p.add_file(bad, 5, &err));
    CHECK(err.find("control character") != std::string::npos);
    CHECK_FALSE(p.exists(bad));
  }
  CHECK_FALSE(p.add_file("Root/d\tir/x.blk", 5, &err));  // in a folder name too
  CHECK(p.add_file("Root/Data/Levels/levt/a b.blk", 5, &err));  // a space and non-ASCII bytes are fine
  CHECK(p.add_file("Root/Data/\xc3\xa9.blk", 5, &err));
  CHECK(p.file_count() == 3);
  uint64_t sz = 0;
  CHECK(p.size("root/data/main.blk", sz) == Error::None);
  CHECK(sz == 100);
  CHECK(p.size("root/data/nope", sz) == Error::NotFound);
  CHECK(p.size("elsewhere/x", sz) == Error::BadMount);
}

TEST_CASE("prefetch fs: reads are served only from ONE resident range that contains them; others are recorded misses") {
  PrefetchFileSystem p;
  REQUIRE(p.add_file("root/a.bin", 1000));
  REQUIRE(fill(p, "root/a.bin", 100, 50));
  REQUIRE(fill(p, "root/a.bin", 150, 50));
  std::vector<uint8_t> out;
  CHECK(p.read_range("root/a.bin", 110, 20, out) == Error::None);
  CHECK(out.size() == 20);
  CHECK(out[0] == 110);
  CHECK(p.read_range("root/a.bin", 140, 20, out) == Error::Io);   // spans two ranges: a miss (fetched as one range)
  CHECK(p.read_range("root/a.bin", 0, 10, out) == Error::Io);     // not resident
  CHECK(p.read_range("root/a.bin", 0, 10, out) == Error::Io);     // the same miss again: stored once
  CHECK(p.read("root/a.bin", out) == Error::Io);                  // whole file = [0, 1000)
  CHECK(p.read_range("root/a.bin", 990, 20, out) == Error::Io);   // outside the file: an error, NOT a miss
  CHECK(p.read_range("root/a.bin", 1000, 0, out) == Error::None); // empty read at the end: nothing to fetch
  CHECK(p.read_range("root/missing", 0, 1, out) == Error::NotFound);
  REQUIRE(p.misses().size() == 3);
  CHECK(p.misses()[0] == ByteRange{"root/a.bin", 140, 20});
  CHECK(p.misses()[1] == ByteRange{"root/a.bin", 0, 10});
  CHECK(p.misses()[2] == ByteRange{"root/a.bin", 0, 1000});
  CHECK(p.misses_total() == 4);
  // reserve refuses what cannot or need not be fetched.
  std::string err;
  CHECK(p.reserve("root/a.bin", 110, 10, &err) == nullptr);
  CHECK(err.find("already resident") != std::string::npos);
  CHECK(p.reserve("root/a.bin", 990, 20, &err) == nullptr);
  CHECK(p.reserve("root/a.bin", 0, 0, &err) == nullptr);
  CHECK(p.reserve("root/zzz", 0, 1, &err) == nullptr);
  CHECK(p.reserve("root/a.bin", 0, PrefetchFileSystem::kMaxRangeBytes + 1, &err) == nullptr);
  // A pending reservation is not readable until committed; abort frees it.
  uint8_t* h = p.reserve("root/a.bin", 0, 10, &err);
  REQUIRE(h);
  CHECK(p.read_range("root/a.bin", 0, 10, out) == Error::Io);
  CHECK(p.pending_bytes() == 10);
  CHECK(p.abort(h));
  CHECK_FALSE(p.commit(h));
  CHECK(p.pending_bytes() == 0);
  CHECK(p.resident_bytes() == 100);
}

TEST_CASE("prefetch fs: byte budget, miss cap, user/ writes, root/ is read-only") {
  PrefetchFileSystem p(64);
  REQUIRE(p.add_file("root/a.bin", 100000));
  std::string err;
  CHECK(fill(p, "root/a.bin", 0, 60));
  CHECK(p.reserve("root/a.bin", 100, 5, &err) == nullptr);
  CHECK(err.find("budget") != std::string::npos);
  CHECK(p.peak_bytes() == 60);
  std::vector<uint8_t> out;
  for (uint32_t i = 0; i < PrefetchFileSystem::kMaxMissesPerGeneration + 10; ++i) p.read_range("root/a.bin", 100 + i, 1, out);
  CHECK(p.misses().size() == PrefetchFileSystem::kMaxMissesPerGeneration);
  CHECK(p.misses_dropped() == 10);
  p.next_generation();
  CHECK(p.misses().empty());
  CHECK(p.misses_dropped() == 0);
  const uint8_t b[3] = {1, 2, 3};
  CHECK(p.write("root/a.bin", b, 3) == Error::ReadOnly);
  CHECK(p.write("User/s.sav", b, 3) == Error::None);
  CHECK(p.read("user/s.sav", out) == Error::None);
  CHECK(out.size() == 3);
  CHECK(p.write("x/../../y", b, 3) == Error::BadMount);
}

TEST_CASE("prefetch fs: generations - release_untouched keeps pinned and touched ranges (shared files), frees the rest") {
  PrefetchFileSystem p;
  REQUIRE(p.add_file("root/boot.osi", 100));
  REQUIRE(p.add_file("root/a.blk", 1000));
  std::vector<uint8_t> out;
  REQUIRE(fill(p, "root/boot.osi", 0, 100));
  p.next_generation();
  REQUIRE(p.read("root/boot.osi", out) == Error::None);
  CHECK(p.pin_touched() == 1);
  // "Area 1" reads [0,10) and [500,100).
  REQUIRE(fill(p, "root/a.blk", 0, 10));
  REQUIRE(fill(p, "root/a.blk", 500, 100));
  p.next_generation();
  REQUIRE(p.read_range("root/a.blk", 0, 10, out) == Error::None);
  REQUIRE(p.read_range("root/a.blk", 500, 100, out) == Error::None);
  CHECK(p.release_untouched() == 0);
  // "Area 2" shares [0,10) and reads [700,50); [500,100) is area 1's only.
  REQUIRE(fill(p, "root/a.blk", 700, 50));
  p.next_generation();
  REQUIRE(p.read_range("root/a.blk", 0, 10, out) == Error::None);
  REQUIRE(p.read_range("root/a.blk", 700, 50, out) == Error::None);
  CHECK(p.touched() == std::vector<ByteRange>{{"root/a.blk", 0, 10}, {"root/a.blk", 700, 50}});
  CHECK(p.release_untouched() == 100);
  CHECK(p.is_resident("root/boot.osi", 0, 100));  // pinned (boot), although not touched in this generation
  CHECK(p.is_resident("root/a.blk", 0, 10));      // shared
  CHECK_FALSE(p.is_resident("root/a.blk", 500, 100));
  CHECK(p.resident_bytes() == 160);
  CHECK(p.pinned_bytes() == 100);
  CHECK(p.resident_ranges() == 3);
}

TEST_CASE("prefetch discovery: the synthetic areas load from misses alone, equal to the direct load and the access log") {
  fs::MemoryFileSystem m;
  REQUIRE(view::mount_synthetic_install(m));
  PrefetchFileSystem p;
  index_synthetic(p, m);
  // Boot: no Art.blk in the synthetic install (size() fails: no miss), Base.osi whole.
  const int boot_rounds = discover(p, m, [&] { view::boot_reads(p); });
  CHECK(boot_rounds == 2);
  CHECK(p.pin_touched() == 1);
  uint64_t osi = 0;
  REQUIRE(m.size("Root/Data/Base.osi", osi) == Error::None);
  CHECK(p.pinned_bytes() == osi);
  for (const char* name : {view::kSyntheticArea, view::kSyntheticStateArea, view::kSyntheticArea}) {
    CAPTURE(name);
    view::AreaData via_prefetch, direct;
    std::string err;
    bool ok = false;
    const int rounds = discover(p, m, [&] { ok = view::load_area(p, view::kSyntheticLevel, name, view::AreaOptions(), via_prefetch, &err); });
    CHECK(rounds >= 2);
    CHECK(rounds <= 8);
    REQUIRE(ok);
    // The last round's touched ranges == the access log of the same load on the source file system (the native
    // `lomn_view --manifest` rows), so the browser fetches exactly what the native engine reads.
    const std::vector<ByteRange> log = view::record_reads(m, [&] { ok = view::load_area(m, view::kSyntheticLevel, name, view::AreaOptions(), direct, &err); });
    REQUIRE(ok);
    CHECK(p.touched() == log);
    CHECK(via_prefetch.stats.draws == direct.stats.draws);
    CHECK(via_prefetch.stats.textures == direct.stats.textures);
    CHECK(via_prefetch.stats.texture_bytes == direct.stats.texture_bytes);
    CHECK(via_prefetch.placements.size() == direct.placements.size());
    CHECK(via_prefetch.warnings == direct.warnings);
    CHECK(via_prefetch.fog.source == direct.fog.source);
    p.release_untouched();
    for (const ByteRange& r : log) CHECK(p.is_resident(r.path, r.offset, r.length));
    uint64_t bytes = 0;
    for (const ByteRange& r : log) bytes += r.length;
    // Everything resident is either pinned (Base.osi, also in the log) or this area's set.
    CHECK(p.resident_bytes() == bytes);
  }
}

// T8 P7: a zero-length read carries no bytes, so it is no prefetch work. The native `lomn_view --manifest` (record_reads over
// the disk file system) used to list it ([lev4/haka.blk, 12, 0]: haka.blk is a blockfile with an EMPTY entry table, so
// BlockFile::open reads the 12-byte header and then a 0-byte table) while PrefetchFileSystem::touched() never could (no
// resident range), so "browser discovery == native manifest" failed on that area. Decision: excluded from BOTH. The
// range logger still reports the raw read (it happened); record_reads, which feeds the manifest and the tests, drops it.
TEST_CASE("prefetch discovery: a zero-length read (an empty blockfile table) is in neither the native read record nor touched()") {
  const char* kPath = "Root/data/levels/zero/zero.blk";
  const std::string kNorm = "root/data/levels/zero/zero.blk";
  const sage_test::Bytes blk = sage_test::make_blk({});  // BLKF, version 1, count 0: 12 bytes, no table
  REQUIRE(blk.size() == 12);
  fs::MemoryFileSystem m;
  REQUIRE(m.mount(kPath, blk));
  PrefetchFileSystem p;
  REQUIRE(p.add_file(kPath, blk.size()));
  auto open_chain = [&](const fs::FileSystem& f) {
    sage::BlockChain chain(f);
    std::string err;
    const bool ok = chain.open_current(kPath, &err);
    INFO(err);
    return ok;
  };
  // The raw logger sees both reads on the native-style fs: header (0, 12) and the 0-byte table (12, 0).
  std::vector<ByteRange> raw;
  m.set_range_logger([&](const std::string& path, uint64_t off, uint64_t len) { raw.push_back({path, off, len}); });
  REQUIRE(open_chain(m));
  m.set_range_logger(nullptr);
  REQUIRE(raw.size() == 2);
  CHECK(raw[0] == ByteRange{kNorm, 0, 12});
  CHECK(raw[1] == ByteRange{kNorm, 12, 0});
  // record_reads (= the manifest rows) keeps only reads that move bytes.
  bool ok = false;
  const std::vector<ByteRange> log = view::record_reads(m, [&] { ok = open_chain(m); });
  REQUIRE(ok);
  CHECK(log == std::vector<ByteRange>{{kNorm, 0, 12}});
  CHECK(view::ranges_text(log) == kNorm + "\t0\t12\n");
  // The browser path: discovery converges on the same set (and the zero-length read is never a miss).
  const int rounds = discover(p, m, [&] { ok = open_chain(p); });
  CHECK(rounds == 2);
  REQUIRE(ok);
  CHECK(p.touched() == log);
  CHECK(p.misses().empty());
  // Zero-length reads at any offset, also at the very end of the file, are served without a resident range.
  std::vector<uint8_t> out{1, 2, 3};
  CHECK(p.read_range(kPath, 12, 0, out) == Error::None);
  CHECK(out.empty());
  CHECK(p.read_range(kPath, 13, 0, out) == Error::Io);  // beyond the end of the file stays an error
}

TEST_CASE("list_areas: levels/<lev>/<area>.blk, the level blockfile excluded") {
  const std::vector<std::string> paths = {"root/data/levels/lev1/lev1.blk", "root/data/levels/lev1/vllg.blk", "root/data/levels/lev1/textures/x.dds",
                                          "root/data/levels/lev10/abcd.blk", "root/data/levels/lev1/toolong.blk", "root/data/main.blk",
                                          "root/data/levels/x.blk"};
  const std::vector<view::AreaRef> a = view::list_areas(paths);
  REQUIRE(a.size() == 1);
  CHECK(a[0] == view::AreaRef{"lev1", "vllg"});
  CHECK(view::ranges_text({{"root/a", 1, 2}, {"root/b", 0, 10}}) == "root/a\t1\t2\nroot/b\t0\t10\n");
}
