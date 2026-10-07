#include <filesystem>
#include <fstream>
#include <unordered_map>

#include "lomn/fs.h"

namespace lomn::fs {
namespace {

class DiskFileSystem final : public FileSystem {
 public:
  DiskFileSystem(std::filesystem::path install, std::filesystem::path user) : install_(std::move(install)), user_(std::move(user)) {}
  // Builds the case-insensitive index. False when the install root cannot be made canonical or its walk cannot start
  // (e.g. no read permission): open_disk then returns nullptr instead of a file system whose empty index silently
  // finds nothing (Task 8B F2). Unreadable entries further down are skipped, as before.
  bool index() {
    std::error_code ec;
    // M1a-3: iterate from the canonical path. A symlinked install root iterates natively, but under Node (wasm32,
    // NODERAWFS) the directory iterator fails on it ("Not a directory"); the relative keys are the same either way.
    const std::filesystem::path canon = std::filesystem::canonical(install_, ec);
    if (ec) return false;
    install_ = canon;
    std::filesystem::recursive_directory_iterator it(install_, ec);
    if (ec) return false;
    for (; !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
      std::error_code ec2;
      if (!it->is_regular_file(ec2) || ec2) continue;
      const std::filesystem::path rel = std::filesystem::relative(it->path(), install_, ec2);
      if (ec2) continue;
      index_["root/" + normalize(rel.generic_string())] = it->path();
    }
    return true;
  }
  bool exists(std::string_view path) const override { return !resolve(normalize(path)).empty(); }
  Error read(std::string_view path, std::vector<uint8_t>& out) const override {
    std::string n = normalize(path);
    if (n.rfind("root/", 0) != 0 && n.rfind("user/", 0) != 0) return Error::BadMount;
    std::filesystem::path p = resolve(n);
    if (p.empty()) return Error::NotFound;
    std::ifstream f(p, std::ios::binary);
    if (!f) return Error::Io;
    // istreambuf_iterator bypasses stream state, so read a known size and check the count instead.
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(p, ec);
    if (ec) return Error::Io;
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    if (size > 0) {
      f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(size));
      if (f.bad() || static_cast<std::uintmax_t>(f.gcount()) != size) return Error::Io;  // truncated read
    }
    out = std::move(buf);
    log_access(n);
    return Error::None;
  }
  Error size(std::string_view path, uint64_t& out) const override {
    std::string n = normalize(path);
    if (n.rfind("root/", 0) != 0 && n.rfind("user/", 0) != 0) return Error::BadMount;
    std::filesystem::path p = resolve(n);
    if (p.empty()) return Error::NotFound;
    std::error_code ec;
    uint64_t s = std::filesystem::file_size(p, ec);
    if (ec) return Error::Io;
    out = s;
    return Error::None;
  }
  Error read_range(std::string_view path, uint64_t offset, uint64_t len, std::vector<uint8_t>& out) const override {
    std::string n = normalize(path);
    if (n.rfind("root/", 0) != 0 && n.rfind("user/", 0) != 0) return Error::BadMount;
    std::filesystem::path p = resolve(n);
    if (p.empty()) return Error::NotFound;
    std::error_code ec;
    uint64_t total = std::filesystem::file_size(p, ec);
    if (ec || offset > total || len > total - offset) return Error::Io;
    std::ifstream f(p, std::ios::binary);
    if (!f) return Error::Io;
    std::vector<uint8_t> buf(static_cast<size_t>(len));  // out is only replaced on success
    f.seekg(static_cast<std::streamoff>(offset));
    if (len) f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(len));
    if (!f) return Error::Io;
    out = std::move(buf);
    log_access(n);
    return Error::None;
  }
  Error write(std::string_view path, const uint8_t* data, size_t size) override {
    std::string n = normalize(path);
    if (n.rfind("root/", 0) == 0) return Error::ReadOnly;
    if (n.rfind("user/", 0) != 0) return Error::BadMount;
    std::filesystem::path p = user_ / n.substr(5);  // n is normalised: no ".." can remain
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    if (ec) return Error::Io;
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return Error::Io;
    f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    f.flush();
    if (!f) return Error::Io;
    f.close();  // close() flushes; a failure here (e.g. full disk) sets failbit
    return f.fail() ? Error::Io : Error::None;
  }

 private:
  std::filesystem::path resolve(const std::string& n) const {
    if (n.rfind("root/", 0) == 0) {
      auto it = index_.find(n);
      return it == index_.end() ? std::filesystem::path() : it->second;
    }
    if (n.rfind("user/", 0) == 0) {
      std::filesystem::path p = user_ / n.substr(5);
      std::error_code ec;  // the non-throwing overload (a permission error is "not found", never an exception)
      return std::filesystem::is_regular_file(p, ec) ? p : std::filesystem::path();
    }
    return {};
  }
  std::filesystem::path install_, user_;
  std::unordered_map<std::string, std::filesystem::path> index_;
};

}  // namespace

std::unique_ptr<FileSystem> open_disk(const std::string& install_dir, const std::string& user_dir) {
  std::error_code ec;
  if (!std::filesystem::is_directory(install_dir, ec) || ec) return nullptr;
  auto fs = std::make_unique<DiskFileSystem>(install_dir, user_dir);
  if (!fs->index()) return nullptr;
  return fs;
}

}  // namespace lomn::fs
