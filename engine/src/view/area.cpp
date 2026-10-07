#include "lomn/view/area.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <tuple>
#include <set>

#include "lomn/d3d/device8.h"
#include "lomn/d3d/ffp.h"
#include "lomn/sage/blockchain.h"
#include "lomn/sage/image.h"
#include "lomn/sage/osi.h"
#include "lomn/sage/xfile.h"
#include "lomn/view/sage_frame.h"

namespace lomn::view {
namespace {

using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  return s;
}
bool ends_with(const std::string& s, const char* suf) {
  const size_t n = std::strlen(suf);
  return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}
// Warnings and errors may embed file bytes (4CC ids, texture names) or path names: every byte outside printable ASCII
// becomes \xHH, so logs and JSON stay ASCII and native == wasm32.
std::string escape_bytes(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (unsigned char c : s) {
    if (c >= 0x20 && c < 0x7f) {
      o.push_back(static_cast<char>(c));
    } else {
      char b[5];
      std::snprintf(b, sizeof b, "\\x%02x", c);
      o += b;
    }
  }
  return o;
}
bool fail(std::string* err, const std::string& msg) {
  if (err) *err = escape_bytes(msg);
  return false;
}
bool finite3(const float v[3]) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }
// Task 8B D6: a double -> float conversion of a finite value outside the float range is undefined behaviour; false for
// such values (and for NaN/inf), else the converted value.
bool to_float(double v, float& out) {
  if (!std::isfinite(v) || std::fabs(v) > double(std::numeric_limits<float>::max())) return false;
  out = static_cast<float>(v);
  return true;
}

bool route_enabled(sage::Route r, const AreaOptions& o) {
  switch (r) {
    case sage::Route::Main: return o.draw_main;
    case sage::Route::Trans: return o.draw_trans;
    case sage::Route::Back: return o.draw_back;
    case sage::Route::Moveable: return o.draw_moveable;
    case sage::Route::NoLightMoveable: return o.draw_nolight;
    default: return false;
  }
}

class Loader {
 public:
  Loader(const fs::FileSystem& fs, AreaData& out) : fs_(fs), chain_(fs), out_(out) {}
  LoadError kind() const { return kind_; }

  bool run(std::string_view level, std::string_view area, const AreaOptions& opt, std::string* err) {
    const auto t0 = Clock::now();
    // Caps: AreaOptions may lower them (tests); values above the defaults are clamped to the defaults.
    max_placements_ = std::min(opt.max_placements, kMaxPlacements);
    max_draws_ = std::min(opt.max_draws, kMaxDraws);
    max_textures_ = std::min(opt.max_textures, kMaxAreaTextures);
    max_texture_bytes_ = std::min(opt.max_texture_bytes, kMaxAreaTextureBytes);
    max_geometry_bytes_ = std::min(opt.max_geometry_bytes, kMaxAreaGeometryBytes);
    max_lights_ = std::min(opt.max_lights, kMaxAreaLights);
    out_.level = std::string(level.substr(0, 4));
    out_.area = std::string(area.substr(0, 4));
    std::string e;
    // GcGame::LoadArtBlockHeader: Art.blk is the global fallback of both lookup orders. Optional here.
    out_.art_blk = chain_.open_art("Root/Data/Art/Art.blk", &e);
    if (!out_.art_blk) warn("art.blk not opened: " + e);
    if (!chain_.enter_area(out_.level, out_.area, &e)) {
      kind_ = enter_failure_kind();
      return fail(err, "area: " + e);
    }
    kind_ = LoadError::Data;  // every later failure is a cap or a data error
    prefix_ = "Root/Data/Levels/" + out_.level + "/" + out_.area;  // "%.4s/%.4s" area prefix (GcAreaLoader::LoadArea)
    // levelType: LoadArea pushes (uint8)(levelName[3] - '0') (0x004ffd0d: movzx byte [edi+3]; add -0x30).
    out_.level_type = static_cast<uint8_t>(static_cast<uint8_t>(out_.level.size() > 3 ? out_.level[3] : 0) - '0');
    load_spec_textures();

    std::vector<sage::ObjRecord> recs;
    if (load_table("_obj.slb", e)) {
      sage::Slb slb;
      if (!slb.parse(std::move(bytes_), &e) || !sage::parse_obj(slb, recs, &e)) {
        warn("obj table rejected: " + e);  // the game's loader throws and LoadArea skips it: nothing is placed
        recs.clear();
      }
    } else {
      warn("no obj table: " + e);
    }
    if (load_table("_pos.slb", e)) {
      sage::Slb slb;
      std::vector<sage::PosRecord> pos;
      if (slb.parse(std::move(bytes_), &e) && sage::parse_pos(slb, pos, &e)) {
        out_.pos_records = static_cast<uint32_t>(pos.size());
        for (uint32_t i = 0; i < pos.size(); ++i)
          if (pos[i].flags & kPosStartFlag) {
            out_.has_start = true;
            out_.start_record = i;
            std::memcpy(out_.start, pos[i].pos, sizeof out_.start);
            break;
          }
      } else {
        warn("pos table rejected: " + e);
      }
    } else {
      warn("no pos table: " + e);
    }

    AreaStats& st = out_.stats;
    st.obj_records = static_cast<uint32_t>(recs.size());
    const Mat4 rot = mat_rotation_x(kWorldRotX);
    uint32_t drawable = 0;
    for (uint32_t i = 0; i < recs.size(); ++i) {  // bounded by parse_obj (n * 60 <= table bytes) and kMaxDrawableRecords
      const sage::ObjRecord& r = recs[i];
      const sage::Route rt = sage::route(r.flags);
      if (rt == sage::Route::Ignored) {
        ++st.ignored_records;
        continue;
      }
      if (!route_enabled(rt, opt)) {
        ++st.disabled_records;
        continue;
      }
      // Every record routed to a drawn group costs a model lookup (or a cached miss) even when nothing is placed: budget.
      if (++drawable > kMaxDrawableRecords) return fail(err, "area: more than kMaxDrawableRecords drawable records");
      // A NaN/Inf position or orientation would poison the world matrix, the bounds and the trans sort: skip the record.
      if (!finite3(r.pos) || !finite3(r.orient)) {
        ++st.nonfinite_records;
        warn("obj record " + std::to_string(i) + ": non-finite position or orientation (skipped)");
        continue;
      }
      if (out_.placements.size() >= max_placements_) return fail(err, "area: more than kMaxPlacements drawn records");
      uint32_t model = 0;
      if (!model_for(sage::fourcc(r.id), model)) {
        if (!fatal_.empty()) return fail(err, fatal_);  // a cap was exceeded
        ++st.missing_models;
        continue;
      }
      Placement p;
      p.record = i;
      p.model = model;
      p.route = rt;
      p.flags = r.flags;
      std::memcpy(p.pos, r.pos, sizeof p.pos);
      std::memcpy(p.orient, r.orient, sizeof p.orient);
      if (r.orient[0] != 0.0f || r.orient[1] != 0.0f || r.orient[2] != 0.0f) ++st.placements_with_orientation;
      p.world = mat_mul(rot, mat_translation(r.pos[0], r.pos[1], r.pos[2]));
      st.routes[static_cast<int>(rt)]++;
      const uint64_t draws = uint64_t(st.draws) + out_.models[model].subsets.size();
      if (draws > max_draws_) return fail(err, "area: more than kMaxDraws draws");
      st.draws = static_cast<uint32_t>(draws);
      if (rt != sage::Route::Back) grow_bounds(out_.models[model], p.world);
      out_.placements.push_back(p);
    }
    apply_spec_textures();
    load_lights();
    load_script();
    st.placements = static_cast<uint32_t>(out_.placements.size());
    st.models = static_cast<uint32_t>(out_.models.size());
    st.textures = static_cast<uint32_t>(out_.textures.size());
    out_.warnings.assign(warnings_.begin(), warnings_.end());  // sorted, deduplicated (std::set)
    out_.warnings_dropped = warnings_dropped_;
    st.load_ms = ms_since(t0) - st.decode_ms;
    return true;
  }

 private:
  // At most kMaxStoredWarnings distinct warnings are kept (the first ones emitted, in load order); the rest are counted.
  void warn(const std::string& w) {
    std::string e = escape_bytes(w);
    if (warnings_.count(e)) return;
    if (warnings_.size() >= kMaxStoredWarnings) {
      ++warnings_dropped_;
      return;
    }
    warnings_.insert(std::move(e));
  }
  bool fatal(const std::string& why) {
    fatal_ = why;
    return false;
  }

  bool load_table(const char* suffix, std::string& e) {
    // "Root/Data/Levels/%.4s/%.4s/%.4s_obj.slb" through SrBlockFileLoad (current -> second -> Art).
    return chain_.load(prefix_ + "/" + out_.area + suffix, bytes_, nullptr, &e);
  }

  // SetCurrentLevel loads Root/Data/Levels/<lev>/textures.slb into GcSpecTextureLoader (the level blockfile is current).
  void load_spec_textures() {
    std::string e;
    if (!chain_.load("Root/Data/Levels/" + out_.level + "/textures.slb", bytes_, nullptr, &e)) {
      warn("no textures.slb: " + e);
      return;
    }
    sage::Slb slb;
    std::vector<sage::SpecTextureRecord> recs;
    if (!slb.parse(std::move(bytes_), &e) || !sage::parse_spec_textures(slb, recs, &e)) {
      warn("textures.slb rejected: " + e);
      recs.clear();
    }
    // Task 8B D3: a record with a non-finite transparency or scroll speed is skipped with a warning (it would reach the
    // TFACTOR rounding and the texture matrix). Finite values are kept as the file has them.
    spec_.clear();
    for (size_t i = 0; i < recs.size(); ++i) {
      const sage::SpecTextureRecord& r = recs[i];
      if (!std::isfinite(r.trans) || !std::isfinite(r.u) || !std::isfinite(r.v)) {
        warn("textures.slb record " + std::to_string(i) + ": non-finite value (skipped)");
        continue;
      }
      spec_.push_back(r);
    }
    out_.special_textures = static_cast<uint32_t>(spec_.size());
  }
  // GcSpecTextureLoader::LoadTextures for every placed model (GcArea / moveable creation): a record applies to the
  // materials of used attributes whose texture path equals "<lev>/textures/<name>" (strcmp: case-sensitive), unless it
  // is (1, 0, 0) (GcArea::SetSpecialTexture 0x00505710). trans < 1 -> SetTransparency; (u, v) != 0 -> SetUVOffset
  // (ScDrawableModel::SetSpecialTexture 0x00553da0). Animated textures (frames != 0) are out of scope.
  // Task 8B D1: the records are indexed by name once (record indices in table order per name), so applying them costs
  // O(subsets x log records) instead of comparing every subset with every record; duplicates still apply in table order.
  void apply_spec_textures() {
    std::map<std::string, std::vector<uint32_t>> by_name;
    for (uint32_t i = 0; i < spec_.size(); ++i) by_name[spec_[i].name].push_back(i);  // bounded by kMaxSpecTextures
    for (Model& m : out_.models)
      for (SubsetDraw& sd : m.subsets) {
        if (!sd.textured) continue;
        const auto it = by_name.find(sd.texture_name);
        if (it == by_name.end()) continue;
        for (uint32_t ri : it->second) {
          const sage::SpecTextureRecord& r = spec_[ri];
          if (r.trans == 1.0f && r.u == 0.0f && r.v == 0.0f) continue;
          if (r.trans < 1.0f) sd.trans = r.trans;
          if (r.u != 0.0f || r.v != 0.0f) {
            sd.uv_offset[0] = r.u;
            sd.uv_offset[1] = r.v;
          }
          if (!sd.special) ++out_.special_matches;
          sd.special = true;
        }
      }
  }
  // GcLightLoader::LoadLights (0x00470010) + GrSetDefaultLighting (0x00470560); lights as GcLight builds them
  // (GcFastLight::SetPointLight 0x0058d6a0 / SetDirectionalLight 0x0058d770 / SetAmbientLight 0x0058d8c0 / SetIntensity
  // 0x0058ddf0). A missing or rejected table throws in the game and LoadArea calls GrSetDefaultLighting.
  static d3d::Color4 col(float r, float g, float b, float a) { return d3d::Color4{r, g, b, a}; }
  static void intensity(SceneLight& s, const d3d::Color4& c, float v) {
    if (v > 0.9f) v *= 1.2f;  // GcLight ctors: intensity above 0.9 is scaled by 1.2 (@0x0070c16c / 0x0070c170)
    s.base = c;
    s.light.diffuse = s.light.specular = col(c.r * v, c.g * v, c.b * v, c.a * v);
  }
  void add_directional(const std::string& name, const d3d::Color4& c, float v, float x, float y, float z) {
    SceneLight s;
    s.name = name;
    s.light.type = d3d::kLightDirectional;
    s.light.direction[0] = x;
    s.light.direction[1] = y;
    s.light.direction[2] = z;
    intensity(s, c, v);
    add_light(s);
  }
  // Task 8B D4: every light the area adds (LIGHT records, their Shad fill, the defaults) goes through here, so the cap
  // counts them all; the ones over it are counted and reported in one warning.
  void add_light(const SceneLight& s) {
    if (out_.lights.size() >= max_lights_) {
      ++lights_dropped_;
      return;
    }
    out_.lights.push_back(s);
  }
  void default_lighting(uint8_t type) {
    if (type < 3) {  // "LmbA": a directional light with only an ambient colour (0.4706 grey @0x00709364)
      SceneLight s;
      s.name = "LmbA";
      s.light.type = d3d::kLightDirectional;
      s.light.ambient = s.base = col(0.47058824f, 0.47058824f, 0.47058824f, 1.0f);
      s.light.attenuation0 = 1.0f;  // @0x0073e938
      s.light.range = 10000.0f;     // kMaxLightRange @0x0073e8f4
      add_light(s);
      return;
    }
    d3d::Color4 sun = col(1, 1, 1, 1);
    if (type == 3) sun = col(1.0f, 0.98039216f, 0.83529413f, 1);
    else if (type == 5) sun = col(0.95686275f, 1.0f, 0.86666667f, 1);
    add_directional("SunL", sun, 1.0f, 0.85570401f, -0.51642299f, 0.0328423f);
    const d3d::Color4 shad = type == 4 ? col(0.35294119f, 0.47058824f, 0.54901963f, 1) : col(0.35294119f, 0.35294119f, 0.35294119f, 1);
    add_directional("Shad", shad, 1.0f, -0.85570401f, 0.51642299f, -0.0328423f);
  }
  void load_lights() {
    std::vector<sage::LightRecord> recs;
    std::string e;
    bool table = load_table("_light.slb", e);
    if (table) {
      sage::Slb slb;
      if (!slb.parse(std::move(bytes_), &e) || !sage::parse_light(slb, recs, &e)) {
        warn("light table rejected: " + e);
        table = false;
      }
    }
    if (!table) {
      default_lighting(out_.level_type);
      out_.lights_source = "default";
      warn_dropped_lights();
      return;
    }
    bool directional = false, shad = false;
    for (size_t ri = 0; ri < recs.size(); ++ri) {  // bounded by parse_light (n * 44 <= table bytes)
      const sage::LightRecord& r = recs[ri];
      // Task 8B D3: a NaN/inf position, intensity, range or colour would reach the lighting uniforms: the record is
      // skipped with a warning (like a non-finite OBJ placement). It then counts neither as a directional nor for Shad.
      bool finite = finite3(r.pos) && std::isfinite(r.range) && std::isfinite(r.param);
      for (float c : r.color) finite = finite && std::isfinite(c);
      if (!finite) {
        warn("light record " + std::to_string(ri) + ": non-finite value (skipped)");
        continue;
      }
      uint8_t style = 0;
      if (r.flags & 2) style = 1;
      else if (r.flags & 4) style = 3;
      else if (r.flags & 8) style = 4;
      else if (r.flags & 0x10) style = 5;
      else if (r.flags & 0x20) style = 2;
      const d3d::Color4 c = col(r.color[0], r.color[1], r.color[2], r.color[3]);
      if (r.flags & 0x40) {
        SceneLight s;
        s.name = sage::fourcc(r.id);
        s.style = style;
        d3d::Light8& l = s.light;
        l.type = d3d::kLightPoint;
        for (int k = 0; k < 3; ++k) l.position[k] = r.pos[k];
        if (r.param > 0.0f) {  // SetPointLight: range > 0 (@0x0073e908)
          l.attenuation0 = 0.0f;
          // 1.0 / (kFarIntensity 0.15 * range). Task 8B D3: for a tiny (denormal) range the float product underflows to 0
          // and 1/0 is a float divide by zero (UBSan). The game's x87 code keeps the product in an 80-bit register (no
          // underflow) and stores 1/x as +inf; we store FLT_MAX instead (finite: it reaches the uniforms). Ranges whose
          // product is a normal float keep the float formula bit for bit.
          const float prod = 0.15f * r.param;
          l.attenuation1 = prod >= std::numeric_limits<float>::min() ? 1.0f / prod : std::numeric_limits<float>::max();
          l.range = r.param;
        } else {
          l.attenuation0 = 1.0f;
          l.attenuation1 = 0.0f;
          l.range = 10000.0f;
        }
        intensity(s, c, r.range);
        add_light(s);
      } else if (r.flags & 0x80) {
        directional = true;
        add_directional(sage::fourcc(r.id), out_.level_type == 4 ? col(1, 1, 1, 1) : c, r.range, r.pos[0], r.pos[1], r.pos[2]);
        if (!shad) {  // one opposite fill light "Shad" for the first directional record
          shad = true;
          const d3d::Color4 sc = out_.level_type == 4 ? col(0.35294119f, 0.47058824f, 0.54901963f, 1) : col(0.35294119f, 0.35294119f, 0.35294119f, 1);
          add_directional("Shad", sc, r.range, -r.pos[0], -r.pos[1], -r.pos[2]);
        }
      }
    }
    out_.lights_source = "light_slb";
    if (!directional) {  // LoadLights calls GrSetDefaultLighting when no 0x80 record was found
      default_lighting(out_.level_type);
      out_.lights_source = "light_slb+default";
    }
    warn_dropped_lights();
  }
  void warn_dropped_lights() {
    if (lights_dropped_) warn("lights: " + std::to_string(lights_dropped_) + " over kMaxAreaLights dropped");
  }
  // The area script's literals (lomn/sage/osi.h): class "lego_area_<area>" (level 1) or "lego_area_<area><level[3]>",
  // method InitFull: GcAreaDirector::SetFog(color, start, end, density) stores (color, -start, -end, density)
  // (0x0048a7a0); GcLightDirector::SetCaveLighting(b). INFERRED class naming: 91 of the 112 lego_area_* classes in Base.osi
  // map to an existing area this way (tools/research/osi.py).
  void load_script() {
    std::vector<uint8_t> bytes;
    if (fs_.read("Root/Data/Base.osi", bytes) != fs::Error::None) {
      warn("osi: Root/Data/Base.osi not readable (fog/cave lighting from defaults)");
      return;
    }
    sage::OsiFile osi;
    std::string e;
    if (!osi.parse(std::move(bytes), &e)) {
      warn(e);
      return;
    }
    std::string cls = "lego_area_" + out_.area;
    if (out_.level_type != 1 && out_.level.size() > 3) cls += out_.level[3];  // lev2 -> "...2" (the level digit)
    if (!osi.find_class(cls)) return;
    out_.script_class = cls;
    std::vector<sage::OsiCall> calls;
    if (!osi.eval_calls(cls, "initfull", calls, &e)) {
      warn(e);
      return;
    }
    for (const sage::OsiCall& c : calls) {
      const std::string key = lower(c.cls) + "." + lower(c.fn);
      double v[4];
      // Literal colours come from an untrusted file: only integers in the 32-bit range (signed or unsigned) are kept,
      // so the conversion below is always defined.
      auto colour_ok = [](const sage::OsiValue& x) { return x.kind == sage::OsiValue::Int && x.i >= INT32_MIN && x.i <= int64_t(UINT32_MAX); };
      // Task 8B D5: every SetFog whose four values are known literals is applied in call order, so the LAST one wins,
      // as in the game (GcAreaDirector::SetFog 0x0048a7a0 only overwrites the GcGraphicsOptions statics). The first one
      // used to win.
      if (key == "gcareadirector.setfog" && c.args.size() == 4 && colour_ok(c.args[0]) && c.args[0].number(v[0]) &&
          c.args[1].number(v[1]) && c.args[2].number(v[2]) && c.args[3].number(v[3])) {
        float st = 0, en = 0, de = 0;
        // Task 8B D3/D6: a NaN/inf value, or a finite double outside the float range (an undefined conversion), is
        // ignored with a warning instead of reaching the fog uniforms.
        if (!to_float(v[1], st) || !to_float(v[2], en) || !to_float(v[3], de)) {
          warn("osi: SetFog with a non-finite or out-of-range value (ignored)");
          continue;
        }
        out_.fog.color = static_cast<uint32_t>(c.args[0].i);
        out_.fog.start = -st;
        out_.fog.end = -en;
        out_.fog.density = de;
        out_.fog.source = "osi";
      } else if (key == "gclightdirector.setcavelighting" && c.args.size() == 1 && c.args[0].number(v[0])) {
        out_.cave_lighting = v[0] != 0.0;
      }
    }
    if (out_.cave_lighting)  // SetCaveLighting(1) 0x0048e670: active style-0 lights get SetIntensity(0.25) (@0x0070bd68)
      for (SceneLight& l : out_.lights)
        if (l.style == 0) {
          const d3d::Color4& b = l.base;  // GcFastLight +0x78: the colour before any intensity
          l.light.diffuse = l.light.specular = col(b.r * 0.25f, b.g * 0.25f, b.b * 0.25f, b.a * 0.25f);
        }
  }

  // Model by 4CC id, cached per area (ScDrawableModel::Create keeps a map keyed by the full path; Find ignores case).
  // False when the model is missing or rejected (a warning); false with fatal_ set when a cap is exceeded.
  bool model_for(const std::string& id, uint32_t& index) {
    const std::string key = lower(id);
    auto it = model_cache_.find(key);
    if (it != model_cache_.end()) {
      if (it->second < 0) return false;
      index = static_cast<uint32_t>(it->second);
      return true;
    }
    model_cache_[key] = -1;
    std::string e;
    const std::string path = prefix_ + "/Xs/" + id + ".x";  // GcArea::GcArea "%s/Xs/%s", LoadX appends ".x"
    if (!chain_.load(path, bytes_, nullptr, &e)) {
      warn("model " + id + ": " + e);
      return false;
    }
    sage::XFile xf;
    if (!sage::parse_x(bytes_.data(), bytes_.size(), xf, &e)) {
      warn("model " + id + ": " + e);
      return false;
    }
    Model m;
    if (!build_model(id, xf.mesh, m)) return false;
    model_cache_[key] = static_cast<int64_t>(out_.models.size());
    index = static_cast<uint32_t>(out_.models.size());
    out_.models.push_back(std::move(m));
    return true;
  }

  bool build_model(const std::string& id, const sage::XMesh& x, Model& m) {
    AreaStats& st = out_.stats;
    const uint64_t n = x.positions.size() / 3;
    if (n == 0 || x.indices.empty()) {
      warn("model " + id + ": empty mesh");
      return false;
    }
    // parse_x guarantees these; re-checked so a parser change can never turn into an out-of-range GPU upload.
    for (uint32_t v : x.indices)
      if (v >= n) {
        warn("model " + id + ": index out of range");
        return false;
      }
    const uint64_t bytes = n * kVertexFloats * 4 + uint64_t(x.indices.size()) * 4;
    if (bytes > max_geometry_bytes_ - std::min(max_geometry_bytes_, st.geometry_bytes)) return fatal("area: geometry over kMaxAreaGeometryBytes");
    st.geometry_bytes += bytes;
    m.id = id;
    m.vertex_count = static_cast<uint32_t>(n);
    m.has_colors = x.has_colors && x.colors.size() == n * 4;
    const bool has_uv = x.uvs.size() == n * 2;
    const bool has_normals = x.normals.size() == n * 3;
    m.vertices.resize(n * kVertexFloats);
    for (uint64_t v = 0; v < n; ++v) {
      float* o = &m.vertices[v * kVertexFloats];
      const float* p = &x.positions[v * 3];
      o[0] = p[0];
      o[1] = p[1];
      o[2] = p[2];
      for (int k = 0; k < 3; ++k) o[3 + k] = has_normals ? x.normals[v * 3 + k] : 0.0f;
      o[6] = has_uv ? x.uvs[v * 2] : 0.0f;
      o[7] = has_uv ? x.uvs[v * 2 + 1] : 0.0f;
      // MeshVertexColors reach the D3D vertex buffer as a D3DCOLOR (8 bits per channel): quantised like D3DXCOLOR -> DWORD.
      for (int c = 0; c < 4; ++c) o[8 + c] = m.has_colors ? d3d::quantize_unorm8(x.colors[v * 4 + c]) : 1.0f;
      if (v == 0 || p[1] >= m.greatest_y[1]) std::memcpy(m.greatest_y, p, sizeof m.greatest_y);  // GetGreatestZ: last max
      for (int k = 0; k < 3; ++k) {
        if (v == 0 || p[k] < m.bounds_min[k]) m.bounds_min[k] = p[k];
        if (v == 0 || p[k] > m.bounds_max[k]) m.bounds_max[k] = p[k];
      }
    }
    m.indices = x.indices;
    const uint64_t tris = x.indices.size() / 3;
    for (const sage::XSubset& s : x.subsets) {  // bounded by parse_x (one per used material)
      if (s.face_count == 0 || uint64_t(s.face_start) + s.face_count > tris) {
        warn("model " + id + ": subset outside the index list");
        return false;
      }
      SubsetDraw d;
      d.first_index = s.face_start * 3;
      d.index_count = s.face_count * 3;
      d.attrib = s.attrib_id;
      // XMesh invariant: materials may be empty (then attrib_id is 0 and there is no material): default material.
      const sage::XMaterial* mat = s.attrib_id < x.materials.size() ? &x.materials[s.attrib_id] : nullptr;
      if (mat) std::memcpy(d.diffuse, mat->diffuse, sizeof d.diffuse);
      d.has_material = mat != nullptr;
      // LoadX (0x00553800) creates an ScDrawableMaterial only for a material with a TextureFilename; DrawX would then
      // call IsAlphaOn on a NULL material (a fault in the original). Such subsets draw untextured here (warned).
      if (mat && !mat->has_texture) {
        warn("model " + id + ": a used material has no texture (the original would fault)");
        ++st.untextured_materials;
      }
      if (mat) build_material(*mat, d);
      if (mat && mat->has_texture) {
        uint32_t t = kNoTexture;
        if (!texture_for(mat->texture, t) && !fatal_.empty()) return false;
        d.texture = t;
        d.alpha = t != kNoTexture && out_.textures[t].alpha_on;
      }
      if (d.texture == kNoTexture) ++st.untextured_subsets;
      if (d.alpha) ++st.alpha_subsets;
      m.subsets.push_back(d);
    }
    st.subsets += static_cast<uint32_t>(m.subsets.size());
    st.vertices += m.vertex_count;
    st.triangles += static_cast<uint32_t>(tris);
    if (!m.has_colors) ++st.models_without_colors;
    return true;
  }

  // ScDrawableMaterial::LoadIndexed (0x00550060) on the path LoadX builds ("<lev>/textures/<TextureFilename>").
  void build_material(const sage::XMaterial& x, SubsetDraw& d) {
    d.textured = x.has_texture;
    d.texture_name = x.texture;
    const std::string path = "Root/Data/Levels/" + out_.level + "/textures/" + x.texture;
    auto has = [&](const char* sub) { return path.find(sub) != std::string::npos; };
    d3d::Material8& m = d.material;
    m.ambient = {0.7f, 0.7f, 0.7f, 1.0f};  // forced (@0x007365ec)
    if (has("_glass")) {
      m.diffuse = {0, 0, 0, 0};  // (@0x007365fc)
      d.trans = 0.0f;            // mTrans (@0x0073660c)
    } else {
      m.diffuse = {x.diffuse[0], x.diffuse[1], x.diffuse[2], x.diffuse[3]};
      d.trans = 1.0f;
    }
    // D3DX fills the .x specular/emissive RGB; their alpha is INFERRED 1 (it reaches no pixel: lighting is off).
    m.specular = {x.specular[0], x.specular[1], x.specular[2], 1.0f};
    m.power = x.power;
    m.emissive = {x.emissive[0], x.emissive[1], x.emissive[2], 1.0f};
    if (has("eye")) {  // eye textures glow by tribe prefix (constants @0x00736614..0x00736680), diffuse black
      if (has("on_")) m.emissive = {0, 1, 0, 1};
      else if (has("ga_")) m.emissive = {1, 0.86666667f, 0.0039215689f, 1};
      else if (has("po_")) m.emissive = {1, 0.50196081f, 0.0039215689f, 1};
      else if (has("ko_")) m.emissive = {0.28235295f, 0.65882355f, 1, 1};
      else if (has("le_")) m.emissive = {0.66274512f, 1, 0.0039215689f, 1};
      else if (has("ta_")) m.emissive = {1, 0.0039215689f, 0.14509805f, 1};
      else if (!has("wh_")) m.emissive = {0, 0, 1, 1};
      m.diffuse = {0, 0, 0, 1};
    }
  }

  // ScDrawableTexture::Load: resolve "<lev>/textures/<name>" (.dds then .tga), cached per area by resolved request.
  bool texture_for(const std::string& name, uint32_t& index) {
    AreaStats& st = out_.stats;
    std::string chosen;
    if (!sage::resolve_texture(chain_, out_.level, name, chosen)) {
      ++st.textures_missing;
      warn("texture " + name + ": not found");
      return false;
    }
    const std::string key = lower(chosen);
    auto it = texture_cache_.find(key);
    if (it != texture_cache_.end()) {
      if (it->second < 0) return false;
      index = static_cast<uint32_t>(it->second);
      return true;
    }
    texture_cache_[key] = -1;
    if (out_.textures.size() >= max_textures_) return fatal("area: more than kMaxAreaTextures textures");
    std::string e;
    std::vector<uint8_t> bytes;
    if (!chain_.load(chosen, bytes, nullptr, &e)) {
      ++st.textures_failed;
      warn("texture " + name + ": " + e);
      return false;
    }
    const auto t0 = Clock::now();
    sage::Image img;
    Texture tex;
    tex.request = chosen;
    bool ok = false;
    if (ends_with(key, ".tga")) {
      sage::TgaInfo ti;
      ok = sage::decode_tga(bytes.data(), bytes.size(), img, &ti, &e);
      if (ok) {
        // Spec R8: a 32-bpp TGA declaring 0 alpha bits is X8R8G8B8 (opaque); INFERRED from D3DX, confirm in A8.
        tex.alpha_on = ti.alpha_bits > 0;
        tex.format = ti.bits == 24 ? "TGA24" : tex.alpha_on ? "TGA32a" : "TGA32";
        if (!tex.alpha_on)
          for (size_t i = 3; i < img.rgba.size(); i += 4) img.rgba[i] = 255;
      }
    } else {
      sage::DdsInfo di;
      ok = sage::decode_dds(bytes.data(), bytes.size(), img, &di, &e);
      if (ok) {
        tex.alpha_on = di.alpha_on;  // ScDrawableTexture mAlphaOn: DXT3 and A8R8G8B8 only
        tex.format = sage::dds_format_name(di.format);
      }
    }
    if (!ok) {
      st.decode_ms += ms_since(t0);
      ++st.textures_failed;
      warn("texture " + name + ": " + e);
      return false;
    }
    const uint64_t chain_bytes = mip_chain_bytes(img.width, img.height);
    if (chain_bytes > max_texture_bytes_ - std::min(max_texture_bytes_, st.texture_bytes)) {
      st.decode_ms += ms_since(t0);
      return fatal("area: textures over kMaxAreaTextureBytes");
    }
    if (!build_mip_chain(std::move(img.rgba), img.width, img.height, tex.mips)) {
      st.decode_ms += ms_since(t0);
      ++st.textures_failed;
      warn("texture " + name + ": mip chain failed");
      return false;
    }
    st.decode_ms += ms_since(t0);
    st.texture_bytes += chain_bytes;
    if (tex.alpha_on) ++st.textures_alpha;
    texture_cache_[key] = static_cast<int64_t>(out_.textures.size());
    index = static_cast<uint32_t>(out_.textures.size());
    out_.textures.push_back(std::move(tex));
    return true;
  }

  // enter_area opens <lev>.blk then <area>.blk and fails on the first that cannot be opened (fs size() fails:
  // Environment) or parsed (Data). Replays that order on the failure path only.
  LoadError enter_failure_kind() const {
    const std::string lp = "Root/Data/Levels/" + out_.level + "/" + out_.level + ".blk";
    const std::string ap = "Root/Data/Levels/" + out_.level + "/" + out_.area + ".blk";
    uint64_t size = 0;
    if (fs_.size(lp, size) != fs::Error::None) return LoadError::Environment;
    sage::BlockChain probe(fs_);
    if (!probe.open_second(lp, nullptr)) return LoadError::Data;
    if (fs_.size(ap, size) != fs::Error::None) return LoadError::Environment;
    return LoadError::Data;
  }

  void grow_bounds(const Model& m, const Mat4& world) {
    for (int c = 0; c < 8; ++c) {
      const float p[3] = {(c & 1) ? m.bounds_max[0] : m.bounds_min[0], (c & 2) ? m.bounds_max[1] : m.bounds_min[1],
                          (c & 4) ? m.bounds_max[2] : m.bounds_min[2]};
      float w[4];
      transform_point(world, p, w);
      for (int k = 0; k < 3; ++k) {
        if (!out_.has_bounds || w[k] < out_.bounds_min[k]) out_.bounds_min[k] = w[k];
        if (!out_.has_bounds || w[k] > out_.bounds_max[k]) out_.bounds_max[k] = w[k];
      }
      out_.has_bounds = true;
    }
  }

  const fs::FileSystem& fs_;
  sage::BlockChain chain_;
  AreaData& out_;
  LoadError kind_ = LoadError::Data;
  std::string prefix_;
  std::vector<uint8_t> bytes_;
  std::map<std::string, int64_t> model_cache_, texture_cache_;  // ordered maps: deterministic, no pointer keys
  std::set<std::string> warnings_;
  uint32_t warnings_dropped_ = 0;
  std::string fatal_;  // set when a cap is exceeded: the whole load fails
  std::vector<sage::SpecTextureRecord> spec_;  // the level's textures.slb
  uint32_t max_placements_ = kMaxPlacements, max_draws_ = kMaxDraws, max_textures_ = kMaxAreaTextures;
  uint64_t max_texture_bytes_ = kMaxAreaTextureBytes, max_geometry_bytes_ = kMaxAreaGeometryBytes;
  uint32_t max_lights_ = kMaxAreaLights, lights_dropped_ = 0;
};

}  // namespace

const char* area_wgsl() { return d3d::ffp_wgsl(); }

bool load_area(const fs::FileSystem& fs, std::string_view level, std::string_view area, const AreaOptions& opt, AreaData& out,
               std::string* err, LoadError* kind) {
  out = AreaData();
  AreaData tmp;
  Loader l(fs, tmp);
  if (err) err->clear();
  if (kind) *kind = LoadError::None;
  if (!l.run(level, area, opt, err)) {  // no partial state on failure
    if (kind) *kind = l.kind();
    return false;
  }
  out = std::move(tmp);
  return true;
}

Camera make_camera(const AreaData& a, const CameraSettings& s, float aspect) {
  Camera c;
  c.fov_deg = s.fov_deg;
  c.near_z = s.near_z;
  c.far_z = s.far_z;
  c.aspect = aspect;
  float centre[3] = {0, 0, 0};
  if (a.has_bounds)
    for (int k = 0; k < 3; ++k) centre[k] = (a.bounds_min[k] + a.bounds_max[k]) * 0.5f;
  if (s.eye_set) {
    std::memcpy(c.eye, s.eye, sizeof c.eye);
    c.eye_source = "cli";
  } else if (a.has_start) {
    c.eye[0] = a.start[0];
    c.eye[1] = a.start[1] + s.eye_height;
    c.eye[2] = a.start[2];
    c.eye_source = "pos_start";
  } else if (a.has_bounds) {
    std::memcpy(c.eye, centre, sizeof c.eye);
    c.eye[1] += s.eye_height;
    c.eye_source = "bounds_centre";
  } else {
    c.eye_source = "origin";
  }
  constexpr double kDeg = 3.14159265358979323846 / 180.0;
  float yaw = s.yaw_deg;
  if (s.yaw_set) {
    c.yaw_source = "cli";
  } else {
    const float dx = centre[0] - c.eye[0], dz = centre[2] - c.eye[2];
    if (a.has_bounds && dx * dx + dz * dz > 1e-6f) {
      yaw = static_cast<float>(std::atan2(static_cast<double>(dx), static_cast<double>(-dz)) / kDeg);
      c.yaw_source = "towards_bounds_centre";
    } else {
      yaw = 0.0f;
      c.yaw_source = "default";
    }
  }
  c.yaw_deg = yaw;
  // Clamped to +-89.9 degrees: at +-90 the view direction is parallel to the up vector and LookAtRH's x axis is zero.
  c.pitch_deg = std::max(-kMaxPitchDeg, std::min(kMaxPitchDeg, s.pitch_deg));
  const double y = yaw * kDeg, p = c.pitch_deg * kDeg;
  const float fwd[3] = {static_cast<float>(std::sin(y) * std::cos(p)), static_cast<float>(std::sin(p)),
                        static_cast<float>(-std::cos(y) * std::cos(p))};
  for (int k = 0; k < 3; ++k) c.at[k] = c.eye[k] + fwd[k];
  const float up[3] = {0, 1, 0};
  c.view = look_at_rh(c.eye, c.at, up);
  c.proj = perspective_fov_rh(static_cast<float>(s.fov_deg * kDeg), aspect, s.near_z, s.far_z);
  return c;
}

uint64_t release_cpu_copies(AreaData& a) {
  uint64_t freed = 0;
  for (Model& m : a.models) {
    freed += m.vertices.capacity() * sizeof(float) + m.indices.capacity() * sizeof(uint32_t);
    std::vector<float>().swap(m.vertices);
    std::vector<uint32_t>().swap(m.indices);
  }
  for (Texture& t : a.textures) {
    for (const MipLevel& l : t.mips) freed += l.rgba.capacity();
    std::vector<MipLevel>().swap(t.mips);
  }
  a.cpu_released = true;
  return freed;
}

namespace {
constexpr uint32_t kSlot = static_cast<uint32_t>(rhi::uniform_stride(d3d::kFfpUniformBytes));  // one slot per draw
}  // namespace

// Everything that lives across frames: the D3D8 device (its state is history-dependent: the HUD view's mode carries
// ALPHAREF 1 into the next frame), the game-side replay and the rhi object caches.
// Task 8B E3: the texture names (device texture handle k -> names[k-1], printed by the state log) live HERE, declared
// before dev8, which keeps a pointer to them: the Impl is heap-allocated and owned by a unique_ptr, so moving the
// AreaRenderer moves the pointer and the names stay where dev8 looks (they used to be an AreaRenderer member).
struct AreaRenderer::Impl {
  std::vector<std::string> names;
  d3d::Device8 dev8;
  SageFrame sage;
  std::map<d3d::PipelineKey, rhi::PipelineId> pipelines;
  std::map<std::tuple<int, int, int, int>, rhi::SamplerId> samplers;
  std::map<std::pair<rhi::TextureId, rhi::SamplerId>, rhi::BindGroupId> groups;
  bool initialised = false;
  Impl(std::vector<std::string>* log, std::vector<std::string> texture_names, const SageFrameOptions& so)
      : names(std::move(texture_names)), dev8(log, &names), sage(dev8, SageCaps(), so) {}
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
};

// DrawSink: one DrawSubset with faces -> one queued rhi draw (pipeline + bind group created on first use, before the
// frame begins) with its own uniform slot.
class AreaRenderer::Sink final : public d3d::DrawSink {
 public:
  struct Draw {
    rhi::PipelineId pipeline;
    rhi::BindGroupId group;
    uint32_t model, first, count;
  };
  Sink(AreaRenderer& r, rhi::Device& dev, const AreaData& a) : r_(r), dev_(dev), a_(a) {}
  std::vector<Draw> draws;
  bool failed = false;
  void draw(const d3d::State& s, const d3d::DrawRef& ref) override {
    ++r_.stats_.draw_subsets;
    if (ref.subset == UINT32_MAX || failed) return;  // an attribute id without faces
    const Placement& p = a_.placements[ref.placement];
    const Model& m = a_.models[p.model];
    const SubsetDraw& sd = m.subsets[ref.subset];
    if (draws.size() >= r_.uniform_slots_) {  // cannot happen: slots = placements x subsets
      failed = true;
      return;
    }
    uint32_t unsup = 0;
    const d3d::PipelineKey key = d3d::pipeline_key(s, &unsup);
    const rhi::SamplerDesc sdesc = d3d::sampler_desc(s, &unsup);
    r_.stats_.unsupported |= unsup;
    Impl& im = *r_.impl_;
    rhi::PipelineId pipe = 0;
    auto pit = im.pipelines.find(key);
    if (pit != im.pipelines.end()) {
      pipe = pit->second;
    } else {
      pipe = dev_.create_mesh_pipeline(d3d::pipeline_desc(key));
      if (!pipe) {
        failed = true;
        return;
      }
      im.pipelines[key] = pipe;
      ++r_.pipelines_created_;
    }
    const auto skey = std::make_tuple(int(sdesc.mag), int(sdesc.min), int(sdesc.mip), int(sdesc.max_anisotropy));
    rhi::SamplerId samp = 0;
    auto sit = im.samplers.find(skey);
    if (sit != im.samplers.end()) {
      samp = sit->second;
    } else {
      samp = dev_.create_sampler(sdesc);
      if (!samp) {
        failed = true;
        return;
      }
      im.samplers[skey] = samp;
    }
    const uint32_t handle = s.texture[0];
    const rhi::TextureId tex = handle && handle <= r_.gpu_textures_.size() ? r_.gpu_textures_[handle - 1] : r_.white_texture_;
    rhi::BindGroupId bg = 0;
    auto git = im.groups.find({tex, samp});
    if (git != im.groups.end()) {
      bg = git->second;
    } else {
      bg = dev_.create_bind_group({r_.uniforms_, d3d::kFfpUniformBytes, tex, samp});
      if (!bg) {
        failed = true;
        return;
      }
      im.groups[{tex, samp}] = bg;
    }
    d3d::DrawInputs in;
    in.vertex_has_diffuse = m.has_colors && r_.opt_.vertex_colors;
    in.texture_bound = handle != 0;
    float* u = reinterpret_cast<float*>(&r_.uniform_data_[draws.size() * size_t(kSlot)]);
    d3d::pack_uniforms(s, in, u);
    if (!r_.opt_.fog) u[d3d::kFfpFogEnableFloat] = 0.0f;  // debug: the fog-enable flag off
    RenderStats& st = r_.stats_;
    if (s.rs[d3d::rs::LIGHTING]) ++st.lit_draws;
    if (s.rs[d3d::rs::FOGENABLE] && r_.opt_.fog) ++st.fogged_draws;
    if (s.rs[d3d::rs::ALPHABLENDENABLE]) ++st.blended_draws;
    if (s.rs[d3d::rs::ALPHATESTENABLE]) ++st.alpha_tested_draws;
    if (!handle) ++st.null_texture_draws;
    draws.push_back({pipe, bg, p.model, sd.first_index, sd.index_count});
    // The shader-relevant state of this draw (lighting, colour source, stage-0 ops/args, alpha test, fog, texture
    // transform, NULL texture) on top of the pipeline key.
    char sig[160];
    const uint32_t* t = s.tss[0];
    std::snprintf(sig, sizeof sig, "%s|%u%u%u|%u%u%u%u%u%u|%u%u|%u%u|%u%u", key.str().c_str(), s.rs[d3d::rs::LIGHTING], in.vertex_has_diffuse ? 1u : 0u,
                  in.texture_bound ? 1u : 0u, t[d3d::tss::COLOROP], t[d3d::tss::COLORARG1], t[d3d::tss::COLORARG2], t[d3d::tss::ALPHAOP],
                  t[d3d::tss::ALPHAARG1], t[d3d::tss::ALPHAARG2], s.rs[d3d::rs::ALPHATESTENABLE], s.rs[d3d::rs::ALPHAFUNC],
                  s.rs[d3d::rs::FOGENABLE], s.rs[d3d::rs::FOGTABLEMODE], t[d3d::tss::TEXTURETRANSFORMFLAGS], s.rs[d3d::rs::ALPHABLENDENABLE]);
    if (variants_.insert(sig).second) ++st.shader_variants;
  }
  std::set<std::string> variants_;

 private:
  AreaRenderer& r_;
  rhi::Device& dev_;
  const AreaData& a_;
};

AreaRenderer::AreaRenderer() = default;
AreaRenderer::AreaRenderer(AreaRenderer&&) noexcept = default;
AreaRenderer& AreaRenderer::operator=(AreaRenderer&&) noexcept = default;
AreaRenderer::~AreaRenderer() = default;

bool AreaRenderer::upload(rhi::Device& dev, const AreaData& a, const RenderOptions& opt, std::string* err) {
  if (a.cpu_released) return fail(err, "render: area CPU copies were already released");
  // A second upload would leak the first one's GPU objects, so it is rejected until release() (M1b) has run.
  if (upload_attempted_) return fail(err, "render: renderer already uploaded (call release() first)");
  upload_attempted_ = true;
  opt_ = opt;
  uniform_slots_ = a.stats.draws ? a.stats.draws : 1;
  uniforms_ = dev.create_uniform_buffer(uniform_slots_ * kSlot);
  if (!uniforms_) return fail(err, "render: uniform buffer rejected");
  // Stands in for a NULL stage-0 texture (D3D8 leaves the texture argument undefined there; white keeps the colour).
  const uint8_t white[4] = {255, 255, 255, 255};
  white_texture_ = dev.create_texture(1, 1, 1);
  if (!white_texture_ || !dev.write_texture(white_texture_, 0, white, 4)) return fail(err, "render: white texture rejected");
  textures_created_ = 1;
  uploaded_texture_bytes_ = 4;
  gpu_textures_.clear();
  std::vector<std::string> texture_names;
  for (const Texture& t : a.textures) {
    const MipLevel& top = t.mips.front();
    rhi::TextureId id = dev.create_texture(top.width, top.height, static_cast<uint32_t>(t.mips.size()));
    if (!id) return fail(err, "render: texture rejected: " + t.request);
    created_textures_.push_back(id);  // M1b: every created object is owned (release() frees it) even if upload fails later
    for (uint32_t m = 0; m < t.mips.size(); ++m) {
      if (!dev.write_texture(id, m, t.mips[m].rgba.data(), t.mips[m].rgba.size())) return fail(err, "render: mip upload rejected: " + t.request);
      uploaded_texture_bytes_ += t.mips[m].rgba.size();
    }
    ++textures_created_;
    gpu_textures_.push_back(id);
    const size_t slash = t.request.find_last_of('/');
    texture_names.push_back(slash == std::string::npos ? t.request : t.request.substr(slash + 1));
  }
  models_.clear();
  for (const Model& m : a.models) {
    GpuModel g;
    const uint32_t vbytes = static_cast<uint32_t>(m.vertices.size() * 4), ibytes = static_cast<uint32_t>(m.indices.size() * 4);
    g.vb = dev.create_vertex_buffer(vbytes);
    g.ib = dev.create_index_buffer(ibytes);
    if (g.vb) created_buffers_.push_back(g.vb);  // M1b (see created_textures_)
    if (g.ib) created_buffers_.push_back(g.ib);
    if (!g.vb || !g.ib) return fail(err, "render: buffer rejected for model " + m.id);
    dev.write_buffer(g.vb, 0, m.vertices.data(), vbytes);
    dev.write_buffer(g.ib, 0, m.indices.data(), ibytes);
    models_.push_back(g);
  }
  uniform_data_.assign(size_t(uniform_slots_) * kSlot, 0);
  SageFrameOptions so;
  so.force_area_lighting = opt.force_area_lighting;
  impl_ = std::make_unique<Impl>(opt.state_log, std::move(texture_names), so);
  uploaded_ = &a;
  return true;
}

bool AreaRenderer::render(rhi::Device& dev, rhi::TextureId target, const AreaData& a, const Camera& cam) {
  if (uploaded_ != &a || !impl_) {  // never uploaded, upload failed, or a different AreaData: no rhi call at all
    if (!render_refused_logged_) {
      render_refused_logged_ = true;
      std::fprintf(stderr, "[area] render skipped: %s\n", uploaded_ ? "not the AreaData that was uploaded" : "no successful upload");
    }
    return false;
  }
  Impl& im = *impl_;
  stats_ = RenderStats();
  if (!im.initialised) {
    im.initialised = true;
    im.sage.init_device();
    // The area script's GcAreaDirector::SetFog only stores the statics (SetFogValue); SetFog(true) applies them.
    im.sage.set_fog_value(a.fog.color, a.fog.start, a.fog.end, a.fog.density);
  }
  const auto t_replay = Clock::now();  // M1b: frame-time breakdown (replay = SageFrame + draw compilation, submit = rhi)
  Sink sink(*this, dev, a);
  im.dev8.set_sink(&sink);
  const uint32_t calls0 = im.dev8.state_calls();
  const uint64_t empty0 = im.sage.empty_subsets();
  im.sage.frame(a, cam, frames_++);
  im.dev8.set_sink(nullptr);
  stats_.state_calls = im.dev8.state_calls() - calls0;
  stats_.empty_subsets = static_cast<uint32_t>(im.sage.empty_subsets() - empty0);
  if (sink.failed) {
    std::fprintf(stderr, "[area] render failed: an rhi object was rejected\n");
    return false;
  }
  const uint32_t n = static_cast<uint32_t>(sink.draws.size());
  stats_.rhi_draws = n;
  const auto t_submit = Clock::now();
  stats_.replay_ms = std::chrono::duration<double, std::milli>(t_submit - t_replay).count();
  if (n) dev.write_buffer(uniforms_, 0, uniform_data_.data(), n * kSlot);
  // The frame starts from the last whole-viewport clear (ScRenderer::ClearRect: 0x00000000, z 1.0). Its alpha is not
  // kept: the D3D8 back buffer is X8R8G8B8, so the PNG gets alpha 1 like every opaque draw.
  const d3d::Color4 cc = d3d::argb_to_color(im.dev8.last_clear_argb());
  const float clear[4] = {cc.r, cc.g, cc.b, 1.0f};
  dev.begin_frame(target, clear);
  for (uint32_t i = 0; i < n; ++i) {
    const Sink::Draw& s = sink.draws[i];
    rhi::DrawIndexedDesc d;
    d.pipeline = s.pipeline;
    d.vertices = models_[s.model].vb;
    d.indices = models_[s.model].ib;
    d.first_index = s.first;
    d.index_count = s.count;
    d.bind_group = s.group;
    d.uniform_offset = i * kSlot;
    dev.draw_indexed(d);
  }
  dev.end_frame();
  stats_.submit_ms = ms_since(t_submit);
  return true;
}

// ---- M1b: area switching ----
// Destroys every rhi object this renderer created, in dependency order: bind groups first (they reference the uniform
// buffer, textures and samplers; rhi rejects destroying a referenced object), then pipelines, samplers, textures and
// buffers. Clears the pipeline / sampler / bind-group caches and the D3D8 device (a new upload starts from init_device,
// as GcAreaLoader::LoadArea runs with a fresh frame state). Works after a failed upload too (objects created before
// the failure are owned). Returns the number of objects destroyed; *failures counts rejected destroys (0 when the
// device and this renderer agree).
uint32_t AreaRenderer::release(rhi::Device& dev, uint32_t* failures) {
  uint32_t destroyed = 0, failed = 0;
  auto count = [&](bool ok) { ok ? ++destroyed : ++failed; };
  if (impl_) {
    for (const auto& kv : impl_->groups) count(dev.destroy_bind_group(kv.second));
    for (const auto& kv : impl_->pipelines) count(dev.destroy_pipeline(kv.second));
    for (const auto& kv : impl_->samplers) count(dev.destroy_sampler(kv.second));
  }
  for (rhi::TextureId t : created_textures_) count(dev.destroy_texture(t));
  if (white_texture_) count(dev.destroy_texture(white_texture_));
  for (rhi::BufferId b : created_buffers_) count(dev.destroy_buffer(b));
  if (uniforms_) count(dev.destroy_buffer(uniforms_));
  impl_.reset();
  created_textures_.clear();
  created_buffers_.clear();
  gpu_textures_.clear();
  models_.clear();
  uniform_data_.clear();
  uniforms_ = 0;
  white_texture_ = 0;
  uniform_slots_ = 0;
  pipelines_created_ = textures_created_ = frames_ = 0;
  uploaded_texture_bytes_ = 0;
  uploaded_ = nullptr;
  upload_attempted_ = false;
  render_refused_logged_ = false;
  stats_ = RenderStats();
  if (failures) *failures = failed;
  return destroyed;
}

}  // namespace lomn::view
