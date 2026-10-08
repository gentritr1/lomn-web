// M2a-1 T1: the fixed-format OSI VM trace (docs/osi/trace.md). Every record is one ASCII line; floats only as f32 bits.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "lomn/osi/variant.h"

namespace lomn::osi {

// A line sink. `line` receives one record WITHOUT its trailing LF.
struct TraceSink {
  virtual void line(std::string_view s) = 0;
  virtual ~TraceSink() = default;
};

// Writes each record plus LF to a FILE* (not owned) or to a path (owned, closed by the destructor).
class FileTrace : public TraceSink {
 public:
  explicit FileTrace(std::FILE* f) : f_(f) {}
  explicit FileTrace(const std::string& path);
  ~FileTrace() override;
  bool ok() const { return f_ != nullptr; }
  void line(std::string_view s) override;

 private:
  std::FILE* f_ = nullptr;
  bool own_ = false;
};

// Keeps every record in memory (tests, the conformance runner).
class StringTrace : public TraceSink {
 public:
  void line(std::string_view s) override { lines.emplace_back(s); }
  std::string text() const;  // every line + LF
  std::vector<std::string> lines;
};

// Digest mode (trace.md §2): each `K` block becomes `D %6d %8d %016x` (frame, line count, FNV-1a 64 of the block's bytes,
// LFs included); the header line (`#...`) is copied verbatim; finish() writes `T %d %016x` (total lines, FNV-1a 64 of the
// whole trace). Lines before the first `K` (the header aside) form a block reported with frame -1 (OPEN for T4: the
// format does not name it).
class DigestTrace : public TraceSink {
 public:
  explicit DigestTrace(TraceSink* out) : out_(out) {}
  void line(std::string_view s) override;
  void finish();

 private:
  void flush_block();
  TraceSink* out_;
  long frame_ = -1;
  uint64_t block_lines_ = 0, total_lines_ = 0;
  uint64_t block_hash_ = 0xcbf29ce484222325ull, total_hash_ = 0xcbf29ce484222325ull;
  bool finished_ = false;
};

uint64_t fnv1a64(uint64_t h, std::string_view bytes);

// Field formatters (trace.md §1).
std::string fmt_variant(Variant v);                      // "%04x:%08x"
const char* kEmptySlot();                                // "----:--------"
std::string fmt_name(std::string_view name);             // name-token escaping (C/M/N lines, natives.log)
std::string fmt_message(std::string_view msg);           // E-line message escaping (without the quotes)
std::string fmt_hex(std::string_view bytes);             // lowercase hex, two digits per byte

// SHA-256 (lowercase hex) for the trace header (`script=… natives=… mods=… types=…`).
std::string sha256_hex(const uint8_t* data, size_t n);
std::string header_line(const std::string& script_sha, const std::string& natives_sha, const std::string& mods_sha,
                        const std::string& types_sha);

}  // namespace lomn::osi
