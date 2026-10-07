// File system interface (frozen in M0; additive changes only, reviewed). Game paths look like
// "Root/data/levels/lev1/vllg.blk" (read-only game data) or "User/save0.sav" (read-write user data).
// Paths are normalised Windows-style: '\\' -> '/', ASCII lower-case, duplicate '/' collapsed, "." dropped and
// ".." folded. A ".." that would climb out of the first segment ("user/../x") consumes it, so the result no longer
// starts with root/ or user/ and every operation on it returns BadMount: paths can never escape a mount.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace lomn::fs {

std::string normalize(std::string_view path);

enum class Error { None, NotFound, BadMount, ReadOnly, Io };

class FileSystem {
 public:
  virtual ~FileSystem() = default;
  virtual bool exists(std::string_view path) const = 0;
  virtual Error read(std::string_view path, std::vector<uint8_t>& out) const = 0;
  virtual Error write(std::string_view path, const uint8_t* data, size_t size) = 0;
  // Additive (M1a-1): size query and byte-range read, so blockfile entries are read on demand like the original
  // ScBlockFile::LoadDiskBlock (fopen/fseek/fread of one entry). The defaults go through read() (whole file);
  // backends override them. read_range fails with Io if [offset, offset+size) is not inside the file.
  virtual Error size(std::string_view path, uint64_t& out) const;
  virtual Error read_range(std::string_view path, uint64_t offset, uint64_t size, std::vector<uint8_t>& out) const;
  // Called with the normalised path on every successful read (drives the per-area prefetch manifest).
  void set_access_logger(std::function<void(const std::string&)> logger) { logger_ = std::move(logger); }

 protected:
  void log_access(const std::string& normalised) const {
    if (logger_) logger_(normalised);
  }

 private:
  std::function<void(const std::string&)> logger_;
};

// In-memory file system used by the browser build (bytes prefetched from OPFS / the player's folder) and tests.
class MemoryFileSystem final : public FileSystem {
 public:
  // Mount read-only game bytes; path must normalise to "root/...". Returns false otherwise.
  bool mount(std::string_view path, std::vector<uint8_t> bytes);
  // Remove every mounted file inside the directory `prefix` (e.g. "Root/data/levels/lev1" or ".../lev1/";
  // never matches the sibling "lev10"); returns the number removed.
  size_t unmount_prefix(std::string_view prefix);
  size_t resident_bytes() const;
  bool exists(std::string_view path) const override;
  Error read(std::string_view path, std::vector<uint8_t>& out) const override;
  Error write(std::string_view path, const uint8_t* data, size_t size) override;
  Error size(std::string_view path, uint64_t& out) const override;
  Error read_range(std::string_view path, uint64_t offset, uint64_t size, std::vector<uint8_t>& out) const override;

 private:
  std::map<std::string, std::vector<uint8_t>> files_;
};

#ifndef __EMSCRIPTEN__
// Native: "root/..." resolves case-insensitively inside install_dir, "user/..." inside user_dir (created on write).
std::unique_ptr<FileSystem> open_disk(const std::string& install_dir, const std::string& user_dir);
#endif

}  // namespace lomn::fs
