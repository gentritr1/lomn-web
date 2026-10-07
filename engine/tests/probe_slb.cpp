// Task 8B C1: crafted-input TIME and MEMORY probe for textures.slb (lomn/sage/slb.h parse_spec_textures), the M1a-3
// table whose record count and names were uncapped (a 64 MiB entry once cost 1,273 MB on wasm32). A 64 MiB table of
// 2.8M records sharing one 63-character name must be rejected by kMaxSpecTextures within kMaxSeconds and grow the
// process peak by <= kMaxGrowthMB (on top of the input itself); a table at the cap is accepted within the same bounds.
// Runs natively and as wasm32 under Node: ctest slb_probe.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "lomn/sage/slb.h"
#include "probe_util.h"

using namespace lomn;

namespace {
constexpr double kMaxSeconds = 5.0, kMaxGrowthMB = 64.0;

void put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i) b[at + i] = static_cast<uint8_t>(v >> (8 * i));
}

// {n, n, ptr 12}, n 20-byte records (name ptr -> one shared length-prefixed name, trans 0.5), the name, then the fix-up
// slots (header ptr + one per record), their count and 0x00C0FFEE (the layout GetSLKFile reads).
std::vector<uint8_t> textures_table(uint32_t n, size_t name_len) {
  const size_t name_at = 12 + size_t(20) * n, data_end = (name_at + 2 + name_len + 3) & ~size_t(3);
  std::vector<uint8_t> b(data_end + 4 * (size_t(n) + 1) + 8, 0);
  put32(b, 0, n);
  put32(b, 4, n);
  put32(b, 8, 12);
  float half = 0.5f;
  uint32_t hb;
  std::memcpy(&hb, &half, 4);
  for (uint32_t i = 0; i < n; ++i) {
    put32(b, 12 + size_t(20) * i, static_cast<uint32_t>(name_at));
    put32(b, 16 + size_t(20) * i, hb);
  }
  b[name_at] = static_cast<uint8_t>(name_len);
  std::memset(&b[name_at + 1], 'n', name_len);
  size_t at = b.size() - 8;
  for (uint32_t k = 1; k <= n + 1; ++k) put32(b, at - 4 * k, k == n + 1 ? 8u : static_cast<uint32_t>(12 + size_t(20) * (n - k)));
  put32(b, at, n + 1);
  put32(b, at + 4, 0x00C0FFEEu);
  return b;
}

bool probe(const char* name, std::vector<uint8_t> file, const char* want) {  // want == nullptr: must be accepted
  const double m0 = probe_peak_mb();
  const auto t0 = std::chrono::steady_clock::now();
  const size_t bytes = file.size();
  std::string err, got;
  size_t records = 0;
  {
    sage::Slb slb;
    std::vector<sage::SpecTextureRecord> recs;
    if (!slb.parse(std::move(file), &err)) got = "slb: " + err;
    else if (!sage::parse_spec_textures(slb, recs, &err)) got = err;
    records = recs.size();
  }
  const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const double grow = probe_peak_mb() - m0;
  const bool want_ok = want == nullptr ? got.empty() : got.find(want) != std::string::npos;
  const bool ok = want_ok && s <= kMaxSeconds && grow <= kMaxGrowthMB;
  std::printf("%s %-12s %9zu bytes  %.3f s  peak +%.1f MB (%.1f MB)  %zu records -> %s\n", ok ? "ok  " : "FAIL", name, bytes, s,
              grow, probe_peak_mb(), records, got.empty() ? "(accepted)" : got.c_str());
  return ok;
}
}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  bool ok = true;
  ok &= probe("at-cap", textures_table(sage::kMaxSpecTextures, sage::kMaxSpecTextureName), nullptr);
  // 64 MiB: (64 MiB - 1 KiB) / 24 bytes per record (20 + a 4-byte fix-up slot).
  ok &= probe("table-64mib", textures_table(static_cast<uint32_t>(((64u << 20) - 1024) / 24), sage::kMaxSpecTextureName),
              "kMaxSpecTextures");
  std::printf("%s\n", ok ? "slb_probe: all probes bounded" : "slb_probe: FAILED");
  return ok ? 0 : 1;
}
