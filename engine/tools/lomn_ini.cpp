// M2a-3 T5a: lomn_ini <Bionicle.ini> <cases.tsv>: one `INI ...` line per case (the pair-comparison format, ini.h contract).
// Case fields (TSV): id, kind (str|int), section, key, default (hex bytes; decimal for int), size. `#` lines are comments.
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "lomn/ini.h"

static std::string hex(const std::string& s) {
  std::string r;
  char b[3];
  for (unsigned char c : s) r += (std::snprintf(b, sizeof b, "%02x", c), b);
  return r;
}
static std::string unhex(const std::string& h) {
  std::string r;
  for (size_t i = 0; i + 1 < h.size(); i += 2) r += static_cast<char>(std::stoi(h.substr(i, 2), nullptr, 16));
  return r;
}

int main(int argc, char** argv) {
  if (argc != 3) return std::fprintf(stderr, "usage: lomn_ini <ini> <cases.tsv>\n"), 2;
  std::ifstream fi(argv[1], std::ios::binary), fc(argv[2]);
  if (!fi || !fc) return std::fprintf(stderr, "lomn_ini: cannot open input\n"), 2;
  const std::string text((std::istreambuf_iterator<char>(fi)), std::istreambuf_iterator<char>());
  const lomn::ini::File f(text);
  std::string line;
  while (std::getline(fc, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> c(1);
    for (char ch : line) ch == '\t' ? c.emplace_back() : (c.back() += ch);
    if (c.size() != 6) return std::fprintf(stderr, "lomn_ini: bad case line: %s\n", line.c_str()), 2;
    if (c[1] == "int") {
      const uint32_t v = f.get_int(c[2], c[3], static_cast<uint32_t>(std::stoul(c[4])));
      std::printf("INI %s int sec=%s key=%s def=%s size=0 -> len=4 hex=%02x%02x%02x%02x\n", c[0].c_str(), c[2].c_str(), c[3].c_str(),
                  c[4].c_str(), v >> 24, (v >> 16) & 255, (v >> 8) & 255, v & 255);
    } else {
      const size_t n = std::stoul(c[5]);
      const std::string v = f.get_string(c[2], c[3], unhex(c[4]), n);
      std::printf("INI %s str sec=%s key=%s def=%s size=%zu -> len=%zu hex=%s\n", c[0].c_str(), c[2].c_str(), c[3].c_str(), c[4].c_str(),
                  n, v.size(), hex(v).c_str());
    }
  }
  return 0;
}
