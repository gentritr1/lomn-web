// M2a-3 Task 8: the HUD viewport pass. Every rule names its address; hud.md §3 and §4.11 hold the readings. Arithmetic:
// the render code runs at x87 precision control 24 bits, so each x87 op is one f32 rounding (FLT_EVAL_METHOD 0 and
// -ffp-contract=off give the same here); a u32 -> float conversion is exact because hud_scene_from_z refuses values
// >= 2^24 (fild of a zero-extended dword).
#include "lomn/view/hud_frame.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace lomn::view {

using namespace lomn::d3d;

namespace {

float fb(uint32_t u) {
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
// D3DUtil_SetRotateXMatrix 0x52c2a0 at -pi_f32 (sprites, 0x72ef8c) and +pi_f32 (fonts, 0x7581c8): cos = -1.0 and
// sin = +-8.742278e-08 (0x33bbbd2e), as f32 from the CRT's double cos/sin; held as bits so no libm is involved.
Mat4 rot_x(uint32_t sin_bits) {
  Mat4 r = mat_identity();
  r.m[5] = -1.0f;
  r.m[6] = fb(sin_bits);
  r.m[9] = -fb(sin_bits);
  r.m[10] = -1.0f;
  return r;
}
constexpr uint32_t kSinMinusPi = 0x33bbbd2eu, kSinPlusPi = 0xb3bbbd2eu;
constexpr float kHudZ = -7499.0f;  // 0x72efbc (sprites), 0x758218 (glyphs)
constexpr uint32_t kFvfQuad = 0x112;

// LoadIndexed 0x550d50 (hud.md §10.1): the material every HUD quad carries.
Material8 hud_material() {
  Material8 m;
  const float k = fb(0x3f333333u);
  m.diffuse = {k, k, k, 1.0f};
  m.ambient = {1, 1, 1, 1};
  m.specular = {1, 1, 1, 1};
  m.emissive = {0, 0, 0, 0};
  m.power = 80.0f;
  return m;
}

// fistp with RC = chop: the int32 truncation, 0x80000000 when out of range (the x87 integer indefinite).
int32_t chop32(float v) {
  if (!(v > -2147483649.0f && v < 2147483648.0f)) return INT32_MIN;
  return static_cast<int32_t>(v);
}
// fistp qword with RC = chop, low dword.
uint32_t chop_lo(float v) {
  if (!(std::fabs(v) < 9.2e18f)) return 0;
  return static_cast<uint32_t>(static_cast<int64_t>(v));
}
// fild of a zero-extended dword, then one rounding when added to an f32: exact sum rounded once.
float add_u(uint32_t u, float y) { return static_cast<float>(static_cast<double>(u) + static_cast<double>(y)); }

// The optional box output of hud_pass. ponytail: a file-local pointer set for one hud_pass call (the engine is single-
// threaded); a parameter through every draw helper if that ever changes.
HudBoxes* g_boxes = nullptr;
void grow(HudBox& b, bool& set, float x, float y) {
  if (!set) b = {x, y, x, y}, set = true;
  b.x0 = std::min(b.x0, x), b.y0 = std::min(b.y0, y), b.x1 = std::max(b.x1, x), b.y1 = std::max(b.y1, y);
}
// The quad point at local (lx, ly) in -0.5..0.5 under m (§10.1's vertex formula).
void xf(const Mat4& m, float lx, float ly, float& x, float& y) {
  x = ((lx * m.m[0] + ly * m.m[4]) + 0.0f * m.m[8]) + m.m[12];
  y = ((lx * m.m[1] + ly * m.m[5]) + 0.0f * m.m[9]) + m.m[13];
}
// Cell box: the four vertices. Ink box (glyphs): the pixels with alpha > 0, uv (0,0) at v0 = local (-0.5, 0.5).
void note_box(const Mat4& m, const std::string& what, const sage::Image* img) {
  if (!g_boxes) return;
  const size_t a = what.find(' ') + 1, b = what.find(' ', a);
  HudBox& bx = (*g_boxes)[what.substr(a, b - a)];
  float x, y;
  for (auto v : {std::pair{-0.5f, 0.5f}, {0.5f, 0.5f}, {-0.5f, -0.5f}, {0.5f, -0.5f}}) xf(m, v.first, v.second, x, y), grow(bx, bx.cell, x, y);
  if (!img || img->width == 0 || img->height == 0) return;
  uint32_t px0 = img->width, py0 = img->height, px1 = 0, py1 = 0;
  for (uint32_t py = 0; py < img->height; ++py)
    for (uint32_t px = 0; px < img->width; ++px)
      if (img->rgba[(static_cast<size_t>(py) * img->width + px) * 4 + 3])
        px0 = std::min(px0, px), py0 = std::min(py0, py), px1 = std::max(px1, px + 1), py1 = std::max(py1, py + 1);
  if (px0 >= px1) return;  // an empty glyph adds no ink
  for (uint32_t u : {px0, px1})
    for (uint32_t v : {py0, py1}) {
      xf(m, -0.5f + static_cast<float>(u) / static_cast<float>(img->width), 0.5f - static_cast<float>(v) / static_cast<float>(img->height), x, y);
      HudBox ink{bx.ix0, bx.iy0, bx.ix1, bx.iy1};
      grow(ink, bx.ink, x, y);
      bx.ix0 = ink.x0, bx.iy0 = ink.y0, bx.ix1 = ink.x1, bx.iy1 = ink.y1;
    }
}

// ScDrawableMaterial::Draw 0x54ee70 -> ScDrawableTexture::Draw 0x5569f0 (§3.4): one quad.
void material_draw(Device8& d, uint32_t tex, const Mat4& world, const std::string& what, const sage::Image* img = nullptr) {
  note_box(world, what, img);
  d.dx_set_tss(0, tss::TEXTURETRANSFORMFLAGS, kTtfDisable);  // 0x54eeba (ScDxStates)
  d.set_material(hud_material());                            // 0x54eef9 (raw)
  d.set_vertex_shader(kFvfQuad);                             // SetStreamSource(0, vb, 0x20) + SetVertexShader(0x112)
  d.dx_set_rs(rs::SOFTWAREVERTEXPROCESSING, 0);              // ScDxStates
  d.set_texture(0, tex);                                     // SetIndices + SetTexture(stage, [this+0x98])
  d.set_transform(kTsWorld, world);                          // matrix count 1
  d.draw_primitive(what, true, 5, 2, hud_vsum(world));       // DrawIndexedPrimitive(TRIANGLESTRIP, 0, 4, 0, 2)
  d.set_texture(0, 0);                                       // then SetStreamSource(stage, 0, 0)
}

// GcBigSprite::Render 0x440570 over a 1x1 sprite -> GcHudSprite::Render 0x527210 (§3.5).
int sprite_render(Device8& d, const HudItem& it, const HudSprite& s, int pri) {
  if (s.alpha && pri != 100) return 100;  // 0x52723f (doTransparency is never set: no native writes +0x50)
  d.dx_set_rs(rs::ALPHABLENDENABLE, 1);   // 0x52747d
  if (s.alpha) {
    d.dx_set_rs(rs::ALPHAREF, static_cast<uint32_t>(chop32(1.0f)));  // chop(+0x40), ctor 1.0 (0x70ffb4)
    d.dx_set_rs(rs::ALPHAFUNC, kCmpGreater);                          // byte +0x161 = 0 (ctor 0x49f04a)
    if (!s.color) d.dx_set_tss(0, tss::COLORARG2, kTaDiffuse);       // 0x5274fb: !doColor || doTransparency
  }
  if (s.color) {  // 0x52757d: doColor && !doTransparency
    d.dx_set_tss(0, tss::COLORARG2, kTaTFactor);
    auto ch = [](float c) { return static_cast<uint32_t>(chop32(c * 255.0f)) & 0xffu; };  // fmul qword 255.0, movsx, & 0xff
    d.dx_set_rs(rs::TEXTUREFACTOR, ch(s.rgba[0]) << 16 | ch(s.rgba[3]) << 24 | ch(s.rgba[1]) << 8 | ch(s.rgba[2]));
  }
  material_draw(d, s.tex, hud_sprite_world(s), "hud.sprite " + it.who + " tile=0,0");
  if (s.color) {  // 0x52776a: RestoreTSS COLORARG1, COLOROP, COLORARG2
    d.dx_restore_tss(0, tss::COLORARG1);
    d.dx_restore_tss(0, tss::COLOROP);
    d.dx_restore_tss(0, tss::COLORARG2);
  }
  return -1;
}

// ScTileFont::Draw 0x627dd0 (§3.6, §4.11).
int font_draw(Device8& d, const HudItem& it, const HudFont& f, float x, float y, const std::string& str, float xs,
              float ys, int pri) {
  const sage::TileFont& tf = *f.font;
  const float start_x = x;
  y = static_cast<float>(static_cast<uint32_t>(tf.baseline)) * ys + y;  // 0x627e0e: fild (u32), fmul yScale, fadd y
  const float kern = static_cast<float>(tf.kerning);                                                     // fild dword 0x627e20
  if ((f.transp || f.alpha) && pri != 100) return 100;                                                   // 0x627e4f
  const uint32_t s1 = d.get_tss(0, tss::COLORARG1), s2 = d.get_tss(0, tss::COLOROP), s3 = d.get_tss(0, tss::COLORARG2);
  if (s1 != kTaTexture) d.set_tss(0, tss::COLORARG1, kTaTexture);
  if (s2 != kTopModulate) d.set_tss(0, tss::COLOROP, kTopModulate);
  if (s3 != kTaTFactor) d.set_tss(0, tss::COLORARG2, kTaTFactor);
  auto ch = [](float c) { return static_cast<uint32_t>(chop32(c * 255.0f)) & 0xffu; };  // fmul qword 255.0, chop, low byte
  d.set_rs(rs::TEXTUREFACTOR, ch(f.color[0]) << 16 | ch(f.color[3]) << 24 | ch(f.color[1]) << 8 | ch(f.color[2]));  // 0x628048
  const uint32_t cull = d.get_rs(rs::CULLMODE);
  if (cull != kCullNone) d.set_rs(rs::CULLMODE, kCullNone);
  const uint32_t minf = d.get_tss(0, tss::MINFILTER);
  bool filt = false;
  if (!f.linear) {
    d.set_tss(0, tss::MINFILTER, kTexfPoint);
    d.set_tss(0, tss::MAGFILTER, kTexfPoint);
    filt = true;
  } else if (minf != kTexfLinear) {
    d.set_tss(0, tss::MINFILTER, kTexfLinear);
    d.set_tss(0, tss::MAGFILTER, kTexfLinear);
    filt = true;
  }
  const bool blend = f.transp || f.alpha;
  if (blend) {
    d.set_rs(rs::ALPHABLENDENABLE, 1);
    d.set_rs(rs::SRCBLEND, kBlendSrcAlpha);
    d.set_rs(rs::DESTBLEND, kBlendInvSrcAlpha);
    // alpha && [+0xc8] == 1: never (the ctor stores 0 at 0x626ea0 and no native writes +0xc8)
    d.set_rs(rs::ZWRITEENABLE, 0);  // 0x62820e
  }
  const Mat4 rot = rot_x(kSinPlusPi);
  for (size_t i = 0; i < str.size(); ++i) {
    const uint8_t b = static_cast<uint8_t>(str[i]);
    if (b == '\n') {  // 0x628258
      x = start_x;
      y = add_u(chop_lo(static_cast<float>(static_cast<uint32_t>(tf.char_height)) * ys), y);
      continue;
    }
    if (b == 0x20) {  // 0x628386: fild dword (signed) + kerning, * xScale, + x
      x = (static_cast<float>(tf.space_width) + kern) * xs + x;
      continue;
    }
    const int idx = tf.find(b);
    if (idx < 0) {  // 0x628753: fild (u32) maxWidth + kerning
      x = (static_cast<float>(static_cast<uint32_t>(tf.max_width)) + kern) * xs + x;
      continue;
    }
    const sage::FontGlyph& g = tf.glyphs[static_cast<size_t>(idx)];
    const float w = static_cast<float>(static_cast<uint32_t>(tf.char_width)) * xs;   // 0x62841c
    const float h = static_cast<float>(static_cast<uint32_t>(tf.char_height)) * ys;  // 0x628444
    x = x - static_cast<float>(static_cast<uint32_t>(g.x_offset)) * xs;             // 0x628477 (fsubr)
    Mat4 sc;  // +0xcc: zeros (0x758200/0x758204), then w, h, 1.0 (0x75820c), m33 1.0 (0x758208)
    sc.m[0] = w;
    sc.m[5] = h;
    sc.m[10] = 1.0f;
    sc.m[15] = 1.0f;
    Mat4 m = hud_mul(rot, sc);                // 0x628630: R·S
    m.m[12] = (0.5f * w + x) + m.m[12];       // 0x628650
    m.m[13] = (y - 0.5f * h) + m.m[13];       // 0x62866b
    m.m[14] = m.m[14] + kHudZ;                // 0x628686
    char what[16];
    std::snprintf(what, sizeof what, " ch=%02x", b);
    material_draw(d, f.glyph_tex0 + static_cast<uint32_t>(idx), m, "hud.glyph " + it.who + what,
                  f.images ? &(*f.images)[static_cast<size_t>(idx)] : nullptr);
    const uint32_t adv = static_cast<uint32_t>(g.x_offset) + static_cast<uint32_t>(g.width);  // 0x628720
    x = (static_cast<float>(adv) + kern) * xs + x;                                            // 0x62873c
  }
  if (s1 != kTaTexture) d.set_tss(0, tss::COLORARG1, s1);  // 0x628799..
  if (s2 != kTopModulate) d.set_tss(0, tss::COLOROP, s2);
  if (s3 != kTaTFactor) d.set_tss(0, tss::COLORARG2, s3);
  if (cull != kCullNone) d.set_rs(rs::CULLMODE, cull);
  if (filt) {
    d.set_tss(0, tss::MINFILTER, minf);
    d.set_tss(0, tss::MAGFILTER, minf);
  }
  if (blend) {
    d.set_rs(rs::ALPHABLENDENABLE, 0);
    d.set_rs(rs::ZWRITEENABLE, 1);
  }
  return -1;
}

// ScTextObject::Render 0x610f60 -> DrawJust 0x627c60 (when +0x28) -> Draw.
int text_render(Device8& d, const HudItem& it, const HudText& t, const HudFont& f, int pri) {
  float x = t.x, y = t.y;
  if (t.juston) {
    const sage::TileFont& tf = *f.font;
    const uint32_t chh = static_cast<uint32_t>(tf.char_height), base = static_cast<uint32_t>(tf.baseline);
    if (t.just & 1) {  // 0x627c7b: + chop(charHeight * yScale)
      y = add_u(chop_lo(static_cast<float>(chh) * t.ys), y);
    } else if (t.just & 2) {  // 0x627cd0: - baseline * yScale
      y = y - static_cast<float>(base) * t.ys;
    } else if (t.just & 4) {  // 0x627cfa: + (chop(charHeight * yScale) >> 1) - baseline * yScale
      const float half = static_cast<float>(chop_lo(static_cast<float>(chh) * t.ys) >> 1);
      y = (half - static_cast<float>(base) * t.ys) + y;
    }
    // GetStrLen 0x628ae0 at the font's xScale (+0x8, written by ScTextObject::Render); fild of the zero-extended dword
    // then fsubr: one rounding of the exact difference.
    const uint32_t len = static_cast<uint32_t>(tf.get_str_len(t.str, t.xs));
    if (t.just & 0x100) x = static_cast<float>(static_cast<double>(x) - static_cast<double>(len >> 1));  // 0x627d61
    else if (t.just & 0x200) x = static_cast<float>(static_cast<double>(x) - static_cast<double>(len));  // 0x627d8f
  }
  return font_draw(d, it, f, x, y, t.str, t.xs, t.ys, pri);
}

bool fail(std::string* err, const std::string& m) {
  if (err) *err = m;
  return false;
}
bool hex_bytes(const std::vector<std::string>& tok, size_t at, std::string& out) {  // "<n> <hex>" or "0"
  if (tok.size() <= at) return false;
  if (tok[at] == "0") return out.clear(), true;
  if (tok.size() <= at + 1) return false;
  const std::string& h = tok[at + 1];
  out.clear();
  for (size_t i = 0; i + 1 < h.size(); i += 2) out.push_back(static_cast<char>(std::stoul(h.substr(i, 2), nullptr, 16)));
  return out.size() == std::stoul(tok[at]);
}
float hexf(const std::string& s) { return fb(static_cast<uint32_t>(std::stoul(s, nullptr, 16))); }
bool small_u(int32_t v) { return v >= 0 && v < (1 << 24); }

}  // namespace

Mat4 hud_mul(const Mat4& a, const Mat4& b) {
  Mat4 o;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      float acc = a.m[i * 4] * b.m[j];
      acc = a.m[i * 4 + 1] * b.m[4 + j] + acc;
      acc = a.m[i * 4 + 2] * b.m[8 + j] + acc;
      acc = a.m[i * 4 + 3] * b.m[12 + j] + acc;
      o.m[i * 4 + j] = acc;
    }
  return o;
}

uint32_t hud_vsum(const Mat4& m) {
  static const float v[4][2] = {{-0.5f, 0.5f}, {0.5f, 0.5f}, {-0.5f, -0.5f}, {0.5f, -0.5f}};  // §3.4, buffer order
  uint32_t sum = 0;
  for (const auto& p : v)
    for (int j = 0; j < 3; ++j) {
      const float c = ((p[0] * m.m[j] + p[1] * m.m[4 + j]) + 0.0f * m.m[8 + j]) + m.m[12 + j];
      uint8_t b[4];
      std::memcpy(b, &c, 4);
      for (uint8_t k : b) sum = sum * 31u + k;
    }
  return sum;
}

Mat4 hud_sprite_world(const HudSprite& s) {
  const float w = s.x2 - s.x1, h = s.y2 - s.y1;  // GcHudSprite::SetPosition 0x526fd0: +0x198, +0x1ac
  Mat4 sc = mat_identity();
  sc.m[0] = w;
  sc.m[5] = h;
  Mat4 m = hud_mul(sc, rot_x(kSinMinusPi));  // 0x527367: S·R
  const float tx = (s.x2 - w * 0.5f) + 0.0f, ty = (s.y2 - h * 0.5f) + 0.0f, tz = 0.0f + kHudZ;  // 0x5271b5..0x5271f1
  m.m[12] = m.m[12] + tx;  // 0x52738d
  m.m[13] = m.m[13] + ty;
  m.m[14] = m.m[14] + tz;
  return m;
}

void hud_pass(Device8& d, const HudScene& s, HudBoxes* boxes) {
  g_boxes = boxes;
  struct Reset {
    ~Reset() { g_boxes = nullptr; }
  } reset;
  // BuildOrthoMatrix 0x406f20 (§3.3): m00 = 2/W, m11 = -2/H, m22 = 2/15000, row 3 (-1, +1, 1, 1), the rest +0.
  Mat4 proj;
  proj.m[0] = 2.0f / s.width;
  proj.m[5] = 2.0f / (0.0f - s.height);
  proj.m[10] = 2.0f / 15000.0f;
  proj.m[12] = -1.0f;
  proj.m[13] = 1.0f;
  proj.m[14] = 1.0f;
  proj.m[15] = 1.0f;
  Mat4 view = mat_identity();  // BuildViewMatrix 0x406030: identity, translation -0.0 (pos = 0)
  view.m[12] = view.m[13] = view.m[14] = -0.0f;
  d.set_transform(kTsProjection, proj);  // 0x406ef6
  d.set_transform(kTsView, view);        // 0x406f04
  auto render = [&](const HudItem& it, int pri) {
    if (it.sprite) return sprite_render(d, it, s.sprites[it.index], pri);
    const HudText& t = s.texts[it.index];
    return text_render(d, it, t, s.fonts[static_cast<size_t>(t.font)], pri);
  };
  // ScRenderer::Render 0x49a870: priority 0 over the children in Show order, then the unsorted priority-100 list.
  std::vector<const HudItem*> deferred;
  for (const HudItem& it : s.shown)
    if (render(it, 0) == 100) deferred.push_back(&it);
  for (const HudItem* it : deferred) render(*it, 100);
}

bool hud_scene_from_z(const std::vector<std::string>& z, const fs::FileSystem& vfs, const TexExists& tex_exists,
                      HudFontCache& cache,
                      std::vector<std::string>& tex_names, HudScene& out, std::string* err) {
  out = HudScene{};
  std::map<std::string, std::map<std::string, std::vector<std::string>>> f;  // "<kind>.<field>" -> who -> tokens
  std::vector<std::pair<uint32_t, HudItem>> shown;
  for (const std::string& line : z) {
    std::istringstream in(line);
    std::string zz, key, who;
    in >> zz >> key >> who;
    if (zz != "Z") continue;
    std::vector<std::string> tok;
    for (std::string t; in >> t;) tok.push_back(t);
    if (key == "hud.node") {  // Z hud.node <id> <kind> <obj> <order|->
      if (tok.size() != 3) return fail(err, "hud: malformed line: " + line);
      if (tok[2] == "-") continue;
      if (tok[1] == "-") return fail(err, "unmodelled: a shown HUD node holding a null object");
      HudItem it;
      it.sprite = tok[0] == "sprite";
      it.who = tok[1];
      shown.push_back({static_cast<uint32_t>(std::stoul(tok[2])), it});
    } else if (key.rfind("sprite.", 0) == 0 || key.rfind("text.", 0) == 0 || key.rfind("font.", 0) == 0) {
      f[key][who] = tok;
    }
  }
  std::sort(shown.begin(), shown.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  auto get = [&](const char* key, const std::string& who) -> const std::vector<std::string>* {
    auto k = f.find(key);
    if (k == f.end()) return nullptr;
    auto w = k->second.find(who);
    return w == k->second.end() ? nullptr : &w->second;
  };
  auto written = [](const std::vector<std::string>* t) { return t && !t->empty() && (*t)[0] != "-"; };
  std::map<std::string, int> font_index;
  for (auto& [order, it] : shown) {
    (void)order;
    if (it.sprite) {
      const auto* rect = get("sprite.rect", it.who);
      const auto* file = get("sprite.file", it.who);
      const auto* b6 = get("sprite.b6", it.who);
      if (!rect || rect->size() != 4 || !file || !b6) return fail(err, "hud: sprite " + it.who + " has no Z fields");
      if ((*b6)[0] != "00") return fail(err, "unmodelled: the 3x3 HUD sprite (b6 != 0) of " + it.who);
      HudSprite s;
      // ScSprite::SetColor 0x49fb20 (EXECUTED): doColor = the colour differs from (1,1,1,1) (0x7100bc..c8), exact compare
      // (a NaN differs); the diffuse is stored only then. Z prints the last call's arguments.
      if (const auto* c = get("sprite.color", it.who); written(c)) {
        if (c->size() != 4) return fail(err, "hud: sprite " + it.who + " colour field");
        for (int k = 0; k < 4; ++k) s.rgba[k] = hexf((*c)[static_cast<size_t>(k)]);
        s.color = !(s.rgba[0] == 1.0f && s.rgba[1] == 1.0f && s.rgba[2] == 1.0f && s.rgba[3] == 1.0f);
      }
      s.x1 = hexf((*rect)[0]), s.y1 = hexf((*rect)[1]), s.x2 = hexf((*rect)[2]), s.y2 = hexf((*rect)[3]);
      const auto* al = get("sprite.alpha", it.who);
      s.alpha = written(al) && (*al)[0] != "00";
      std::string name;
      if (!hex_bytes(*file, 0, name)) return fail(err, "hud: sprite " + it.who + " file field");
      // ScDrawableTexture::Load 0x5561c0 (EXECUTED, §4.11): with a '.' in the name, its extension is matched first; then
      // name + each entry of the table at 0x7381d0 (".dds", ".tga", then the 0 terminator) through SrBlockFileExists
      // 0x54ddc0. No match: no texture, SetTexture(stage, NULL) (M1 reaches this: turnhead0024). A dotted name and a .dds
      // hit are refused (the compare 0x65bd90 and the DDS path are not modelled).
      if (name.find('.') != std::string::npos) return fail(err, "unmodelled: HUD sprite file with a '.' (" + name + ")");
      if (tex_exists(name + ".dds")) return fail(err, "unmodelled: HUD sprite texture " + name + ".dds");
      if (const std::string tga = name + ".tga"; tex_exists(tga)) {
        auto [st, fresh] = cache.sprite_tex.emplace(tga, 0);
        if (fresh) {
          tex_names.push_back(tga);
          st->second = static_cast<uint32_t>(tex_names.size());
        }
        s.tex = st->second;
      }
      it.index = out.sprites.size();
      out.sprites.push_back(s);
    } else {
      const auto* pos = get("text.pos", it.who);
      const auto* str = get("text.string", it.who);
      const auto* fh = get("text.font", it.who);
      if (!pos || pos->size() != 3 || !str || !fh) return fail(err, "hud: text " + it.who + " has no Z fields");
      if ((*fh)[0] == "-") return fail(err, "unmodelled: a shown HUD text with no font (" + it.who + ")");
      HudText t;
      t.x = hexf((*pos)[0]), t.y = hexf((*pos)[1]), t.z = hexf((*pos)[2]);
      if (const auto* sc = get("text.scale", it.who); written(sc)) t.xs = hexf((*sc)[0]), t.ys = hexf((*sc)[1]);
      if (const auto* j = get("text.just", it.who); written(j)) t.just = static_cast<uint16_t>(std::stoul((*j)[0], nullptr, 16));
      if (const auto* j = get("text.juston", it.who); written(j)) t.juston = (*j)[0] != "00";
      if (!hex_bytes(*str, 0, t.str)) return fail(err, "hud: text " + it.who + " string field");
      for (unsigned char c : t.str)
        if (c >= 0xf0) return fail(err, "unmodelled: HUD text byte 0x" + std::to_string(c) + " (2-byte code or 0xFF callback)");
      const std::string& fw = (*fh)[0];
      auto fi = font_index.find(fw);
      if (fi == font_index.end()) {
        const auto* ff = get("font.file", fw);
        std::string path;
        if (!ff || !hex_bytes(*ff, 0, path)) return fail(err, "hud: font " + fw + " has no file field");
        if (!cache.fonts.count(path)) {
          auto lf = std::make_unique<sage::LoadedFont>();
          std::string e;
          if (!sage::load_tile_font(vfs, path, *lf, &e)) return fail(err, "hud: " + e);
          const sage::TileFont& tf = lf->font;
          bool ok = small_u(tf.char_width) && small_u(tf.char_height) && small_u(tf.baseline) && small_u(tf.max_width);
          for (const auto& g : tf.glyphs) ok = ok && small_u(g.x_offset) && small_u(g.x_offset + g.width);
          if (!ok) return fail(err, "unmodelled: font " + path + " has a field outside 0..2^24 (u32 -> f32 not exact)");
          cache.glyph_tex0[path] = static_cast<uint32_t>(tex_names.size() + 1);
          for (size_t i = 0; i < tf.glyphs.size(); ++i)
            tex_names.push_back(path + ".col/" + lf->name + std::to_string(i / static_cast<size_t>(tf.columns)) + "_" +
                                std::to_string(i % static_cast<size_t>(tf.columns)) + ".tga");
          cache.fonts[path] = std::move(lf);
        }
        HudFont hf;
        hf.font = &cache.fonts[path]->font;
        hf.images = &cache.fonts[path]->glyph_images;
        hf.glyph_tex0 = cache.glyph_tex0[path];
        if (const auto* c = get("font.color", fw); written(c))
          for (int k = 0; k < 4; ++k) hf.color[k] = hexf((*c)[static_cast<size_t>(k)]);
        if (const auto* tr = get("font.transp", fw); written(tr)) hf.transp = (*tr)[0] != "00";
        fi = font_index.emplace(fw, static_cast<int>(out.fonts.size())).first;
        out.fonts.push_back(hf);
      }
      t.font = fi->second;
      it.index = out.texts.size();
      out.texts.push_back(t);
    }
    out.shown.push_back(it);
  }
  return true;
}

}  // namespace lomn::view
