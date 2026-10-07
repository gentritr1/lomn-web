// M1b: the viewer's free-fly camera (lomn/view/fly_camera.h).
#include <cmath>
#include <cstring>
#include <limits>

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

// T8 P2: the first touch must not nudge the view. The Approx check above allows ~3e-4 deg of drift; here a look delta far
// below the float resolution of yaw/pitch (denorm_min, FLT_MIN, 1e-9 px: all < half an ulp of the angle) flips moved()
// but leaves yaw, pitch and eye bit-identical, so view and proj of the first frame are bit-equal to the default view. The
// bases use small and awkward angles (0.3 has an ulp of 3e-8, so a seed that adds even 1e-6 shows) and a POS-start eye
// that is not re-lifted by eye_height once the overrides take over.
TEST_CASE("fly camera: a sub-ulp first look delta moves the state but not one bit of the view") {
  lomn::fs::MemoryFileSystem m;
  REQUIRE(mount_synthetic_install(m));
  AreaData a;
  std::string err;
  REQUIRE(load_area(m, kSyntheticLevel, kSyntheticArea, AreaOptions(), a, &err));
  const float bases[][2] = {{0.3f, -10.3f}, {33.3f, 12.7f}, {-170.1f, 0.07f}, {179.99f, 45.5f}};
  const float deltas[] = {std::numeric_limits<float>::denorm_min(), std::numeric_limits<float>::min(), 1e-9f};
  for (const auto& b : bases) {
    CameraSettings bs;
    bs.yaw_set = true;
    bs.yaw_deg = b[0];
    bs.pitch_deg = b[1];
    const Camera base = make_camera(a, bs, 16.0f / 9.0f);
    for (float d : deltas) {
      for (int axis = 0; axis < 3; ++axis) {  // dx only, dy only, both (negative dy too: pitch moves the other way)
        INFO("yaw " << b[0] << " pitch " << b[1] << " delta " << d << " axis " << axis);
        FlyCamera f;
        f.reset(base);
        FlyInput in;
        if (axis != 1) in.look_dx = d;
        if (axis != 0) in.look_dy = (axis == 2 ? -d : d);
        f.update(in, 0.016);
        REQUIRE(f.moved());  // the touch counts: the overrides are now in force
        const float fy = f.yaw_deg(), fp = f.pitch_deg();
        CHECK(std::memcmp(&fy, &base.yaw_deg, sizeof(float)) == 0);
        CHECK(std::memcmp(&fp, &base.pitch_deg, sizeof(float)) == 0);
        const Camera first = make_camera(a, f.settings(bs), 16.0f / 9.0f);
        CHECK(first.eye_source == "cli");
        CHECK(std::memcmp(first.eye, base.eye, sizeof first.eye) == 0);
        CHECK(std::memcmp(first.view.m, base.view.m, sizeof base.view.m) == 0);
        CHECK(std::memcmp(first.proj.m, base.proj.m, sizeof base.proj.m) == 0);
      }
    }
  }
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
