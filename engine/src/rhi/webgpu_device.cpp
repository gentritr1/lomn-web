// WebGPU backend. Same code for native (Dawn, headless render targets + synchronous readback via
// wgpuInstanceWaitAny) and browser (emdawnwebgpu, presents to a canvas; no Asyncify: device creation
// is callback-driven and the caller starts its frame loop from the callback).
#include <webgpu/webgpu.h>

#include <cstdio>
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

  // Releases every owned handle in reverse order of creation (children before the device, then
  // queue, device, adapter, instance).
  ~WebGpuDevice() override {
    if (pass_) {
      wgpuRenderPassEncoderEnd(pass_);
      wgpuRenderPassEncoderRelease(pass_);
    }
    if (enc_) wgpuCommandEncoderRelease(enc_);
    if (frame_surface_view_) wgpuTextureViewRelease(frame_surface_view_);
    if (frame_surface_texture_) wgpuTextureRelease(frame_surface_texture_);
    for (auto& kv : bind_groups_) wgpuBindGroupRelease(kv.second);
    for (auto& kv : pipelines_) {
      Pipeline& p = kv.second;
      for (auto& v : p.by_format) wgpuRenderPipelineRelease(v.second);
      if (p.pipeline_layout) wgpuPipelineLayoutRelease(p.pipeline_layout);
      if (p.layout) wgpuBindGroupLayoutRelease(p.layout);
      if (p.module) wgpuShaderModuleRelease(p.module);
    }
    for (auto& kv : buffers_) wgpuBufferRelease(kv.second);
    for (auto& kv : targets_) {
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
    frame_active_ = false;
    WGPUTextureView view = nullptr;
    if (target == 0) {
      // Skip the frame (draw/end_frame become no-ops) rather than touching a null surface or texture.
      if (!surface_) {
        log_skip_once("no presentation surface (headless device)");
        return;
      }
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
    frame_active_ = true;
  }

  void draw(PipelineId pid, uint32_t n, BufferId u, uint32_t off) override {
    if (!frame_active_) return;
    Pipeline& p = pipelines_.at(pid);
    wgpuRenderPassEncoderSetPipeline(pass_, variant(p, pass_format_));
    if (p.desc.uniform_bytes) wgpuRenderPassEncoderSetBindGroup(pass_, 0, bind_group(pid, p, u), 1, &off);
    wgpuRenderPassEncoderDraw(pass_, n, 1, 0, 0);
    ++stats_.draws;
  }

  void end_frame() override {
    if (!frame_active_) return;
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
    bool mapped = false;
    WGPUBufferMapCallbackInfo mcb = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    mcb.mode = WGPUCallbackMode_WaitAnyOnly;
    mcb.callback = [](WGPUMapAsyncStatus s, WGPUStringView msg, void* u1, void*) {
      *static_cast<bool*>(u1) = s == WGPUMapAsyncStatus_Success;
      if (s != WGPUMapAsyncStatus_Success) std::fprintf(stderr, "[webgpu] readback map failed (status %d): %s\n", int(s), str(msg).c_str());
    };
    mcb.userdata1 = &mapped;
    WGPUFutureWaitInfo w = WGPU_FUTURE_WAIT_INFO_INIT;
    w.future = wgpuBufferMapAsync(buf, WGPUMapMode_Read, 0, bd.size, mcb);
    const WGPUWaitStatus ws = wgpuInstanceWaitAny(inst_, 1, &w, UINT64_MAX);
    bool ok = false;
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
  bool frame_active_ = false;
  bool logged_skip_ = false;
};

struct Pending {
  WGPUInstance inst = nullptr;
  WGPUAdapter adapter = nullptr;
  WGPUDevice device = nullptr;  // native: set by the device callback, consumed after the wait
  std::string error;            // native: set by a failing callback, consumed after the wait
  std::string canvas;
  uint32_t width = 0, height = 0;
  DeviceCallback done;
};

// Releases whatever creation acquired so far, reports the error and frees p. Never leaks instance/adapter.
void fail(Pending* p, const std::string& msg, WGPUDevice dev = nullptr, WGPUSurface surface = nullptr) {
  if (surface) wgpuSurfaceRelease(surface);
  if (dev) wgpuDeviceRelease(dev);
  if (p->adapter) wgpuAdapterRelease(p->adapter);
  if (p->inst) wgpuInstanceRelease(p->inst);
  DeviceCallback done = std::move(p->done);
  delete p;
  done(nullptr, msg);
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
  auto device = std::make_unique<WebGpuDevice>(p->inst, p->adapter, dev, surface, fmt, desc);  // takes ownership of all handles
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
    dd.deviceLostCallbackInfo.callback = [](WGPUDevice const*, WGPUDeviceLostReason reason, WGPUStringView m, void*, void*) {
      // Destroyed / CallbackCancelled are the normal teardown path, not a loss.
      if (reason == WGPUDeviceLostReason_Destroyed || reason == WGPUDeviceLostReason_CallbackCancelled) return;
      std::fprintf(stderr, "[webgpu] device lost (reason %d): %s\n", int(reason), str(m).c_str());
    };
    dd.uncapturedErrorCallbackInfo.callback = [](WGPUDevice const*, WGPUErrorType type, WGPUStringView m, void*, void*) {
      std::fprintf(stderr, "[webgpu] uncaptured %s error: %s\n", error_type_name(type), str(m).c_str());
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
  WGPUFuture f = wgpuInstanceRequestAdapter(p->inst, nullptr, acb);
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
