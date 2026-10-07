// M1a-3: the game-side half of the D3D8 shim. Replays, against a d3d::Device8, the call sequence the decompiled engine
// issues for one frame of an area (every rule below names its function and address). The device records it (A9 state
// log) and its DrawSink turns each DrawSubset into an rhi draw.
//
// Frame (ScScreen::RenderLoop 0x0049d550 -> ScViewPort::Render 0x004a0460 for the main view, then the HUD view, which
// GcGame::SetUpView 0x0043a4d0 moves to the END of the view list):
//   Clear(full target, TARGET|ZBUFFER, 0xff000000, z 0.0) ; BeginScene
//   main view: ScRenderer::Start = SrPushRenderMode(0x10635) ; ScRenderer::ClearRect(viewport, colour of the viewport
//     = 0x00000000, z 1.0 from 0x00730f48) ; ScCamera::Render -> SetTransform(VIEW, PROJECTION)
//     master node children, in GcGame::SetUpDatabase (0x004399e0) push order, each GcRenderNodeGroup pushing its mode
//     through ScRenderer::SetRenderMode (vtbl +0x10, 0x005308e0):
//       MovingLight (lights; plain group) | MainArea 0x14634 {AreaLight, Area (static areas, Show order), Nearest
//       (moveables, nearest first)} | NoLight 0x14634 | StaticLight (plain) | Shadow 0x4400 | Main 0x10635 |
//       Back 0x10234 | Trans 0x10034 {Trans (GcTransDirector::SortIt order), emitters, particles, projectiles}
//     ScRenderer::Finish 0x00530440: LightEnable(0..7, FALSE)
//   HUD view: SrPushRenderMode(0x8c034) (no HUD draws here) ; Finish ; EndScene
// Note: the render mode of the area nodes has bit 0 CLEAR, so D3DRS_LIGHTING is FALSE for every area/back/trans draw
// in normal play (the spec's "render mode 0x11" is the ScRenderNodeGroup constructor flag, not a render mode); lighting
// reaches the area nodes only through GcRenderNodeGroup::SetLighting (0x00516c00) from GcLightDirector::GetNearest
// (0x0048eb40) when sUseSpecialLightColor (night vision) is on. SageFrameOptions::force_area_lighting models that.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "lomn/d3d/device8.h"

namespace lomn::view {

struct AreaData;
struct Camera;

// GcGame::SetUpRenderer (0x004395d0) and GcGame::SetUpDatabase (0x004399e0).
constexpr uint32_t kModeMainView = 0x10635, kModeHud = 0x8c034, kModeBack = 0x10234, kModeArea = 0x14634,
                   kModeNoLight = 0x14634, kModeTrans = 0x10034, kModeShadow = 0x4400;

// Device capabilities the original reads at start-up. Values: table fog (D3DPRASTERCAPS_FOGTABLE: every DX8-class
// card), W-buffer allowed (GcGraphicsOptions::mCanUseW defaults to 1 @0x0074a324; only Intel/ATI vendor ids clear it),
// no stencil-capable depth format (mStencilCaps: formats 0x4b/0x4f/0x49 only), software vertex processing off.
struct SageCaps {
  bool fog_table = true, fog_vertex = false, can_use_w = true, stencil = false;
};

struct SageFrameOptions {
  bool force_area_lighting = false;  // GcRenderNodeGroup::SetLighting(MainArea/MainTrans, true): night-vision state
};

class SageFrame {
 public:
  SageFrame(d3d::Device8& dev, const SageCaps& caps, const SageFrameOptions& opt) : dev_(dev), caps_(caps), opt_(opt) {}
  // ScPlatformScreen::CreateDirectXWindow (0x0052e570) device states, AppMain (0x005381f0) states, then the loading
  // screen's HUD frame (GcAreaLoader::LoadArea shows the HUD while loading): SrPushRenderMode(0x8c034).
  void init_device();
  void set_fog_value(uint32_t color, float start, float end, float density);  // GcGraphicsOptions::SetFogValue 0x005e5450
  // One frame. Textures are device handles (AreaData::textures index + 1).
  void frame(const AreaData& area, const Camera& cam, uint32_t frame_index);

  // Building blocks (public for unit tests).
  void push_render_mode(uint32_t mode);  // SrPushRenderMode 0x00531100
  void set_fog(bool on);                 // GcGraphicsOptions::SetFog 0x005e5090
  void draw_x(const AreaData& area, uint32_t placement, float transparency);  // ScDrawableModel::DrawX 0x005528c0
  void area_render(const AreaData& area, uint32_t placement);                 // GcArea::Render 0x00505840
  bool context_flag() const { return ctx_flag_; }  // ScDrawableContext +0xc (GcArea::Render, DrawX R14)
  uint32_t last_mode() const { return last_mode_; }
  uint64_t empty_subsets() const { return empty_subsets_; }  // DrawSubset calls on attribute ids without faces

 private:
  void material_set_texture(const AreaData& area, uint32_t model, uint32_t subset, float transparency);  // 0x0054f260
  void draw_attrib_range(const AreaData& area, uint32_t placement, uint32_t from, uint32_t to);
  void lights_render(const AreaData& area);  // GcLight::Render 0x00490af0 + ScRenderer::GetLightID 0x005310c0
  void finish();                             // ScRenderer::Finish 0x00530440
  d3d::Device8& dev_;
  SageCaps caps_;
  SageFrameOptions opt_;
  uint32_t last_mode_ = 0;   // gLastRenderMode
  bool fog_on_ = false;      // GcGraphicsOptions::mFogOn
  uint32_t fog_color_ = 0xffffffffu;
  float fog_start_ = 4701.0f, fog_end_ = 4702.0f, fog_density_ = 0.001f;  // statics @0x0074a338
  bool ctx_flag_ = false;
  uint32_t light_count_ = 0;  // gLightCount
  uint64_t empty_subsets_ = 0;
  std::vector<float> start_time_;  // per (model, subset): ScDrawableMaterial::mStartTime, -1 until the first SetTexture
};

}  // namespace lomn::view
