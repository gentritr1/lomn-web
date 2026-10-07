// The game's blockfile lookup chain over the VFS: a "current" blockfile (the area during an area load), a
// "second" one (the level) and the global Art.blk. Reproduces SrBlockFileLoad (0x0054db60) and
// SrBlockFileExists (0x0054ddc0), which search in DIFFERENT orders. Only the basename of a request matters
// (ScBlockFile::Find); loose files are reached only while no current blockfile is set.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "lomn/fs.h"
#include "lomn/sage/blockfile.h"

namespace lomn::sage {

enum class Source { None, Current, Second, Art, Disk };
const char* source_name(Source s);  // "none" | "current" | "second" | "art" | "disk"

struct Lookup {
  Source source = Source::None;
  int index = -1;  // entry index in that blockfile (-1 for Disk/None)
};

class BlockChain {
 public:
  explicit BlockChain(const fs::FileSystem& fs) : fs_(fs) {}

  // Opens a blockfile header through the VFS (case-insensitive path). The game builds these paths from
  // "Root/Data/Levels/%.4s/%.4s.blk" (0x0070b7ec) and "Root/Data/Art/Art.blk".
  bool open_art(std::string_view path, std::string* err);       // GcGame::LoadArtBlockHeader (0x0043b030)
  bool open_current(std::string_view path, std::string* err);   // e.g. LoadAreaBlockHeader (0x0048bc10)
  bool open_second(std::string_view path, std::string* err);    // e.g. LoadLevelBlockHeader (0x0048bd10)
  void clear_current() { current_ = BlockFile(); current_path_.clear(); }
  void clear_second() { second_ = BlockFile(); second_path_.clear(); }
  // Area entry as GcAreaLoader::LoadArea (0x004fedd0) sets it up: second = <lev>.blk, current = <area>.blk.
  bool enter_area(std::string_view level, std::string_view area, std::string* err);

  // Where SrBlockFileLoad would take `request` from: current -> second -> Art when a current blockfile is set,
  // otherwise Art -> disk (full request path through the VFS).
  Lookup find_load(std::string_view request) const;
  // Where SrBlockFileExists would find `request`: Art FIRST, then current -> second; disk only without current.
  Lookup find_exists(std::string_view request) const;
  bool exists(std::string_view request) const { return find_exists(request).source != Source::None; }

  // SrBlockFileLoad: bytes of the first hit in load order. An entry that fails to read is skipped like the
  // original's NULL return (the search continues); `from` reports the source actually used.
  bool load(std::string_view request, std::vector<uint8_t>& out, Source* from, std::string* err) const;

  const BlockFile* blockfile(Source s) const;
  const std::string& path(Source s) const;

 private:
  bool open_into(std::string_view path, BlockFile& bf, std::string& stored_path, std::string* err);
  const fs::FileSystem& fs_;
  BlockFile current_, second_, art_;
  std::string current_path_, second_path_, art_path_, none_;
};

// Texture path resolution of ScDrawableTexture::Load (0x005561c0): the request is
// "Root/Data/Levels/<lev>/textures/<name>". If its extension (compared case-insensitively) is one of
// gTextureTypes {".dds", ".tga"} and the file exists (SrBlockFileExists order, chain.exists()), it is kept as
// written; otherwise ".dds" then ".tga" are appended to the whole request and the first that exists wins.
// Returns false and clears `chosen` when nothing exists.
bool resolve_texture(const BlockChain& chain, std::string_view lev, std::string_view name, std::string& chosen);

}  // namespace lomn::sage
