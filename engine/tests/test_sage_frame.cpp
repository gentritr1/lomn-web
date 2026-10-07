// M1a-3: the SAGE frame replay (lomn/view/sage_frame.h) on the D3D8 state shim, and the A10 contract through AreaRenderer.
// Expected values come from the decompiled functions named in each case.
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "doctest.h"
#include "lomn/d3d/device8.h"
#include "lomn/d3d/ffp.h"
#include "lomn/fs.h"
#include "lomn/rhi.h"
#include "lomn/view/area.h"
#include "lomn/view/sage_frame.h"
#include "lomn/view/synthetic.h"

using namespace lomn;
using namespace lomn::d3d;

namespace {

std::vector<std::string> state_lines(const std::vector<std::string>& log) {
  std::vector<std::string> o;
  for (const auto& l : log)
    if (l.rfind("# ", 0) != 0) o.push_back(l);
  return o;
}

view::AreaData synthetic_area() {
  fs::MemoryFileSystem mfs;
  REQUIRE(view::mount_synthetic_install(mfs));
  view::AreaData a;
  std::string err;
  REQUIRE_MESSAGE(view::load_area(mfs, view::kSyntheticLevel, view::kSyntheticStateArea, view::AreaOptions(), a, &err), err);
  return a;
}

}  // namespace

TEST_CASE("sage: SrPushRenderMode carries ALPHAREF across the HUD's 0x80000 mode (R13 history)") {
  std::vector<std::string> log;
  Device8 d(&log);
  view::SageFrame f(d, view::SageCaps(), view::SageFrameOptions());
  f.push_render_mode(view::kModeMainView);  // from gLastRenderMode 0: nothing about ALPHAREF
  CHECK(d.get_rs(rs::ALPHAREF) == 0);
  CHECK(d.get_rs(rs::LIGHTING) == 1);
  CHECK(d.get_rs(rs::CULLMODE) == kCullCW);
  f.push_render_mode(view::kModeHud);  // entering 0x80000: ALPHAREF 0, blend + test on, z write off
  CHECK(d.get_rs(rs::ALPHAREF) == 0);
  CHECK(d.get_rs(rs::ALPHABLENDENABLE) == 1);
  CHECK(d.get_rs(rs::ZWRITEENABLE) == 0);
  f.push_render_mode(view::kModeArea);  // leaving it: ALPHAREF 1
  CHECK(d.get_rs(rs::ALPHAREF) == 1);
  CHECK(d.get_rs(rs::ALPHABLENDENABLE) == 0);
  CHECK(d.get_rs(rs::ZWRITEENABLE) == 1);
  CHECK(d.get_rs(rs::LIGHTING) == 0);  // 0x14634 bit 0 clear: the area nodes are unlit
  CHECK(d.get_rs(rs::ZENABLE) == kZbUseW);
  CHECK(d.get_rs(rs::FILLMODE) == kFillSolid);
  CHECK(d.get_rs(rs::FOGENABLE) == 1);  // 0x200
  f.push_render_mode(view::kModeTrans);  // 0x10034: no 0x200 -> fog off, cull none
  CHECK(d.get_rs(rs::FOGENABLE) == 0);
  CHECK(d.get_rs(rs::CULLMODE) == kCullNone);
}

TEST_CASE("sage: SetFog writes the statics' bit patterns once, then only toggles FOGENABLE") {
  std::vector<std::string> log;
  Device8 d(&log);
  view::SageFrame f(d, view::SageCaps(), view::SageFrameOptions());
  f.set_fog_value(0xfff9dcf9u, 40.0f, 1200.0f, 0.05f);
  f.set_fog(true);
  f.set_fog(true);  // mFogOn already set: returns early
  f.set_fog(false);
  f.set_fog(true);
  // FOGTABLEMODE: the D3D8 default is NONE; SetFog writes LINEAR (AppMain did too, on the real device).
  const std::vector<std::string> want = {"RS FOGENABLE 1",       "RS FOGTABLEMODE 3", "RS FOGCOLOR 0xfff9dcf9",         "RS FOGSTART 0x42200000 (40)",
                                         "RS FOGEND 0x44960000 (1200)", "RS FOGDENSITY 0x3d4ccccd (0.05)", "RS FOGENABLE 0",
                                         "RS FOGENABLE 1"};
  CHECK(state_lines(log) == want);
}

TEST_CASE("sage: DrawX two passes; plnt sets the context flag (alpha test only) and turns fog off afterwards (R14)") {
  view::AreaData a = synthetic_area();
  std::vector<std::string> log;
  Device8 d(&log);
  view::SageFrame f(d, view::SageCaps(), view::SageFrameOptions());
  f.push_render_mode(view::kModeArea);
  uint32_t quad = 0, plnt = 0;
  for (uint32_t i = 0; i < a.placements.size(); ++i) {
    const std::string& id = a.models[a.placements[i].model].id;
    if (id == "quad" && a.placements[i].route == sage::Route::Main) quad = i;
    if (id == "plnt") plnt = i;
  }
  log.clear();
  f.area_render(a, quad);
  std::vector<std::string> l = state_lines(log);
  // quad: red (opaque), nothere (opaque, NULL texture), then cut in pass 2 with blend + GREATER test.
  auto at = [&](const std::string& s) { return std::find(l.begin(), l.end(), s) - l.begin(); };
  REQUIRE(at("DRAW quad#0 attrib=0") < at("DRAW quad#0 attrib=2"));
  REQUIRE(at("DRAW quad#0 attrib=2") < at("RS ALPHATESTENABLE 1"));
  CHECK(at("RS ALPHABLENDENABLE 1") < at("DRAW quad#0 attrib=1"));
  CHECK(at("RS ALPHAFUNC 5") < at("DRAW quad#0 attrib=1"));
  CHECK(at("RS ZBIAS 16") > at("DRAW quad#0 attrib=1"));  // left at 16 after the alpha pass
  CHECK(l.back() == "TEXTURE 1 NULL");
  CHECK(d.get_rs(rs::FOGENABLE) == 1);
  log.clear();
  f.area_render(a, plnt);
  l = state_lines(log);
  // ALPHABLENDENABLE goes 1 then back to 0 before the alpha subset is drawn; fog is off after the model.
  const auto draw = at("DRAW plnt#6 attrib=0");
  REQUIRE(draw < static_cast<std::ptrdiff_t>(l.size()));
  std::string last_blend;
  for (std::ptrdiff_t k = 0; k < draw; ++k)
    if (l[k].rfind("RS ALPHABLENDENABLE", 0) == 0) last_blend = l[k];
  CHECK(last_blend == "RS ALPHABLENDENABLE 0");
  CHECK(l.back() == "RS FOGENABLE 0");
  CHECK(d.get_rs(rs::FOGENABLE) == 0);
  CHECK_FALSE(f.context_flag());
}

TEST_CASE("sage: a model with mTransparency < 1 gets ZBIAS 16 and a TFACTOR alpha (DrawX prologue)") {
  view::AreaData a = synthetic_area();
  std::vector<std::string> log;
  Device8 d(&log);
  view::SageFrame f(d, view::SageCaps(), view::SageFrameOptions());
  f.draw_x(a, 0, 0.5f);
  const std::vector<std::string> l = state_lines(log);
  REQUIRE(l.size() > 6);
  CHECK(l[0].rfind("XFORM WORLD", 0) == 0);  // SOFTWAREVERTEXPROCESSING 0 is the default: ScDxStates filters it
  CHECK(l[1] == "RS ZBIAS 16");
  CHECK(l[2] == "RS ALPHABLENDENABLE 1");
  CHECK(l[3] == "TSS0 ALPHAOP 4");
  CHECK(l[4] == "TSS0 ALPHAARG1 3");
  CHECK(l[5] == "TSS0 ALPHAARG2 2");  // D3D8 default CURRENT -> TEXTURE
  CHECK(l[6] == "RS TEXTUREFACTOR 0x80000000");
}

TEST_CASE("area: A10 - every drawn subset of a coloured model feeds its vertex DIFFUSE to the shader") {
  view::AreaData a = synthetic_area();
  std::vector<std::string> calls;
  auto dev = rhi::create_null_device(&calls);
  rhi::TextureId rt = dev->create_render_target_ex(rhi::RenderTargetDesc{16, 16, true, 1.0f});
  view::AreaRenderer r;
  std::string err;
  REQUIRE_MESSAGE(r.upload(*dev, a, view::RenderOptions(), &err), err);
  REQUIRE(r.render(*dev, rt, a, view::make_camera(a, view::synthetic_camera(), 1.0f)));
  // The vertex layout carries the colour at @location(3) (48-byte stride, offset 32).
  bool layout = false;
  for (const auto& l : calls)
    if (l.rfind("create_mesh_pipeline", 0) == 0) layout = l.find("3:f32x4@32") != std::string::npos;
  CHECK(layout);
  CHECK(std::string(view::area_wgsl()).find("@location(3) c: vec4f") != std::string::npos);
  CHECK(std::string(view::area_wgsl()).find("select(vec4f(1.0), c, u.a.y > 0.5)") != std::string::npos);
  CHECK(r.last_stats().rhi_draws == 20);  // Task 8A E1: + the "sky1" Trans placement
}


namespace {

// The placements the Trans node draws, in order ("id#record", one entry per placement).
std::vector<std::string> trans_order(const view::AreaData& a) {
  std::vector<std::string> log;
  Device8 d(&log);
  view::SageFrame f(d, view::SageCaps(), view::SageFrameOptions());
  f.frame(a, view::make_camera(a, view::synthetic_camera(), 1.0f), 0);
  std::vector<std::string> out;
  bool in_trans = false;
  for (const std::string& l : log) {
    if (l.rfind("# node ", 0) == 0) in_trans = l == "# node Trans";
    if (l.rfind("# main view: ScRenderer::Finish", 0) == 0) in_trans = false;
    if (!in_trans || l.rfind("DRAW ", 0) != 0) continue;
    const std::string tag = l.substr(5, l.find(' ', 5) - 5);
    if (out.empty() || out.back() != tag) out.push_back(tag);
  }
  return out;
}

}  // namespace

TEST_CASE("sage: GcTransDirector::SortIt - sky1 first, watr and ids ending in 'w' only when last (Task 8A E1)") {
  // The exe holds the two ids at 0x0070b060 / 0x0070b064 as the bytes "1yks" / "rtaw": read as the engine's
  // little-endian u32 4CC (first character in the top byte, as the "tnlp" -> plnt check) they are "sky1" / "watr".
  // Distances (camera at the POS start (0,0,10); greatest-Y point + position, xz): watr#8 92.3 > quad#3 82.8 >
  // blue#4 72.4 > sky1#11 33.0. A distance-only sort (the byte-swapped ids) would draw watr first and sky1 last.
  const view::AreaData a = synthetic_area();
  CHECK(trans_order(a) == std::vector<std::string>{"sky1#11", "quad#3", "blue#4", "watr#8"});
  // The 'w'-suffix rule: rename the watr model to "pdlw" -> still deferred to the end; to "pdlx" -> distance sort.
  view::AreaData b = a;
  for (view::Model& m : b.models)
    if (m.id == "watr") m.id = "pdlw";
  CHECK(trans_order(b) == std::vector<std::string>{"sky1#11", "quad#3", "blue#4", "pdlw#8"});
  for (view::Model& m : b.models)
    if (m.id == "pdlw") m.id = "pdlx";
  CHECK(trans_order(b) == std::vector<std::string>{"sky1#11", "pdlx#8", "quad#3", "blue#4"});
  // Neither special id: plain far-to-near (sky1 renamed "skyx" is the nearest).
  for (view::Model& m : b.models)
    if (m.id == "sky1") m.id = "skyx";
  CHECK(trans_order(b) == std::vector<std::string>{"pdlx#8", "quad#3", "blue#4", "skyx#11"});
}
