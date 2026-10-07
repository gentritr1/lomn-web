#include "lomn/view/math.h"

#include <cmath>

namespace lomn::view {

Mat4 mat_identity() {
  Mat4 r;
  r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
  return r;
}

Mat4 mat_mul(const Mat4& a, const Mat4& b) {
  Mat4 r;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      float s = 0.0f;
      for (int k = 0; k < 4; ++k) s += a.m[i * 4 + k] * b.m[k * 4 + j];
      r.m[i * 4 + j] = s;
    }
  return r;
}

Mat4 mat_rotation_x(float radians) {
  const float c = static_cast<float>(std::cos(static_cast<double>(radians)));
  const float s = static_cast<float>(std::sin(static_cast<double>(radians)));
  Mat4 r = mat_identity();
  r.m[5] = c;
  r.m[6] = s;
  r.m[9] = -s;
  r.m[10] = c;
  return r;
}

Mat4 mat_translation(float x, float y, float z) {
  Mat4 r = mat_identity();
  r.m[12] = x;
  r.m[13] = y;
  r.m[14] = z;
  return r;
}

Mat4 perspective_fov_rh(float fovy, float aspect, float zn, float zf) {
  const float ys = static_cast<float>(1.0 / std::tan(static_cast<double>(fovy) / 2.0));
  const float xs = ys / aspect;
  Mat4 r;
  r.m[0] = xs;
  r.m[5] = ys;
  r.m[10] = zf / (zn - zf);
  r.m[11] = -1.0f;
  r.m[14] = zn * zf / (zn - zf);
  return r;
}

namespace {
void normalize3(float v[3]) {
  const float len = static_cast<float>(std::sqrt(static_cast<double>(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])));
  if (len > 0.0f) {
    v[0] /= len;
    v[1] /= len;
    v[2] /= len;
  }
}
void cross3(const float a[3], const float b[3], float o[3]) {
  o[0] = a[1] * b[2] - a[2] * b[1];
  o[1] = a[2] * b[0] - a[0] * b[2];
  o[2] = a[0] * b[1] - a[1] * b[0];
}
float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
}  // namespace

Mat4 look_at_rh(const float eye[3], const float at[3], const float up[3]) {
  float z[3] = {eye[0] - at[0], eye[1] - at[1], eye[2] - at[2]};
  normalize3(z);
  float x[3];
  cross3(up, z, x);
  normalize3(x);
  float y[3];
  cross3(z, x, y);
  Mat4 r;
  for (int i = 0; i < 3; ++i) {
    r.m[i * 4 + 0] = x[i];
    r.m[i * 4 + 1] = y[i];
    r.m[i * 4 + 2] = z[i];
  }
  r.m[12] = -dot3(x, eye);
  r.m[13] = -dot3(y, eye);
  r.m[14] = -dot3(z, eye);
  r.m[15] = 1.0f;
  return r;
}

void transform_point(const Mat4& m, const float p[3], float out[4]) {
  for (int j = 0; j < 4; ++j) out[j] = p[0] * m.m[j] + p[1] * m.m[4 + j] + p[2] * m.m[8 + j] + m.m[12 + j];
}

}  // namespace lomn::view
