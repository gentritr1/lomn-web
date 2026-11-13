// M2a-3 T5a: Bionicle.ini with Win32 GetPrivateProfileStringA / GetPrivateProfileIntA semantics (docs/osi/hud.md §6; the
// original passes the file path [0x83b410] to kernel32). The text is read through the VFS ("Root/Bionicle.ini") and never
// written: LoadIni's rebuild branch (hud.md 6.2a) is modelled in memory by File::after_loadini().
//
// Contract (Microsoft's documented behaviour and the Wine tests; not configparser, not asm-derived: kernel32 is not read):
//  - section and key names match case-insensitively (ASCII); the first matching key wins;
//  - a line ends at CRLF, CR or LF; a line whose first non-blank character is ';' is a comment; "key = value" with the
//    key and the value trimmed of blanks; no inline-comment stripping;
//  - a value wrapped in a matching pair of quotes (" or ') loses them; a lone or mismatched quote stays;
//  - the result is cut to bufsize - 1 bytes (empty for bufsize 0); trailing blanks of the DEFAULT are stripped;
//  - get_int: optional '-' or '+', then leading decimal digits, 0 when there are none, wrapping as UINT; the default only
//    for a missing key.
// INFERRED, deliberately NOT implemented (Wine-only behaviours; not in the crafted table): an empty int value giving the
// default, 0x hex ints, quoted ints, searching the first section block only (here: every block of that name, in file
// order), "[ Sp ]" trimming inside the brackets, the last ']' ending a header (here: the first), quotes in the default,
// truncation of a quoted value, keys before any section (here: ignored, as are lines without '=' and headers without ']'),
// whitespace-only sets.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lomn::fs {
class FileSystem;
}

namespace lomn::ini {

class File {
 public:
  File() = default;
  explicit File(std::string_view text);
  // GetPrivateProfileStringA(section, key, def, buf, bufsize, path): the value, or `def` when the section or key is
  // missing, cut to bufsize - 1 bytes.
  std::string get_string(std::string_view section, std::string_view key, std::string_view def, size_t bufsize) const;
  // GetPrivateProfileIntA (the original only calls it, hud.md §6.2); see the contract above.
  uint32_t get_int(std::string_view section, std::string_view key, uint32_t def) const;
  // LoadIni's rebuild (hud.md 6.2a): when the 32-bit ResolutionW reads 0 the file is rebuilt with exactly 14 keys
  // (GraphicsOptions x10 with Windowed=1, Gamma=256; Misc x4) and every other section is lost; otherwise a copy of *this.
  File after_loadini() const;

 private:
  struct Entry {
    std::string section, key, value;  // section and key lower-cased
  };
  std::vector<Entry> entries_;
  const Entry* find(std::string_view section, std::string_view key) const;
};

// The install's Bionicle.ini through the VFS ("Root/Bionicle.ini"), after LoadIni's rebuild rule (after_loadini); false
// when the file is missing (the callers then keep their defaults).
bool load(const fs::FileSystem& vfs, File& out);

}  // namespace lomn::ini
