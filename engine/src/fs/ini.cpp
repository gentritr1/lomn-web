// M2a-3 T5a: Bionicle.ini with Win32 GetPrivateProfileStringA / GetPrivateProfileIntA semantics; the contract is in
// lomn/ini.h (docs/osi/hud.md §6). Citation: the original calls kernel32 with the path buffer [0x83b410] (0x5d4fca-0x5d4fea
// and the 4 other string/16 int call sites, hud.md §6.1); the matching rules are Microsoft's documented behaviour, not
// asm-derived (kernel32 is not in the install's binaries we read).
#include "lomn/ini.h"

#include "lomn/fs.h"

namespace lomn::ini {
namespace {
bool blank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }
char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c | 0x20) : c; }
std::string lowered(std::string_view s) {
  std::string r(s);
  for (char& c : r) c = lower(c);
  return r;
}
std::string_view trim(std::string_view s) {
  while (!s.empty() && blank(s.front())) s.remove_prefix(1);
  while (!s.empty() && blank(s.back())) s.remove_suffix(1);
  return s;
}
std::string_view trim_end(std::string_view s) {
  while (!s.empty() && blank(s.back())) s.remove_suffix(1);
  return s;
}
}  // namespace

File::File(std::string_view text) {
  std::string section;
  bool in_section = false;
  size_t pos = 0;
  while (pos < text.size()) {
    // a line ends at CRLF, CR or LF (Windows and Wine; each of the three ends exactly one line)
    size_t nl = text.find_first_of("\r\n", pos);
    if (nl == std::string_view::npos) nl = text.size();
    const std::string_view line = trim(text.substr(pos, nl - pos));
    pos = nl + ((nl + 1 < text.size() && text[nl] == '\r' && text[nl + 1] == '\n') ? 2 : 1);
    if (line.empty() || line[0] == ';') continue;
    if (line[0] == '[') {
      const size_t close = line.find(']');
      if (close == std::string_view::npos) continue;  // malformed header: ignored (outside the crafted table)
      section = lowered(line.substr(1, close - 1));
      in_section = true;
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string_view::npos || !in_section) continue;
    const std::string_view key = trim(line.substr(0, eq));
    if (key.empty()) continue;
    std::string_view v = trim(line.substr(eq + 1));
    if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front()) v = v.substr(1, v.size() - 2);
    entries_.push_back({section, lowered(key), std::string(v)});
  }
}

const File::Entry* File::find(std::string_view section, std::string_view key) const {
  const std::string s = lowered(section), k = lowered(key);
  for (const Entry& e : entries_)  // file order: the first match wins
    if (e.section == s && e.key == k) return &e;
  return nullptr;
}

std::string File::get_string(std::string_view section, std::string_view key, std::string_view def, size_t bufsize) const {
  if (bufsize == 0) return {};
  const Entry* e = find(section, key);
  std::string r = e ? e->value : std::string(trim_end(def));  // Win32 strips trailing blanks of the DEFAULT
  if (r.size() > bufsize - 1) r.resize(bufsize - 1);
  return r;
}

uint32_t File::get_int(std::string_view section, std::string_view key, uint32_t def) const {
  const Entry* e = find(section, key);
  if (!e) return def;
  const std::string& s = e->value;
  size_t i = 0;
  const bool neg = i < s.size() && s[i] == '-';
  if (neg || (i < s.size() && s[i] == '+')) ++i;
  uint32_t v = 0;
  for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) v = v * 10u + static_cast<uint32_t>(s[i] - '0');  // wraps like UINT
  return neg ? 0u - v : v;
}

File File::after_loadini() const {
  // LoadIni 0x5d4890: GetPrivateProfileIntA(GraphicsOptions, ResolutionW, 0) is tested as a 32-bit value (test eax,eax,
  // 0x5d4a2a; hud.md 6.2a). 0 rebuilds the file: fopen "w+" truncates, then these 14 keys (10 + 4) and nothing else, so every
  // other section (Cheat included) is gone. Modelled in memory only; the user's file is never written.
  if (get_int("GraphicsOptions", "ResolutionW", 0) != 0) return *this;
  static const char* const kRebuilt[][3] = {
      {"graphicsoptions", "ResolutionW", "640"}, {"graphicsoptions", "ResolutionH", "480"}, {"graphicsoptions", "Windowed", "1"},
      {"graphicsoptions", "BitDepth", "32"},     {"graphicsoptions", "SvpOther", "0"},      {"graphicsoptions", "SvpRad", "0"},
      {"graphicsoptions", "Fog", "1"},           {"graphicsoptions", "ParticleDetail", "100"},
      {"graphicsoptions", "ShadowDetail", "2"},  {"graphicsoptions", "Gamma", "256"},       {"misc", "Language", "0"},
      {"misc", "ControlOption", "1"},            {"misc", "SoundVolume", "100"},            {"misc", "MusicVolume", "100"}};
  File r;
  for (const auto& k : kRebuilt) r.entries_.push_back({k[0], lowered(k[1]), k[2]});
  return r;
}

bool load(const fs::FileSystem& vfs, File& out) {
  std::vector<uint8_t> bytes;
  if (vfs.read("Root/Bionicle.ini", bytes) != fs::Error::None) return false;
  out = File(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size())).after_loadini();
  return true;
}

}  // namespace lomn::ini
