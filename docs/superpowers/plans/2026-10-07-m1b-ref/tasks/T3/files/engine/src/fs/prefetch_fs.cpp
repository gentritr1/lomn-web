#include "lomn/prefetch_fs.h"

#include <algorithm>
#include <cstring>
#include <new>

namespace lomn::fs {
namespace {

bool starts_with(const std::string& s, std::string_view p) { return s.compare(0, p.size(), p) == 0; }
bool set_err(std::string* err, const std::string& m) {
  if (err) *err = m;
  return false;
}
constexpr uint64_t kMaxJsSafeInteger = (1ull << 53) - 1;

}  // namespace

PrefetchFileSystem::PrefetchFileSystem(uint64_t budget_bytes) : budget_(budget_bytes) {}

bool PrefetchFileSystem::add_file(std::string_view path, uint64_t size, std::string* err) {
  if (path.size() > kMaxPathBytes) return set_err(err, "path longer than kMaxPathBytes");
  const std::string n = normalize(path);
  if (!starts_with(n, "root/") || n.size() <= 5) return set_err(err, "path outside root/: " + n);
  if (size > kMaxJsSafeInteger) return set_err(err, "size out of range: " + n);
  if (index_.size() >= kMaxFiles) return set_err(err, "more than kMaxFiles files");
  if (index_.count(n)) return set_err(err, "duplicate path: " + n);
  index_[n].size = size;
  return true;
}

const PrefetchFileSystem::Range* PrefetchFileSystem::find(const File& f, uint64_t offset, uint64_t length) const {
  for (const auto& r : f.ranges)
    if (r->committed && offset >= r->offset && length <= r->length && offset - r->offset <= r->length - length) return r.get();
  return nullptr;
}

bool PrefetchFileSystem::is_resident(std::string_view path, uint64_t offset, uint64_t length) const {
  auto it = index_.find(normalize(path));
  return it != index_.end() && find(it->second, offset, length) != nullptr;
}

uint8_t* PrefetchFileSystem::reserve(std::string_view path, uint64_t offset, uint64_t length, std::string* err) {
  if (path.size() > kMaxPathBytes) return set_err(err, "path longer than kMaxPathBytes"), nullptr;
  const std::string n = normalize(path);
  auto it = index_.find(n);
  if (it == index_.end()) return set_err(err, "not in the index: " + n), nullptr;
  File& f = it->second;
  if (length == 0 || length > kMaxRangeBytes) return set_err(err, "length out of range: " + n), nullptr;
  if (offset > f.size || length > f.size - offset) return set_err(err, "range outside the file: " + n), nullptr;
  if (find(f, offset, length)) return set_err(err, "already resident: " + n), nullptr;
  if (range_count_ >= kMaxResidentRanges) return set_err(err, "more than kMaxResidentRanges ranges"), nullptr;
  if (length > budget_ - std::min(budget_, resident_bytes_ + pending_bytes_)) return set_err(err, "over the byte budget"), nullptr;
  auto r = std::make_unique<Range>();
  r->offset = offset;
  r->length = length;
  r->bytes.reset(new (std::nothrow) uint8_t[static_cast<size_t>(length)]);
  if (!r->bytes) return set_err(err, "allocation failed"), nullptr;
  uint8_t* handle = r->bytes.get();
  pending_[handle] = {n, r.get()};
  f.ranges.push_back(std::move(r));
  ++range_count_;
  pending_bytes_ += length;
  peak_bytes_ = std::max(peak_bytes_, resident_bytes_ + pending_bytes_);
  return handle;
}

bool PrefetchFileSystem::commit(const uint8_t* handle) {
  auto it = pending_.find(handle);
  if (it == pending_.end()) return false;
  Range* r = it->second.second;
  r->committed = true;
  pending_bytes_ -= r->length;
  resident_bytes_ += r->length;
  ++committed_count_;
  pending_.erase(it);
  return true;
}

bool PrefetchFileSystem::abort(const uint8_t* handle) {
  auto it = pending_.find(handle);
  if (it == pending_.end()) return false;
  File& f = index_.at(it->second.first);
  Range* r = it->second.second;
  pending_bytes_ -= r->length;
  f.ranges.erase(std::find_if(f.ranges.begin(), f.ranges.end(), [r](const std::unique_ptr<Range>& x) { return x.get() == r; }));
  --range_count_;
  pending_.erase(it);
  return true;
}

void PrefetchFileSystem::next_generation() {
  ++generation_;
  misses_.clear();
  miss_set_.clear();
  misses_dropped_ = 0;
}

uint32_t PrefetchFileSystem::pin_touched() {
  uint32_t n = 0;
  for (auto& kv : index_)
    for (auto& r : kv.second.ranges)
      if (r->committed && r->touched_gen == generation_ && !r->pinned) {
        r->pinned = true;
        ++n;
      }
  return n;
}

uint64_t PrefetchFileSystem::release_untouched() {
  uint64_t freed = 0;
  for (auto& kv : index_) {
    auto& v = kv.second.ranges;
    for (auto it = v.begin(); it != v.end();) {
      Range& r = **it;
      if (r.committed && !r.pinned && r.touched_gen != generation_) {
        freed += r.length;
        resident_bytes_ -= r.length;
        --committed_count_;
        --range_count_;
        it = v.erase(it);
      } else {
        ++it;
      }
    }
  }
  return freed;
}

std::vector<ByteRange> PrefetchFileSystem::touched() const {
  std::vector<ByteRange> out;
  for (const auto& kv : index_)
    for (const auto& r : kv.second.ranges)
      if (r->committed && r->touched_gen == generation_) out.push_back({kv.first, r->offset, r->length});
  std::sort(out.begin(), out.end());
  return out;
}

uint64_t PrefetchFileSystem::pinned_bytes() const {
  uint64_t n = 0;
  for (const auto& kv : index_)
    for (const auto& r : kv.second.ranges)
      if (r->committed && r->pinned) n += r->length;
  return n;
}

void PrefetchFileSystem::note_miss(const std::string& n, uint64_t offset, uint64_t length) const {
  ++misses_total_;
  ByteRange b{n, offset, length};
  if (miss_set_.count(b)) return;
  if (misses_.size() >= kMaxMissesPerGeneration) {
    ++misses_dropped_;
    return;
  }
  miss_set_.insert(b);
  misses_.push_back(std::move(b));
}

Error PrefetchFileSystem::read_at(const std::string& n, uint64_t offset, uint64_t length, std::vector<uint8_t>& out) const {
  auto it = index_.find(n);
  if (it == index_.end()) return Error::NotFound;
  const File& f = it->second;
  if (offset > f.size || length > f.size - offset) return Error::Io;  // outside the file: a caller error, not a miss
  if (length == 0) {  // nothing to move (e.g. the 6 zero-byte blockfile entries): never a miss, nothing to fetch
    out.clear();
    log_read(n, offset, 0);
    return Error::None;
  }
  const Range* r = find(f, offset, length);
  if (!r) {
    note_miss(n, offset, length);
    return Error::Io;
  }
  r->touched_gen = generation_;
  const uint8_t* first = r->bytes.get() + (offset - r->offset);
  out.assign(first, first + static_cast<size_t>(length));
  log_read(n, offset, length);
  return Error::None;
}

bool PrefetchFileSystem::exists(std::string_view path) const {
  const std::string n = normalize(path);
  return index_.count(n) != 0 || user_.count(n) != 0;
}

Error PrefetchFileSystem::size(std::string_view path, uint64_t& out) const {
  const std::string n = normalize(path);
  if (starts_with(n, "user/")) {
    auto u = user_.find(n);
    if (u == user_.end()) return Error::NotFound;
    out = u->second.size();
    return Error::None;
  }
  if (!starts_with(n, "root/")) return Error::BadMount;
  auto it = index_.find(n);
  if (it == index_.end()) return Error::NotFound;
  out = it->second.size;
  return Error::None;
}

Error PrefetchFileSystem::read(std::string_view path, std::vector<uint8_t>& out) const {
  const std::string n = normalize(path);
  if (starts_with(n, "user/")) {
    auto u = user_.find(n);
    if (u == user_.end()) return Error::NotFound;
    out = u->second;
    log_read(n, 0, out.size());
    return Error::None;
  }
  if (!starts_with(n, "root/")) return Error::BadMount;
  auto it = index_.find(n);
  if (it == index_.end()) return Error::NotFound;
  return read_at(n, 0, it->second.size, out);
}

Error PrefetchFileSystem::read_range(std::string_view path, uint64_t offset, uint64_t size, std::vector<uint8_t>& out) const {
  const std::string n = normalize(path);
  if (starts_with(n, "user/")) {
    auto u = user_.find(n);
    if (u == user_.end()) return Error::NotFound;
    if (offset > u->second.size() || size > u->second.size() - offset) return Error::Io;
    out.assign(u->second.begin() + static_cast<std::ptrdiff_t>(offset), u->second.begin() + static_cast<std::ptrdiff_t>(offset + size));
    log_read(n, offset, size);
    return Error::None;
  }
  if (!starts_with(n, "root/")) return Error::BadMount;
  return read_at(n, offset, size, out);
}

Error PrefetchFileSystem::write(std::string_view path, const uint8_t* data, size_t size) {
  const std::string n = normalize(path);
  if (starts_with(n, "root/")) return Error::ReadOnly;
  if (!starts_with(n, "user/")) return Error::BadMount;
  user_[n].assign(data, data + size);
  return Error::None;
}

}  // namespace lomn::fs
