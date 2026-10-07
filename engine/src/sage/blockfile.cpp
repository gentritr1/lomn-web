#include "lomn/sage/blockfile.h"

#include <memory>

#include "bytes.h"

namespace lomn::sage {
using detail::fail;
using detail::in_range;
using detail::le32;

static uint8_t fold(uint8_t c) { return (c >= 0x41 && c <= 0x5a) ? static_cast<uint8_t>(c | 0x60) : c; }

// C-string view: the original compares NUL-terminated strings, so anything after an embedded NUL is ignored.
static std::string_view cstr(std::string_view s) {
  size_t z = s.find('\0');
  return z == std::string_view::npos ? s : s.substr(0, z);
}

int block_strcmp(std::string_view a, std::string_view b) {
  a = cstr(a);
  b = cstr(b);
  for (size_t i = 0;; ++i) {
    bool ea = i >= a.size(), eb = i >= b.size();
    if (ea && !eb) return -1;
    if (eb && !ea) return 1;
    if (ea && eb) return 0;
    uint8_t ca = fold(static_cast<uint8_t>(a[i])), cb = fold(static_cast<uint8_t>(b[i]));
    if (ca != cb) return ca < cb ? -1 : 1;
  }
}

std::string_view block_basename(std::string_view request) {
  request = cstr(request);
  size_t cut = request.find_last_of("/\\");
  return cut == std::string_view::npos ? request : request.substr(cut + 1);
}

// M1a-4 (fuzz_formats finding): decodes into a local buffer; `out` is replaced only on success and cleared on any
// failure, so a rejected stream never leaves a partial decode behind.
static bool decode_lzss_into(const uint8_t* in, size_t in_size, uint32_t out_size, std::vector<uint8_t>& out, std::string* err);

bool decode_lzss(const uint8_t* in, size_t in_size, uint32_t out_size, std::vector<uint8_t>& out, std::string* err) {
  std::vector<uint8_t> dec;
  if (!decode_lzss_into(in, in_size, out_size, dec, err)) {
    out.clear();
    return false;
  }
  out = std::move(dec);
  return true;
}

static bool decode_lzss_into(const uint8_t* in, size_t in_size, uint32_t out_size, std::vector<uint8_t>& out, std::string* err) {
  if (static_cast<uint64_t>(out_size) > static_cast<uint64_t>(in_size) * kLzssMaxRatio)
    return fail(err, "lzss: declared size exceeds expansion-ratio cap");
  out.assign(out_size, 0);
  uint8_t ring[4096];
  for (size_t i = 0; i < 0xfee; ++i) ring[i] = 0x20;
  for (size_t i = 0xfee; i < 4096; ++i) ring[i] = 0;  // never read before written by a valid stream
  uint32_t r = 0xfee, flags = 0;
  size_t ip = 0, op = 0;
  for (;;) {
    flags >>= 1;
    if ((flags & 0x100) == 0) {
      if (ip >= in_size) break;
      flags = in[ip++] | 0xff00u;
    }
    if (flags & 1) {
      if (ip >= in_size) break;
      if (op >= out_size) return fail(err, "lzss: output overrun");
      uint8_t c = in[ip++];
      out[op++] = c;
      ring[r] = c;
      r = (r + 1) & 0xfff;
    } else {
      if (ip + 1 >= in_size) break;  // the original stops when either byte of the pair is missing
      uint32_t lo = in[ip], hi = in[ip + 1];
      ip += 2;
      uint32_t pos = lo | ((hi & 0xf0) << 4), len = (hi & 0x0f) + 3;
      if (len > out_size - op) return fail(err, "lzss: output overrun");
      for (uint32_t k = 0; k < len; ++k) {
        uint8_t c = ring[(pos + k) & 0xfff];
        out[op++] = c;
        ring[r] = c;
        r = (r + 1) & 0xfff;
      }
    }
  }
  if (op != out_size) return fail(err, "lzss: stream ended after " + std::to_string(op) + " of " + std::to_string(out_size) + " bytes");
  return true;
}

// Extends a 40-byte unterminated name with the in-memory bytes StrCmp keeps reading, up to the next NUL or the end
// of the table. false when the run is longer than kMaxNameRun (hostile table: each name would copy up to the whole
// remaining table, count^2 bytes overall).
static bool extend_name_run(const std::vector<uint8_t>& table, size_t entry, std::string& lookup_name) {
  size_t ext = 0;
  for (size_t k = entry * kBlockEntrySize + kBlockNameSize; k < table.size() && table[k] != 0; ++k) {
    if (++ext > kMaxNameRun) return false;
    lookup_name.push_back(static_cast<char>(table[k]));
  }
  return true;
}

bool BlockFile::open(uint64_t file_size, RangeReader reader, std::string* err) {
  *this = BlockFile();
  std::vector<uint8_t> head;
  if (file_size < kBlockHeaderSize) return fail(err, "blk: shorter than the 12-byte header");
  if (!reader(0, kBlockHeaderSize, head) || head.size() != kBlockHeaderSize) return fail(err, "blk: header read failed");
  if (le32(head.data()) != 0x464b4c42u) return fail(err, "blk: bad magic (want BLKF)");
  uint32_t version = le32(head.data() + 4), count = le32(head.data() + 8);
  if (static_cast<uint64_t>(count) > (file_size - kBlockHeaderSize) / kBlockEntrySize)
    return fail(err, "blk: entry table (count*56) larger than the file");
  std::vector<uint8_t> table;
  uint64_t table_bytes = static_cast<uint64_t>(count) * kBlockEntrySize;
  if (!reader(kBlockHeaderSize, table_bytes, table) || table.size() != table_bytes) return fail(err, "blk: table read failed");
  std::vector<BlockEntry> entries;  // assigned to entries_ only when the whole table is good
  entries.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* e = table.data() + static_cast<size_t>(i) * kBlockEntrySize;
    BlockEntry be;
    size_t len = 0;
    while (len < kBlockNameSize && e[len] != 0) ++len;
    be.name.assign(reinterpret_cast<const char*>(e), len);
    be.name_terminated = len < kBlockNameSize;
    be.lookup_name = be.name;
    if (!be.name_terminated)  // StrCmp keeps reading the in-memory table after the 40 name bytes
      if (!extend_name_run(table, i, be.lookup_name)) return fail(err, "blk: unterminated name run exceeds 512 bytes");
    be.stored_size = le32(e + 40);
    be.full_size = le32(e + 44);
    be.flags = le32(e + 48);
    be.offset = le32(e + 52);
    entries.push_back(std::move(be));
  }
  entries_ = std::move(entries);
  version_ = version;
  file_size_ = file_size;
  reader_ = std::move(reader);
  open_ = true;
  return true;
}

bool BlockFile::open_bytes(std::vector<uint8_t> bytes, std::string* err) {
  auto data = std::make_shared<std::vector<uint8_t>>(std::move(bytes));
  uint64_t size = data->size();
  return open(size, [data](uint64_t off, uint64_t n, std::vector<uint8_t>& out) {
    if (!in_range(data->size(), off, n)) return false;
    auto first = data->begin() + static_cast<std::ptrdiff_t>(off);
    out.assign(first, first + static_cast<std::ptrdiff_t>(n));
    return true;
  }, err);
}

bool BlockFile::is_sorted() const {
  for (size_t i = 1; i < entries_.size(); ++i)
    if (block_strcmp(entries_[i - 1].lookup_name, entries_[i].lookup_name) >= 0) return false;
  return true;
}

int BlockFile::find(std::string_view request) const {
  // Verbatim shape of the decompiled loop, including its exit when the window shrinks to one probe.
  int n = entries_.size() > 0x7fffffff ? 0x7fffffff : static_cast<int>(entries_.size());
  if (n < 1) return -1;
  std::string_view key = block_basename(request);
  int lo = 0, hi = n;
  for (;;) {
    int mid = (lo + hi) >> 1;
    int c = block_strcmp(key, entries_[static_cast<size_t>(mid)].lookup_name);
    if (c == 0) return mid;
    if (lo == hi - 1) break;
    if (c < 0) hi = mid;
    else lo = mid;
  }
  return -1;
}

// M1a-4 (fuzz_formats finding): any failure leaves `out` empty (never the caller's old bytes or a partial decode).
bool BlockFile::read(size_t index, std::vector<uint8_t>& out, std::string* err) const {
  if (read_entry(index, out, err)) return true;
  out.clear();
  return false;
}

bool BlockFile::read_entry(size_t index, std::vector<uint8_t>& out, std::string* err) const {
  if (!open_) return fail(err, "blk: not open");
  if (index >= entries_.size()) return fail(err, "blk: entry index out of range");
  const BlockEntry& e = entries_[index];
  bool compressed = (e.flags & 1) != 0;
  // stored_size is only meaningful (and only read) for compressed entries; a stored one never uses it.
  if (e.full_size > kMaxEntrySize || (compressed && e.stored_size > kMaxEntrySize)) return fail(err, "blk: entry too large");
  uint64_t span = compressed ? e.stored_size : e.full_size;
  if (!in_range(file_size_, e.offset, span)) return fail(err, "blk: entry '" + e.name + "' offset+size outside the file");
  if (compressed && static_cast<uint64_t>(e.full_size) > static_cast<uint64_t>(e.stored_size) * kLzssMaxRatio)
    return fail(err, "blk: entry '" + e.name + "' exceeds the LZSS expansion-ratio cap");
  std::vector<uint8_t> raw;
  if (!reader_(e.offset, span, raw) || raw.size() != span) return fail(err, "blk: entry '" + e.name + "' read failed");
  if (!compressed) {
    out = std::move(raw);
    return true;
  }
  std::string lerr;
  if (!decode_lzss(raw.data(), raw.size(), e.full_size, out, &lerr)) return fail(err, "blk: entry '" + e.name + "': " + lerr);
  return true;
}

}  // namespace lomn::sage
