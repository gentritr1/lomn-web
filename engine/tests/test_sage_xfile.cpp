#include <initializer_list>

#include "doctest.h"
#include "lomn/sage/xfile.h"
#include "sage_test_util.h"

using namespace lomn::sage;
using namespace sage_test;

namespace {

// Binary .x token writer.
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
    put16(b, 20);  // ';'
    return *this;
  }
  X& guid() {
    put16(b, 5);
    for (int i = 0; i < 16; ++i) put8(b, static_cast<uint8_t>(i));
    return *this;
  }
  X& ints(std::initializer_list<uint32_t> v) {
    put16(b, 6);
    put32(b, static_cast<uint32_t>(v.size()));
    for (uint32_t x : v) put32(b, x);
    return *this;
  }
  X& floats(std::initializer_list<float> v) {
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
  X& tok(uint16_t t) {
    put16(b, t);
    return *this;
  }
  X& obj(const std::string& type) { return name(type).open(); }
  X& ref(const std::string& n) { return open().name(n).close(); }
  // template Mesh { <guid> DWORD nVertices; array Vector vertices[nVertices]; [...] }
  X& templ() {
    tok(31).name("Mesh").open().guid();
    tok(41).name("nVertices").tok(20);
    tok(52).name("Vector").name("vertices").tok(14).name("nVertices").tok(15).tok(20);
    tok(14).tok(18).tok(18).tok(18).tok(15);
    return close();
  }
};

// Mesh: 5 vertices, faces {0,1,2} (mat 1), quad {0,2,3,4} (mat 0), {1,3,2} (mat 1). Normal indices make vertex 0
// appear with two different normals (split) and vertex 2 with one. Vertex colours, UVs, 2 materials.
void mesh(X& x, float tag = 0.f, bool with_colors = true) {
  x.name("Mesh").name("body").guid().open();  // named object with a GUID
  x.ints({5}).floats({tag, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 0.5f, 0.5f, 1});
  x.ints({3, 3, 0, 1, 2, 4, 0, 2, 3, 4, 3, 1, 3, 2});
  x.obj("MeshNormals").ints({2}).floats({0, 0, 1, 0, 1, 0}).ints({3, 3, 0, 0, 0, 4, 1, 0, 0, 0, 3, 0, 0, 0}).close();
  x.obj("MeshTextureCoords").ints({5}).floats({0, 0, 1, 0, 1, 1, 0, 1, 0.5f, 0.5f}).close();
  if (with_colors) {
    x.obj("MeshVertexColors").ints({5});
    for (uint32_t i = 0; i < 5; ++i) x.ints({i}).floats({0.1f * i, 0.2f, 0.3f, 1});
    x.close();
  }
  x.obj("MeshMaterialList").ints({2, 3, 1, 0, 1});
  x.obj("Material").floats({1, 0, 0, 1, 8, 0.5f, 0.5f, 0.5f, 0, 0, 0}).close();
  x.obj("Material").floats({0, 1, 0, 0.5f, 4, 0, 0, 0, 0.1f, 0.2f, 0.3f});
  x.obj("TextureFilename").str("on_lev_20xx_ocean").close();
  x.close();
  x.close();  // MeshMaterialList
  x.close();  // Mesh
}

Bytes sample(size_t* frame_at = nullptr) {
  X x;
  x.templ();
  if (frame_at) *frame_at = x.b.size();
  x.name("Frame").name("root").open();
  x.obj("FrameTransformMatrix").floats({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}).close();
  mesh(x);
  x.close();
  return x.b;
}

}  // namespace

TEST_CASE("x: templates skipped, mesh decoded with split vertices, fan triangulation and attribute sort") {
  Bytes b = sample();
  XFile xf;
  std::string err;
  REQUIRE_MESSAGE(parse_x(b.data(), b.size(), xf, &err), err);
  CHECK(xf.mesh_count == 1);
  const XMesh& m = xf.mesh;
  CHECK(m.file_vertices == 5);
  CHECK(m.file_faces == 3);
  // Vertex 0 is used with normal 0 (face 0) and normal 1 (quad) -> one extra vertex appended.
  REQUIRE(m.source_vertex.size() == 6);
  CHECK(m.source_vertex[5] == 0);
  CHECK(m.normals[5 * 3 + 1] == 1.0f);  // the appended copy carries normal 1 (0,1,0)
  CHECK(m.normals[0 * 3 + 2] == 1.0f);  // the original keeps normal 0 (0,0,1)
  CHECK(m.positions[5 * 3 + 0] == m.positions[0]);
  CHECK(m.uvs[5 * 2 + 0] == m.uvs[0]);
  REQUIRE(m.has_colors);
  CHECK(m.colors[4 * 4 + 0] == doctest::Approx(0.4f));
  // 1 + 2 + 1 triangles, sorted by attribute: the quad (mat 0) first, then faces 0 and 2 (mat 1) in file order.
  REQUIRE(m.indices.size() == 12);
  CHECK(m.triangle_attrib == std::vector<uint32_t>{0, 0, 1, 1});
  CHECK(std::vector<uint32_t>(m.indices.begin(), m.indices.begin() + 6) == std::vector<uint32_t>{5, 2, 3, 5, 3, 4});
  CHECK(std::vector<uint32_t>(m.indices.begin() + 6, m.indices.end()) == std::vector<uint32_t>{0, 1, 2, 1, 3, 2});
  REQUIRE(m.subsets.size() == 2);
  CHECK(m.subsets[0].attrib_id == 0);
  CHECK(m.subsets[0].face_start == 0);
  CHECK(m.subsets[0].face_count == 2);
  CHECK(m.subsets[0].vertex_start == 2);
  CHECK(m.subsets[0].vertex_count == 4);
  CHECK(m.subsets[1].attrib_id == 1);
  CHECK(m.subsets[1].face_start == 2);
  CHECK(m.subsets[1].vertex_start == 0);
  REQUIRE(m.materials.size() == 2);
  CHECK_FALSE(m.materials[0].has_texture);
  CHECK(m.materials[0].power == 8);
  CHECK(m.materials[1].has_texture);
  CHECK(m.materials[1].texture == "on_lev_20xx_ocean");
  CHECK(m.materials[1].diffuse[3] == 0.5f);
  CHECK(m.materials[1].emissive[2] == doctest::Approx(0.3f));
}

TEST_CASE("x: the last mesh wins; files without vertex colours; material references") {
  X x;
  x.templ();
  mesh(x, 7.f);
  x.name("Material").name("shared").open().floats({0, 0, 1, 1, 2, 0, 0, 0, 0, 0, 0}).obj("TextureFilename").str("ref_tex").close().close();
  x.obj("Mesh").ints({3}).floats({9, 0, 0, 1, 0, 0, 0, 1, 0}).ints({1, 3, 0, 1, 2});
  x.obj("MeshMaterialList").ints({1, 1, 0}).ref("shared").close();
  x.close();
  XFile xf;
  std::string err;
  REQUIRE_MESSAGE(parse_x(x.b.data(), x.b.size(), xf, &err), err);
  CHECK(xf.mesh_count == 2);
  CHECK(xf.mesh.file_vertices == 3);
  CHECK(xf.mesh.positions[0] == 9.f);
  CHECK_FALSE(xf.mesh.has_colors);
  CHECK(xf.mesh.colors.empty());
  CHECK(xf.mesh.normals == std::vector<float>(9, 0.f));  // no MeshNormals
  REQUIRE(xf.mesh.materials.size() == 1);
  CHECK(xf.mesh.materials[0].texture == "ref_tex");
  CHECK(xf.mesh.subsets.size() == 1);
}

TEST_CASE("x: truncations never crash, and every cut inside the data object is rejected") {
  size_t frame_at = 0;
  Bytes b = sample(&frame_at);
  XFile xf;
  std::string err;
  for (size_t n = 0; n < 16; ++n) CHECK_FALSE(parse_x(b.data(), n, xf, &err));
  for (size_t n = 16; n < frame_at; ++n) (void)parse_x(b.data(), n, xf, &err);  // inside the template: must not crash
  for (size_t n = frame_at + 1; n < b.size(); ++n) CHECK_FALSE(parse_x(b.data(), n, xf, &err));
}

TEST_CASE("x: bad headers and tokens") {
  XFile xf;
  std::string err;
  Bytes txt(16, ' ');
  std::copy_n("xof 0303txt 0032", 16, txt.begin());
  CHECK_FALSE(parse_x(txt.data(), txt.size(), xf, &err));
  Bytes f80(16, ' ');
  std::copy_n("xof 0303bin 0080", 16, f80.begin());
  CHECK_FALSE(parse_x(f80.data(), f80.size(), xf, &err));
  X t;
  t.tok(3);  // INTEGER at top level
  CHECK_FALSE(parse_x(t.b.data(), t.b.size(), xf, &err));
  X u;
  u.obj("Frame").tok(99).close();  // unknown token in data
  CHECK_FALSE(parse_x(u.b.data(), u.b.size(), xf, &err));
  X e;  // header only: valid, no mesh
  REQUIRE(parse_x(e.b.data(), e.b.size(), xf, &err));
  CHECK(xf.mesh_count == 0);
}

TEST_CASE("x: list counts are bounded by the remaining bytes") {
  XFile xf;
  std::string err;
  for (uint16_t tok : {6, 7}) {
    X x;
    x.obj("Mesh");
    put16(x.b, tok);
    put32(x.b, 0x40000001u);  // * 4 wraps a 32-bit size_t
    put32(x.b, 0);
    CHECK_FALSE(parse_x(x.b.data(), x.b.size(), xf, &err));
    CHECK(err.find("longer than the file") != std::string::npos);
  }
  X n;
  put16(n.b, 1);
  put32(n.b, 0xffffffffu);  // name length
  CHECK_FALSE(parse_x(n.b.data(), n.b.size(), xf, &err));
}

TEST_CASE("x: nesting depth is capped at 64") {
  XFile xf;
  std::string err;
  for (int depth : {64, 65}) {
    X x;
    for (int i = 0; i <= depth; ++i) x.obj("Frame");  // top level is depth 0
    for (int i = 0; i <= depth; ++i) x.close();
    bool ok = parse_x(x.b.data(), x.b.size(), xf, &err);
    CHECK(ok == (depth == 64));
  }
  X deep;
  for (int i = 0; i < 100000; ++i) deep.obj("F");  // must fail at 65, not overflow the stack
  CHECK_FALSE(parse_x(deep.b.data(), deep.b.size(), xf, &err));
}

TEST_CASE("x: mesh index validation") {
  XFile xf;
  std::string err;
  auto try_mesh = [&](std::initializer_list<uint32_t> faces, std::initializer_list<uint32_t> mml) {
    X x;
    x.obj("Mesh").ints({4}).floats({0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0}).ints(faces);
    x.obj("MeshMaterialList").ints(mml).obj("Material").floats({1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0}).close().close();
    x.close();
    return parse_x(x.b.data(), x.b.size(), xf, &err);
  };
  CHECK(try_mesh({1, 3, 0, 1, 2}, {1, 1, 0}));
  CHECK_FALSE(try_mesh({1, 3, 0, 1, 4}, {1, 1, 0}));        // vertex index == nVertices
  CHECK_FALSE(try_mesh({1, 5, 0, 1, 2, 3, 0}, {1, 1, 0}));  // pentagon
  CHECK_FALSE(try_mesh({1, 2, 0, 1}, {1, 1, 0}));           // degenerate 2-gon
  CHECK_FALSE(try_mesh({1, 3, 0, 1, 2}, {1, 1, 1}));        // material index == nMaterials
  CHECK_FALSE(try_mesh({1, 3, 0, 1, 2}, {2, 1, 0}));        // declares 2 materials, has 1
  CHECK_FALSE(try_mesh({0x7fffffff, 3, 0, 1, 2}, {1, 1, 0}));  // face count beyond the data
  CHECK(try_mesh({2, 3, 0, 1, 2, 3, 0, 2, 3}, {1, 1, 0}));    // fewer face indices than faces: last repeats
  CHECK(xf.mesh.triangle_attrib == std::vector<uint32_t>{0, 0});
  X bad_normals;
  bad_normals.obj("Mesh").ints({3}).floats({0, 0, 0, 1, 0, 0, 1, 1, 0}).ints({1, 3, 0, 1, 2});
  bad_normals.obj("MeshNormals").ints({1}).floats({0, 0, 1}).ints({1, 3, 0, 0, 1}).close().close();  // normal 1 of 1
  CHECK_FALSE(parse_x(bad_normals.b.data(), bad_normals.b.size(), xf, &err));
  X bad_colors;
  bad_colors.obj("Mesh").ints({3}).floats({0, 0, 0, 1, 0, 0, 1, 1, 0}).ints({1, 3, 0, 1, 2});
  bad_colors.obj("MeshVertexColors").ints({1, 3}).floats({1, 1, 1, 1}).close().close();  // colour for vertex 3 of 3
  CHECK_FALSE(parse_x(bad_colors.b.data(), bad_colors.b.size(), xf, &err));
}
