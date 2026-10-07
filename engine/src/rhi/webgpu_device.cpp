// WebGPU backend. Same code for native (Dawn, headless render targets + synchronous readback via
// wgpuInstanceWaitAny) and browser (emdawnwebgpu, presents to a canvas; no Asyncify: device creation
// is callback-driven and the caller starts its frame loop from the callback).
// M1a-2: indexed meshes (vertex/index buffers, sampled RGBA8 textures with CPU mips, samplers, depth targets, mesh
// pipelines, bind groups). Validation is shared with the null backend (registry.h): a rejected call never reaches Dawn.
// M1b: destroy_* (registry dependency rule; the backend's private M0 bind-group cache is released with its pipeline or
// buffer), live_objects(), a Depth32Float depth buffer for the presentation surface and resize().
#include <webgpu/webgpu.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <utility>

#include "lomn/rhi.h"
#include "registry.h"

#if defined(__EMSCRIPTEN__)
#define LOMN_CB_MODE WGPUCallbackMode_AllowSpontaneous
#else
#define LOMN_CB_MODE WGPUCallbackMode_WaitAnyOnly
#endif

namespace lomn::rhi {
namespace {

using detail::BufKind;
using detail::Registry;
using detail::TexKind;

WGPUStringView sv(const std::string& s) { return WGPUStringView{s.data(), s.size()}; }
std::string str(WGPUStringView v) { return v.data ? std::string(v.data, v.length == WGPU_STRLEN ? std::strlen(v.data) : v.length) : std::string(); }

constexpr WGPUTextureFormat kDepthFormat = WGPUTextureFormat_Depth32Float;

struct Target {
  WGPUTexture texture = nullptr;
  WGPUTextureView view = nullptr;
  WGPUTexture depth = nullptr;        // M1a-2: optional depth buffer
  WGPUTextureView depth_view = nullptr;
  float clear_depth = 1.0f;
  uint32_t width = 0, height = 0;
};
struct Sampled {
  WGPUTexture texture = nullptr;
  WGPUTextureView view = nullptr;
};
// Pipeline variants are built lazily per (colour format, pass has depth): the canvas format differs from offscreen
// RGBA8Unorm, and a pass with a depth attachment needs a depthStencil state in every pipeline drawn into it.
using VariantKey = std::pair<WGPUTextureFormat, bool>;
struct Pipeline {
  PipelineDesc desc;
  MeshPipelineDesc mesh_desc;  // M1a-2 (mesh == true)
  bool mesh = false;
  WGPUShaderModule module = nullptr;
  WGPUBindGroupLayout layout = nullptr;  // owned only for M0 pipelines; mesh pipelines share mesh_layouts_
  WGPUPipelineLayout pipeline_layout = nullptr;
  std::map<VariantKey, WGPURenderPipeline> by_format;
};

WGPUVertexFormat vformat(VertexFormat f) {
  switch (f) {
    case VertexFormat::Float32x2: return WGPUVertexFormat_Float32x2;
    case VertexFormat::Float32x3: return WGPUVertexFormat_Float32x3;
    default: return WGPUVertexFormat_Float32x4;
  }
}
WGPUCompareFunction cmp(CompareFunc c) {
  switch (c) {
    case CompareFunc::Never: return WGPUCompareFunction_Never;
    case CompareFunc::Less: return WGPUCompareFunction_Less;
    case CompareFunc::Equal: return WGPUCompareFunction_Equal;
    case CompareFunc::LessEqual: return WGPUCompareFunction_LessEqual;
    case CompareFunc::Greater: return WGPUCompareFunction_Greater;
    case CompareFunc::NotEqual: return WGPUCompareFunction_NotEqual;
    case CompareFunc::GreaterEqual: return WGPUCompareFunction_GreaterEqual;
    default: return WGPUCompareFunction_Always;
  }
}
WGPUAddressMode addr(AddressMode a) {
  return a == AddressMode::Wrap ? WGPUAddressMode_Repeat : a == AddressMode::Clamp ? WGPUAddressMode_ClampToEdge : WGPUAddressMode_MirrorRepeat;
}

// REF4 r2 (C1): where the device-lost and uncaptured-error callbacks report. It is the callbacks' userdata, so it is
// created before the device and deliberately never freed (Dawn may still deliver a Destroyed loss while the device is
// released; one small object per device, and a page creates one device).
// T8 P6: notify() runs the caller's handler inside the Dawn callback (re-entrantly, from within a Dawn call on this
// device natively): the handler must not destroy the device or re-enter rhi (see Device::set_lost_handler in rhi.h).
struct LostSink {
  std::function<void(const std::string&)> handler;
  bool fired = false;
  std::string pending;  // a loss before set_lost_handler: delivered when the handler is set
  void notify(std::string why) {
    if (fired) return;
    fired = true;
    if (handler) handler(why);
    else pending = std::move(why);
  }
};

class WebGpuDevice final : public Device {
 public:
  WebGpuDevice(WGPUInstance inst, WGPUAdapter adapter, WGPUDevice dev, WGPUSurface surface, WGPUTextureFormat surface_format,
               std::string adapter_desc, uint32_t surface_width = 0, uint32_t surface_height = 0, LostSink* lost = nullptr)
      : inst_(inst), adapter_(adapter), dev_(dev), queue_(wgpuDeviceGetQueue(dev)), surface_(surface),
        surface_format_(surface_format), adapter_desc_(std::move(adapter_desc)), surface_w_(surface_width), surface_h_(surface_height),
        lost_(lost) {}

  // Releases every owned handle in reverse order of creation (children before the device, then
  // queue, device, adapter, instance).
  WebGpuDevice(const WebGpuDevice&) = delete;
  WebGpuDevice& operator=(const WebGpuDevice&) = delete;
  WebGpuDevice(WebGpuDevice&&) = delete;
  WebGpuDevice& operator=(WebGpuDevice&&) = delete;

  ~WebGpuDevice() override {
    flush_deferred_destroys();
    if (pass_) {
      wgpuRenderPassEncoderEnd(pass_);
      wgpuRenderPassEncoderRelease(pass_);
    }
    if (enc_) wgpuCommandEncoderRelease(enc_);
    if (frame_surface_view_) wgpuTextureViewRelease(frame_surface_view_);
    if (frame_surface_texture_) wgpuTextureRelease(frame_surface_texture_);
    release_surface_depth();
    for (auto& kv : bind_groups_) wgpuBindGroupRelease(kv.second);
    for (auto& kv : mesh_bind_groups_) wgpuBindGroupRelease(kv.second);
    for (auto& kv : pipelines_) {
      Pipeline& p = kv.second;
      for (auto& v : p.by_format) wgpuRenderPipelineRelease(v.second);
      if (p.pipeline_layout) wgpuPipelineLayoutRelease(p.pipeline_layout);
      if (p.layout && !p.mesh) wgpuBindGroupLayoutRelease(p.layout);
      if (p.module) wgpuShaderModuleRelease(p.module);
    }
    for (auto& kv : mesh_layouts_) wgpuBindGroupLayoutRelease(kv.second);
    for (auto& kv : samplers_) wgpuSamplerRelease(kv.second);
    for (auto& kv : sampled_) {
      if (kv.second.view) wgpuTextureViewRelease(kv.second.view);
      if (kv.second.texture) wgpuTextureRelease(kv.second.texture);
    }
    for (auto& kv : buffers_) wgpuBufferRelease(kv.second);
    for (auto& kv : targets_) {
      if (kv.second.depth_view) wgpuTextureViewRelease(kv.second.depth_view);
      if (kv.second.depth) wgpuTextureRelease(kv.second.depth);
      if (kv.second.view) wgpuTextureViewRelease(kv.second.view);
      if (kv.second.texture) wgpuTextureRelease(kv.second.texture);
    }
    if (surface_) {
      wgpuSurfaceUnconfigure(surface_);
      wgpuSurfaceRelease(surface_);
    }
    wgpuQueueRelease(queue_);
    wgpuDeviceRelease(dev_);
    wgpuAdapterRelease(adapter_);
    wgpuInstanceRelease(inst_);
  }

  std::string backend() const override { return "webgpu"; }
  std::string adapter() const override { return adapter_desc_; }

  TextureId create_render_target(uint32_t w, uint32_t h) override {
    Target t = make_target(w, h);
    TextureId id = reg_.add_texture({TexKind::RenderTarget, w, h, 1, false});
    targets_[id] = t;
    return id;
  }

  BufferId create_uniform_buffer(uint32_t bytes) override {
    const uint64_t cap = detail::uniform_capacity(bytes);
    if (bytes && cap == 0) {
      // Over kMaxBufferBytes: registered without storage and no Dawn buffer (every later use is rejected by the registry).
      reject("create_uniform_buffer", "size over kMaxBufferBytes (id registered without storage)");
      return reg_.add_buffer({BufKind::Uniform, 0});
    }
    WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
    bd.size = cap;
    bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    BufferId id = reg_.add_buffer({BufKind::Uniform, static_cast<uint32_t>(cap)});
    buffers_[id] = wgpuDeviceCreateBuffer(dev_, &bd);
    return id;
  }

  void write_buffer(BufferId b, uint32_t off, const void* data, uint32_t size) override {
    std::string why = reg_.check_write_buffer(b, off, size, data == nullptr);
    if (!why.empty()) return reject("write_buffer", why);
    wgpuQueueWriteBuffer(queue_, buffers_.at(b), off, data, size);
  }

  PipelineId create_pipeline(const PipelineDesc& d) override {
    Pipeline p;
    p.desc = d;
    p.module = shader(d.wgsl);
    if (d.uniform_bytes) {
      WGPUBindGroupLayoutEntry e = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
      e.binding = 0;
      e.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
      e.buffer.type = WGPUBufferBindingType_Uniform;
      e.buffer.hasDynamicOffset = true;
      e.buffer.minBindingSize = d.uniform_bytes;
      WGPUBindGroupLayoutDescriptor bl = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
      bl.entryCount = 1;
      bl.entries = &e;
      p.layout = wgpuDeviceCreateBindGroupLayout(dev_, &bl);
      WGPUPipelineLayoutDescriptor pl = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
      pl.bindGroupLayoutCount = 1;
      pl.bindGroupLayouts = &p.layout;
      p.pipeline_layout = wgpuDeviceCreatePipelineLayout(dev_, &pl);
    }
    ++stats_.pipelines_total;
    PipelineId id = reg_.add_pipeline({false, d.uniform_bytes, 0});
    pipelines_[id] = p;
    return id;
  }

  WGPURenderPipeline variant(Pipeline& p, WGPUTextureFormat fmt, bool depth) {
    auto it = p.by_format.find({fmt, depth});
    if (it != p.by_format.end()) return it->second;
    WGPUColorTargetState ct = WGPU_COLOR_TARGET_STATE_INIT;
    ct.format = fmt;
    WGPUBlendState blend = WGPU_BLEND_STATE_INIT;
    if (p.mesh && p.mesh_desc.blend == BlendMode::AlphaBlend) {
      blend.color = {WGPUBlendOperation_Add, WGPUBlendFactor_SrcAlpha, WGPUBlendFactor_OneMinusSrcAlpha};
      // Destination alpha is kept: the D3D8 back buffer's alpha is not displayed (X8R8G8B8), and blending it here would
      // leave partially transparent pixels in readbacks/PNGs (measured: 14,560 pixels in a ghut frame before this rule).
      blend.alpha = {WGPUBlendOperation_Add, WGPUBlendFactor_Zero, WGPUBlendFactor_One};
      ct.blend = &blend;
    }
    WGPUFragmentState fs = WGPU_FRAGMENT_STATE_INIT;
    fs.module = p.module;
    fs.entryPoint = sv(p.mesh ? p.mesh_desc.fs_entry : p.desc.fs_entry);
    fs.targetCount = 1;
    fs.targets = &ct;
    WGPURenderPipelineDescriptor rpd = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    rpd.layout = p.pipeline_layout;
    rpd.vertex.module = p.module;
    rpd.vertex.entryPoint = sv(p.mesh ? p.mesh_desc.vs_entry : p.desc.vs_entry);
    rpd.fragment = &fs;
    WGPUDepthStencilState ds = WGPU_DEPTH_STENCIL_STATE_INIT;
    if (depth) {
      ds.format = kDepthFormat;
      // M0 pipelines ignore depth (always pass, never write) so they keep their M0 output in a depth pass.
      ds.depthWriteEnabled = p.mesh && p.mesh_desc.depth_write ? WGPUOptionalBool_True : WGPUOptionalBool_False;
      ds.depthCompare = p.mesh ? cmp(p.mesh_desc.depth_compare) : WGPUCompareFunction_Always;
      if (p.mesh) ds.depthBias = p.mesh_desc.depth_bias;  // M1a-3 (D3DRS_ZBIAS); 0 = M1a-2 behaviour
      rpd.depthStencil = &ds;
    }
    std::vector<WGPUVertexAttribute> attrs;
    WGPUVertexBufferLayout vbl = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
    if (p.mesh) {
      for (const VertexAttribute& a : p.mesh_desc.attributes) {
        WGPUVertexAttribute va = WGPU_VERTEX_ATTRIBUTE_INIT;
        va.format = vformat(a.format);
        va.offset = a.offset;
        va.shaderLocation = a.location;
        attrs.push_back(va);
      }
      vbl.stepMode = WGPUVertexStepMode_Vertex;
      vbl.arrayStride = p.mesh_desc.vertex_stride;
      vbl.attributeCount = attrs.size();
      vbl.attributes = attrs.data();
      rpd.vertex.bufferCount = 1;
      rpd.vertex.buffers = &vbl;
      rpd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
      rpd.primitive.cullMode = p.mesh_desc.cull == CullMode::None ? WGPUCullMode_None
                               : p.mesh_desc.cull == CullMode::Front ? WGPUCullMode_Front : WGPUCullMode_Back;
      rpd.primitive.frontFace = p.mesh_desc.front_face == FrontFace::CW ? WGPUFrontFace_CW : WGPUFrontFace_CCW;  // M1a-3
    }
    return p.by_format[{fmt, depth}] = wgpuDeviceCreateRenderPipeline(dev_, &rpd);
  }

  void begin_frame(TextureId target, const float c[4]) override {
    stats_.draws = 0;
    stats_.indices = 0;
    stats_.rejected = 0;
    frame_active_ = false;
    frame_open_ = true;  // M1a-3: draws between begin_frame and end_frame are in a frame even if it was rejected
    pass_depth_ = false;
    WGPUTextureView view = nullptr;
    const Target* tgt = nullptr;
    // An unknown id, a sampled texture or 0 without a surface: a counted rejection (shared rule with the null backend);
    // draw/end_frame become no-ops for this frame.
    const std::string why = reg_.check_begin_frame(target, surface_ != nullptr);
    if (!why.empty()) return reject("begin_frame", why);
    if (target == 0) {
      // A transient surface failure (browser) still skips the frame with a once-only log: it is not a caller error.
      WGPUSurfaceTexture st = WGPU_SURFACE_TEXTURE_INIT;
      wgpuSurfaceGetCurrentTexture(surface_, &st);
      const bool ok = (st.status == WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal ||
                       st.status == WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) && st.texture;
      if (!ok) {
        log_skip_once("surface texture unavailable, status=" + std::to_string(int(st.status)));
        if (st.texture) wgpuTextureRelease(st.texture);
        return;
      }
      frame_surface_texture_ = st.texture;
      frame_surface_view_ = view = wgpuTextureCreateView(st.texture, nullptr);
      pass_format_ = surface_format_;
      // M1b: the surface depth buffer (set_surface_depth) has the configured surface size; a surface texture of another
      // size (the canvas resized behind resize()'s back) would fail validation, so that frame is skipped instead.
      if (surface_depth_view_) {
        const uint32_t tw = wgpuTextureGetWidth(st.texture), th = wgpuTextureGetHeight(st.texture);
        if (tw != surface_depth_w_ || th != surface_depth_h_) {
          log_skip_once("surface texture " + std::to_string(tw) + "x" + std::to_string(th) + " != depth buffer " +
                        std::to_string(surface_depth_w_) + "x" + std::to_string(surface_depth_h_) + " (call resize)");
          wgpuTextureViewRelease(frame_surface_view_);
          wgpuTextureRelease(frame_surface_texture_);
          frame_surface_view_ = nullptr;
          frame_surface_texture_ = nullptr;
          return;
        }
      }
    } else {
      auto it = targets_.find(target);
      if (it == targets_.end()) return reject("begin_frame", "unknown render target");  // registry and targets_ agree
      tgt = &it->second;
      view = tgt->view;
      pass_format_ = WGPUTextureFormat_RGBA8Unorm;
    }
    enc_ = wgpuDeviceCreateCommandEncoder(dev_, nullptr);
    WGPURenderPassColorAttachment ca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    ca.view = view;
    ca.loadOp = WGPULoadOp_Clear;
    ca.storeOp = WGPUStoreOp_Store;
    ca.clearValue = {c[0], c[1], c[2], c[3]};
    WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &ca;
    WGPURenderPassDepthStencilAttachment da = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
    WGPUTextureView depth_view = tgt ? tgt->depth_view : surface_depth_view_;
    if (depth_view) {
      da.view = depth_view;
      da.depthLoadOp = WGPULoadOp_Clear;
      da.depthStoreOp = WGPUStoreOp_Discard;
      da.depthClearValue = tgt ? tgt->clear_depth : surface_clear_depth_;
      rp.depthStencilAttachment = &da;
      pass_depth_ = true;
    }
    pass_ = wgpuCommandEncoderBeginRenderPass(enc_, &rp);
    frame_active_ = true;
  }

  void draw(PipelineId pid, uint32_t n, BufferId u, uint32_t off) override {
    if (!frame_active_) {  // M1a-3: outside a frame (or in a rejected one) -> skipped; outside any frame it counts
      if (!frame_open_) reject("draw", "outside a frame");
      return;
    }
    std::string why = reg_.check_draw(pid, u, off);
    if (!why.empty()) return reject("draw", why);  // unknown id or a mesh pipeline: never reaches Dawn, never throws
    Pipeline& p = pipelines_.at(pid);
    wgpuRenderPassEncoderSetPipeline(pass_, variant(p, pass_format_, pass_depth_));
    if (p.desc.uniform_bytes) wgpuRenderPassEncoderSetBindGroup(pass_, 0, bind_group(pid, p, u), 1, &off);
    wgpuRenderPassEncoderDraw(pass_, n, 1, 0, 0);
    ++stats_.draws;
  }

  void end_frame() override {
    frame_open_ = false;
    if (!frame_active_) return flush_deferred_destroys();
    frame_active_ = false;
    wgpuRenderPassEncoderEnd(pass_);
    wgpuRenderPassEncoderRelease(pass_);
    pass_ = nullptr;
    WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc_, nullptr);
    wgpuQueueSubmit(queue_, 1, &cb);
    wgpuCommandBufferRelease(cb);
    wgpuCommandEncoderRelease(enc_);
    enc_ = nullptr;
    if (frame_surface_view_) {
      wgpuTextureViewRelease(frame_surface_view_);
      wgpuTextureRelease(frame_surface_texture_);  // the surface texture is acquired per frame and must be released
      frame_surface_view_ = nullptr;
      frame_surface_texture_ = nullptr;
    }
    flush_deferred_destroys();  // REF4 r2 (I1): after the submit that may still reference them
  }

  bool read_pixels(TextureId target, std::vector<uint8_t>& rgba) override {
#if defined(__EMSCRIPTEN__)
    (void)target;
    (void)rgba;
    return false;  // browser readback is async; tests screenshot the canvas instead
#else
    auto tit = targets_.find(target);
    if (tit == targets_.end()) return false;
    const Target& t = tit->second;
    const uint32_t row = (t.width * 4 + 255) / 256 * 256;
    WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
    bd.size = uint64_t(row) * t.height;
    bd.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
    WGPUBuffer buf = wgpuDeviceCreateBuffer(dev_, &bd);
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(dev_, nullptr);
    WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    src.texture = t.texture;
    WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    dst.buffer = buf;
    dst.layout.bytesPerRow = row;
    dst.layout.rowsPerImage = t.height;
    WGPUExtent3D ext = {t.width, t.height, 1};
    wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &ext);
    WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(queue_, 1, &cb);
    // Heap userdata: if the wait below fails, the map callback may still fire later, so it must not point at this stack frame.
    struct MapState { bool mapped = false; };
    auto* ms = new MapState;
    WGPUBufferMapCallbackInfo mcb = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    mcb.mode = WGPUCallbackMode_WaitAnyOnly;
    mcb.callback = [](WGPUMapAsyncStatus s, WGPUStringView msg, void* u1, void*) {
      static_cast<MapState*>(u1)->mapped = s == WGPUMapAsyncStatus_Success;
      if (s != WGPUMapAsyncStatus_Success) std::fprintf(stderr, "[webgpu] readback map failed (status %d): %s\n", int(s), str(msg).c_str());
    };
    mcb.userdata1 = ms;
    WGPUFutureWaitInfo w = WGPU_FUTURE_WAIT_INFO_INIT;
    w.future = wgpuBufferMapAsync(buf, WGPUMapMode_Read, 0, bd.size, mcb);
    const WGPUWaitStatus ws = wgpuInstanceWaitAny(inst_, 1, &w, UINT64_MAX);
    bool ok = false;
    bool mapped = false;
    if (ws == WGPUWaitStatus_Success) {  // callback has run; safe to free its state
      mapped = ms->mapped;
      delete ms;
    }  // else: intentionally leaked (a late callback would write to freed memory)
    if (ws != WGPUWaitStatus_Success) {
      std::fprintf(stderr, "[webgpu] readback wait failed (status %d)\n", int(ws));
    } else if (mapped) {
      const uint8_t* px = static_cast<const uint8_t*>(wgpuBufferGetConstMappedRange(buf, 0, bd.size));
      if (!px) {
        std::fprintf(stderr, "[webgpu] readback: mapped range is null\n");
      } else {
        rgba.resize(size_t(t.width) * t.height * 4);
        for (uint32_t y = 0; y < t.height; ++y) std::memcpy(&rgba[size_t(y) * t.width * 4], px + size_t(y) * row, t.width * 4);
        ok = true;
      }
      wgpuBufferUnmap(buf);
    }
    wgpuCommandBufferRelease(cb);
    wgpuCommandEncoderRelease(enc);
    wgpuBufferRelease(buf);
    return ok;
#endif
  }

  FrameStats last_frame_stats() const override { return stats_; }

  // ---- M1a-2 ----
  TextureId create_render_target_ex(const RenderTargetDesc& d) override {
    std::string why = Registry::check_render_target(d);
    if (!why.empty()) {
      reject("create_render_target_ex", why);
      return 0;
    }
    Target t = make_target(d.width, d.height);
    if (d.depth) {
      WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
      td.size = {d.width, d.height, 1};
      td.format = kDepthFormat;
      td.usage = WGPUTextureUsage_RenderAttachment;
      t.depth = wgpuDeviceCreateTexture(dev_, &td);
      t.depth_view = wgpuTextureCreateView(t.depth, nullptr);
      t.clear_depth = d.clear_depth;
    }
    TextureId id = reg_.add_texture({TexKind::RenderTarget, d.width, d.height, 1, d.depth});
    targets_[id] = t;
    return id;
  }

  BufferId create_vertex_buffer(uint32_t bytes) override { return make_buffer(BufKind::Vertex, bytes, WGPUBufferUsage_Vertex); }
  BufferId create_index_buffer(uint32_t bytes) override { return make_buffer(BufKind::Index, bytes, WGPUBufferUsage_Index); }

  TextureId create_texture(uint32_t w, uint32_t h, uint32_t mips) override {
    std::string why = Registry::check_texture(w, h, mips);
    if (!why.empty()) {
      reject("create_texture", why);
      return 0;
    }
    WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
    td.size = {w, h, 1};
    td.format = WGPUTextureFormat_RGBA8Unorm;
    td.mipLevelCount = mips;
    td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    Sampled s;
    s.texture = wgpuDeviceCreateTexture(dev_, &td);
    s.view = wgpuTextureCreateView(s.texture, nullptr);
    TextureId id = reg_.add_texture({TexKind::Sampled, w, h, mips, false});
    sampled_[id] = s;
    return id;
  }

  bool write_texture(TextureId t, uint32_t mip, const void* rgba, size_t bytes) override {
    std::string why = reg_.check_write_texture(t, mip, bytes, rgba == nullptr);
    if (!why.empty()) {
      reject("write_texture", why);
      return false;
    }
    const detail::TexInfo& ti = reg_.textures.at(t);
    const uint32_t w = detail::mip_dim(ti.width, mip), h = detail::mip_dim(ti.height, mip);
    WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    dst.texture = sampled_.at(t).texture;
    dst.mipLevel = mip;
    WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
    layout.bytesPerRow = w * 4;
    layout.rowsPerImage = h;
    WGPUExtent3D ext = {w, h, 1};
    wgpuQueueWriteTexture(queue_, &dst, rgba, bytes, &layout, &ext);
    return true;
  }

  SamplerId create_sampler(const SamplerDesc& s) override {
    std::string why = Registry::check_sampler(s);
    if (!why.empty()) {
      reject("create_sampler", why);
      return 0;
    }
    WGPUSamplerDescriptor sd = WGPU_SAMPLER_DESCRIPTOR_INIT;
    sd.addressModeU = addr(s.u);
    sd.addressModeV = addr(s.v);
    sd.addressModeW = WGPUAddressMode_Repeat;
    sd.magFilter = s.mag == FilterMode::Linear ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
    sd.minFilter = s.min == FilterMode::Linear ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
    sd.mipmapFilter = s.mip == FilterMode::Linear ? WGPUMipmapFilterMode_Linear : WGPUMipmapFilterMode_Nearest;
    sd.maxAnisotropy = s.max_anisotropy;
    SamplerId id = reg_.add_sampler(s);
    samplers_[id] = wgpuDeviceCreateSampler(dev_, &sd);
    return id;
  }

  PipelineId create_mesh_pipeline(const MeshPipelineDesc& d) override {
    std::string why = Registry::check_mesh_pipeline(d);
    if (!why.empty()) {
      reject("create_mesh_pipeline", why);
      return 0;
    }
    Pipeline p;
    p.mesh = true;
    p.mesh_desc = d;
    p.module = shader(d.wgsl);
    p.layout = mesh_layout(static_cast<uint32_t>(uniform_stride(d.uniform_bytes)), detail::kMeshSlots);  // <= 65536: validated
    WGPUPipelineLayoutDescriptor pl = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    pl.bindGroupLayoutCount = 1;
    pl.bindGroupLayouts = &p.layout;
    p.pipeline_layout = wgpuDeviceCreatePipelineLayout(dev_, &pl);
    ++stats_.pipelines_total;
    PipelineId id = reg_.add_pipeline({true, d.uniform_bytes, detail::kMeshSlots});
    pipelines_[id] = p;
    return id;
  }

  BindGroupId create_bind_group(const BindGroupDesc& d) override {
    std::string why = reg_.check_bind_group(d);
    if (!why.empty()) {
      reject("create_bind_group", why);
      return 0;
    }
    const uint32_t stride = static_cast<uint32_t>(uniform_stride(d.uniform_bytes)), slots = detail::bind_slots(d);
    std::vector<WGPUBindGroupEntry> e;  // one entry per populated slot, matching mesh_layout(stride, slots)
    if (slots & detail::kSlotUniforms) {
      WGPUBindGroupEntry x = WGPU_BIND_GROUP_ENTRY_INIT;
      x.binding = 0;
      x.buffer = buffers_.at(d.uniforms);
      x.size = stride;  // the whole slot: offset + stride is validated against the buffer
      e.push_back(x);
    }
    if (slots & detail::kSlotTexture) {
      WGPUBindGroupEntry x = WGPU_BIND_GROUP_ENTRY_INIT;
      x.binding = 1;
      x.textureView = sampled_.at(d.texture).view;
      e.push_back(x);
    }
    if (slots & detail::kSlotSampler) {
      WGPUBindGroupEntry x = WGPU_BIND_GROUP_ENTRY_INIT;
      x.binding = 2;
      x.sampler = samplers_.at(d.sampler);
      e.push_back(x);
    }
    WGPUBindGroupDescriptor bd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bd.layout = mesh_layout(stride, slots);
    bd.entryCount = e.size();
    bd.entries = e.data();
    const BindGroupId id = reg_.add_bind_group({d, stride, slots});
    mesh_bind_groups_[id] = wgpuDeviceCreateBindGroup(dev_, &bd);
    return id;
  }

  void draw_indexed(const DrawIndexedDesc& d) override {
    if (!frame_active_) {
      if (!frame_open_) reject("draw_indexed", "outside a frame");
      return;
    }
    std::string why = reg_.check_draw_indexed(d);
    if (!why.empty()) return reject("draw_indexed", why);
    Pipeline& p = pipelines_.at(d.pipeline);
    wgpuRenderPassEncoderSetPipeline(pass_, variant(p, pass_format_, pass_depth_));
    wgpuRenderPassEncoderSetBindGroup(pass_, 0, mesh_bind_groups_.at(d.bind_group), 1, &d.uniform_offset);
    WGPUBuffer vb = buffers_.at(d.vertices), ib = buffers_.at(d.indices);
    wgpuRenderPassEncoderSetVertexBuffer(pass_, 0, vb, 0, WGPU_WHOLE_SIZE);
    wgpuRenderPassEncoderSetIndexBuffer(pass_, ib, WGPUIndexFormat_Uint32, 0, WGPU_WHOLE_SIZE);
    wgpuRenderPassEncoderDrawIndexed(pass_, d.index_count, 1, d.first_index, 0, 0);
    ++stats_.draws;
    stats_.indices += d.index_count;
  }

  // ---- M1b ----
  bool destroy_texture(TextureId t) override {
    std::string why = reg_.check_destroy_texture(t);
    if (!why.empty()) return reject("destroy_texture", why), false;
    auto ti = targets_.find(t);
    if (ti != targets_.end()) {
      if (ti->second.depth_view) wgpuTextureViewRelease(ti->second.depth_view);
      if (ti->second.depth) destroy_now_or_after_frame(ti->second.depth);
      if (ti->second.view) wgpuTextureViewRelease(ti->second.view);
      if (ti->second.texture) destroy_now_or_after_frame(ti->second.texture);
      targets_.erase(ti);
    }
    auto si = sampled_.find(t);
    if (si != sampled_.end()) {
      if (si->second.view) wgpuTextureViewRelease(si->second.view);
      if (si->second.texture) destroy_now_or_after_frame(si->second.texture);
      sampled_.erase(si);
    }
    reg_.textures.erase(t);
    return true;
  }
  bool destroy_buffer(BufferId b) override {
    std::string why = reg_.check_destroy_buffer(b);
    if (!why.empty()) return reject("destroy_buffer", why), false;
    // The backend's private M0 bind groups (per pipeline x uniform buffer) are a cache, not rhi objects: released here.
    for (auto it = bind_groups_.begin(); it != bind_groups_.end();) {
      if (static_cast<BufferId>(it->first & 0xffffffffu) == b) {
        wgpuBindGroupRelease(it->second);
        it = bind_groups_.erase(it);
      } else {
        ++it;
      }
    }
    auto bi = buffers_.find(b);  // absent for a uniform buffer registered without storage
    if (bi != buffers_.end()) {
      destroy_now_or_after_frame(bi->second);
      buffers_.erase(bi);
    }
    reg_.buffers.erase(b);
    return true;
  }
  bool destroy_pipeline(PipelineId pid) override {
    std::string why = reg_.check_destroy_pipeline(pid);
    if (!why.empty()) return reject("destroy_pipeline", why), false;
    for (auto it = bind_groups_.begin(); it != bind_groups_.end();) {
      if (static_cast<PipelineId>(it->first >> 32) == pid) {
        wgpuBindGroupRelease(it->second);
        it = bind_groups_.erase(it);
      } else {
        ++it;
      }
    }
    auto pi = pipelines_.find(pid);
    if (pi != pipelines_.end()) {
      Pipeline& p = pi->second;
      for (auto& v : p.by_format) wgpuRenderPipelineRelease(v.second);
      if (p.pipeline_layout) wgpuPipelineLayoutRelease(p.pipeline_layout);
      if (p.layout && !p.mesh) wgpuBindGroupLayoutRelease(p.layout);  // mesh layouts are shared (mesh_layouts_)
      if (p.module) wgpuShaderModuleRelease(p.module);
      pipelines_.erase(pi);
    }
    reg_.pipelines.erase(pid);
    return true;
  }
  bool destroy_sampler(SamplerId s) override {
    std::string why = reg_.check_destroy_sampler(s);
    if (!why.empty()) return reject("destroy_sampler", why), false;
    auto si = samplers_.find(s);
    if (si != samplers_.end()) {
      wgpuSamplerRelease(si->second);
      samplers_.erase(si);
    }
    reg_.samplers.erase(s);
    return true;
  }
  bool destroy_bind_group(BindGroupId g) override {
    std::string why = reg_.check_destroy_bind_group(g);
    if (!why.empty()) return reject("destroy_bind_group", why), false;
    auto gi = mesh_bind_groups_.find(g);
    if (gi != mesh_bind_groups_.end()) {
      wgpuBindGroupRelease(gi->second);
      mesh_bind_groups_.erase(gi);
    }
    reg_.bind_groups.erase(g);
    return true;
  }
  LiveObjects live_objects() const override { return reg_.live(); }

  // T8 P6: a pending loss is delivered from here; a later one from the Dawn callbacks below. Either way the handler must
  // not destroy this device or call back into it (documented on Device::set_lost_handler).
  void set_lost_handler(std::function<void(const std::string&)> h) override {
    if (!lost_) return;
    lost_->handler = std::move(h);
    if (lost_->handler && !lost_->pending.empty()) {
      std::string why = std::move(lost_->pending);
      lost_->pending.clear();
      lost_->handler(why);
    }
  }

  bool set_surface_depth(bool on, float clear_depth) override {
    if (!surface_) return reject("set_surface_depth", "no presentation surface (headless device)"), false;
    if (frame_open_) return reject("set_surface_depth", "inside a frame"), false;
    if (!(clear_depth >= 0.0f && clear_depth <= 1.0f)) return reject("set_surface_depth", "clear_depth outside [0,1]"), false;
    release_surface_depth();
    surface_clear_depth_ = clear_depth;
    if (on) make_surface_depth();
    return true;
  }
  bool resize(uint32_t w, uint32_t h) override {
    if (!surface_) return reject("resize", "no presentation surface (headless device)"), false;
    if (frame_open_) return reject("resize", "inside a frame"), false;
    if (w == 0 || h == 0 || w > kMaxRenderTargetSize || h > kMaxRenderTargetSize) return reject("resize", "size out of range"), false;
    WGPUSurfaceConfiguration cfg = WGPU_SURFACE_CONFIGURATION_INIT;
    cfg.device = dev_;
    cfg.format = surface_format_;
    cfg.width = w;
    cfg.height = h;
    wgpuSurfaceConfigure(surface_, &cfg);
    surface_w_ = w;
    surface_h_ = h;
    if (surface_depth_view_) {
      release_surface_depth();
      make_surface_depth();
    }
    return true;
  }

 private:
  // REF4 r2 (I1): GPU memory goes back at destroy time, not when a JS wrapper is garbage-collected (emdawnwebgpu) or
  // the last reference drops: an explicit Destroy, then Release. Inside an open frame the object may be referenced by
  // the command buffer end_frame() submits, and destroying it before that submit would fail validation, so the Destroy
  // (and the handle) wait for end_frame(). Same semantics for callers: "destroy inside an open frame is allowed".
  void destroy_now_or_after_frame(WGPUTexture t) {
    if (frame_open_) return deferred_textures_.push_back(t);
    wgpuTextureDestroy(t);
    wgpuTextureRelease(t);
  }
  void destroy_now_or_after_frame(WGPUBuffer b) {
    if (frame_open_) return deferred_buffers_.push_back(b);
    wgpuBufferDestroy(b);
    wgpuBufferRelease(b);
  }
  void flush_deferred_destroys() {
    for (WGPUTexture t : deferred_textures_) {
      wgpuTextureDestroy(t);
      wgpuTextureRelease(t);
    }
    for (WGPUBuffer b : deferred_buffers_) {
      wgpuBufferDestroy(b);
      wgpuBufferRelease(b);
    }
    deferred_textures_.clear();
    deferred_buffers_.clear();
  }
  void release_surface_depth() {
    if (surface_depth_view_) wgpuTextureViewRelease(surface_depth_view_);
    if (surface_depth_) wgpuTextureRelease(surface_depth_);
    surface_depth_view_ = nullptr;
    surface_depth_ = nullptr;
    surface_depth_w_ = surface_depth_h_ = 0;
  }
  void make_surface_depth() {
    WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
    td.size = {surface_w_, surface_h_, 1};
    td.format = kDepthFormat;
    td.usage = WGPUTextureUsage_RenderAttachment;
    surface_depth_ = wgpuDeviceCreateTexture(dev_, &td);
    surface_depth_view_ = wgpuTextureCreateView(surface_depth_, nullptr);
    surface_depth_w_ = surface_w_;
    surface_depth_h_ = surface_h_;
  }

  Target make_target(uint32_t w, uint32_t h) {
    WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
    td.size = {w, h, 1};
    td.format = WGPUTextureFormat_RGBA8Unorm;
    td.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
    Target t;
    t.texture = wgpuDeviceCreateTexture(dev_, &td);
    t.view = wgpuTextureCreateView(t.texture, nullptr);
    t.width = w;
    t.height = h;
    return t;
  }

  WGPUShaderModule shader(const std::string& wgsl) {
    WGPUShaderSourceWGSL src = WGPU_SHADER_SOURCE_WGSL_INIT;
    src.code = sv(wgsl);
    WGPUShaderModuleDescriptor smd = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    smd.nextInChain = &src.chain;
    return wgpuDeviceCreateShaderModule(dev_, &smd);
  }

  BufferId make_buffer(BufKind kind, uint32_t bytes, WGPUBufferUsage usage) {
    std::string why = Registry::check_buffer_bytes(bytes);
    if (!why.empty()) {
      reject(kind == BufKind::Vertex ? "create_vertex_buffer" : "create_index_buffer", why);
      return 0;
    }
    WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
    bd.size = (uint64_t(bytes) + 3) / 4 * 4;
    bd.usage = usage | WGPUBufferUsage_CopyDst;
    BufferId id = reg_.add_buffer({kind, bytes});
    buffers_[id] = wgpuDeviceCreateBuffer(dev_, &bd);
    return id;
  }

  // One canonical group-0 layout per (uniform stride, populated slots), shared by every mesh pipeline and bind group
  // with that key, so a bind group is compatible with all of them. M1a-2 has one slot set (kMeshSlots); new optional
  // slots add bits to the key without disturbing existing layouts.
  WGPUBindGroupLayout mesh_layout(uint32_t stride, uint32_t slots) {
    const uint64_t key = (uint64_t(stride) << 32) | slots;
    auto it = mesh_layouts_.find(key);
    if (it != mesh_layouts_.end()) return it->second;
    std::vector<WGPUBindGroupLayoutEntry> e;  // one entry per populated slot, in binding order
    if (slots & detail::kSlotUniforms) {
      WGPUBindGroupLayoutEntry x = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
      x.binding = 0;
      x.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
      x.buffer.type = WGPUBufferBindingType_Uniform;
      x.buffer.hasDynamicOffset = true;
      x.buffer.minBindingSize = stride;
      e.push_back(x);
    }
    if (slots & detail::kSlotTexture) {
      WGPUBindGroupLayoutEntry x = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
      x.binding = 1;
      x.visibility = WGPUShaderStage_Fragment;
      x.texture.sampleType = WGPUTextureSampleType_Float;
      x.texture.viewDimension = WGPUTextureViewDimension_2D;
      e.push_back(x);
    }
    if (slots & detail::kSlotSampler) {
      WGPUBindGroupLayoutEntry x = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
      x.binding = 2;
      x.visibility = WGPUShaderStage_Fragment;
      x.sampler.type = WGPUSamplerBindingType_Filtering;
      e.push_back(x);
    }
    WGPUBindGroupLayoutDescriptor bl = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    bl.entryCount = e.size();
    bl.entries = e.data();
    return mesh_layouts_[key] = wgpuDeviceCreateBindGroupLayout(dev_, &bl);
  }

  WGPUBindGroup bind_group(PipelineId pid, const Pipeline& p, BufferId u) {
    auto key = (uint64_t(pid) << 32) | u;
    auto it = bind_groups_.find(key);
    if (it != bind_groups_.end()) return it->second;
    WGPUBindGroupEntry e = WGPU_BIND_GROUP_ENTRY_INIT;
    e.binding = 0;
    e.buffer = buffers_.at(u);
    e.size = p.desc.uniform_bytes;
    WGPUBindGroupDescriptor bd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bd.layout = p.layout;
    bd.entryCount = 1;
    bd.entries = &e;
    return bind_groups_[key] = wgpuDeviceCreateBindGroup(dev_, &bd);
  }

  void reject(const char* call, const std::string& why) {
    ++stats_.rejected;
    if (rejects_++ < 16) std::fprintf(stderr, "[webgpu] %s rejected: %s\n", call, why.c_str());
  }

  void log_skip_once(const std::string& why) {
    if (logged_skip_) return;
    logged_skip_ = true;
    std::fprintf(stderr, "[webgpu] skipping frame(s): %s\n", why.c_str());
  }

  WGPUInstance inst_;
  WGPUAdapter adapter_;
  WGPUDevice dev_;
  WGPUQueue queue_;
  WGPUSurface surface_;
  WGPUTextureFormat surface_format_;
  std::string adapter_desc_;
  Registry reg_;
  std::vector<WGPUTexture> deferred_textures_;  // REF4 r2 (I1): destroyed after the open frame's submit
  std::vector<WGPUBuffer> deferred_buffers_;
  std::map<TextureId, Target> targets_;
  std::map<TextureId, Sampled> sampled_;
  std::map<BufferId, WGPUBuffer> buffers_;
  std::map<PipelineId, Pipeline> pipelines_;
  std::map<SamplerId, WGPUSampler> samplers_;
  std::map<uint64_t, WGPUBindGroup> bind_groups_;          // M0: implicit, per (pipeline, uniform buffer)
  std::map<BindGroupId, WGPUBindGroup> mesh_bind_groups_;  // M1a-2: explicit
  std::map<uint64_t, WGPUBindGroupLayout> mesh_layouts_;  // key: (uniform stride << 32) | slot mask
  WGPUCommandEncoder enc_ = nullptr;
  WGPURenderPassEncoder pass_ = nullptr;
  WGPUTextureView frame_surface_view_ = nullptr;
  WGPUTexture frame_surface_texture_ = nullptr;
  WGPUTextureFormat pass_format_ = WGPUTextureFormat_RGBA8Unorm;
  bool pass_depth_ = false;
  FrameStats stats_;
  bool frame_active_ = false;
  bool frame_open_ = false;  // M1a-3: between begin_frame and end_frame (null-backend parity for out-of-frame draws)
  bool logged_skip_ = false;
  uint32_t rejects_ = 0;
  // M1b: presentation-surface size and optional depth buffer.
  uint32_t surface_w_ = 0, surface_h_ = 0;
  WGPUTexture surface_depth_ = nullptr;
  WGPUTextureView surface_depth_view_ = nullptr;
  uint32_t surface_depth_w_ = 0, surface_depth_h_ = 0;
  float surface_clear_depth_ = 1.0f;
  LostSink* lost_ = nullptr;  // REF4 r2 (C1): not owned (see LostSink)
};

struct Pending {
  WGPUInstance inst = nullptr;
  WGPUAdapter adapter = nullptr;
  WGPUDevice device = nullptr;  // native: set by the device callback, consumed after the wait
  std::string error;            // native: set by a failing callback, consumed after the wait
  std::string canvas;
  uint32_t width = 0, height = 0;
  DeviceCallback done;
  LostSink* lost = new LostSink();  // REF4 r2 (C1): handed to the device; never freed (see LostSink)
};

// Releases whatever creation acquired so far, reports the error and frees p. Never leaks instance/adapter.
// msg is taken by value: callers pass p->error, which dies with `delete p`.
void fail(Pending* p, std::string msg, WGPUDevice dev = nullptr, WGPUSurface surface = nullptr) {
  if (surface) wgpuSurfaceRelease(surface);
  if (dev) wgpuDeviceRelease(dev);
  if (p->adapter) wgpuAdapterRelease(p->adapter);
  if (p->inst) wgpuInstanceRelease(p->inst);
  DeviceCallback done = std::move(p->done);
  delete p;
  done(nullptr, std::move(msg));
}

void finish(Pending* p, WGPUDevice dev) {
  WGPUSurface surface = nullptr;
  WGPUTextureFormat fmt = WGPUTextureFormat_RGBA8Unorm;
#if defined(__EMSCRIPTEN__)
  if (!p->canvas.empty()) {
    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector cs = WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
    cs.selector = sv(p->canvas);
    WGPUSurfaceDescriptor sd = WGPU_SURFACE_DESCRIPTOR_INIT;
    sd.nextInChain = &cs.chain;
    surface = wgpuInstanceCreateSurface(p->inst, &sd);
    if (!surface) return fail(p, "cannot create surface for canvas " + p->canvas, dev);
    WGPUSurfaceCapabilities caps = WGPU_SURFACE_CAPABILITIES_INIT;
    wgpuSurfaceGetCapabilities(surface, p->adapter, &caps);
    if (caps.formatCount == 0) {
      wgpuSurfaceCapabilitiesFreeMembers(caps);
      return fail(p, "surface reports no supported texture formats", dev, surface);
    }
    fmt = caps.formats[0];
    wgpuSurfaceCapabilitiesFreeMembers(caps);
    WGPUSurfaceConfiguration cfg = WGPU_SURFACE_CONFIGURATION_INIT;
    cfg.device = dev;
    cfg.format = fmt;
    cfg.width = p->width;
    cfg.height = p->height;
    wgpuSurfaceConfigure(surface, &cfg);
  }
#endif
  WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
  wgpuAdapterGetInfo(p->adapter, &info);
  std::string desc = str(info.vendor) + "/" + str(info.architecture) + "/" + str(info.description);
  wgpuAdapterInfoFreeMembers(info);
  auto device = std::make_unique<WebGpuDevice>(p->inst, p->adapter, dev, surface, fmt, desc, p->width, p->height, p->lost);  // owns all handles
  DeviceCallback done = std::move(p->done);
  delete p;
  done(std::move(device), "");
}

const char* error_type_name(WGPUErrorType t) {
  switch (t) {
    case WGPUErrorType_Validation: return "validation";
    case WGPUErrorType_OutOfMemory: return "out-of-memory";
    case WGPUErrorType_Internal: return "internal";
    default: return "unknown";
  }
}

}  // namespace

void create_webgpu_device(const char* canvas_selector, uint32_t width, uint32_t height, DeviceCallback done) {
  auto* p = new Pending;
  p->canvas = canvas_selector ? canvas_selector : "";
  p->width = width;
  p->height = height;
  p->done = std::move(done);
#if defined(__EMSCRIPTEN__)
  p->inst = wgpuCreateInstance(nullptr);
#else
  WGPUInstanceFeatureName feat = WGPUInstanceFeatureName_TimedWaitAny;
  WGPUInstanceDescriptor idesc = WGPU_INSTANCE_DESCRIPTOR_INIT;
  idesc.requiredFeatureCount = 1;
  idesc.requiredFeatures = &feat;
  p->inst = wgpuCreateInstance(&idesc);
#endif
  if (!p->inst) return fail(p, "cannot create WebGPU instance");
  WGPURequestAdapterCallbackInfo acb = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
  acb.mode = LOMN_CB_MODE;
  acb.userdata1 = p;
  acb.callback = [](WGPURequestAdapterStatus st, WGPUAdapter a, WGPUStringView msg, void* u1, void*) {
    auto* p = static_cast<Pending*>(u1);
    if (st != WGPURequestAdapterStatus_Success || !a) {
      p->error = "no WebGPU adapter: " + str(msg);
#if defined(__EMSCRIPTEN__)
      fail(p, p->error);  // browser: callbacks are spontaneous, nothing runs after this
#endif
      return;  // native: the outer function reports the failure after wgpuInstanceWaitAny returns
    }
    p->adapter = a;
    // Visible errors: bad WGSL / validation failures would otherwise be a silent blank frame.
    WGPUDeviceDescriptor dd = WGPU_DEVICE_DESCRIPTOR_INIT;
    dd.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
    dd.deviceLostCallbackInfo.userdata1 = p->lost;
    dd.deviceLostCallbackInfo.callback = [](WGPUDevice const*, WGPUDeviceLostReason reason, WGPUStringView m, void* u1, void*) {
      // Destroyed / CallbackCancelled are the normal teardown path, not a loss.
      if (reason == WGPUDeviceLostReason_Destroyed || reason == WGPUDeviceLostReason_CallbackCancelled) return;
      std::fprintf(stderr, "[webgpu] device lost (reason %d): %s\n", int(reason), str(m).c_str());
      static_cast<LostSink*>(u1)->notify("GPU device lost (reason " + std::to_string(int(reason)) + "): " + str(m));
    };
    dd.uncapturedErrorCallbackInfo.userdata1 = p->lost;
    dd.uncapturedErrorCallbackInfo.callback = [](WGPUDevice const*, WGPUErrorType type, WGPUStringView m, void* u1, void*) {
      std::fprintf(stderr, "[webgpu] uncaptured %s error: %s\n", error_type_name(type), str(m).c_str());
      static_cast<LostSink*>(u1)->notify(std::string("GPU error (") + error_type_name(type) + "): " + str(m));
    };
    WGPURequestDeviceCallbackInfo dcb = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    dcb.mode = LOMN_CB_MODE;
    dcb.userdata1 = p;
    dcb.callback = [](WGPURequestDeviceStatus st2, WGPUDevice d, WGPUStringView msg2, void* u, void*) {
      auto* p2 = static_cast<Pending*>(u);
      if (st2 != WGPURequestDeviceStatus_Success || !d) {
        p2->error = "no WebGPU device: " + str(msg2);
#if defined(__EMSCRIPTEN__)
        fail(p2, p2->error);
#endif
        return;
      }
#if defined(__EMSCRIPTEN__)
      finish(p2, d);
#else
      p2->device = d;
#endif
    };
    WGPUFuture f = wgpuAdapterRequestDevice(a, &dd, dcb);
#if !defined(__EMSCRIPTEN__)
    WGPUFutureWaitInfo w = WGPU_FUTURE_WAIT_INFO_INIT;
    w.future = f;
    const WGPUWaitStatus ws = wgpuInstanceWaitAny(p->inst, 1, &w, UINT64_MAX);
    if (ws != WGPUWaitStatus_Success && !p->device && p->error.empty())
      p->error = "WebGPU device request did not complete (wait status " + std::to_string(int(ws)) + ")";
#else
    (void)f;
#endif
  };
#if !defined(__EMSCRIPTEN__)
  // Native debugging/test override: LOMN_WEBGPU_BACKEND=metal|vulkan|d3d12|d3d11|opengl|null forces the adapter backend
  // (e.g. d3d12 on macOS yields "no adapter", which makes the failure path testable). Unset or unknown = default.
  WGPURequestAdapterOptions aopts = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
  const WGPURequestAdapterOptions* aopts_ptr = nullptr;
  if (const char* be = std::getenv("LOMN_WEBGPU_BACKEND")) {
    const std::string v = be;
    WGPUBackendType bt = WGPUBackendType_Undefined;
    if (v == "metal") bt = WGPUBackendType_Metal;
    else if (v == "vulkan") bt = WGPUBackendType_Vulkan;
    else if (v == "d3d12") bt = WGPUBackendType_D3D12;
    else if (v == "d3d11") bt = WGPUBackendType_D3D11;
    else if (v == "opengl") bt = WGPUBackendType_OpenGL;
    else if (v == "null") bt = WGPUBackendType_Null;
    if (bt == WGPUBackendType_Undefined) {
      if (!v.empty()) std::fprintf(stderr, "[webgpu] ignoring unknown LOMN_WEBGPU_BACKEND=%s\n", v.c_str());
    } else {
      aopts.backendType = bt;
      aopts_ptr = &aopts;
    }
  }
  WGPUFuture f = wgpuInstanceRequestAdapter(p->inst, aopts_ptr, acb);
#else
  WGPUFuture f = wgpuInstanceRequestAdapter(p->inst, nullptr, acb);
#endif
#if !defined(__EMSCRIPTEN__)
  WGPUFutureWaitInfo w = WGPU_FUTURE_WAIT_INFO_INIT;
  w.future = f;
  const WGPUWaitStatus ws = wgpuInstanceWaitAny(p->inst, 1, &w, UINT64_MAX);
  if (p->device) return finish(p, p->device);
  if (p->error.empty())
    p->error = "WebGPU adapter request did not complete (wait status " + std::to_string(int(ws)) + ")";
  fail(p, p->error);
#else
  (void)f;
#endif
}

}  // namespace lomn::rhi
