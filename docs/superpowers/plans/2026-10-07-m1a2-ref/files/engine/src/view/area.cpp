#include "lomn/view/area.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <set>

#include "lomn/sage/blockchain.h"
#include "lomn/sage/image.h"
#include "lomn/sage/xfile.h"

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
bool fail(std::string* err, const std::string& msg) {
  if (err) *err = msg;
  return false;
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
  Loader(const fs::FileSystem& fs, AreaData& out) : chain_(fs), out_(out) {}

  bool run(std::string_view level, std::string_view area, const AreaOptions& opt, std::string* err) {
    const auto t0 = Clock::now();
    out_.level = std::string(level.substr(0, 4));
    out_.area = std::string(area.substr(0, 4));
    std::string e;
    // GcGame::LoadArtBlockHeader: Art.blk is the global fallback of both lookup orders. Optional here.
    out_.art_blk = chain_.open_art("Root/Data/Art/Art.blk", &e);
    if (!out_.art_blk) warn("art.blk not opened: " + e);
    if (!chain_.enter_area(out_.level, out_.area, &e)) return fail(err, "area: " + e);
    prefix_ = "Root/Data/Levels/" + out_.level + "/" + out_.area;  // "%.4s/%.4s" area prefix (GcAreaLoader::LoadArea)

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
    for (uint32_t i = 0; i < recs.size(); ++i) {  // bounded by parse_obj (n * 60 <= table bytes) and kMaxPlacements
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
      if (out_.placements.size() >= kMaxPlacements) return fail(err, "area: more than kMaxPlacements drawn records");
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
      if (draws > kMaxDraws) return fail(err, "area: more than kMaxDraws draws");
      st.draws = static_cast<uint32_t>(draws);
      if (rt != sage::Route::Back) grow_bounds(out_.models[model], p.world);
      out_.placements.push_back(p);
    }
    st.placements = static_cast<uint32_t>(out_.placements.size());
    st.models = static_cast<uint32_t>(out_.models.size());
    st.textures = static_cast<uint32_t>(out_.textures.size());
    std::sort(warnings_.begin(), warnings_.end());
    warnings_.erase(std::unique(warnings_.begin(), warnings_.end()), warnings_.end());
    out_.warnings = warnings_;
    st.load_ms = ms_since(t0) - st.decode_ms;
    return true;
  }

 private:
  void warn(std::string w) { warnings_.push_back(std::move(w)); }
  bool fatal(const std::string& why) {
    fatal_ = why;
    return false;
  }

  bool load_table(const char* suffix, std::string& e) {
    // "Root/Data/Levels/%.4s/%.4s/%.4s_obj.slb" through SrBlockFileLoad (current -> second -> Art).
    return chain_.load(prefix_ + "/" + out_.area + suffix, bytes_, nullptr, &e);
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
    if (bytes > kMaxAreaGeometryBytes - std::min(kMaxAreaGeometryBytes, st.geometry_bytes)) return fatal("area: geometry over kMaxAreaGeometryBytes");
    st.geometry_bytes += bytes;
    m.id = id;
    m.vertex_count = static_cast<uint32_t>(n);
    m.has_colors = x.has_colors && x.colors.size() == n * 4;
    const bool has_uv = x.uvs.size() == n * 2;
    m.vertices.resize(n * kVertexFloats);
    for (uint64_t v = 0; v < n; ++v) {
      float* o = &m.vertices[v * kVertexFloats];
      const float* p = &x.positions[v * 3];
      o[0] = p[0];
      o[1] = p[1];
      o[2] = p[2];
      o[3] = has_uv ? x.uvs[v * 2] : 0.0f;
      o[4] = has_uv ? x.uvs[v * 2 + 1] : 0.0f;
      for (int c = 0; c < 4; ++c) o[5 + c] = m.has_colors ? x.colors[v * 4 + c] : 1.0f;
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
      // XMesh invariant: materials may be empty (then attrib_id is 0 and there is no material): default material.
      const sage::XMaterial* mat = s.attrib_id < x.materials.size() ? &x.materials[s.attrib_id] : nullptr;
      if (mat) std::memcpy(d.diffuse, mat->diffuse, sizeof d.diffuse);
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
    if (out_.textures.size() >= kMaxAreaTextures) return fatal("area: more than kMaxAreaTextures textures");
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
    if (chain_bytes > kMaxAreaTextureBytes - std::min(kMaxAreaTextureBytes, st.texture_bytes)) {
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

  sage::BlockChain chain_;
  AreaData& out_;
  std::string prefix_;
  std::vector<uint8_t> bytes_;
  std::map<std::string, int64_t> model_cache_, texture_cache_;  // ordered maps: deterministic, no pointer keys
  std::vector<std::string> warnings_;
  std::string fatal_;  // set when a cap is exceeded: the whole load fails
};

const char* kWgsl = R"(// lomn M1a-2 unlit area shader. Uniform block per draw (dynamic offset).
struct U {
  wvp: mat4x4f,     // world * view * proj, D3D row-vector matrix uploaded as-is (column-major read == transpose)
  diffuse: vec4f,   // material diffuse, used when the vertex has no colour
  params: vec4f,    // x: 1 = use vertex colour, y: alpha reference (0..1), alpha pass discards a <= y
}
@group(0) @binding(0) var<uniform> u: U;
@group(0) @binding(1) var tex: texture_2d<f32>;
@group(0) @binding(2) var samp: sampler;
struct VOut {
  @builtin(position) pos: vec4f,
  @location(0) uv: vec2f,
  @location(1) color: vec4f,
}
@vertex fn vs(@location(0) p: vec3f, @location(1) uv: vec2f, @location(2) c: vec4f) -> VOut {
  var o: VOut;
  o.pos = u.wvp * vec4f(p, 1.0);
  o.uv = uv;
  o.color = select(u.diffuse, c, u.params.x > 0.5);
  return o;
}
// D3D stage 0 defaults: COLOROP MODULATE(TEXTURE, DIFFUSE), ALPHAOP SELECTARG1(TEXTURE).
fn shade(i: VOut) -> vec4f {
  let t = textureSample(tex, samp, i.uv);
  return vec4f(t.rgb * i.color.rgb, t.a);
}
@fragment fn fs_opaque(i: VOut) -> @location(0) vec4f {
  return vec4f(shade(i).rgb, 1.0);
}
@fragment fn fs_alpha(i: VOut) -> @location(0) vec4f {
  let c = shade(i);
  if (c.a <= u.params.y) {
    discard;
  }
  return c;
}
)";

}  // namespace

const char* area_wgsl() { return kWgsl; }

bool load_area(const fs::FileSystem& fs, std::string_view level, std::string_view area, const AreaOptions& opt, AreaData& out,
               std::string* err) {
  out = AreaData();
  AreaData tmp;
  Loader l(fs, tmp);
  if (err) err->clear();
  if (!l.run(level, area, opt, err)) return false;  // no partial state on failure
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
  c.pitch_deg = s.pitch_deg;
  const double y = yaw * kDeg, p = s.pitch_deg * kDeg;
  const float fwd[3] = {static_cast<float>(std::sin(y) * std::cos(p)), static_cast<float>(std::sin(p)),
                        static_cast<float>(-std::cos(y) * std::cos(p))};
  for (int k = 0; k < 3; ++k) c.at[k] = c.eye[k] + fwd[k];
  const float up[3] = {0, 1, 0};
  c.view = look_at_rh(c.eye, c.at, up);
  c.proj = perspective_fov_rh(static_cast<float>(s.fov_deg * kDeg), aspect, s.near_z, s.far_z);
  return c;
}

bool AreaRenderer::upload(rhi::Device& dev, const AreaData& a, const RenderOptions& opt, std::string* err) {
  opt_ = opt;
  rhi::MeshPipelineDesc pd;
  pd.wgsl = kWgsl;
  pd.uniform_bytes = kAreaUniformBytes;
  pd.vertex_stride = kVertexFloats * 4;
  pd.attributes = {{rhi::VertexFormat::Float32x3, 0, 0}, {rhi::VertexFormat::Float32x2, 12, 1}, {rhi::VertexFormat::Float32x4, 20, 2}};
  pd.cull = rhi::CullMode::None;               // render mode 0x11: D3DCULL_NONE
  pd.depth_compare = rhi::CompareFunc::LessEqual;  // D3DRS_ZFUNC LESSEQUAL
  pd.depth_write = true;                       // ZWRITE on in both DrawX passes
  pd.fs_entry = "fs_opaque";
  pd.blend = rhi::BlendMode::Opaque;
  opaque_ = dev.create_mesh_pipeline(pd);
  pd.fs_entry = "fs_alpha";
  pd.blend = rhi::BlendMode::AlphaBlend;  // DrawX pass 2: ALPHABLENDENABLE 1, SRCALPHA/INVSRCALPHA, ALPHATEST GREATER
  alpha_ = dev.create_mesh_pipeline(pd);
  if (!opaque_ || !alpha_) return fail(err, "render: mesh pipeline rejected");
  pipelines_created_ = 2;
  // MAG/MIN ANISOTROPIC (max 3) and MIP LINEAR set globally; GcArea::Render forces MAGFILTER LINEAR; ADDRESS never set.
  rhi::SamplerDesc sd;
  sd.max_anisotropy = 3;
  sampler_ = dev.create_sampler(sd);
  if (!sampler_) return fail(err, "render: sampler rejected");
  uniform_slots_ = a.stats.draws ? a.stats.draws : 1;
  uniforms_ = dev.create_uniform_buffer(uniform_slots_ * rhi::kUniformAlign);
  if (!uniforms_) return fail(err, "render: uniform buffer rejected");
  const uint8_t white[4] = {255, 255, 255, 255};
  rhi::TextureId wt = dev.create_texture(1, 1, 1);
  if (!wt || !dev.write_texture(wt, 0, white, 4)) return fail(err, "render: white texture rejected");
  white_group_ = dev.create_bind_group(uniforms_, kAreaUniformBytes, wt, sampler_);
  if (!white_group_) return fail(err, "render: bind group rejected");
  textures_created_ = 1;
  uploaded_texture_bytes_ = 4;
  texture_groups_.clear();
  for (const Texture& t : a.textures) {
    const MipLevel& top = t.mips.front();
    rhi::TextureId id = dev.create_texture(top.width, top.height, static_cast<uint32_t>(t.mips.size()));
    if (!id) return fail(err, "render: texture rejected: " + t.request);
    for (uint32_t m = 0; m < t.mips.size(); ++m) {
      if (!dev.write_texture(id, m, t.mips[m].rgba.data(), t.mips[m].rgba.size())) return fail(err, "render: mip upload rejected: " + t.request);
      uploaded_texture_bytes_ += t.mips[m].rgba.size();
    }
    ++textures_created_;
    const rhi::BindGroupId bg = dev.create_bind_group(uniforms_, kAreaUniformBytes, id, sampler_);
    if (!bg) return fail(err, "render: bind group rejected: " + t.request);
    texture_groups_.push_back(bg);
  }
  models_.clear();
  for (const Model& m : a.models) {
    GpuModel g;
    const uint32_t vbytes = static_cast<uint32_t>(m.vertices.size() * 4), ibytes = static_cast<uint32_t>(m.indices.size() * 4);
    g.vb = dev.create_vertex_buffer(vbytes);
    g.ib = dev.create_index_buffer(ibytes);
    if (!g.vb || !g.ib) return fail(err, "render: buffer rejected for model " + m.id);
    dev.write_buffer(g.vb, 0, m.vertices.data(), vbytes);
    dev.write_buffer(g.ib, 0, m.indices.data(), ibytes);
    models_.push_back(g);
  }
  uniform_data_.assign(size_t(uniform_slots_) * rhi::kUniformAlign, 0);
  return true;
}

void AreaRenderer::render(rhi::Device& dev, rhi::TextureId target, const AreaData& a, const Camera& cam) {
  // Object order: route groups, record order inside a group; trans sorted far-to-near (GcTransDirector::SortIt; the
  // direction is INFERRED). Distances in double so the order is identical on every platform for equal floats.
  static const sage::Route kOrder[] = {sage::Route::Main, sage::Route::Moveable, sage::Route::NoLightMoveable, sage::Route::Back};
  std::vector<uint32_t> objects;
  for (sage::Route r : kOrder)
    for (uint32_t i = 0; i < a.placements.size(); ++i)
      if (a.placements[i].route == r) objects.push_back(i);
  std::vector<std::pair<double, uint32_t>> trans;
  for (uint32_t i = 0; i < a.placements.size(); ++i)
    if (a.placements[i].route == sage::Route::Trans) {
      double d = 0;
      for (int k = 0; k < 3; ++k) {
        const double v = double(a.placements[i].pos[k]) - cam.eye[k];
        d += v * v;
      }
      trans.push_back({-d, i});
    }
  std::stable_sort(trans.begin(), trans.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
  for (const auto& t : trans) objects.push_back(t.second);

  struct Item {
    uint32_t placement, subset;
  };
  std::vector<Item> items;
  for (uint32_t o : objects) {
    const Model& m = a.models[a.placements[o].model];
    for (int pass = 0; pass < 2; ++pass)
      for (uint32_t s = 0; s < m.subsets.size(); ++s)
        if (m.subsets[s].alpha == (pass == 1)) items.push_back({o, s});
  }
  const Mat4 vp = mat_mul(cam.view, cam.proj);
  const uint32_t n = static_cast<uint32_t>(std::min<size_t>(items.size(), uniform_slots_));
  for (uint32_t i = 0; i < n; ++i) {
    const Placement& p = a.placements[items[i].placement];
    const SubsetDraw& sd = a.models[p.model].subsets[items[i].subset];
    float u[24];
    const Mat4 wvp = mat_mul(p.world, vp);
    std::memcpy(u, wvp.m, 64);
    std::memcpy(u + 16, sd.diffuse, 16);
    u[20] = opt_.vertex_colors && a.models[p.model].has_colors ? 1.0f : 0.0f;
    u[21] = opt_.alpha_ref / 255.0f;
    u[22] = 0.0f;
    u[23] = 0.0f;
    std::memcpy(&uniform_data_[size_t(i) * rhi::kUniformAlign], u, sizeof u);
  }
  if (n) dev.write_buffer(uniforms_, 0, uniform_data_.data(), n * rhi::kUniformAlign);
  dev.begin_frame(target, opt_.clear);
  for (uint32_t i = 0; i < n; ++i) {
    const Placement& p = a.placements[items[i].placement];
    const SubsetDraw& sd = a.models[p.model].subsets[items[i].subset];
    rhi::DrawIndexedDesc d;
    d.pipeline = sd.alpha ? alpha_ : opaque_;
    d.vertices = models_[p.model].vb;
    d.indices = models_[p.model].ib;
    d.first_index = sd.first_index;
    d.index_count = sd.index_count;
    d.bind_group = sd.texture == kNoTexture ? white_group_ : texture_groups_[sd.texture];
    d.uniform_offset = i * rhi::kUniformAlign;
    dev.draw_indexed(d);
  }
  dev.end_frame();
}

}  // namespace lomn::view
