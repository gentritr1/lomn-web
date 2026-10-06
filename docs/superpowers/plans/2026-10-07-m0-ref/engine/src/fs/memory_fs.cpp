#include "lomn/fs.h"

namespace lomn::fs {

std::string normalize(std::string_view path) {
  std::string out;
  out.reserve(path.size());
  for (char c : path) {
    if (c == '\\') c = '/';
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (c == '/' && !out.empty() && out.back() == '/') continue;
    out.push_back(c);
  }
  while (out.rfind("./", 0) == 0) out.erase(0, 2);
  return out;
}

static bool starts_with(const std::string& s, std::string_view p) { return s.compare(0, p.size(), p) == 0; }

bool MemoryFileSystem::mount(std::string_view path, std::vector<uint8_t> bytes) {
  std::string n = normalize(path);
  if (!starts_with(n, "root/")) return false;
  files_[n] = std::move(bytes);
  return true;
}

size_t MemoryFileSystem::unmount_prefix(std::string_view prefix) {
  std::string p = normalize(prefix);
  size_t removed = 0;
  for (auto it = files_.lower_bound(p); it != files_.end() && starts_with(it->first, p);) {
    it = files_.erase(it);
    ++removed;
  }
  return removed;
}

size_t MemoryFileSystem::resident_bytes() const {
  size_t total = 0;
  for (const auto& kv : files_) total += kv.second.size();
  return total;
}

bool MemoryFileSystem::exists(std::string_view path) const { return files_.count(normalize(path)) != 0; }

Error MemoryFileSystem::read(std::string_view path, std::vector<uint8_t>& out) const {
  std::string n = normalize(path);
  if (!starts_with(n, "root/") && !starts_with(n, "user/")) return Error::BadMount;
  auto it = files_.find(n);
  if (it == files_.end()) return Error::NotFound;
  out = it->second;
  log_access(n);
  return Error::None;
}

Error MemoryFileSystem::write(std::string_view path, const uint8_t* data, size_t size) {
  std::string n = normalize(path);
  if (starts_with(n, "root/")) return Error::ReadOnly;
  if (!starts_with(n, "user/")) return Error::BadMount;
  files_[n].assign(data, data + size);
  return Error::None;
}

}  // namespace lomn::fs
