#include "lomn/view/mips.h"

namespace lomn::view {
namespace {

struct Tap {
  uint32_t src;     // source index
  uint64_t weight;  // overlap in units of 1/dst_size source texels
};

// Per destination texel of one axis: the source texels it covers and their overlap weights (sum == src_size).
// Destination texel x covers [x*S, (x+1)*S) and source texel i covers [i*D, (i+1)*D) in the common unit 1/(S*D).
std::vector<std::vector<Tap>> axis_taps(uint32_t s, uint32_t d) {
  std::vector<std::vector<Tap>> taps(d);
  for (uint32_t x = 0; x < d; ++x) {
    const uint64_t lo = uint64_t(x) * s, hi = uint64_t(x + 1) * s;
    for (uint64_t i = lo / d; i < s && i * d < hi; ++i) {
      const uint64_t a = i * d > lo ? i * d : lo, b = (i + 1) * d < hi ? (i + 1) * d : hi;
      if (b > a) taps[x].push_back({static_cast<uint32_t>(i), b - a});
    }
  }
  return taps;
}

}  // namespace

uint32_t mip_level_count(uint32_t width, uint32_t height) {
  if (width == 0 || height == 0) return 0;
  uint32_t m = width > height ? width : height, n = 1;
  while (m > 1) {
    m >>= 1;
    ++n;
  }
  return n;
}

uint64_t mip_chain_bytes(uint32_t width, uint32_t height) {
  uint64_t total = 0;
  const uint32_t n = mip_level_count(width, height);
  for (uint32_t i = 0; i < n; ++i) {
    const uint64_t w = (width >> i) ? (width >> i) : 1, h = (height >> i) ? (height >> i) : 1;
    total += w * h * 4;
  }
  return total;
}

bool downsample_box(const std::vector<uint8_t>& src, uint32_t width, uint32_t height, MipLevel& dst) {
  if (width == 0 || height == 0 || src.size() != uint64_t(width) * height * 4) return false;
  const uint32_t dw = width > 1 ? width / 2 : 1, dh = height > 1 ? height / 2 : 1;
  const auto tx = axis_taps(width, dw), ty = axis_taps(height, dh);
  const uint64_t total = uint64_t(width) * height;  // sum of all weights of one destination texel
  MipLevel out;
  out.width = dw;
  out.height = dh;
  out.rgba.resize(size_t(dw) * dh * 4);
  for (uint32_t y = 0; y < dh; ++y) {
    for (uint32_t x = 0; x < dw; ++x) {
      uint64_t acc[4] = {0, 0, 0, 0};
      for (const Tap& vy : ty[y]) {
        const uint8_t* row = &src[(size_t(vy.src) * width) * 4];
        for (const Tap& vx : tx[x]) {
          const uint64_t w = vx.weight * vy.weight;
          const uint8_t* p = row + size_t(vx.src) * 4;
          for (int c = 0; c < 4; ++c) acc[c] += w * p[c];
        }
      }
      uint8_t* o = &out.rgba[(size_t(y) * dw + x) * 4];
      for (int c = 0; c < 4; ++c) o[c] = static_cast<uint8_t>((acc[c] + total / 2) / total);
    }
  }
  dst = std::move(out);
  return true;
}

bool build_mip_chain(std::vector<uint8_t> rgba, uint32_t width, uint32_t height, std::vector<MipLevel>& out) {
  out.clear();
  if (width == 0 || height == 0 || rgba.size() != uint64_t(width) * height * 4) return false;
  const uint32_t n = mip_level_count(width, height);
  out.reserve(n);
  out.push_back({width, height, std::move(rgba)});
  for (uint32_t i = 1; i < n; ++i) {
    MipLevel next;
    const MipLevel& prev = out.back();
    if (!downsample_box(prev.rgba, prev.width, prev.height, next)) {
      out.clear();
      return false;
    }
    out.push_back(std::move(next));
  }
  return true;
}

}  // namespace lomn::view
