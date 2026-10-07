// M1a-2 "first pixels": an area's static geometry, loaded the way GcAreaLoader::LoadArea does and drawn UNLIT and
// textured straight on rhi (no D3D8 shim yet; that is M1a-3).
//
// Load (CPU only, no device): Art.blk header (optional), enter_area (second = <lev>.blk, current = <area>.blk),
// <area>_OBJ.slb -> route() per record -> for every record routed main / trans / back / moveable / no-light moveable:
// model "<area prefix>/Xs/<id>.x" (SrBlockFileLoad order; cached per area by id) -> parse_x -> per subset, the
// material's texture (only materials a subset uses) -> resolve_texture (ScDrawableTexture::Load rule) -> load ->
// decode_dds / decode_tga -> CPU box-filter mip chain (cached per area by resolved request). <area>_POS.slb gives the
// start position (first record with flag 0x1000).
// Upload + render (rhi): one vertex and index buffer per model, one RGBA8 texture per resolved name (+ a 1x1 white
// texture for untextured or missing ones), 2 mesh pipelines (opaque; alpha = blend + alpha test), one bind group per
// texture, one 256-byte uniform slot per draw. See REPORT.md (M1a-2 reference bundle) for every decision.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "lomn/fs.h"
#include "lomn/rhi.h"
#include "lomn/sage/slb.h"
#include "lomn/view/math.h"
#include "lomn/view/mips.h"

namespace lomn::view {

// World transform (spec M1 §2.1): RotX(kWorldRotX) then the OBJ record position in _41.._43 (GcArea::GcArea 0x00504220
// + GcObjectLoader::LoadObjects 0x0053fc60). Constant read from LEGOBionicle.exe .data @0x72a444.
constexpr float kWorldRotX = -1.5707982f;
constexpr uint32_t kPosStartFlag = 0x1000;

// Caps on what one area may make the renderer allocate (untrusted input, spec §5). Measured over all 123 area
// blockfiles of the Beta 1.5.0 install with `lomn_view --survey 1` (2026-10-07; every area loads, 0 failures):
//   drawn placements max 55 (lev6/ta03), draws max 165 (lev6/ta11), textures max 76 (lev2/vllg),
//   texture bytes incl. mips max 56,994,412 (lev0/frnt), vertex+index bytes max 1,270,872 (lev6/ta14).
// Headroom: 18.6x, 24.8x, 13.5x, 9.4x, 52.8x. The texture-byte cap is bounded by the wasm32 heap (2 GB, and the
// CPU copy lives until upload) rather than by a multiple; a rejection is a named error, never a partial area.
constexpr uint32_t kMaxPlacements = 1024;                // drawn OBJ records per area
constexpr uint32_t kMaxDraws = 4096;                     // placements x subsets per frame (uniform buffer <= 1 MiB)
constexpr uint32_t kMaxAreaTextures = 1024;              // distinct resolved textures per area
constexpr uint64_t kMaxAreaTextureBytes = 512ull << 20;  // RGBA8 incl. mips, per area
constexpr uint64_t kMaxAreaGeometryBytes = 64ull << 20;  // vertex + index bytes, per area

constexpr uint32_t kNoTexture = 0xffffffffu;  // subset drawn with the 1x1 white texture

struct AreaOptions {
  bool draw_main = true, draw_trans = true, draw_back = true, draw_moveable = true, draw_nolight = true;
};

struct SubsetDraw {
  uint32_t first_index = 0, index_count = 0;  // into Model::indices (triangle list)
  uint32_t texture = kNoTexture;              // index into AreaData::textures
  bool alpha = false;                         // texture IsAlphaOn (DXT3 / A8R8G8B8 / TGA with alpha bits): 2nd pass
  float diffuse[4] = {1, 1, 1, 1};            // material faceColor (white without a material)
};

struct Model {
  std::string id;                 // 4CC as in the OBJ record
  uint32_t vertex_count = 0;
  bool has_colors = false;        // MeshVertexColors present
  std::vector<float> vertices;    // interleaved, kVertexFloats per vertex: position xyz, uv, colour rgba
  std::vector<uint32_t> indices;  // triangle list, sorted by attribute id (parse_x)
  std::vector<SubsetDraw> subsets;
  float bounds_min[3] = {0, 0, 0}, bounds_max[3] = {0, 0, 0};  // model space
};
constexpr uint32_t kVertexFloats = 9;  // 36-byte stride

struct Texture {
  std::string request;   // resolved request ("Root/Data/Levels/lev1/textures/<name>.dds")
  std::string format;    // "DXT1" | "DXT3" | "DXT5" | "A8R8G8B8" | "TGA24" | "TGA32" | "TGA32a"
  bool alpha_on = false;
  std::vector<MipLevel> mips;
};

struct Placement {
  uint32_t record = 0;  // index in the OBJ table
  uint32_t model = 0;   // index into AreaData::models
  sage::Route route = sage::Route::Ignored;
  uint32_t flags = 0;
  float pos[3] = {0, 0, 0};
  float orient[3] = {0, 0, 0};  // NOT applied (moveables use SetOrientation; out of M1a-2 scope)
  Mat4 world;                   // RotX(kWorldRotX) * T(pos)
};

struct AreaStats {
  double load_ms = 0;    // blockfile headers, SLB tables, .x reads + parses, texture entry reads (excludes decode_ms)
  double decode_ms = 0;  // DDS/TGA decode + mip generation
  uint32_t obj_records = 0, placements = 0, ignored_records = 0, disabled_records = 0, missing_models = 0;
  uint32_t routes[6] = {0, 0, 0, 0, 0, 0};  // placements per sage::Route value
  uint32_t models = 0, subsets = 0, alpha_subsets = 0, untextured_subsets = 0, vertices = 0, triangles = 0;
  uint32_t models_without_colors = 0, placements_with_orientation = 0;
  uint32_t textures = 0, textures_missing = 0, textures_failed = 0, textures_alpha = 0;
  uint64_t texture_bytes = 0;   // RGBA8 incl. full mip chains (what is uploaded)
  uint64_t geometry_bytes = 0;  // vertex + index bytes
  uint32_t draws = 0;           // placements x subsets (one indexed draw each)
};

struct AreaData {
  std::string level, area;
  bool art_blk = false;  // Art.blk opened (optional: without it only the area and level blockfiles are searched)
  std::vector<Model> models;
  std::vector<Texture> textures;
  std::vector<Placement> placements;
  uint32_t pos_records = 0;
  bool has_start = false;
  uint32_t start_record = 0;
  float start[3] = {0, 0, 0};
  bool has_bounds = false;  // world AABB of every placed model (all routes except back)
  float bounds_min[3] = {0, 0, 0}, bounds_max[3] = {0, 0, 0};
  AreaStats stats;
  std::vector<std::string> warnings;  // sorted, deduplicated
};

// False only when the area cannot be entered (no level/area blockfile) or a cap is exceeded; missing models or
// textures are warnings (counted in stats) like the game's skip-and-continue loaders.
bool load_area(const fs::FileSystem& fs, std::string_view level, std::string_view area, const AreaOptions& opt, AreaData& out,
               std::string* err);

// Camera (spec M1 §2.3). Only the eye position comes from data (first POS record with flag 0x1000); orientation, FOV
// and clip planes are a DOCUMENTED FALLBACK until M1a-3 mines the Base.osi camera literals. Defaults below.
struct CameraSettings {
  float fov_deg = 60.0f;      // vertical, D3DXMatrixPerspectiveFovRH fovy
  float near_z = 0.5f;
  float far_z = 4000.0f;
  float eye_height = 3.0f;    // added to the POS start y (the record marks where the Toa stands)
  float pitch_deg = -10.0f;   // negative looks down
  bool eye_set = false;       // --eye overrides the POS start (eye_height is then not added)
  float eye[3] = {0, 0, 0};
  bool yaw_set = false;       // default yaw: towards the centre of the area's world bounds (xz)
  float yaw_deg = 0.0f;       // 0 looks along -Z, 90 along +X
};

struct Camera {
  float eye[3] = {0, 0, 0}, at[3] = {0, 0, 0};
  float fov_deg = 0, near_z = 0, far_z = 0, yaw_deg = 0, pitch_deg = 0, aspect = 1;
  std::string eye_source;  // "pos_start" | "cli" | "bounds_centre" | "origin"
  std::string yaw_source;  // "cli" | "towards_bounds_centre" | "default"
  Mat4 view, proj;
};
Camera make_camera(const AreaData& area, const CameraSettings& s, float aspect);

struct RenderOptions {
  bool vertex_colors = true;  // diffuse = vertex colour when present (D3DMCS_COLOR1), else material diffuse
  float alpha_ref = 1.0f;     // ALPHAREF (0..255): alpha pass discards a*255 <= ref (ALPHAFUNC GREATER)
  float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};  // 0xff000000
};

class AreaRenderer {
 public:
  // Creates every GPU object for `area` (which must outlive the renderer's use of it). False on a device failure.
  bool upload(rhi::Device& dev, const AreaData& area, const RenderOptions& opt, std::string* err);
  // One frame into `target` (a render target with depth). Draw order: main, moveable, no-light, back, then trans
  // sorted far-to-near from the eye; within each object the opaque subsets, then the alpha subsets (DrawX's 2 passes).
  void render(rhi::Device& dev, rhi::TextureId target, const AreaData& area, const Camera& cam);
  uint32_t pipelines() const { return pipelines_created_; }
  uint64_t uploaded_texture_bytes() const { return uploaded_texture_bytes_; }
  uint32_t textures_created() const { return textures_created_; }

 private:
  struct GpuModel {
    rhi::BufferId vb = 0, ib = 0;
  };
  RenderOptions opt_;
  rhi::PipelineId opaque_ = 0, alpha_ = 0;
  rhi::SamplerId sampler_ = 0;
  rhi::BufferId uniforms_ = 0;
  uint32_t uniform_slots_ = 0;
  std::vector<GpuModel> models_;
  std::vector<rhi::BindGroupId> texture_groups_;  // per AreaData::textures index
  rhi::BindGroupId white_group_ = 0;
  uint32_t pipelines_created_ = 0, textures_created_ = 0;
  uint64_t uploaded_texture_bytes_ = 0;
  std::vector<uint8_t> uniform_data_;
};

// The WGSL used by both pipelines (exposed for tests and the call-log checksum).
const char* area_wgsl();
constexpr uint32_t kAreaUniformBytes = 96;  // mat4 wvp, vec4 diffuse, vec4 params

}  // namespace lomn::view
