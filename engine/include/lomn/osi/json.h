// M2a-1 T1: a minimal JSON reader for the OSI data tables (data/osi/*.json) and the conformance expectations. Objects
// keep their key order; numbers keep their source text (so u32 values and hex-looking strings are exact).
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lomn::osi {

struct Json {
  enum Type { Null, Bool, Number, String, Array, Object } type = Null;
  bool b = false;
  std::string str;  // String: the decoded text (UTF-8 for \u escapes); Number: the source text
  std::vector<Json> arr;
  std::vector<std::pair<std::string, Json>> obj;

  const Json* get(std::string_view key) const;  // Object member or nullptr
  bool is_null() const { return type == Null; }
  double num() const;                           // Number as double
  bool as_i64(int64_t& out) const;              // Number that is an integer
};

bool parse_json(std::string_view text, Json& out, std::string* err);

}  // namespace lomn::osi
