#include "doctest.h"
#include "lomn/sage/image.h"
#include "sage_test_util.h"

using namespace lomn::sage;
using namespace sage_test;

namespace {

Bytes dds_header(uint32_t w, uint32_t h, const char* fourcc, uint32_t mips = 0, uint32_t caps2 = 0) {
  Bytes b;
  putstr(b, "DDS ");
  put32(b, 124);
  put32(b, 0x1007 | (mips ? 0x20000u : 0u));
  put32(b, h);
  put32(b, w);
  put32(b, 0);
  put32(b, 0);
  put32(b, mips);
  for (int i = 0; i < 11; ++i) put32(b, 0);
  put32(b, 32);  // ddspf
  if (fourcc) {
    put32(b, 0x4);
    putstr(b, fourcc);
    for (int i = 0; i < 5; ++i) put32(b, 0);
  } else {  // A8R8G8B8
    put32(b, 0x41);
    put32(b, 0);
    put32(b, 32);
    put32(b, 0x00ff0000u);
    put32(b, 0x0000ff00u);
    put32(b, 0x000000ffu);
    put32(b, 0xff000000u);
  }
  put32(b, 0x1000);
  put32(b, caps2);
  put32(b, 0);
  put32(b, 0);
  put32(b, 0);
  REQUIRE(b.size() == 128);
  return b;
}

const uint8_t* px(const Image& img, uint32_t x, uint32_t y) { return &img.rgba[(static_cast<size_t>(y) * img.width + x) * 4]; }

}  // namespace

TEST_CASE("bc1: 4-colour and 3-colour+transparent modes, D3D UNORM expansion and rounding") {
  uint8_t out[64];
  // c0 = pure red 0xF800 > c1 = pure blue 0x001F: 4-colour mode. Indices: texel t uses code t % 4.
  uint8_t blk[8] = {0x00, 0xf8, 0x1f, 0x00, 0xe4, 0xe4, 0xe4, 0xe4};
  decode_bc1_block(blk, out, false);
  CHECK(out[0] == 255);
  CHECK(out[2] == 0);  // code 0 = c0 (red)
  CHECK(out[4 + 2] == 255);  // code 1 = c1 (blue)
  CHECK(out[8] == 170);      // code 2: (2*255 + 0 + 1) / 3
  CHECK(out[8 + 2] == 85);   //          (2*0 + 255 + 1) / 3
  CHECK(out[12] == 85);      // code 3: (255 + 0 + 1) / 3
  CHECK(out[12 + 2] == 170);
  CHECK(out[15] == 255);
  // Swap: c0 <= c1 -> 3-colour mode; code 3 is transparent black, code 2 the rounded midpoint.
  uint8_t blk3[8] = {0x1f, 0x00, 0x00, 0xf8, 0xe4, 0xe4, 0xe4, 0xe4};
  decode_bc1_block(blk3, out, false);
  CHECK(out[8] == 128);  // 255 * 31/62 = 127.5: ties round up
  CHECK(out[8 + 2] == 128);
  CHECK(out[12] == 0);
  CHECK(out[15] == 0);
  // The same bits forced to 4-colour (DXT3/DXT5 colour blocks).
  decode_bc1_block(blk3, out, true);
  CHECK(out[15] == 255);
  CHECK(out[12] == 170);
  // Green 6-bit expansion: g=63 -> 255, g=1 -> 4.
  uint8_t g[8] = {0xe0, 0x07, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00};
  decode_bc1_block(g, out, false);
  CHECK(out[1] == 255);
  // Endpoints are x/31 and x/63 rounded, not bit-replicated: r=3 -> 25 (replication: 24), g=11 -> 45 (44).
  // c0 = (r3,g11,b0) = 0x1960, c1 = 0: r c2 = round(255*6/93) = 16, c3 = round(255*3/93) = 8;
  // g c2 = round(255*22/189) = 30 (replication: (2*44+1)/3 = 29), c3 = round(255*11/189) = 15.
  uint8_t odd[8] = {0x60, 0x19, 0x00, 0x00, 0xe4, 0xe4, 0xe4, 0xe4};
  decode_bc1_block(odd, out, false);
  CHECK(out[0] == 25);
  CHECK(out[1] == 45);
  CHECK(out[8] == 16);
  CHECK(out[9] == 30);
  CHECK(out[12] == 8);
  CHECK(out[13] == 15);
}

TEST_CASE("bc2: explicit 4-bit alpha scaled by 17, nibble order low first") {
  uint8_t blk[16] = {0x10, 0xf2, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 0x00, 0x00, 0, 0, 0, 0};
  uint8_t out[64];
  decode_bc2_block(blk, out);
  CHECK(out[3] == 0);         // texel 0 = low nibble 0
  CHECK(out[7] == 17);        // texel 1 = high nibble 1
  CHECK(out[11] == 34);       // texel 2 = 2
  CHECK(out[15] == 255);      // texel 3 = 15
  CHECK(out[0] == 255);       // colour 0xFFFF > 0x0000, code 0 = white
}

TEST_CASE("bc3: 8-alpha and 6-alpha palettes, 3-bit indices across byte boundaries") {
  uint8_t out[64];
  // a0=255 > a1=0: codes 2..7 = (6*255+3)/7=219, 182, 146, 109, 73, 36. Indices 0..7 for texels 0..7.
  uint8_t blk[16] = {255, 0, 0x88, 0xc6, 0xfa, 0, 0, 0, 0xff, 0xff, 0, 0, 0, 0, 0, 0};
  // bits for texels 0..7 = 0,1,2,3,4,5,6,7 -> 0b111110101100011010001000 = 0xFAC688
  decode_bc3_block(blk, out);
  const uint8_t want[8] = {255, 0, 219, 182, 146, 109, 73, 36};
  for (int t = 0; t < 8; ++t) CHECK(out[4 * t + 3] == want[t]);
  // a0=0 <= a1=255: codes 2..5 = (4*0+1*255+2)/5=51, 102, 153, 204; 6 -> 0, 7 -> 255.
  blk[0] = 0;
  blk[1] = 255;
  decode_bc3_block(blk, out);
  const uint8_t want6[8] = {0, 255, 51, 102, 153, 204, 0, 255};
  for (int t = 0; t < 8; ++t) CHECK(out[4 * t + 3] == want6[t]);
}

TEST_CASE("dds: DXT1/3/5 and A8R8G8B8 decode, partial blocks clipped, alpha_on rule") {
  Image img;
  DdsInfo di;
  std::string err;
  Bytes d1 = dds_header(5, 3, "DXT1");  // 2x1 blocks
  for (int i = 0; i < 2; ++i) {
    uint8_t blk[8] = {0x00, 0xf8, 0x1f, 0x00, 0, 0, 0, 0};  // all code 0 (red) ...
    if (i == 1) blk[4] = 0x01;                               // ... except texel 0 of block 1 (blue)
    d1.insert(d1.end(), blk, blk + 8);
  }
  REQUIRE_MESSAGE(decode_dds(d1.data(), d1.size(), img, &di, &err), err);
  CHECK(di.format == DdsFormat::DXT1);
  CHECK_FALSE(di.alpha_on);
  CHECK(img.width == 5);
  CHECK(img.height == 3);
  CHECK(img.rgba.size() == 5 * 3 * 4);
  CHECK(px(img, 3, 2)[0] == 255);
  CHECK(px(img, 4, 0)[2] == 255);  // texel (0,0) of block 1
  CHECK(px(img, 4, 0)[0] == 0);

  Bytes d3 = dds_header(4, 4, "DXT3");
  d3.resize(d3.size() + 16, 0xff);
  REQUIRE(decode_dds(d3.data(), d3.size(), img, &di, &err));
  CHECK(di.alpha_on);
  Bytes d5 = dds_header(4, 4, "DXT5");
  d5.resize(d5.size() + 16, 0);
  REQUIRE(decode_dds(d5.data(), d5.size(), img, &di, &err));
  CHECK_FALSE(di.alpha_on);  // DXT5: mAlphaOn false in ScDrawableTexture

  Bytes argb = dds_header(2, 1, nullptr);
  for (uint8_t v : {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88}) argb.push_back(v);  // B G R A
  REQUIRE(decode_dds(argb.data(), argb.size(), img, &di, &err));
  CHECK(di.format == DdsFormat::A8R8G8B8);
  CHECK(di.alpha_on);
  CHECK(img.rgba == Bytes{0x33, 0x22, 0x11, 0x44, 0x77, 0x66, 0x55, 0x88});
}

TEST_CASE("dds: rejects cube maps, volumes, bad sizes, mip counts, formats and truncation") {
  Image img;
  std::string err;
  auto rejects = [&](Bytes b) { return !decode_dds(b.data(), b.size(), img, nullptr, &err); };
  auto with_data = [](Bytes b, size_t n) {
    b.resize(b.size() + n, 0);
    return b;
  };
  CHECK(rejects(with_data(dds_header(4, 4, "DXT5", 0, 0x200 | 0xfc00), 16 * 6)));  // cube
  CHECK(rejects(with_data(dds_header(4, 4, "DXT5", 0, 0x200000), 16)));            // volume
  CHECK(rejects(with_data(dds_header(0, 4, "DXT5"), 16)));
  CHECK(rejects(with_data(dds_header(4097, 4, "DXT5"), 1025 * 16)));
  CHECK_FALSE(rejects(with_data(dds_header(4096, 4, "DXT1"), 1024 * 8)));
  CHECK(rejects(with_data(dds_header(256, 256, "DXT5", 10), 64 * 64 * 16)));  // log2(256)+1 = 9
  CHECK_FALSE(rejects(with_data(dds_header(256, 256, "DXT5", 9), 64 * 64 * 16)));
  CHECK(rejects(with_data(dds_header(4, 4, "DXT2"), 16)));
  CHECK(rejects(with_data(dds_header(8, 8, "DXT5"), 4 * 16 - 1)));  // one byte short
  Bytes good = with_data(dds_header(8, 8, "DXT1"), 4 * 8);
  for (size_t n = 0; n < good.size(); ++n) CHECK(rejects(Bytes(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(n))));
  CHECK(rejects(Bytes{}));  // the 6 zero-length .dds entries of the Beta data
}

TEST_CASE("tga: uncompressed 24/32 bpp, origin handling, RLE packets and their bounds") {
  Image img;
  TgaInfo ti;
  std::string err;
  auto header = [](uint8_t type, uint16_t w, uint16_t h, uint8_t bpp, uint8_t desc) {
    Bytes b = {3, 0, type, 0, 0, 0, 0, 0, 0, 0, 0, 0};  // id length 3
    put16(b, w);
    put16(b, h);
    put8(b, bpp);
    put8(b, desc);
    putstr(b, "abc");  // image id
    return b;
  };
  // 2x2 24 bpp bottom-up: file rows are bottom first.
  Bytes t = header(2, 2, 2, 24, 0);
  for (uint8_t v : {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}) t.push_back(v);
  REQUIRE_MESSAGE(decode_tga(t.data(), t.size(), img, &ti, &err), err);
  CHECK(img.rgba == Bytes{9, 8, 7, 255, 12, 11, 10, 255, 3, 2, 1, 255, 6, 5, 4, 255});
  CHECK_FALSE(ti.top_down);
  // 2x1 32 bpp top-down with declared alpha.
  Bytes u = header(2, 2, 1, 32, 0x28);
  for (uint8_t v : {1, 2, 3, 4, 5, 6, 7, 8}) u.push_back(v);
  REQUIRE(decode_tga(u.data(), u.size(), img, &ti, &err));
  CHECK(img.rgba == Bytes{3, 2, 1, 4, 7, 6, 5, 8});
  CHECK(ti.alpha_bits == 8);
  // RLE 4x1 32 bpp: run packet of 3, then a raw packet of 1.
  Bytes r = header(10, 4, 1, 32, 0x28);
  for (uint8_t v : {0x82, 10, 20, 30, 40, 0x00, 1, 2, 3, 4}) r.push_back(v);
  REQUIRE(decode_tga(r.data(), r.size(), img, &ti, &err));
  CHECK(img.rgba == Bytes{30, 20, 10, 40, 30, 20, 10, 40, 30, 20, 10, 40, 3, 2, 1, 4});
  // A packet that runs past the image, a truncated packet, and every truncation of a valid file fail.
  Bytes over = header(10, 2, 1, 32, 0x28);
  for (uint8_t v : {0x82, 10, 20, 30, 40}) over.push_back(v);
  CHECK_FALSE(decode_tga(over.data(), over.size(), img, &ti, &err));
  CHECK(err.find("past the image") != std::string::npos);
  // Every truncation is an exact-size copy (M1a-4 final F1): a prefix view into the longer vector hides a read of the byte
  // after the end (e.g. a deleted `pos >= size` in the RLE loop) from AddressSanitizer, which `unit_asan` now runs here.
  for (size_t n = 0; n < r.size(); ++n) {
    Bytes cut(r.begin(), r.begin() + static_cast<std::ptrdiff_t>(n));
    CHECK_FALSE(decode_tga(cut.data(), cut.size(), img, &ti, &err));
  }
  for (size_t n = 0; n < t.size(); ++n) {
    Bytes cut(t.begin(), t.begin() + static_cast<std::ptrdiff_t>(n));
    CHECK_FALSE(decode_tga(cut.data(), cut.size(), img, &ti, &err));
  }
  // Unsupported kinds.
  Bytes cm = header(1, 2, 2, 8, 0);
  cm.resize(cm.size() + 64, 0);
  CHECK_FALSE(decode_tga(cm.data(), cm.size(), img, &ti, &err));
  Bytes b16 = header(2, 2, 2, 16, 0);
  b16.resize(b16.size() + 8, 0);
  CHECK_FALSE(decode_tga(b16.data(), b16.size(), img, &ti, &err));
  Bytes zero = header(2, 0, 2, 24, 0);
  CHECK_FALSE(decode_tga(zero.data(), zero.size(), img, &ti, &err));
}

TEST_CASE("tga: a header promising more pixels than the input can hold is rejected before the pixel buffer exists (M1a-4 fuzz_formats finding)") {
  // 4096x4096x32 RLE needs at least ceil(16M / 128) packets of 1 + 4 bytes; 10 bytes of packets cannot be enough. The
  // decoder used to allocate the 64 MiB pixel buffer first and fail inside the packet loop.
  Bytes big = {0, 0, 10, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  put16(big, 4096);
  put16(big, 4096);
  put8(big, 32);
  put8(big, 0x28);
  for (uint8_t x : {0xff, 1, 2, 3, 4, 0x00, 5, 6, 7, 8}) big.push_back(x);
  Image img;
  TgaInfo ti;
  std::string err;
  CHECK_FALSE(decode_tga(big.data(), big.size(), img, &ti, &err));
  CHECK(err.find("too short for the image") != std::string::npos);
  CHECK(img.rgba.empty());
  // Uncompressed: same rule (the data length is checked before the allocation).
  big[2] = 2;
  CHECK_FALSE(decode_tga(big.data(), big.size(), img, &ti, &err));
  CHECK(err.find("pixel data truncated") != std::string::npos);
  // The exact lower bound still reaches the packet loop: 2x1 RLE with one packet of 1 + 4 bytes is accepted.
  Bytes ok = {0, 0, 10, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  put16(ok, 2);
  put16(ok, 1);
  put8(ok, 32);
  put8(ok, 0x28);
  for (uint8_t x : {0x81, 1, 2, 3, 4}) ok.push_back(x);
  CHECK(decode_tga(ok.data(), ok.size(), img, &ti, &err));
}
