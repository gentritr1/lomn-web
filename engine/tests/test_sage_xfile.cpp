#include <chrono>
#include <initializer_list>

#include "doctest.h"
#include "lomn/sage/blockfile.h"
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

namespace {

// Material "M" with n empty child objects, and a MeshMaterialList that references {M} `refs` times (all faces use
// material 0). Decoding M once per reference is refs x n work: 40000 x 40000 took 4.8 s before the memo.
Bytes many_refs(uint32_t n, uint32_t nmat, uint32_t refs) {
  X x;
  x.obj("Mesh").ints({3}).floats({0, 0, 0, 1, 0, 0, 1, 1, 0}).ints({1, 3, 0, 1, 2});
  x.name("Material").name("M").open().floats({1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0});
  for (uint32_t i = 0; i < n; ++i) x.obj("A").close();
  x.close();
  x.obj("MeshMaterialList").ints({nmat, 1, 0});
  for (uint32_t i = 0; i < refs; ++i) x.ref("M");
  x.close();
  x.close();
  return x.b;
}

double seconds_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// Wall-clock bound for the DoS-regression cases below. Release takes ~0.005 s; ASan+UBSan Debug at loadavg 3.7
// takes ~0.6 s, so a 1 s bound flaked. 10 s tolerates any sanitizer / host load and still catches hangs and
// worse-than-quadratic growth. (The original 4.8 s native quadratic decode sits under it: wall time alone is a
// coarse backstop, not the oracle for that bug.)
constexpr double kSlowBound = 10.0;

}  // namespace

TEST_CASE("x: many material references stay linear (memoised Material decode)") {
  XFile xf;
  std::string err;
  const uint32_t n = 40000;
  {
    Bytes b = many_refs(n, n, n);
    auto t0 = std::chrono::steady_clock::now();
    bool ok = parse_x(b.data(), b.size(), xf, &err);
    double dt = seconds_since(t0);
    REQUIRE(ok);
    CHECK(xf.mesh.materials.size() == n);
    CHECK(dt < kSlowBound);  // old code: ~4.8 s native (Release now ~0.005 s)
  }
  {
    // nMaterials = 1: the second reference fails. Sized so that decoding M once per reference (the pre-memo code:
    // refs x n = 400,000 x 100,000 = 25x the case above, ~120 s native) blows far past kSlowBound, while the current
    // code takes ~ms. n stays under kXMaxObjects; refs are members, not objects (file ~6 MB, under kMaxEntrySize).
    // This guards the MEMO: removing only the early bail keeps it fast (400k cheap copies, measured ~0.05 s).
    Bytes b = many_refs(100000, 1, 400000);
    auto t0 = std::chrono::steady_clock::now();
    bool ok = parse_x(b.data(), b.size(), xf, &err);
    double dt = seconds_since(t0);
    CHECK_FALSE(ok);
    CHECK(err.find("material count") != std::string::npos);
    CHECK(dt < kSlowBound);
  }
}

TEST_CASE("x: the object arena is capped at kXMaxObjects") {
  XFile xf;
  std::string err;
  for (uint32_t count : {kXMaxObjects, kXMaxObjects + 1}) {
    X x;
    for (uint32_t i = 0; i < count; ++i) x.obj("A").close();
    bool ok = parse_x(x.b.data(), x.b.size(), xf, &err);
    CHECK(ok == (count == kXMaxObjects));
    if (!ok) CHECK(err.find("too many objects") != std::string::npos);
  }
}

namespace {

// Material "M" with one TextureFilename of `len` bytes, and a MeshMaterialList of `refs` references to it.
Bytes tex_refs(uint32_t refs, size_t len, uint32_t nmat) {
  X x;
  x.obj("Mesh").ints({3}).floats({0, 0, 0, 1, 0, 0, 1, 1, 0}).ints({1, 3, 0, 1, 2});
  x.name("Material").name("M").open().floats({1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0});
  x.obj("TextureFilename").str(std::string(len, 't')).close();
  x.close();
  x.obj("MeshMaterialList").ints({nmat, 1, 0});
  for (uint32_t i = 0; i < refs; ++i) x.ref("M");
  x.close();
  x.close();
  return x.b;
}

}  // namespace

TEST_CASE("x: texture name x reference amplification is capped (both caps)") {
  XFile xf;
  std::string err;
  {  // 35000 refs of a 70 KB name: 2.4 GB of copies before the caps (wasm32 aborted at a 460 KB file)
    Bytes b = tex_refs(35000, 70000, 35000);
    auto t0 = std::chrono::steady_clock::now();
    CHECK_FALSE(parse_x(b.data(), b.size(), xf, &err));
    CHECK(err.find("texture name too long") != std::string::npos);
    CHECK(seconds_since(t0) < kSlowBound);
  }
  {  // texture name boundary: exactly the cap passes, +1 fails
    Bytes ok = tex_refs(1, kXMaxTextureName, 1);
    REQUIRE(parse_x(ok.data(), ok.size(), xf, &err));
    CHECK(xf.mesh.materials[0].texture.size() == kXMaxTextureName);
    Bytes bad = tex_refs(1, kXMaxTextureName + 1, 1);
    CHECK_FALSE(parse_x(bad.data(), bad.size(), xf, &err));
    CHECK(err.find("texture name too long") != std::string::npos);
  }
  {  // material count boundary: nMaterials == cap (with that many references) passes, +1 fails
    Bytes ok = many_refs(kXMaxMaterials, kXMaxMaterials, kXMaxMaterials);
    REQUIRE(parse_x(ok.data(), ok.size(), xf, &err));
    CHECK(xf.mesh.materials.size() == kXMaxMaterials);
    Bytes bad = many_refs(1, kXMaxMaterials + 1, 1);
    CHECK_FALSE(parse_x(bad.data(), bad.size(), xf, &err));
    CHECK(err.find("too many materials") != std::string::npos);
  }
}

TEST_CASE("x: a Mesh without MeshMaterialList has no materials and one subset with attrib_id 0") {
  X x;
  x.obj("Mesh").ints({3}).floats({0, 0, 0, 1, 0, 0, 0, 1, 0}).ints({1, 3, 0, 1, 2}).close();
  XFile xf;
  std::string err;
  REQUIRE_MESSAGE(parse_x(x.b.data(), x.b.size(), xf, &err), err);
  CHECK(xf.mesh.materials.empty());
  REQUIRE(xf.mesh.subsets.size() == 1);
  CHECK(xf.mesh.subsets[0].attrib_id == 0);
  CHECK(xf.mesh.subsets[0].face_count == 1);
}

namespace {

bool is_default(const XFile& f) {
  const XMesh& m = f.mesh;
  return f.mesh_count == 0 && m.positions.empty() && m.normals.empty() && m.colors.empty() && m.uvs.empty() && m.source_vertex.empty() &&
         m.indices.empty() && m.triangle_attrib.empty() && m.subsets.empty() && m.materials.empty() && !m.has_colors && m.file_vertices == 0 &&
         m.file_faces == 0;
}

}  // namespace

TEST_CASE("x: any parse failure leaves a default-constructed XFile (no partial mesh)") {
  XFile xf;
  std::string err;
  Bytes good = sample();
  REQUIRE_MESSAGE(parse_x(good.data(), good.size(), xf, &err), err);
  REQUIRE(xf.mesh_count == 1);
  REQUIRE_FALSE(is_default(xf));
  // A truncated file, parsed into the same (dirty) output.
  CHECK_FALSE(parse_x(good.data(), good.size() - 1, xf, &err));
  CHECK(is_default(xf));
  // A failure inside decode_mesh, after vertices, colours and one material were already filled in.
  REQUIRE(parse_x(good.data(), good.size(), xf, &err));
  X bad;
  bad.obj("Mesh").ints({3}).floats({0, 0, 0, 1, 0, 0, 0, 1, 0}).ints({1, 3, 0, 1, 2});
  bad.obj("MeshMaterialList").ints({2, 1, 0});  // announces 2 materials, supplies 1
  bad.obj("Material").floats({1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0}).close();
  bad.close();
  bad.close();
  CHECK_FALSE(parse_x(bad.b.data(), bad.b.size(), xf, &err));
  CHECK(err.find("material count") != std::string::npos);
  CHECK(is_default(xf));
  // A bad header too.
  REQUIRE(parse_x(good.data(), good.size(), xf, &err));
  Bytes junk(32, 'z');
  CHECK_FALSE(parse_x(junk.data(), junk.size(), xf, &err));
  CHECK(is_default(xf));
}

#ifndef __EMSCRIPTEN__
// Native only: kMaxEntrySize + 1 bytes (64 MiB) is not worth allocating under wasm32; the check itself is a plain
// size compare that both targets share.
TEST_CASE("x: input larger than kMaxEntrySize is rejected before any parsing") {
  XFile xf;
  std::string err;
  Bytes big(static_cast<size_t>(kMaxEntrySize) + 1, 0);
  CHECK_FALSE(parse_x(big.data(), big.size(), xf, &err));
  CHECK(err == "x: file too large");
  Bytes edge(static_cast<size_t>(kMaxEntrySize), 0);  // exactly the cap is accepted into the parser (and fails on the header)
  CHECK_FALSE(parse_x(edge.data(), edge.size(), xf, &err));
  CHECK(err == "x: not a .x file");
}
#endif

// M1a-4 fix round 1 (review I-2): count/length checks in decode_mesh that the fuzzer cannot reach (each needs two
// coordinated field changes: a count raised past its list AND an index that uses the extra range). Without the check
// each of these files decodes by reading past the float list (ASan: heap-buffer-overflow / BUS in memmove).
TEST_CASE("x: MeshNormals with fewer normal floats than nNormals is rejected (xfile.cpp 'MeshNormals floats short')") {
  X x;  // nNormals 5, but 2 normals of floats; face normal index 4 lies in the missing range
  x.obj("Mesh").ints({3}).floats({0, 0, 0, 1, 0, 0, 0, 1, 0}).ints({1, 3, 0, 1, 2});
  x.obj("MeshNormals").ints({5}).floats({0, 0, 1, 0, 1, 0}).ints({1, 3, 0, 1, 4}).close();
  x.close();
  XFile xf;
  std::string err;
  CHECK_FALSE(parse_x(x.b.data(), x.b.size(), xf, &err));
  CHECK(err.find("MeshNormals floats short") != std::string::npos);
  CHECK(xf.mesh.positions.empty());
}

TEST_CASE("x: MeshTextureCoords with fewer UV floats than nVertices is rejected ('MeshTextureCoords floats short')") {
  X x;  // 3 vertices, 2 UVs of floats
  x.obj("Mesh").ints({3}).floats({0, 0, 0, 1, 0, 0, 0, 1, 0}).ints({1, 3, 0, 1, 2});
  x.obj("MeshTextureCoords").ints({3}).floats({0, 0, 1, 0}).close();
  x.close();
  XFile xf;
  std::string err;
  CHECK_FALSE(parse_x(x.b.data(), x.b.size(), xf, &err));
  CHECK(err.find("MeshTextureCoords floats short") != std::string::npos);
  CHECK(xf.mesh.uvs.empty());
}

TEST_CASE("x: MeshVertexColors with fewer entries than nColors is rejected ('MeshVertexColors truncated')") {
  {
    X x;  // nColors 3 with 3 vertex indices but one colour of floats
    x.obj("Mesh").ints({3}).floats({0, 0, 0, 1, 0, 0, 0, 1, 0}).ints({1, 3, 0, 1, 2});
    x.obj("MeshVertexColors").ints({3}).ints({0}).floats({0.1f, 0.2f, 0.3f, 1}).ints({1}).ints({2}).close();
    x.close();
    XFile xf;
    std::string err;
    CHECK_FALSE(parse_x(x.b.data(), x.b.size(), xf, &err));
    CHECK(err.find("MeshVertexColors truncated") != std::string::npos);
    CHECK_FALSE(xf.mesh.has_colors);
  }
  {
    X x;  // nColors 3 with one vertex index (and one colour)
    x.obj("Mesh").ints({3}).floats({0, 0, 0, 1, 0, 0, 0, 1, 0}).ints({1, 3, 0, 1, 2});
    x.obj("MeshVertexColors").ints({3}).ints({0}).floats({0.1f, 0.2f, 0.3f, 1}).close();
    x.close();
    XFile xf;
    std::string err;
    CHECK_FALSE(parse_x(x.b.data(), x.b.size(), xf, &err));
    CHECK(err.find("MeshVertexColors truncated") != std::string::npos);
  }
}

// M1a-4 cleanup C1-a: the remaining decode_mesh count/length checks that deleting left the whole suite green. One
// crafted input per check (per sub-condition where a line holds several), each reaching exactly that check, asserting
// the exact message. Rows are doctest SUBCASEs so a mutation run can name one row: `lomn_tests -tc="x: crafted*"
// -sc="<id>"`. The mutation table (check deleted in a scratch copy -> which row fails) is in the M1a-4 cleanup report.
//   - "ASan" in a row's note = deleting the check reads past the end of the std::vector; the row's list size is a power
//     of two (vector capacity == size), so ASan reports a heap-buffer-overflow. Plain builds see garbage instead.
//   - None of the seven lines is redundant: every row below is an input that passes every EARLIER check and is only
//     stopped by this one (see the per-row reach notes).
namespace {

struct XRow {
  const char* id;
  Bytes bytes;
  const char* msg;  // the exact error string
};

// A Mesh object whose body is written by `body`.
template <class F>
Bytes mesh_obj(F&& body) {
  X x;
  x.obj("Mesh");
  body(x);
  x.close();
  return x.b;
}

// 3 vertices and one triangle: passes every Mesh-level check; children are added by the row.
void tri_mesh(X& x) { x.ints({3}).floats({0, 0, 0, 1, 0, 0, 1, 1, 0}).ints({1, 3, 0, 1, 2}); }
// 4 vertices only (nVertices + positions); the row appends the face list.
void quad_verts(X& x) { x.ints({4}).floats({0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0}); }

std::vector<XRow> count_length_rows() {
  std::vector<XRow> r;
  // L181 "Mesh header truncated": fewer than 2 ints (nVertices, nFaces). ASan / null deref without it (iv[1] / iv[0]).
  r.push_back({"L181-mesh-1-int", mesh_obj([](X& x) { x.ints({3}); }), "x: Mesh header truncated"});
  r.push_back({"L181-mesh-0-ints", mesh_obj([](X&) {}), "x: Mesh header truncated"});
  // L191 "Mesh faces truncated" (k >= size): four quads consume exactly the list (22 ints) while nFaces = 5 still passes
  // the nf <= (size-2)/4 pre-check (5 <= 5). Fifth face: k == size. Without it: iv[22] is read past the end.
  r.push_back({"L191-faces-exhausted",
               mesh_obj([](X& x) { quad_verts(x); x.ints({5, 4, 0, 1, 2, 3, 4, 0, 1, 2, 3, 4, 0, 1, 2, 3, 4, 0, 1, 2, 3}); }),
               "x: Mesh faces truncated"});
  // L194 "Mesh faces truncated" (c > size-k-1): one quad announced, its 4 indices cut to 3 (6 ints; nf=1 <= (6-2)/4).
  r.push_back({"L194-face-longer-than-list", mesh_obj([](X& x) { quad_verts(x); x.ints({1, 4, 0, 1, 2}); }), "x: Mesh faces truncated"});
  // L210 "MeshNormals header truncated": < 2 ints. ASan / null deref without it (ni[1] / ni[0]).
  r.push_back({"L210-normals-1-int", mesh_obj([](X& x) { tri_mesh(x); x.obj("MeshNormals").ints({1}).floats({0, 0, 1}).close(); }),
               "x: MeshNormals header truncated"});
  r.push_back({"L210-normals-0-ints", mesh_obj([](X& x) { tri_mesh(x); x.obj("MeshNormals").floats({0, 0, 1}).close(); }),
               "x: MeshNormals header truncated"});
  // L213 "MeshNormals face count differs": the normals list has 2 faces (both well-formed) for a 1-face mesh. Without
  // the check the per-face loop only walks nf=1 faces and the file is accepted.
  r.push_back({"L213-normals-more-faces",
               mesh_obj([](X& x) { tri_mesh(x); x.obj("MeshNormals").ints({1, 2, 3, 0, 0, 0, 3, 0, 0, 0}).floats({0, 0, 1}).close(); }),
               "x: MeshNormals face count differs from Mesh"});
  // L216 "MeshNormals face mismatch", three sub-conditions, each alone:
  //  (a) q >= ni.size(): normals list has 3 of 4 faces' worth of data (16 ints, nFaces field says 4): the 4th face starts
  //      at q == 16. ASan without it (ni[16] read past the end).
  r.push_back({"L216a-normals-exhausted",
               mesh_obj([](X& x) {
                 quad_verts(x);
                 x.ints({4, 4, 0, 1, 2, 3, 4, 0, 1, 2, 3, 3, 0, 1, 2, 3, 0, 1, 2});
                 x.obj("MeshNormals").ints({1, 4, 4, 0, 0, 0, 0, 4, 0, 0, 0, 0, 3, 0, 0, 0}).floats({0, 0, 1}).close();
               }),
               "x: MeshNormals face mismatch"});
  //  (b) ni[q] != poly_size[f]: a 4-corner normal list for a triangle, with enough ints that (c) cannot fire. Without
  //      the sub-condition the file is accepted.
  r.push_back({"L216b-normals-corner-count",
               mesh_obj([](X& x) { tri_mesh(x); x.obj("MeshNormals").ints({1, 1, 4, 0, 0, 0, 0}).floats({0, 0, 1}).close(); }),
               "x: MeshNormals face mismatch"});
  //  (c) ni[q] > ni.size()-q-1: the corner count matches (3) but only 2 indices remain. Without it ni[5] is read past
  //      the end (garbage index in a plain build, an ASan report in the sanitizer build).
  r.push_back({"L216c-normals-list-cut",
               mesh_obj([](X& x) { tri_mesh(x); x.obj("MeshNormals").ints({1, 1, 3, 0, 0}).floats({0, 0, 1}).close(); }),
               "x: MeshNormals face mismatch"});
  // L228 "MeshTextureCoords count differs from nVertices", both sub-conditions:
  //  (a) mt->ints.empty(): no count at all. Without it mt->ints[0] dereferences a null vector.
  r.push_back({"L228a-uv-no-count", mesh_obj([](X& x) { tri_mesh(x); x.obj("MeshTextureCoords").floats({0, 0, 1, 0, 1, 1}).close(); }),
               "x: MeshTextureCoords count differs from nVertices"});
  //  (b) ints[0] != nv: 5 UVs of floats for a 3-vertex mesh (the later 'floats short' check only looks at nv, so the file
  //      would be accepted).
  r.push_back({"L228b-uv-count-differs",
               mesh_obj([](X& x) { tri_mesh(x); x.obj("MeshTextureCoords").ints({5}).floats({0, 0, 1, 0, 1, 1, 0, 1, 0.5f, 0.5f}).close(); }),
               "x: MeshTextureCoords count differs from nVertices"});
  return r;
}

}  // namespace

TEST_CASE("x: crafted count/length rejections, one input per decode_mesh check (cleanup C1-a)") {
  for (const XRow& row : count_length_rows()) {
    SUBCASE(row.id) {
      XFile xf;
      std::string err;
      CHECK_FALSE(parse_x(row.bytes.data(), row.bytes.size(), xf, &err));
      CHECK(err == row.msg);
      CHECK(is_default(xf));  // nothing partial leaks out
    }
  }
}
