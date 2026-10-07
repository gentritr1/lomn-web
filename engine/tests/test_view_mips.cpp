#include <cstdint>
#include <vector>

#include "doctest.h"
#include "lomn/view/mips.h"

using lomn::view::build_mip_chain;
using lomn::view::downsample_box;
using lomn::view::mip_chain_bytes;
using lomn::view::mip_level_count;
using lomn::view::MipLevel;

namespace {
// RGBA texels whose 4 channels are v, v, v, a (a = 255 unless given).
std::vector<uint8_t> gray(std::initializer_list<int> vs) {
  std::vector<uint8_t> o;
  for (int v : vs) {
    for (int c = 0; c < 3; ++c) o.push_back(static_cast<uint8_t>(v));
    o.push_back(255);
  }
  return o;
}
}  // namespace

TEST_CASE("mips: level count is floor(log2(max)) + 1, 0 for an empty image") {
  CHECK(mip_level_count(1, 1) == 1);
  CHECK(mip_level_count(2, 1) == 2);
  CHECK(mip_level_count(4, 4) == 3);
  CHECK(mip_level_count(5, 3) == 3);
  CHECK(mip_level_count(1, 8) == 4);
  CHECK(mip_level_count(4096, 4096) == 13);
  CHECK(mip_level_count(4096, 1) == 13);
  CHECK(mip_level_count(0, 5) == 0);
  CHECK(mip_chain_bytes(4, 2) == 32 + 8 + 4);
  CHECK(mip_chain_bytes(1, 1) == 4);
}

TEST_CASE("mips: 2x2 -> 1x1 is the exact average, rounded half up") {
  MipLevel d;
  REQUIRE(downsample_box(gray({0, 1, 2, 4}), 2, 2, d));  // 7/4 = 1.75 -> 2
  CHECK(d.width == 1);
  CHECK(d.height == 1);
  CHECK(d.rgba == gray({2}));
  REQUIRE(downsample_box(gray({0, 0, 0, 1}), 2, 2, d));  // 0.25 -> 0
  CHECK(d.rgba[0] == 0);
  REQUIRE(downsample_box(gray({0, 0, 1, 1}), 2, 2, d));  // 0.5 -> 1 (ties up)
  CHECK(d.rgba[0] == 1);
  REQUIRE(downsample_box(gray({255, 255, 255, 254}), 2, 2, d));  // 254.75 -> 255, no overflow
  CHECK(d.rgba[0] == 255);
}

TEST_CASE("mips: odd sizes split the middle texel by area (non-power-of-two safe)") {
  MipLevel d;
  // 3x1 -> 1x1: all three texels weigh 1/3.
  REQUIRE(downsample_box(gray({0, 3, 9}), 3, 1, d));
  CHECK(d.width == 1);
  CHECK(d.rgba[0] == 4);
  REQUIRE(downsample_box(gray({1, 1, 2}), 3, 1, d));  // 4/3 -> 1
  CHECK(d.rgba[0] == 1);
  // 5x1 -> 2x1: texel 2 is split; dst0 = (2*10 + 2*20 + 1*30)/5 = 18, dst1 = (1*30 + 2*40 + 2*50)/5 = 42.
  REQUIRE(downsample_box(gray({10, 20, 30, 40, 50}), 5, 1, d));
  CHECK(d.width == 2);
  CHECK(d.height == 1);
  CHECK(d.rgba == gray({18, 42}));
  // 3x3 -> 1x1: nine equal weights.
  REQUIRE(downsample_box(gray({0, 0, 0, 0, 90, 0, 0, 0, 0}), 3, 3, d));
  CHECK(d.rgba[0] == 10);
}

TEST_CASE("mips: 1xN columns halve only along the long axis") {
  std::vector<MipLevel> chain;
  REQUIRE(build_mip_chain(gray({0, 10, 20, 31}), 1, 4, chain));
  REQUIRE(chain.size() == 3);
  CHECK(chain[1].width == 1);
  CHECK(chain[1].height == 2);
  CHECK(chain[1].rgba == gray({5, 26}));  // (20+31)/2 = 25.5 -> 26
  CHECK(chain[2].width == 1);
  CHECK(chain[2].height == 1);
  CHECK(chain[2].rgba == gray({16}));  // (5+26)/2 = 15.5 -> 16
}

TEST_CASE("mips: full chain sizes and channel independence (straight alpha)") {
  std::vector<uint8_t> px = {
      255, 0, 0, 255, 0, 0, 255, 0, 10, 20, 30, 40, 50, 60, 70, 80,  // row 0
      0, 255, 0, 255, 0, 0, 0, 0, 90, 100, 110, 120, 130, 140, 150, 160,  // row 1
  };
  std::vector<MipLevel> chain;
  REQUIRE(build_mip_chain(px, 4, 2, chain));
  REQUIRE(chain.size() == 3);
  CHECK(chain[0].rgba == px);
  CHECK(chain[1].width == 2);
  CHECK(chain[1].height == 1);
  // Left 2x2: R (255+0+0+0)/4 = 63.75 -> 64, G 63.75 -> 64, B 255/4 -> 64, A (255+0+255+0)/4 = 127.5 -> 128.
  CHECK(std::vector<uint8_t>(chain[1].rgba.begin(), chain[1].rgba.begin() + 4) == std::vector<uint8_t>{64, 64, 64, 128});
  // Right 2x2: (10+50+90+130)/4 = 70, (20+60+100+140)/4 = 80, 90, 100.
  CHECK(std::vector<uint8_t>(chain[1].rgba.begin() + 4, chain[1].rgba.end()) == std::vector<uint8_t>{70, 80, 90, 100});
  CHECK(chain[2].width == 1);
  CHECK(chain[2].rgba == std::vector<uint8_t>{67, 72, 77, 114});  // (64+70)/2, (64+80)/2, (64+90)/2, (128+100)/2
}

TEST_CASE("mips: size mismatch and empty input are rejected without touching the output") {
  MipLevel d;
  d.width = 7;
  CHECK_FALSE(downsample_box(gray({1, 2, 3}), 2, 2, d));
  CHECK(d.width == 7);
  CHECK_FALSE(downsample_box({}, 0, 0, d));
  std::vector<MipLevel> chain(2);
  CHECK_FALSE(build_mip_chain(gray({1, 2, 3}), 2, 2, chain));
  CHECK(chain.empty());
}
