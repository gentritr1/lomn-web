// M1b: the viewer's free-fly camera (lomn/view/fly_camera.h).
#include <cmath>
#include <cstring>

#include "doctest.h"
#include "lomn/fs.h"
#include "lomn/view/fly_camera.h"
#include "lomn/view/synthetic.h"

using namespace lomn::view;

namespace {
Camera start_camera(float yaw, float pitch) {
  Camera c;
  c.eye[0] = 1;
  c.eye[1] = 2;
  c.eye[2] = 3;
  c.yaw_deg = yaw;
  c.pitch_deg = pitch;
  return c;
}
}  // namespace

TEST_CASE("fly camera: untouched it returns the base settings, so the first view is the default camera bit for bit") {
  lomn::fs::MemoryFileSystem m;
  REQUIRE(mount_synthetic_install(m));
  AreaData a;
  std::string err;
  REQUIRE(load_area(m, kSyntheticLevel, kSyntheticArea, AreaOptions(), a, &err));
  const Camera base = make_camera(a, CameraSettings(), 16.0f / 9.0f);
  FlyCamera f;
  f.reset(base);
  FlyInput none;
  f.update(none, 0.016);
  CHECK_FALSE(f.moved());
  const Camera again = make_camera(a, f.settings(CameraSettings()), 16.0f / 9.0f);
  CHECK(std::memcmp(again.view.m, base.view.m, sizeof base.view.m) == 0);
  CHECK(std::memcmp(again.proj.m, base.proj.m, sizeof base.proj.m) == 0);
  CHECK(again.eye_source == "pos_start");
  // After a move the overrides take over: eye exactly the fly eye (no eye_height added), yaw/pitch as flown.
  FlyInput w;
  w.forward = true;
  f.update(w, 0.5);
  REQUIRE(f.moved());
  const Camera moved = make_camera(a, f.settings(CameraSettings()), 16.0f / 9.0f);
  CHECK(moved.eye_source == "cli");
  for (int k = 0; k < 3; ++k) CHECK(moved.eye[k] == f.eye()[k]);
  CHECK(moved.yaw_deg == doctest::Approx(base.yaw_deg));
}

TEST_CASE("fly camera: WASD moves along make_camera's forward/right, Q/E (up/down) along world Y, Shift is faster") {
  FlyCamera f;
  f.reset(start_camera(0, 0));  // looking along -Z
  FlyInput in;
  in.forward = true;
  f.update(in, 1.0 / 8);  // 12 u/s * 0.125 s
  CHECK(f.eye()[2] == doctest::Approx(3 - 1.5));
  CHECK(f.eye()[0] == doctest::Approx(1));
  in = FlyInput();
  in.right = true;
  in.fast = true;
  f.update(in, 0.1);  // +X at 60 u/s
  CHECK(f.eye()[0] == doctest::Approx(1 + 6));
  in = FlyInput();
  in.up = true;
  f.update(in, 0.25);
  CHECK(f.eye()[1] == doctest::Approx(2 + 3));
  // yaw 90 looks along +X: forward moves +X.
  f.reset(start_camera(90, 0));
  in = FlyInput();
  in.forward = true;
  f.update(in, 0.25);
  CHECK(f.eye()[0] == doctest::Approx(1 + 3));
  CHECK(f.eye()[2] == doctest::Approx(3).epsilon(1e-5));
}

TEST_CASE("fly camera: mouse look, pitch clamp, dt clamp, non-finite input ignored") {
  FlyCamera f;
  f.reset(start_camera(170, 0));
  FlyInput in;
  in.look_dx = 100;  // 15 degrees: wraps past 180
  in.look_dy = -100; // up 15 degrees
  f.update(in, 0.016);
  CHECK(f.yaw_deg() == doctest::Approx(-175));
  CHECK(f.pitch_deg() == doctest::Approx(15));
  in.look_dx = 0;
  in.look_dy = -10000;
  f.update(in, 0.016);
  CHECK(f.pitch_deg() == doctest::Approx(kMaxPitchDeg));
  f.reset(start_camera(0, 0));
  in = FlyInput();
  in.forward = true;
  f.update(in, 10.0);  // a stalled tab: at most 0.25 s of movement
  CHECK(f.eye()[2] == doctest::Approx(3 - 3));
  f.reset(start_camera(0, 0));
  f.update(in, std::nan(""));
  CHECK_FALSE(f.moved());
  in = FlyInput();
  in.look_dx = std::nanf("");
  f.update(in, 0.016);
  CHECK_FALSE(f.moved());
}
