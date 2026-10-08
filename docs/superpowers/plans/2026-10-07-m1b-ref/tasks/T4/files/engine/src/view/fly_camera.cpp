#include "lomn/view/fly_camera.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace lomn::view {
namespace {
constexpr double kDeg = 3.14159265358979323846 / 180.0;
float wrap_yaw(double y) {
  y = std::fmod(y, 360.0);
  if (y > 180.0) y -= 360.0;
  if (y <= -180.0) y += 360.0;
  return static_cast<float>(y);
}
}  // namespace

void FlyCamera::reset(const Camera& start) {
  std::memcpy(eye_, start.eye, sizeof eye_);
  yaw_ = start.yaw_deg;
  pitch_ = start.pitch_deg;
  moved_ = false;
}

void FlyCamera::update(const FlyInput& in, double dt_s, const FlySpeeds& sp) {
  if (!std::isfinite(dt_s)) dt_s = 0;
  dt_s = std::max(0.0, std::min(0.25, dt_s));
  const bool look = std::isfinite(in.look_dx) && std::isfinite(in.look_dy) && (in.look_dx != 0 || in.look_dy != 0);
  if (look) {
    yaw_ = wrap_yaw(static_cast<double>(yaw_) + static_cast<double>(in.look_dx) * sp.deg_per_pixel);
    const double p = static_cast<double>(pitch_) - static_cast<double>(in.look_dy) * sp.deg_per_pixel;
    pitch_ = static_cast<float>(std::max<double>(-kMaxPitchDeg, std::min<double>(kMaxPitchDeg, p)));
    moved_ = true;
  }
  const float f = (in.forward ? 1.0f : 0.0f) - (in.back ? 1.0f : 0.0f);
  const float r = (in.right ? 1.0f : 0.0f) - (in.left ? 1.0f : 0.0f);
  const float u = (in.up ? 1.0f : 0.0f) - (in.down ? 1.0f : 0.0f);
  if ((f != 0 || r != 0 || u != 0) && dt_s > 0) {
    const double step = static_cast<double>(sp.units_per_s) * (in.fast ? sp.fast_multiplier : 1.0f) * dt_s;
    const double y = yaw_ * kDeg, p = pitch_ * kDeg;
    // make_camera's forward: (sin y cos p, sin p, -cos y cos p); right = forward x up projected on xz: (cos y, 0, sin y).
    const double fwd[3] = {std::sin(y) * std::cos(p), std::sin(p), -std::cos(y) * std::cos(p)};
    const double right[3] = {std::cos(y), 0.0, std::sin(y)};
    for (int k = 0; k < 3; ++k)
      eye_[k] = static_cast<float>(eye_[k] + step * (f * fwd[k] + r * right[k] + (k == 1 ? u : 0.0f)));
    moved_ = true;
  }
}

CameraSettings FlyCamera::settings(const CameraSettings& base) const {
  if (!moved_) return base;
  CameraSettings s = base;
  s.eye_set = true;
  std::memcpy(s.eye, eye_, sizeof s.eye);
  s.yaw_set = true;
  s.yaw_deg = yaw_;
  s.pitch_deg = pitch_;
  return s;
}

}  // namespace lomn::view
