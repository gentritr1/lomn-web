// Texture decoders to RGBA8 (top mip only; mips are generated later on the CPU, spec §5). The game hands the
// same bytes to D3DXCreateTextureFromFileInMemory from ScDrawableTexture::Load (0x005561c0).
//
// BC1-3 decode (DXT1/3/5) follows the Direct3D definition: an n-bit UNORM endpoint is the value x/(2^n-1), the
// palette entries are exact interpolations of those values (c2 = (2*c0 + c1)/3, c3 = (c0 + 2*c1)/3, or in DXT1's
// 3-colour mode -- c0 <= c1 as u16 -- c2 = (c0 + c1)/2 and c3 = transparent black), and each result is rounded
// to the nearest 8-bit value, ties up. Computed in integers straight from the 5/6-bit codes: no intermediate
// 8-bit endpoint. DXT3/DXT5 colour blocks always use 4-colour mode. DXT3 alpha a4 -> a4 * 17. DXT5 alpha:
// a0 > a1 -> 6 interpolated round(((7-i)*a0 + i*a1) / 7), i=1..6; else 4 interpolated round(((5-i)*a0 + i*a1) / 5),
// i=1..4, then 0 and 255. NOTE: decoders that widen 565 by bit replication (r<<3|r>>2) and interpolate in 8 bits
// differ by 1 LSB on some codes (e.g. r=3 -> 24 vs 25 here); D3D8-era GPUs varied the same way.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace lomn::sage {

constexpr uint32_t kMaxTextureDim = 4096;

struct Image {
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> rgba;  // width*height*4, rows top to bottom
};

enum class DdsFormat { DXT1, DXT3, DXT5, A8R8G8B8 };
const char* dds_format_name(DdsFormat f);

struct DdsInfo {
  DdsFormat format = DdsFormat::DXT1;
  uint32_t width = 0, height = 0;
  uint32_t mip_count = 0;  // as stored (the Beta files store 0: D3DX then builds the chain)
  // ScDrawableTexture mAlphaOn: true for DXT3 and A8R8G8B8, false for DXT5 (and DXT1).
  bool alpha_on = false;
};

// DDS: "DDS " + 124-byte header. Rejects cube maps and volumes, dims of 0 or > 4096, mip counts above
// log2(max(w,h)) + 1, and any pixel format other than DXT1/3/5 and 32-bit A8R8G8B8 masks.
bool decode_dds(const uint8_t* data, size_t size, Image& out, DdsInfo* info, std::string* err);

struct TgaInfo {
  uint32_t width = 0, height = 0;
  uint8_t image_type = 0;   // 2 uncompressed true-colour, 10 RLE true-colour
  uint8_t bits = 0;         // 24 or 32
  uint8_t alpha_bits = 0;   // descriptor bits 0..3; 0 with 32 bpp means the 4th byte is not declared alpha
  bool top_down = false;    // descriptor bit 5
};
// TGA types 2 and 10, 24/32 bpp. RLE packets may not run past the image; the output alpha byte is the file's
// 4th byte for 32 bpp (255 for 24 bpp), and TgaInfo.alpha_bits says whether the file declares it as alpha.
bool decode_tga(const uint8_t* data, size_t size, Image& out, TgaInfo* info, std::string* err);

// Single-block decoders (16 texels, 64 bytes RGBA out, row-major 4x4). Exposed for tests.
void decode_bc1_block(const uint8_t* block, uint8_t* out, bool four_colour_only);
void decode_bc2_block(const uint8_t* block, uint8_t* out);
void decode_bc3_block(const uint8_t* block, uint8_t* out);

}  // namespace lomn::sage
