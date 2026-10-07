#include <chrono>
#include <filesystem>
#include <fstream>

#include "doctest.h"
#include "lomn/fs.h"

using namespace lomn::fs;

TEST_CASE("normalize: windows separators, case, duplicate slashes, leading ./") {
  CHECK(normalize("Root\\Data\\Levels\\LEV1\\vllg.BLK") == "root/data/levels/lev1/vllg.blk");
  CHECK(normalize("./Root//data") == "root/data");
  CHECK(normalize("") == "");
  CHECK(normalize("Root/data/../data/Main.blk") == "root/data/main.blk");
  CHECK(normalize("User/../Root/data/x") == "root/data/x");
  CHECK(normalize("User/../../etc/passwd") == "etc/passwd");
}

TEST_CASE("no path escapes a mount via ..") {
  MemoryFileSystem m;
  const uint8_t b[1] = {1};
  CHECK(m.write("User/../../escape", b, 1) == Error::BadMount);
  CHECK(m.write("User/../Root/data/x", b, 1) == Error::ReadOnly);
  std::vector<uint8_t> out;
  CHECK(m.read("User/../../etc/passwd", out) == Error::BadMount);
}

TEST_CASE("memory fs: mount, read, exists, logger") {
  MemoryFileSystem m;
  CHECK(m.mount("Root/data/Main.blk", {1, 2, 3}));
  CHECK_FALSE(m.mount("C:/elsewhere/x", {1}));
  std::vector<std::string> seen;
  m.set_access_logger([&](const std::string& p) { seen.push_back(p); });
  std::vector<uint8_t> out;
  CHECK(m.read("ROOT\\DATA\\main.blk", out) == Error::None);
  CHECK(out == std::vector<uint8_t>{1, 2, 3});
  CHECK(seen == std::vector<std::string>{"root/data/main.blk"});
  CHECK(m.exists("root/data/main.blk"));
  CHECK(m.read("Root/data/missing.blk", out) == Error::NotFound);
  CHECK(m.read("D:/nowhere", out) == Error::BadMount);
}

TEST_CASE("memory fs: Root is read-only, User is writable") {
  MemoryFileSystem m;
  const uint8_t b[2] = {9, 8};
  CHECK(m.write("Root/data/x", b, 2) == Error::ReadOnly);
  CHECK(m.write("other/x", b, 2) == Error::BadMount);
  CHECK(m.write("User/save0.sav", b, 2) == Error::None);
  std::vector<uint8_t> out;
  CHECK(m.read("user/SAVE0.sav", out) == Error::None);
  CHECK(out.size() == 2);
}

TEST_CASE("memory fs: unmount_prefix releases an area and keeps the rest") {
  MemoryFileSystem m;
  m.mount("Root/data/levels/lev1/vllg.blk", std::vector<uint8_t>(100));
  m.mount("Root/data/levels/lev1/cave.blk", std::vector<uint8_t>(50));
  m.mount("Root/data/levels/lev10/x.blk", std::vector<uint8_t>(7));
  m.mount("Root/data/Main.blk", std::vector<uint8_t>(10));
  CHECK(m.resident_bytes() == 167);
  CHECK(m.unmount_prefix("Root/data/levels/lev1/") == 2);
  CHECK(m.resident_bytes() == 17);
  CHECK(m.exists("root/data/levels/lev10/x.blk"));
}

#ifndef __EMSCRIPTEN__
TEST_CASE("disk fs: case-insensitive Root lookup and User writes") {
  namespace sfs = std::filesystem;
  sfs::path base = sfs::temp_directory_path() / ("lomn_disk_fs_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  sfs::remove_all(base);
  sfs::create_directories(base / "install" / "data" / "levels" / "lev1");
  std::ofstream(base / "install" / "data" / "levels" / "lev1" / "nUju.blk") << "abc";
  auto fs = open_disk((base / "install").string(), (base / "user").string());
  std::vector<uint8_t> out;
  CHECK(fs->read("Root\\DATA\\levels\\LEV1\\NUJU.blk", out) == Error::None);
  CHECK(out.size() == 3);
  const uint8_t b[1] = {7};
  CHECK(fs->write("Root/data/new", b, 1) == Error::ReadOnly);
  CHECK(fs->write("User/saves/s0.sav", b, 1) == Error::None);
  CHECK(fs->exists("user/saves/s0.sav"));
  sfs::remove_all(base);
}

TEST_CASE("disk fs: User path colliding with a file returns Io, never throws") {
  namespace sfs = std::filesystem;
  sfs::path base = sfs::temp_directory_path() / ("lomn_disk_fs_collide_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  sfs::remove_all(base);
  sfs::create_directories(base / "install");
  auto fs = open_disk((base / "install").string(), (base / "user").string());
  const uint8_t b[1] = {1};
  CHECK(fs->write("User/a", b, 1) == Error::None);
  CHECK(fs->write("User/a/b", b, 1) == Error::Io);
  CHECK(fs->write("User/a/b/c", b, 1) == Error::Io);
  std::vector<uint8_t> out;
  CHECK(fs->read("User/a", out) == Error::None);
  CHECK(out == std::vector<uint8_t>{1});
  sfs::remove_all(base);
}
#endif
