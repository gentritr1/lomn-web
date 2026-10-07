// Little-endian byte builders for the synthetic SAGE test data.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace sage_test {

using Bytes = std::vector<uint8_t>;

inline void put8(Bytes& b, uint8_t v) { b.push_back(v); }
inline void put16(Bytes& b, uint16_t v) {
  b.push_back(static_cast<uint8_t>(v));
  b.push_back(static_cast<uint8_t>(v >> 8));
}
inline void put32(Bytes& b, uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
inline void putf(Bytes& b, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  put32(b, u);
}
inline void putstr(Bytes& b, const std::string& s) { b.insert(b.end(), s.begin(), s.end()); }
inline void set32(Bytes& b, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i) b[at + i] = static_cast<uint8_t>(v >> (8 * i));
}

struct BlkEntrySpec {
  std::string name;  // up to 40 bytes; exactly 40 makes it unterminated
  Bytes stored;      // bytes placed in the file
  uint32_t full_size;
  uint32_t flags;
};

// "BLKF", version 1, count, entries, then payloads in order.
inline Bytes make_blk(const std::vector<BlkEntrySpec>& es) {
  Bytes b;
  putstr(b, "BLKF");
  put32(b, 1);
  put32(b, static_cast<uint32_t>(es.size()));
  uint32_t off = static_cast<uint32_t>(12 + 56 * es.size());
  for (const auto& e : es) {
    Bytes name(40, 0);
    std::memcpy(name.data(), e.name.data(), e.name.size() < 40 ? e.name.size() : 40);
    b.insert(b.end(), name.begin(), name.end());
    put32(b, static_cast<uint32_t>(e.stored.size()));
    put32(b, e.full_size);
    put32(b, e.flags);
    put32(b, off);
    off += static_cast<uint32_t>(e.stored.size());
  }
  for (const auto& e : es) b.insert(b.end(), e.stored.begin(), e.stored.end());
  return b;
}

inline BlkEntrySpec stored(const std::string& name, const std::string& data) {
  return {name, Bytes(data.begin(), data.end()), static_cast<uint32_t>(data.size()), 2};
}

}  // namespace sage_test
