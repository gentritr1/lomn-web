// File system interface (frozen in M0; additive changes only, reviewed). Game paths look like
// "Root/data/levels/lev1/vllg.blk" (read-only game data) or "User/save0.sav" (read-write user data).
// Paths are normalised Windows-style: '\\' -> '/', ASCII lower-case, duplicate '/' collapsed.
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
  // Remove every mounted file whose normalised path starts with prefix; returns the number removed.
  size_t unmount_prefix(std::string_view prefix);
  size_t resident_bytes() const;
  bool exists(std::string_view path) const override;
  Error read(std::string_view path, std::vector<uint8_t>& out) const override;
  Error write(std::string_view path, const uint8_t* data, size_t size) override;

 private:
  std::map<std::string, std::vector<uint8_t>> files_;
};

#ifndef __EMSCRIPTEN__
// Native: "root/..." resolves case-insensitively inside install_dir, "user/..." inside user_dir (created on write).
std::unique_ptr<FileSystem> open_disk(const std::string& install_dir, const std::string& user_dir);
#endif

}  // namespace lomn::fs
