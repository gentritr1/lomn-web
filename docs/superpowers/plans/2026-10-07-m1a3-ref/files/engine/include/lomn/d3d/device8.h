// M1a-3 "faithful states": the subset of IDirect3DDevice8 fixed-function state the area path uses, as a recording state
// machine (no GPU). The game-side code (lomn/view/sage_frame.h) replays the decompiled call sequence against it; every
// call that reaches the device is appended to a deterministic STATE LOG (A9), and each draw hands the current state to
// a DrawSink, which compiles it to rhi (lomn/d3d/ffp.h).
//
// Surface (docs/surface.md, IDirect3DDevice8 slots): SetRenderState 50, GetRenderState 51, SetTextureStageState 63,
// GetTextureStageState 62, SetTransform 37, SetMaterial 42, SetLight 44, LightEnable 46, SetTexture 61, Clear 36,
// BeginScene 34 / EndScene 35, and the draw itself (ID3DXMesh::DrawSubset -> DrawIndexedPrimitive 71).
// The redundancy filter the game puts in front of most state calls (ScDxStates, SfDxStates.cpp: 0x0064b1e0 SetRenderState
// / 0x0064b250 SetTextureStageState read the device value and forward only a change) is dx_set_rs / dx_set_tss below.
// Calls the game makes straight on the device (CreateDirectXWindow, AppMain, GcArea::Render's MAGFILTER, ...) use the
// unfiltered set_rs / set_tss, exactly as in the exe, so the log shows the same redundant writes the device received.
//
// Log lines (stable text; native == wasm32): "RS <NAME> <value>", "TSS<stage> <NAME> <value>", "XFORM <NAME> sum=<hex>",
// "MATERIAL ...", "LIGHT <i> ...", "LIGHTENABLE <i> <0|1>", "TEXTURE <stage> <name|NULL>", "CLEAR ...", "BEGINSCENE",
// "ENDSCENE", "DRAW ..." and "# ..." structure notes. Floats print with %.6g; float-valued render states (FOGSTART/END/
// DENSITY) print the bit pattern and the value.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "lomn/view/math.h"

namespace lomn::d3d {

// D3DRENDERSTATETYPE values (d3d8types.h) used by the game's area path.
namespace rs {
constexpr uint32_t ZENABLE = 7, FILLMODE = 8, SHADEMODE = 9, ZWRITEENABLE = 14, ALPHATESTENABLE = 15, SRCBLEND = 19,
                   DESTBLEND = 20, CULLMODE = 22, ZFUNC = 23, ALPHAREF = 24, ALPHAFUNC = 25, DITHERENABLE = 26,
                   ALPHABLENDENABLE = 27, FOGENABLE = 28, SPECULARENABLE = 29, FOGCOLOR = 34, FOGTABLEMODE = 35,
                   FOGSTART = 36, FOGEND = 37, FOGDENSITY = 38, ZBIAS = 47, RANGEFOGENABLE = 48, TEXTUREFACTOR = 60,
                   LIGHTING = 137, AMBIENT = 139, FOGVERTEXMODE = 140, COLORVERTEX = 141, LOCALVIEWER = 142,
                   NORMALIZENORMALS = 143, DIFFUSEMATERIALSOURCE = 145, SPECULARMATERIALSOURCE = 146,
                   AMBIENTMATERIALSOURCE = 147, EMISSIVEMATERIALSOURCE = 148, SOFTWAREVERTEXPROCESSING = 153;
}
// D3DTEXTURESTAGESTATETYPE values.
namespace tss {
constexpr uint32_t COLOROP = 1, COLORARG1 = 2, COLORARG2 = 3, ALPHAOP = 4, ALPHAARG1 = 5, ALPHAARG2 = 6, TEXCOORDINDEX = 11,
                   ADDRESSU = 13, ADDRESSV = 14, MAGFILTER = 16, MINFILTER = 17, MIPFILTER = 18, MAXANISOTROPY = 21,
                   TEXTURETRANSFORMFLAGS = 24;
}
// Enumerant values.
constexpr uint32_t kTopDisable = 1, kTopSelectArg1 = 2, kTopSelectArg2 = 3, kTopModulate = 4;  // D3DTEXTUREOP
constexpr uint32_t kTaDiffuse = 0, kTaCurrent = 1, kTaTexture = 2, kTaTFactor = 3;              // D3DTA_*
constexpr uint32_t kCmpNever = 1, kCmpLess = 2, kCmpEqual = 3, kCmpLessEqual = 4, kCmpGreater = 5, kCmpNotEqual = 6,
                   kCmpGreaterEqual = 7, kCmpAlways = 8;                                        // D3DCMPFUNC
constexpr uint32_t kCullNone = 1, kCullCW = 2, kCullCCW = 3;                                    // D3DCULL
constexpr uint32_t kBlendZero = 1, kBlendOne = 2, kBlendSrcAlpha = 5, kBlendInvSrcAlpha = 6;    // D3DBLEND
constexpr uint32_t kFogNone = 0, kFogExp = 1, kFogExp2 = 2, kFogLinear = 3;                     // D3DFOGMODE
constexpr uint32_t kTexfNone = 0, kTexfPoint = 1, kTexfLinear = 2, kTexfAnisotropic = 3;        // D3DTEXTUREFILTERTYPE
constexpr uint32_t kZbFalse = 0, kZbTrue = 1, kZbUseW = 2;                                      // D3DZBUFFERTYPE
constexpr uint32_t kFillPoint = 1, kFillWireframe = 2, kFillSolid = 3;                          // D3DFILLMODE
constexpr uint32_t kTtfDisable = 0, kTtfCount2 = 2;                                             // D3DTEXTURETRANSFORMFLAGS
constexpr uint32_t kMcsMaterial = 0, kMcsColor1 = 1, kMcsColor2 = 2;                            // D3DMATERIALCOLORSOURCE
constexpr uint32_t kLightPoint = 1, kLightSpot = 2, kLightDirectional = 3;                      // D3DLIGHTTYPE
constexpr uint32_t kTsView = 2, kTsProjection = 3, kTsTexture0 = 16, kTsTexture1 = 17, kTsWorld = 256;  // D3DTRANSFORMSTATETYPE
constexpr uint32_t kClearTarget = 1, kClearZBuffer = 2, kClearStencil = 4;                       // D3DCLEAR_*
constexpr uint32_t kMaxLights = 8, kStages = 2;

struct Color4 {
  float r = 0, g = 0, b = 0, a = 0;
};
// D3DLIGHT8 field for field.
struct Light8 {
  uint32_t type = kLightDirectional;
  Color4 diffuse, specular, ambient;
  float position[3] = {0, 0, 0}, direction[3] = {0, 0, 0};
  float range = 0, falloff = 0, attenuation0 = 0, attenuation1 = 0, attenuation2 = 0, theta = 0, phi = 0;
};
// D3DMATERIAL8.
struct Material8 {
  Color4 diffuse, ambient, specular, emissive;
  float power = 0;
};

// Everything the shim tracks. rs/tss hold only the slots listed above (others stay 0 and are never read).
struct State {
  uint32_t rs[256] = {};
  uint32_t tss[kStages][32] = {};
  view::Mat4 world, view, proj, tex[kStages];
  Material8 material;
  Light8 lights[kMaxLights];
  bool light_on[kMaxLights] = {};
  uint32_t texture[kStages] = {0, 0};  // 0 = NULL, else a caller texture handle (1-based)
};
// The D3D8 device defaults (DirectX 8.1 SDK, "Render states" / "Texture stage states" default values) for every slot
// the area path reads; transforms are identity, lights disabled, material zero.
State default_state();

std::string rs_name(uint32_t rs);
std::string tss_name(uint32_t t);

// One DrawSubset on the device: what is drawn (the caller's ids) plus the current State.
struct DrawRef {
  uint32_t placement = 0;  // index into the caller's placement list
  uint32_t subset = 0;     // index into the model's subset list
  uint32_t attrib = 0;     // D3DX attribute id (DrawSubset argument)
};
class DrawSink {
 public:
  virtual ~DrawSink() = default;
  virtual void draw(const State& s, const DrawRef& d) = 0;
};

class Device8 {
 public:
  // log may be null (no log kept); names maps texture handle k (1-based) to names[k-1] for TEXTURE lines.
  explicit Device8(std::vector<std::string>* log, const std::vector<std::string>* texture_names = nullptr);
  void set_sink(DrawSink* sink) { sink_ = sink; }

  // ---- IDirect3DDevice8 (unfiltered: every call is logged) ----
  void set_rs(uint32_t state, uint32_t value);
  uint32_t get_rs(uint32_t state) const { return state < 256 ? s_.rs[state] : 0; }
  void set_tss(uint32_t stage, uint32_t type, uint32_t value);
  uint32_t get_tss(uint32_t stage, uint32_t type) const { return stage < kStages && type < 32 ? s_.tss[stage][type] : 0; }
  void set_transform(uint32_t type, const view::Mat4& m);
  void set_material(const Material8& m);
  void set_light(uint32_t index, const Light8& l);
  void light_enable(uint32_t index, bool on);
  void set_texture(uint32_t stage, uint32_t handle);
  // Clear: rect = false for the whole target (ScScreen::RenderLoop), true for a viewport rect (ScRenderer::ClearRect).
  void clear(uint32_t flags, uint32_t argb, float z, bool rect);
  void begin_scene();
  void end_scene();
  // ID3DXMesh::DrawSubset: logs "DRAW ..." and hands the current state to the sink. `what` names the drawn object.
  void draw_subset(const std::string& what, const DrawRef& d);
  void note(const std::string& text);  // "# text" structure line (does not count as a state change)

  // ---- ScDxStates (0x0064b1e0 / 0x0064b250): forward only a change ----
  void dx_set_rs(uint32_t state, uint32_t value) {
    if (get_rs(state) != value) set_rs(state, value);
  }
  void dx_set_tss(uint32_t stage, uint32_t type, uint32_t value) {
    if (get_tss(stage, type) != value) set_tss(stage, type, value);
  }

  const State& state() const { return s_; }
  uint32_t state_calls() const { return state_calls_; }  // state-setting calls that reached the device
  uint32_t draws() const { return draws_; }
  // Clear values of the last whole-viewport clear (what the frame starts from) for the rhi begin_frame.
  uint32_t last_clear_argb() const { return clear_argb_; }
  float last_clear_z() const { return clear_z_; }

 private:
  void rec(const std::string& line, bool is_state);
  State s_;
  std::vector<std::string>* log_;
  const std::vector<std::string>* names_;
  DrawSink* sink_ = nullptr;
  uint32_t state_calls_ = 0, draws_ = 0;
  uint32_t clear_argb_ = 0xff000000u;
  float clear_z_ = 1.0f;
};

float bits_to_float(uint32_t v);
uint32_t float_to_bits(float f);

}  // namespace lomn::d3d
