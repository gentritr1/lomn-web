// M2a-3 T8: the HUD viewport pass on the shim (hud.md §3, §10). Expected values: hud.md §10.2's worked example (computed
// there by a scratch f32 script, not by an engine) and hand derivations written next to each check.
#include <cstring>
#include <string>
#include <vector>

#include "doctest.h"
#include "lomn/fs.h"
#include "lomn/view/area.h"
#include "lomn/view/hud_frame.h"
#include "lomn/view/sage_frame.h"

using namespace lomn;
using namespace lomn::view;

namespace {
uint32_t ub(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}
size_t find(const std::vector<std::string>& log, const std::string& line, size_t from = 0) {
  for (size_t i = from; i < log.size(); ++i)
    if (log[i] == line) return i;
  return log.size();
}
size_t count_prefix(const std::vector<std::string>& log, const std::string& p) {
  size_t n = 0;
  for (const auto& l : log) n += l.rfind(p, 0) == 0;
  return n;
}
// (x', y') of quad vertex v under m, the §10.1 formula.
void xf(const Mat4& m, float vx, float vy, float& x, float& y) {
  x = ((vx * m.m[0] + vy * m.m[4]) + 0.0f * m.m[8]) + m.m[12];
  y = ((vx * m.m[1] + vy * m.m[5]) + 0.0f * m.m[9]) + m.m[13];
}
}  // namespace

TEST_CASE("hud frame: the cursor of hud.md §10.2 gives its WORLD bits, sums and five lines") {
  HudSprite s;
  s.x1 = 50, s.y1 = 50, s.x2 = 82, s.y2 = 82;
  const Mat4 w = hud_sprite_world(s);
  const uint32_t want[16] = {0x42000000, 0, 0, 0, 0, 0xc2000000, 0x363bbd2e, 0, 0, 0xb3bbbd2e, 0xbf800000, 0,
                             0x42840000, 0x42840000, 0xc5ea5800, 0x3f800000};
  for (int i = 0; i < 16; ++i) CHECK_MESSAGE(ub(w.m[i]) == want[i], "m[" << i << "]");
  CHECK(hud_vsum(w) == 0x9f18392cu);

  std::vector<std::string> log, names = {"Root/Data/Art/MenuArt/cursor0000.tga"};
  d3d::Device8 d(&log, &names);
  s.tex = 1;
  HudScene sc;
  sc.sprites.push_back(s);
  sc.shown.push_back({true, "0026:0000009a", 0});
  hud_pass(d, sc);
  const size_t m = find(log, "MATERIAL d=(0.7,0.7,0.7,1) a=(1,1,1,1) s=(1,1,1,1) e=(0,0,0,0) p=80");
  REQUIRE(m + 4 < log.size());
  CHECK(log[m + 1] == "TEXTURE 0 Root/Data/Art/MenuArt/cursor0000.tga");
  CHECK(log[m + 2] == "XFORM WORLD sum=381857c2");
  CHECK(log[m + 3] == "DRAW hud.sprite 0026:0000009a tile=0,0 call=DIP prim=5 count=2 fvf=0x112 vsum=9f18392c");
  CHECK(log[m + 4] == "TEXTURE 0 NULL");
}

TEST_CASE("hud frame: the cursor quad's projected centre is the mouse pixel (I2), y down at 640x480") {
  // The menu script places the cursor at (mx - 16, my - 16, mx + 16, my + 16) (M1's last frame: (434,354)-(466,386) with
  // the mouse at 450,370). Project the quad centre through the ortho PROJECTION the pass sets (§3.3).
  HudSprite s;
  s.x1 = 434, s.y1 = 354, s.x2 = 466, s.y2 = 386;
  float cx = 0, cy = 0;
  const Mat4 w = hud_sprite_world(s);
  float x, y;
  for (auto v : {std::pair{-0.5f, 0.5f}, {0.5f, 0.5f}, {-0.5f, -0.5f}, {0.5f, -0.5f}}) {
    xf(w, v.first, v.second, x, y);
    cx += x / 4, cy += y / 4;
  }
  const float ndc_x = cx * (2.0f / 640.0f) - 1.0f, ndc_y = cy * (2.0f / (0.0f - 480.0f)) + 1.0f;
  CHECK((ndc_x + 1.0f) * 0.5f * 640.0f == doctest::Approx(450.0f).epsilon(1e-6));
  CHECK((1.0f - ndc_y) * 0.5f * 480.0f == doctest::Approx(370.0f).epsilon(1e-6));
  xf(w, -0.5f, 0.5f, x, y);  // v0, uv (0,0): the top-left
  CHECK(x == 434.0f);
  CHECK(y == 354.0f);
}

TEST_CASE("hud frame: a glyph's states, box and advance; restores; a hidden object draws nothing") {
  sage::TileFont tf;
  tf.rows = tf.columns = tf.count = 1;
  tf.char_width = tf.char_height = 16, tf.baseline = 12, tf.space_width = 5, tf.max_width = 10;
  tf.glyphs.push_back({'P', 10, 16, 1, 0});
  HudFont f;
  f.font = &tf;
  f.glyph_tex0 = 1;
  f.color[2] = 0.0f;  // (1,1,0,1): TEXTUREFACTOR 0xffffff00
  HudText t;
  t.x = 400, t.y = 320, t.str = "P P";
  t.font = 0;
  std::vector<std::string> log, names = {"F.col/F0_0.tga"};
  d3d::Device8 d(&log, &names);
  HudScene sc;
  sc.fonts.push_back(f);
  sc.texts.push_back(t);
  sc.shown.push_back({false, "002e:00000001", 0});
  hud_pass(d, sc);
  // Device defaults: COLORARG1 TEXTURE, COLOROP MODULATE (kept), COLORARG2 CURRENT (set to TFACTOR), CULL CCW, MIN POINT.
  const size_t a = find(log, "TSS0 COLORARG2 3");
  REQUIRE(a < log.size());
  CHECK(log[a + 1] == "RS TEXTUREFACTOR 0xffffff00");
  CHECK(log[a + 2] == "RS CULLMODE 1");
  CHECK(log[a + 3] == "TSS0 MINFILTER 2");
  CHECK(log[a + 4] == "TSS0 MAGFILTER 2");
  CHECK(log[a + 5] == "RS ALPHABLENDENABLE 1");
  CHECK(log[a + 6] == "RS SRCBLEND 5");
  CHECK(log[a + 7] == "RS DESTBLEND 6");
  CHECK(log[a + 8] == "RS ZWRITEENABLE 0");
  CHECK(count_prefix(log, "DRAW hud.glyph 002e:00000001 ch=50 call=DIP prim=5 count=2 fvf=0x112") == 2);
  // Restores, in order, after the last draw.
  const size_t r = find(log, "TSS0 COLORARG2 1");
  REQUIRE(r < log.size());
  CHECK(log[r + 1] == "RS CULLMODE 3");
  CHECK(log[r + 2] == "TSS0 MINFILTER 1");
  CHECK(log[r + 3] == "TSS0 MAGFILTER 1");
  CHECK(log[r + 4] == "RS ALPHABLENDENABLE 0");
  CHECK(log[r + 5] == "RS ZWRITEENABLE 1");
  std::vector<std::string> hidden_log;
  d3d::Device8 d2(&hidden_log, &names);
  HudScene none;  // nothing shown
  hud_pass(d2, none);
  CHECK(count_prefix(hidden_log, "DRAW ") == 0);
  CHECK(hidden_log.size() == 2);  // PROJECTION + VIEW only
}

TEST_CASE("hud frame: the glyph box (hand-derived) and the scene from Z lines refuses what is not modelled") {
  // Glyph 'P' at pen (399, 332) with w = h = 16 (§3.6): v0 -> (x, y - h), v3 -> (x + w, y).
  sage::TileFont tf;
  tf.rows = tf.columns = tf.count = 1;
  tf.char_width = tf.char_height = 16, tf.baseline = 12;
  tf.glyphs.push_back({'P', 10, 16, 1, 0});
  HudFont f;
  f.font = &tf;
  HudText t;
  t.x = 400, t.y = 320, t.str = "P";
  t.font = 0;
  std::vector<std::string> log;
  d3d::Device8 d(&log);
  HudScene sc;
  sc.fonts.push_back(f);
  sc.texts.push_back(t);
  sc.shown.push_back({false, "002e:00000001", 0});
  hud_pass(d, sc);
  const Mat4& w = d.state().world;
  float x, y;
  xf(w, -0.5f, 0.5f, x, y);
  CHECK(x == 399.0f);
  CHECK(y == 316.0f);
  xf(w, 0.5f, -0.5f, x, y);
  CHECK(x == 415.0f);
  CHECK(y == 332.0f);
  CHECK(ub(w.m[14]) == 0xc5ea5800u);

  // DrawJust (§4.11) with just = 4 at scale 2: y += (chop(16*2) >> 1) - 12*2 = -8, then Draw adds 24: bottom 336.
  t.just = 4, t.juston = true, t.xs = t.ys = 2;
  sc.texts[0] = t;
  log.clear();
  hud_pass(d, sc);
  xf(d.state().world, 0.5f, -0.5f, x, y);
  CHECK(y == 336.0f);
  CHECK(x == 400.0f - 2.0f + 32.0f);

  fs::MemoryFileSystem vfs;
  const TexExists ex = [&](const std::string& n) { return vfs.exists(n); };
  HudFontCache cache;
  std::vector<std::string> names;
  HudScene out;
  std::string err;
  const std::vector<std::string> b6 = {"Z hud.node 00000000 sprite 0026:00000001 0", "Z sprite.rect 0026:00000001 0 0 0 0",
                                       "Z sprite.file 0026:00000001 1 41", "Z sprite.b6 0026:00000001 01"};
  CHECK_FALSE(hud_scene_from_z(b6, vfs, ex, cache, names, out, &err));
  CHECK(err.find("3x3") != std::string::npos);
  const std::vector<std::string> missing = {"Z hud.node 00000000 sprite 0026:00000001 0", "Z sprite.rect 0026:00000001 0 0 0 0",
                                            "Z sprite.file 0026:00000001 6 526f6f742f41", "Z sprite.b6 0026:00000001 00"};
  REQUIRE(hud_scene_from_z(missing, vfs, ex, cache, names, out, &err));  // no file at all: drawn with a NULL texture (§7.1)
  CHECK(out.sprites.at(0).tex == 0);
  REQUIRE(vfs.mount("Root/A.bmp", {0}));  // not in the loader's table (.dds, .tga): still NULL
  REQUIRE(hud_scene_from_z(missing, vfs, ex, cache, names, out, &err));
  CHECK(out.sprites.at(0).tex == 0);
  REQUIRE(vfs.mount("Root/A.tga", {0}));
  REQUIRE(hud_scene_from_z(missing, vfs, ex, cache, names, out, &err));
  CHECK(out.sprites.at(0).tex == 1);
  CHECK(names.back() == "Root/A.tga");
  REQUIRE(vfs.mount("Root/A.dds", {0}));  // .dds is probed first: refused
  CHECK_FALSE(hud_scene_from_z(missing, vfs, ex, cache, names, out, &err));
  CHECK(err.find("Root/A.dds") != std::string::npos);
  const std::vector<std::string> hidden = {"Z hud.node 00000000 sprite 0026:00000001 -"};
  CHECK(hud_scene_from_z(hidden, vfs, ex, cache, names, out, &err));
  CHECK(out.shown.empty());
}

TEST_CASE("hud frame: SageFrame runs the HUD after the area's Finish and before ENDSCENE; none -> the area-only log") {
  AreaData area;
  Camera cam;
  std::vector<std::string> plain, with;
  {
    d3d::Device8 d(&plain);
    SageFrame f(d, SageCaps{}, SageFrameOptions{});
    f.frame(area, cam, 1);
  }
  {
    d3d::Device8 d(&with);
    SageFrame f(d, SageCaps{}, SageFrameOptions{});
    f.set_hud([&] { d.note("hud draws here"); });
    f.frame(area, cam, 1);
  }
  CHECK(find(plain, "# viewport HdDr") == plain.size());
  const size_t v = find(with, "# viewport HdDr"), fin = find(with, "# main view: ScRenderer::Finish"),
               push = find(with, "# SrPushRenderMode 0x8c034", v), hud = find(with, "# hud draws here"),
               end = find(with, "ENDSCENE");
  CHECK(fin < v);
  CHECK(v < push);
  CHECK(push < hud);
  CHECK(hud < end);
  // Removing the two HUD notes gives the area-only log back.
  std::vector<std::string> back;
  for (const auto& l : with)
    if (l != "# viewport HdDr" && l != "# hud draws here") back.push_back(l);
  CHECK(back == plain);
}

TEST_CASE("hud frame: a coloured sprite sets TFACTOR and restores from the ScDxStates cache; white is no colour") {
  // Hand derivation: (0.5, 1, 0, 1) -> R chop(127.5) = 0x7f, G 0xff, B 0, A 0xff -> 0xff7fff00. On a fresh device the
  // cache holds 0 for COLORARG1/COLOROP (never dx-set: no restore) and saved COLORARG2 = the device default 1 (CURRENT).
  HudSprite s;
  s.x1 = 0, s.y1 = 0, s.x2 = 8, s.y2 = 8;
  s.color = true;
  s.rgba[0] = 0.5f, s.rgba[2] = 0.0f;
  std::vector<std::string> log;
  d3d::Device8 d(&log);
  HudScene sc;
  sc.sprites.push_back(s);
  sc.shown.push_back({true, "0026:00000012", 0});
  hud_pass(d, sc);
  const size_t a = find(log, "TSS0 COLORARG2 3");
  REQUIRE(a + 1 < log.size());
  CHECK(log[a + 1] == "RS TEXTUREFACTOR 0xff7fff00");
  const size_t dr = find(log, "DRAW hud.sprite 0026:00000012 tile=0,0", a), r = find(log, "TSS0 COLORARG2 1", a);
  (void)dr;
  CHECK(count_prefix(log, "DRAW hud.sprite") == 1);
  REQUIRE(r < log.size());
  CHECK(log[r - 1] == "TEXTURE 0 NULL");
  CHECK(r == log.size() - 1);
  CHECK(count_prefix(log, "TSS0 COLORARG1") == 0);
  // doAlpha + doColor (0x5274fb): the alpha branch skips COLORARG2 = 0, so COLORARG2 goes straight to 3.
  sc.sprites[0].alpha = true;
  std::vector<std::string> log2;
  d3d::Device8 d2(&log2);
  hud_pass(d2, sc);
  CHECK(find(log2, "TSS0 COLORARG2 0") == log2.size());
  CHECK(find(log2, "RS ALPHAFUNC 5") < find(log2, "TSS0 COLORARG2 3"));

  fs::MemoryFileSystem vfs;
  const TexExists ex = [&](const std::string& n) { return vfs.exists(n); };
  HudFontCache cache;
  std::vector<std::string> names;
  HudScene out;
  std::string err;
  std::vector<std::string> z = {"Z hud.node 00000000 sprite 0026:00000001 0", "Z sprite.rect 0026:00000001 0 0 0 0",
                                "Z sprite.file 0026:00000001 1 41", "Z sprite.b6 0026:00000001 00",
                                "Z sprite.color 0026:00000001 3f800000 3f800000 3f800000 3f800000"};
  REQUIRE(hud_scene_from_z(z, vfs, ex, cache, names, out, &err));
  CHECK_FALSE(out.sprites.at(0).color);
  z.back() = "Z sprite.color 0026:00000001 3f800000 3f800000 00000000 3f800000";
  REQUIRE(hud_scene_from_z(z, vfs, ex, cache, names, out, &err));
  CHECK(out.sprites.at(0).color);
}

TEST_CASE("hud frame: the script camera's projection follows D3dx8cw.dll's op order (hud.md §1.7 pin)") {
  // Expected bits: an exact-rational f32 rounding of the DLL's sequence (fsincos, cos/sin, zf/(zn-zf), m10*zn), not an
  // engine run. The documented zn*zf/(zn-zf) in f64 gives m14 bc23d721 / ys 3fb02d48 instead.
  const Mat4 p = d3dx_perspective_fov_rh(72.0f * 0.017453292f, 640.0f / 480.0f, 0.01f, 4700.0f);
  CHECK(ub(p.m[0]) == 0x3f8421f6u);
  CHECK(ub(p.m[5]) == 0x3fb02d49u);
  CHECK(ub(p.m[10]) == 0xbf800011u);
  CHECK(ub(p.m[11]) == 0xbf800000u);
  CHECK(ub(p.m[14]) == 0xbc23d720u);
  CHECK(ub(p.m[15]) == 0u);
}
