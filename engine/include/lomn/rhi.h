// Render interface (frozen in M0; additive changes only, reviewed). Two backends: "null" (records a
// deterministic call log, no GPU) and "webgpu" (Dawn natively, emdawnwebgpu in the browser).
// M0 surface: render targets, one uniform block per pipeline (dynamic offset, 256-byte aligned), non-indexed draws.
// M1a-3 additive: MeshPipelineDesc.front_face and .depth_bias (D3D CW front faces, D3DRS_ZBIAS).
// M1b additive: destroy_* (with a dependency rule), live_objects(), and a presentation-surface depth buffer + resize.
// REF4 r2 additive: set_lost_handler (device loss / first uncaptured error reaches the caller).
// M1a-2 additive surface (unlit textured indexed draws): vertex/index buffers, RGBA8 textures with a CPU-supplied mip
// chain, samplers, render targets with a depth buffer, mesh pipelines (vertex layout, blend, depth, cull), bind groups
// {dynamic-offset uniform block, texture, sampler} and indexed draws. Every M0 call keeps its behaviour and log text.
// Ids are never reused within a device; every new call validates its ids and sizes in BOTH backends (shared rules in
// src/rhi/registry.h): an invalid create returns 0, an invalid write/draw is rejected (logged by the null backend,
// reported on stderr by WebGPU) and never reaches the GPU.
// Not validated (documented limits of the M1a-2 surface):
//  - index VALUES are not checked against the vertex count: WebGPU's robust buffer access keeps an out-of-range vertex
//    fetch inside the buffer, and the data path (parse_x + lomn_area) already bounds every index;
//  - WGSL vs pipeline/bind-group layout mismatches (missing entry point, wrong binding type) are not detected here: they
//    surface only as a Dawn uncaptured-error message on stderr and that pipeline's frame draws nothing.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace lomn::rhi {

using TextureId = uint32_t;   // 0 = the presentation surface (browser canvas); never valid headless
using BufferId = uint32_t;    // 0 = none
using PipelineId = uint32_t;
constexpr uint32_t kUniformAlign = 256;
using SamplerId = uint32_t;    // M1a-2; 0 = none
using BindGroupId = uint32_t;  // M1a-2; 0 = none
// M1a-2 limits. Sampled textures: the DDS decoder's cap (sage::kMaxTextureDim). Render targets: WebGPU's default
// maxTextureDimension2D. Mesh pipelines take at most 8 vertex attributes (WebGPU's default maxVertexAttributes is 16).
constexpr uint32_t kMaxTextureSize = 4096;
constexpr uint32_t kMaxRenderTargetSize = 8192;
constexpr uint32_t kMaxVertexAttributes = 8;
// Mesh uniform blocks: up to WebGPU's default maxUniformBufferBindingSize. A block occupies a slot of
// uniform_stride(bytes) = bytes rounded up to kUniformAlign; dynamic offsets stay multiples of kUniformAlign.
constexpr uint32_t kMaxUniformBytes = 65536;
// Computed in u64 so it never wraps (uniform_stride(0xFFFFFFFF) == 4 GiB, not 0); for valid sizes (<= kMaxUniformBytes)
// the result fits in uint32_t.
constexpr uint64_t uniform_stride(uint32_t bytes) { return (uint64_t(bytes) + kUniformAlign - 1) / kUniformAlign * kUniformAlign; }

struct PipelineDesc {
  std::string wgsl;               // must define the vertex/fragment entry points below
  std::string vs_entry = "vs";
  std::string fs_entry = "fs";
  uint32_t uniform_bytes = 0;     // 0 = no bind group; else @group(0) @binding(0) var<uniform>, dynamic offset
};

struct FrameStats {
  uint32_t draws = 0;            // M0 draws + M1a-2 indexed draws accepted this frame
  uint32_t pipelines_total = 0;  // M0 + mesh pipelines created so far
  uint64_t indices = 0;          // M1a-2: indices submitted by draw_indexed this frame
  // M1a-2: calls rejected by validation since the last begin_frame (any call kind, creation calls included; before the
  // first begin_frame: since the device was created). A rejected begin_frame counts 1 and resets the others.
  // M1a-3: draws outside begin_frame/end_frame (before the first begin_frame, or after end_frame) are counted here too.
  // The counters are NOT frozen by end_frame: last_frame_stats() read after end_frame also includes rejections of calls
  // made since (e.g. an out-of-frame draw or a rejected create), until the next begin_frame resets them.
  uint32_t rejected = 0;
};

// ---- M1a-2 additive types ----
struct RenderTargetDesc {
  uint32_t width = 0, height = 0;   // 1..kMaxRenderTargetSize
  bool depth = false;               // adds a depth buffer (Depth32Float in WebGPU), cleared at every begin_frame
  float clear_depth = 1.0f;         // 0..1; with LESSEQUAL the far plane (1.0) is the only useful clear value
};

enum class VertexFormat : uint8_t { Float32x2, Float32x3, Float32x4 };
struct VertexAttribute {
  VertexFormat format = VertexFormat::Float32x3;
  uint32_t offset = 0;    // bytes into the vertex
  uint32_t location = 0;  // @location(n), < kMaxVertexAttributes, unique
};
// Opaque: no blending. AlphaBlend: colour = src * SRCALPHA + dst * INVSRCALPHA (D3DRS_SRCBLEND 5, D3DRS_DESTBLEND 6);
// the target's alpha channel is left unchanged (the D3D8 back buffer alpha is never displayed).
enum class BlendMode : uint8_t { Opaque, AlphaBlend };
// Numbered like D3DCMPFUNC - 1 (D3DCMP_NEVER = 1 ... D3DCMP_ALWAYS = 8).
enum class CompareFunc : uint8_t { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };
// None = render mode 0x11 (D3DCULL_NONE). Front/Back are RELATIVE to MeshPipelineDesc::front_face (M1a-3): Front culls
// the triangles whose screen winding equals front_face, Back the others. With the default front_face CCW this is the
// M1a-2 behaviour. D3D8's front face is clockwise (D3DCULL_CCW culls counter-clockwise = back faces): the D3D8 shim sets
// front_face CW and maps D3DCULL_CW -> Front, D3DCULL_CCW -> Back.
enum class CullMode : uint8_t { None, Front, Back };
// M1a-3: which winding is the front face (WebGPU primitive.frontFace). CCW is WebGPU's default (and M1a-2's); the
// D3D8 shim uses CW, D3D's front face, and maps D3DCULL_CW -> Front, D3DCULL_CCW -> Back (verified on Dawn by
// test_gpu_mesh's winding probe).
enum class FrontFace : uint8_t { CCW, CW };
// M1a-3: constant depth bias limit (|depth_bias| <= kMaxDepthBias, WebGPU depthBias units; one more is a rejected create).
// Semantics (WebGPU depthBias, the constant term only): the fragment depth is offset by depth_bias * r, where r is the
// smallest resolvable depth difference of the target's depth format. Every depth target here is Depth32Float, whose r
// depends on the depth value (2^(exponent(z) - 23)): about 2^-24 near z = 0.5. Sign: NEGATIVE = nearer the camera
// (passes LESS / LESSEQUAL against an equal depth), positive = farther. depthBiasSlopeScale and depthBiasClamp are always
// 0 (not exposed): no slope-scaled bias, no clamp.
constexpr int32_t kMaxDepthBias = 1 << 16;

// Pipeline for indexed meshes. Bind group 0: @binding(0) var<uniform> (dynamic offset, bound size
// uniform_stride(uniform_bytes)), @binding(1) texture_2d<f32>, @binding(2) sampler (filtering). One vertex buffer
// (slot 0), u32 indices, triangle list. A bind group is compatible with a pipeline when both have the same uniform
// stride and the same populated slots (backends cache one layout per (stride, slots)). Visibility: the uniform block is
// visible to both stages; the texture and sampler bindings are FRAGMENT-ONLY (no vertex texture fetch).
// Every enum field (and SamplerDesc's) is range-checked: a value past the last enumerator is rejected.
// Alpha test has no fixed-function equivalent in WebGPU: it is a `discard` in the fragment entry point (pick a
// different fs_entry per pipeline).
struct MeshPipelineDesc {
  std::string wgsl;
  std::string vs_entry = "vs";
  std::string fs_entry = "fs";
  uint32_t uniform_bytes = 0;     // 1..kMaxUniformBytes (slot stride uniform_stride(uniform_bytes))
  uint32_t vertex_stride = 0;     // 4..2048, multiple of 4
  std::vector<VertexAttribute> attributes;  // 1..kMaxVertexAttributes, each inside the stride
  BlendMode blend = BlendMode::Opaque;
  bool depth_write = true;                  // only meaningful on a target with depth
  CompareFunc depth_compare = CompareFunc::LessEqual;
  CullMode cull = CullMode::None;
  // ---- M1a-3 additive fields (defaults = M1a-2 behaviour; the null log prints them only when not default) ----
  FrontFace front_face = FrontFace::CCW;
  int32_t depth_bias = 0;  // WebGPU depthBias (constant part; negative = nearer, see kMaxDepthBias); D3DRS_ZBIAS maps here (lomn/d3d/ffp.h)
};

enum class FilterMode : uint8_t { Nearest, Linear };
enum class AddressMode : uint8_t { Wrap, Clamp, Mirror };
struct SamplerDesc {
  FilterMode mag = FilterMode::Linear, min = FilterMode::Linear, mip = FilterMode::Linear;
  AddressMode u = AddressMode::Wrap, v = AddressMode::Wrap;  // D3D8 default (the game never sets ADDRESSU/V)
  uint16_t max_anisotropy = 1;  // 1..16; > 1 requires all three filters Linear (WebGPU rule)
};

struct DrawIndexedDesc {
  PipelineId pipeline = 0;       // from create_mesh_pipeline
  BufferId vertices = 0;         // from create_vertex_buffer
  BufferId indices = 0;          // from create_index_buffer (u32 indices)
  uint32_t first_index = 0;      // in indices
  uint32_t index_count = 0;      // > 0, multiple of 3; first_index + index_count inside the index buffer
  BindGroupId bind_group = 0;    // its uniform stride and populated slots must equal the pipeline's
  uint32_t uniform_offset = 0;   // multiple of kUniformAlign; offset + uniform_stride inside the uniform buffer
};

// Group 0 contents. Named fields (not positional args) so later milestones can add optional slots additively
// (e.g. a per-frame uniform block or a second texture: 0 = absent) without breaking callers.
struct BindGroupDesc {
  BufferId uniforms = 0;       // uniform buffer, bound with a dynamic offset
  uint32_t uniform_bytes = 0;  // 1..kMaxUniformBytes; the buffer must hold at least uniform_stride(uniform_bytes)
  TextureId texture = 0;       // sampled texture (create_texture)
  SamplerId sampler = 0;
};

class Device {
 public:
  virtual ~Device() = default;
  virtual std::string backend() const = 0;   // "null" | "webgpu"
  virtual std::string adapter() const = 0;   // adapter description, or "none"
  virtual TextureId create_render_target(uint32_t width, uint32_t height) = 0;  // RGBA8, readable
  // Capacity = bytes rounded up to kUniformAlign. Over 256 MiB (M1a-2): the id is returned but has no storage (counted as
  // a rejection); every later write, bind group or draw using it is rejected. A 0-byte uniform buffer has no storage
  // either, exactly like one over kMaxBufferBytes: every write, bind or draw using it is rejected.
  virtual BufferId create_uniform_buffer(uint32_t bytes) = 0;
  // Within one frame a uniform range may be written at most once: queue writes land before the frame's command buffer
  // executes (WebGPU), so rewriting the same range between two draws feeds the last value to both. Give each draw its
  // own dynamic offset.
  virtual void write_buffer(BufferId buffer, uint32_t offset, const void* data, uint32_t size) = 0;
  virtual PipelineId create_pipeline(const PipelineDesc& desc) = 0;
  // target: a render target, or 0 = the presentation surface. An unknown id, a sampled texture, or 0 on a headless
  // device is a counted rejection (M1a-2) and that frame's draws are skipped (not drawn, not counted).
  virtual void begin_frame(TextureId target, const float clear_rgba[4]) = 0;
  // M1a-3: a draw (or draw_indexed) outside begin_frame/end_frame - before the first begin_frame or after end_frame - is
  // a counted rejection (FrameStats::rejected) in both backends, never a silent no-op. Inside a frame whose begin_frame
  // was rejected, draws are skipped (not drawn, not counted, not rejected again).
  virtual void draw(PipelineId pipeline, uint32_t vertex_count, BufferId uniforms, uint32_t uniform_offset) = 0;
  virtual void end_frame() = 0;
  // Synchronous readback of a render target into tightly packed RGBA8. False if unsupported (null, browser).
  virtual bool read_pixels(TextureId target, std::vector<uint8_t>& rgba) = 0;
  virtual FrameStats last_frame_stats() const = 0;

  // ---- M1a-2 additive calls ----
  // NOT pure virtual: a Device written against the M0 surface (e.g. a test double) still compiles and reports every
  // M1a-2 call as unsupported (0 / false / no draw). Both shipped backends override all of them.
  // Render target with an optional depth buffer (RGBA8, readable like create_render_target). 0 if invalid.
  virtual TextureId create_render_target_ex(const RenderTargetDesc&) { return 0; }
  // Vertex / index (u32) buffers, filled with write_buffer (offset and size multiples of 4). 0 if bytes is 0 or > 256 MiB.
  virtual BufferId create_vertex_buffer(uint32_t) { return 0; }
  virtual BufferId create_index_buffer(uint32_t) { return 0; }
  // Sampled RGBA8 (unorm, no sRGB: D3D8 had none) texture with mip_levels levels (1..floor(log2(max(w,h)))+1). 0 if invalid.
  virtual TextureId create_texture(uint32_t /*width*/, uint32_t /*height*/, uint32_t /*mip_levels*/) { return 0; }
  // Uploads one whole mip level, tightly packed RGBA8: bytes must be max(1,w>>mip) * max(1,h>>mip) * 4. False if rejected.
  virtual bool write_texture(TextureId, uint32_t /*mip*/, const void* /*rgba*/, std::size_t /*bytes*/) { return false; }
  virtual SamplerId create_sampler(const SamplerDesc&) { return 0; }
  virtual PipelineId create_mesh_pipeline(const MeshPipelineDesc&) { return 0; }
  // Group 0 of a mesh pipeline (see BindGroupDesc). 0 if invalid.
  virtual BindGroupId create_bind_group(const BindGroupDesc&) { return 0; }
  // Indexed triangle-list draw inside begin_frame/end_frame. Invalid draws are rejected and not counted.
  // Within one frame a uniform range may be written at most once: queue writes land before the frame's command buffer
  // executes (WebGPU), so rewriting the same range between two draws feeds the last value to both. Give each draw its
  // own dynamic offset.
  virtual void draw_indexed(const DrawIndexedDesc&) {}

  // ---- M1b additive calls (area switching and browser presentation) ----
  // NOT pure virtual (as the M1a-2 calls): a Device written against an older surface reports them unsupported.
  // destroy_*: frees the object; ids are still never reused. Returns true when the object was destroyed. Rejected
  // (counted in FrameStats::rejected, logged by the null backend, reported on stderr by WebGPU; nothing is freed):
  //  - an unknown or already-destroyed id (0 included; the presentation surface is not a texture that can be destroyed);
  //  - DEPENDENCY RULE: a sampled texture, a sampler or a uniform buffer still referenced by a LIVE bind group. Destroy the
  //    bind group first. Rejection was chosen over cascading: a cascade would silently invalidate a bind group id that
  //    another owner still holds, while a rejection is visible and testable. Pipelines are referenced by no rhi object.
  // Inside an open frame a destroy is allowed: draws already recorded keep their objects alive (WebGPU reference
  // counting); a later draw that names a destroyed id is rejected like any unknown id.
  virtual bool destroy_texture(TextureId) { return false; }  // sampled textures and render targets (+ their depth buffer)
  virtual bool destroy_buffer(BufferId) { return false; }    // uniform, vertex and index buffers
  virtual bool destroy_pipeline(PipelineId) { return false; }  // M0 and mesh pipelines (+ every per-format variant)
  virtual bool destroy_sampler(SamplerId) { return false; }
  virtual bool destroy_bind_group(BindGroupId) { return false; }
  // Live objects per kind (created and not destroyed): the leak check of area switching (counts must return to the
  // same values after N switches). Render targets count as textures.
  struct LiveObjects {
    uint32_t textures = 0, buffers = 0, pipelines = 0, samplers = 0, bind_groups = 0;
    bool operator==(const LiveObjects& o) const {
      return textures == o.textures && buffers == o.buffers && pipelines == o.pipelines && samplers == o.samplers &&
             bind_groups == o.bind_groups;
    }
  };
  virtual LiveObjects live_objects() const { return LiveObjects(); }
  // Presentation surface (browser canvas; target 0). Both are counted rejections returning false on a device without a
  // surface (headless WebGPU, null) and inside an open frame.
  // set_surface_depth: gives frames on target 0 a Depth32Float depth buffer (the same format, pipeline variants and
  // depth-bias units as create_render_target_ex), cleared to clear_depth (0..1) at every begin_frame(0); false turns it off.
  virtual bool set_surface_depth(bool /*enabled*/, float /*clear_depth*/) { return false; }
  // resize: reconfigures the surface to width x height (1..kMaxRenderTargetSize; in the browser this also sets the
  // canvas drawing-buffer size) and recreates the surface depth buffer at the new size when it is enabled.
  virtual bool resize(uint32_t /*width*/, uint32_t /*height*/) { return false; }
  // REF4 r2 (C1): called at most once, with a readable reason, when the device becomes unusable: a device loss for any
  // reason other than the device's own destruction, or the first uncaptured GPU error (a validation error the shared
  // checks could not see, e.g. bad WGSL). A loss that happened before the handler was set is delivered when it is set.
  // The null backend never loses its device (it logs the call). Default: no-op.
  virtual void set_lost_handler(std::function<void(const std::string& reason)> /*handler*/) {}
};

// Null backend: appends one line per call to *call_log (if non-null). Lines are stable text, so two runs
// (or native vs wasm32) can be compared byte for byte.
std::unique_ptr<Device> create_null_device(std::vector<std::string>* call_log);

#if defined(LOMN_HAS_WEBGPU)
using DeviceCallback = std::function<void(std::unique_ptr<Device> device, std::string error)>;
// Asynchronous creation (the only form available in the browser). canvas_selector: CSS selector of the
// canvas to present to, or nullptr for headless. Natively the callback runs before this function returns.
void create_webgpu_device(const char* canvas_selector, uint32_t width, uint32_t height, DeviceCallback done);
#endif

}  // namespace lomn::rhi
