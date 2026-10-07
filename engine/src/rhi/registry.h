// Internal (M1a-2): the object table and validation rules shared by the null and WebGPU backends, so both accept and
// reject exactly the same calls. Ids are allocated per kind (textures = render targets + sampled textures, buffers,
// pipelines, samplers, bind groups), start at 1 and are never reused. Every check returns "" when the call is valid,
// else a short reason. All size arithmetic is u64 (wasm32 size_t is 32-bit).
#pragma once
#include <cstdint>
#include <map>
#include <string>

#include "lomn/rhi.h"

namespace lomn::rhi::detail {

constexpr uint64_t kMaxBufferBytes = 256ull << 20;  // WebGPU default maxBufferSize

enum class BufKind { Uniform, Vertex, Index };
struct BufInfo {
  BufKind kind = BufKind::Uniform;
  uint32_t bytes = 0;
};
enum class TexKind { RenderTarget, Sampled };
struct TexInfo {
  TexKind kind = TexKind::RenderTarget;
  uint32_t width = 0, height = 0, mips = 1;
  bool depth = false;
};
// Populated group-0 slots (bit mask). Layout compatibility = same uniform stride and same slot mask; later milestones
// add bits for new optional slots (e.g. a per-frame uniform block, a second texture).
constexpr uint32_t kSlotUniforms = 1u, kSlotTexture = 2u, kSlotSampler = 4u;
constexpr uint32_t kMeshSlots = kSlotUniforms | kSlotTexture | kSlotSampler;
struct PipeInfo {
  bool mesh = false;
  uint32_t uniform_bytes = 0;
  uint32_t slots = 0;  // mesh pipelines: kMeshSlots
};
struct BindInfo {
  BindGroupDesc desc;
  uint32_t stride = 0;  // uniform_stride(desc.uniform_bytes)
  uint32_t slots = 0;
};
inline uint32_t bind_slots(const BindGroupDesc& d) {
  return (d.uniforms ? kSlotUniforms : 0u) | (d.texture ? kSlotTexture : 0u) | (d.sampler ? kSlotSampler : 0u);
}

// floor(log2(max(w, h))) + 1: the full chain down to 1x1.
inline uint32_t full_mip_count(uint32_t w, uint32_t h) {
  uint32_t m = w > h ? w : h, n = 1;
  while (m > 1) {
    m >>= 1;
    ++n;
  }
  return n;
}
inline uint32_t mip_dim(uint32_t d, uint32_t mip) {
  uint32_t v = mip >= 32 ? 0 : d >> mip;
  return v ? v : 1;
}
// The M0 content checksum (sum = sum * 31 + byte), reused for every M1a-2 data upload.
inline uint32_t checksum(const void* data, uint64_t size) {
  uint32_t sum = 0;
  const uint8_t* p = static_cast<const uint8_t*>(data);
  for (uint64_t i = 0; i < size; ++i) sum = sum * 31u + p[i];
  return sum;
}
inline uint32_t vertex_format_bytes(VertexFormat f) {
  switch (f) {
    case VertexFormat::Float32x2: return 8;
    case VertexFormat::Float32x3: return 12;
    default: return 16;
  }
}
inline const char* vertex_format_name(VertexFormat f) {
  switch (f) {
    case VertexFormat::Float32x2: return "f32x2";
    case VertexFormat::Float32x3: return "f32x3";
    default: return "f32x4";
  }
}
inline const char* blend_name(BlendMode b) { return b == BlendMode::AlphaBlend ? "alpha" : "opaque"; }
inline const char* compare_name(CompareFunc c) {
  static const char* n[] = {"never", "less", "equal", "lessequal", "greater", "notequal", "greaterequal", "always"};
  return static_cast<unsigned>(c) < 8 ? n[static_cast<unsigned>(c)] : "?";
}
inline const char* cull_name(CullMode c) { return c == CullMode::None ? "none" : c == CullMode::Front ? "front" : "back"; }
inline const char* filter_name(FilterMode f) { return f == FilterMode::Linear ? "linear" : "nearest"; }
inline const char* address_name(AddressMode a) { return a == AddressMode::Wrap ? "wrap" : a == AddressMode::Clamp ? "clamp" : "mirror"; }

class Registry {
 public:
  std::map<TextureId, TexInfo> textures;
  std::map<BufferId, BufInfo> buffers;
  std::map<PipelineId, PipeInfo> pipelines;
  std::map<SamplerId, SamplerDesc> samplers;
  std::map<BindGroupId, BindInfo> bind_groups;

  TextureId add_texture(const TexInfo& t) { textures[++next_tex_] = t; return next_tex_; }
  BufferId add_buffer(const BufInfo& b) { buffers[++next_buf_] = b; return next_buf_; }
  PipelineId add_pipeline(const PipeInfo& p) { pipelines[++next_pipe_] = p; return next_pipe_; }
  SamplerId add_sampler(const SamplerDesc& s) { samplers[++next_sampler_] = s; return next_sampler_; }
  BindGroupId add_bind_group(const BindInfo& b) { bind_groups[++next_bind_] = b; return next_bind_; }

  static std::string check_render_target(const RenderTargetDesc& d) {
    if (d.width == 0 || d.height == 0 || d.width > kMaxRenderTargetSize || d.height > kMaxRenderTargetSize) return "size out of range";
    if (!(d.clear_depth >= 0.0f && d.clear_depth <= 1.0f)) return "clear_depth outside [0,1]";
    return "";
  }
  static std::string check_buffer_bytes(uint32_t bytes) {
    if (bytes == 0 || bytes > kMaxBufferBytes) return "size out of range";
    return "";
  }
  static std::string check_texture(uint32_t w, uint32_t h, uint32_t mips) {
    if (w == 0 || h == 0 || w > kMaxTextureSize || h > kMaxTextureSize) return "size out of range";
    if (mips == 0 || mips > full_mip_count(w, h)) return "mip count out of range";
    return "";
  }
  std::string check_write_buffer(BufferId b, uint32_t off, uint32_t size, bool data_null) const {
    auto it = buffers.find(b);
    if (it == buffers.end()) return "unknown buffer";
    if (data_null && size) return "null data";
    if (off % 4 || size % 4) return "offset/size not a multiple of 4";
    if (uint64_t(off) + size > it->second.bytes) return "write outside the buffer";
    return "";
  }
  std::string check_write_texture(TextureId t, uint32_t mip, uint64_t bytes, bool data_null) const {
    auto it = textures.find(t);
    if (it == textures.end() || it->second.kind != TexKind::Sampled) return "unknown sampled texture";
    if (data_null) return "null data";
    if (mip >= it->second.mips) return "mip out of range";
    if (bytes != uint64_t(mip_dim(it->second.width, mip)) * mip_dim(it->second.height, mip) * 4) return "byte count does not match the mip size";
    return "";
  }
  static std::string check_sampler(const SamplerDesc& s) {
    if (s.max_anisotropy < 1 || s.max_anisotropy > 16) return "max_anisotropy out of range";
    if (s.max_anisotropy > 1 && (s.mag != FilterMode::Linear || s.min != FilterMode::Linear || s.mip != FilterMode::Linear))
      return "anisotropy needs linear filters";
    return "";
  }
  static std::string check_mesh_pipeline(const MeshPipelineDesc& d) {
    if (d.wgsl.empty()) return "empty wgsl";
    if (d.uniform_bytes == 0 || d.uniform_bytes > kMaxUniformBytes) return "uniform_bytes out of range";
    if (d.vertex_stride < 4 || d.vertex_stride > 2048 || d.vertex_stride % 4) return "vertex_stride out of range";
    if (d.attributes.empty() || d.attributes.size() > kMaxVertexAttributes) return "attribute count out of range";
    uint32_t used = 0;
    for (const VertexAttribute& a : d.attributes) {
      if (a.location >= kMaxVertexAttributes || (used >> a.location) & 1u) return "attribute location invalid or repeated";
      used |= 1u << a.location;
      if (a.offset % 4 || uint64_t(a.offset) + vertex_format_bytes(a.format) > d.vertex_stride) return "attribute outside the stride";
    }
    if (static_cast<unsigned>(d.depth_compare) > 7) return "bad depth compare";
    return "";
  }
  std::string check_bind_group(const BindGroupDesc& d) const {
    auto b = buffers.find(d.uniforms);
    if (b == buffers.end() || b->second.kind != BufKind::Uniform) return "unknown uniform buffer";
    if (d.uniform_bytes == 0 || d.uniform_bytes > kMaxUniformBytes || uniform_stride(d.uniform_bytes) > b->second.bytes)
      return "uniform_bytes out of range";
    auto tx = textures.find(d.texture);
    if (tx == textures.end() || tx->second.kind != TexKind::Sampled) return "unknown sampled texture";
    if (!samplers.count(d.sampler)) return "unknown sampler";
    return "";
  }
  // M0 draw(): the pipeline must be an M0 pipeline; with a uniform block, the buffer must hold [off, off + bytes).
  std::string check_draw(PipelineId p, BufferId u, uint32_t off) const {
    auto pi = pipelines.find(p);
    if (pi == pipelines.end() || pi->second.mesh) return "unknown M0 pipeline";
    if (pi->second.uniform_bytes == 0) return "";
    auto b = buffers.find(u);
    if (b == buffers.end() || b->second.kind != BufKind::Uniform) return "unknown uniform buffer";
    if (off % kUniformAlign) return "uniform_offset not aligned";
    if (uint64_t(off) + pi->second.uniform_bytes > b->second.bytes) return "uniform range outside the buffer";
    return "";
  }
  std::string check_draw_indexed(const DrawIndexedDesc& d) const {
    auto p = pipelines.find(d.pipeline);
    if (p == pipelines.end() || !p->second.mesh) return "unknown mesh pipeline";
    auto vb = buffers.find(d.vertices);
    if (vb == buffers.end() || vb->second.kind != BufKind::Vertex) return "unknown vertex buffer";
    auto ib = buffers.find(d.indices);
    if (ib == buffers.end() || ib->second.kind != BufKind::Index) return "unknown index buffer";
    if (d.index_count == 0 || d.index_count % 3) return "index_count not a positive multiple of 3";
    if ((uint64_t(d.first_index) + d.index_count) * 4 > ib->second.bytes) return "index range outside the index buffer";
    auto bg = bind_groups.find(d.bind_group);
    if (bg == bind_groups.end()) return "unknown bind group";
    if (bg->second.stride != uniform_stride(p->second.uniform_bytes) || bg->second.slots != p->second.slots)
      return "bind group does not match the pipeline layout";
    if (d.uniform_offset % kUniformAlign) return "uniform_offset not aligned";
    auto ub = buffers.find(bg->second.desc.uniforms);
    if (ub == buffers.end() || uint64_t(d.uniform_offset) + bg->second.stride > ub->second.bytes) return "uniform range outside the buffer";
    return "";
  }

 private:
  uint32_t next_tex_ = 0, next_buf_ = 0, next_pipe_ = 0, next_sampler_ = 0, next_bind_ = 0;
};

}  // namespace lomn::rhi::detail
