// M1b: the browser's file system (spec §4.3 "sync I/O solution"). The engine keeps its synchronous lomn::fs API; the
// bytes come from the player's own files, which JavaScript reads asynchronously (File System Access handle or OPFS) and
// writes straight into buffers this class allocates in the wasm heap, chunk by chunk (no whole-file JS buffer).
//
// Model:
//  - an INDEX of every file of the install (normalised path -> size), registered before anything is read: exists()
//    and size() answer from it, so blockfile headers can be sized without any bytes being resident;
//  - RESIDENT byte ranges: reserve(path, offset, length) allocates the destination, JS fills it, commit() makes it
//    readable. read()/read_range() succeed only when ONE resident range contains the whole request;
//  - a read of an indexed file outside the resident set is a MISS: it fails with Error::Io (never blocks, never hangs)
//    and is recorded (distinct, in first-seen order). JS fetches the misses and re-runs the load: the engine's own
//    reads drive the prefetch ("discovery"), so no manifest has to be shipped (REF4 decision D2);
//  - TOUCH GENERATIONS: every successful read stamps the range it hit with the current generation. After an area load
//    whose last round had no miss, release_untouched() frees every unpinned range that load did not read: the previous
//    area's buffers go, files shared with the new area stay (spec §4.3 "release on area switch"). pin_touched() marks
//    the boot set (never released).
// Untrusted input: paths, offsets and lengths come from JS (ultimately from the player's folder or a cached manifest).
// Every count, length and total is capped (constants below, each with the measured real value) and all range arithmetic
// is u64 (wasm32 size_t is 32-bit); a rejected call changes nothing.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "lomn/fs.h"

namespace lomn::fs {

struct ByteRange {
  std::string path;  // normalised ("root/data/...")
  uint64_t offset = 0, length = 0;
  bool operator<(const ByteRange& o) const { return std::tie(path, offset, length) < std::tie(o.path, o.offset, o.length); }
  bool operator==(const ByteRange& o) const { return path == o.path && offset == o.offset && length == o.length; }
};

class PrefetchFileSystem final : public FileSystem {
 public:
  // Caps. Measured on the Beta 1.5.0 install (2026-10-07): 529 files (incl. 2 .DS_Store), longest relative path 54 bytes,
  // largest single engine read 2,511,728 bytes (the largest blockfile entry; Base.osi 486,274), at most a few hundred
  // distinct reads per area (lomn_view --manifest, REF4 REPORT). Headroom >= 16x everywhere.
  static constexpr uint32_t kMaxFiles = 65536;
  static constexpr uint32_t kMaxPathBytes = 1024;
  static constexpr uint64_t kMaxRangeBytes = 64ull << 20;   // = sage::kMaxEntrySize, the largest entry the engine reads
  static constexpr uint32_t kMaxResidentRanges = 65536;     // resident + pending
  static constexpr uint32_t kMaxMissesPerGeneration = 4096; // distinct misses kept; more are counted, not stored
  static constexpr uint64_t kDefaultBudgetBytes = 1536ull << 20;  // resident + pending bytes (wasm32 heap max is 2 GB)

  explicit PrefetchFileSystem(uint64_t budget_bytes = kDefaultBudgetBytes);
  PrefetchFileSystem(const PrefetchFileSystem&) = delete;
  PrefetchFileSystem& operator=(const PrefetchFileSystem&) = delete;

  // ---- index ----
  // path must normalise to "root/..."; size <= 2^53 (a JS number). Re-adding a path is an error. False + *err on reject.
  bool add_file(std::string_view path, uint64_t size, std::string* err = nullptr);
  size_t file_count() const { return index_.size(); }

  // ---- resident data ----
  // Allocates [offset, offset+length) of an indexed file (length 1..kMaxRangeBytes, inside the file, within the budget
  // and the range cap) and returns a writable pointer, the handle for commit()/abort(). nullptr + *err on reject, also
  // when the range is already resident (is_resident()).
  uint8_t* reserve(std::string_view path, uint64_t offset, uint64_t length, std::string* err = nullptr);
  bool commit(const uint8_t* handle);  // the bytes are written: readable from now on. False for an unknown handle.
  bool abort(const uint8_t* handle);   // frees a reservation that will not be filled
  bool is_resident(std::string_view path, uint64_t offset, uint64_t length) const;

  // ---- generations, release, boot pinning ----
  void next_generation();          // starts a new touch generation and clears the miss list
  uint32_t generation() const { return generation_; }
  uint32_t pin_touched();          // pins the ranges touched in the current generation; returns how many
  uint64_t release_untouched();    // frees unpinned committed ranges not touched in the current generation; bytes freed
  std::vector<ByteRange> touched() const;  // ranges touched in the current generation, sorted

  // ---- misses ----
  const std::vector<ByteRange>& misses() const { return misses_; }
  uint64_t misses_dropped() const { return misses_dropped_; }  // current generation, beyond kMaxMissesPerGeneration
  uint64_t misses_total() const { return misses_total_; }      // since construction

  // ---- stats ----
  uint64_t resident_bytes() const { return resident_bytes_; }  // committed
  uint64_t pending_bytes() const { return pending_bytes_; }
  uint64_t peak_bytes() const { return peak_bytes_; }          // max of committed + pending
  uint32_t resident_ranges() const { return static_cast<uint32_t>(committed_count_); }
  uint64_t pinned_bytes() const;

  // ---- FileSystem ----
  bool exists(std::string_view path) const override;
  Error read(std::string_view path, std::vector<uint8_t>& out) const override;
  Error write(std::string_view path, const uint8_t* data, size_t size) override;  // root/: ReadOnly; user/: in memory
  Error size(std::string_view path, uint64_t& out) const override;
  Error read_range(std::string_view path, uint64_t offset, uint64_t size, std::vector<uint8_t>& out) const override;

 private:
  struct Range {
    uint64_t offset = 0, length = 0;
    std::unique_ptr<uint8_t[]> bytes;
    bool committed = false, pinned = false;
    mutable uint32_t touched_gen = 0;  // 0 = never
  };
  struct File {
    uint64_t size = 0;
    std::vector<std::unique_ptr<Range>> ranges;  // unsorted; a handful to a few hundred per file
  };
  const Range* find(const File& f, uint64_t offset, uint64_t length) const;  // a committed range containing the request
  Error read_at(const std::string& n, uint64_t offset, uint64_t length, std::vector<uint8_t>& out) const;
  void note_miss(const std::string& n, uint64_t offset, uint64_t length) const;

  uint64_t budget_;
  std::map<std::string, File> index_;
  std::map<std::string, std::vector<uint8_t>> user_;
  std::map<const uint8_t*, std::pair<std::string, Range*>> pending_;  // handle -> (path, range)
  uint32_t generation_ = 1;
  uint64_t resident_bytes_ = 0, pending_bytes_ = 0, peak_bytes_ = 0;
  size_t range_count_ = 0, committed_count_ = 0;
  mutable std::vector<ByteRange> misses_;
  mutable std::set<ByteRange> miss_set_;
  mutable uint64_t misses_dropped_ = 0, misses_total_ = 0;
};

}  // namespace lomn::fs
