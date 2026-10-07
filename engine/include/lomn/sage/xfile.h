// Binary DirectX .x ("xof 0303bin 0032"/"0064") -> static mesh, replacing what the game gets from
// ScDrawableModel::LoadX (0x00553800) + LoadFrames (0x00554680): DirectXFileCreate/RegisterTemplates,
// D3DXLoadSkinMeshFromXof, ConvertToBlendedMesh and GetAttributeTable. Our own parser (spec §5): no D3DX.
//
// Token stream (u16 ids): NAME=1 (u32 n, chars), STRING=2 (u32 n, chars, u16 terminator), INTEGER=3 (u32),
// GUID=5 (16 bytes), INTEGER_LIST=6 (u32 n, n*u32), FLOAT_LIST=7 (u32 n, n*f32|f64), '{'=10, '}'=11,
// TEMPLATE=31; 12..20 and 40..53 are punctuation / primitive-type tokens without payload.
// TEMPLATE -> NAME token -> '{' ... '}' is skipped. A data object is NAME(type) [NAME(name)] [GUID] '{' members
// '}'; members are lists, strings, nested objects and references '{' NAME '}'.
// Bounds (spec §5): nesting depth <= 64, list counts bounded by the remaining bytes, face sizes 3..4, vertex /
// normal / colour indices in range, material index < nMaterials.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace lomn::sage {

constexpr int kXMaxDepth = 64;
// Cap on data objects per file (the parser keeps an arena entry per object). Measured over all 2,653 .x entries of
// the Beta install: at most 5,489 objects (vllg.blk main1.x). 16 x 5,489 = 87,824, rounded up to a power of two.
constexpr uint32_t kXMaxObjects = 131072;

struct XMaterial {
  float diffuse[4] = {};   // faceColor RGBA
  float power = 0;
  float specular[3] = {};
  float emissive[3] = {};
  bool has_texture = false;
  std::string texture;     // TextureFilename as stored (the game appends nothing; extension resolved later)
};

// D3DXATTRIBUTERANGE: triangles [face_start, face_start+face_count) use materials[attrib_id].
struct XSubset {
  uint32_t attrib_id = 0, face_start = 0, face_count = 0, vertex_start = 0, vertex_count = 0;
};

struct XMesh {
  // Vertex buffer after splitting every position that is used with more than one normal index (D3DX
  // duplicates such vertices; the first use keeps the original index, later (position, normal) pairs are
  // appended in face order). Per vertex: position, normal, optional diffuse colour, one UV set.
  std::vector<float> positions;  // 3 per vertex
  std::vector<float> normals;    // 3 per vertex
  std::vector<float> colors;     // 4 per vertex (RGBA, as stored) when has_colors, else empty
  std::vector<float> uvs;        // 2 per vertex
  std::vector<uint32_t> source_vertex;  // output vertex -> vertex index in the file
  std::vector<uint32_t> indices;        // 3 per triangle, triangles stably sorted by attribute id
  std::vector<uint32_t> triangle_attrib;
  std::vector<XSubset> subsets;         // attribute table, ascending attrib_id, one range per used material
  std::vector<XMaterial> materials;     // all materials of the MeshMaterialList, in file order
  bool has_colors = false;              // MeshVertexColors present
  uint32_t file_vertices = 0;           // nVertices as stored
  uint32_t file_faces = 0;              // nFaces as stored (polygons)
};

struct XFile {
  uint32_t mesh_count = 0;  // Mesh data objects anywhere in the file (pre-order)
  XMesh mesh;               // the LAST mesh: LoadFrames overwrites mpSkinMesh for each Mesh it meets
};

bool parse_x(const uint8_t* data, size_t size, XFile& out, std::string* err);

}  // namespace lomn::sage
