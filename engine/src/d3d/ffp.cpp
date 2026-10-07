#include "lomn/d3d/ffp.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <tuple>

namespace lomn::d3d {

// Uniform block layout (floats):
//   0 wvp mat4 | 16 world mat4 | 32 tex0 mat4 | 48 material diffuse | 52 material ambient | 56 material emissive
//   60 global ambient (RS AMBIENT) | 64 TFACTOR | 68 FOGCOLOR | 72 fog (start, end, density, mode)
//   76 a (lighting, vertex has diffuse, diffuse from vertex, texture transform count2)
//   80 b (colorop, colorarg1, colorarg2, alphaop) | 84 c (alphaarg1, alphaarg2, alpha test enable, alphafunc)
//   88 d (alpharef 0..255, fog enable, texture bound, light count) | 92 e (unused, unused, write alpha 1, unused)
//   96 lights: 8 x [diffuse, ambient, position xyz + type, direction xyz + range, attenuation a0 a1 a2 0]
namespace {
constexpr int kWvp = 0, kWorld = 16, kTex0 = 32, kMatD = 48, kMatA = 52, kMatE = 56, kGAmb = 60, kTf = 64, kFogC = 68,
              kFog = 72, kA = 76, kB = 80, kC = 84, kD = 88, kE = 92, kLights = 96;

const char* kWgsl = R"(// lomn M1a-3 D3D8 fixed-function uber-shader (lomn/d3d/ffp.h). One uniform block per draw.
struct U {
  wvp: mat4x4f,
  world: mat4x4f,
  tex0: mat4x4f,
  mat_diffuse: vec4f,
  mat_ambient: vec4f,
  mat_emissive: vec4f,
  global_ambient: vec4f,
  tfactor: vec4f,
  fog_color: vec4f,
  fog: vec4f,
  a: vec4f,
  b: vec4f,
  c: vec4f,
  d: vec4f,
  e: vec4f,
  lights: array<vec4f, 40>,
}
@group(0) @binding(0) var<uniform> u: U;
@group(0) @binding(1) var tex: texture_2d<f32>;
@group(0) @binding(2) var samp: sampler;
struct VOut {
  @builtin(position) pos: vec4f,
  @location(0) uv: vec2f,
  @location(1) color: vec4f,
  @location(2) eye_w: f32,
}
// D3D row-vector matrices are uploaded as-is: WGSL M * v == D3D v * M.
fn lit(p: vec3f, n: vec3f, vd: vec4f) -> vec4f {
  let wp = (u.world * vec4f(p, 1.0)).xyz;
  let wn = (u.world * vec4f(n, 0.0)).xyz;
  var amb = u.global_ambient.rgb;
  var dif = vec3f(0.0);
  let count = u32(u.d.w);
  for (var i = 0u; i < 8u; i = i + 1u) {
    if (i >= count) {
      break;
    }
    let ld = u.lights[5u * i];
    let la = u.lights[5u * i + 1u];
    let lp = u.lights[5u * i + 2u];
    let dr = u.lights[5u * i + 3u];
    let at = u.lights[5u * i + 4u];
    var dir = vec3f(0.0);
    var att = 1.0;
    if (u32(lp.w) == 3u) {
      let len = length(dr.xyz);
      if (len > 0.0) {
        dir = -dr.xyz / len;
      }
    } else {
      let v = lp.xyz - wp;
      let dist = length(v);
      if (dist > dr.w) {
        continue;
      }
      if (dist > 0.0) {
        dir = v / dist;
      }
      att = 1.0 / max(at.x + at.y * dist + at.z * dist * dist, 1e-20);
    }
    amb = amb + la.rgb * att;
    dif = dif + ld.rgb * (max(dot(wn, dir), 0.0) * att);
  }
  let rgb = clamp(u.mat_emissive.rgb + u.mat_ambient.rgb * amb + vd.rgb * dif, vec3f(0.0), vec3f(1.0));
  return vec4f(rgb, vd.a);
}
@vertex fn vs(@location(0) p: vec3f, @location(1) n: vec3f, @location(2) uv: vec2f, @location(3) c: vec4f) -> VOut {
  var o: VOut;
  o.pos = u.wvp * vec4f(p, 1.0);
  o.eye_w = o.pos.w;
  o.uv = select(uv, (u.tex0 * vec4f(uv, 1.0, 0.0)).xy, u.a.w > 0.5);
  let vd = select(u.mat_diffuse, c, u.a.z > 0.5);
  if (u.a.x > 0.5) {
    o.color = lit(p, n, vd);
  } else {
    o.color = select(vec4f(1.0), c, u.a.y > 0.5);
  }
  return o;
}
fn arg(a: u32, diffuse: vec4f, t: vec4f) -> vec4f {
  switch a {
    case 2u: { return t; }
    case 3u: { return u.tfactor; }
    default: { return diffuse; }
  }
}
fn op(o: u32, a1: vec4f, a2: vec4f, cur: vec4f) -> vec4f {
  switch o {
    case 2u: { return a1; }
    case 3u: { return a2; }
    case 4u: { return a1 * a2; }
    default: { return cur; }
  }
}
fn cmpf(func: u32, a: f32, r: f32) -> bool {
  switch func {
    case 1u: { return false; }
    case 2u: { return a < r; }
    case 3u: { return a == r; }
    case 4u: { return a <= r; }
    case 5u: { return a > r; }
    case 6u: { return a != r; }
    case 7u: { return a >= r; }
    default: { return true; }
  }
}
@fragment fn fs(i: VOut) -> @location(0) vec4f {
  let s = textureSample(tex, samp, i.uv);
  let t = select(vec4f(1.0), s, u.d.z > 0.5);
  let dcol = i.color;
  let col = op(u32(u.b.x), arg(u32(u.b.y), dcol, t), arg(u32(u.b.z), dcol, t), dcol);
  let alp = op(u32(u.b.w), arg(u32(u.c.x), dcol, t), arg(u32(u.c.y), dcol, t), dcol);
  if (u.c.z > 0.5) {
    let a8 = floor(clamp(alp.a, 0.0, 1.0) * 255.0 + 0.5);
    if (!cmpf(u32(u.c.w), a8, u.d.x)) {
      discard;
    }
  }
  var rgb = col.rgb;
  if (u.d.y > 0.5) {
    let w = i.eye_w;
    let mode = u32(u.fog.w);
    var f = 1.0;
    if (mode == 3u) {
      f = clamp((u.fog.y - w) / (u.fog.y - u.fog.x), 0.0, 1.0);
    } else if (mode == 1u) {
      f = clamp(exp(-u.fog.z * w), 0.0, 1.0);
    } else if (mode == 2u) {
      f = clamp(exp(-(u.fog.z * w) * (u.fog.z * w)), 0.0, 1.0);
    }
    rgb = f * rgb + (1.0 - f) * u.fog_color.rgb;
  }
  return vec4f(rgb, select(alp.a, 1.0, u.e.z > 0.5));
}
)";

rhi::CompareFunc cmp_of(uint32_t d3dcmp) {
  if (d3dcmp < 1 || d3dcmp > 8) return rhi::CompareFunc::Always;
  return static_cast<rhi::CompareFunc>(d3dcmp - 1);  // rhi numbers CompareFunc as D3DCMPFUNC - 1
}
bool supported_op(uint32_t o) { return o >= kTopDisable && o <= kTopModulate; }
bool supported_arg(uint32_t a) { return a <= kTaTFactor; }
void put4(float* o, const Color4& c) {
  o[0] = c.r;
  o[1] = c.g;
  o[2] = c.b;
  o[3] = c.a;
}
float fogf(uint32_t bits) { return bits_to_float(bits); }
}  // namespace

bool PipelineKey::operator<(const PipelineKey& o) const {
  return std::make_tuple(blend, depth_write, int(depth_compare), int(cull), depth_bias) <
         std::make_tuple(o.blend, o.depth_write, int(o.depth_compare), int(o.cull), o.depth_bias);
}
std::string PipelineKey::str() const {
  static const char* cmpn[] = {"never", "less", "equal", "lessequal", "greater", "notequal", "greaterequal", "always"};
  static const char* culln[] = {"none", "front", "back"};
  return std::string("blend=") + (blend ? "alpha" : "opaque") + " zwrite=" + (depth_write ? "1" : "0") + " zfunc=" +
         cmpn[int(depth_compare)] + " cull=" + culln[int(cull)] + " bias=" + std::to_string(depth_bias);
}

PipelineKey pipeline_key(const State& s, uint32_t* unsup) {
  uint32_t u = 0;
  PipelineKey k;
  const uint32_t* r = s.rs;
  k.blend = r[rs::ALPHABLENDENABLE] != 0;
  // Task 8B B3: ONE/ZERO (the D3D8 default factors) blends src * 1 + dst * 0 = src: exactly an opaque draw (the written
  // alpha is the stage alpha either way, pack_uniforms), so it is drawn opaque instead of being flagged.
  if (k.blend && r[rs::SRCBLEND] == kBlendOne && r[rs::DESTBLEND] == kBlendZero) k.blend = false;
  if (k.blend && !(r[rs::SRCBLEND] == kBlendSrcAlpha && r[rs::DESTBLEND] == kBlendInvSrcAlpha)) u |= kUnsupBlendFactors;
  if (r[rs::ZENABLE] == kZbFalse) {
    k.depth_write = false;
    k.depth_compare = rhi::CompareFunc::Always;
  } else {  // D3DZB_TRUE, or D3DZB_USEW (a W-buffer): WebGPU has only a depth buffer (documented deviation)
    k.depth_write = r[rs::ZWRITEENABLE] != 0;
    k.depth_compare = cmp_of(r[rs::ZFUNC]);
  }
  // D3D's front face is clockwise; the pipeline uses FrontFace::CW, so D3DCULL_CW (cull clockwise) culls front faces.
  k.cull = r[rs::CULLMODE] == kCullCW ? rhi::CullMode::Front : r[rs::CULLMODE] == kCullCCW ? rhi::CullMode::Back : rhi::CullMode::None;
  // D3DRS_ZBIAS (0..16, larger = nearer) -> a negative constant bias under LESSEQUAL. D3D8 leaves the scale to the
  // driver; one WebGPU depth-bias unit per ZBIAS step is our choice (UNVERIFIED against hardware; static frames never
  // draw with ZBIAS != 0, see REPORT).
  k.depth_bias = -static_cast<int32_t>(std::min<uint32_t>(r[rs::ZBIAS], 16));
  if (r[rs::FILLMODE] != kFillSolid) u |= kUnsupFill;
  if (r[rs::SHADEMODE] == 1) u |= kUnsupFlatShade;  // D3DSHADE_FLAT
  if (r[rs::SPECULARENABLE]) u |= kUnsupSpecular;
  if (s.tss[1][tss::COLOROP] != kTopDisable || s.tss[1][tss::ALPHAOP] != kTopDisable) u |= kUnsupStage1;
  const uint32_t* t = s.tss[0];
  if (!supported_op(t[tss::COLOROP]) || !supported_op(t[tss::ALPHAOP]) || !supported_arg(t[tss::COLORARG1]) ||
      !supported_arg(t[tss::COLORARG2]) || !supported_arg(t[tss::ALPHAARG1]) || !supported_arg(t[tss::ALPHAARG2]))
    u |= kUnsupTexOp;
  if (r[rs::LIGHTING]) {
    for (uint32_t i = 0; i < kMaxLights; ++i) {
      if (!s.light_on[i]) continue;
      const uint32_t ty = s.lights[i].type;
      if (ty == kLightSpot) u |= kUnsupSpot;
      else if (ty != kLightPoint && ty != kLightDirectional) u |= kUnsupLightType;
    }
    if (r[rs::NORMALIZENORMALS]) u |= kUnsupNormalize;
    if (r[rs::DIFFUSEMATERIALSOURCE] != kMcsColor1 || r[rs::AMBIENTMATERIALSOURCE] != kMcsMaterial ||
        r[rs::EMISSIVEMATERIALSOURCE] != kMcsMaterial)
      u |= kUnsupMaterialSource;
  }
  if (unsup) *unsup |= u;
  return k;
}

rhi::MeshPipelineDesc pipeline_desc(const PipelineKey& k) {
  rhi::MeshPipelineDesc d;
  d.wgsl = kWgsl;
  d.vs_entry = "vs";
  d.fs_entry = "fs";
  d.uniform_bytes = kFfpUniformBytes;
  d.vertex_stride = kFfpVertexStride;
  d.attributes = {{rhi::VertexFormat::Float32x3, 0, 0},
                  {rhi::VertexFormat::Float32x3, 12, 1},
                  {rhi::VertexFormat::Float32x2, 24, 2},
                  {rhi::VertexFormat::Float32x4, 32, 3}};
  d.blend = k.blend ? rhi::BlendMode::AlphaBlend : rhi::BlendMode::Opaque;
  d.depth_write = k.depth_write;
  d.depth_compare = k.depth_compare;
  d.cull = k.cull;
  d.front_face = rhi::FrontFace::CW;
  d.depth_bias = k.depth_bias;
  return d;
}

rhi::SamplerDesc sampler_desc(const State& s, uint32_t* unsup) {
  // D3DTEXF_ANISOTROPIC samples like LINEAR when MAXANISOTROPY is 1 (the D3D8 default; the exe never sets it); WebGPU
  // has one maxAnisotropy for both min and mag, so it applies only when both are ANISOTROPIC.
  const uint32_t* t = s.tss[0];
  auto f = [](uint32_t v) { return v == kTexfPoint ? rhi::FilterMode::Nearest : rhi::FilterMode::Linear; };
  rhi::SamplerDesc d;
  d.mag = f(t[tss::MAGFILTER]);
  d.min = f(t[tss::MINFILTER]);
  if (t[tss::MIPFILTER] == kTexfNone && unsup) *unsup |= kUnsupMipNone;
  if ((t[tss::ADDRESSU] != 1 || t[tss::ADDRESSV] != 1) && unsup) *unsup |= kUnsupAddress;  // D3DTADDRESS_WRAP = 1
  d.mip = t[tss::MIPFILTER] == kTexfLinear ? rhi::FilterMode::Linear : rhi::FilterMode::Nearest;
  uint32_t an = t[tss::MAXANISOTROPY];
  an = std::max<uint32_t>(1, std::min<uint32_t>(16, an));
  const bool all_linear = d.mag == rhi::FilterMode::Linear && d.min == rhi::FilterMode::Linear && d.mip == rhi::FilterMode::Linear;
  d.max_anisotropy = static_cast<uint16_t>(t[tss::MAGFILTER] == kTexfAnisotropic && t[tss::MINFILTER] == kTexfAnisotropic && all_linear ? an : 1);
  return d;
}

Color4 argb_to_color(uint32_t v) {
  Color4 c;
  c.a = float((v >> 24) & 255u) / 255.0f;
  c.r = float((v >> 16) & 255u) / 255.0f;
  c.g = float((v >> 8) & 255u) / 255.0f;
  c.b = float(v & 255u) / 255.0f;
  return c;
}
float quantize_unorm8(float x) {
  // NaN (a hostile .x) maps to 0: every comparison is false, so it must not reach the integer conversion.
  const float c = x >= 1.0f ? 1.0f : x > 0.0f ? x : 0.0f;
  return float(static_cast<uint32_t>(c * 255.0f + 0.5f)) / 255.0f;
}

void pack_uniforms(const State& s, const DrawInputs& in, float* o) {
  std::memset(o, 0, kFfpUniformBytes);
  const view::Mat4 wvp = view::mat_mul(view::mat_mul(s.world, s.view), s.proj);
  std::memcpy(o + kWvp, wvp.m, 64);
  std::memcpy(o + kWorld, s.world.m, 64);
  std::memcpy(o + kTex0, s.tex[0].m, 64);
  put4(o + kMatD, s.material.diffuse);
  put4(o + kMatA, s.material.ambient);
  put4(o + kMatE, s.material.emissive);
  put4(o + kGAmb, argb_to_color(s.rs[rs::AMBIENT]));
  put4(o + kTf, argb_to_color(s.rs[rs::TEXTUREFACTOR]));
  put4(o + kFogC, argb_to_color(s.rs[rs::FOGCOLOR]));
  o[kFog + 0] = fogf(s.rs[rs::FOGSTART]);
  o[kFog + 1] = fogf(s.rs[rs::FOGEND]);
  o[kFog + 2] = fogf(s.rs[rs::FOGDENSITY]);
  o[kFog + 3] = float(s.rs[rs::FOGTABLEMODE] != kFogNone ? s.rs[rs::FOGTABLEMODE] : s.rs[rs::FOGVERTEXMODE]);
  const bool lighting = s.rs[rs::LIGHTING] != 0;
  o[kA + 0] = lighting ? 1.0f : 0.0f;
  o[kA + 1] = in.vertex_has_diffuse ? 1.0f : 0.0f;
  o[kA + 2] = in.vertex_has_diffuse && s.rs[rs::COLORVERTEX] && s.rs[rs::DIFFUSEMATERIALSOURCE] == kMcsColor1 ? 1.0f : 0.0f;
  o[kA + 3] = (s.tss[0][tss::TEXTURETRANSFORMFLAGS] & 0xff) == kTtfCount2 ? 1.0f : 0.0f;
  const uint32_t* t = s.tss[0];
  o[kB + 0] = float(t[tss::COLOROP]);
  o[kB + 1] = float(t[tss::COLORARG1] & 0xf);
  o[kB + 2] = float(t[tss::COLORARG2] & 0xf);
  o[kB + 3] = float(t[tss::ALPHAOP]);
  o[kC + 0] = float(t[tss::ALPHAARG1] & 0xf);
  o[kC + 1] = float(t[tss::ALPHAARG2] & 0xf);
  o[kC + 2] = s.rs[rs::ALPHATESTENABLE] ? 1.0f : 0.0f;
  o[kC + 3] = float(s.rs[rs::ALPHAFUNC]);
  o[kD + 0] = float(s.rs[rs::ALPHAREF] & 0xff);
  o[kD + 1] = s.rs[rs::FOGENABLE] ? 1.0f : 0.0f;
  o[kD + 2] = in.texture_bound ? 1.0f : 0.0f;
  o[kE + 2] = s.rs[rs::ALPHABLENDENABLE] ? 0.0f : 1.0f;  // opaque draws write alpha 1 (the back buffer is X8R8G8B8)
  uint32_t n = 0;
  if (lighting)
    for (uint32_t i = 0; i < kMaxLights; ++i) {
      if (!s.light_on[i]) continue;
      const Light8& l = s.lights[i];
      float* L = o + kLights + 20 * n;
      put4(L + 0, l.diffuse);
      put4(L + 4, l.ambient);
      L[8] = l.position[0];
      L[9] = l.position[1];
      L[10] = l.position[2];
      L[11] = float(l.type);
      L[12] = l.direction[0];
      L[13] = l.direction[1];
      L[14] = l.direction[2];
      L[15] = l.range;
      L[16] = l.attenuation0;
      L[17] = l.attenuation1;
      L[18] = l.attenuation2;
      ++n;
    }
  o[kD + 3] = float(n);
}

const char* ffp_wgsl() { return kWgsl; }

Color4 reference_vertex_color(const State& s, bool has_vcol, const Color4& vcol, const float p[3], const float n[3]) {
  if (!s.rs[rs::LIGHTING]) return has_vcol ? vcol : Color4{1, 1, 1, 1};
  const bool from_vertex = has_vcol && s.rs[rs::COLORVERTEX] && s.rs[rs::DIFFUSEMATERIALSOURCE] == kMcsColor1;
  const Color4 vd = from_vertex ? vcol : s.material.diffuse;
  float wp[4];
  view::transform_point(s.world, p, wp);
  float wn[3];
  for (int k = 0; k < 3; ++k) wn[k] = n[0] * s.world.m[0 * 4 + k] + n[1] * s.world.m[1 * 4 + k] + n[2] * s.world.m[2 * 4 + k];
  const Color4 ga = argb_to_color(s.rs[rs::AMBIENT]);
  float amb[3] = {ga.r, ga.g, ga.b}, dif[3] = {0, 0, 0};
  for (uint32_t i = 0; i < kMaxLights; ++i) {
    if (!s.light_on[i]) continue;
    const Light8& l = s.lights[i];
    float dir[3] = {0, 0, 0}, att = 1.0f;
    if (l.type == kLightDirectional) {
      const float len = std::sqrt(l.direction[0] * l.direction[0] + l.direction[1] * l.direction[1] + l.direction[2] * l.direction[2]);
      if (len > 0)
        for (int k = 0; k < 3; ++k) dir[k] = -l.direction[k] / len;
    } else {
      float v[3];
      for (int k = 0; k < 3; ++k) v[k] = l.position[k] - wp[k];
      const float dist = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
      if (dist > l.range) continue;
      if (dist > 0)
        for (int k = 0; k < 3; ++k) dir[k] = v[k] / dist;
      att = 1.0f / std::max(l.attenuation0 + l.attenuation1 * dist + l.attenuation2 * dist * dist, 1e-20f);
    }
    const float nl = std::max(wn[0] * dir[0] + wn[1] * dir[1] + wn[2] * dir[2], 0.0f);
    const float la[3] = {l.ambient.r, l.ambient.g, l.ambient.b}, ld[3] = {l.diffuse.r, l.diffuse.g, l.diffuse.b};
    for (int k = 0; k < 3; ++k) {
      amb[k] += la[k] * att;
      dif[k] += ld[k] * nl * att;
    }
  }
  const float ce[3] = {s.material.emissive.r, s.material.emissive.g, s.material.emissive.b};
  const float ca[3] = {s.material.ambient.r, s.material.ambient.g, s.material.ambient.b};
  const float cd[3] = {vd.r, vd.g, vd.b};
  float out[3];
  for (int k = 0; k < 3; ++k) out[k] = std::min(1.0f, std::max(0.0f, ce[k] + ca[k] * amb[k] + cd[k] * dif[k]));
  return Color4{out[0], out[1], out[2], vd.a};
}

void reference_texcoord(const State& s, const float uv[2], float out[2]) {
  if ((s.tss[0][tss::TEXTURETRANSFORMFLAGS] & 0xff) != kTtfCount2) {
    out[0] = uv[0];
    out[1] = uv[1];
    return;
  }
  // (u, v, 1, 0) * TEXTURE0, row-vector convention (_ij = m[(i-1)*4 + (j-1)]): _41/_42 never contribute.
  const float* m = s.tex[0].m;
  for (int j = 0; j < 2; ++j) out[j] = uv[0] * m[j] + uv[1] * m[4 + j] + m[8 + j];
}

bool reference_fragment(const State& s, const Color4& d, const Color4& texel, bool bound, float w, Color4& out) {
  const Color4 t = bound ? texel : Color4{1, 1, 1, 1};
  const Color4 tf = argb_to_color(s.rs[rs::TEXTUREFACTOR]);
  auto arg = [&](uint32_t a) { return (a & 0xf) == kTaTexture ? t : (a & 0xf) == kTaTFactor ? tf : d; };
  auto op = [&](uint32_t o, const Color4& a1, const Color4& a2) {
    if (o == kTopSelectArg1) return a1;
    if (o == kTopSelectArg2) return a2;
    if (o == kTopModulate) return Color4{a1.r * a2.r, a1.g * a2.g, a1.b * a2.b, a1.a * a2.a};
    return d;
  };
  const uint32_t* ts = s.tss[0];
  const Color4 col = op(ts[tss::COLOROP], arg(ts[tss::COLORARG1]), arg(ts[tss::COLORARG2]));
  const Color4 alp = op(ts[tss::ALPHAOP], arg(ts[tss::ALPHAARG1]), arg(ts[tss::ALPHAARG2]));
  if (s.rs[rs::ALPHATESTENABLE]) {
    const float a8 = std::floor(std::min(1.0f, std::max(0.0f, alp.a)) * 255.0f + 0.5f), r = float(s.rs[rs::ALPHAREF] & 0xff);
    bool pass = true;
    switch (s.rs[rs::ALPHAFUNC]) {
      case kCmpNever: pass = false; break;
      case kCmpLess: pass = a8 < r; break;
      case kCmpEqual: pass = a8 == r; break;
      case kCmpLessEqual: pass = a8 <= r; break;
      case kCmpGreater: pass = a8 > r; break;
      case kCmpNotEqual: pass = a8 != r; break;
      case kCmpGreaterEqual: pass = a8 >= r; break;
      default: break;
    }
    if (!pass) return false;
  }
  out = col;
  out.a = s.rs[rs::ALPHABLENDENABLE] ? alp.a : 1.0f;
  if (s.rs[rs::FOGENABLE]) {
    const uint32_t mode = s.rs[rs::FOGTABLEMODE] != kFogNone ? s.rs[rs::FOGTABLEMODE] : s.rs[rs::FOGVERTEXMODE];
    const float st = fogf(s.rs[rs::FOGSTART]), en = fogf(s.rs[rs::FOGEND]), de = fogf(s.rs[rs::FOGDENSITY]);
    float f = 1.0f;
    if (mode == kFogLinear) f = std::min(1.0f, std::max(0.0f, (en - w) / (en - st)));
    else if (mode == kFogExp) f = std::exp(-de * w);
    else if (mode == kFogExp2) f = std::exp(-(de * w) * (de * w));
    // Task 8B B2: one rule with the GPU shader (and ffp_oracle.py): the factor is clamped to [0, 1] in every mode, so an
    // EXP fog with a negative density (f > 1) draws unfogged instead of overshooting.
    f = std::min(1.0f, std::max(0.0f, f));
    const Color4 fc = argb_to_color(s.rs[rs::FOGCOLOR]);
    out.r = f * out.r + (1 - f) * fc.r;
    out.g = f * out.g + (1 - f) * fc.g;
    out.b = f * out.b + (1 - f) * fc.b;
  }
  return true;
}

}  // namespace lomn::d3d
