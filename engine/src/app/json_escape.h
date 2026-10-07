// Minimal JSON string escaper for the hand-written metrics.json (internal to the app layer).
#pragma once
#include <cstdio>
#include <string>
#include <string_view>

namespace lomn {

// Escapes `"`, `\` and control characters < 0x20 (as \u00XX); other bytes pass through unchanged.
inline std::string json_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char ch : s) {
    unsigned char c = static_cast<unsigned char>(ch);
    if (c == '"') out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (c < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof buf, "\\u%04x", c);
      out += buf;
    } else {
      out += ch;
    }
  }
  return out;
}

}  // namespace lomn
