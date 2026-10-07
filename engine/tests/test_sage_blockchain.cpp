#include "doctest.h"
#include "lomn/fs.h"
#include "lomn/sage/blockchain.h"
#include "sage_test_util.h"

using namespace lomn;
using namespace lomn::sage;
using namespace sage_test;

namespace {

// Art.blk, lev1.blk and vllg.blk with overlapping names, mounted under the on-disk (lower-case) layout.
void mount_world(fs::MemoryFileSystem& m) {
  m.mount("Root/data/Art/Art.blk", make_blk({stored("both.dds", "art-both"), stored("loading.tga", "art-loading"), stored("tahu.dds", "art-tahu")}));
  m.mount("Root/data/levels/lev1/lev1.blk", make_blk({stored("both.dds", "lev-both"), stored("ocean.dds", "lev-ocean"), stored("tahu.dds", "lev-tahu")}));
  m.mount("Root/data/levels/lev1/vllg.blk", make_blk({stored("main.x", "area-main"), stored("tahu.dds", "area-tahu")}));
  m.mount("Root/data/loose.txt", Bytes{'d', 'i', 's', 'k'});
}

std::string load_str(const BlockChain& c, const char* name, Source* from) {
  std::vector<uint8_t> out;
  std::string err;
  if (!c.load(name, out, from, &err)) return "<" + err + ">";
  return std::string(out.begin(), out.end());
}

}  // namespace

TEST_CASE("blockchain: Load searches current -> second -> Art; Exists searches Art first") {
  fs::MemoryFileSystem m;
  mount_world(m);
  BlockChain c(m);
  std::string err;
  REQUIRE(c.open_art("Root/Data/Art/Art.blk", &err));
  REQUIRE(c.enter_area("lev1", "vllg", &err));  // game-cased paths reach the lower-case files
  Source from;
  CHECK(load_str(c, "Root/Data/Levels/lev1/textures/tahu.dds", &from) == "area-tahu");
  CHECK(from == Source::Current);
  CHECK(load_str(c, "Root/Data/Levels/lev1/textures/both.dds", &from) == "lev-both");
  CHECK(from == Source::Second);
  CHECK(load_str(c, "x/y/LOADING.TGA", &from) == "art-loading");
  CHECK(from == Source::Art);
  // Exists answers from Art for names Art also has, even though Load would read the area/level copy.
  CHECK(c.find_exists("tahu.dds").source == Source::Art);
  CHECK(c.find_load("tahu.dds").source == Source::Current);
  CHECK(c.find_exists("both.dds").source == Source::Art);
  CHECK(c.find_exists("ocean.dds").source == Source::Second);
  CHECK(c.find_exists("main.x").source == Source::Current);
  // Loose files are dead while a current blockfile is set.
  CHECK_FALSE(c.exists("Root/data/loose.txt"));
  CHECK(c.find_load("Root/data/loose.txt").source == Source::None);
  CHECK(load_str(c, "nothing.dds", &from).front() == '<');
  CHECK(from == Source::None);
}

TEST_CASE("blockchain: without a current blockfile, Art then the disk (both orders)") {
  fs::MemoryFileSystem m;
  mount_world(m);
  BlockChain c(m);
  std::string err;
  REQUIRE(c.open_art("ROOT/DATA/ART/ART.BLK", &err));
  Source from;
  CHECK(load_str(c, "Root/Data/Art/hud/loading.tga", &from) == "art-loading");
  CHECK(from == Source::Art);
  CHECK(load_str(c, "Root/Data/LOOSE.txt", &from) == "disk");
  CHECK(from == Source::Disk);
  CHECK(c.find_exists("Root/data/loose.txt").source == Source::Disk);
  // A second blockfile alone is never consulted (the original only reads it after a current one).
  REQUIRE(c.open_second("Root/data/levels/lev1/lev1.blk", &err));
  CHECK(c.find_load("ocean.dds").source == Source::None);
  CHECK(c.find_exists("ocean.dds").source == Source::None);
}

TEST_CASE("blockchain: current without second, and the Exists order stops before disk") {
  fs::MemoryFileSystem m;
  mount_world(m);
  BlockChain c(m);
  std::string err;
  REQUIRE(c.open_current("root/data/levels/lev1/vllg.blk", &err));
  CHECK(c.find_load("main.x").source == Source::Current);
  CHECK(c.find_load("ocean.dds").source == Source::None);
  CHECK(c.find_exists("ocean.dds").source == Source::None);
  CHECK_FALSE(c.exists("Root/data/loose.txt"));
}

TEST_CASE("blockchain: an entry that fails to read falls through to the next blockfile") {
  fs::MemoryFileSystem m;
  Bytes area = make_blk({stored("tex.dds", "AREA")});
  set32(area, 12 + 52, 0x7ffffff0u);  // offset outside the file -> LoadDiskBlock returns NULL
  m.mount("Root/data/levels/lev1/vllg.blk", area);
  m.mount("Root/data/levels/lev1/lev1.blk", make_blk({stored("tex.dds", "LEVEL")}));
  BlockChain c(m);
  std::string err;
  REQUIRE(c.enter_area("lev1", "vllg", &err));
  Source from;
  CHECK(load_str(c, "tex.dds", &from) == "LEVEL");
  CHECK(from == Source::Second);
  CHECK(c.find_load("tex.dds").source == Source::Current);  // the lookup itself still hits the area table
}

TEST_CASE("blockchain: missing or corrupt blockfiles fail to open; range reads are bounded") {
  fs::MemoryFileSystem m;
  m.mount("Root/data/bad.blk", Bytes{'B', 'L', 'K', 'F', 1, 0, 0, 0, 9, 0, 0, 0});
  BlockChain c(m);
  std::string err;
  CHECK_FALSE(c.open_current("Root/data/missing.blk", &err));
  CHECK_FALSE(c.open_current("Root/data/bad.blk", &err));
  CHECK(c.blockfile(Source::Current) == nullptr);
  std::vector<uint8_t> out;
  CHECK(m.read_range("Root/data/bad.blk", 4, 4, out) == fs::Error::None);
  CHECK(out == Bytes{1, 0, 0, 0});
  CHECK(m.read_range("Root/data/bad.blk", 10, 3, out) == fs::Error::Io);
  CHECK(m.read_range("Root/data/bad.blk", 0xffffffffffffffffull, 2, out) == fs::Error::Io);
  uint64_t size = 0;
  CHECK(m.size("Root/data/BAD.blk", size) == fs::Error::None);
  CHECK(size == 12);
}

TEST_CASE("blockchain: a failed enter_area leaves no area entered") {
  fs::MemoryFileSystem m;
  mount_world(m);
  BlockChain c(m);
  std::string err;
  REQUIRE(c.open_art("Root/Data/Art/Art.blk", &err));
  REQUIRE(c.enter_area("lev1", "vllg", &err));
  Source from;
  REQUIRE(load_str(c, "main.x", &from) == "area-main");  // only the old area's blk has main.x
  CHECK_FALSE(c.enter_area("lev9", "zzzz", &err));      // level blk missing
  CHECK(c.blockfile(Source::Current) == nullptr);
  CHECK(c.blockfile(Source::Second) == nullptr);
  CHECK(load_str(c, "main.x", &from).front() == '<');    // old area is gone
  CHECK(from == Source::None);
  // "no area" now: Art, then the disk.
  CHECK(load_str(c, "Root/data/loose.txt", &from) == "disk");
  CHECK(from == Source::Disk);
  // The level blk opens but the area blk is missing: neither stays open.
  REQUIRE(c.enter_area("lev1", "vllg", &err));
  CHECK_FALSE(c.enter_area("lev1", "nope", &err));
  CHECK(c.blockfile(Source::Current) == nullptr);
  CHECK(c.blockfile(Source::Second) == nullptr);
}

TEST_CASE("blockchain: resolve_texture follows ScDrawableTexture::Load (gTextureTypes .dds/.tga)") {
  fs::MemoryFileSystem m;
  // The entry table is searched by binary search: keep the names sorted.
  m.mount("Root/data/Art/Art.blk", make_blk({stored("bare.dds", "c"), stored("bare.tga", "d"), stored("fall.dds.dds", "f"), stored("kept.dds", "a"),
                                             stored("only.tga", "e"), stored("tgaext.tga", "g"), stored("upper.dds", "b")}));
  BlockChain c(m);
  std::string err;
  REQUIRE(c.open_art("Root/Data/Art/Art.blk", &err));
  std::string chosen = "stale";
  const std::string pre = "Root/Data/Levels/lev1/textures/";
  // An extension from gTextureTypes is kept when that file exists.
  CHECK(resolve_texture(c, "lev1", "kept.dds", chosen));
  CHECK(chosen == pre + "kept.dds");
  CHECK(resolve_texture(c, "lev1", "tgaext.tga", chosen));
  CHECK(chosen == pre + "tgaext.tga");
  // The extension compare is case-insensitive: ".DDS" counts as an extension and is kept as written.
  CHECK(resolve_texture(c, "lev1", "UPPER.DDS", chosen));
  CHECK(chosen == pre + "UPPER.DDS");
  // Extension present but that file is missing: fall back to appending (full + ".dds").
  CHECK(resolve_texture(c, "lev1", "fall.dds", chosen));
  CHECK(chosen == pre + "fall.dds.dds");
  // No extension: ".dds" is preferred over ".tga" when both exist; ".tga" when it is the only one.
  CHECK(resolve_texture(c, "lev1", "bare", chosen));
  CHECK(chosen == pre + "bare.dds");
  CHECK(resolve_texture(c, "lev1", "only", chosen));
  CHECK(chosen == pre + "only.tga");
  // Neither: false, and `chosen` is cleared.
  chosen = "stale";
  CHECK_FALSE(resolve_texture(c, "lev1", "nothing", chosen));
  CHECK(chosen.empty());
  chosen = "stale";
  CHECK_FALSE(resolve_texture(c, "lev1", "nothing.dds", chosen));  // kept-extension probe fails, both appends fail
  CHECK(chosen.empty());
}
