// Internal: bounds-checked little-endian reads over untrusted bytes (spec §5). Every check is written as
// "count <= remaining / stride" or in u64 so it cannot wrap on wasm32, where size_t is 32-bit.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>

namespace lomn::sage::detail {

inline uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
inline float lef32(const uint8_t* p) {
  uint32_t u = le32(p);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
inline double lef64(const uint8_t* p) {
  uint64_t u = static_cast<uint64_t>(le32(p)) | (static_cast<uint64_t>(le32(p + 4)) << 32);
  double d;
  std::memcpy(&d, &u, 8);
  return d;
}

// True when [offset, offset + size) lies inside a buffer of `total` bytes. All operands u64: no wrap.
inline bool in_range(uint64_t total, uint64_t offset, uint64_t size) { return offset <= total && size <= total - offset; }

// Sequential reader. Every accessor returns false (and leaves pos unchanged) when the bytes are not there.
class Cursor {
 public:
  Cursor(const uint8_t* data, size_t size, size_t pos = 0) : d_(data), n_(size), p_(pos) {}
  size_t pos() const { return p_; }
  size_t remaining() const { return n_ - p_; }
  bool at_end() const { return p_ >= n_; }
  bool has(uint64_t k) const { return k <= remaining(); }
  bool u16(uint16_t& v) {
    if (!has(2)) return false;
    v = le16(d_ + p_);
    p_ += 2;
    return true;
  }
  bool peek16(uint16_t& v) const {
    if (!has(2)) return false;
    v = le16(d_ + p_);
    return true;
  }
  bool u32(uint32_t& v) {
    if (!has(4)) return false;
    v = le32(d_ + p_);
    p_ += 4;
    return true;
  }
  bool u8(uint8_t& v) {  // M1a-3 (OSI reader)
    if (!has(1)) return false;
    v = d_[p_++];
    return true;
  }
  bool str(uint64_t k, std::string& s) {  // M1a-3: k raw bytes as a string
    if (!has(k)) return false;
    s.assign(reinterpret_cast<const char*>(d_ + p_), static_cast<size_t>(k));
    p_ += static_cast<size_t>(k);
    return true;
  }
  bool skip(uint64_t k) {
    if (!has(k)) return false;
    p_ += static_cast<size_t>(k);
    return true;
  }
  const uint8_t* here() const { return d_ + p_; }

 private:
  const uint8_t* d_;
  size_t n_;
  size_t p_;
};

inline bool fail(std::string* err, std::string msg) {
  if (err) *err = std::move(msg);
  return false;
}

}  // namespace lomn::sage::detail
