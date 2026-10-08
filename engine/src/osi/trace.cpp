// M2a-1 T1: trace sinks and field formatters (docs/osi/trace.md), FNV-1a 64, SHA-256.
#include "lomn/osi/trace.h"

#include <cstdlib>
#include <cstring>

namespace lomn::osi {

FileTrace::FileTrace(const std::string& path) : f_(std::fopen(path.c_str(), "wb")), own_(true) {}
FileTrace::~FileTrace() {
  if (own_ && f_) std::fclose(f_);
}
void FileTrace::line(std::string_view s) {
  if (!f_) return;
  std::fwrite(s.data(), 1, s.size(), f_);
  std::fputc('\n', f_);
}

std::string StringTrace::text() const {
  std::string t;
  for (const auto& l : lines) {
    t += l;
    t += '\n';
  }
  return t;
}

uint64_t fnv1a64(uint64_t h, std::string_view bytes) {
  for (unsigned char c : bytes) {
    h ^= c;
    h *= 0x100000001b3ull;
  }
  return h;
}

void DigestTrace::flush_block() {
  if (block_lines_ == 0 && frame_ == -1) return;
  char b[64];
  std::snprintf(b, sizeof b, "D %6ld %8llu %016llx", frame_, static_cast<unsigned long long>(block_lines_),
                static_cast<unsigned long long>(block_hash_));
  out_->line(b);
  block_lines_ = 0;
  block_hash_ = 0xcbf29ce484222325ull;
}

void DigestTrace::line(std::string_view s) {
  total_hash_ = fnv1a64(fnv1a64(total_hash_, s), "\n");
  ++total_lines_;
  if (!s.empty() && s[0] == '#') {
    out_->line(s);
    return;
  }
  if (!s.empty() && s[0] == 'K') {
    flush_block();
    frame_ = std::strtol(std::string(s.substr(1)).c_str(), nullptr, 10);
  }
  block_hash_ = fnv1a64(fnv1a64(block_hash_, s), "\n");
  ++block_lines_;
}

void DigestTrace::finish() {
  if (finished_) return;
  finished_ = true;
  flush_block();
  char b[64];
  std::snprintf(b, sizeof b, "T %llu %016llx", static_cast<unsigned long long>(total_lines_),
                static_cast<unsigned long long>(total_hash_));
  out_->line(b);
}

std::string fmt_variant(Variant v) {
  char b[16];
  std::snprintf(b, sizeof b, "%04x:%08x", v.type, v.value);
  return b;
}

const char* kEmptySlot() { return "----:--------"; }

std::string fmt_name(std::string_view name) {
  std::string o;
  for (unsigned char c : name) {
    if (c == '\\') {
      o += "\\\\";
    } else if (c >= 0x21 && c <= 0x7e) {
      o += static_cast<char>(c);
    } else {
      char b[8];
      std::snprintf(b, sizeof b, "\\x%02x", c);
      o += b;
    }
  }
  return o;
}

std::string fmt_message(std::string_view msg) {
  std::string o;
  for (unsigned char c : msg) {
    if (c == '"') {
      o += "\\\"";
    } else if (c == '\\') {
      o += "\\\\";
    } else if (c >= 0x20 && c <= 0x7e) {
      o += static_cast<char>(c);
    } else {
      char b[8];
      std::snprintf(b, sizeof b, "\\x%02x", c);
      o += b;
    }
  }
  return o;
}

std::string fmt_hex(std::string_view bytes) {
  static const char* d = "0123456789abcdef";
  std::string o;
  o.reserve(bytes.size() * 2);
  for (unsigned char c : bytes) {
    o += d[c >> 4];
    o += d[c & 15];
  }
  return o;
}

namespace {
const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
void block(uint32_t h[8], const uint8_t* p) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i) w[i] = (uint32_t(p[4 * i]) << 24) | (uint32_t(p[4 * i + 1]) << 16) | (uint32_t(p[4 * i + 2]) << 8) | p[4 * i + 3];
  for (int i = 16; i < 64; ++i) {
    uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
  for (int i = 0; i < 64; ++i) {
    uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = k + S1 + ch + K256[i] + w[i];
    uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = S0 + mj;
    k = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
  h[4] += e;
  h[5] += f;
  h[6] += g;
  h[7] += k;
}
}  // namespace

std::string sha256_hex(const uint8_t* data, size_t n) {
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  size_t i = 0;
  for (; i + 64 <= n; i += 64) block(h, data + i);
  uint8_t tail[128] = {};
  const size_t r = n - i;
  if (r) std::memcpy(tail, data + i, r);
  tail[r] = 0x80;
  const size_t tl = r + 1 + 8 <= 64 ? 64 : 128;
  const uint64_t bits = static_cast<uint64_t>(n) * 8;
  for (int k = 0; k < 8; ++k) tail[tl - 1 - k] = static_cast<uint8_t>(bits >> (8 * k));
  block(h, tail);
  if (tl == 128) block(h, tail + 64);
  char out[65];
  for (int k = 0; k < 8; ++k) std::snprintf(out + 8 * k, 9, "%08x", h[k]);
  return std::string(out, 64);
}

std::string header_line(const std::string& script_sha, const std::string& natives_sha, const std::string& mods_sha,
                        const std::string& types_sha) {
  return "# lomn-osi-trace 1 script=" + script_sha + " natives=" + natives_sha + " mods=" + mods_sha + " types=" + types_sha +
         " fpu=pc24";
}

}  // namespace lomn::osi
