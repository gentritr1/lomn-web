// CPU box-filter mip chains for RGBA8 textures (spec M1 §5: RGBA8 upload + CPU mips; the game's DDS files store mip
// count 0 and D3DX built the chain at load, with its default BOX mip filter).
//
// Level n+1 has size max(1, w/2) x max(1, h/2) (floor), down to 1x1: floor(log2(max(w, h))) + 1 levels, the same
// chain length D3D/WebGPU use. Each destination texel is the exact area-weighted average of the source texels its
// footprint covers: per axis, destination texel x covers source interval [x*W/w, (x+1)*W/w), and a source texel's
// weight is its overlap with that interval. For an even size this is the classic 2-tap average; for an odd size
// (e.g. 5 -> 2) the middle texel is split between both destination texels, so no source texel is dropped
// (non-power-of-two safe); a 1-texel axis stays 1 and is copied. The result is round-half-up of the weighted sum
// divided by W*H in exact integer (u64) arithmetic, per channel, on straight (non-premultiplied) alpha like D3DX.
#pragma once
#include <cstdint>
#include <vector>

namespace lomn::view {

struct MipLevel {
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> rgba;  // width*height*4
};

// floor(log2(max(w, h))) + 1 (0 for an empty image).
uint32_t mip_level_count(uint32_t width, uint32_t height);

// One box-filter step. False (and dst untouched) if the sizes do not match rgba.size() or either side is 0.
bool downsample_box(const std::vector<uint8_t>& src, uint32_t width, uint32_t height, MipLevel& dst);

// The full chain; level 0 takes ownership of `rgba`. False if the size does not match or is 0.
bool build_mip_chain(std::vector<uint8_t> rgba, uint32_t width, uint32_t height, std::vector<MipLevel>& out);

// Bytes of the whole chain for a width x height RGBA8 texture.
uint64_t mip_chain_bytes(uint32_t width, uint32_t height);

}  // namespace lomn::view
