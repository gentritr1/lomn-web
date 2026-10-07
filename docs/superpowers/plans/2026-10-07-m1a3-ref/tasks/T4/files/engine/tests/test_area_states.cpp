// M1a-3 area-loader additions (lomn/view/area.h): LIGHT.slb / default lights (GcLightLoader::LoadLights 0x00470010,
// GrSetDefaultLighting 0x00470560), the Base.osi fog / cave-lighting literals and textures.slb, on the synthetic state scene.
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "doctest.h"
#include "lomn/d3d/device8.h"
#include "lomn/fs.h"
#include "lomn/view/area.h"
#include "lomn/view/synthetic.h"

using namespace lomn;
using namespace lomn::d3d;

namespace {

view::AreaData synthetic_area() {
  fs::MemoryFileSystem mfs;
  REQUIRE(view::mount_synthetic_install(mfs));
  view::AreaData a;
  std::string err;
  REQUIRE_MESSAGE(view::load_area(mfs, view::kSyntheticLevel, view::kSyntheticStateArea, view::AreaOptions(), a, &err), err);
  return a;
}

}  // namespace

TEST_CASE("area: lights, fog and special textures come from LIGHT.slb, Base.osi and textures.slb") {
  view::AreaData a = synthetic_area();
  // LIGHT.slb: a point light and a directional one (+ the opposite "Shad" fill for the first directional).
  REQUIRE(a.lights.size() == 3);
  CHECK(a.lights_source == "light_slb");
  const Light8& pt = a.lights[0].light;
  CHECK(pt.type == kLightPoint);
  CHECK(pt.range == 20.0f);
  CHECK(pt.attenuation1 == doctest::Approx(1.0f / (0.15f * 20.0f)));
  // intensity 1.0 > 0.9 -> x 1.2; then SetCaveLighting(1) from the script: style 0 -> SetIntensity(0.25) on the base.
  CHECK(pt.diffuse.g == doctest::Approx(0.5f * 0.25f));
  CHECK(a.lights[1].name == "sun1");
  CHECK(a.lights[1].light.direction[1] == -1.0f);
  CHECK(a.lights[2].name == "Shad");
  CHECK(a.lights[2].light.direction[0] == -0.5f);
  CHECK(a.cave_lighting);
  CHECK(a.fog.source == "osi");
  CHECK(a.fog.color == 0xff808080u);
  CHECK(a.fog.start == 5.0f);  // GcAreaDirector::SetFog negates the script's -5 / -50
  CHECK(a.fog.end == 50.0f);
  CHECK(a.fog.density == doctest::Approx(0.1f));
  CHECK(a.script_class == "lego_area_statt");
  CHECK(a.special_textures == 3);
  CHECK(a.special_matches == 2);  // "tfx" (trans) and "cut" (scroll); "red" is (1, 0, 0) and skipped
  CHECK(a.level_type == uint8_t('t' - '0'));
}

TEST_CASE("area: default lighting by level type (GrSetDefaultLighting)") {
  fs::MemoryFileSystem mfs;
  REQUIRE(view::mount_synthetic_install(mfs));
  std::vector<uint8_t> blk;
  REQUIRE(mfs.read("Root/Data/Levels/levt/levt.blk", blk) == fs::Error::None);
  for (const char* lev : {"lev1", "lev4"}) {
    fs::MemoryFileSystem m2;
    REQUIRE(m2.mount(std::string("Root/Data/Levels/") + lev + "/" + lev + ".blk", blk));
    std::vector<uint8_t> area;
    REQUIRE(mfs.read("Root/Data/Levels/levt/test.blk", area) == fs::Error::None);
    REQUIRE(m2.mount(std::string("Root/Data/Levels/") + lev + "/quad.blk", area));  // no quad_light.slb in it
    view::AreaData a;
    std::string err;
    REQUIRE_MESSAGE(view::load_area(m2, lev, "quad", view::AreaOptions(), a, &err), err);
    CHECK(a.lights_source == "default");
    if (std::string(lev) == "lev1") {
      REQUIRE(a.lights.size() == 1);  // "LmbA": ambient 120/255 grey
      CHECK(a.lights[0].name == "LmbA");
      CHECK(a.lights[0].light.ambient.r == doctest::Approx(0.47058824f));
      CHECK(a.lights[0].light.diffuse.r == 0.0f);
    } else {
      REQUIRE(a.lights.size() == 2);  // SunL white x 1.2, Shad blue-grey (level type 4)
      CHECK(a.lights[0].name == "SunL");
      CHECK(a.lights[0].light.diffuse.r == doctest::Approx(1.2f));
      CHECK(a.lights[0].light.direction[0] == doctest::Approx(0.85570401f));
      CHECK(a.lights[1].light.diffuse.b == doctest::Approx(0.54901963f * 1.2f));
    }
  }
}

