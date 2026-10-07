// lomn_ffp_cases: prints, one JSON object per line, every input the CPU reference of the D3D8 fixed-function formulas
// (lomn/d3d/ffp.h reference_vertex_color / reference_fragment) reads, and its results, for gpu_ffp's cases
// (tests/ffp_cases.h) followed by N seeded random samples. tools/research/oracle/ffp_oracle.py recomputes every result
// from the D3D8 documentation's formulas and compares (ctest ffp_oracle). Usage: lomn_ffp_cases [N] [SEED]
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "ffp_cases.h"
#include "lomn/d3d/ffp.h"

using namespace lomn;
using namespace lomn::d3d;

namespace {
uint64_t g_rng = 1;
uint32_t rnd(uint32_t n) {
  g_rng = g_rng * 6364136223846793005ull + 1442695040888963407ull;
  return static_cast<uint32_t>((g_rng >> 33) % n);
}
float uni(float lo, float hi) { return lo + (hi - lo) * static_cast<float>(rnd(1u << 24)) / static_cast<float>(1u << 24); }
Color4 col() { return Color4{uni(0, 1), uni(0, 1), uni(0, 1), uni(0, 1)}; }
uint32_t argb() { return (rnd(256) << 24) | (rnd(256) << 16) | (rnd(256) << 8) | rnd(256); }

std::string f(float x) {
  char b[32];
  std::snprintf(b, sizeof b, "%.9g", static_cast<double>(x));
  return b;
}
std::string c4(const Color4& c) { return "[" + f(c.r) + "," + f(c.g) + "," + f(c.b) + "," + f(c.a) + "]"; }

void emit(const char* what, const State& s, bool has_vcol, const Color4& vcol, const float p[3], const float n[3],
          const Color4& d, const Color4& texel, bool bound, float w) {
  const Color4 v = reference_vertex_color(s, has_vcol, vcol, p, n);
  Color4 o;
  const bool kept = reference_fragment(s, d, texel, bound, w, o);
  std::string j = std::string("{\"what\":\"") + what + "\",\"rs\":{";
  const uint32_t rsl[] = {rs::LIGHTING, rs::COLORVERTEX, rs::DIFFUSEMATERIALSOURCE, rs::AMBIENT, rs::TEXTUREFACTOR,
                          rs::ALPHATESTENABLE, rs::ALPHAFUNC, rs::ALPHAREF, rs::ALPHABLENDENABLE, rs::FOGENABLE,
                          rs::FOGTABLEMODE, rs::FOGVERTEXMODE, rs::FOGSTART, rs::FOGEND, rs::FOGDENSITY, rs::FOGCOLOR};
  for (size_t i = 0; i < sizeof rsl / sizeof rsl[0]; ++i)
    j += (i ? ",\"" : "\"") + rs_name(rsl[i]) + "\":" + std::to_string(s.rs[rsl[i]]);
  j += "},\"tss0\":{";
  const uint32_t tl[] = {tss::COLOROP, tss::COLORARG1, tss::COLORARG2, tss::ALPHAOP, tss::ALPHAARG1, tss::ALPHAARG2};
  for (size_t i = 0; i < 6; ++i) j += (i ? ",\"" : "\"") + tss_name(tl[i]) + "\":" + std::to_string(s.tss[0][tl[i]]);
  j += "},\"world\":[";
  for (int i = 0; i < 16; ++i) j += (i ? "," : "") + f(s.world.m[i]);
  j += "],\"material\":{\"diffuse\":" + c4(s.material.diffuse) + ",\"ambient\":" + c4(s.material.ambient) +
       ",\"emissive\":" + c4(s.material.emissive) + "},\"lights\":[";
  bool first = true;
  for (uint32_t i = 0; i < kMaxLights; ++i) {
    if (!s.light_on[i]) continue;
    const Light8& l = s.lights[i];
    j += std::string(first ? "" : ",") + "{\"type\":" + std::to_string(l.type) + ",\"diffuse\":" + c4(l.diffuse) +
         ",\"ambient\":" + c4(l.ambient) + ",\"position\":[" + f(l.position[0]) + "," + f(l.position[1]) + "," +
         f(l.position[2]) + "],\"direction\":[" + f(l.direction[0]) + "," + f(l.direction[1]) + "," + f(l.direction[2]) +
         "],\"range\":" + f(l.range) + ",\"att\":[" + f(l.attenuation0) + "," + f(l.attenuation1) + "," + f(l.attenuation2) + "]}";
    first = false;
  }
  j += "],\"has_vcol\":" + std::string(has_vcol ? "true" : "false") + ",\"vcol\":" + c4(vcol) + ",\"p\":[" + f(p[0]) + "," +
       f(p[1]) + "," + f(p[2]) + "],\"n\":[" + f(n[0]) + "," + f(n[1]) + "," + f(n[2]) + "],\"d\":" + c4(d) +
       ",\"texel\":" + c4(texel) + ",\"bound\":" + (bound ? "true" : "false") + ",\"w\":" + f(w) +
       ",\"out_vertex\":" + c4(v) + ",\"out_kept\":" + (kept ? "true" : "false") + ",\"out_fragment\":" + (kept ? c4(o) : "null") + "}";
  std::puts(j.c_str());
}
}  // namespace

int main(int argc, char** argv) {
  const uint32_t n = argc > 1 ? static_cast<uint32_t>(std::strtoul(argv[1], nullptr, 10)) : 4000;
  g_rng = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1;
  // 1. gpu_ffp's cases with gpu_ffp's inputs: vertex 0 of the triangle, normal (0,0,-1), white texel, w = 1, the fragment
  //    fed with the case's own lit vertex colour (as the GPU test does).
  float verts[3][12];
  test::ffp_triangle(verts);
  const float nrm[3] = {0, 0, -1};
  const Color4 vc{test::kFfpVcol[0], test::kFfpVcol[1], test::kFfpVcol[2], test::kFfpVcol[3]};
  for (const test::FfpCase& c : test::gpu_ffp_cases()) {
    const Color4 d = reference_vertex_color(c.s, c.has_vcol, vc, verts[0], nrm);
    emit(c.what, c.s, c.has_vcol, vc, verts[0], nrm, d, Color4{1, 1, 1, 1}, true, 1.0f);
  }
  // 2. Seeded random samples over every state the reference reads. World matrices are rigid (rotation about a random
  //    axis + translation): the area path only builds RotX * translation.
  const uint32_t ops[] = {kTopDisable, kTopSelectArg1, kTopSelectArg2, kTopModulate};
  const uint32_t args[] = {kTaDiffuse, kTaCurrent, kTaTexture, kTaTFactor};
  for (uint32_t k = 0; k < n; ++k) {
    State s = default_state();
    s.rs[rs::LIGHTING] = rnd(4) != 0;
    s.rs[rs::COLORVERTEX] = rnd(4) != 0;
    s.rs[rs::DIFFUSEMATERIALSOURCE] = rnd(3);
    s.rs[rs::AMBIENT] = argb();
    s.material.diffuse = col();
    s.material.ambient = col();
    s.material.emissive = Color4{uni(0, 0.3f), uni(0, 0.3f), uni(0, 0.3f), 1};
    const float ax[3] = {uni(-1, 1), uni(-1, 1), uni(-1, 1)};
    const float len = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]) + 1e-6f;
    const float u[3] = {ax[0] / len, ax[1] / len, ax[2] / len}, th = uni(-3.14159f, 3.14159f), cs = std::cos(th), sn = std::sin(th);
    for (int i = 0; i < 3; ++i)  // Rodrigues, row-vector convention: row i is the image of basis vector i
      for (int j = 0; j < 3; ++j) {
        const float e = (i == j ? cs : 0.0f) + (1 - cs) * u[i] * u[j];
        const int ijk = (i + 1) % 3 == j ? 1 : (j + 1) % 3 == i ? -1 : 0;
        const int other = 3 - i - j;
        s.world.m[i * 4 + j] = e + (ijk ? ijk * sn * u[other] : 0.0f);
      }
    s.world.m[12] = uni(-20, 20);
    s.world.m[13] = uni(-20, 20);
    s.world.m[14] = uni(-20, 20);
    s.world.m[15] = 1;
    for (uint32_t i = 0; i < kMaxLights; ++i) {
      if (rnd(3) != 0) continue;
      Light8& l = s.lights[i];
      l.type = rnd(2) ? kLightPoint : kLightDirectional;
      l.diffuse = col();
      l.ambient = Color4{uni(0, 0.3f), uni(0, 0.3f), uni(0, 0.3f), 1};
      for (int a = 0; a < 3; ++a) {
        l.position[a] = uni(-30, 30);
        l.direction[a] = uni(-1, 1);
      }
      l.range = uni(0, 60);
      l.attenuation0 = rnd(2) ? 0.0f : uni(0, 1);
      l.attenuation1 = uni(0.01f, 1);
      l.attenuation2 = rnd(2) ? 0.0f : uni(0, 0.05f);
      s.light_on[i] = true;
    }
    s.rs[rs::TEXTUREFACTOR] = argb();
    s.tss[0][tss::COLOROP] = ops[rnd(4)];
    s.tss[0][tss::COLORARG1] = args[rnd(4)];
    s.tss[0][tss::COLORARG2] = args[rnd(4)];
    s.tss[0][tss::ALPHAOP] = ops[rnd(4)];
    s.tss[0][tss::ALPHAARG1] = args[rnd(4)];
    s.tss[0][tss::ALPHAARG2] = args[rnd(4)];
    s.rs[rs::ALPHATESTENABLE] = rnd(2);
    s.rs[rs::ALPHAFUNC] = 1 + rnd(8);
    s.rs[rs::ALPHAREF] = rnd(256);
    s.rs[rs::ALPHABLENDENABLE] = rnd(2);
    s.rs[rs::FOGENABLE] = rnd(2);
    s.rs[rs::FOGTABLEMODE] = rnd(4);
    s.rs[rs::FOGVERTEXMODE] = rnd(4);
    const float st = uni(-50, 1500);
    s.rs[rs::FOGSTART] = float_to_bits(st);
    s.rs[rs::FOGEND] = float_to_bits(st + uni(0.5f, 3000));
    s.rs[rs::FOGDENSITY] = float_to_bits(uni(0, 0.2f));
    s.rs[rs::FOGCOLOR] = argb();
    const Color4 vcol = col();
    const float p[3] = {uni(-10, 10), uni(-10, 10), uni(-10, 10)};
    float nn[3] = {uni(-1, 1), uni(-1, 1), uni(-1, 1)};
    const float nl = std::sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]) + 1e-6f;
    for (float& x : nn) x /= nl;
    // Alpha inputs on the 8-bit grid half the time (the quantisation edge: TFACTOR and texel alphas are bytes).
    Color4 d = col(), texel = col();
    if (rnd(2)) {
      d.a = static_cast<float>(rnd(256)) / 255.0f;
      texel.a = static_cast<float>(rnd(256)) / 255.0f;
    }
    emit("random", s, rnd(2) != 0, vcol, p, nn, d, texel, rnd(4) != 0, uni(0, 3000));
  }
  return 0;
}
