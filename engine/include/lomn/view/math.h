// 4x4 matrices in the game's convention (D3D8 / D3DX): row-major storage m[row*4 + col], ROW vectors (v' = v * M),
// translation in _41.._43 (m[12..14]); a product a*b applies a first. Uploaded unchanged to WGSL, the same 16 floats
// read as a column-major mat4x4f, so `M * v` in WGSL equals the D3D `v * M` (no transpose anywhere).
//
// Determinism (native == wasm32 call logs): trig and sqrt are evaluated in double through <cmath> and rounded once to
// float; every other step is plain float arithmetic with -ffp-contract=off (inherited from lomn_core). D3DX itself
// computes in float, so the last bit may differ from the original; bit-faithful D3DX helpers are a later item
// (parent spec §4: "validated against recorded outputs").
#pragma once
#include <cstdint>

namespace lomn::view {

struct Mat4 {
  float m[16] = {};
};

Mat4 mat_identity();
Mat4 mat_mul(const Mat4& a, const Mat4& b);
// D3DUtil_SetRotateXMatrix: _22 = cos, _23 = sin, _32 = -sin, _33 = cos. With the game's -1.5707982 this maps a
// model-space (x, y, z) to (x, z, -y) up to ~2e-6: the .x files are Z-up, the world is Y-up.
Mat4 mat_rotation_x(float radians);
Mat4 mat_translation(float x, float y, float z);
// D3DXMatrixPerspectiveFovRH(fovy, aspect, zn, zf), documented form:
//   yScale = cot(fovy/2), xScale = yScale / aspect
//   [xScale 0 0 0; 0 yScale 0 0; 0 0 zf/(zn-zf) -1; 0 0 zn*zf/(zn-zf) 0]
// A view-space point at z = -zn gets z_ndc = 0 and at z = -zf gets z_ndc = 1: D3D's [0,1] depth range, which is also
// WebGPU's, so the matrix is used unchanged (no [-1,1] remap as for OpenGL).
Mat4 perspective_fov_rh(float fovy_radians, float aspect, float zn, float zf);
// D3DXMatrixLookAtRH: z = normalize(eye - at), x = normalize(cross(up, z)), y = cross(z, x);
//   [x.x y.x z.x 0; x.y y.y z.y 0; x.z y.z z.z 0; -dot(x,eye) -dot(y,eye) -dot(z,eye) 1]
// Kept as D3DX computes it: when eye - at is parallel to up, x is the zero vector (degenerate view). make_camera keeps
// the pitch within +-kMaxPitchDeg (area.h) so that never happens for the area camera.
Mat4 look_at_rh(const float eye[3], const float at[3], const float up[3]);
// out = (p, 1) * M (homogeneous, 4 floats).
void transform_point(const Mat4& m, const float p[3], float out[4]);

}  // namespace lomn::view
