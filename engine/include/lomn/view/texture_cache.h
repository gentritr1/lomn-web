// M1a-4 Task 3: a process-lifetime CPU cache of decoded textures (RGBA8 + the CPU box-filter mip chain) shared across
// area loads. Opt-in: load_area uses it only when AreaOptions::texture_cache points at one (lomn_view
// --texture-cache on); with the default nullptr no TextureCache exists and the loader runs exactly as before.
//
// Key: (canonical texture path, SHA-256 of the entry's bytes). The path is the lower-cased resolved request
// ("root/data/levels/lev1/textures/x.dds"); it selects the decoder (.dds / .tga). The digest makes "same path,
// different bytes" (another install, a patched blockfile, a test remount) a miss, never a stale hit.
// Value: shared_ptr<const DecodedTexture>. The cache owns one reference per resident entry; eviction drops only that
// reference, so a DecodedTexture a caller still holds stays valid (the cache never hands out pointers into data it can
// free). load_area copies the mips into AreaData (release_cpu_copies frees AreaData's copy, never the cache's).
// Capacity: bytes of resident mip chains (RGBA8 incl. mips), 256 MiB by default, least-recently-used eviction (a
// lookup hit or an insert makes an entry most recent). An entry larger than the whole cap is not stored.
// Single-threaded (as the loader); deterministic (no clocks, no pointer-keyed or unordered containers).
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "lomn/view/mips.h"

namespace lomn::view {

using Sha256Digest = std::array<uint8_t, 32>;
// FIPS 180-4 SHA-256 (self-contained; no dependency).
Sha256Digest sha256(const uint8_t* data, size_t size);
std::string sha256_hex(const Sha256Digest& d);

struct DecodedTexture {
  std::string format;  // as Texture::format
  bool alpha_on = false;
  std::vector<MipLevel> mips;
  uint64_t bytes() const;  // sum of the mip levels' RGBA8 sizes
};

struct TextureCacheStats {
  uint64_t hits = 0, misses = 0;  // lookups
  uint64_t bytes = 0;             // resident mip-chain bytes
  uint64_t evictions = 0;         // entries dropped to stay within the cap
  uint64_t entries = 0;           // resident entries
  uint64_t inserts = 0;           // entries stored (an over-cap entry is not stored and not counted)
};

class TextureCache {
 public:
  static constexpr uint64_t kDefaultCapBytes = 256ull << 20;
  // `cap_bytes` is injectable for tests (any value; 0 stores nothing).
  explicit TextureCache(uint64_t cap_bytes = kDefaultCapBytes);
  TextureCache(const TextureCache&) = delete;
  TextureCache& operator=(const TextureCache&) = delete;

  // Hit: the entry (now most recent) and ++hits. Miss: nullptr and ++misses.
  std::shared_ptr<const DecodedTexture> find(const std::string& path, const Sha256Digest& digest);
  // Stores (replacing an entry with the same key), then evicts least-recently-used entries other than the new one
  // until bytes <= cap. An entry with bytes() > cap is not stored. A null value or one without mips is ignored.
  void insert(const std::string& path, const Sha256Digest& digest, std::shared_ptr<const DecodedTexture> value);

  const TextureCacheStats& stats() const { return stats_; }
  uint64_t cap_bytes() const { return cap_; }

  // Test hook: TextureCache objects constructed in this process ("flag off never constructs the cache").
  static uint64_t instances_constructed();

 private:
  using Key = std::pair<std::string, Sha256Digest>;
  struct Entry {
    std::shared_ptr<const DecodedTexture> value;
    uint64_t bytes = 0;
    std::list<Key>::iterator lru;  // position in lru_ (front = most recent)
  };
  void evict_to_cap(const Key* keep);
  uint64_t cap_;
  std::list<Key> lru_;
  std::map<Key, Entry> entries_;
  TextureCacheStats stats_;
};

}  // namespace lomn::view
