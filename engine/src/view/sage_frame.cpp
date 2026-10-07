#include "lomn/view/sage_frame.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

#include "lomn/view/area.h"

namespace lomn::view {
namespace {

using namespace lomn::d3d;

// x87 ROUND (round to nearest even), as the exe's (int)ROUND(...) conversions.
uint32_t round_u8(float v) {  // NaN (hostile textures.slb transparency) -> 0, never an out-of-range conversion
  const double r = std::nearbyint(static_cast<double>(v));
  return r >= 255 ? 255u : r > 0 ? static_cast<uint32_t>(r) : 0u;
}
uint32_t id_value(const std::string& id) {  // the 4CC as the engine holds it: first character in the top byte
  uint32_t v = 0;
  for (size_t i = 0; i < 4; ++i) v = (v << 8) | (i < id.size() ? static_cast<uint8_t>(id[i]) : 0u);
  return v;
}
constexpr uint32_t kIdPlnt = 0x706c6e74u, kIdTre1 = 0x74726531u;  // "plnt", "tre1" (string @0x0072a55c)
// GcTransDirector::SortIt's ids: the exe bytes at 0x0070b060 / 0x0070b064 are "1yks" / "rtaw", i.e. the little-endian
// u32s 0x736b7931 / 0x77617472 = "sky1" / "watr" in id_value's convention (the same as "tnlp" -> kIdPlnt). Task 8A E1:
// these were the big-endian reads 0x31796b73 / 0x72746177, which no real id matches (9/123 real areas mis-ordered).
constexpr uint32_t kIdSky1 = 0x736b7931u, kIdWatr = 0x77617472u;

}  // namespace

void SageFrame::init_device() {
  Device8& d = dev_;
  d.note("init: ScPlatformScreen::CreateDirectXWindow 0x0052e570");
  d.set_rs(rs::ZENABLE, kZbUseW);
  d.set_rs(rs::ZFUNC, kCmpLessEqual);
  d.set_rs(rs::SRCBLEND, kBlendSrcAlpha);
  d.set_rs(rs::DESTBLEND, kBlendInvSrcAlpha);
  d.set_rs(rs::DITHERENABLE, 0);
  d.set_tss(0, tss::COLOROP, kTopModulate);
  d.set_tss(0, tss::COLORARG1, kTaTexture);
  d.set_tss(0, tss::COLORARG2, kTaDiffuse);
  d.set_tss(0, tss::ALPHAARG2, kTaTexture);
  d.set_tss(1, tss::TEXCOORDINDEX, 0);
  d.set_tss(1, tss::COLORARG1, kTaTexture);
  d.set_tss(1, tss::COLORARG2, kTaCurrent);
  d.note("init: AppMain 0x005381f0");
  d.set_tss(0, tss::MIPFILTER, kTexfLinear);  // TextureCaps check passes on every DX8 card (assumed cap)
  if (caps_.fog_table) {
    d.set_rs(rs::FOGVERTEXMODE, kFogNone);
    d.set_rs(rs::FOGTABLEMODE, kFogLinear);
    d.set_rs(rs::RANGEFOGENABLE, 0);
  }
  d.set_rs(rs::LIGHTING, 1);
  d.set_rs(rs::AMBIENT, 0xff000000u);  // SrSetGlobalAmbientLight(kBlackRGBAFloats) 0x00534f10 (alpha INFERRED 1)
  d.set_rs(rs::SHADEMODE, 2);
  d.set_tss(0, tss::MAGFILTER, kTexfAnisotropic);
  d.set_tss(0, tss::MINFILTER, kTexfAnisotropic);
  d.note("init: GcGame::SetUpDatabase 0x004399e0 (DAT_007032ac = 1): SrSetGlobalAmbientLight(0.1, 0.1, 0.15, 1)");
  d.set_rs(rs::AMBIENT, (255u << 24) | (round_u8(255.0f * 0.1f) << 16) | (round_u8(255.0f * 0.1f) << 8) | round_u8(255.0f * 0.15f));
  d.note("init: loading screen HUD view (GcAreaLoader::LoadArea): ScRenderer::Start");
  push_render_mode(kModeHud);
}

void SageFrame::set_fog_value(uint32_t color, float start, float end, float density) {
  fog_color_ = color;
  fog_start_ = start;
  fog_end_ = end;
  fog_density_ = density;
}

void SageFrame::push_render_mode(uint32_t m) {
  Device8& d = dev_;
  d.note("SrPushRenderMode 0x" + [&] {
    char b[16];
    std::snprintf(b, sizeof b, "%05x", m);
    return std::string(b);
  }());
  if ((m & 0x80000) == 0) {
    if (last_mode_ & 0x80000) {  // leaving a sprite mode
      d.dx_set_rs(rs::ALPHABLENDENABLE, 0);
      d.dx_set_rs(rs::ALPHAREF, 1);
      d.dx_set_rs(rs::ALPHATESTENABLE, 0);
      d.dx_set_rs(rs::ZWRITEENABLE, 1);
    }
  } else if ((last_mode_ & 0x80000) == 0) {  // entering a sprite mode
    d.dx_set_tss(0, tss::ALPHAOP, kTopSelectArg1);
    d.dx_set_tss(0, tss::ALPHAARG1, kTaTexture);
    d.dx_set_rs(rs::ALPHABLENDENABLE, 1);
    d.dx_set_rs(rs::SRCBLEND, kBlendSrcAlpha);
    d.dx_set_rs(rs::DESTBLEND, kBlendInvSrcAlpha);
    d.dx_set_rs(rs::ALPHAREF, 0);
    d.dx_set_rs(rs::ALPHATESTENABLE, 1);
    d.dx_set_rs(rs::ZWRITEENABLE, 0);
    d.dx_set_tss(0, tss::COLOROP, kTopModulate);
    d.dx_set_tss(0, tss::COLORARG1, kTaTexture);
  }
  if ((m & 0x40000) == 0) {
    if (last_mode_ & 0x40000) {
      d.dx_set_tss(1, tss::COLOROP, kTopDisable);
      d.dx_set_tss(1, tss::ALPHAOP, kTopDisable);
    }
  } else if ((last_mode_ & 0x40000) == 0) {
    d.dx_set_tss(1, tss::COLORARG1, kTaTexture);
    d.dx_set_tss(1, tss::COLOROP, 5);  // D3DTOP_MODULATE2X (caustics stage)
    d.dx_set_tss(1, tss::COLORARG2, kTaCurrent);
    d.dx_set_tss(1, tss::ALPHAOP, kTopDisable);
  }
  if ((m & 0x10000) == 0 || !caps_.can_use_w) {
    if (caps_.can_use_w) {
      if (m & 0x8000) d.dx_set_rs(rs::ZENABLE, kZbFalse);
    } else {
      d.dx_set_rs(rs::ZENABLE, kZbTrue);
    }
  } else {
    d.dx_set_rs(rs::ZENABLE, kZbUseW);
  }
  if (m & 0x2000) d.dx_set_rs(rs::FILLMODE, kFillWireframe);
  else if (m & 0x4000) d.dx_set_rs(rs::FILLMODE, kFillSolid);
  d.dx_set_rs(rs::LIGHTING, (m & 1) ? 1u : 0u);
  d.dx_set_rs(rs::CULLMODE, (m & 0x400) ? kCullCW : (m & 0x800) ? kCullCCW : kCullNone);
  const bool can_fog = caps_.fog_table || caps_.fog_vertex;
  const bool fog_rs = d.get_rs(rs::FOGENABLE) != 0;  // GetRenderState(FOGENABLE) straight from the device
  if ((m & 0x200) == 0 || !can_fog) {
    if (fog_rs) set_fog(false);
  } else if (!fog_rs) {
    set_fog(true);
  }
  last_mode_ = m;
}

void SageFrame::set_fog(bool on) {
  Device8& d = dev_;
  if (caps_.fog_table || caps_.fog_vertex) {
    if (fog_on_ == on) return;
    fog_on_ = on;
  } else {
    fog_on_ = false;
  }
  if (!fog_on_) {
    d.dx_set_rs(rs::FOGENABLE, 0);
    return;
  }
  if (caps_.fog_table) {
    d.dx_set_rs(rs::FOGENABLE, 1);
    d.dx_set_rs(rs::FOGVERTEXMODE, kFogNone);
    d.dx_set_rs(rs::FOGTABLEMODE, kFogLinear);
    d.dx_set_rs(rs::RANGEFOGENABLE, 0);
  } else if (caps_.fog_vertex) {
    d.dx_set_rs(rs::FOGENABLE, 1);
    d.dx_set_rs(rs::FOGTABLEMODE, kFogNone);
    d.dx_set_rs(rs::FOGVERTEXMODE, kFogLinear);
    d.dx_set_rs(rs::RANGEFOGENABLE, 0);
  } else {
    return;
  }
  // The float statics go to the device as their bit patterns (0x005e511e: fld/fstp/mov, no conversion).
  d.dx_set_rs(rs::FOGCOLOR, fog_color_);
  d.dx_set_rs(rs::FOGSTART, float_to_bits(fog_start_));
  d.dx_set_rs(rs::FOGEND, float_to_bits(fog_end_));
  d.dx_set_rs(rs::FOGDENSITY, float_to_bits(fog_density_));
}

void SageFrame::material_set_texture(const AreaData& a, uint32_t model, uint32_t subset, float transparency) {
  Device8& d = dev_;
  const SubsetDraw& sd = a.models[model].subsets[subset];
  // Texture transform: SetUVOffset speeds 0 -> TTF DISABLE; else TTF COUNT2 with the scroll matrix. The first
  // SetTexture of a material starts its clock (mStartTime == -1), so the first frame's matrix is the identity; the
  // viewer's clock never advances (spec M1: static offset only), so it stays the identity.
  if (sd.uv_offset[0] == 0.0f && sd.uv_offset[1] == 0.0f) {
    d.dx_set_tss(0, tss::TEXTURETRANSFORMFLAGS, kTtfDisable);
  } else {
    d.dx_set_tss(0, tss::TEXTURETRANSFORMFLAGS, kTtfCount2);
    d.set_transform(kTsTexture0, mat_identity());
  }
  if (!sd.textured) return;  // mTexture NULL: no SetTexture, no SetMaterial
  if (transparency == 1.0f && sd.trans < 1.0f) {
    d.dx_set_rs(rs::ALPHABLENDENABLE, 1);
    d.dx_set_tss(0, tss::ALPHAOP, kTopSelectArg2);
    d.dx_set_tss(0, tss::ALPHAARG2, kTaTFactor);
    d.dx_set_rs(rs::TEXTUREFACTOR, round_u8(255.0f * sd.trans) << 24);
  }
  d.set_texture(0, sd.texture == kNoTexture ? 0u : sd.texture + 1);  // ScDrawableTexture::SetTexture 0x00556bc0
  // Model emissive is ScFastColor(0) (ScDrawableModel ctor 0x00552670) and mEmissiveIntensity 1.0: plain SetMaterial.
  d.set_material(sd.material);
}

void SageFrame::draw_attrib_range(const AreaData& a, uint32_t placement, uint32_t from, uint32_t to) {
  const Placement& p = a.placements[placement];
  const Model& m = a.models[p.model];
  // ID3DXMesh::DrawSubset(id) for every id in [from, to]; an id outside the attribute table has no faces and D3DX
  // issues no DrawIndexedPrimitive for it, so only ids with faces reach the device (empty ones are only counted).
  for (uint32_t s = 0; s < m.subsets.size(); ++s) {
    const uint32_t id = m.subsets[s].attrib;
    if (id < from || id > to) continue;
    DrawRef r;
    r.placement = placement;
    r.attrib = id;
    r.subset = s;
    dev_.draw_subset(m.id + "#" + std::to_string(p.record), r);
  }
  empty_subsets_ += (to - from + 1) - [&] {
    uint32_t k = 0;
    for (const SubsetDraw& sd : m.subsets) k += sd.attrib >= from && sd.attrib <= to;
    return k;
  }();
}

void SageFrame::draw_x(const AreaData& a, uint32_t placement, float transparency) {
  Device8& d = dev_;
  const Placement& p = a.placements[placement];
  const Model& m = a.models[p.model];
  d.set_transform(kTsWorld, p.world);  // no hierarchy, matrix count 1
  d.dx_set_rs(rs::SOFTWAREVERTEXPROCESSING, 0);  // GcGraphicsOptions::GetSVP() == false
  if (transparency < 1.0f) {
    d.dx_set_rs(rs::ZBIAS, 16);
    d.dx_set_rs(rs::ALPHABLENDENABLE, 1);
    d.dx_set_tss(0, tss::ALPHAOP, kTopModulate);
    d.dx_set_tss(0, tss::ALPHAARG1, kTaTFactor);
    d.dx_set_tss(0, tss::ALPHAARG2, kTaTexture);
    d.dx_set_rs(rs::TEXTUREFACTOR, round_u8(255.0f * transparency) << 24);
  } else {
    d.dx_set_rs(rs::ZBIAS, 0);
  }
  // Pass 1: subsets whose material is not IsAlphaOn; attribute ids between them are drawn too (empty).
  bool any_alpha = false;
  uint32_t next = 0;
  for (uint32_t s = 0; s < m.subsets.size(); ++s) {
    const SubsetDraw& sd = m.subsets[s];
    if (sd.alpha) {
      next = sd.attrib + 1;
      any_alpha = true;
      continue;
    }
    if (transparency >= 1.0f) {
      d.dx_set_rs(rs::ALPHABLENDENABLE, 0);
      d.dx_set_tss(0, tss::ALPHAOP, kTopDisable);
    }
    material_set_texture(a, p.model, s, transparency);
    if (next <= sd.attrib) draw_attrib_range(a, placement, next, sd.attrib);
    next = std::max(next, sd.attrib + 1);
  }
  if (any_alpha) {
    // Pass 2 (alpha subsets): blend on, alpha = texture alpha, test GREATER ALPHAREF; with the context flag set
    // (GcArea::Render for plnt/tre1) blending is turned off again: alpha test only (R14).
    d.dx_set_rs(rs::ALPHABLENDENABLE, 1);
    if (transparency >= 1.0f) {
      d.dx_set_tss(0, tss::ALPHAOP, kTopSelectArg2);
      d.dx_set_tss(0, tss::ALPHAARG2, kTaTexture);
    }
    d.dx_set_rs(rs::ZBIAS, 0);
    d.dx_set_rs(rs::ALPHATESTENABLE, 1);
    d.dx_set_rs(rs::ALPHAFUNC, kCmpGreater);
    if (ctx_flag_) {
      d.dx_set_rs(rs::ALPHABLENDENABLE, 0);
      d.dx_set_rs(rs::ALPHAFUNC, kCmpGreater);
    }
    next = 0;
    for (uint32_t s = 0; s < m.subsets.size(); ++s) {
      const SubsetDraw& sd = m.subsets[s];
      if (!sd.alpha) {
        next = sd.attrib + 1;
        continue;
      }
      material_set_texture(a, p.model, s, transparency);
      if (next <= sd.attrib) draw_attrib_range(a, placement, next, sd.attrib);
      next = std::max(next, sd.attrib + 1);
    }
    d.dx_set_rs(rs::ZBIAS, 16);
    d.dx_set_rs(rs::ALPHATESTENABLE, 0);
  }
  d.dx_set_rs(rs::ALPHABLENDENABLE, 0);
  d.dx_set_tss(0, tss::ALPHAARG2, kTaTexture);
  d.set_texture(0, 0);
  d.set_texture(1, 0);
}

void SageFrame::area_render(const AreaData& a, uint32_t placement) {
  Device8& d = dev_;
  const Model& m = a.models[a.placements[placement].model];
  d.set_tss(0, tss::MAGFILTER, kTexfLinear);  // straight on the device, every draw
  const uint32_t id = id_value(m.id);
  const bool plant = id == kIdPlnt || id == kIdTre1;
  if (plant) {
    ctx_flag_ = true;
    set_fog(true);
  } else {
    ctx_flag_ = false;
  }
  draw_x(a, placement, 1.0f);  // ScDrawableModel::mTransparency = 1.0 (@0x00737c80)
  if (plant) {
    set_fog(false);
    ctx_flag_ = false;
  }
  // GcArea::SunFlare for area "bech" + id "main" is out of scope (spec §3).
}

void SageFrame::lights_render(const AreaData& a) {
  // GcLight::Render for every light the area added, light ids from ScRenderer::GetLightID (cycles 0..7). INFERRED
  // simplification: the original sorts constant lights into the static node (after the areas) and up to 7 nearest
  // into the moving node (GcLightDirector::GetNearest 0x0048eb40); with LIGHTING off for the area nodes the placement
  // changes no pixel, and the forced-lighting evidence mode needs them before the area draws.
  for (const SceneLight& l : a.lights) {
    if (light_count_ == 8) light_count_ = 0;
    const uint32_t id = light_count_++;
    dev_.set_light(id, l.light);
    dev_.light_enable(id, true);
  }
}

void SageFrame::finish() {
  for (uint32_t i = 0; i < 8; ++i) dev_.light_enable(i, false);
  light_count_ = 0;
}

void SageFrame::frame(const AreaData& a, const Camera& cam, uint32_t frame_index) {
  Device8& d = dev_;
  d.note("frame " + std::to_string(frame_index) + ": ScScreen::RenderLoop");
  d.clear(kClearTarget | kClearZBuffer | (caps_.stencil ? kClearStencil : 0u), 0xff000000u, 0.0f, false);
  d.begin_scene();
  d.note("main view: ScRenderer::Start");
  push_render_mode(kModeMainView);
  // ScRenderer::ClearRect: the main viewport's colour is (0,0,0,0) (GcGame::SetUpView, constants @0x007033a4) and z 1.0
  d.clear(kClearTarget | kClearZBuffer | (caps_.stencil ? kClearStencil : 0u), 0x00000000u, 1.0f, true);
  if (frame_index == 0) d.set_transform(kTsProjection, cam.proj);  // ScPerspectiveCamera::BuildProjectionMatrix (dirty)
  d.set_transform(kTsView, cam.view);                                 // ScRenderer::SetViewTransform 0x00530560
  d.set_transform(kTsProjection, cam.proj);                           // ScRenderer::SetProjectionTransform 0x00530640

  d.note("node MovingLight");
  lights_render(a);

  auto group = [&](const char* name, uint32_t mode) {
    d.note(std::string("node ") + name);
    push_render_mode(mode);
  };
  const uint32_t area_mode = opt_.force_area_lighting ? (kModeArea | 1u) : kModeArea;
  group("MainArea", area_mode);
  for (uint32_t i = 0; i < a.placements.size(); ++i)
    if (a.placements[i].route == sage::Route::Main) area_render(a, i);
  // Nearest: moveables in GcAreaDirector::GetNearest (0x00487e90) order, ascending distance from the camera position
  // (stable insertion). sMaxObjects / sMaxObjectDist are not modelled (every moveable is drawn).
  {
    std::vector<std::pair<float, uint32_t>> mv;
    for (uint32_t i = 0; i < a.placements.size(); ++i)
      if (a.placements[i].route == sage::Route::Moveable) {
        const float* p = a.placements[i].pos;
        const float dx = p[0] - cam.eye[0], dy = p[1] - cam.eye[1], dz = p[2] - cam.eye[2];
        float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (!std::isfinite(dist)) dist = std::numeric_limits<float>::max();
        mv.push_back({dist, i});
      }
    std::stable_sort(mv.begin(), mv.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    for (const auto& e : mv) {
      d.note("moveable " + a.models[a.placements[e.second].model].id);
      draw_x(a, e.second, 1.0f);  // GcMoveableObj::Render 0x005458d0: SetTransparency(mTrans = 1, INFERRED) + DrawX
    }
  }
  group("NoLight", kModeNoLight);
  for (uint32_t i = 0; i < a.placements.size(); ++i)
    if (a.placements[i].route == sage::Route::NoLightMoveable) draw_x(a, i, 1.0f);
  d.note("node StaticLight");
  group("Shadow", kModeShadow);
  group("Main", kModeMainView);
  group("Back", kModeBack);
  for (uint32_t i = 0; i < a.placements.size(); ++i)
    if (a.placements[i].route == sage::Route::Back) area_render(a, i);
  group("Trans", opt_.force_area_lighting ? (kModeTrans | 1u) : kModeTrans);
  // GcTransDirector::SortIt (0x00485bf0): selection by the largest horizontal distance from the camera position to
  // the area's greatest-Y point + its position (far to near); id "sky1" (@0x0070b060) first, "watr" (@0x0070b064)
  // and ids ending in 'w' only when they are the last one left.
  {
    std::vector<uint32_t> rem, sorted;
    for (uint32_t i = 0; i < a.placements.size(); ++i)
      if (a.placements[i].route == sage::Route::Trans) rem.push_back(i);
    while (!rem.empty()) {
      size_t best = rem.size();
      float best_d = 0.0f;  // @0x0070b068
      for (size_t k = 0; k < rem.size(); ++k) {
        const Placement& p = a.placements[rem[k]];
        const Model& m = a.models[p.model];
        const uint32_t id = id_value(m.id);
        if (id == kIdSky1) {
          best = k;
          break;
        }
        if (id == kIdWatr || (id & 0xffu) == 'w') {
          if (rem.size() == 1) {
            best = k;
            break;
          }
          continue;
        }
        const float gx = m.greatest_y[0] + p.pos[0], gz = m.greatest_y[2] + p.pos[2];
        const float dx = gx - cam.eye[0], dz = gz - cam.eye[2];
        const float dist = std::sqrt(dx * dx + dz * dz);
        if (best_d < dist) {
          best = k;
          best_d = dist;
        }
      }
      if (best == rem.size()) {  // the original would keep a stale pick here (no termination); take the first
        d.note("trans sort: no candidate (original would not terminate); taking the first remaining");
        best = 0;
      }
      sorted.push_back(rem[best]);
      rem.erase(rem.begin() + static_cast<std::ptrdiff_t>(best));
    }
    for (uint32_t i : sorted) area_render(a, i);
  }
  d.note("main view: ScRenderer::Finish");
  finish();
  d.note("HUD view: ScRenderer::Start");
  push_render_mode(kModeHud);
  d.note("HUD view: ScRenderer::Finish");
  finish();
  d.end_scene();
}

}  // namespace lomn::view
