#include "lomn/sage/image.h"

#include <cstring>

#include "bytes.h"

namespace lomn::sage {
using detail::fail;
using detail::in_range;
using detail::le16;
using detail::le32;

const char* dds_format_name(DdsFormat f) {
  switch (f) {
    case DdsFormat::DXT1: return "DXT1";
    case DdsFormat::DXT3: return "DXT3";
    case DdsFormat::DXT5: return "DXT5";
    default: return "A8R8G8B8";
  }
}

// Round-to-nearest (ties up) of 255 * num / den, for the exact UNORM arithmetic described in image.h.
static uint8_t unorm8(uint32_t num, uint32_t den) { return static_cast<uint8_t>((510u * num + den) / (2u * den)); }

void decode_bc1_block(const uint8_t* block, uint8_t* out, bool four_colour_only) {
  uint16_t c0 = le16(block), c1 = le16(block + 2);
  const uint32_t a[3] = {(c0 >> 11) & 31u, (c0 >> 5) & 63u, c0 & 31u};
  const uint32_t b[3] = {(c1 >> 11) & 31u, (c1 >> 5) & 63u, c1 & 31u};
  const uint32_t maxv[3] = {31, 63, 31};
  uint8_t pal[4][4];
  for (int k = 0; k < 3; ++k) {
    pal[0][k] = unorm8(a[k], maxv[k]);
    pal[1][k] = unorm8(b[k], maxv[k]);
  }
  pal[0][3] = pal[1][3] = 255;
  if (four_colour_only || c0 > c1) {
    for (int k = 0; k < 3; ++k) {
      pal[2][k] = unorm8(2 * a[k] + b[k], 3 * maxv[k]);
      pal[3][k] = unorm8(a[k] + 2 * b[k], 3 * maxv[k]);
    }
    pal[2][3] = pal[3][3] = 255;
  } else {
    for (int k = 0; k < 3; ++k) pal[2][k] = unorm8(a[k] + b[k], 2 * maxv[k]);
    pal[2][3] = 255;
    pal[3][0] = pal[3][1] = pal[3][2] = pal[3][3] = 0;
  }
  uint32_t bits = le32(block + 4);
  for (int t = 0; t < 16; ++t) std::memcpy(out + 4 * t, pal[(bits >> (2 * t)) & 3], 4);
}

void decode_bc2_block(const uint8_t* block, uint8_t* out) {
  decode_bc1_block(block + 8, out, true);
  for (int t = 0; t < 16; ++t) {
    uint8_t nib = static_cast<uint8_t>((block[t / 2] >> (4 * (t & 1))) & 15);
    out[4 * t + 3] = static_cast<uint8_t>(nib * 17);
  }
}

void decode_bc3_block(const uint8_t* block, uint8_t* out) {
  decode_bc1_block(block + 8, out, true);
  uint32_t a0 = block[0], a1 = block[1];
  uint8_t pal[8];
  pal[0] = static_cast<uint8_t>(a0);
  pal[1] = static_cast<uint8_t>(a1);
  if (a0 > a1) {
    for (uint32_t i = 1; i <= 6; ++i) pal[1 + i] = static_cast<uint8_t>((2 * ((7 - i) * a0 + i * a1) + 7) / 14);
  } else {
    for (uint32_t i = 1; i <= 4; ++i) pal[1 + i] = static_cast<uint8_t>((2 * ((5 - i) * a0 + i * a1) + 5) / 10);
    pal[6] = 0;
    pal[7] = 255;
  }
  uint64_t bits = 0;
  for (int i = 0; i < 6; ++i) bits |= static_cast<uint64_t>(block[2 + i]) << (8 * i);
  for (int t = 0; t < 16; ++t) out[4 * t + 3] = pal[(bits >> (3 * t)) & 7];
}

bool decode_dds(const uint8_t* d, size_t size, Image& out, DdsInfo* info, std::string* err) {
  out = Image();
  if (size < 128) return fail(err, "dds: shorter than the 128-byte header");
  if (std::memcmp(d, "DDS ", 4) != 0) return fail(err, "dds: bad magic");
  uint32_t flags = le32(d + 8), h = le32(d + 12), w = le32(d + 16), depth = le32(d + 24), mips = le32(d + 28);
  uint32_t pf_flags = le32(d + 80), fourcc = le32(d + 84), bitcount = le32(d + 88);
  uint32_t rmask = le32(d + 92), gmask = le32(d + 96), bmask = le32(d + 100), amask = le32(d + 104);
  uint32_t caps2 = le32(d + 112);
  if (caps2 & 0x200u) return fail(err, "dds: cube maps are not supported");
  if ((caps2 & 0x200000u) || ((flags & 0x800000u) && depth > 1)) return fail(err, "dds: volume textures are not supported");
  if (w == 0 || h == 0 || w > kMaxTextureDim || h > kMaxTextureDim) return fail(err, "dds: dimensions outside 1..4096");
  uint32_t max_mips = 1;
  for (uint32_t m = w > h ? w : h; m > 1; m >>= 1) ++max_mips;
  if (mips > max_mips) return fail(err, "dds: mip count above log2(size)+1");
  DdsInfo di;
  di.width = w;
  di.height = h;
  di.mip_count = mips;
  uint64_t need;
  uint64_t bw = (w + 3) / 4, bh = (h + 3) / 4;
  if (pf_flags & 0x4u) {
    if (fourcc == 0x31545844u) di.format = DdsFormat::DXT1;       // "DXT1"
    else if (fourcc == 0x33545844u) di.format = DdsFormat::DXT3;  // "DXT3"
    else if (fourcc == 0x35545844u) di.format = DdsFormat::DXT5;  // "DXT5"
    else return fail(err, "dds: unsupported FourCC");
    need = bw * bh * (di.format == DdsFormat::DXT1 ? 8 : 16);
  } else if ((pf_flags & 0x40u) && bitcount == 32 && rmask == 0x00ff0000u && gmask == 0x0000ff00u && bmask == 0x000000ffu &&
             amask == 0xff000000u && (pf_flags & 0x1u)) {
    di.format = DdsFormat::A8R8G8B8;
    need = static_cast<uint64_t>(w) * h * 4;
  } else {
    return fail(err, "dds: unsupported pixel format");
  }
  di.alpha_on = di.format == DdsFormat::DXT3 || di.format == DdsFormat::A8R8G8B8;
  if (!in_range(size, 128, need)) return fail(err, "dds: pixel data truncated");
  const uint8_t* px = d + 128;
  out.width = w;
  out.height = h;
  out.rgba.resize(static_cast<size_t>(w) * h * 4);
  if (di.format == DdsFormat::A8R8G8B8) {
    for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i) {
      out.rgba[i * 4 + 0] = px[i * 4 + 2];
      out.rgba[i * 4 + 1] = px[i * 4 + 1];
      out.rgba[i * 4 + 2] = px[i * 4 + 0];
      out.rgba[i * 4 + 3] = px[i * 4 + 3];
    }
  } else {
    size_t bsize = di.format == DdsFormat::DXT1 ? 8 : 16;
    uint8_t texels[64];
    for (uint64_t by = 0; by < bh; ++by)
      for (uint64_t bx = 0; bx < bw; ++bx) {
        const uint8_t* b = px + (by * bw + bx) * bsize;
        if (di.format == DdsFormat::DXT1) decode_bc1_block(b, texels, false);
        else if (di.format == DdsFormat::DXT3) decode_bc2_block(b, texels);
        else decode_bc3_block(b, texels);
        for (uint32_t ty = 0; ty < 4; ++ty)
          for (uint32_t tx = 0; tx < 4; ++tx) {
            uint64_t x = bx * 4 + tx, y = by * 4 + ty;
            if (x < w && y < h) std::memcpy(&out.rgba[(y * w + x) * 4], texels + 4 * (ty * 4 + tx), 4);
          }
      }
  }
  if (info) *info = di;
  return true;
}

bool decode_tga(const uint8_t* d, size_t size, Image& out, TgaInfo* info, std::string* err) {
  out = Image();
  if (size < 18) return fail(err, "tga: shorter than the 18-byte header");
  uint8_t id_len = d[0], cm_type = d[1], type = d[2];
  uint16_t cm_len = le16(d + 5);
  uint8_t cm_depth = d[7];
  uint16_t w = le16(d + 12), h = le16(d + 14);
  uint8_t bpp = d[16], desc = d[17];
  if (type != 2 && type != 10) return fail(err, "tga: only true-colour types 2 and 10 are supported");
  if (bpp != 24 && bpp != 32) return fail(err, "tga: only 24/32 bpp are supported");
  if (cm_type > 1) return fail(err, "tga: bad colour-map type");
  if (w == 0 || h == 0 || w > kMaxTextureDim || h > kMaxTextureDim) return fail(err, "tga: dimensions outside 1..4096");
  uint64_t pos = 18 + static_cast<uint64_t>(id_len);
  if (cm_type == 1) pos += static_cast<uint64_t>(cm_len) * ((cm_depth + 7u) / 8u);
  if (pos > size) return fail(err, "tga: header fields run past the file");
  size_t bytes_pp = bpp / 8;
  size_t npix = static_cast<size_t>(w) * h;
  // M1a-4 (fuzz_formats finding): the input must be able to hold the pixels BEFORE the w*h*bpp buffer is allocated (a
  // 4096x4096 header over a few bytes used to allocate 64 MiB and then fail). Uncompressed: the exact size. RLE: every
  // packet is a header byte + at least one pixel and covers at most 128 pixels, so any stream that can complete has
  // at least ceil(npix / 128) * (1 + bpp) bytes; shorter ones would only fail later, in the packet loop.
  if (type == 2 && !in_range(size, pos, static_cast<uint64_t>(npix) * bytes_pp)) return fail(err, "tga: pixel data truncated");
  if (type == 10 && size - pos < (static_cast<uint64_t>(npix) + 127) / 128 * (1 + bytes_pp))
    return fail(err, "tga: RLE data too short for the image");
  std::vector<uint8_t> raw(npix * bytes_pp);  // file pixel order
  if (type == 2) {
    std::memcpy(raw.data(), d + pos, raw.size());
  } else {
    size_t pix = 0;
    while (pix < npix) {
      if (pos >= size) return fail(err, "tga: RLE data truncated");
      uint8_t hdr = d[pos++];
      size_t run = (hdr & 0x7f) + 1u;
      if (run > npix - pix) return fail(err, "tga: RLE packet runs past the image");
      if (hdr & 0x80) {
        if (!in_range(size, pos, bytes_pp)) return fail(err, "tga: RLE data truncated");
        for (size_t i = 0; i < run; ++i) std::memcpy(&raw[(pix + i) * bytes_pp], d + pos, bytes_pp);
        pos += bytes_pp;
      } else {
        if (!in_range(size, pos, run * bytes_pp)) return fail(err, "tga: RLE data truncated");
        std::memcpy(&raw[pix * bytes_pp], d + pos, run * bytes_pp);
        pos += run * bytes_pp;
      }
      pix += run;
    }
  }
  bool top = (desc & 0x20) != 0, right_to_left = (desc & 0x10) != 0;
  out.width = w;
  out.height = h;
  out.rgba.resize(npix * 4);
  for (size_t y = 0; y < h; ++y)
    for (size_t x = 0; x < w; ++x) {
      size_t sy = top ? y : h - 1 - y, sx = right_to_left ? w - 1 - x : x;
      const uint8_t* s = &raw[(sy * w + sx) * bytes_pp];
      uint8_t* o = &out.rgba[(y * w + x) * 4];
      o[0] = s[2];
      o[1] = s[1];
      o[2] = s[0];
      o[3] = bytes_pp == 4 ? s[3] : 255;
    }
  if (info) {
    info->width = w;
    info->height = h;
    info->image_type = type;
    info->bits = bpp;
    info->alpha_bits = desc & 0x0f;
    info->top_down = top;
  }
  return true;
}

}  // namespace lomn::sage
