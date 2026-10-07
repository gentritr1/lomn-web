// Native headless WebGPU (Dawn) triangle -> PNG. Proves: sync device acquisition via WaitAny,
// offscreen render, texture->buffer readback. Exit 0 and prints centre pixel.
#include <webgpu/webgpu.h>
#include <cstdio>
#include <cstring>
#include <vector>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

static WGPUStringView sv(const char* s) { return WGPUStringView{s, strlen(s)}; }

int main(int argc, char** argv) {
  const char* out = argc > 1 ? argv[1] : "triangle.png";
  const uint32_t W = 64, H = 64;
  WGPUInstanceFeatureName feat = WGPUInstanceFeatureName_TimedWaitAny;
  WGPUInstanceDescriptor idesc = WGPU_INSTANCE_DESCRIPTOR_INIT;
  idesc.requiredFeatureCount = 1;
  idesc.requiredFeatures = &feat;
  WGPUInstance instance = wgpuCreateInstance(&idesc);
  if (!instance) { fprintf(stderr, "no instance\n"); return 2; }

  WGPUAdapter adapter = nullptr;
  WGPURequestAdapterCallbackInfo acb = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
  acb.mode = WGPUCallbackMode_WaitAnyOnly;
  acb.callback = [](WGPURequestAdapterStatus st, WGPUAdapter a, WGPUStringView msg, void* u1, void*) {
    if (st == WGPURequestAdapterStatus_Success) *static_cast<WGPUAdapter*>(u1) = a;
    else fprintf(stderr, "adapter: %.*s\n", (int)msg.length, msg.data);
  };
  acb.userdata1 = &adapter;
  WGPUFutureWaitInfo w = WGPU_FUTURE_WAIT_INFO_INIT;
  w.future = wgpuInstanceRequestAdapter(instance, nullptr, acb);
  wgpuInstanceWaitAny(instance, 1, &w, UINT64_MAX);
  if (!adapter) return 3;

  WGPUDevice device = nullptr;
  WGPURequestDeviceCallbackInfo dcb = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
  dcb.mode = WGPUCallbackMode_WaitAnyOnly;
  dcb.callback = [](WGPURequestDeviceStatus st, WGPUDevice d, WGPUStringView msg, void* u1, void*) {
    if (st == WGPURequestDeviceStatus_Success) *static_cast<WGPUDevice*>(u1) = d;
    else fprintf(stderr, "device: %.*s\n", (int)msg.length, msg.data);
  };
  dcb.userdata1 = &device;
  w.future = wgpuAdapterRequestDevice(adapter, nullptr, dcb);
  wgpuInstanceWaitAny(instance, 1, &w, UINT64_MAX);
  if (!device) return 4;
  WGPUQueue queue = wgpuDeviceGetQueue(device);

  const char* wgsl =
      "@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {"
      "  var p = array<vec2f,3>(vec2f(0.0,0.8), vec2f(-0.8,-0.8), vec2f(0.8,-0.8));"
      "  return vec4f(p[i], 0.0, 1.0); }"
      "@fragment fn fs() -> @location(0) vec4f { return vec4f(1.0, 0.5, 0.0, 1.0); }";
  WGPUShaderSourceWGSL src = WGPU_SHADER_SOURCE_WGSL_INIT;
  src.code = sv(wgsl);
  WGPUShaderModuleDescriptor smd = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
  smd.nextInChain = &src.chain;
  WGPUShaderModule sm = wgpuDeviceCreateShaderModule(device, &smd);

  WGPUColorTargetState ct = WGPU_COLOR_TARGET_STATE_INIT;
  ct.format = WGPUTextureFormat_RGBA8Unorm;
  WGPUFragmentState fs = WGPU_FRAGMENT_STATE_INIT;
  fs.module = sm; fs.entryPoint = sv("fs"); fs.targetCount = 1; fs.targets = &ct;
  WGPURenderPipelineDescriptor rpd = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
  rpd.vertex.module = sm; rpd.vertex.entryPoint = sv("vs");
  rpd.fragment = &fs;
  WGPURenderPipeline pipe = wgpuDeviceCreateRenderPipeline(device, &rpd);

  WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
  td.size = {W, H, 1}; td.format = WGPUTextureFormat_RGBA8Unorm;
  td.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
  WGPUTexture tex = wgpuDeviceCreateTexture(device, &td);
  WGPUTextureView view = wgpuTextureCreateView(tex, nullptr);

  const uint32_t rowBytes = 256;  // W*4 = 256 already satisfies 256-byte row alignment
  WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
  bd.size = rowBytes * H; bd.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
  WGPUBuffer readback = wgpuDeviceCreateBuffer(device, &bd);

  WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(device, nullptr);
  WGPURenderPassColorAttachment ca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
  ca.view = view; ca.loadOp = WGPULoadOp_Clear; ca.storeOp = WGPUStoreOp_Store;
  ca.clearValue = {0.0, 0.0, 0.25, 1.0};
  WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
  rp.colorAttachmentCount = 1; rp.colorAttachments = &ca;
  WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
  wgpuRenderPassEncoderSetPipeline(pass, pipe);
  wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
  wgpuRenderPassEncoderEnd(pass);
  WGPUTexelCopyTextureInfo srcInfo = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
  srcInfo.texture = tex;
  WGPUTexelCopyBufferInfo dstInfo = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
  dstInfo.buffer = readback; dstInfo.layout.bytesPerRow = rowBytes; dstInfo.layout.rowsPerImage = H;
  WGPUExtent3D ext = {W, H, 1};
  wgpuCommandEncoderCopyTextureToBuffer(enc, &srcInfo, &dstInfo, &ext);
  WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, nullptr);
  wgpuQueueSubmit(queue, 1, &cb);

  bool mapped = false;
  WGPUBufferMapCallbackInfo mcb = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
  mcb.mode = WGPUCallbackMode_WaitAnyOnly;
  mcb.callback = [](WGPUMapAsyncStatus st, WGPUStringView, void* u1, void*) {
    *static_cast<bool*>(u1) = st == WGPUMapAsyncStatus_Success;
  };
  mcb.userdata1 = &mapped;
  w.future = wgpuBufferMapAsync(readback, WGPUMapMode_Read, 0, rowBytes * H, mcb);
  wgpuInstanceWaitAny(instance, 1, &w, UINT64_MAX);
  if (!mapped) return 5;
  const uint8_t* px = static_cast<const uint8_t*>(wgpuBufferGetConstMappedRange(readback, 0, rowBytes * H));
  const uint8_t* c = px + (H / 2) * rowBytes + (W / 2) * 4;
  printf("centre %u %u %u %u\n", c[0], c[1], c[2], c[3]);
  stbi_write_png(out, W, H, 4, px, rowBytes);
  wgpuBufferUnmap(readback);
  return (c[0] == 255 && c[1] >= 126 && c[1] <= 129 && c[2] == 0) ? 0 : 6;
}
