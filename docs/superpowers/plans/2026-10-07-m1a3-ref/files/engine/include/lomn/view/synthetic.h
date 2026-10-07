// The A7 contract scene (spec M1 §4 A7): a tiny synthetic install, built in memory from bytes written here (no game
// data), that goes through the same path as a real area: blockfiles, OBJ/POS SLB tables, binary .x, DDS decode,
// texture resolution, mip generation, upload and draw.
//
// Root/Data/Levels/levt/levt.blk: red.dds (DXT1 4x4, solid red, alpha off), cut.dds (A8R8G8B8 8x8: u < 0.5 green
//   alpha 255, u >= 0.5 magenta alpha 0; alpha on)
// Root/Data/Levels/levt/test.blk:
//   quad.x  3 subsets (Z-up model space, faces in the y = 0 plane, so the world plane is z = 0 after RotX):
//           red quad x [-8,-1] (texture red), cut quad x [1,8] (texture cut, alpha pass), a third quad far off-screen
//           whose texture "nothere" does not exist (white fallback); vertex colours (1, 0.5, 1, 1); one normal
//           (0,-1,0) for every corner (MeshNormals)
//   blue.x  one quad x [-8,8], model z [-2,8] (world y), material diffuse (0,0,1,1), no texture, no vertex colours,
//           no normals
//   test_OBJ.slb  quad main @0; blue main @(0,0,-2); blue back @-50; quad trans @-60; blue trans @-70;
//                 blue nolight @-80; "nope" flags 0x4 (ignored: never loaded); "miss" main (no such model: warning)
//   test_POS.slb  a pickup record (0x60) then the start record (0x1000) at (0, 0, 10)
// M1a-3 additions: levt.blk also holds half.dds (A8R8G8B8 green, alpha 128), x_glass.dds and tfx.dds (DXT1) and
// textures.slb {tfx 0.5 0 0, red 1 0 0, cut 1 0.25 0}; Root/Data/Base.osi; Root/Data/Levels/levt/stat.blk (above).
// Camera for the scene: eye = start (eye_height 0), yaw 0 (looking along -Z), pitch 0, fov 90, near 0.1, far 100.
#pragma once
#include "lomn/fs.h"
#include "lomn/view/area.h"

namespace lomn::view {

constexpr const char* kSyntheticLevel = "levt";
constexpr const char* kSyntheticArea = "test";
// M1a-3 A9 state scene (same level, same camera): the A7 objects plus a plant, a mixed-material model, a trans id
// ending in 'w', two moveables (nearest-first order), a LIGHT table, the level's textures.slb and a Base.osi class
// "lego_area_statt" (fog + cave lighting).
constexpr const char* kSyntheticStateArea = "stat";

// Mounts the scene's blockfiles into `fs`. False if a mount fails.
bool mount_synthetic_install(fs::MemoryFileSystem& fs);
// The camera settings the expected call log and pixel checks were written for.
CameraSettings synthetic_camera();

}  // namespace lomn::view
