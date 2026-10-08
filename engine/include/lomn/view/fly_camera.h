// M1b viewer: free-fly camera (mouse look + WASD) on top of make_camera (area.h). Until the player moves, settings()
// returns the base settings unchanged, so the first frames are the POS-start / exe-default view bit for bit (the same
// view lomn_view renders natively). Once moved it overrides eye, yaw and pitch (eye_set / yaw_set, eye_height unused).
// Conventions are make_camera's: yaw 0 looks along -Z, 90 along +X; positive pitch looks up; Y is up.
#pragma once
#include "lomn/view/area.h"

namespace lomn::view {

struct FlyInput {
  bool forward = false, back = false, left = false, right = false, up = false, down = false, fast = false;
  float look_dx = 0, look_dy = 0;  // pointer movement in CSS pixels since the last update (right / down positive)
};

struct FlySpeeds {
  float units_per_s = 12.0f;      // walking speed in world units (area extents are hundreds of units)
  float fast_multiplier = 5.0f;   // Shift
  float deg_per_pixel = 0.15f;    // mouse look
};

class FlyCamera {
 public:
  // Starts from the camera make_camera produced for the base settings (eye, yaw and clamped pitch).
  void reset(const Camera& start);
  // dt_s is clamped to [0, 0.25] s so a stalled tab does not teleport the camera. Non-finite inputs are ignored.
  void update(const FlyInput& in, double dt_s, const FlySpeeds& sp = FlySpeeds());
  bool moved() const { return moved_; }
  // base unchanged until the first movement; then base with eye/yaw/pitch overridden.
  CameraSettings settings(const CameraSettings& base) const;
  const float* eye() const { return eye_; }
  float yaw_deg() const { return yaw_; }
  float pitch_deg() const { return pitch_; }

 private:
  float eye_[3] = {0, 0, 0};
  float yaw_ = 0, pitch_ = 0;
  bool moved_ = false;
};

}  // namespace lomn::view
