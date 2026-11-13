// lomn_font_table <install_dir>
// Prints the glyph table of every tile font (data/Art/Fonts/*.col) of an install, in a fixed text format the controller
// diffs against the Python side's table (M2a-3 Task 7). Fonts in ASCII order of their names.
//   line 1: this format description (the diff strips it)
//   font NAME dir=N glyphs=N rows=R cols=C cw=W ch=H kern=K base=B space=S maxw=M minw=m tga=WxH
//   glyph NAME IDX ASCII W H XOFF YOFF          one per record, record order; ASCII = 4 lowercase hex digits, rest decimal
//   str NAME 'TEXT' len=L height=H              "New Game", "Options", "Quit", "Play Game"; height=na when a character of the
//                                               string has no record (a space included: the original reads out of bounds)
// Exit codes: 0 ok, 2 usage, 3 the install or a font could not be read.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "lomn/fs.h"
#include "lomn/sage/colfont.h"

using namespace lomn;
using namespace lomn::sage;

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: lomn_font_table <install_dir>\n");
    return 2;
  }
  const std::string install = argv[1];
  std::unique_ptr<fs::FileSystem> vfs = fs::open_disk(install, "");
  if (!vfs) {
    std::fprintf(stderr, "lomn_font_table: %s is not an install directory\n", install.c_str());
    return 3;
  }
  std::vector<std::string> names;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(install + "/data/Art/Fonts", ec)) {
    std::string n = e.path().filename().string();
    std::string low = n;
    for (char& c : low) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    if (low.size() > 4 && low.compare(low.size() - 4, 4, ".col") == 0) names.push_back(n.substr(0, n.size() - 4));
  }
  if (ec || names.empty()) {
    std::fprintf(stderr, "lomn_font_table: no fonts under %s/data/Art/Fonts\n", install.c_str());
    return 3;
  }
  std::sort(names.begin(), names.end());
  std::printf("# lomn_font_table v1: font NAME dir= glyphs= rows= cols= cw= ch= kern= base= space= maxw= minw= tga=WxH | glyph NAME IDX ASCII(4 hex) W H XOFF YOFF | str NAME 'TEXT' len= height=(N|na); fonts in ASCII order\n");
  for (const std::string& name : names) {
    LoadedFont lf;
    std::string err;
    if (!load_tile_font(*vfs, "Root/Data/Art/Fonts/" + name, lf, &err)) {
      std::fprintf(stderr, "lomn_font_table: %s: %s\n", name.c_str(), err.c_str());
      return 3;
    }
    const TileFont& f = lf.font;
    std::printf("font %s dir=%zu glyphs=%d rows=%d cols=%d cw=%d ch=%d kern=%d base=%d space=%d maxw=%u minw=%u tga=%ux%u\n", name.c_str(),
                lf.col.entries().size(), f.count, f.rows, f.columns, f.char_width, f.char_height, f.kerning, f.baseline, f.space_width,
                static_cast<uint32_t>(f.max_width), static_cast<uint32_t>(f.min_width), lf.glyph_images[0].width, lf.glyph_images[0].height);
    for (size_t i = 0; i < f.glyphs.size(); ++i) {
      const FontGlyph& g = f.glyphs[i];
      std::printf("glyph %s %zu %04x %d %d %d %d\n", name.c_str(), i, g.ascii, g.width, g.height, g.x_offset, g.y_offset);
    }
    for (const char* text : {"New Game", "Options", "Quit", "Play Game"}) {
      int h = 0;
      const bool ok = f.get_str_height(text, 1.0f, &h);
      if (ok) std::printf("str %s '%s' len=%d height=%d\n", name.c_str(), text, f.get_str_len(text), h);
      else std::printf("str %s '%s' len=%d height=na\n", name.c_str(), text, f.get_str_len(text));
    }
  }
  return 0;
}
