#include "lomn/view/texture_cache.h"

#include <atomic>
#if defined(__aarch64__) && defined(__ARM_FEATURE_SHA2)
#include <arm_neon.h>
#define LOMN_SHA256_ARMV8 1
#endif

namespace lomn::view {
namespace {

std::atomic<uint64_t> g_instances{0};

constexpr uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#if !defined(LOMN_SHA256_ARMV8)
inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void compress(uint32_t h[8], const uint8_t* p) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i)
    w[i] = (uint32_t(p[4 * i]) << 24) | (uint32_t(p[4 * i + 1]) << 16) | (uint32_t(p[4 * i + 2]) << 8) | uint32_t(p[4 * i + 3]);
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
  for (int i = 0; i < 64; ++i) {
    const uint32_t t1 = k + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kK[i] + w[i];
    const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    k = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += k;
}
#endif

// `blocks` 64-byte blocks. Portable FIPS 180-4 rounds everywhere (wasm32 included); on arm64 with the SHA-2 extension
// (Apple silicon) the same rounds via the ARMv8 SHA256H/H2/SU0/SU1 instructions: 210 MB/s portable vs several GB/s
// (measured 2026-10-07, see the M1a-4 Task 3 report). Both give the same digest (the FIPS vectors run on both builds).
void compress_blocks(uint32_t h[8], const uint8_t* p, size_t blocks) {
#if defined(LOMN_SHA256_ARMV8)
  uint32x4_t abcd = vld1q_u32(&h[0]), efgh = vld1q_u32(&h[4]);
  for (; blocks; --blocks, p += 64) {
    const uint32x4_t abcd0 = abcd, efgh0 = efgh;
    uint32x4_t msg[4];
    for (int i = 0; i < 4; ++i) msg[i] = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p + 16 * i)));
    for (int i = 0; i < 16; ++i) {  // rounds 4i..4i+3 use W[4i..4i+3] = msg[i & 3]
      const uint32x4_t wk = vaddq_u32(msg[i & 3], vld1q_u32(&kK[4 * i]));
      const uint32x4_t prev = abcd;
      abcd = vsha256hq_u32(abcd, efgh, wk);
      efgh = vsha256h2q_u32(efgh, prev, wk);
      if (i < 12)  // W[4i+16..4i+19] from W[4i..4i+15]
        msg[i & 3] = vsha256su1q_u32(vsha256su0q_u32(msg[i & 3], msg[(i + 1) & 3]), msg[(i + 2) & 3], msg[(i + 3) & 3]);
    }
    abcd = vaddq_u32(abcd, abcd0);
    efgh = vaddq_u32(efgh, efgh0);
  }
  vst1q_u32(&h[0], abcd);
  vst1q_u32(&h[4], efgh);
#else
  for (; blocks; --blocks, p += 64) compress(h, p);
#endif
}

}  // namespace

Sha256Digest sha256(const uint8_t* data, size_t size) {
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  const size_t i = size / 64 * 64;
  compress_blocks(h, data, size / 64);
  // Tail: the remaining < 64 bytes, 0x80, zeros, then the bit length (u64 big-endian) -> one or two blocks.
  uint8_t block[128] = {};
  const size_t rest = size - i;
  for (size_t k = 0; k < rest; ++k) block[k] = data[i + k];
  block[rest] = 0x80;
  const size_t blocks = rest < 56 ? 1 : 2;
  const uint64_t bits = uint64_t(size) * 8;
  for (int k = 0; k < 8; ++k) block[blocks * 64 - 1 - k] = static_cast<uint8_t>(bits >> (8 * k));
  compress_blocks(h, block, blocks);
  Sha256Digest d;
  for (int k = 0; k < 8; ++k)
    for (int j = 0; j < 4; ++j) d[4 * k + j] = static_cast<uint8_t>(h[k] >> (24 - 8 * j));
  return d;
}

std::string sha256_hex(const Sha256Digest& d) {
  static const char* hex = "0123456789abcdef";
  std::string s;
  s.reserve(64);
  for (uint8_t b : d) {
    s.push_back(hex[b >> 4]);
    s.push_back(hex[b & 15]);
  }
  return s;
}

uint64_t DecodedTexture::bytes() const {
  uint64_t n = 0;
  for (const MipLevel& l : mips) n += l.rgba.size();
  return n;
}

TextureCache::TextureCache(uint64_t cap_bytes) : cap_(cap_bytes) { g_instances.fetch_add(1, std::memory_order_relaxed); }

uint64_t TextureCache::instances_constructed() { return g_instances.load(std::memory_order_relaxed); }

std::shared_ptr<const DecodedTexture> TextureCache::find(const std::string& path, const Sha256Digest& digest) {
  const auto it = entries_.find(Key(path, digest));
  if (it == entries_.end()) {
    ++stats_.misses;
    return nullptr;
  }
  ++stats_.hits;
  lru_.splice(lru_.begin(), lru_, it->second.lru);  // most recent; iterators stay valid
  return it->second.value;
}

void TextureCache::insert(const std::string& path, const Sha256Digest& digest, std::shared_ptr<const DecodedTexture> value) {
  if (!value || value->mips.empty()) return;  // the loader reads mips.front() of a hit
  const uint64_t bytes = value->bytes();
  if (bytes > cap_) return;  // could never fit: storing it would evict everything and then itself
  Key key(path, digest);
  auto it = entries_.find(key);
  if (it != entries_.end()) {  // replace in place
    stats_.bytes -= it->second.bytes;
    it->second.value = std::move(value);
    it->second.bytes = bytes;
    lru_.splice(lru_.begin(), lru_, it->second.lru);
  } else {
    lru_.push_front(key);
    Entry e;
    e.value = std::move(value);
    e.bytes = bytes;
    e.lru = lru_.begin();
    it = entries_.emplace(std::move(key), std::move(e)).first;
    ++stats_.entries;
  }
  stats_.bytes += bytes;
  ++stats_.inserts;
  evict_to_cap(&it->first);
}

void TextureCache::evict_to_cap(const Key* keep) {
  while (stats_.bytes > cap_ && !lru_.empty()) {
    const Key& victim = lru_.back();
    if (keep && victim == *keep) break;  // cannot happen while bytes(keep) <= cap; defensive
    const auto it = entries_.find(victim);
    stats_.bytes -= it->second.bytes;
    entries_.erase(it);  // drops the cache's reference only; holders keep theirs
    lru_.pop_back();
    --stats_.entries;
    ++stats_.evictions;
  }
}

}  // namespace lomn::view
