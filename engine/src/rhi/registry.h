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

// Uniform buffer capacity as the WebGPU backend allocates it: bytes rounded up to kUniformAlign, in u64. A size whose
// capacity exceeds kMaxBufferBytes gets 0: the id is still returned (M0 contract, M0 log text) but it has no storage,
// so every later write, bind group or draw that uses it is rejected (and no Dawn buffer is ever created for it).
inline uint64_t uniform_capacity(uint32_t bytes) {
  const uint64_t cap = (uint64_t(bytes) + kUniformAlign - 1) / kUniformAlign * kUniformAlign;
  return cap > kMaxBufferBytes ? 0 : cap;
}

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
    if (it->second.bytes == 0) return "buffer has no storage";
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
    const FilterMode filters[3] = {s.mag, s.min, s.mip};
    for (FilterMode f : filters)
      if (static_cast<unsigned>(f) > static_cast<unsigned>(FilterMode::Linear)) return "bad filter mode";
    if (static_cast<unsigned>(s.u) > static_cast<unsigned>(AddressMode::Mirror) ||
        static_cast<unsigned>(s.v) > static_cast<unsigned>(AddressMode::Mirror))
      return "bad address mode";
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
      if (static_cast<unsigned>(a.format) > static_cast<unsigned>(VertexFormat::Float32x4)) return "bad vertex format";
      if (a.location >= kMaxVertexAttributes || (used >> a.location) & 1u) return "attribute location invalid or repeated";
      used |= 1u << a.location;
      if (a.offset % 4 || uint64_t(a.offset) + vertex_format_bytes(a.format) > d.vertex_stride) return "attribute outside the stride";
    }
    if (static_cast<unsigned>(d.blend) > static_cast<unsigned>(BlendMode::AlphaBlend)) return "bad blend mode";
    if (static_cast<unsigned>(d.cull) > static_cast<unsigned>(CullMode::Back)) return "bad cull mode";
    if (static_cast<unsigned>(d.depth_compare) > static_cast<unsigned>(CompareFunc::Always)) return "bad depth compare";
    if (static_cast<unsigned>(d.front_face) > static_cast<unsigned>(FrontFace::CW)) return "bad front face";
    if (d.depth_bias > kMaxDepthBias || d.depth_bias < -kMaxDepthBias) return "depth_bias out of range";
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
  // begin_frame: a render target (create_render_target / _ex), or 0 = the presentation surface when the device has one.
  // An unknown id, a sampled texture or 0 on a headless device is a counted rejection; that frame's draws are skipped.
  std::string check_begin_frame(TextureId t, bool has_surface) const {
    if (t == 0) return has_surface ? "" : "no presentation surface (headless device)";
    auto it = textures.find(t);
    if (it == textures.end() || it->second.kind != TexKind::RenderTarget) return "unknown render target";
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

  // ---- M1b: destroy rules (shared by both backends) ----
  // The first live bind group referencing a texture / sampler / uniform buffer (0 = none). Linear in the number of live
  // bind groups (an area holds at most a few hundred; destroy runs only on area switches).
  BindGroupId bind_group_using_texture(TextureId t) const {
    for (const auto& kv : bind_groups)
      if (kv.second.desc.texture == t) return kv.first;
    return 0;
  }
  BindGroupId bind_group_using_sampler(SamplerId s) const {
    for (const auto& kv : bind_groups)
      if (kv.second.desc.sampler == s) return kv.first;
    return 0;
  }
  BindGroupId bind_group_using_buffer(BufferId b) const {
    for (const auto& kv : bind_groups)
      if (kv.second.desc.uniforms == b) return kv.first;
    return 0;
  }
  std::string check_destroy_texture(TextureId t) const {
    if (!textures.count(t)) return "unknown texture";
    if (BindGroupId g = bind_group_using_texture(t)) return "in use by bind group " + std::to_string(g);
    return "";
  }
  std::string check_destroy_buffer(BufferId b) const {
    if (!buffers.count(b)) return "unknown buffer";
    if (BindGroupId g = bind_group_using_buffer(b)) return "in use by bind group " + std::to_string(g);
    return "";
  }
  std::string check_destroy_pipeline(PipelineId p) const { return pipelines.count(p) ? "" : "unknown pipeline"; }
  std::string check_destroy_sampler(SamplerId s) const {
    if (!samplers.count(s)) return "unknown sampler";
    if (BindGroupId g = bind_group_using_sampler(s)) return "in use by bind group " + std::to_string(g);
    return "";
  }
  std::string check_destroy_bind_group(BindGroupId g) const { return bind_groups.count(g) ? "" : "unknown bind group"; }
  Device::LiveObjects live() const {
    Device::LiveObjects o;
    o.textures = static_cast<uint32_t>(textures.size());
    o.buffers = static_cast<uint32_t>(buffers.size());
    o.pipelines = static_cast<uint32_t>(pipelines.size());
    o.samplers = static_cast<uint32_t>(samplers.size());
    o.bind_groups = static_cast<uint32_t>(bind_groups.size());
    return o;
  }

 private:
  uint32_t next_tex_ = 0, next_buf_ = 0, next_pipe_ = 0, next_sampler_ = 0, next_bind_ = 0;
};

}  // namespace lomn::rhi::detail
