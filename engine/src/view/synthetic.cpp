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
  Bytes level = make_blk({{"red.dds", red_dds()}, {"cut.dds", cut_dds()}});
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
  return fs.mount("Root/Data/Levels/levt/levt.blk", std::move(level)) && fs.mount("Root/Data/Levels/levt/test.blk", std::move(area));
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
