// M1a-2 "first pixels" + M1a-3 "faithful states": an area's static geometry, loaded the way GcAreaLoader::LoadArea does,
// and drawn by replaying the decompiled D3D8 call sequence (lomn/view/sage_frame.h) on the state shim (lomn/d3d/), which
// compiles every DrawSubset to rhi.
//
// Load (CPU only, no device): Art.blk header (optional), enter_area (second = <lev>.blk, current = <area>.blk),
// <area>_OBJ.slb -> route() per record -> for every record routed main / trans / back / moveable / no-light moveable:
// model "<area prefix>/Xs/<id>.x" (SrBlockFileLoad order; cached per area by id) -> parse_x -> per subset, the
// material's texture (only materials a subset uses) -> resolve_texture (ScDrawableTexture::Load rule) -> load ->
// decode_dds / decode_tga -> CPU box-filter mip chain (cached per area by resolved request). <area>_POS.slb gives the
// start position (first record with flag 0x1000).
// M1a-3 additions to the load: per-subset D3DMATERIAL8 (ScDrawableMaterial::LoadIndexed 0x00550060), the level's
// textures.slb (GcSpecTextureLoader::LoadTextures 0x0046f270), <area>_LIGHT.slb or the default lights
// (GcLightLoader::LoadLights 0x00470010, GrSetDefaultLighting 0x00470560), and the area script's fog / cave-lighting
// literals from data/Base.osi (lomn/sage/osi.h).
// Upload + render (rhi): one vertex and index buffer per model, one RGBA8 texture per resolved name (+ a 1x1 white
// texture standing in for a NULL stage-0 texture), pipelines created lazily per d3d::PipelineKey, one bind group per
// (texture, sampler), one 1024-byte uniform slot per draw. See the M1a-2 and M1a-3 reference REPORTs.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "lomn/d3d/device8.h"
#include "lomn/fs.h"
#include "lomn/rhi.h"
#include "lomn/sage/slb.h"
#include "lomn/view/math.h"
#include "lomn/view/mips.h"

namespace lomn::view {

// World transform (spec M1 §2.1): RotX(kWorldRotX) then the OBJ record position in _41.._43 (GcArea::GcArea 0x00504220
// + GcObjectLoader::LoadObjects 0x0053fc60). Constant read from LEGOBionicle.exe .data @0x72a444.
constexpr float kWorldRotX = -1.5707982f;
// M1b: the two files every area load reads first (GcGame::LoadArtBlockHeader's Art.blk table, the script file whose
// area class supplies fog): one definition, shared by load_area and the browser's boot prefetch (lomn/view/prefetch.h).
constexpr const char* kArtBlkPath = "Root/Data/Art/Art.blk";
constexpr const char* kBaseOsiPath = "Root/Data/Base.osi";
constexpr uint32_t kPosStartFlag = 0x1000;

// Caps on what one area may make the renderer allocate (untrusted input, spec §5). Measured over all 123 area
// blockfiles of the Beta 1.5.0 install with `lomn_view --survey 1` (2026-10-07; every area loads, 0 failures):
//   drawn placements max 55 (lev6/ta03), draws max 165 (lev6/ta11), textures max 76 (lev2/vllg),
//   texture bytes incl. mips max 56,994,412 (lev0/frnt), vertex+index bytes max 1,649,376 (lev6/ta14, 48-byte
//   vertices; 1,270,872 with the earlier 36-byte layout).
// Headroom: 18.6x, 24.8x, 13.5x, 4.7x, 40.7x. The texture-byte cap is bounded by the wasm32 heap (2 GB; the CPU
// mip chains exist until release_cpu_copies() after upload) rather than by a multiple; a rejection is a named error,
// never a partial area.
constexpr uint32_t kMaxPlacements = 1024;                // drawn OBJ records per area
constexpr uint32_t kMaxDraws = 4096;                     // placements x subsets per frame (uniform buffer <= 4 MiB)
constexpr uint32_t kMaxAreaTextures = 1024;              // distinct resolved textures per area
constexpr uint64_t kMaxAreaTextureBytes = 256ull << 20;  // RGBA8 incl. mips, per area
constexpr uint64_t kMaxAreaGeometryBytes = 64ull << 20;  // vertex + index bytes, per area
// Not caps on what is drawn but on the loader's own work and output (Task 6 cleanup):
// - kMaxStoredWarnings: AreaData::warnings keeps at most this many distinct warnings (the first emitted); the rest are
//   counted in AreaData::warnings_dropped. (1.11M missing-model records once produced 457k warnings.)
// - kMaxDrawableRecords = kMaxPlacements x 64: OBJ records routed to a drawn group (placed, missing model or non-finite)
//   examined per area; one more fails the load. Missing-model records therefore cannot make the loader do unbounded
//   model lookups. Real maximum: 55 records (lev6/ta03). Ignored/disabled records cost nothing and stay bounded only by
//   parse_obj (n * 60 <= table bytes).
constexpr uint32_t kMaxStoredWarnings = 1024;
// kMaxAreaLights: lights kept per area, counting LIGHT records, their "Shad" fill and the GrSetDefaultLighting lights
// (Task 8B D4: injectable via AreaOptions::max_lights, lower only; the lights over it are dropped with ONE warning,
// "lights: N over kMaxAreaLights dropped"). Measured maximum 209 lights (lev6/ta08, lomn_view --survey 1, 2026-10-07)
// -> 4,096 (19.6x; was 1,024 = 4.9x).
constexpr uint32_t kMaxAreaLights = 4096;
constexpr uint32_t kMaxDrawableRecords = kMaxPlacements * 64;
constexpr float kMaxPitchDeg = 89.9f;  // make_camera clamps the pitch to +-kMaxPitchDeg (LookAtRH degenerates at +-90)

constexpr uint32_t kNoTexture = 0xffffffffu;  // subset drawn with the 1x1 white texture

struct AreaOptions {
  bool draw_main = true, draw_trans = true, draw_back = true, draw_moveable = true, draw_nolight = true;
  // The R16 caps (+ kMaxAreaLights, Task 8B D4), injectable so tests can cross them with tiny inputs. They can only be
  // LOWERED: larger values are clamped to the defaults (the renderer sizes its uniform buffer from draws <= kMaxDraws).
  uint32_t max_placements = kMaxPlacements, max_draws = kMaxDraws, max_textures = kMaxAreaTextures;
  uint64_t max_texture_bytes = kMaxAreaTextureBytes, max_geometry_bytes = kMaxAreaGeometryBytes;
  uint32_t max_lights = kMaxAreaLights;
};

struct SubsetDraw {
  uint32_t first_index = 0, index_count = 0;  // into Model::indices (triangle list)
  uint32_t texture = kNoTexture;              // index into AreaData::textures
  bool alpha = false;                         // texture IsAlphaOn (DXT3 / A8R8G8B8 / TGA with alpha bits): 2nd pass
  float diffuse[4] = {1, 1, 1, 1};            // material faceColor (white without a material)
  // ---- M1a-3 ----
  uint32_t attrib = 0;           // D3DX attribute id (the DrawSubset argument; subsets are sorted by it)
  // has_material: the subset's attribute indexes an .x Material, so `material` / `diffuse` hold its values. The game's
  // ScDrawableMaterial exists only when the material is ALSO `textured` (LoadX 0x00553800 creates one only for a
  // TextureFilename): the replay tests `textured`, never this (Task 8B D6).
  bool has_material = false;
  bool textured = false;         // the material names a texture (ScDrawableMaterial::SetTexture is a no-op otherwise)
  std::string texture_name;      // TextureFilename as in the .x (textures.slb matches it with strcmp)
  d3d::Material8 material;       // LoadIndexed: .x diffuse/specular/power/emissive, ambient forced 0.7, name overrides
  float trans = 1.0f;            // StTextureData::mTrans: 0 for "_glass" names, a textures.slb value < 1, else 1
  float uv_offset[2] = {0, 0};   // textures.slb U/V scroll speeds (SetUVOffset); TTF COUNT2 when non-zero
  bool special = false;          // matched a textures.slb record
};

struct Model {
  std::string id;                 // 4CC as in the OBJ record
  uint32_t vertex_count = 0;
  bool has_colors = false;        // MeshVertexColors present
  std::vector<float> vertices;    // interleaved, kVertexFloats per vertex: position xyz, normal xyz, uv, colour rgba
  std::vector<uint32_t> indices;  // triangle list, sorted by attribute id (parse_x)
  std::vector<SubsetDraw> subsets;
  float bounds_min[3] = {0, 0, 0}, bounds_max[3] = {0, 0, 0};  // model space
  // ScDrawableModel::GetGreatestZ (0x00554c20): the LAST vertex (buffer order) with the greatest model-space Y. The
  // trans sort adds the placement position to it (GcArea::GetGreatestZ 0x00506bc0) without rotating it.
  float greatest_y[3] = {0, 0, 0};
};
// 48-byte stride: position @0 (@location(0)), normal @12 (@location(1); parse_x's per-split-vertex normal, zero
// without MeshNormals; unused by the unlit shader, there for M1a-3 lighting), uv @24 (@location(2)), colour rgba @32
// (@location(3); white without MeshVertexColors).
constexpr uint32_t kVertexFloats = 12;

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
  uint32_t nonfinite_records = 0;  // drawable records skipped for a NaN/Inf position or orientation (with a warning)
  uint32_t untextured_materials = 0;  // M1a-3: used subsets whose material has no TextureFilename (the original faults)
  uint32_t textures = 0, textures_missing = 0, textures_failed = 0, textures_alpha = 0;
  uint64_t texture_bytes = 0;   // RGBA8 incl. full mip chains (what is uploaded)
  uint64_t geometry_bytes = 0;  // vertex + index bytes
  uint32_t draws = 0;           // placements x subsets (one indexed draw each)
};

// A light the area adds to GcLightDirector, already turned into the D3DLIGHT8 GcLight::Render hands to SetLight.
struct SceneLight {
  std::string name;  // 4CC (LIGHT record id, or "LmbA" / "SunL" / "Shad")
  d3d::Light8 light;
  d3d::Color4 base;   // the colour before intensity (GcFastLight +0x78; SetCaveLighting rescales from it)
  uint8_t style = 0;  // GeLightStyle from the record flags (0 = constant)
};
// GcGraphicsOptions fog statics after the area script ran (GcAreaDirector::SetFog 0x0048a7a0 negates start/end).
struct FogSettings {
  uint32_t color = 0xffffffffu;
  float start = 4701.0f, end = 4702.0f, density = 0.001f;  // defaults @0x0074a338..0x0074a344
  std::string source = "default";                          // "default" | "osi"
};

struct AreaData {
  std::string level, area;
  // ---- M1a-3 ----
  uint8_t level_type = 0;            // GcAreaLoader::LoadArea: (uint8)(level[3] - '0') -> LoadLights / GrSetDefaultLighting
  std::vector<SceneLight> lights;    // in GcLightDirector add order
  std::string lights_source;         // "light_slb" | "light_slb+default" | "default"
  FogSettings fog;
  bool cave_lighting = false;        // the area script called GcLightDirector::SetCaveLighting(1)
  std::string script_class;          // the Base.osi class whose InitFull was read ("" when none)
  uint32_t special_textures = 0;     // textures.slb records
  uint32_t special_matches = 0;      // subsets a textures.slb record applied to
  bool cpu_released = false;  // release_cpu_copies() ran: models' vertices/indices and textures' mips are empty
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
  std::vector<std::string> warnings;  // sorted, deduplicated, ASCII (file bytes escaped as \xHH), <= kMaxStoredWarnings
  uint32_t warnings_dropped = 0;      // distinct warnings beyond kMaxStoredWarnings
};

// False only when the area cannot be entered (no level/area blockfile) or a cap is exceeded; missing models or
// textures are warnings (counted in stats) like the game's skip-and-continue loaders. Records with a NaN/Inf position
// or orientation are skipped with a warning (stats.nonfinite_records). On false, `out` is a default AreaData.
// Non-finite values contract (Task 8B D3; untrusted input): no NaN/inf from a file reaches the renderer. Skipped with a
// warning: an OBJ record with a non-finite position/orientation, a LIGHT record with a non-finite position/direction,
// intensity, range or colour ("light record N: non-finite value (skipped)"; it then counts neither as a directional
// light nor for "Shad"), a textures.slb record with a non-finite transparency or U/V speed ("textures.slb record N:
// ..."), and a SetFog whose start/end/density is non-finite or outside the float range ("osi: SetFog with a non-finite
// or out-of-range value (ignored)"; the previous SetFog, or the default fog, stays). A point light's attenuation1 is
// finite for every finite range > 0 (FLT_MAX where 0.15 * range underflows; the game's x87 code would store +inf).
// Material values from .x files are not filtered here (d3d::quantize_unorm8 and the shader clamp them).
// `kind` (optional) classifies the failure: Environment = the level or area blockfile does not exist / cannot be opened
// (lomn_view exit 3); Data = everything else: a corrupt blockfile or an exceeded cap (exit 4). None on success.
enum class LoadError { None, Environment, Data };
bool load_area(const fs::FileSystem& fs, std::string_view level, std::string_view area, const AreaOptions& opt, AreaData& out,
               std::string* err, LoadError* kind = nullptr);

// Camera (spec M1 §2.3). Only the eye position comes from data (first POS record with flag 0x1000); orientation, FOV
// and clip planes are a DOCUMENTED FALLBACK until M1a-3 mines the Base.osi camera literals. Defaults below.
// lomn_view's metrics JSON flags it: camera.fallback is true while ANY of eye/yaw/pitch/fov/near/far is a fallback,
// with per-field eye_source, yaw_source, pitch_source, fov_source, clip_source: "cli" (the flag(s) given), "exe" (M1a-3:
// the exe's own value, real installs; fov and clip), "fallback" (the documented default; the synthetic scene's values),
// and for clip "cli_partial" (Task 8B F1: only one of --near / --far given).
// M1a-3: FOV, near and far are the exe's own values: GcViewPort::GcViewPort (0x004479f0) builds the main GcLegoCamera with
// fov 72 deg (@0x00704e00, ScPerspectiveCamera x pi/180 @0x0070d508, vertical fovy of D3DXMatrixPerspectiveFovRH), near
// 0.01 (@0x00704e1c) and far = GcGraphicsOptions::GetDrawDistance() = 4700 (mDrawDistance @0x0074a328); Base.osi never
// calls SetDrawDistance or a FOV setter (tools/research/osi.py). lomn_view reports them with source "exe".
struct CameraSettings {
  float fov_deg = 72.0f;      // vertical, D3DXMatrixPerspectiveFovRH fovy
  float near_z = 0.01f;
  float far_z = 4700.0f;
  float eye_height = 3.0f;    // added to the POS start y (the record marks where the Toa stands)
  float pitch_deg = -10.0f;   // negative looks down; clamped to +-kMaxPitchDeg
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
  bool vertex_colors = true;          // false: draw as if the vertex format had no DIFFUSE (debug)
  bool force_area_lighting = false;   // true: LIGHTING on for the area nodes, as with night vision (debug / evidence)
  bool fog = true;                    // false: never fog (debug)
  std::vector<std::string>* state_log = nullptr;  // the D3D8 state log (A9), appended per rendered frame when set
};
// Per-frame counters of the last render() (metrics JSON).
struct RenderStats {
  uint32_t state_calls = 0;      // state-setting D3D8 calls that reached the device in the frame
  uint32_t draw_subsets = 0;     // DrawSubset calls that drew faces (one DrawIndexedPrimitive each)
  uint32_t empty_subsets = 0;    // DrawSubset calls on attribute ids without faces (no device call)
  uint32_t rhi_draws = 0;        // indexed draws handed to rhi
  uint32_t unsupported = 0;      // OR of d3d::Unsupported bits seen at draws
  // Distinct (pipeline key + every state the uber-shader branches on) in the frame: the pipeline count a design that
  // generated one WGSL variant per fixed-function state would need (the uber-shader needs only `pipelines`).
  uint32_t shader_variants = 0;
  uint32_t lit_draws = 0, fogged_draws = 0, blended_draws = 0, alpha_tested_draws = 0, null_texture_draws = 0;
  // M1b frame-time breakdown (wall clock, CPU side): replay = the SageFrame replay on the D3D8 shim + draw compilation
  // (pipelines/bind groups looked up, uniforms packed); submit = the rhi calls (uniform write, begin_frame, draws,
  // end_frame). Not part of any log.
  double replay_ms = 0, submit_ms = 0;
};

// Frees the CPU-side vertex/index arrays and texture mip chains once they are on the GPU (upload copies them).
// Placements, subsets, stats and warnings stay, so render() keeps working; upload() refuses released data.
// Returns the bytes freed.
uint64_t release_cpu_copies(AreaData& area);

class AreaRenderer {
 public:
  // Creates every GPU object for `area` (which must outlive the renderer's use of it). False on a device failure.
  // ONE upload per renderer until release(): a repeat upload (after success or failure) is rejected without creating
  // anything. M1b: release() destroys the GPU objects, after which upload() may run again (another area, same device).
  bool upload(rhi::Device& dev, const AreaData& area, const RenderOptions& opt, std::string* err);
  // One frame into `target` (a render target with depth): the SageFrame replay (sage_frame.h) on a d3d::Device8 whose
  // DrawSink compiles every DrawSubset to an rhi draw (pipelines and (texture, sampler) bind groups created on first
  // use, one uniform slot per draw). Returns false, makes no rhi call and logs once on stderr unless upload() succeeded
  // for this very AreaData object. The D3D8 device persists across frames (render states are history-dependent).
  bool render(rhi::Device& dev, rhi::TextureId target, const AreaData& area, const Camera& cam);
  // M1b: destroys every GPU object this renderer created (also after a failed upload), bind groups first, and resets
  // the renderer to its never-uploaded state. Returns the objects destroyed; *failures (optional) = rejected destroys.
  uint32_t release(rhi::Device& dev, uint32_t* failures = nullptr);
  uint32_t pipelines() const { return pipelines_created_; }
  uint64_t uploaded_texture_bytes() const { return uploaded_texture_bytes_; }
  uint32_t textures_created() const { return textures_created_; }
  const RenderStats& last_stats() const { return stats_; }
  // Task 8B E3: the renderer owns its Impl (unique_ptr). Copying would share it, so it is deleted; moving hands it over
  // (the moved-from renderer then refuses to render). The texture names the state log prints live inside the Impl.
  AreaRenderer();
  AreaRenderer(const AreaRenderer&) = delete;
  AreaRenderer& operator=(const AreaRenderer&) = delete;
  AreaRenderer(AreaRenderer&&) noexcept;
  AreaRenderer& operator=(AreaRenderer&&) noexcept;
  ~AreaRenderer();

 private:
  struct GpuModel {
    rhi::BufferId vb = 0, ib = 0;
  };
  class Sink;
  RenderOptions opt_;
  rhi::BufferId uniforms_ = 0;
  uint32_t uniform_slots_ = 0;
  std::vector<GpuModel> models_;
  std::vector<rhi::TextureId> gpu_textures_;  // per AreaData::textures index
  std::vector<rhi::TextureId> created_textures_;  // M1b: every texture upload() created (release() destroys them)
  std::vector<rhi::BufferId> created_buffers_;    // M1b: every vertex/index buffer upload() created
  rhi::TextureId white_texture_ = 0;
  uint32_t pipelines_created_ = 0, textures_created_ = 0, frames_ = 0;
  uint64_t uploaded_texture_bytes_ = 0;
  std::vector<uint8_t> uniform_data_;
  const AreaData* uploaded_ = nullptr;  // set only by a successful upload
  bool upload_attempted_ = false, render_refused_logged_ = false;
  RenderStats stats_;
  struct Impl;
  // d3d::Device8 + SageFrame + texture names + pipeline / sampler / bind-group caches (lives across frames).
  std::unique_ptr<Impl> impl_;
};

// The fixed-function uber-shader (lomn/d3d/ffp.h), exposed for tests and the call-log checksum.
const char* area_wgsl();

}  // namespace lomn::view
