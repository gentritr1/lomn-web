#include "lomn/sage/xfile.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

#include "bytes.h"

namespace lomn::sage {
namespace {
using detail::Cursor;
using detail::fail;
using detail::le32;
using detail::lef32;
using detail::lef64;

enum : uint16_t { T_NAME = 1, T_STRING = 2, T_INTEGER = 3, T_GUID = 5, T_INTLIST = 6, T_FLTLIST = 7, T_OBRACE = 10, T_CBRACE = 11, T_TEMPLATE = 31 };

bool simple_token(uint16_t t) { return (t >= 12 && t <= 20) || (t >= 40 && t <= 53); }

struct Member {
  bool is_ref;
  uint32_t child;   // index into objects when !is_ref
  std::string ref;  // referenced name when is_ref
};

struct Obj {
  std::string type, name;
  std::vector<uint32_t> ints;  // INTEGER and INTEGER_LIST values in file order
  std::vector<float> floats;   // FLOAT_LIST values in file order
  std::vector<std::string> strings;
  std::vector<Member> members;  // nested objects and references in file order
};

class Parser {
 public:
  Parser(const uint8_t* d, size_t n, size_t fsize) : c_(d, n, 16), fsize_(fsize) {}
  std::vector<Obj> objs;   // arena; objs[i] for every data object, pre-order
  std::vector<uint32_t> top;
  std::string err;

  bool parse() {
    while (!c_.at_end()) {
      uint16_t t;
      if (!c_.u16(t)) return bad("truncated token");
      if (t == T_TEMPLATE) {
        if (!skip_template()) return false;
      } else if (t == T_NAME) {
        std::string type;
        if (!name(type)) return false;
        uint32_t idx;
        if (!object(std::move(type), 0, idx)) return false;
        top.push_back(idx);
      } else {
        return bad("token " + std::to_string(t) + " at top level");
      }
    }
    return true;
  }

 private:
  bool bad(std::string m) {
    err = "x: " + m + " at byte " + std::to_string(c_.pos());
    return false;
  }
  bool name(std::string& s) {
    uint32_t n;
    if (!c_.u32(n) || !c_.has(n)) return bad("truncated name");
    s.assign(reinterpret_cast<const char*>(c_.here()), n);
    c_.skip(n);
    return true;
  }
  bool skip_template() {
    uint16_t t;
    std::string s;
    if (!c_.u16(t) || t != T_NAME) return bad("template without a NAME token");
    if (!name(s)) return false;
    if (!c_.u16(t) || t != T_OBRACE) return bad("template without '{'");
    int depth = 1;
    while (depth > 0) {
      if (!c_.u16(t)) return bad("truncated template");
      uint32_t u;
      if (t == T_NAME) {
        if (!name(s)) return false;
      } else if (t == T_GUID) {
        if (!c_.skip(16)) return bad("truncated GUID");
      } else if (t == T_INTEGER) {
        if (!c_.u32(u)) return bad("truncated integer");
      } else if (t == T_OBRACE) {
        if (++depth > kXMaxDepth) return bad("template nesting too deep");
      } else if (t == T_CBRACE) {
        --depth;
      } else if (!simple_token(t)) {
        return bad("token " + std::to_string(t) + " in template");
      }
    }
    return true;
  }
  bool object(std::string type, int depth, uint32_t& idx) {
    if (depth > kXMaxDepth) return bad("nesting deeper than 64");
    idx = static_cast<uint32_t>(objs.size());
    if (objs.size() >= kXMaxObjects) return bad("too many objects");
    objs.emplace_back();
    objs[idx].type = std::move(type);
    uint16_t t;
    if (!c_.peek16(t)) return bad("truncated object");
    if (t == T_NAME) {
      c_.skip(2);
      std::string n;
      if (!name(n)) return false;
      objs[idx].name = std::move(n);
      if (!c_.peek16(t)) return bad("truncated object");
    }
    if (t == T_GUID) {
      c_.skip(2);
      if (!c_.skip(16)) return bad("truncated GUID");
    }
    if (!c_.u16(t) || t != T_OBRACE) return bad("expected '{'");
    for (;;) {
      if (!c_.u16(t)) return bad("truncated object body");
      if (t == T_CBRACE) return true;
      if (t == T_INTLIST) {
        uint32_t n;
        if (!c_.u32(n)) return bad("truncated list");
        if (n > c_.remaining() / 4) return bad("integer list longer than the file");
        auto& v = objs[idx].ints;  // no exact reserve: lists arrive one element at a time (IndexedColor)
        for (uint32_t i = 0; i < n; ++i) v.push_back(le32(c_.here() + 4 * static_cast<size_t>(i)));
        c_.skip(4 * static_cast<uint64_t>(n));
      } else if (t == T_FLTLIST) {
        uint32_t n;
        if (!c_.u32(n)) return bad("truncated list");
        if (n > c_.remaining() / fsize_) return bad("float list longer than the file");
        auto& v = objs[idx].floats;
        for (uint32_t i = 0; i < n; ++i) {
          const uint8_t* p = c_.here() + fsize_ * static_cast<size_t>(i);
          v.push_back(fsize_ == 4 ? lef32(p) : static_cast<float>(lef64(p)));
        }
        c_.skip(static_cast<uint64_t>(fsize_) * n);
      } else if (t == T_INTEGER) {
        uint32_t u;
        if (!c_.u32(u)) return bad("truncated integer");
        objs[idx].ints.push_back(u);
      } else if (t == T_STRING) {
        std::string s;
        uint16_t term;
        if (!name(s)) return false;
        if (!c_.u16(term)) return bad("truncated string terminator");
        objs[idx].strings.push_back(std::move(s));
      } else if (t == T_NAME) {
        std::string ctype;
        if (!name(ctype)) return false;
        uint32_t child;
        if (!object(std::move(ctype), depth + 1, child)) return false;
        objs[idx].members.push_back({false, child, {}});
      } else if (t == T_OBRACE) {
        std::string r;
        if (!c_.u16(t) || t != T_NAME) return bad("bad reference");
        if (!name(r)) return false;
        if (!c_.u16(t) || t != T_CBRACE) return bad("bad reference end");
        objs[idx].members.push_back({true, 0, std::move(r)});
      } else if (!simple_token(t)) {
        return bad("token " + std::to_string(t) + " in data");
      }
    }
  }
  Cursor c_;
  size_t fsize_;
};

const Obj* last_child(const std::vector<Obj>& objs, const Obj& o, const char* type) {
  const Obj* hit = nullptr;
  for (const Member& m : o.members)
    if (!m.is_ref && objs[m.child].type == type) hit = &objs[m.child];
  return hit;
}

bool decode_mesh(const std::vector<Obj>& objs, const Obj& m, XMesh& out, std::string* err) {
  out = XMesh();
  const auto& iv = m.ints;
  if (iv.size() < 2) return fail(err, "x: Mesh header truncated");
  uint32_t nv = iv[0];
  if (nv > m.floats.size() / 3) return fail(err, "x: Mesh vertex floats short");
  uint32_t nf = iv[1];
  // Polygons: (count, idx...) each; collect corners as (position, polygon) then triangulate as a fan.
  std::vector<uint32_t> poly_start, poly_size;
  if (nf > (iv.size() - 2) / 4) return fail(err, "x: Mesh face count larger than its data");
  poly_start.reserve(nf);
  size_t k = 2;
  for (uint32_t f = 0; f < nf; ++f) {
    if (k >= iv.size()) return fail(err, "x: Mesh faces truncated");
    uint32_t c = iv[k];
    if (c < 3 || c > 4) return fail(err, "x: face size " + std::to_string(c) + " (only 3..4 supported)");
    if (c > iv.size() - k - 1) return fail(err, "x: Mesh faces truncated");
    for (uint32_t j = 0; j < c; ++j)
      if (iv[k + 1 + j] >= nv) return fail(err, "x: face index out of range");
    poly_start.push_back(static_cast<uint32_t>(k + 1));
    poly_size.push_back(c);
    k += 1 + c;
  }
  out.file_vertices = nv;
  out.file_faces = nf;

  // Normals: per-face normal index lists parallel to the faces.
  const Obj* mn = last_child(objs, m, "MeshNormals");
  std::vector<uint32_t> corner_normal;  // per corner, in polygon order; empty when no normals
  uint32_t nn = 0;
  if (mn) {
    const auto& ni = mn->ints;
    if (ni.size() < 2) return fail(err, "x: MeshNormals header truncated");
    nn = ni[0];
    if (nn > mn->floats.size() / 3) return fail(err, "x: MeshNormals floats short");
    if (ni[1] != nf) return fail(err, "x: MeshNormals face count differs from Mesh");
    size_t q = 2;
    for (uint32_t f = 0; f < nf; ++f) {
      if (q >= ni.size() || ni[q] != poly_size[f] || ni[q] > ni.size() - q - 1) return fail(err, "x: MeshNormals face mismatch");
      for (uint32_t j = 0; j < poly_size[f]; ++j) {
        uint32_t n = ni[q + 1 + j];
        if (n >= nn) return fail(err, "x: normal index out of range");
        corner_normal.push_back(n);
      }
      q += 1 + poly_size[f];
    }
  }

  const Obj* mt = last_child(objs, m, "MeshTextureCoords");
  if (mt) {
    if (mt->ints.empty() || mt->ints[0] != nv) return fail(err, "x: MeshTextureCoords count differs from nVertices");
    if (nv > mt->floats.size() / 2) return fail(err, "x: MeshTextureCoords floats short");
  }

  const Obj* mc = last_child(objs, m, "MeshVertexColors");
  std::vector<float> src_colors;
  if (mc) {
    if (mc->ints.empty()) return fail(err, "x: MeshVertexColors header truncated");
    uint32_t nc = mc->ints[0];
    if (nc > mc->ints.size() - 1 || nc > mc->floats.size() / 4) return fail(err, "x: MeshVertexColors truncated");
    src_colors.assign(static_cast<size_t>(nv) * 4, 1.0f);  // vertices without an entry: opaque white
    for (uint32_t i = 0; i < nc; ++i) {
      uint32_t v = mc->ints[1 + i];
      if (v >= nv) return fail(err, "x: vertex colour index out of range");
      std::memcpy(&src_colors[static_cast<size_t>(v) * 4], &mc->floats[static_cast<size_t>(i) * 4], 4 * sizeof(float));
    }
    out.has_colors = true;
  }

  // Materials and the per-face attribute ids.
  std::vector<uint32_t> face_attrib(nf, 0);
  const Obj* ml = last_child(objs, m, "MeshMaterialList");
  if (ml) {
    const auto& ci = ml->ints;
    if (ci.size() < 2) return fail(err, "x: MeshMaterialList header truncated");
    uint32_t nmat = ci[0], nfi = ci[1];
    if (nfi > ci.size() - 2) return fail(err, "x: MeshMaterialList face indices truncated");
    if (nf > 0 && nfi == 0) return fail(err, "x: MeshMaterialList has no face indices");
    for (uint32_t f = 0; f < nf; ++f) {
      uint32_t a = ci[2 + std::min(f, nfi - 1)];  // fewer indices than faces: the last one repeats
      if (a >= nmat) return fail(err, "x: material index out of range");
      face_attrib[f] = a;
    }
    std::unordered_map<std::string, uint32_t> named;  // like the reference parser: the last object of a name wins
    for (uint32_t i = 0; i < objs.size(); ++i)
      if (!objs[i].name.empty()) named[objs[i].name] = i;
    std::unordered_map<uint32_t, XMaterial> decoded;  // each Material object is decoded at most once
    for (const Member& mem : ml->members) {
      uint32_t oi = 0;
      if (mem.is_ref) {
        auto it = named.find(mem.ref);
        if (it == named.end()) continue;
        oi = it->second;
      } else {
        oi = mem.child;
      }
      const Obj* mo = &objs[oi];
      if (mo->type != "Material") continue;
      if (auto hit = decoded.find(oi); hit != decoded.end()) {
        out.materials.push_back(hit->second);
        if (out.materials.size() > nmat) return fail(err, "x: material count differs from MeshMaterialList");
        continue;
      }
      if (mo->floats.size() < 11) return fail(err, "x: Material floats short");
      XMaterial mat;
      std::copy(mo->floats.begin(), mo->floats.begin() + 4, mat.diffuse);
      mat.power = mo->floats[4];
      std::copy(mo->floats.begin() + 5, mo->floats.begin() + 8, mat.specular);
      std::copy(mo->floats.begin() + 8, mo->floats.begin() + 11, mat.emissive);
      if (const Obj* tf = last_child(objs, *mo, "TextureFilename")) {
        if (!tf->strings.empty()) {
          mat.has_texture = true;
          mat.texture = tf->strings.front();
        }
      }
      out.materials.push_back(decoded.emplace(oi, std::move(mat)).first->second);
      if (out.materials.size() > nmat) return fail(err, "x: material count differs from MeshMaterialList");
    }
    if (out.materials.size() != nmat) return fail(err, "x: material count differs from MeshMaterialList");
  }

  // Vertex split by (position, normal index), triangulation, then the stable attribute sort.
  std::vector<uint32_t> assigned(nv, UINT32_MAX);
  std::unordered_map<uint64_t, uint32_t> extra;
  for (uint32_t v = 0; v < nv; ++v) out.source_vertex.push_back(v);
  std::vector<uint32_t> out_normal(nv, UINT32_MAX);
  std::vector<uint32_t> corner_vertex;
  corner_vertex.reserve(corner_normal.empty() ? 0 : corner_normal.size());
  size_t corner = 0;
  std::vector<uint32_t> tri_index;  // 3 per triangle, pre-sort
  std::vector<uint32_t> tri_attrib;
  for (uint32_t f = 0; f < nf; ++f) {
    uint32_t vtx[4];
    for (uint32_t j = 0; j < poly_size[f]; ++j, ++corner) {
      uint32_t p = iv[poly_start[f] + j];
      if (corner_normal.empty()) {
        vtx[j] = p;
        continue;
      }
      uint32_t n = corner_normal[corner];
      if (assigned[p] == UINT32_MAX || assigned[p] == n) {
        assigned[p] = n;
        out_normal[p] = n;
        vtx[j] = p;
      } else {
        uint64_t key = (static_cast<uint64_t>(p) << 32) | n;
        auto it = extra.find(key);
        if (it == extra.end()) {
          uint32_t id = static_cast<uint32_t>(out.source_vertex.size());
          out.source_vertex.push_back(p);
          out_normal.push_back(n);
          it = extra.emplace(key, id).first;
        }
        vtx[j] = it->second;
      }
    }
    for (uint32_t j = 1; j + 1 < poly_size[f]; ++j) {
      tri_index.insert(tri_index.end(), {vtx[0], vtx[j], vtx[j + 1]});
      tri_attrib.push_back(face_attrib[f]);
    }
  }
  size_t nout = out.source_vertex.size();
  out.positions.resize(nout * 3);
  out.normals.assign(nout * 3, 0.0f);
  if (mt) out.uvs.resize(nout * 2);
  if (out.has_colors) out.colors.resize(nout * 4);
  for (size_t v = 0; v < nout; ++v) {
    size_t s = out.source_vertex[v];
    std::memcpy(&out.positions[v * 3], &m.floats[s * 3], 3 * sizeof(float));
    if (mn && out_normal[v] != UINT32_MAX) std::memcpy(&out.normals[v * 3], &mn->floats[static_cast<size_t>(out_normal[v]) * 3], 3 * sizeof(float));
    if (mt) std::memcpy(&out.uvs[v * 2], &mt->floats[s * 2], 2 * sizeof(float));
    if (out.has_colors) std::memcpy(&out.colors[v * 4], &src_colors[s * 4], 4 * sizeof(float));
  }
  size_t ntri = tri_attrib.size();
  std::vector<uint32_t> order(ntri);
  for (size_t i = 0; i < ntri; ++i) order[i] = static_cast<uint32_t>(i);
  std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return tri_attrib[a] < tri_attrib[b]; });
  out.indices.resize(ntri * 3);
  out.triangle_attrib.resize(ntri);
  for (size_t i = 0; i < ntri; ++i) {
    std::memcpy(&out.indices[i * 3], &tri_index[static_cast<size_t>(order[i]) * 3], 3 * sizeof(uint32_t));
    out.triangle_attrib[i] = tri_attrib[order[i]];
  }
  for (size_t i = 0; i < ntri;) {
    XSubset s;
    s.attrib_id = out.triangle_attrib[i];
    s.face_start = static_cast<uint32_t>(i);
    uint32_t lo = UINT32_MAX, hi = 0;
    while (i < ntri && out.triangle_attrib[i] == s.attrib_id) {
      for (int j = 0; j < 3; ++j) {
        lo = std::min(lo, out.indices[i * 3 + j]);
        hi = std::max(hi, out.indices[i * 3 + j]);
      }
      ++i;
    }
    s.face_count = static_cast<uint32_t>(i) - s.face_start;
    s.vertex_start = lo;
    s.vertex_count = hi - lo + 1;
    out.subsets.push_back(s);
  }
  return true;
}

}  // namespace

bool parse_x(const uint8_t* data, size_t size, XFile& out, std::string* err) {
  out = XFile();
  if (size < 16 || std::memcmp(data, "xof ", 4) != 0) return fail(err, "x: not a .x file");
  if (std::memcmp(data + 8, "bin ", 4) != 0) return fail(err, "x: not a binary .x file");
  size_t fsize = std::memcmp(data + 12, "0032", 4) == 0 ? 4 : std::memcmp(data + 12, "0064", 4) == 0 ? 8 : 0;
  if (!fsize) return fail(err, "x: unsupported float size");
  Parser p(data, size, fsize);
  if (!p.parse()) return fail(err, p.err);
  const Obj* last = nullptr;
  for (const Obj& o : p.objs)  // arena order is pre-order, file order
    if (o.type == "Mesh") {
      ++out.mesh_count;
      last = &o;
    }
  if (!last) return true;
  return decode_mesh(p.objs, *last, out.mesh, err);
}

}  // namespace lomn::sage
