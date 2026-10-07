#include "lomn/sage/slb.h"

#include <algorithm>

#include "bytes.h"

namespace lomn::sage {
using detail::fail;
using detail::in_range;
using detail::le32;
using detail::lef32;

bool Slb::parse(std::vector<uint8_t> bytes, std::string* err) {
  bytes_ = std::move(bytes);
  fields_.clear();
  has_trailer_ = false;
  if (bytes_.size() > 0xffffffffu) return fail(err, "slb: larger than 4 GiB");
  uint32_t len = static_cast<uint32_t>(bytes_.size());
  data_end_ = len;
  if (len < 8) return true;  // no room for magic + count: GetSLKFile would find no magic either
  uint32_t m = (len - 4) & ~3u;
  if (le32(bytes_.data() + m) != kSlbMagic) return true;  // no trailer: used without fix-ups, like the original
  uint32_t count = le32(bytes_.data() + m - 4);
  if (count > (m - 4) / 4) return fail(err, "slb: fix-up count larger than the buffer");
  uint32_t table = m - 4 - 4 * count;
  fields_.reserve(count);
  for (uint32_t k = 1; k <= count; ++k) {
    uint32_t field = le32(bytes_.data() + m - 4 - 4 * k) & ~3u;
    if (!in_range(table, field, 4)) return fail(err, "slb: fix-up slot " + std::to_string(field) + " outside the data area");
    uint32_t target = le32(bytes_.data() + field);
    if (target >= len) return fail(err, "slb: fix-up target " + std::to_string(target) + " outside the buffer");
    fields_.push_back(field);
  }
  std::sort(fields_.begin(), fields_.end());
  if (std::adjacent_find(fields_.begin(), fields_.end()) != fields_.end()) return fail(err, "slb: pointer field fixed up twice");
  has_trailer_ = true;
  data_end_ = table;
  return true;
}

bool Slb::is_pointer_field(uint32_t offset) const { return std::binary_search(fields_.begin(), fields_.end(), offset); }

bool Slb::read_u32(uint64_t offset, uint32_t& v) const {
  if (!in_range(data_end_, offset, 4)) return false;
  v = le32(bytes_.data() + offset);
  return true;
}

bool Slb::read_f32(uint64_t offset, float& v) const {
  if (!in_range(data_end_, offset, 4)) return false;
  v = lef32(bytes_.data() + offset);
  return true;
}

bool Slb::read_ptr(uint64_t offset, uint32_t& target) const {
  if (offset > 0xffffffffu || !is_pointer_field(static_cast<uint32_t>(offset))) return false;
  return read_u32(offset, target);
}

std::string fourcc(uint32_t id) {
  std::string s;
  for (int sh = 24; sh >= 0; sh -= 8) {
    char c = static_cast<char>((id >> sh) & 0xff);
    if (c == 0) break;
    s.push_back(c);
  }
  return s;
}

// Table header shared by the typed views: count at `count_off`, records pointer at `ptr_off`.
static bool table(const Slb& slb, uint32_t count_off, uint32_t ptr_off, size_t stride, uint32_t& n, uint32_t& base,
                  const char* what, std::string* err) {
  if (!slb.read_u32(count_off, n)) return fail(err, std::string(what) + ": header truncated");
  if (!slb.read_ptr(ptr_off, base)) return fail(err, std::string(what) + ": records pointer is not a fix-up slot");
  // ptr + n*stride <= data_end, as "n <= (data_end - ptr) / stride" so it cannot wrap.
  if (base > slb.data_end() || n > (slb.data_end() - base) / stride) return fail(err, std::string(what) + ": record array outside the buffer");
  return true;
}

static void vec3(const uint8_t* p, float* v) {
  for (int i = 0; i < 3; ++i) v[i] = lef32(p + 4 * i);
}

bool parse_obj(const Slb& slb, std::vector<ObjRecord>& out, std::string* err) {
  out.clear();
  uint32_t n = 0, base = 0;
  if (!table(slb, 8, 12, kObjRecordSize, n, base, "obj", err)) return false;
  out.resize(n);
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t* r = slb.bytes().data() + base + static_cast<size_t>(i) * kObjRecordSize;
    ObjRecord& o = out[i];
    o.id = le32(r);
    vec3(r + 0x04, o.pos);
    vec3(r + 0x10, o.orient);
    o.unk1c = le32(r + 0x1c);
    vec3(r + 0x20, o.ext0);
    vec3(r + 0x2c, o.ext1);
    o.flags = le32(r + 0x38);
  }
  return true;
}

bool parse_pos(const Slb& slb, std::vector<PosRecord>& out, std::string* err) {
  out.clear();
  uint32_t n = 0, base = 0;
  if (!table(slb, 4, 8, kPosRecordSize, n, base, "pos", err)) return false;
  out.resize(n);
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t* r = slb.bytes().data() + base + static_cast<size_t>(i) * kPosRecordSize;
    out[i].id = le32(r);
    vec3(r + 4, out[i].pos);
    out[i].flags = le32(r + 0x10);
  }
  return true;
}

bool parse_light(const Slb& slb, std::vector<LightRecord>& out, std::string* err) {
  out.clear();
  uint32_t n = 0, base = 0;
  if (!table(slb, 4, 8, kLightRecordSize, n, base, "light", err)) return false;
  out.resize(n);
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t* r = slb.bytes().data() + base + static_cast<size_t>(i) * kLightRecordSize;
    LightRecord& l = out[i];
    l.id = le32(r);
    vec3(r + 4, l.pos);
    l.range = lef32(r + 0x10);
    l.param = lef32(r + 0x14);
    for (int k = 0; k < 4; ++k) l.color[k] = lef32(r + 0x18 + 4 * k);
    l.flags = le32(r + 0x28);
  }
  return true;
}

// Task 8B C1: the record count is capped (kMaxSpecTextures) BEFORE anything is allocated, and the table is built into a
// local vector that replaces `out` only on success: a rejected table leaves `out` empty, never a partial record list.
bool parse_spec_textures(const Slb& slb, std::vector<SpecTextureRecord>& out, std::string* err) {
  out.clear();
  uint32_t n = 0, base = 0;
  if (!table(slb, 4, 8, kSpecTextureRecordSize, n, base, "textures", err)) return false;
  if (n > kMaxSpecTextures) return fail(err, "textures: more than kMaxSpecTextures records");
  std::vector<SpecTextureRecord> recs(n);
  const std::vector<uint8_t>& b = slb.bytes();
  for (uint32_t i = 0; i < n; ++i) {
    const uint64_t r = base + uint64_t(i) * kSpecTextureRecordSize;
    SpecTextureRecord& t = recs[i];
    uint32_t name = 0;
    if (!slb.read_ptr(r, name)) return fail(err, "textures: record " + std::to_string(i) + " name is not a fix-up slot");
    for (uint64_t p = uint64_t(name) + 1; p < b.size() && b[static_cast<size_t>(p)] != 0; ++p) {
      if (t.name.size() >= kMaxSpecTextureName) return fail(err, "textures: name longer than kMaxSpecTextureName");
      t.name.push_back(static_cast<char>(b[static_cast<size_t>(p)]));
    }
    t.trans = lef32(b.data() + r + 4);
    t.u = lef32(b.data() + r + 8);
    t.v = lef32(b.data() + r + 12);
    t.frames = le32(b.data() + r + 16);
  }
  out = std::move(recs);
  return true;
}

Route route(uint32_t flags) {
  if (flags & 0x10) {
    if (flags & 0x20) return Route::Moveable;
    return (flags & 0x100) ? Route::Trans : Route::Main;
  }
  if (flags & 0x20) return Route::NoLightMoveable;
  if (flags & 0x08) return Route::Back;
  return Route::Ignored;
}

const char* route_name(Route r) {
  switch (r) {
    case Route::Main: return "main";
    case Route::Trans: return "trans";
    case Route::Back: return "back";
    case Route::Moveable: return "moveable";
    case Route::NoLightMoveable: return "nolight";
    default: return "ignored";
  }
}

bool is_cinematic_obj(const std::string& entry_name) {
  std::string n;
  for (char c : entry_name) n.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
  return n.size() >= 11 && n.compare(0, 3, "cin") == 0 && n.compare(n.size() - 8, 8, "_obj.slb") == 0;
}

}  // namespace lomn::sage
