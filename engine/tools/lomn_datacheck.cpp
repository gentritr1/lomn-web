// lomn_datacheck <install_dir> [--areas lev/area,...] [--dump <blk vfs path> <entry> <out.rgba>]...
// Walks every .blk of a LOMN install through the VFS (case-insensitive "Root/..." paths) and prints one JSON
// document: BLK entry tables, OBJ routing per area, per-.x mesh counts, texture resolution for sample areas
// (SrBlockFileLoad and SrBlockFileExists orders), DDS/TGA/SLB decode stats and per-parser timings.
// --dump decodes one DDS/TGA entry to <out>: u32 width, u32 height, then width*height*4 RGBA bytes (no walk).
// Exit codes: 0 ok, 2 bad args, 3 install unreadable, 4 dump failed.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "lomn/fs.h"
#include "lomn/sage/blockchain.h"
#include "lomn/sage/blockfile.h"
#include "lomn/sage/image.h"
#include "lomn/sage/slb.h"
#include "lomn/sage/xfile.h"

using namespace lomn;
using namespace lomn::sage;
using Clock = std::chrono::steady_clock;

namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  return s;
}
bool ends_with(const std::string& s, const std::string& suf) { return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0; }

// JSON string literal; bytes >= 0x80 are latin1 code points (the names are latin1 in the files).
std::string js(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      o += '\\';
      o += static_cast<char>(c);
    } else if (c < 0x20 || c >= 0x7f) {
      char b[8];
      std::snprintf(b, sizeof b, "\\u%04x", c);
      o += b;
    } else {
      o += static_cast<char>(c);
    }
  }
  return o + "\"";
}
std::string hex(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof b, "0x%x", v);
  return b;
}

struct Meter {
  double ms = 0;
  uint64_t bytes = 0, items = 0;
  void add(Clock::time_point t0, uint64_t b) {
    ms += std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    bytes += b;
    ++items;
  }
  std::string json() const {
    char b[160];
    double mbs = ms > 0 ? (bytes / 1048576.0) / (ms / 1000.0) : 0;
    std::snprintf(b, sizeof b, "{\"items\":%llu,\"bytes\":%llu,\"ms\":%.3f,\"MiB_per_s\":%.1f}", static_cast<unsigned long long>(items),
                  static_cast<unsigned long long>(bytes), ms, mbs);
    return b;
  }
};

struct Failure {
  std::string blk, entry, error;
  uint64_t size;
};
std::string failures_json(const std::vector<Failure>& f) {
  std::string o = "[";
  for (size_t i = 0; i < f.size(); ++i)
    o += std::string(i ? "," : "") + "{\"blk\":" + js(f[i].blk) + ",\"entry\":" + js(f[i].entry) + ",\"size\":" + std::to_string(f[i].size) +
         ",\"error\":" + js(f[i].error) + "}";
  return o + "]";
}
std::string counts_json(const std::map<std::string, uint64_t>& m) {
  std::string o = "{";
  bool first = true;
  for (const auto& kv : m) {
    o += std::string(first ? "" : ",") + js(kv.first) + ":" + std::to_string(kv.second);
    first = false;
  }
  return o + "}";
}

int dump(const fs::FileSystem& fs, const std::string& blk, const std::string& entry, const std::string& out_path) {
  BlockChain chain(fs);
  std::string err;
  if (!chain.open_current(blk, &err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 4;
  }
  std::vector<uint8_t> bytes;
  if (!chain.load(entry, bytes, nullptr, &err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 4;
  }
  Image img;
  bool ok = ends_with(lower(entry), ".tga") ? decode_tga(bytes.data(), bytes.size(), img, nullptr, &err)
                                            : decode_dds(bytes.data(), bytes.size(), img, nullptr, &err);
  if (!ok) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 4;
  }
  std::ofstream f(out_path, std::ios::binary);
  uint32_t wh[2] = {img.width, img.height};
  f.write(reinterpret_cast<const char*>(wh), 8);
  f.write(reinterpret_cast<const char*>(img.rgba.data()), static_cast<std::streamsize>(img.rgba.size()));
  return f ? 0 : 4;
}

struct XInfo {
  std::vector<std::string> textures;  // all material texture names of the (last) mesh
  std::set<uint32_t> used;            // attribute ids used by subsets
  std::vector<std::string> mat_tex;   // per material ("" when none)
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: lomn_datacheck <install_dir> [--areas lev/area,...] [--dump <blk> <entry> <out>]...\n");
    return 2;
  }
  std::string install = argv[1];
  std::vector<std::string> areas = {"lev1/vllg", "lev2/ghut", "lev1/l1a7", "lev3/boss", "lev4/crss",
                                    "lev5/le08", "lev6/ta11", "lev7/kpka", "lev0/frnt"};
  std::vector<std::vector<std::string>> dumps;
  for (int i = 2; i < argc; ++i) {
    std::string k = argv[i];
    if (k == "--areas" && i + 1 < argc) {
      areas.clear();
      std::string v = argv[++i];
      for (size_t p = 0; p <= v.size();) {
        size_t q = v.find(',', p);
        if (q == std::string::npos) q = v.size();
        if (q > p) areas.push_back(v.substr(p, q - p));
        p = q + 1;
      }
    } else if (k == "--dump" && i + 3 < argc) {
      dumps.push_back({argv[i + 1], argv[i + 2], argv[i + 3]});
      i += 3;
    } else {
      std::fprintf(stderr, "bad option %s\n", k.c_str());
      return 2;
    }
  }
  auto t_all = Clock::now();
  std::error_code ec;
  if (!std::filesystem::is_directory(install, ec)) {
    std::fprintf(stderr, "not a directory: %s\n", install.c_str());
    return 3;
  }
  auto t_index = Clock::now();
  std::unique_ptr<fs::FileSystem> fsp = fs::open_disk(install, (std::filesystem::temp_directory_path() / "lomn_datacheck_user").string());
  if (!fsp) { std::fprintf(stderr, "cannot open install: %s\n", install.c_str()); return 3; }
  double index_ms = std::chrono::duration<double, std::milli>(Clock::now() - t_index).count();
  const fs::FileSystem& fs = *fsp;
  if (!dumps.empty()) {
    for (const auto& d : dumps)
      if (int rc = dump(fs, d[0], d[1], d[2])) return rc;
    return 0;
  }

  // Every .blk, as a VFS path "Root/<relative path>", sorted by normalised path.
  std::vector<std::string> blks;
  for (auto it = std::filesystem::recursive_directory_iterator(install, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
    if (it->is_regular_file() && lower(it->path().extension().string()) == ".blk")
      blks.push_back("Root/" + std::filesystem::relative(it->path(), install).generic_string());
  std::sort(blks.begin(), blks.end(), [](const std::string& a, const std::string& b) { return fs::normalize(a) < fs::normalize(b); });

  Meter m_header, m_read, m_x, m_dds, m_tga, m_slb;
  std::string out_blk, out_x, out_obj;
  std::vector<Failure> dds_fail, tga_fail, slb_fail, x_fail, read_fail;
  std::map<std::string, uint64_t> dds_formats, tga_kinds, route_tot, flag_tot;
  uint64_t dds_ok = 0, dds_alpha_on = 0, dds_pixels = 0, tga_ok = 0, slb_total = 0, slb_trailer = 0, slb_cin_obj = 0, obj_files = 0,
           moveable_131_132 = 0, obj_missing_models = 0, pos_tables = 0, pos_records = 0, pos_start = 0, light_tables = 0, light_records = 0,
           x_total = 0, x_ok = 0, x_vertices = 0, x_faces = 0, x_colors = 0, x_out_vertices = 0, entries_total = 0, blk_open = 0,
           blk_sorted = 0, unterminated = 0, find_misses = 0;
  std::map<std::string, std::map<std::string, XInfo>> xinfo;  // blk vfs path (normalised) -> entry (lower) -> info
  std::map<std::string, uint64_t> entry_bytes;                 // "<blk>|<entry lower>" -> size (texture bytes per area)

  for (const std::string& blk : blks) {
    uint64_t fsize = 0;
    fs.size(blk, fsize);
    auto t0 = Clock::now();
    BlockChain chain(fs);
    std::string err;
    bool ok = chain.open_current(blk, &err);
    m_header.add(t0, 0);
    std::string rel = blk.substr(5);
    out_blk += std::string(out_blk.empty() ? "" : ",\n") + "{\"path\":" + js(rel);
    if (!ok) {
      out_blk += ",\"ok\":false,\"error\":" + js(err) + "}";
      continue;
    }
    ++blk_open;
    const BlockFile& bf = *chain.blockfile(Source::Current);
    bool sorted = bf.is_sorted();
    blk_sorted += sorted;
    out_blk += ",\"ok\":true,\"version\":" + std::to_string(bf.version()) + ",\"count\":" + std::to_string(bf.entries().size()) +
               ",\"sorted\":" + (sorted ? "true" : "false") + ",\"size\":" + std::to_string(fsize) + ",\"entries\":[";
    std::string norm = fs::normalize(blk);
    std::vector<std::string> parts;
    for (size_t p = 0; p <= norm.size();) {
      size_t q = norm.find('/', p);
      if (q == std::string::npos) q = norm.size();
      parts.push_back(norm.substr(p, q - p));
      p = q + 1;
    }
    bool in_levels = parts.size() == 5 && parts[1] == "data" && parts[2] == "levels";
    for (size_t i = 0; i < bf.entries().size(); ++i) {
      const BlockEntry& e = bf.entries()[i];
      ++entries_total;
      unterminated += !e.name_terminated;
      out_blk += std::string(i ? "," : "") + js(e.name);
      std::string ln = lower(e.name);
      entry_bytes[norm + "|" + ln] = e.full_size;
      // Lookup sanity: ScBlockFile::Find must reach every entry by its own name (true for a sorted table).
      if (bf.find(e.name) != static_cast<int>(i)) ++find_misses;
      std::vector<uint8_t> bytes;
      auto tr = Clock::now();
      if (!bf.read(i, bytes, &err)) {
        read_fail.push_back({rel, e.name, err, e.full_size});
        continue;
      }
      m_read.add(tr, bytes.size());
      if (ends_with(ln, ".x")) {
        ++x_total;
        XFile xf;
        auto tx = Clock::now();
        bool xok = parse_x(bytes.data(), bytes.size(), xf, &err);
        m_x.add(tx, bytes.size());
        out_x += std::string(out_x.empty() ? "" : ",\n") + "{\"blk\":" + js(rel) + ",\"entry\":" + js(e.name);
        if (!xok) {
          x_fail.push_back({rel, e.name, err, bytes.size()});
          out_x += ",\"error\":" + js(err) + "}";
          continue;
        }
        ++x_ok;
        const XMesh& m = xf.mesh;
        x_vertices += m.file_vertices;
        x_faces += m.file_faces;
        x_colors += m.has_colors;
        x_out_vertices += m.source_vertex.size();
        out_x += ",\"meshes\":" + std::to_string(xf.mesh_count) + ",\"vertices\":" + std::to_string(m.file_vertices) +
                 ",\"faces\":" + std::to_string(m.file_faces) + ",\"subsets\":" + std::to_string(m.subsets.size()) +
                 ",\"has_colors\":" + (m.has_colors ? "true" : "false") + ",\"out_vertices\":" + std::to_string(m.source_vertex.size()) +
                 ",\"triangles\":" + std::to_string(m.indices.size() / 3) + ",\"materials\":" + std::to_string(m.materials.size()) + "}";
        XInfo xi;
        for (const XMaterial& mt : m.materials) {
          xi.mat_tex.push_back(mt.has_texture ? mt.texture : "");
          if (mt.has_texture) xi.textures.push_back(mt.texture);
        }
        for (const XSubset& s : m.subsets) xi.used.insert(s.attrib_id);
        xinfo[norm][ln] = std::move(xi);
      } else if (ends_with(ln, ".dds")) {
        Image img;
        DdsInfo di;
        auto td = Clock::now();
        bool dok = decode_dds(bytes.data(), bytes.size(), img, &di, &err);
        m_dds.add(td, bytes.size());
        if (!dok) {
          dds_fail.push_back({rel, e.name, err, bytes.size()});
          continue;
        }
        ++dds_ok;
        dds_formats[dds_format_name(di.format)]++;
        dds_alpha_on += di.alpha_on;
        dds_pixels += static_cast<uint64_t>(img.width) * img.height;
      } else if (ends_with(ln, ".tga")) {
        Image img;
        TgaInfo ti;
        auto tt = Clock::now();
        bool tok = decode_tga(bytes.data(), bytes.size(), img, &ti, &err);
        m_tga.add(tt, bytes.size());
        if (!tok) {
          tga_fail.push_back({rel, e.name, err, bytes.size()});
          continue;
        }
        ++tga_ok;
        tga_kinds["type" + std::to_string(ti.image_type) + "_" + std::to_string(ti.bits) + "bpp_a" + std::to_string(ti.alpha_bits) +
                  (ti.top_down ? "_top" : "_bottom")]++;
      } else if (ends_with(ln, ".slb")) {
        ++slb_total;
        Slb slb;
        auto ts = Clock::now();
        bool sok = slb.parse(bytes, &err);
        m_slb.add(ts, bytes.size());
        if (!sok) {
          slb_fail.push_back({rel, e.name, err, bytes.size()});
          continue;
        }
        slb_trailer += slb.has_trailer();
        if (!in_levels) continue;
        if (ends_with(ln, "_obj.slb")) {
          if (is_cinematic_obj(e.name)) {
            ++slb_cin_obj;
            continue;
          }
          ++obj_files;
          std::vector<ObjRecord> recs;
          if (!parse_obj(slb, recs, &err)) {
            slb_fail.push_back({rel, e.name, "obj view: " + err, bytes.size()});
            continue;
          }
          std::map<std::string, uint64_t> routes, flags;
          std::string drawn, missing;
          for (const ObjRecord& r : recs) {
            Route rt = route(r.flags);
            routes[route_name(rt)]++;
            route_tot[route_name(rt)]++;
            flags[hex(r.flags)]++;
            flag_tot[hex(r.flags)]++;
            if (rt == Route::Moveable && (r.flags == 0x131 || r.flags == 0x132)) ++moveable_131_132;
            if (rt == Route::Ignored) continue;
            std::string id = fourcc(r.id);
            drawn += std::string(drawn.empty() ? "" : ",") + js(id);
            if (bf.find(id + ".x") < 0) {
              ++obj_missing_models;
              missing += std::string(missing.empty() ? "" : ",") + js(id);
            }
          }
          out_obj += std::string(out_obj.empty() ? "" : ",\n") + "{\"blk\":" + js(rel) + ",\"entry\":" + js(e.name) +
                     ",\"records\":" + std::to_string(recs.size()) + ",\"routes\":" + counts_json(routes) + ",\"flags\":" + counts_json(flags) +
                     ",\"drawn\":[" + drawn + "],\"missing_models\":[" + missing + "]}";
        } else if (ends_with(ln, "_pos.slb")) {
          std::vector<PosRecord> recs;
          if (!parse_pos(slb, recs, &err)) {
            slb_fail.push_back({rel, e.name, "pos view: " + err, bytes.size()});
            continue;
          }
          ++pos_tables;
          pos_records += recs.size();
          for (const PosRecord& r : recs) pos_start += (r.flags & 0x1000) != 0;
        } else if (ends_with(ln, "_light.slb")) {
          std::vector<LightRecord> recs;
          if (!parse_light(slb, recs, &err)) {
            slb_fail.push_back({rel, e.name, "light view: " + err, bytes.size()});
            continue;
          }
          ++light_tables;
          light_records += recs.size();
        }
      }
    }
    out_blk += "]}";
  }

  // Texture resolution for the sample areas, the way an area load sees it: Art.blk header loaded,
  // second = <lev>.blk, current = <area>.blk. Names come from every material of every .x in the area blk.
  std::string out_tex;
  auto t_tex = Clock::now();
  for (const std::string& la : areas) {
    size_t slash = la.find('/');
    std::string lev = la.substr(0, slash), area = la.substr(slash + 1);
    BlockChain chain(fs);
    std::string err;
    out_tex += std::string(out_tex.empty() ? "" : ",\n") + "{\"area\":" + js(la);
    if (!chain.open_art("Root/Data/Art/Art.blk", &err) || !chain.enter_area(lev, area, &err)) {
      out_tex += ",\"error\":" + js(err) + "}";
      continue;
    }
    std::string area_norm = fs::normalize(chain.path(Source::Current));
    std::set<std::string> names;
    for (const auto& kv : xinfo[area_norm])
      for (const std::string& t : kv.second.textures) names.insert(t);
    // Textures the drawn models load: materials used by a subset of a model an OBJ record routes somewhere.
    std::set<std::string> drawn_models, drawn_textures;
    {
      const BlockFile& abf = *chain.blockfile(Source::Current);
      for (size_t i = 0; i < abf.entries().size(); ++i) {
        std::string ln = lower(abf.entries()[i].name);
        if (!ends_with(ln, "_obj.slb") || is_cinematic_obj(ln)) continue;
        std::vector<uint8_t> bytes;
        Slb slb;
        std::vector<ObjRecord> recs;
        if (!abf.read(i, bytes, &err) || !slb.parse(bytes, &err) || !parse_obj(slb, recs, &err)) continue;
        for (const ObjRecord& r : recs)
          if (route(r.flags) != Route::Ignored) drawn_models.insert(lower(fourcc(r.id)) + ".x");
      }
      for (const std::string& mdl : drawn_models) {
        auto it = xinfo[area_norm].find(mdl);
        if (it == xinfo[area_norm].end()) continue;
        for (uint32_t a : it->second.used)
          if (a < it->second.mat_tex.size() && !it->second.mat_tex[a].empty()) drawn_textures.insert(it->second.mat_tex[a]);
      }
    }
    std::map<std::string, uint64_t> summary;
    uint64_t drawn_bytes = 0, drawn_resolved = 0;
    std::string list;
    for (const std::string& t : names) {
      std::string chosen;
      resolve_texture(chain, lev, t, chosen);  // ScDrawableTexture::Load's rule (lomn_sage)
      Lookup ld, ex;
      if (!chosen.empty()) {
        ld = chain.find_load(chosen);
        ex = chain.find_exists(chosen);
      }
      std::string base(block_basename(chosen));
      std::string ld_blk = ld.source == Source::None ? "" : std::filesystem::path(chain.path(ld.source)).filename().string();
      std::string ex_blk = ex.source == Source::None ? "" : std::filesystem::path(chain.path(ex.source)).filename().string();
      std::string kind = chosen.empty() ? "missing" : std::string(source_name(ld.source)) + ":" + lower(base.substr(base.rfind('.') + 1));
      summary[kind]++;
      bool drawn = drawn_textures.count(t) != 0;
      if (drawn && ld.source != Source::None) {
        ++drawn_resolved;
        drawn_bytes += entry_bytes[fs::normalize(chain.path(ld.source)) + "|" + lower(chain.blockfile(ld.source)->entries()[ld.index].name)];
      }
      list += std::string(list.empty() ? "" : ",") + "{\"name\":" + js(t) + ",\"chosen\":" + js(base) + ",\"load\":" +
              js(ld.source == Source::None ? "" : std::string(source_name(ld.source)) + ":" + ld_blk + ":" + std::to_string(ld.index)) +
              ",\"exists\":" + js(ex.source == Source::None ? "" : std::string(source_name(ex.source)) + ":" + ex_blk + ":" + std::to_string(ex.index)) +
              ",\"drawn\":" + (drawn ? "true" : "false") + "}";
    }
    std::string dm;
    for (const std::string& d : drawn_models) dm += std::string(dm.empty() ? "" : ",") + js(d);
    out_tex += ",\"distinct\":" + std::to_string(names.size()) + ",\"summary\":" + counts_json(summary) + ",\"drawn_models\":[" + dm +
               "],\"drawn_textures\":" + std::to_string(drawn_textures.size()) + ",\"drawn_textures_resolved\":" + std::to_string(drawn_resolved) +
               ",\"drawn_texture_bytes\":" + std::to_string(drawn_bytes) + ",\"names\":[" + list + "]}";
  }
  double tex_ms = std::chrono::duration<double, std::milli>(Clock::now() - t_tex).count();
  double wall_ms = std::chrono::duration<double, std::milli>(Clock::now() - t_all).count();

  std::printf("{\n\"install\":%s,\n", js(install).c_str());
  std::printf("\"blk_summary\":{\"files\":%zu,\"opened\":%llu,\"sorted\":%llu,\"entries\":%llu,\"unterminated_names\":%llu,\"find_misses\":%llu,\"read_failures\":%s},\n",
              blks.size(), (unsigned long long)blk_open, (unsigned long long)blk_sorted, (unsigned long long)entries_total,
              (unsigned long long)unterminated, (unsigned long long)find_misses, failures_json(read_fail).c_str());
  std::printf("\"blk\":[\n%s],\n", out_blk.c_str());
  std::printf("\"obj_summary\":{\"files\":%llu,\"cin_obj_skipped\":%llu,\"routes\":%s,\"flags\":%s,\"moveable_0x131_0x132\":%llu,\"missing_models\":%llu},\n",
              (unsigned long long)obj_files, (unsigned long long)slb_cin_obj, counts_json(route_tot).c_str(), counts_json(flag_tot).c_str(),
              (unsigned long long)moveable_131_132, (unsigned long long)obj_missing_models);
  std::printf("\"obj\":[\n%s],\n", out_obj.c_str());
  std::printf("\"pos_summary\":{\"tables\":%llu,\"records\":%llu,\"start_0x1000\":%llu},\n\"light_summary\":{\"tables\":%llu,\"records\":%llu},\n",
              (unsigned long long)pos_tables, (unsigned long long)pos_records, (unsigned long long)pos_start, (unsigned long long)light_tables,
              (unsigned long long)light_records);
  std::printf("\"slb_summary\":{\"total\":%llu,\"with_trailer\":%llu,\"failures\":%s},\n", (unsigned long long)slb_total,
              (unsigned long long)slb_trailer, failures_json(slb_fail).c_str());
  std::printf("\"x_summary\":{\"files\":%llu,\"ok\":%llu,\"vertices\":%llu,\"faces\":%llu,\"with_colors\":%llu,\"out_vertices\":%llu,\"failures\":%s},\n",
              (unsigned long long)x_total, (unsigned long long)x_ok, (unsigned long long)x_vertices, (unsigned long long)x_faces,
              (unsigned long long)x_colors, (unsigned long long)x_out_vertices, failures_json(x_fail).c_str());
  std::printf("\"x\":[\n%s],\n", out_x.c_str());
  std::printf("\"dds_summary\":{\"files\":%llu,\"ok\":%llu,\"formats\":%s,\"alpha_on\":%llu,\"pixels\":%llu,\"failures\":%s},\n",
              (unsigned long long)(dds_ok + dds_fail.size()), (unsigned long long)dds_ok, counts_json(dds_formats).c_str(),
              (unsigned long long)dds_alpha_on, (unsigned long long)dds_pixels, failures_json(dds_fail).c_str());
  std::printf("\"tga_summary\":{\"files\":%llu,\"ok\":%llu,\"kinds\":%s,\"failures\":%s},\n", (unsigned long long)(tga_ok + tga_fail.size()),
              (unsigned long long)tga_ok, counts_json(tga_kinds).c_str(), failures_json(tga_fail).c_str());
  std::printf("\"textures\":[\n%s],\n", out_tex.c_str());
  std::printf("\"timing\":{\"wall_ms\":%.1f,\"vfs_index_ms\":%.1f,\"texture_resolution_ms\":%.1f,\"blk_header\":%s,\"entry_read\":%s,\"x\":%s,\"dds\":%s,\"tga\":%s,\"slb\":%s}\n}\n",
              wall_ms, index_ms, tex_ms, m_header.json().c_str(), m_read.json().c_str(), m_x.json().c_str(), m_dds.json().c_str(),
              m_tga.json().c_str(), m_slb.json().c_str());
  return 0;
}
