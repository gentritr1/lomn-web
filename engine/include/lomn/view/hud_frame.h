// M2a-3 Task 8: the HUD viewport pass (docs/osi/hud.md §3, contract §10). Replays, against a d3d::Device8, the device
// calls of the HUD view after the area: the ortho camera, then every shown HUD item in Show order at priority 0 and the
// deferred ones again at priority 100 (§3.1). The input is the HUD state of one `Z` snapshot (hud.md §9.2), so the pass
// never reads the VM: the same lines drive the C++ pass and the independent oracle (Task 9).
//
// Modelled (EXECUTED unless marked, citations in hud.md §3.4–§3.6 and §4.11):
//   sprites: 1x1 tiles only (b6 = 0 with no ReadMe.bin: every shipped sprite, census in §4.11), doAlpha, doColor;
//   text: Draw and DrawJust (§4.11), one-byte codes, spaces, unknown codes, '\n'.
// Refused (fail closed with a message, never drawn wrong): b6 != 0 (the 3x3 grid), box-fit text, the 0xFF callback, 2-byte codes, a missing font file (a VM error, §7.2).
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "lomn/d3d/device8.h"
#include "lomn/fs.h"
#include "lomn/sage/colfont.h"

namespace lomn::view {

// ScTileFont fields Draw reads; defaults are the ctor's (0x626de9..0x626ec2, constants 0x75819c..0x7581c4).
struct HudFont {
  const sage::TileFont* font = nullptr;
  const std::vector<sage::Image>* images = nullptr;  // glyph images (record order), only for hud_pass's ink boxes
  uint32_t glyph_tex0 = 0;               // device handle of record 0 (record i = glyph_tex0 + i)
  float color[4] = {1, 1, 1, 1};         // +0x24 R, +0x28 G, +0x2c B, +0x30 A
  bool transp = true, alpha = true;      // +0xb0, +0xb1 (both 1 in the ctor: every text defers)
  bool linear = true;                    // +0xb2
};
struct HudSprite {
  float x1 = 0, y1 = 0, x2 = 0, y2 = 0;  // GcHudSprite::SetPosition / ctor rect
  bool alpha = false;                    // +0x51 doAlpha (enablealphachannel)
  bool color = false;                    // +0x52 doColor (setcolor with a colour other than (1,1,1,1))
  float rgba[4] = {1, 1, 1, 1};          // +0x168..+0x174 diffuse
  uint32_t tex = 0;                      // device handle, 0 = NULL (a missing .tga, §7.1)
};
struct HudText {
  float x = 0, y = 0, z = 0, xs = 1, ys = 1;  // +0x13c..+0x144, +0x15c/+0x160 (ctor scale 1.0, 0x754584/0x754588)
  uint16_t just = 0;                          // +0x2a
  bool juston = false;                        // +0x28
  std::string str;
  int font = -1;  // index into HudScene::fonts, -1 = none
};
struct HudItem {
  bool sprite = true;
  std::string who;  // "<type>:<handle>" as the Z lines print it
  size_t index = 0;  // into sprites / texts
};
struct HudScene {
  float width = 640, height = 480;  // the ortho camera's r/b (GetResolutionWidth/Height at DoSetUp)
  std::vector<HudSprite> sprites;
  std::vector<HudText> texts;
  std::vector<HudFont> fonts;
  std::vector<HudItem> shown;  // Show order
};

// Fonts and sprite textures registered once per path (device handles on first use), so a frame replay never grows the table.
struct HudFontCache {
  std::map<std::string, std::unique_ptr<sage::LoadedFont>> fonts;
  std::map<std::string, uint32_t> glyph_tex0;
  std::map<std::string, uint32_t> sprite_tex;  // "<file>.tga" -> device handle
};

// The HUD part of one Z snapshot -> a scene. Texture names are appended to `tex_names` (handle = index + 1), as the
// Device8 TEXTURE lines print them: a sprite "<file>.tga" (its VFS path, §10.1), a glyph "<font file>.col/<entry>".
// tex_exists: SrBlockFileExists 0x54ddc0 (Art.blk, then the block-file slots, by base name), the sprite texture lookup.
using TexExists = std::function<bool(const std::string&)>;
bool hud_scene_from_z(const std::vector<std::string>& z, const fs::FileSystem& vfs, const TexExists& tex_exists,
                      HudFontCache& cache,
                      std::vector<std::string>& tex_names, HudScene& out, std::string* err);

// The HUD view's device calls between its SrPushRenderMode(0x8c034) and its Finish (§3.1): ortho PROJECTION + VIEW
// (GcOrthoCamera::Render 0x406ec0), then the items (§3.4–§3.6).
// boxes (optional): per item ("<type>:<handle>"), the union of its quads' transformed (x', y') = pixels at the scene's
// size (the ortho camera maps them 1:1, y down), for the I2/F22 checks; never part of the log.
struct HudBox {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;      // the quads (cells)
  float ix0 = 0, iy0 = 0, ix1 = 0, iy1 = 0;  // glyph pixels with alpha > 0 (texts only)
  bool cell = false, ink = false;
};
using HudBoxes = std::map<std::string, HudBox>;
void hud_pass(d3d::Device8& d, const HudScene& s, HudBoxes* boxes = nullptr);

// Building blocks (public for tests). Row-vector matrix product, each product and sum rounded to f32 in the order of
// 0x52b580: ((a0*b0 + a1*b1) + a2*b2) + a3*b3.
Mat4 hud_mul(const Mat4& a, const Mat4& b);
// The 31-hash over (x', y', z') of the four quad vertices of §3.4 under WORLD `m` (§10.1 vsum).
uint32_t hud_vsum(const Mat4& m);
Mat4 hud_sprite_world(const HudSprite& s);  // §3.4: S(w, h)·RotX(-pi_f32), row 3 += (tx, ty, -7499)

}  // namespace lomn::view
