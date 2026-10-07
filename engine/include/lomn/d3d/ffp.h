// M1a-3: compiles a D3D8 fixed-function State (device8.h) into rhi objects.
//
// Choice (measured, see REPORT): ONE WGSL uber-shader whose per-draw uniform block carries every fixed-function value
// that WGSL can branch on (lighting, vertex-colour source, texture-stage ops/args, TFACTOR, alpha test, fog, texture
// transform, up to 8 lights), plus one rhi pipeline per PipelineKey = the raster/blend state WebGPU bakes into a
// pipeline (blend, depth write, depth compare, cull, depth bias). Real areas need 2..4 pipelines (lomn_view metrics
// "pipelines"), so generating WGSL per state key would only multiply shader modules.
//
// Formulas (D3D8 fixed function, DirectX 8.1 SDK "Mathematics of Lighting" / "Fog formulas"):
//  LIGHTING = FALSE: diffuse = the vertex DIFFUSE colour when the vertex has one, else opaque white; the material and
//    the lights are ignored.
//  LIGHTING = TRUE (per vertex, world space; equal to D3D's camera space for the rigid world matrices used here):
//    Cd = vertex colour if COLORVERTEX && DIFFUSEMATERIALSOURCE == COLOR1 && the vertex has one, else material diffuse
//    Ca = material ambient (AMBIENTMATERIALSOURCE = MATERIAL), Ce = material emissive (EMISSIVEMATERIALSOURCE = MATERIAL)
//    per enabled light i: directional: L = -normalize(Direction), Atten = 1;
//                         point: d = |Pos - P|, skipped when d > Range, L = (Pos - P)/d, Atten = 1/(a0 + a1 d + a2 d^2)
//    rgb = clamp(Ce + Ca * (Ga + sum Atten_i La_i) + Cd * sum (Atten_i Ld_i max(N.L_i, 0)), 0, 1); alpha = Cd.a
//    Ga = RS AMBIENT; specular is off (SPECULARENABLE never set; LOCALVIEWER irrelevant); spot lights unsupported.
//  Stage 0 (stage 1 must be disabled): CURRENT = DIFFUSE = the colour above; ops DISABLE (pass CURRENT), SELECTARG1,
//    SELECTARG2, MODULATE on args DIFFUSE / CURRENT / TEXTURE / TFACTOR, separately for colour and alpha.
//  Alpha test: the stage alpha in 8 bits, round(a * 255), compared with ALPHAREF by ALPHAFUNC; failed pixels discard.
//  Fog (table or vertex mode, evaluated per pixel on the eye depth w = clip w, i.e. W-fog):
//    LINEAR f = clamp((end - w) / (end - start), 0, 1); EXP f = exp(-density w); EXP2 f = exp(-(density w)^2);
//    rgb = f * rgb + (1 - f) * FOGCOLOR; alpha untouched.
//  Texture transform: TEXTURETRANSFORMFLAGS COUNT2 on 2-D coordinates -> uv' = ((u, v, 1, 0) * TEXTURE0).xy: D3D8 pads a
//    2-D coordinate with 1 then 0, so the translation sits in _31/_32 and row 4 (_41/_42) is ignored (Wine's
//    compute_texture_matrix encodes the same; Task 8A B1 - this padded (u, v, 1, 1) before, adding _41/_42).
#pragma once
#include <cstdint>
#include <string>

#include "lomn/d3d/device8.h"
#include "lomn/rhi.h"

namespace lomn::d3d {

constexpr uint32_t kFfpUniformBytes = 1024;  // 256 floats; layout in ffp.cpp (pack_uniforms)
constexpr uint32_t kFfpVertexStride = 48;    // position @0, normal @12, uv @24, colour rgba @32 (view/area.h)

struct PipelineKey {
  bool blend = false;  // ALPHABLENDENABLE with SRCALPHA / INVSRCALPHA
  bool depth_write = true;
  rhi::CompareFunc depth_compare = rhi::CompareFunc::LessEqual;
  rhi::CullMode cull = rhi::CullMode::None;
  int32_t depth_bias = 0;
  bool operator<(const PipelineKey& o) const;
  std::string str() const;
};

// Bits of state the compiler cannot reproduce (counted per frame, listed in the metrics JSON).
enum Unsupported : uint32_t {
  kUnsupStage1 = 1u << 0,        // stage 1 COLOROP/ALPHAOP not DISABLE (caustics, 0x40000 modes)
  kUnsupBlendFactors = 1u << 1,  // blending with factors other than SRCALPHA / INVSRCALPHA
  kUnsupFill = 1u << 2,          // FILLMODE not SOLID
  kUnsupSpecular = 1u << 3,      // SPECULARENABLE
  kUnsupSpot = 1u << 4,          // an enabled spot light (treated as a point light)
  kUnsupTexOp = 1u << 5,         // a stage-0 op or argument outside DISABLE/SELECTARG1/SELECTARG2/MODULATE x
                                 // DIFFUSE/CURRENT/TEXTURE/TFACTOR
  kUnsupMipNone = 1u << 6,       // MIPFILTER NONE (rhi samplers always mipmap)
  kUnsupMaterialSource = 1u << 7,  // a *MATERIALSOURCE other than the D3D8 defaults
};

PipelineKey pipeline_key(const State& s, uint32_t* unsupported);
rhi::MeshPipelineDesc pipeline_desc(const PipelineKey& k);
rhi::SamplerDesc sampler_desc(const State& s, uint32_t* unsupported);

struct DrawInputs {
  bool vertex_has_diffuse = false;  // the model's vertex format carries DIFFUSE (MeshVertexColors)
  bool texture_bound = false;       // a texture is set on stage 0 (else the shader samples white)
};
// Fills the per-draw block (kFfpUniformBytes / 4 floats): wvp = world * view * proj, and everything above.
void pack_uniforms(const State& s, const DrawInputs& in, float out[kFfpUniformBytes / 4]);

const char* ffp_wgsl();

// CPU reference of the same formulas (tests, GPU probe expectations). p, n in model space; vcol the vertex colour.
Color4 reference_vertex_color(const State& s, bool has_vcol, const Color4& vcol, const float p[3], const float n[3]);
// Stage 0 texture coordinate after the texture transform (TEXTURETRANSFORMFLAGS / TEXTURE0; see the formulas above).
void reference_texcoord(const State& s, const float uv[2], float out[2]);
// Stage 0 + alpha test + fog on a vertex colour and texel; returns false when the alpha test discards. eye_w = clip w.
bool reference_fragment(const State& s, const Color4& diffuse, const Color4& texel, bool texture_bound, float eye_w, Color4& out);

// D3DCOLOR (ARGB) -> rgba floats, byte / 255.
Color4 argb_to_color(uint32_t argb);
// D3DXCOLOR -> D3DCOLOR channel conversion used for vertex colours: clamp to [0,1], then (uint8)(x * 255 + 0.5).
float quantize_unorm8(float x);

}  // namespace lomn::d3d
