// Browser WebGPU (emdawnwebgpu) triangle on #canvas. Device is acquired asynchronously in main();
// the frame loop starts only in the device callback (no Asyncify/JSPI). Publishes window.__lomn.state.
#include <webgpu/webgpu.h>
#include <emscripten/emscripten.h>
#include <cstdio>
#include <cstring>

static WGPUStringView sv(const char* s) { return WGPUStringView{s, strlen(s)}; }
static WGPUInstance gInstance; static WGPUAdapter gAdapter; static WGPUDevice gDevice;
static WGPUSurface gSurface; static WGPURenderPipeline gPipe; static int gFrames;

static void frame() {
  WGPUSurfaceTexture st = WGPU_SURFACE_TEXTURE_INIT;
  wgpuSurfaceGetCurrentTexture(gSurface, &st);
  if (st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
      st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) return;
  WGPUTextureView view = wgpuTextureCreateView(st.texture, nullptr);
  WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(gDevice, nullptr);
  WGPURenderPassColorAttachment ca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
  ca.view = view; ca.loadOp = WGPULoadOp_Clear; ca.storeOp = WGPUStoreOp_Store; ca.clearValue = {0.0, 0.0, 0.25, 1.0};
  WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
  rp.colorAttachmentCount = 1; rp.colorAttachments = &ca;
  WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
  wgpuRenderPassEncoderSetPipeline(pass, gPipe);
  wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
  wgpuRenderPassEncoderEnd(pass);
  WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, nullptr);
  wgpuQueueSubmit(wgpuDeviceGetQueue(gDevice), 1, &cb);
  wgpuTextureViewRelease(view);
  if (++gFrames == 3) EM_ASM({ (window.__lomn = window.__lomn || {}).state = 'drawn'; });
}

static void on_device(WGPURequestDeviceStatus st, WGPUDevice d, WGPUStringView msg, void*, void*) {
  if (st != WGPURequestDeviceStatus_Success) { EM_ASM({ (window.__lomn = window.__lomn || {}).state = 'no-device'; }); return; }
  gDevice = d;
  WGPUEmscriptenSurfaceSourceCanvasHTMLSelector cs = WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
  cs.selector = sv("#canvas");
  WGPUSurfaceDescriptor sd = WGPU_SURFACE_DESCRIPTOR_INIT; sd.nextInChain = &cs.chain;
  gSurface = wgpuInstanceCreateSurface(gInstance, &sd);
  WGPUSurfaceCapabilities caps = WGPU_SURFACE_CAPABILITIES_INIT;
  wgpuSurfaceGetCapabilities(gSurface, gAdapter, &caps);
  WGPUTextureFormat fmt = caps.formats[0];
  WGPUSurfaceConfiguration cfg = WGPU_SURFACE_CONFIGURATION_INIT;
  cfg.device = gDevice; cfg.format = fmt; cfg.width = 64; cfg.height = 64;
  wgpuSurfaceConfigure(gSurface, &cfg);
  const char* wgsl =
      "@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {"
      "  var p = array<vec2f,3>(vec2f(0.0,0.8), vec2f(-0.8,-0.8), vec2f(0.8,-0.8));"
      "  return vec4f(p[i], 0.0, 1.0); }"
      "@fragment fn fs() -> @location(0) vec4f { return vec4f(1.0, 0.5, 0.0, 1.0); }";
  WGPUShaderSourceWGSL src = WGPU_SHADER_SOURCE_WGSL_INIT; src.code = sv(wgsl);
  WGPUShaderModuleDescriptor smd = WGPU_SHADER_MODULE_DESCRIPTOR_INIT; smd.nextInChain = &src.chain;
  WGPUShaderModule sm = wgpuDeviceCreateShaderModule(gDevice, &smd);
  WGPUColorTargetState ct = WGPU_COLOR_TARGET_STATE_INIT; ct.format = fmt;
  WGPUFragmentState fs = WGPU_FRAGMENT_STATE_INIT; fs.module = sm; fs.entryPoint = sv("fs"); fs.targetCount = 1; fs.targets = &ct;
  WGPURenderPipelineDescriptor rpd = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
  rpd.vertex.module = sm; rpd.vertex.entryPoint = sv("vs"); rpd.fragment = &fs;
  gPipe = wgpuDeviceCreateRenderPipeline(gDevice, &rpd);
  emscripten_set_main_loop(frame, 0, false);
}

static void on_adapter(WGPURequestAdapterStatus st, WGPUAdapter a, WGPUStringView, void*, void*) {
  if (st != WGPURequestAdapterStatus_Success) { EM_ASM({ (window.__lomn = window.__lomn || {}).state = 'no-adapter'; }); return; }
  gAdapter = a;
  WGPURequestDeviceCallbackInfo dcb = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
  dcb.mode = WGPUCallbackMode_AllowSpontaneous; dcb.callback = on_device;
  wgpuAdapterRequestDevice(gAdapter, nullptr, dcb);
}

int main() {
  gInstance = wgpuCreateInstance(nullptr);
  WGPURequestAdapterCallbackInfo acb = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
  acb.mode = WGPUCallbackMode_AllowSpontaneous; acb.callback = on_adapter;
  wgpuInstanceRequestAdapter(gInstance, nullptr, acb);
  return 0;  // runtime stays alive (EXIT_RUNTIME=0 default); frames start from on_device
}
