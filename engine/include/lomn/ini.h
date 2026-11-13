// M2a-3 T5a: Bionicle.ini with Win32 GetPrivateProfileStringA / GetPrivateProfileIntA semantics (docs/osi/hud.md §6; the
// original passes the file path [0x83b410] to kernel32). The text is read through the VFS ("Root/Bionicle.ini") and never
// written (LoadIni's write-if-missing branch, 0x5d4a24, is recorded in hud.md §6.2 and not run: the install is read-only).
//
// Contract (Microsoft's documented behaviour; not configparser): section and key names match case-insensitively (ASCII);
// the first matching key in a section wins, and a section header may repeat (the keys of every block of that name are
// searched in file order, INFERRED, not in the crafted table); lines are split on LF, CR is whitespace; a line whose first
// non-blank character is ';' is a comment; "key = value" with the key and the value trimmed of blanks; a value wrapped in a
// matching pair of quotes (" or ') loses them (a lone or mismatched quote stays); no inline-comment stripping; a line
// without '=' and a key before any [section] are ignored; the result is cut to bufsize - 1 bytes (0 for bufsize 0).
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
  // GetPrivateProfileIntA: the value's leading decimal digits (an optional '-' wraps as UINT), 0 when it has none; `def` only
  // when the key is missing. INFERRED from Microsoft's description of the API (the original only calls it, hud.md §6.2).
  uint32_t get_int(std::string_view section, std::string_view key, uint32_t def) const;

 private:
  struct Entry {
    std::string section, key, value;  // section and key lower-cased
  };
  std::vector<Entry> entries_;
  const Entry* find(std::string_view section, std::string_view key) const;
};

// The install's Bionicle.ini through the VFS ("Root/Bionicle.ini"); false when it is missing (the original would write
// defaults; here the callers keep the defaults).
bool load(const fs::FileSystem& vfs, File& out);

}  // namespace lomn::ini
