// The parser error-string fragments the fuzz driver (fuzz_formats.cpp) matches to decide "deep reach" (an input got past
// the header checks into the format's body loop) and the .x token-stream column. One table, two users:
//   - fuzz_formats.cpp matches ONLY through a `Fragment` (its has() takes nothing else), so a raw string literal at a match
//     site does not compile: Fragment has no converting constructor and its only constructor is private (M1a-4 cleanup S3,
//     batch-1 review M-2; the static_asserts in test_fuzz_fragments.cpp pin that);
//   - test_fuzz_fragments.cpp proves every entry is produced by at least one crafted input, so renaming a parser message
//     fails a ctest instead of silently turning the driver's deep-reach percentage into 0 (M1a-4 cleanup C3, review M-6),
//     and that every message of xfile.cpp's decode_mesh starts with a kXDecodeMeshPrefixes entry (cleanup S2).
// Adding a fragment to the driver means adding it here AND a crafted producer there (the test fails otherwise).
#pragma once
#include <cstring>
#include <string>
#include <vector>

namespace fuzz_frag {

enum class Match { Substr, Prefix };

struct Fragment;
namespace detail {
constexpr Fragment make(const char* fmt, const char* text, Match how);
}

struct Fragment {
  const char* fmt;   // the fuzz format that uses it
  const char* text;  // what the driver looks for in the parser's error string
  Match how;

 private:
  constexpr Fragment(const char* f, const char* t, Match h) : fmt(f), text(t), how(h) {}
  friend constexpr Fragment detail::make(const char*, const char*, Match);
};

namespace detail {
constexpr Fragment make(const char* fmt, const char* text, Match how) { return Fragment(fmt, text, how); }
}  // namespace detail

// BLK / LZSS / SLB / TGA: substring matches.
inline constexpr Fragment kBlkNameRun = detail::make("blk", "name run", Match::Substr);  // blockfile.cpp: the entry loop's own error
inline constexpr Fragment kLzssRatio = detail::make("lzss", "ratio", Match::Substr);     // blockfile.cpp: declared size exceeds expansion-ratio cap (NOT deep)
inline constexpr Fragment kSlbTexFixup = detail::make("slb.tex", "name is not a fix-up slot", Match::Substr);  // slb.cpp parse_spec_textures record loop
inline constexpr Fragment kSlbTexNameLong = detail::make("slb.tex", "name longer", Match::Substr);             // slb.cpp parse_spec_textures record loop
inline constexpr Fragment kTgaRleTruncated = detail::make("tga", "RLE data truncated", Match::Substr);        // image.cpp RLE packet loop
inline constexpr Fragment kTgaRleRunsPast = detail::make("tga", "RLE packet runs past", Match::Substr);       // image.cpp RLE packet loop

// .x: parse_x's header errors (substring; they decide the token-stream column, not deep reach).
inline constexpr Fragment kXTooLarge = detail::make("x", "file too large", Match::Substr);
inline constexpr Fragment kXNotX = detail::make("x", "not a .x file", Match::Substr);
inline constexpr Fragment kXNotBinary = detail::make("x", "not a binary .x file", Match::Substr);
inline constexpr Fragment kXFloatSize = detail::make("x", "unsupported float size", Match::Substr);

// .x: every error decode_mesh (xfile.cpp) can return starts with one of these (prefix match). test_fuzz_fragments.cpp
// scans decode_mesh's source and fails for a message that does not (cleanup S2).
inline constexpr Fragment kXDecodeMeshPrefixes[] = {
    detail::make("x", "x: Mesh ", Match::Prefix),          detail::make("x", "x: MeshNormals ", Match::Prefix),
    detail::make("x", "x: MeshTextureCoords ", Match::Prefix), detail::make("x", "x: MeshVertexColors ", Match::Prefix),
    detail::make("x", "x: MeshMaterialList ", Match::Prefix),  detail::make("x", "x: Material floats", Match::Prefix),
    detail::make("x", "x: face ", Match::Prefix),          detail::make("x", "x: material ", Match::Prefix),
    detail::make("x", "x: normal index", Match::Prefix),   detail::make("x", "x: texture name", Match::Prefix),
    detail::make("x", "x: too many materials", Match::Prefix), detail::make("x", "x: vertex colour", Match::Prefix)};

// Every fragment above, flattened (the prefix list expanded). A constant added above but missing here is caught by
// test_fuzz_fragments.cpp (it scans this header: each k* name must be used a second time).
inline std::vector<Fragment> all_fragments() {
  std::vector<Fragment> v = {kBlkNameRun,    kLzssRatio,      kSlbTexFixup, kSlbTexNameLong, kTgaRleTruncated,
                             kTgaRleRunsPast, kXTooLarge, kXNotX,       kXNotBinary,     kXFloatSize};
  for (const Fragment& pre : kXDecodeMeshPrefixes) v.push_back(pre);
  return v;
}

inline bool matches(const std::string& err, const Fragment& f) {
  if (f.how == Match::Prefix) return err.compare(0, std::strlen(f.text), f.text) == 0;
  return err.find(f.text) != std::string::npos;
}

}  // namespace fuzz_frag
