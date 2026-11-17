// M2a-3 T6a: the HUD setters (hud.md §4.1-§4.4) as tier 1, the HUD director's show/hide, and the `Z` lines of hud.md
// §9.2 they feed. One citation per binding (natives.md §1.3). Every setter takes `this` = w0 without a null test (a null
// object is an access violation in the original: an Undefined error here, re-reported by the caller's handler).
#include "lomn/osi/hud.h"

#include <cstdio>
#include <cstring>

#include "lomn/osi/objects.h"
#include "lomn/osi/services.h"

namespace lomn::osi {

namespace {
HostContext& host(void* user) { return *static_cast<HostContext*>(user); }
uint32_t w(NativeCall& c, size_t i) { return i < c.args.size() ? c.args[i]->value : 0u; }

std::string hex32(std::initializer_list<uint32_t> v) {
  std::string o;
  char b[12];
  for (uint32_t x : v) {
    std::snprintf(b, sizeof b, o.empty() ? "%08x" : " %08x", x);
    o += b;
  }
  return o;
}
std::string hexn(uint32_t v, int digits) {
  char b[12];
  std::snprintf(b, sizeof b, "%0*x", digits, v);
  return b;
}
// a byte string as on `S` lines (trace.md §1): `%d <hex>`, empty `0`
std::string bytes(const std::string& s) {
  std::string o = std::to_string(s.size());
  if (!s.empty()) o += ' ';
  char b[4];
  for (unsigned char ch : s) {
    std::snprintf(b, sizeof b, "%02x", ch);
    o += b;
  }
  return o;
}
std::string handle_str(HostContext& ctx, uint32_t h) {
  char b[24];
  std::snprintf(b, sizeof b, "%04x:%08x", ctx.heap().game_type(h), h);
  return b;
}
// a GcBigSprite / ScTextObject / ScTileFont colour component, f32(x * f32 0x3b808081) ([0x703c90..] for the sprite,
// [0x7581cc..] for the font; hud.md §4.1/§4.4). x is a byte or an int whose exact product fits a double (|x| < 2^29),
// so one f32 rounding at either precision control gives the stored value.
uint32_t colour(int64_t x) {
  float k;
  const uint32_t kb = 0x3b808081u;
  std::memcpy(&k, &kb, 4);
  return f32_bits(static_cast<float>(static_cast<double>(x) * static_cast<double>(k)));
}

// `this` = w0: the handle of a live game object (0x82's ConvertVariant typed it); nothing -> the null dereference.
uint32_t self(NativeCall& c, const char* what) {
  if (c.args.empty() || c.args[0]->type == kNothing) throw VmError(std::string("Undefined: null object in ") + what);
  return c.args[0]->value;
}
void set(HostContext& ctx, uint32_t h, const char* field, std::string v) {
  ctx.subsystem<HudModel>().fields[h][field] = std::move(v);
}

// ---- GcBigSprite (hud.md §4.1) ----

// body 0x43dd60 -> SetZDepth 0x441ac0; asm pin: big +0x3c and each tile's +0x188 = raw f32 (0x536906). No render effect.
Variant n_setzdepth(NativeCall& c, void* u) {
  set(host(u), self(c, "gcbigsprite.setzdepth"), "zdepth", hex32({w(c, 1)}));
  return nothing();
}
// body 0x43dd30 -> 0x4408f0; asm pin: big +0x2b and each tile's +0x51 (doAlpha) = the low byte of w1.
Variant n_enablealphachannel(NativeCall& c, void* u) {
  set(host(u), self(c, "gcbigsprite.enablealphachannel"), "alpha", hexn(w(c, 1) & 0xff, 2));
  return nothing();
}
// body 0x43db80 -> SetHudPosition 0x441860; asm pin: big +0x48..+0x54 = x1, y1, x2, y2 raw f32.
Variant n_sethudposition(NativeCall& c, void* u) {
  set(host(u), self(c, "gcbigsprite.sethudposition"), "rect", hex32({w(c, 1), w(c, 2), w(c, 3), w(c, 4)}));
  return nothing();
}
// body 0x43ddc0 -> 0x440970; asm pin: the low bytes of w1..w4 * [0x703c90..0x703c9c] (0x3b808081) -> ScSprite::SetColor.
Variant n_sprite_setcolor(NativeCall& c, void* u) {
  set(host(u), self(c, "gcbigsprite.setcolor"), "color",
      hex32({colour(w(c, 1) & 0xff), colour(w(c, 2) & 0xff), colour(w(c, 3) & 0xff), colour(w(c, 4) & 0xff)}));
  return nothing();
}

// ---- ScTextObject (hud.md §4.3) ----

// body 0x611e50; asm pin: +0x13c/+0x140/+0x144 = w1, w2, w3 raw (0x611e7d / 0x611e85 / 0x611e8d).
Variant n_text_setposition(NativeCall& c, void* u) {
  set(host(u), self(c, "sctextobject.setposition"), "pos", hex32({w(c, 1), w(c, 2), w(c, 3)}));
  return nothing();
}
// body 0x6120d0; asm pin: +0x15c = w1, +0x160 = w2 raw (0x6120f2 / 0x6120fa).
Variant n_text_setscale(NativeCall& c, void* u) {
  set(host(u), self(c, "sctextobject.setscale"), "scale", hex32({w(c, 1), w(c, 2)}));
  return nothing();
}
// body 0x612000; asm pin: word +0x2a = the low 16 bits of w1 (0x612014).
Variant n_text_setjustification(NativeCall& c, void* u) {
  set(host(u), self(c, "sctextobject.setjustification"), "just", hexn(w(c, 1) & 0xffff, 4));
  return nothing();
}
// body 0x612030; asm pin: byte +0x28 = the low byte of w1 (0x612043).
Variant n_text_enablejustification(NativeCall& c, void* u) {
  set(host(u), self(c, "sctextobject.enablejustification"), "juston", hexn(w(c, 1) & 0xff, 2));
  return nothing();
}
// body 0x611f40; asm pin: ScFixedString::operator= at 0x611f57 (ecx = obj+0x30): the bytes up to the first NUL.
Variant n_text_setstring(NativeCall& c, void* u) {
  HostContext& ctx = host(u);
  const uint32_t h = self(c, "sctextobject.setstring");
  if (c.args.size() < 2 || c.args[1]->type != kString) throw VmError("Undefined: string argument expected");
  set(ctx, h, "string", bytes(osi_cstring(ctx.heap().text(c.args[1]->value))));
  return nothing();
}
// body 0x611f90; asm pin: the same font -> nothing (0x611fa9); else inc the new one (0x611fb1, skipped for null), dec
// the old one (0x611fbd, Free at 0 0x611fc8, skipped for null), store +0x2c (0x611fce). A null font prints `-`.
Variant n_text_setfont(NativeCall& c, void* u) {
  HostContext& ctx = host(u);
  const uint32_t h = self(c, "sctextobject.setfont");
  ScTileFont* nf = ctx.object_arg<ScTileFont>(c, 1);
  auto* t = dynamic_cast<ScTextObject*>(ctx.object(h));
  ScTileFont* of = t ? t->font() : nullptr;
  if (t && of == nf) return nothing();
  if (nf) ctx.add_ref(nf);
  if (t) t->set_font(nf);
  if (of) ctx.release(of);
  set(ctx, h, "font", nf && nf->handle() ? handle_str(ctx, nf->handle()) : std::string("-"));
  return nothing();
}

// ---- ScTileFont (hud.md §4.4) ----

// body 0x611a00 -> ScTileFont::SetColor 0x627c20 (0x611a23); asm pin: +0x24..+0x30 = fild w1..w4, fmul [0x7581cc..d8].
// |w| >= 2^29 would make the product round twice at pc53: not modelled, it fails closed.
Variant n_font_setcolor(NativeCall& c, void* u) {
  const uint32_t h = self(c, "sctilefont.setcolor");
  uint32_t v[4];
  for (int i = 0; i < 4; ++i) {
    const int32_t x = static_cast<int32_t>(w(c, i + 1));
    if (x >= (1 << 29) || x <= -(1 << 29)) throw VmError("unmodelled: sctilefont.setcolor component beyond 2^29");
    v[i] = colour(x);
  }
  set(host(u), h, "color", hex32({v[0], v[1], v[2], v[3]}));
  return nothing();
}
// body 0x611b70; asm pin: byte +0xb0 = the low byte of w1 (0x611b83).
Variant n_font_enabletransparency(NativeCall& c, void* u) {
  set(host(u), self(c, "sctilefont.enabletransparency"), "transp", hexn(w(c, 1) & 0xff, 2));
  return nothing();
}

// ---- GcHudDirector::Show 0x47b320 / Hide 0x47b3e0 (hud.md §4.2) ----
// The id is looked up in the item table 0x782458 (0x47ba00 -> 0x650640, a linear search; a miss returns 0 at 0x650673
// and the caller then reads [0+0x10]: Undefined). mainHud ([0x83a534]) vfunc +0x40 is asked whether a child holds the
// node's object ([node+0x10], the ScRenderNode ctor's held pointer 0x4ac784/0x4ac786). Show: held -> nothing
// (0x47b36f); else push_back on the child vector +0x44 (0x47b3a6). Hide: not held -> nothing (0x47b42f); else the
// node itself is searched in the vector (0x47b46b-0x47b471) and erased if present (0x4197c0 at 0x47b4a7).
GameObject* node_object(HostContext& ctx, uint32_t id, const char* what) {
  auto& el = ctx.subsystem<HudDirector>().elements;
  auto it = el.find(id);
  if (it == el.end()) throw VmError(std::string("Undefined: ") + what + " of an unknown HUD id (0x650673 returns 0)");
  return it->second;
}
bool object_shown(HostContext& ctx, GameObject* o) {
  auto& el = ctx.subsystem<HudDirector>().elements;
  for (uint32_t id : ctx.subsystem<HudModel>().shown)
    if (el.at(id) == o) return true;
  return false;
}
Variant n_hud_show(NativeCall& c, void* u) {
  HostContext& ctx = host(u);
  const uint32_t id = w(c, 0);
  GameObject* o = node_object(ctx, id, "GcHudDirector::Show");
  if (!object_shown(ctx, o)) ctx.subsystem<HudModel>().shown.push_back(id);
  return nothing();
}
Variant n_hud_hide(NativeCall& c, void* u) {
  HostContext& ctx = host(u);
  const uint32_t id = w(c, 0);
  GameObject* o = node_object(ctx, id, "GcHudDirector::Hide");
  if (!object_shown(ctx, o)) return nothing();
  auto& s = ctx.subsystem<HudModel>().shown;
  for (auto it = s.begin(); it != s.end(); ++it)
    if (*it == id) {
      s.erase(it);
      break;
    }
  return nothing();
}
}  // namespace

void hud_created_sprite(HostContext& ctx, uint32_t h, const uint32_t rect[4], const std::string& file, uint8_t b6) {
  auto& f = ctx.subsystem<HudModel>().fields[h];
  f.clear();
  f["rect"] = hex32({rect[0], rect[1], rect[2], rect[3]});
  f["file"] = bytes(file);
  f["b6"] = hexn(b6, 2);
}
void hud_created_text(HostContext& ctx, uint32_t h, const uint32_t pos[3], GameObject* font, const std::string& s) {
  auto& f = ctx.subsystem<HudModel>().fields[h];
  f.clear();
  f["pos"] = hex32({pos[0], pos[1], pos[2]});
  f["font"] = font && font->handle() ? handle_str(ctx, font->handle()) : std::string("-");
  f["string"] = bytes(s);
}
void hud_created_font(HostContext& ctx, uint32_t h, const std::string& file) {
  auto& f = ctx.subsystem<HudModel>().fields[h];
  f.clear();
  f["file"] = bytes(file);
}

std::vector<std::string> hud_snapshot_lines(HostContext& ctx) {
  static const char* const kSprite[] = {"alpha", "b6", "color", "file", "rect", "zdepth"};
  static const char* const kText[] = {"font", "just", "juston", "pos", "scale", "string"};
  static const char* const kFont[] = {"color", "file", "transp"};
  HudModel& m = ctx.subsystem<HudModel>();
  Heap& heap = ctx.heap();
  std::vector<std::string> z;
  for (uint32_t h : heap.live_games()) {
    const uint16_t t = heap.game_type(h);
    const char* kind = t == m.sprite_type ? "sprite" : t == m.text_type ? "text" : t == m.font_type ? "font" : nullptr;
    if (!kind || !t) continue;
    const auto it = m.fields.find(h);
    auto emit = [&](const char* const* keys, size_t n) {
      for (size_t i = 0; i < n; ++i) {
        std::string v = "-";
        if (it != m.fields.end()) {
          auto f = it->second.find(keys[i]);
          if (f != it->second.end()) v = f->second;
        }
        z.push_back(std::string("Z ") + kind + "." + keys[i] + " " + handle_str(ctx, h) + " " + v);
      }
    };
    if (kind[0] == 's') emit(kSprite, 6);
    else if (kind[0] == 't') emit(kText, 6);
    else emit(kFont, 3);
  }
  // one line per node the director wrapped (none is ever removed on the port's path)
  const auto& el = ctx.subsystem<HudDirector>().elements;
  for (const auto& [id, o] : el) {
    std::string order = "-";
    for (size_t i = 0; i < m.shown.size(); ++i)
      if (m.shown[i] == id) order = std::to_string(i);
    std::string kind = "-", obj = "-";
    if (o && o->handle()) {
      kind = o->type() == m.text_type ? "text" : "sprite";
      obj = handle_str(ctx, o->handle());
    }
    char b[16];
    std::snprintf(b, sizeof b, "%08x", id);
    z.push_back(std::string("Z hud.node ") + b + " " + kind + " " + obj + " " + order);
  }
  return z;
}

void bind_hud_group(NativeRegistry& reg, HostContext& ctx) {
  struct Row {
    const char* cls;
    const char* fn;
    NativeFn f;
  };
  static const Row rows[] = {
      {"gcbigsprite", "setzdepth", n_setzdepth},
      {"gcbigsprite", "enablealphachannel", n_enablealphachannel},
      {"gcbigsprite", "sethudposition", n_sethudposition},
      {"gcbigsprite", "setcolor", n_sprite_setcolor},
      {"sctextobject", "setposition", n_text_setposition},
      {"sctextobject", "setscale", n_text_setscale},
      {"sctextobject", "setjustification", n_text_setjustification},
      {"sctextobject", "enablejustification", n_text_enablejustification},
      {"sctextobject", "setstring", n_text_setstring},
      {"sctextobject", "setfont", n_text_setfont},
      {"sctilefont", "setcolor", n_font_setcolor},
      {"sctilefont", "enabletransparency", n_font_enabletransparency},
      {"gchuddirector", "show", n_hud_show},
      {"gchuddirector", "hide", n_hud_hide},
  };
  HudModel& m = ctx.subsystem<HudModel>();
  if (const NativeEntry* e = reg.find("gcbigsprite", "loadhudsprite")) m.sprite_type = e->ret_type;
  if (const NativeEntry* e = reg.find("sctextobject", "create")) m.text_type = e->ret_type;
  if (const NativeEntry* e = reg.find("sctilefont", "create")) m.font_type = e->ret_type;
  for (const Row& r : rows)
    if (reg.find(r.cls, r.fn)) reg.bind(std::string(r.cls) + "." + r.fn, r.f, &ctx, Tier::Implemented);
}

}  // namespace lomn::osi
