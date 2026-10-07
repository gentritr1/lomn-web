#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>

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

// One boundary table for size()/read_range(), run against every FileSystem implementation (memory, the base-class
// defaults, disk). `path` names a 4-byte file {10,20,30,40}; `logs` = the backend logs exactly once per successful
// read_range and never for a failed one.
static void check_range_table(FileSystem& fs, const char* path, const char* missing, const char* bad_mount, bool logs) {
  const uint64_t kMax = ~0ull;
  uint64_t sz = 123;
  CHECK(fs.size(path, sz) == Error::None);
  CHECK(sz == 4);
  sz = 123;
  CHECK(fs.size(missing, sz) == Error::NotFound);
  CHECK(fs.size(bad_mount, sz) == Error::BadMount);
  CHECK(sz == 123);  // untouched on failure

  struct Row {
    uint64_t off, len;
    Error expect;
  };
  const Row rows[] = {
      {0, 3, Error::None},  {0, 0, Error::None},    {4, 0, Error::None},   {2, 2, Error::None},   // ok, empty, offset==size, tail
      {4, 1, Error::Io},    {3, 2, Error::Io},      {5, 0, Error::Io},                              // past EOF, offset>size
      {1, kMax, Error::Io}, {kMax, 1, Error::Io},   {kMax, 0, Error::Io},                           // u64 wrap
  };
  std::vector<std::string> seen;
  if (logs) fs.set_access_logger([&](const std::string& n) { seen.push_back(n); });
  size_t ok_rows = 0;
  for (const Row& r : rows) {
    std::vector<uint8_t> out = {9, 9};
    INFO("offset=" << r.off << " len=" << r.len);
    Error e = fs.read_range(path, r.off, r.len, out);
    CHECK(e == r.expect);
    if (e == Error::None) {
      CHECK(out.size() == r.len);
      ++ok_rows;
    } else {
      CHECK(out == std::vector<uint8_t>{9, 9});  // a failed read leaves out untouched
    }
  }
  std::vector<uint8_t> out = {9, 9};
  CHECK(fs.read_range(path, 1, 2, out) == Error::None);
  CHECK(out == std::vector<uint8_t>{20, 30});
  ++ok_rows;
  CHECK(fs.read_range(missing, 0, 1, out) == Error::NotFound);
  CHECK(fs.read_range(bad_mount, 0, 1, out) == Error::BadMount);
  CHECK(out == std::vector<uint8_t>{20, 30});
  if (logs) {
    CHECK(seen.size() == ok_rows);  // one log per successful read_range, none for failures
    for (const auto& n : seen) CHECK(n == normalize(path));
    fs.set_access_logger(nullptr);
  }
}

// Implements only the pure virtuals, so size()/read_range() run the base-class defaults (via read()).
class MapOnlyFs final : public FileSystem {
 public:
  bool exists(std::string_view path) const override { return files.count(normalize(path)) != 0; }
  Error read(std::string_view path, std::vector<uint8_t>& out) const override {
    std::string n = normalize(path);
    if (n.rfind("root/", 0) != 0 && n.rfind("user/", 0) != 0) return Error::BadMount;
    auto it = files.find(n);
    if (it == files.end()) return Error::NotFound;
    out = it->second;
    return Error::None;
  }
  Error write(std::string_view path, const uint8_t* data, size_t size) override {
    files[normalize(path)].assign(data, data + size);
    return Error::None;
  }
  std::map<std::string, std::vector<uint8_t>> files;
};

TEST_CASE("memory fs: size/read_range boundary table") {
  MemoryFileSystem m;
  CHECK(m.mount("Root/data/t.bin", {10, 20, 30, 40}));
  check_range_table(m, "Root/data/T.bin", "Root/data/missing.bin", "other/t.bin", true);
}

TEST_CASE("base-class size/read_range defaults: boundary table") {
  MapOnlyFs f;
  f.files["root/data/t.bin"] = {10, 20, 30, 40};
  check_range_table(f, "Root/data/T.bin", "Root/data/missing.bin", "other/t.bin", false);
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
  {
    std::ofstream t(base / "install" / "data" / "t.bin", std::ios::binary);
    const char bytes[4] = {10, 20, 30, 40};
    t.write(bytes, 4);
  }
  auto fs = open_disk((base / "install").string(), (base / "user").string());
  std::vector<uint8_t> out;
  CHECK(fs->read("Root\\DATA\\levels\\LEV1\\NUJU.blk", out) == Error::None);
  CHECK(out.size() == 3);
  uint64_t size = 0;  // M1a-1 additive range API: real seek+read on disk, bounds-checked
  CHECK(fs->size("root/data/levels/lev1/nuju.blk", size) == Error::None);
  CHECK(size == 3);
  CHECK(fs->read_range("Root/data/levels/lev1/NUJU.BLK", 1, 2, out) == Error::None);
  CHECK(out == std::vector<uint8_t>{'b', 'c'});
  CHECK(fs->read_range("Root/data/levels/lev1/nuju.blk", 2, 2, out) == Error::Io);
  CHECK(fs->read_range("Root/data/levels/lev1/nuju.blk", ~0ull, 2, out) == Error::Io);
  CHECK(fs->read_range("Root/data/levels/lev1/missing.blk", 0, 1, out) == Error::NotFound);
  check_range_table(*fs, "Root/data/T.bin", "Root/data/missing.bin", "other/t.bin", true);
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

TEST_CASE("disk fs: open_disk rejects a missing install dir and a regular file") {
  namespace sfs = std::filesystem;
  sfs::path base = sfs::temp_directory_path() / ("lomn_disk_fs_missing_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  sfs::remove_all(base);
  sfs::create_directories(base);
  CHECK(open_disk((base / "does-not-exist").string(), (base / "user").string()) == nullptr);
  std::ofstream(base / "a_file") << "x";
  CHECK(open_disk((base / "a_file").string(), (base / "user").string()) == nullptr);
  sfs::remove_all(base);
}
#endif
