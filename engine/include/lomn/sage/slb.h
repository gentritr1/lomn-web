// SLB ("SLK binary") tables: a little-endian memory image of a C struct with a pointer fix-up trailer, loaded
// by ScSLKManager::GetSLKFile (0x004032e0). Pointers stay data-relative offsets here (never absolute
// addresses), and the typed record views are decoded field by field.
//
// Trailer, exactly as GetSLKFile reads it (len = byte length of the entry):
//   m = (len - 4) & ~3                      u32 at m must be 0x00C0FFEE, else no fix-ups at all
//   count = u32 at m - 4
//   slot k (k = 1..count) = u32 at m - 4 - 4k; the pointer FIELD is at (slot & ~3), and holds a data-relative
//   offset that the game turns into base + offset. Only the field position is masked; the target is not,
//   so targets may be unaligned (OBJ records start at 0x16).
// Bounds (spec §5): count*4 must fit before m - 4; every field (4 bytes) must lie in the data area before the
// slot table, a field may be listed once, and every target must lie inside the buffer (target < len).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace lomn::sage {

constexpr uint32_t kSlbMagic = 0x00C0FFEEu;

class Slb {
 public:
  bool parse(std::vector<uint8_t> bytes, std::string* err);

  const std::vector<uint8_t>& bytes() const { return bytes_; }
  bool has_trailer() const { return has_trailer_; }
  uint32_t data_end() const { return data_end_; }  // first byte of the slot table (== size when no trailer)
  const std::vector<uint32_t>& pointer_fields() const { return fields_; }  // masked field offsets, sorted
  bool is_pointer_field(uint32_t offset) const;

  // Bounds-checked field reads (offsets need not be aligned). read_ptr also requires the field to be a fix-up
  // slot, and yields the data-relative target.
  bool read_u32(uint64_t offset, uint32_t& v) const;
  bool read_f32(uint64_t offset, float& v) const;
  bool read_ptr(uint64_t offset, uint32_t& target) const;

 private:
  std::vector<uint8_t> bytes_;
  std::vector<uint32_t> fields_;
  bool has_trailer_ = false;
  uint32_t data_end_ = 0;
};

// SrAsString on an ScIdentifier: the 4CC is a LE u32 whose most significant byte is the first character
// (file bytes "wllv" -> "vllw"). Trailing NULs are dropped.
std::string fourcc(uint32_t id);

// <area>_OBJ.slb header {u32 slot (overwritten with the area id), u32 n, u32 n2, ptr records}; LoadObjects
// iterates n2 records (data+8) of 60 bytes from the pointer at +12.
struct ObjRecord {
  uint32_t id = 0;            // +0x00 4CC = model name
  float pos[3] = {};          // +0x04 added into the world matrix _41.._43
  float orient[3] = {};       // +0x10 (SetOrientation, moveables)
  uint32_t unk1c = 0;         // +0x1c not read by LoadObjects (spec §2.1's 56-byte field list omits it)
  float ext0[3] = {};         // +0x20 InitializeCollisionPhysics extents (moveables)
  float ext1[3] = {};         // +0x2c
  uint32_t flags = 0;         // +0x38 routing bits; bit 0 also selects the collision mode (1 vs 0x400)
};
constexpr size_t kObjRecordSize = 60;

// <area>_POS.slb header {u32 slot, u32 n, ptr records} (GcPositionLoader::LoadPositions 0x00502a50).
struct PosRecord {
  uint32_t id = 0;     // +0x00
  float pos[3] = {};   // +0x04
  uint32_t flags = 0;  // +0x10; 0x1000 = start position, 0x40|0x20 = pickup sprite
};
constexpr size_t kPosRecordSize = 20;

// <area>_LIGHT.slb header {u32 slot, u32 n, ptr records} (GcLightLoader::LoadLights 0x00470010).
struct LightRecord {
  uint32_t id = 0;        // +0x00
  float pos[3] = {};      // +0x04 position (point) or direction (directional)
  // M1a-3 (GcLightLoader::LoadLights 0x00470010 read in full): +0x10 is the INTENSITY (GcLight ctor -> SetIntensity,
  // x1.2 above 0.9) and +0x14 the point light's RANGE (SetPointLight: > 0 -> attenuation1 = 1/(0.15 range), |v| > 1e-5
  // selects attenuation algorithm 2). The M1a-1 field names are kept.
  float range = 0;        // +0x10 intensity (see above)
  float param = 0;        // +0x14 range (see above)
  float color[4] = {};    // +0x18 r, g, b, a
  uint32_t flags = 0;     // +0x28; 0x40 point light, 0x80 directional (+ opposite "Shad" fill)
};
constexpr size_t kLightRecordSize = 44;

// M1a-3: <lev>/textures.slb (GcSpecTextureLoader::LoadTextures 0x0046f270): header {u32 n, u32 n, ptr records}, 20-byte
// records {ptr name, f32 transparency, f32 u speed, f32 v speed, u32 animation frames}. The name pointer targets a
// length-prefixed string; the game reads the C string at name + 1 (the length byte is ignored), so do we, bounded by
// the buffer and kMaxSpecTextureName.
struct SpecTextureRecord {
  std::string name;
  float trans = 1.0f, u = 0.0f, v = 0.0f;
  uint32_t frames = 0;
};
constexpr size_t kSpecTextureRecordSize = 20;
constexpr size_t kMaxSpecTextureName = 255;
bool parse_spec_textures(const Slb& slb, std::vector<SpecTextureRecord>& out, std::string* err);

bool parse_obj(const Slb& slb, std::vector<ObjRecord>& out, std::string* err);
bool parse_pos(const Slb& slb, std::vector<PosRecord>& out, std::string* err);
bool parse_light(const Slb& slb, std::vector<LightRecord>& out, std::string* err);

// Placement routing of GcObjectLoader::LoadObjects (0x0053fc60), in the decompiled test order:
//   0x10 set:   0x20 -> Moveable (GcAreaDirector::AddMoveableObj); else 0x100 -> Trans; else Main
//   0x10 clear: 0x20 -> NoLightMoveable; else 0x08 -> Back; else Ignored
// NOTE: spec §2.1 lists 0x8 first; the code tests 0x8 only when 0x10 and 0x20 are both clear. The two agree on
// every record in the Beta data (no record combines 0x8 with 0x10/0x20); see REPORT.md.
enum class Route { Ignored, Main, Trans, Back, Moveable, NoLightMoveable };
Route route(uint32_t flags);
const char* route_name(Route r);  // "ignored" | "main" | "trans" | "back" | "moveable" | "nolight"

// Cinematic tables use another header and are not placement tables: the whole entry name must match
// cin*_obj.slb case-insensitively (no directory stripping: BLK entry names carry no path).
bool is_cinematic_obj(const std::string& entry_name);

}  // namespace lomn::sage
