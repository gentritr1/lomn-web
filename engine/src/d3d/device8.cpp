#include "lomn/d3d/device8.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace lomn::d3d {

float bits_to_float(uint32_t v) {
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
uint32_t float_to_bits(float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  return v;
}

State default_state() {
  State s;
  // Render states (DirectX 8.1 SDK defaults). ZENABLE is D3DZB_TRUE because the game creates the device with an
  // automatic depth-stencil surface (CreateDirectXWindow 0x0052e570, D3DPRESENT_PARAMETERS.EnableAutoDepthStencil = 1).
  s.rs[rs::ZENABLE] = kZbTrue;
  s.rs[rs::FILLMODE] = kFillSolid;
  s.rs[rs::SHADEMODE] = 2;  // D3DSHADE_GOURAUD
  s.rs[rs::ZWRITEENABLE] = 1;
  s.rs[rs::ALPHATESTENABLE] = 0;
  s.rs[rs::SRCBLEND] = kBlendOne;
  s.rs[rs::DESTBLEND] = kBlendZero;
  s.rs[rs::CULLMODE] = kCullCCW;
  s.rs[rs::ZFUNC] = kCmpLessEqual;
  s.rs[rs::ALPHAREF] = 0;
  s.rs[rs::ALPHAFUNC] = kCmpAlways;
  s.rs[rs::DITHERENABLE] = 0;
  s.rs[rs::ALPHABLENDENABLE] = 0;
  s.rs[rs::FOGENABLE] = 0;
  s.rs[rs::SPECULARENABLE] = 0;
  s.rs[rs::FOGCOLOR] = 0;
  s.rs[rs::FOGTABLEMODE] = kFogNone;
  s.rs[rs::FOGSTART] = float_to_bits(0.0f);
  s.rs[rs::FOGEND] = float_to_bits(1.0f);
  s.rs[rs::FOGDENSITY] = float_to_bits(1.0f);
  s.rs[rs::ZBIAS] = 0;
  s.rs[rs::RANGEFOGENABLE] = 0;
  s.rs[rs::TEXTUREFACTOR] = 0xffffffffu;
  s.rs[rs::LIGHTING] = 1;
  s.rs[rs::AMBIENT] = 0;
  s.rs[rs::FOGVERTEXMODE] = kFogNone;
  s.rs[rs::COLORVERTEX] = 1;
  s.rs[rs::LOCALVIEWER] = 1;
  s.rs[rs::NORMALIZENORMALS] = 0;
  s.rs[rs::DIFFUSEMATERIALSOURCE] = kMcsColor1;
  s.rs[rs::SPECULARMATERIALSOURCE] = kMcsColor2;
  s.rs[rs::AMBIENTMATERIALSOURCE] = kMcsMaterial;
  s.rs[rs::EMISSIVEMATERIALSOURCE] = kMcsMaterial;
  s.rs[rs::SOFTWAREVERTEXPROCESSING] = 0;
  // Texture stage states: stage 0 modulates texture and diffuse, alpha = texture; every higher stage is disabled.
  for (uint32_t st = 0; st < kStages; ++st) {
    uint32_t* t = s.tss[st];
    t[tss::COLOROP] = st == 0 ? kTopModulate : kTopDisable;
    t[tss::COLORARG1] = kTaTexture;
    t[tss::COLORARG2] = kTaCurrent;
    t[tss::ALPHAOP] = st == 0 ? kTopSelectArg1 : kTopDisable;
    t[tss::ALPHAARG1] = kTaTexture;
    t[tss::ALPHAARG2] = kTaCurrent;
    t[tss::TEXCOORDINDEX] = st;
    t[tss::ADDRESSU] = 1;  // D3DTADDRESS_WRAP
    t[tss::ADDRESSV] = 1;
    t[tss::MAGFILTER] = kTexfPoint;
    t[tss::MINFILTER] = kTexfPoint;
    t[tss::MIPFILTER] = kTexfNone;
    t[tss::MAXANISOTROPY] = 1;
    t[tss::TEXTURETRANSFORMFLAGS] = kTtfDisable;
  }
  s.world = s.view = s.proj = view::mat_identity();
  for (auto& m : s.tex) m = view::mat_identity();
  return s;
}

std::string rs_name(uint32_t r) {
  switch (r) {
    case rs::ZENABLE: return "ZENABLE";
    case rs::FILLMODE: return "FILLMODE";
    case rs::SHADEMODE: return "SHADEMODE";
    case rs::ZWRITEENABLE: return "ZWRITEENABLE";
    case rs::ALPHATESTENABLE: return "ALPHATESTENABLE";
    case rs::SRCBLEND: return "SRCBLEND";
    case rs::DESTBLEND: return "DESTBLEND";
    case rs::CULLMODE: return "CULLMODE";
    case rs::ZFUNC: return "ZFUNC";
    case rs::ALPHAREF: return "ALPHAREF";
    case rs::ALPHAFUNC: return "ALPHAFUNC";
    case rs::DITHERENABLE: return "DITHERENABLE";
    case rs::ALPHABLENDENABLE: return "ALPHABLENDENABLE";
    case rs::FOGENABLE: return "FOGENABLE";
    case rs::SPECULARENABLE: return "SPECULARENABLE";
    case rs::FOGCOLOR: return "FOGCOLOR";
    case rs::FOGTABLEMODE: return "FOGTABLEMODE";
    case rs::FOGSTART: return "FOGSTART";
    case rs::FOGEND: return "FOGEND";
    case rs::FOGDENSITY: return "FOGDENSITY";
    case rs::ZBIAS: return "ZBIAS";
    case rs::RANGEFOGENABLE: return "RANGEFOGENABLE";
    case rs::TEXTUREFACTOR: return "TEXTUREFACTOR";
    case rs::LIGHTING: return "LIGHTING";
    case rs::AMBIENT: return "AMBIENT";
    case rs::FOGVERTEXMODE: return "FOGVERTEXMODE";
    case rs::COLORVERTEX: return "COLORVERTEX";
    case rs::LOCALVIEWER: return "LOCALVIEWER";
    case rs::NORMALIZENORMALS: return "NORMALIZENORMALS";
    case rs::DIFFUSEMATERIALSOURCE: return "DIFFUSEMATERIALSOURCE";
    case rs::SPECULARMATERIALSOURCE: return "SPECULARMATERIALSOURCE";
    case rs::AMBIENTMATERIALSOURCE: return "AMBIENTMATERIALSOURCE";
    case rs::EMISSIVEMATERIALSOURCE: return "EMISSIVEMATERIALSOURCE";
    case rs::SOFTWAREVERTEXPROCESSING: return "SOFTWAREVERTEXPROCESSING";
    default: return "RS" + std::to_string(r);
  }
}
std::string tss_name(uint32_t t) {
  switch (t) {
    case tss::COLOROP: return "COLOROP";
    case tss::COLORARG1: return "COLORARG1";
    case tss::COLORARG2: return "COLORARG2";
    case tss::ALPHAOP: return "ALPHAOP";
    case tss::ALPHAARG1: return "ALPHAARG1";
    case tss::ALPHAARG2: return "ALPHAARG2";
    case tss::TEXCOORDINDEX: return "TEXCOORDINDEX";
    case tss::ADDRESSU: return "ADDRESSU";
    case tss::ADDRESSV: return "ADDRESSV";
    case tss::MAGFILTER: return "MAGFILTER";
    case tss::MINFILTER: return "MINFILTER";
    case tss::MIPFILTER: return "MIPFILTER";
    case tss::MAXANISOTROPY: return "MAXANISOTROPY";
    case tss::TEXTURETRANSFORMFLAGS: return "TEXTURETRANSFORMFLAGS";
    default: return "TSS" + std::to_string(t);
  }
}

namespace {
std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* f, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, f);
  const int n = std::vsnprintf(buf, sizeof buf, f, ap);
  va_end(ap);
  return std::string(buf, n < 0 ? 0 : (static_cast<size_t>(n) < sizeof buf ? static_cast<size_t>(n) : sizeof buf - 1));
}
// Every float in the state log (Task 8A B2): %.6g for finite values; non-finite values are spelled here, not by the C
// library (macOS prints the x87 default NaN 0xffc00000 as "nan", emscripten/musl as "-nan", which broke the native ==
// wasm32 log contract). Rule: any NaN (sign and payload ignored) -> "nan"; +infinity -> "inf"; -infinity -> "-inf".
std::string g6(double x) {
  if (std::isnan(x)) return "nan";
  if (std::isinf(x)) return x > 0 ? "inf" : "-inf";
  return fmt("%.6g", x);
}
std::string c4(const Color4& c) { return "(" + g6(c.r) + "," + g6(c.g) + "," + g6(c.b) + "," + g6(c.a) + ")"; }
// The M0 checksum (sum = sum * 31 + byte) over a matrix's 64 bytes.
uint32_t mat_sum(const view::Mat4& m) {
  uint32_t sum = 0;
  const uint8_t* p = reinterpret_cast<const uint8_t*>(m.m);
  for (int i = 0; i < 64; ++i) sum = sum * 31u + p[i];
  return sum;
}
std::string ts_name(uint32_t t) {
  switch (t) {
    case kTsView: return "VIEW";
    case kTsProjection: return "PROJECTION";
    case kTsTexture0: return "TEXTURE0";
    case kTsTexture1: return "TEXTURE1";
    case kTsWorld: return "WORLD";
    default: return "T" + std::to_string(t);
  }
}
bool is_float_rs(uint32_t r) { return r == rs::FOGSTART || r == rs::FOGEND || r == rs::FOGDENSITY; }
}  // namespace

Device8::Device8(std::vector<std::string>* log, const std::vector<std::string>* names) : s_(default_state()), log_(log), names_(names) {}

void Device8::rec(const std::string& line, bool is_state) {
  if (is_state) ++state_calls_;
  if (log_) log_->push_back(line);
}

// Task 8B B3: a call outside the tracked slots (render state >= 256, stage >= kStages, stage state >= 32, light index
// >= kMaxLights) changes nothing here; it is logged as a "# dropped ..." note (not a state call) so it is never silent.
void Device8::set_rs(uint32_t r, uint32_t v) {
  if (r >= 256) return rec(fmt("# dropped SetRenderState state=%u value=%u", r, v), false);
  s_.rs[r] = v;
  if (is_float_rs(r)) rec(fmt("RS %s 0x%08x (", rs_name(r).c_str(), v) + g6(bits_to_float(v)) + ")", true);
  else if (r == rs::FOGCOLOR || r == rs::TEXTUREFACTOR || r == rs::AMBIENT) rec(fmt("RS %s 0x%08x", rs_name(r).c_str(), v), true);
  else rec(fmt("RS %s %u", rs_name(r).c_str(), v), true);
}
void Device8::set_tss(uint32_t st, uint32_t t, uint32_t v) {
  if (st >= kStages || t >= 32) return rec(fmt("# dropped SetTextureStageState stage=%u type=%u value=%u", st, t, v), false);
  s_.tss[st][t] = v;
  rec(fmt("TSS%u %s %u", st, tss_name(t).c_str(), v), true);
}
void Device8::set_transform(uint32_t t, const view::Mat4& m) {
  if (t == kTsWorld) s_.world = m;
  else if (t == kTsView) s_.view = m;
  else if (t == kTsProjection) s_.proj = m;
  else if (t == kTsTexture0) s_.tex[0] = m;
  else if (t == kTsTexture1) s_.tex[1] = m;
  rec(fmt("XFORM %s sum=%08x", ts_name(t).c_str(), mat_sum(m)), true);
}
void Device8::set_material(const Material8& m) {
  s_.material = m;
  rec("MATERIAL d=" + c4(m.diffuse) + " a=" + c4(m.ambient) + " s=" + c4(m.specular) + " e=" + c4(m.emissive) + " p=" + g6(m.power), true);
}
void Device8::set_light(uint32_t i, const Light8& l) {
  if (i >= kMaxLights) return rec(fmt("# dropped SetLight index=%u", i), false);
  s_.lights[i] = l;
  light_set_[i] = true;
  rec(fmt("LIGHT %u type=%u", i, l.type) + " d=" + c4(l.diffuse) + " s=" + c4(l.specular) + " a=" + c4(l.ambient) +
          " pos=(" + g6(l.position[0]) + "," + g6(l.position[1]) + "," + g6(l.position[2]) + ") dir=(" + g6(l.direction[0]) +
          "," + g6(l.direction[1]) + "," + g6(l.direction[2]) + ") range=" + g6(l.range) + " att=(" + g6(l.attenuation0) +
          "," + g6(l.attenuation1) + "," + g6(l.attenuation2) + ")",
      true);
}
void Device8::light_enable(uint32_t i, bool on) {
  if (i >= kMaxLights) return rec(fmt("# dropped LightEnable index=%u on=%d", i, on ? 1 : 0), false);
  // Task 8B B3 (DirectX 8.1 SDK, IDirect3DDevice8::LightEnable): an index SetLight never assigned gets the default light,
  // D3DLIGHT_DIRECTIONAL with Diffuse (1,1,1,0), Direction (0,0,1) and every other member 0. The note is logged only when
  // such a light is ENABLED (it then shines): every real area disables never-set indices at init (LightEnable(i, FALSE)
  // on 90 of 123 areas), which has no effect and stays out of the log.
  if (!light_set_[i]) {
    Light8 d;
    d.type = kLightDirectional;
    d.diffuse = Color4{1, 1, 1, 0};
    d.direction[2] = 1.0f;
    s_.lights[i] = d;
    if (on) rec(fmt("# LightEnable %u: never set, D3D8 default light (white directional +Z)", i), false);
  }
  s_.light_on[i] = on;
  rec(fmt("LIGHTENABLE %u %d", i, on ? 1 : 0), true);
}
void Device8::set_texture(uint32_t st, uint32_t h) {
  if (st >= kStages) return rec(fmt("# dropped SetTexture stage=%u", st), false);
  s_.texture[st] = h;
  std::string n = "NULL";
  if (h) n = names_ && h <= names_->size() ? (*names_)[h - 1] : "#" + std::to_string(h);
  rec(fmt("TEXTURE %u ", st) + n, true);
}
void Device8::clear(uint32_t flags, uint32_t argb, float z, bool rect) {
  clear_argb_ = argb;
  clear_z_ = z;
  rec(fmt("CLEAR %s flags=%u color=0x%08x z=", rect ? "viewport" : "full", flags, argb) + g6(z), false);
}
void Device8::begin_scene() { rec("BEGINSCENE", false); }
void Device8::end_scene() { rec("ENDSCENE", false); }
void Device8::draw_subset(const std::string& what, const DrawRef& d) {
  ++draws_;
  rec(fmt("DRAW %s attrib=%u", what.c_str(), d.attrib), false);
  if (sink_) sink_->draw(s_, d);
}
void Device8::note(const std::string& text) { rec("# " + text, false); }

}  // namespace lomn::d3d
