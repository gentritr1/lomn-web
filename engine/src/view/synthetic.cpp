#include "lomn/view/synthetic.h"

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

namespace lomn::view {
namespace {

using Bytes = std::vector<uint8_t>;

void put16(Bytes& b, uint16_t v) {
  b.push_back(static_cast<uint8_t>(v));
  b.push_back(static_cast<uint8_t>(v >> 8));
}
void put32(Bytes& b, uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void putf(Bytes& b, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  put32(b, u);
}
void putstr(Bytes& b, const std::string& s) { b.insert(b.end(), s.begin(), s.end()); }
uint32_t id4(const char* s) {  // "vllw" -> LE u32 whose file bytes read "wllv"
  return (uint32_t(uint8_t(s[0])) << 24) | (uint32_t(uint8_t(s[1])) << 16) | (uint32_t(uint8_t(s[2])) << 8) | uint32_t(uint8_t(s[3]));
}

// "BLKF", version 1, entries sorted by name (the game's Find binary-searches), stored (flags 2), then payloads.
Bytes make_blk(std::vector<std::pair<std::string, Bytes>> es) {
  std::sort(es.begin(), es.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  Bytes b;
  putstr(b, "BLKF");
  put32(b, 1);
  put32(b, static_cast<uint32_t>(es.size()));
  uint32_t off = static_cast<uint32_t>(12 + 56 * es.size());
  for (const auto& e : es) {
    Bytes name(40, 0);
    std::memcpy(name.data(), e.first.data(), std::min<size_t>(e.first.size(), 39));
    b.insert(b.end(), name.begin(), name.end());
    put32(b, static_cast<uint32_t>(e.second.size()));
    put32(b, static_cast<uint32_t>(e.second.size()));
    put32(b, 2);
    put32(b, off);
    off += static_cast<uint32_t>(e.second.size());
  }
  for (const auto& e : es) b.insert(b.end(), e.second.begin(), e.second.end());
  return b;
}

Bytes dds_header(uint32_t w, uint32_t h, const char* fourcc) {
  Bytes b;
  putstr(b, "DDS ");
  put32(b, 124);
  put32(b, 0x1007);
  put32(b, h);
  put32(b, w);
  put32(b, 0);
  put32(b, 0);
  put32(b, 0);  // mip count 0, like every Beta DDS
  for (int i = 0; i < 11; ++i) put32(b, 0);
  put32(b, 32);
  if (fourcc) {
    put32(b, 0x4);
    putstr(b, fourcc);
    for (int i = 0; i < 5; ++i) put32(b, 0);
  } else {  // A8R8G8B8
    put32(b, 0x41);
    put32(b, 0);
    put32(b, 32);
    put32(b, 0x00ff0000u);
    put32(b, 0x0000ff00u);
    put32(b, 0x000000ffu);
    put32(b, 0xff000000u);
  }
  put32(b, 0x1000);
  for (int i = 0; i < 4; ++i) put32(b, 0);
  return b;
}

Bytes red_dds() {  // DXT1 4x4: c0 = c1 = 0xF800 (red), all indices 0
  Bytes b = dds_header(4, 4, "DXT1");
  put16(b, 0xF800);
  put16(b, 0xF800);
  put32(b, 0);
  return b;
}

Bytes cut_dds() {  // A8R8G8B8 8x8: columns 0..3 opaque green, 4..7 transparent magenta
  Bytes b = dds_header(8, 8, nullptr);
  for (int y = 0; y < 8; ++y)
    for (int x = 0; x < 8; ++x) put32(b, x < 4 ? 0xff00ff00u : 0x00ff00ffu);
  return b;
}

// M1a-3 textures.
Bytes solid_argb(uint32_t argb) {  // A8R8G8B8 4x4, one colour (alpha on)
  Bytes b = dds_header(4, 4, nullptr);
  for (int i = 0; i < 16; ++i) put32(b, argb);
  return b;
}
Bytes solid_dxt1(uint16_t c565) {  // DXT1 4x4, one colour (alpha off)
  Bytes b = dds_header(4, 4, "DXT1");
  put16(b, c565);
  put16(b, c565);
  put32(b, 0);
  return b;
}

// Binary .x token writer (xof 0303bin 0032): NAME=1, STRING=2, INTEGER_LIST=6, FLOAT_LIST=7, '{'=10, '}'=11.
struct X {
  Bytes b;
  X() { putstr(b, "xof 0303bin 0032"); }
  X& name(const std::string& s) {
    put16(b, 1);
    put32(b, static_cast<uint32_t>(s.size()));
    putstr(b, s);
    return *this;
  }
  X& str(const std::string& s) {
    put16(b, 2);
    put32(b, static_cast<uint32_t>(s.size()));
    putstr(b, s);
    put16(b, 20);
    return *this;
  }
  X& ints(const std::vector<uint32_t>& v) {
    put16(b, 6);
    put32(b, static_cast<uint32_t>(v.size()));
    for (uint32_t x : v) put32(b, x);
    return *this;
  }
  X& floats(const std::vector<float>& v) {
    put16(b, 7);
    put32(b, static_cast<uint32_t>(v.size()));
    for (float x : v) putf(b, x);
    return *this;
  }
  X& open() {
    put16(b, 10);
    return *this;
  }
  X& close() {
    put16(b, 11);
    return *this;
  }
  X& obj(const std::string& type) { return name(type).open(); }
};

// A quad in the model's y = 0 plane (world z = 0 after RotX), x [x0,x1], model z [z0,z1] (world y), u along x.
void quad(std::vector<float>& pos, std::vector<float>& uv, std::vector<uint32_t>& faces, float x0, float x1, float z0, float z1) {
  const uint32_t base = static_cast<uint32_t>(pos.size() / 3);
  for (float v : {x0, 0.f, z0, x1, 0.f, z0, x1, 0.f, z1, x0, 0.f, z1}) pos.push_back(v);
  for (float v : {0.f, 1.f, 1.f, 1.f, 1.f, 0.f, 0.f, 0.f}) uv.push_back(v);
  for (uint32_t v : {4u, base, base + 1, base + 2, base + 3}) faces.push_back(v);
}

void material(X& x, std::initializer_list<float> diffuse, const char* texture) {
  std::vector<float> f(diffuse);
  for (float v : {0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f}) f.push_back(v);  // power, specular, emissive
  x.obj("Material").floats(f);
  if (texture) x.obj("TextureFilename").str(texture).close();
  x.close();
}

Bytes quad_x() {
  std::vector<float> pos, uv;
  std::vector<uint32_t> faces;
  quad(pos, uv, faces, -8, -1, -4, 4);   // subset 0: red
  quad(pos, uv, faces, 1, 8, -4, 4);     // subset 1: cut (alpha)
  quad(pos, uv, faces, 50, 60, -4, 4);   // subset 2: missing texture, off-screen
  X x;
  x.obj("Mesh").ints({12}).floats(pos);
  std::vector<uint32_t> fi = {3};
  fi.insert(fi.end(), faces.begin(), faces.end());
  x.ints(fi);
  // One normal (0,-1,0) for every corner (world +Z after RotX, towards the camera): no vertex is split.
  std::vector<uint32_t> ni = {3};  // nFaceNormals, then per face: 4 corners, normal 0
  for (int f = 0; f < 3; ++f) ni.insert(ni.end(), {4u, 0u, 0u, 0u, 0u});
  x.obj("MeshNormals").ints({1}).floats({0.f, -1.f, 0.f}).ints(ni).close();
  x.obj("MeshTextureCoords").ints({12}).floats(uv).close();
  x.obj("MeshVertexColors").ints({12});
  for (uint32_t i = 0; i < 12; ++i) x.ints({i}).floats({1.0f, 0.5f, 1.0f, 1.0f});
  x.close();
  x.obj("MeshMaterialList").ints({3, 3, 0, 1, 2});
  material(x, {1, 1, 1, 1}, "red");
  material(x, {1, 1, 1, 1}, "cut");
  material(x, {1, 1, 1, 1}, "nothere");
  x.close();
  x.close();
  return x.b;
}

// M1a-3: a mesh of quads in the model's y = 0 plane (world z = position z), one subset per quad, each with its own
// textured material (white diffuse), no vertex colours, no normals.
struct QuadSpec {
  float x0, x1, z0, z1;
  const char* texture;
};
Bytes quads_x(const std::vector<QuadSpec>& qs) {
  std::vector<float> pos, uv;
  std::vector<uint32_t> faces;
  for (const QuadSpec& q : qs) quad(pos, uv, faces, q.x0, q.x1, q.z0, q.z1);
  X x;
  const uint32_t n = static_cast<uint32_t>(qs.size());
  x.obj("Mesh").ints({4 * n}).floats(pos);
  std::vector<uint32_t> fi = {n};
  fi.insert(fi.end(), faces.begin(), faces.end());
  x.ints(fi);
  x.obj("MeshTextureCoords").ints({4 * n}).floats(uv).close();
  std::vector<uint32_t> ml = {n, n};
  for (uint32_t i = 0; i < n; ++i) ml.push_back(i);
  x.obj("MeshMaterialList").ints(ml);
  for (const QuadSpec& q : qs) material(x, {1, 1, 1, 1}, q.texture);
  x.close();
  x.close();
  return x.b;
}

Bytes blue_x() {
  std::vector<float> pos, uv;
  std::vector<uint32_t> faces;
  quad(pos, uv, faces, -8, 8, -2, 8);
  X x;
  x.obj("Mesh").ints({4}).floats(pos);
  std::vector<uint32_t> fi = {1};
  fi.insert(fi.end(), faces.begin(), faces.end());
  x.ints(fi);
  x.obj("MeshMaterialList").ints({1, 1, 0});
  material(x, {0, 0, 1, 1}, nullptr);
  x.close();
  x.close();
  return x.b;
}

struct Rec {
  const char* id;
  float x, y, z;
  uint32_t flags;
};

// {slot, n, n, ptr=0x16} + 60-byte records at 0x16, padding, one fix-up slot (12), count, 0x00C0FFEE (the Beta shape).
Bytes obj_slb(const std::vector<Rec>& recs) {
  Bytes b;
  put32(b, id4("????"));
  put32(b, static_cast<uint32_t>(recs.size()));
  put32(b, static_cast<uint32_t>(recs.size()));
  put32(b, 0x16);
  put16(b, 0);
  put32(b, 0);
  for (const Rec& r : recs) {
    put32(b, id4(r.id));
    putf(b, r.x);
    putf(b, r.y);
    putf(b, r.z);
    for (int k = 0; k < 3; ++k) putf(b, 0.0f);  // orient
    put32(b, 0);                                // +0x1c
    for (int k = 0; k < 6; ++k) putf(b, 0.0f);  // ext0, ext1
    put32(b, r.flags);
  }
  put16(b, 0);
  put32(b, 12);
  put32(b, 1);
  put32(b, 0x00C0FFEEu);
  return b;
}

// <area>_LIGHT.slb {slot, n, ptr=12} + 44-byte records {id, pos/dir, intensity, range, rgba, flags}.
struct LightSpec {
  const char* id;
  float x, y, z, intensity, range, r, g, b, a;
  uint32_t flags;
};
Bytes light_slb(const std::vector<LightSpec>& ls) {
  Bytes b;
  put32(b, 0);
  put32(b, static_cast<uint32_t>(ls.size()));
  put32(b, 12);
  for (const LightSpec& l : ls) {
    put32(b, id4(l.id));
    for (float v : {l.x, l.y, l.z, l.intensity, l.range, l.r, l.g, l.b, l.a}) putf(b, v);
    put32(b, l.flags);
  }
  put32(b, 8);
  put32(b, 1);
  put32(b, 0x00C0FFEEu);
  return b;
}

// <lev>/textures.slb {n, n, ptr=12} + 20-byte records {ptr name, trans, u, v, frames} + length-prefixed names.
struct SpecSpec {
  const char* name;
  float trans, u, v;
};
Bytes textures_slb(const std::vector<SpecSpec>& ss) {
  Bytes b;
  put32(b, static_cast<uint32_t>(ss.size()));
  put32(b, static_cast<uint32_t>(ss.size()));
  put32(b, 12);
  uint32_t str = static_cast<uint32_t>(12 + 20 * ss.size());
  std::vector<uint32_t> slots = {8};
  for (const SpecSpec& s : ss) {
    slots.push_back(static_cast<uint32_t>(b.size()));
    put32(b, str);
    putf(b, s.trans);
    putf(b, s.u);
    putf(b, s.v);
    put32(b, 0);
    str += static_cast<uint32_t>(std::strlen(s.name)) + 2;
  }
  for (const SpecSpec& s : ss) {
    b.push_back(static_cast<uint8_t>(std::strlen(s.name)));
    putstr(b, s.name);
    b.push_back(0);
  }
  while (b.size() % 4) b.push_back(0);
  for (size_t i = slots.size(); i-- > 0;) put32(b, slots[i]);
  put32(b, static_cast<uint32_t>(slots.size()));
  put32(b, 0x00C0FFEEu);
  return b;
}

// data/Base.osi (version 4.1): one class "lego_area_statt" (area "stat" of level "levt": suffix = the level name's 4th
// character, as for lev2..lev6) whose InitFull calls GcAreaDirector::SetFog(0xff808080, 0 - 5, 0 - 50, 0.1) and
// GcLightDirector::SetCaveLighting(1), compiled the way Base.osi's area scripts are.
Bytes base_osi() {
  Bytes b;
  putstr(b, std::string("OSI\0", 4));
  put16(b, 4);
  put16(b, 1);
  auto strings = [&](const std::vector<std::string>& v) {
    put16(b, static_cast<uint16_t>(v.size()));
    for (const std::string& s : v) {
      b.push_back(static_cast<uint8_t>(s.size()));
      putstr(b, s);
      b.push_back(0);
    }
  };
  strings({"gcareadirector", "setfog", "gclightdirector", "setcavelighting"});
  strings({});      // globals
  put16(b, 0);      // functions
  put16(b, 1);      // classes
  b.push_back(0);   // properties
  b.push_back(1);   // methods
  put16(b, 0);      // symbol 0 = "initfull"
  const size_t off_at = b.size();
  put32(b, 0);      // bytecode offset (patched)
  b.push_back(15);
  putstr(b, "lego_area_statt");
  b.push_back(0);
  strings({"initfull"});
  strings({});      // sources
  const uint32_t code = static_cast<uint32_t>(b.size());
  for (int i = 0; i < 4; ++i) b[off_at + i] = static_cast<uint8_t>(code >> (8 * i));
  b.push_back(0x40);
  put32(b, 0xff808080u);  // PushConstanti32 (-8355712)
  b.push_back(0x45);      // PushConstant0
  b.push_back(0x44);
  putf(b, 5.0f);
  b.push_back(0x71);      // Subtract -> -5
  b.push_back(0x45);
  b.push_back(0x44);
  putf(b, 50.0f);
  b.push_back(0x71);      // -> -50
  b.push_back(0x44);
  putf(b, 0.1f);
  b.push_back(0x82);      // CallGameFunction gcareadirector.setfog/4
  put16(b, 0);
  put16(b, 1);
  b.push_back(4);
  b.push_back(0x30);      // Pop
  b.push_back(0x43);
  b.push_back(1);         // PushConstanti8 1
  b.push_back(0x82);      // CallGameFunction gclightdirector.setcavelighting/1
  put16(b, 2);
  put16(b, 3);
  b.push_back(1);
  b.push_back(0x30);
  b.push_back(0x47);      // PushNothing
  b.push_back(0x52);      // Return
  return b;
}

Bytes pos_slb(const std::vector<Rec>& recs) {  // {slot, n, ptr=12} + 20-byte records
  Bytes b;
  put32(b, 0);
  put32(b, static_cast<uint32_t>(recs.size()));
  put32(b, 12);
  for (const Rec& r : recs) {
    put32(b, id4(r.id));
    putf(b, r.x);
    putf(b, r.y);
    putf(b, r.z);
    put32(b, r.flags);
  }
  put32(b, 8);
  put32(b, 1);
  put32(b, 0x00C0FFEEu);
  return b;
}

}  // namespace

bool mount_synthetic_install(fs::MemoryFileSystem& fs) {
  Bytes level = make_blk({{"red.dds", red_dds()},
                          {"cut.dds", cut_dds()},
                          {"half.dds", solid_argb(0x8000ff00u)},  // green, alpha 128
                          {"x_glass.dds", solid_dxt1(0xFFE0)},     // yellow; "_glass" -> mTrans 0
                          {"tfx.dds", solid_dxt1(0x001F)},         // blue; textures.slb trans 0.5
                          {"on_eye.dds", solid_dxt1(0x07E0)},      // green; "eye" + "on_" -> emissive (0,1,0) (Task 8B D2)
                          {"textures.slb", textures_slb({{"tfx", 0.5f, 0, 0}, {"red", 1, 0, 0}, {"cut", 1, 0.25f, 0}})}});
  Bytes area = make_blk({
      {"quad.x", quad_x()},
      {"blue.x", blue_x()},
      {"test_obj.slb", obj_slb({{"quad", 0, 0, 0, 0x10},
                                {"blue", 0, 0, -2, 0x10},
                                {"blue", 0, 0, -50, 0x08},
                                {"quad", 0, 0, -60, 0x110},
                                {"blue", 0, 0, -70, 0x110},
                                {"blue", 0, 0, -80, 0x20},
                                {"nope", 0, 0, 0, 0x04},
                                {"miss", 0, 0, 0, 0x10}})},
      {"test_pos.slb", pos_slb({{"pkup", 1, 2, 3, 0x60}, {"strt", 0, 0, 10, 0x1000}})},
  });
  // The A9 state scene: the A7 objects plus a plant ("plnt": fog toggle + DrawX context flag), "mixm" (alpha 128,
  // "_glass" mTrans 0, textures.slb TFACTOR 0.5), the two GcTransDirector::SortIt special ids (Task 8A E1) - "sky1"
  // (always first; placed NEAREST, so a distance sort would draw it last) and "watr" (only when last; placed FARTHEST,
  // so a distance sort would draw it first) - a LIGHT table and the script. The 'w'-suffix rule (also "only when last")
  // cannot share a Trans node with "watr": with two such ids left the original loop has no pick (see sage_frame.cpp),
  // so it is covered by test_sage_frame.cpp on a copy of this area with "watr" renamed. Both new models sit behind
  // the 'blue' quad @-2 (hidden: no pixel probe changes). Task 8B D2 adds "eyes" (an "on_eye" texture: LoadIndexed's
  // eye emissive), also behind the blue quad, as the last record.
  Bytes state = make_blk({
      {"quad.x", quad_x()},
      {"blue.x", blue_x()},
      {"plnt.x", quads_x({{-9, -5, -10, -5, "half"}})},
      {"mixm.x", quads_x({{-4, 0, -10, -5, "half"}, {1, 4, -10, -5, "x_glass"}, {5, 9, -10, -5, "tfx"}})},
      {"sky1.x", blue_x()},
      {"watr.x", blue_x()},
      {"eyes.x", quads_x({{-1, 1, 0, 2, "on_eye"}})},
      {"stat_obj.slb", obj_slb({{"quad", 0, 0, 0, 0x10},
                                {"blue", 0, 0, -2, 0x10},
                                {"blue", 0, 0, -50, 0x08},
                                {"quad", 0, 0, -60, 0x110},
                                {"blue", 0, 0, -70, 0x110},
                                {"blue", 0, 0, -80, 0x20},
                                {"plnt", 0, 0, 0, 0x10},
                                {"mixm", 0, 0, 0, 0x10},
                                {"watr", 0, 0, -90, 0x110},
                                {"blue", 0, 0, -40, 0x30},    // moveables (hidden behind the blue quad): drawn
                                {"quad", 0, 0, -20, 0x30},     // nearest first -> quad @-20 before blue @-40
                                {"sky1", 0, 0, -30, 0x110},     // last record: earlier records keep their indices
                                {"eyes", 0, 0, -10, 0x10}})},   // Task 8B D2: an eye texture (hidden behind 'blue' @-2)
      {"stat_pos.slb", pos_slb({{"strt", 0, 0, 10, 0x1000}})},
      {"stat_light.slb", light_slb({{"lmp1", 1, 2, 3, 1.0f, 20.0f, 1, 0.5f, 0.25f, 1, 0x40},
                                    {"sun1", 0.5f, -1, 0, 0.8f, 0, 0.9f, 0.9f, 0.8f, 1, 0x80}})},
  });
  return fs.mount("Root/Data/Levels/levt/levt.blk", std::move(level)) && fs.mount("Root/Data/Levels/levt/test.blk", std::move(area)) &&
         fs.mount("Root/Data/Levels/levt/stat.blk", std::move(state)) && fs.mount("Root/Data/Base.osi", base_osi());
}

CameraSettings synthetic_camera() {
  CameraSettings s;
  s.fov_deg = 90.0f;
  s.near_z = 0.1f;
  s.far_z = 100.0f;
  s.eye_height = 0.0f;
  s.pitch_deg = 0.0f;
  s.yaw_set = true;
  s.yaw_deg = 0.0f;
  return s;
}

}  // namespace lomn::view
