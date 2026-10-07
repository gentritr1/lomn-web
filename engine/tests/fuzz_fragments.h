// The parser error-string fragments the fuzz driver (fuzz_formats.cpp) matches to decide "deep reach" (an input got past
// the header checks into the format's body loop) and the .x token-stream column. One table, two users:
//   - fuzz_formats.cpp matches ONLY through these names (no string literal at a match site);
//   - test_fuzz_fragments.cpp proves every entry is produced by at least one crafted input, so renaming a parser message
//     fails a ctest instead of silently turning the driver's deep-reach percentage into 0 (M1a-4 cleanup C3, review M-6).
// Adding a fragment to the driver means adding it here AND a crafted producer there (the test fails otherwise).
#pragma once
#include <cstring>
#include <string>
#include <vector>

namespace fuzz_frag {

enum class Match { Substr, Prefix };

struct Fragment {
  const char* fmt;   // the fuzz format that uses it
  const char* text;  // what the driver looks for in the parser's error string
  Match how;
};

// BLK / LZSS / SLB / TGA: substring matches.
inline constexpr const char* kBlkNameRun = "name run";           // blockfile.cpp: the entry loop's own error
inline constexpr const char* kLzssRatio = "ratio";               // blockfile.cpp: declared size exceeds expansion-ratio cap (NOT deep)
inline constexpr const char* kSlbTexFixup = "name is not a fix-up slot";  // slb.cpp parse_spec_textures record loop
inline constexpr const char* kSlbTexNameLong = "name longer";             // slb.cpp parse_spec_textures record loop
inline constexpr const char* kTgaRleTruncated = "RLE data truncated";     // image.cpp RLE packet loop
inline constexpr const char* kTgaRleRunsPast = "RLE packet runs past";    // image.cpp RLE packet loop

// .x: parse_x's header errors (substring; they decide the token-stream column, not deep reach).
inline constexpr const char* kXTooLarge = "file too large";
inline constexpr const char* kXNotX = "not a .x file";
inline constexpr const char* kXNotBinary = "not a binary .x file";
inline constexpr const char* kXFloatSize = "unsupported float size";

// .x: every error decode_mesh (xfile.cpp) can return starts with one of these (prefix match).
inline constexpr const char* kXDecodeMeshPrefixes[] = {"x: Mesh ",       "x: MeshNormals ", "x: MeshTextureCoords ", "x: MeshVertexColors ",
                                                       "x: MeshMaterialList ", "x: Material floats", "x: face ",       "x: material ",
                                                       "x: normal index", "x: texture name", "x: too many materials", "x: vertex colour"};

// Every fragment above, flattened (the prefix list expanded). A constant added above but missing here is caught by
// test_fuzz_fragments.cpp (it scans this header: each k* name must be used a second time).
inline std::vector<Fragment> all_fragments() {
  std::vector<Fragment> v = {{"blk", kBlkNameRun, Match::Substr},        {"lzss", kLzssRatio, Match::Substr},
                             {"slb.tex", kSlbTexFixup, Match::Substr},   {"slb.tex", kSlbTexNameLong, Match::Substr},
                             {"tga", kTgaRleTruncated, Match::Substr},   {"tga", kTgaRleRunsPast, Match::Substr},
                             {"x", kXTooLarge, Match::Substr},           {"x", kXNotX, Match::Substr},
                             {"x", kXNotBinary, Match::Substr},          {"x", kXFloatSize, Match::Substr}};
  for (const char* pre : kXDecodeMeshPrefixes) v.push_back({"x", pre, Match::Prefix});
  return v;
}

inline bool matches(const std::string& err, const char* text, Match how) {
  if (how == Match::Prefix) return err.compare(0, std::strlen(text), text) == 0;
  return err.find(text) != std::string::npos;
}

}  // namespace fuzz_frag
