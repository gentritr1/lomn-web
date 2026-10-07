// M1a-4 Task 3 fix1 M-1: real-data pixel evidence for the texture cache's HIT path (single-area runs never hit).
//   lomn_texture_cache_real <install> [lev:areaA:areaB ...]
// For each pair (default: three same-level pairs that share many textures on Beta 1.5.0), both areas are loaded into
// ONE shared TextureCache (A then B, so B's shared textures are hits) and each is also loaded without a cache (truth).
// Every texture (request, format, alpha_on, every mip's size and RGBA bytes), the texture stats, the warnings and every
// subset's texture index / alpha must match. Exit 0 = 0 mismatches AND hits > 0 in every pair (so the miss path alone
// cannot pass); 1 = a mismatch, no hit, or a load failure; 77 = no install (ctest skip).
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "lomn/fs.h"
#include "lomn/view/area.h"
#include "lomn/view/texture_cache.h"

using namespace lomn;

namespace {

uint64_t compare(const view::AreaData& truth, const view::AreaData& got, const std::string& where) {
  uint64_t bad = 0;
  auto miss = [&](const std::string& what) {
    if (bad++ < 10) std::printf("MISMATCH %s: %s\n", where.c_str(), what.c_str());
  };
  if (truth.textures.size() != got.textures.size()) return miss("texture count"), bad;
  for (size_t i = 0; i < truth.textures.size(); ++i) {
    const view::Texture &t = truth.textures[i], &g = got.textures[i];
    const std::string at = "texture " + std::to_string(i) + " " + t.request;
    if (t.request != g.request || t.format != g.format || t.alpha_on != g.alpha_on) miss(at + " request/format/alpha_on");
    if (t.mips.size() != g.mips.size()) {
      miss(at + " mip count");
      continue;
    }
    for (size_t m = 0; m < t.mips.size(); ++m)
      if (t.mips[m].width != g.mips[m].width || t.mips[m].height != g.mips[m].height || t.mips[m].rgba != g.mips[m].rgba)
        miss(at + " mip " + std::to_string(m));
  }
  const view::AreaStats &a = truth.stats, &b = got.stats;
  if (a.textures != b.textures || a.texture_bytes != b.texture_bytes || a.textures_alpha != b.textures_alpha ||
      a.textures_failed != b.textures_failed || a.textures_missing != b.textures_missing)
    miss("texture stats");
  if (truth.warnings != got.warnings) miss("warnings");
  if (truth.models.size() != got.models.size()) return miss("model count"), bad;
  for (size_t mi = 0; mi < truth.models.size(); ++mi) {
    if (truth.models[mi].subsets.size() != got.models[mi].subsets.size()) {
      miss("subset count");
      continue;
    }
    for (size_t s = 0; s < truth.models[mi].subsets.size(); ++s)
      if (truth.models[mi].subsets[s].texture != got.models[mi].subsets[s].texture ||
          truth.models[mi].subsets[s].alpha != got.models[mi].subsets[s].alpha)
        miss("model " + std::to_string(mi) + " subset " + std::to_string(s) + " texture/alpha");
  }
  return bad;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2 || std::string(argv[1]).empty() || !std::filesystem::is_directory(argv[1])) {
    std::printf("SKIPPED (no LOMN_DATA install)\n");
    return 77;
  }
  std::vector<std::string> pairs;
  for (int i = 2; i < argc; ++i) pairs.push_back(argv[i]);
  // Chosen 2026-10-07 from 15 consecutive same-level candidates (ranked by `lomn_view --survey 1 --texture-cache on`
  // hits), measured pair-locally with this tool: B's hits 22 / 21 / 21 of 28 / 33 / 37 textures, three levels.
  if (pairs.empty()) pairs = {"lev5:le01:le02", "lev6:tvil:vllg", "lev4:brd4:crss"};
  std::error_code ec;
  const std::filesystem::path tmp = std::filesystem::temp_directory_path(ec);
  auto fsp = fs::open_disk(argv[1], ((ec ? std::filesystem::path("/tmp") : tmp) / "lomn_view_user").string());
  if (!fsp) return std::printf("cannot open install %s\n", argv[1]), 1;
  uint64_t total_bad = 0, total_hits = 0, total_textures = 0, total_bytes = 0;
  int rc = 0;
  for (const std::string& p : pairs) {
    const size_t c1 = p.find(':'), c2 = p.find(':', c1 + 1);
    if (c1 == std::string::npos || c2 == std::string::npos) return std::printf("bad pair %s (lev:areaA:areaB)\n", p.c_str()), 1;
    const std::string lev = p.substr(0, c1), areas[2] = {p.substr(c1 + 1, c2 - c1 - 1), p.substr(c2 + 1)};
    view::TextureCache cache;  // one cache per pair: A fills it, B hits it
    uint64_t pair_hits = 0;
    for (const std::string& area : areas) {
      view::AreaData truth, got;
      std::string err;
      view::AreaOptions on;
      on.texture_cache = &cache;
      if (!view::load_area(*fsp, lev, area, view::AreaOptions(), truth, &err) || !view::load_area(*fsp, lev, area, on, got, &err)) {
        std::printf("FAIL %s/%s: load: %s\n", lev.c_str(), area.c_str(), err.c_str());
        return 1;
      }
      const uint64_t bad = compare(truth, got, lev + "/" + area);
      uint64_t bytes = 0;
      for (const view::Texture& t : got.textures)
        for (const view::MipLevel& l : t.mips) bytes += l.rgba.size();
      std::printf("%s/%s: textures %zu, rgba bytes %llu, hits %u, misses %u, mismatches %llu\n", lev.c_str(), area.c_str(), got.textures.size(),
                  static_cast<unsigned long long>(bytes), got.stats.texture_cache_hits, got.stats.texture_cache_misses,
                  static_cast<unsigned long long>(bad));
      // Reconciliation: with no failed texture, every texture of the area was looked up exactly once (hit or miss).
      if (got.stats.textures_failed == 0 && got.stats.texture_cache_hits + got.stats.texture_cache_misses != got.textures.size()) {
        std::printf("FAIL %s/%s: hits + misses != textures\n", lev.c_str(), area.c_str());
        rc = 1;
      }
      pair_hits += got.stats.texture_cache_hits;
      total_bad += bad;
      total_textures += got.textures.size();
      total_bytes += bytes;
    }
    if (pair_hits == 0) {
      std::printf("FAIL %s: no cache hit (the hit path was not exercised)\n", p.c_str());
      rc = 1;
    }
    total_hits += pair_hits;
  }
  std::printf("texture_cache_real: %zu pairs, %llu textures, %llu RGBA bytes compared, %llu hits, %llu mismatches\n", pairs.size(),
              static_cast<unsigned long long>(total_textures), static_cast<unsigned long long>(total_bytes),
              static_cast<unsigned long long>(total_hits), static_cast<unsigned long long>(total_bad));
  return total_bad || rc ? 1 : 0;
}
