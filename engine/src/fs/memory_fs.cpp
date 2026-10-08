#include "lomn/fs.h"

namespace lomn::fs {

std::string normalize(std::string_view path) {
  std::vector<std::string> segs;
  std::string cur;
  auto flush = [&] {
    if (cur == "..") {
      if (!segs.empty()) segs.pop_back();
    } else if (!cur.empty() && cur != ".") {
      segs.push_back(cur);
    }
    cur.clear();
  };
  for (char c : path) {
    if (c == '\\' || c == '/') {
      flush();
      continue;
    }
    cur.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
  }
  flush();
  std::string out;
  for (size_t i = 0; i < segs.size(); ++i) out += (i ? "/" : "") + segs[i];
  return out;
}

static bool starts_with(const std::string& s, std::string_view p) { return s.compare(0, p.size(), p) == 0; }

// Range end checked in u64 without overflow: offset <= total && size <= total - offset.
static bool range_ok(uint64_t total, uint64_t offset, uint64_t size) { return offset <= total && size <= total - offset; }

Error FileSystem::size(std::string_view path, uint64_t& out) const {
  std::vector<uint8_t> all;
  Error e = read(path, all);
  if (e == Error::None) out = all.size();
  return e;
}

Error FileSystem::read_range(std::string_view path, uint64_t offset, uint64_t size, std::vector<uint8_t>& out) const {
  std::vector<uint8_t> all;
  Error e = read(path, all);
  if (e != Error::None) return e;
  if (!range_ok(all.size(), offset, size)) return Error::Io;
  out.assign(all.begin() + static_cast<std::ptrdiff_t>(offset), all.begin() + static_cast<std::ptrdiff_t>(offset + size));
  return Error::None;
}

bool MemoryFileSystem::mount(std::string_view path, std::vector<uint8_t> bytes) {
  std::string n = normalize(path);
  if (!starts_with(n, "root/")) return false;
  files_[n] = std::move(bytes);
  return true;
}

size_t MemoryFileSystem::unmount_prefix(std::string_view prefix) {
  std::string p = normalize(prefix) + "/";  // directory semantics: "lev1/" must not match "lev10/..."
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
  log_read(n, 0, out.size());
  return Error::None;
}

Error MemoryFileSystem::size(std::string_view path, uint64_t& out) const {
  std::string n = normalize(path);
  if (!starts_with(n, "root/") && !starts_with(n, "user/")) return Error::BadMount;
  auto it = files_.find(n);
  if (it == files_.end()) return Error::NotFound;
  out = it->second.size();
  return Error::None;
}

Error MemoryFileSystem::read_range(std::string_view path, uint64_t offset, uint64_t size, std::vector<uint8_t>& out) const {
  std::string n = normalize(path);
  if (!starts_with(n, "root/") && !starts_with(n, "user/")) return Error::BadMount;
  auto it = files_.find(n);
  if (it == files_.end()) return Error::NotFound;
  if (!range_ok(it->second.size(), offset, size)) return Error::Io;
  auto first = it->second.begin() + static_cast<std::ptrdiff_t>(offset);
  out.assign(first, first + static_cast<std::ptrdiff_t>(size));
  log_read(n, offset, size);
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
