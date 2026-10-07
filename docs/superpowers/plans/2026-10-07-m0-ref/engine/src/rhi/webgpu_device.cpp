// WebGPU backend. Same code for native (Dawn, headless render targets + synchronous readback via
// wgpuInstanceWaitAny) and browser (emdawnwebgpu, presents to a canvas; no Asyncify: device creation
// is callback-driven and the caller starts its frame loop from the callback).
#include <webgpu/webgpu.h>

#include <cstring>
#include <map>

#include "lomn/rhi.h"

#if defined(__EMSCRIPTEN__)
#define LOMN_CB_MODE WGPUCallbackMode_AllowSpontaneous
#else
#define LOMN_CB_MODE WGPUCallbackMode_WaitAnyOnly
#endif

namespace lomn::rhi {
namespace {

WGPUStringView sv(const std::string& s) { return WGPUStringView{s.data(), s.size()}; }
std::string str(WGPUStringView v) { return v.data ? std::string(v.data, v.length == WGPU_STRLEN ? std::strlen(v.data) : v.length) : std::string(); }

struct Target {
  WGPUTexture texture = nullptr;
  WGPUTextureView view = nullptr;
  uint32_t width = 0, height = 0;
};
struct Pipeline {
  PipelineDesc desc;
  WGPUShaderModule module = nullptr;
  WGPUBindGroupLayout layout = nullptr;
  WGPUPipelineLayout pipeline_layout = nullptr;
  std::map<WGPUTextureFormat, WGPURenderPipeline> by_format;  // built lazily per render-target format
};

class WebGpuDevice final : public Device {
 public:
  WebGpuDevice(WGPUInstance inst, WGPUAdapter adapter, WGPUDevice dev, WGPUSurface surface, WGPUTextureFormat surface_format,
               std::string adapter_desc)
      : inst_(inst), adapter_(adapter), dev_(dev), queue_(wgpuDeviceGetQueue(dev)), surface_(surface),
        surface_format_(surface_format), adapter_desc_(std::move(adapter_desc)) {}

  std::string backend() const override { return "webgpu"; }
  std::string adapter() const override { return adapter_desc_; }

  TextureId create_render_target(uint32_t w, uint32_t h) override {
    WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
    td.size = {w, h, 1};
    td.format = WGPUTextureFormat_RGBA8Unorm;
    td.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
    Target t;
    t.texture = wgpuDeviceCreateTexture(dev_, &td);
    t.view = wgpuTextureCreateView(t.texture, nullptr);
    t.width = w;
    t.height = h;
    targets_[++next_tex_] = t;
    return next_tex_;
  }

  BufferId create_uniform_buffer(uint32_t bytes) override {
    WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
    bd.size = (bytes + kUniformAlign - 1) / kUniformAlign * kUniformAlign;
    bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    buffers_[++next_buf_] = wgpuDeviceCreateBuffer(dev_, &bd);
    return next_buf_;
  }

  void write_buffer(BufferId b, uint32_t off, const void* data, uint32_t size) override {
    wgpuQueueWriteBuffer(queue_, buffers_.at(b), off, data, size);
  }

  PipelineId create_pipeline(const PipelineDesc& d) override {
    WGPUShaderSourceWGSL src = WGPU_SHADER_SOURCE_WGSL_INIT;
    src.code = sv(d.wgsl);
    WGPUShaderModuleDescriptor smd = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    smd.nextInChain = &src.chain;
    Pipeline p;
    p.desc = d;
    p.module = wgpuDeviceCreateShaderModule(dev_, &smd);
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
    pipelines_[++next_pipe_] = p;
    return next_pipe_;
  }

  WGPURenderPipeline variant(Pipeline& p, WGPUTextureFormat fmt) {
    auto it = p.by_format.find(fmt);
    if (it != p.by_format.end()) return it->second;
    WGPUColorTargetState ct = WGPU_COLOR_TARGET_STATE_INIT;
    ct.format = fmt;
    WGPUFragmentState fs = WGPU_FRAGMENT_STATE_INIT;
    fs.module = p.module;
    fs.entryPoint = sv(p.desc.fs_entry);
    fs.targetCount = 1;
    fs.targets = &ct;
    WGPURenderPipelineDescriptor rpd = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    rpd.layout = p.pipeline_layout;
    rpd.vertex.module = p.module;
    rpd.vertex.entryPoint = sv(p.desc.vs_entry);
    rpd.fragment = &fs;
    return p.by_format[fmt] = wgpuDeviceCreateRenderPipeline(dev_, &rpd);
  }

  void begin_frame(TextureId target, const float c[4]) override {
    stats_.draws = 0;
    WGPUTextureView view = nullptr;
    if (target == 0) {
      WGPUSurfaceTexture st = WGPU_SURFACE_TEXTURE_INIT;
      wgpuSurfaceGetCurrentTexture(surface_, &st);
      frame_surface_texture_ = st.texture;
      frame_surface_view_ = view = wgpuTextureCreateView(st.texture, nullptr);
      pass_format_ = surface_format_;
    } else {
      view = targets_.at(target).view;
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
    pass_ = wgpuCommandEncoderBeginRenderPass(enc_, &rp);
  }

  void draw(PipelineId pid, uint32_t n, BufferId u, uint32_t off) override {
    Pipeline& p = pipelines_.at(pid);
    wgpuRenderPassEncoderSetPipeline(pass_, variant(p, pass_format_));
    if (p.desc.uniform_bytes) wgpuRenderPassEncoderSetBindGroup(pass_, 0, bind_group(pid, p, u), 1, &off);
    wgpuRenderPassEncoderDraw(pass_, n, 1, 0, 0);
    ++stats_.draws;
  }

  void end_frame() override {
    wgpuRenderPassEncoderEnd(pass_);
    wgpuRenderPassEncoderRelease(pass_);
    WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc_, nullptr);
    wgpuQueueSubmit(queue_, 1, &cb);
    wgpuCommandBufferRelease(cb);
    wgpuCommandEncoderRelease(enc_);
    if (frame_surface_view_) {
      wgpuTextureViewRelease(frame_surface_view_);
      wgpuTextureRelease(frame_surface_texture_);  // the surface texture is acquired per frame and must be released
      frame_surface_view_ = nullptr;
      frame_surface_texture_ = nullptr;
    }
  }

  bool read_pixels(TextureId target, std::vector<uint8_t>& rgba) override {
#if defined(__EMSCRIPTEN__)
    (void)target;
    (void)rgba;
    return false;  // browser readback is async; tests screenshot the canvas instead
#else
    const Target& t = targets_.at(target);
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
    bool ok = false;
    WGPUBufferMapCallbackInfo mcb = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    mcb.mode = WGPUCallbackMode_WaitAnyOnly;
    mcb.callback = [](WGPUMapAsyncStatus s, WGPUStringView, void* u1, void*) { *static_cast<bool*>(u1) = s == WGPUMapAsyncStatus_Success; };
    mcb.userdata1 = &ok;
    WGPUFutureWaitInfo w = WGPU_FUTURE_WAIT_INFO_INIT;
    w.future = wgpuBufferMapAsync(buf, WGPUMapMode_Read, 0, bd.size, mcb);
    wgpuInstanceWaitAny(inst_, 1, &w, UINT64_MAX);
    if (ok) {
      const uint8_t* px = static_cast<const uint8_t*>(wgpuBufferGetConstMappedRange(buf, 0, bd.size));
      rgba.resize(size_t(t.width) * t.height * 4);
      for (uint32_t y = 0; y < t.height; ++y) std::memcpy(&rgba[size_t(y) * t.width * 4], px + size_t(y) * row, t.width * 4);
      wgpuBufferUnmap(buf);
    }
    wgpuCommandBufferRelease(cb);
    wgpuCommandEncoderRelease(enc);
    wgpuBufferRelease(buf);
    return ok;
#endif
  }

  FrameStats last_frame_stats() const override { return stats_; }

 private:
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

  WGPUInstance inst_;
  WGPUAdapter adapter_;
  WGPUDevice dev_;
  WGPUQueue queue_;
  WGPUSurface surface_;
  WGPUTextureFormat surface_format_;
  std::string adapter_desc_;
  std::map<TextureId, Target> targets_;
  std::map<BufferId, WGPUBuffer> buffers_;
  std::map<PipelineId, Pipeline> pipelines_;
  std::map<uint64_t, WGPUBindGroup> bind_groups_;
  uint32_t next_tex_ = 0, next_buf_ = 0, next_pipe_ = 0;
  WGPUCommandEncoder enc_ = nullptr;
  WGPURenderPassEncoder pass_ = nullptr;
  WGPUTextureView frame_surface_view_ = nullptr;
  WGPUTexture frame_surface_texture_ = nullptr;
  WGPUTextureFormat pass_format_ = WGPUTextureFormat_RGBA8Unorm;
  FrameStats stats_;
};

struct Pending {
  WGPUInstance inst = nullptr;
  WGPUAdapter adapter = nullptr;
  std::string canvas;
  uint32_t width = 0, height = 0;
  DeviceCallback done;
};

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
    WGPUSurfaceCapabilities caps = WGPU_SURFACE_CAPABILITIES_INIT;
    wgpuSurfaceGetCapabilities(surface, p->adapter, &caps);
    fmt = caps.formats[0];
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
  p->done(std::make_unique<WebGpuDevice>(p->inst, p->adapter, dev, surface, fmt, desc), "");
  delete p;
}

}  // namespace

void create_webgpu_device(const char* canvas_selector, uint32_t width, uint32_t height, DeviceCallback done) {
  auto* p = new Pending{nullptr, nullptr, canvas_selector ? canvas_selector : "", width, height, std::move(done)};
#if defined(__EMSCRIPTEN__)
  p->inst = wgpuCreateInstance(nullptr);
#else
  WGPUInstanceFeatureName feat = WGPUInstanceFeatureName_TimedWaitAny;
  WGPUInstanceDescriptor idesc = WGPU_INSTANCE_DESCRIPTOR_INIT;
  idesc.requiredFeatureCount = 1;
  idesc.requiredFeatures = &feat;
  p->inst = wgpuCreateInstance(&idesc);
#endif
  WGPURequestAdapterCallbackInfo acb = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
  acb.mode = LOMN_CB_MODE;
  acb.userdata1 = p;
  acb.callback = [](WGPURequestAdapterStatus st, WGPUAdapter a, WGPUStringView msg, void* u1, void*) {
    auto* p = static_cast<Pending*>(u1);
    if (st != WGPURequestAdapterStatus_Success) {
      p->done(nullptr, "no WebGPU adapter: " + str(msg));
      delete p;
      return;
    }
    p->adapter = a;
    WGPURequestDeviceCallbackInfo dcb = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    dcb.mode = LOMN_CB_MODE;
    dcb.userdata1 = p;
    dcb.callback = [](WGPURequestDeviceStatus st2, WGPUDevice d, WGPUStringView msg2, void* u, void*) {
      auto* p2 = static_cast<Pending*>(u);
      if (st2 != WGPURequestDeviceStatus_Success) {
        p2->done(nullptr, "no WebGPU device: " + str(msg2));
        delete p2;
        return;
      }
      finish(p2, d);
    };
    WGPUFuture f = wgpuAdapterRequestDevice(a, nullptr, dcb);
#if !defined(__EMSCRIPTEN__)
    WGPUFutureWaitInfo w = WGPU_FUTURE_WAIT_INFO_INIT;
    w.future = f;
    wgpuInstanceWaitAny(p->inst, 1, &w, UINT64_MAX);
#else
    (void)f;
#endif
  };
  WGPUFuture f = wgpuInstanceRequestAdapter(p->inst, nullptr, acb);
#if !defined(__EMSCRIPTEN__)
  WGPUInstance inst = p->inst;  // p may be deleted inside the callbacks
  WGPUFutureWaitInfo w = WGPU_FUTURE_WAIT_INFO_INIT;
  w.future = f;
  wgpuInstanceWaitAny(inst, 1, &w, UINT64_MAX);
#else
  (void)f;
#endif
}

}  // namespace lomn::rhi
