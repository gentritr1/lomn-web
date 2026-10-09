// M2a-2 T9a: the "objects, model director, camera, HUD" native group (docs/osi/natives.md §2, §4; citations on each
// binding in src/osi/natives/objects.cpp). The host classes mirror the exe's classes where an argument check or a
// reference count needs them (GcToa derives from GcCharacter), and record the constructor arguments that M2a-3 renders.
// Render, audio and model side effects are not modelled (natives.md §1.1 Decision 6).
#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>

#include "lomn/osi/natives.h"

namespace lomn::osi {

// 'cnt1': the model-director key of the Toa ([0x782884], static init 0x482d43), the default main character
// ([0x7796f4], static init 0x43c193) and the controller id every GcToa is built with ([0x7cb1cc], static init 0x570be3).
constexpr uint32_t kIdCnt1 = 0x636e7431;

// ---- constructed objects (natives.md §2.4: G 0, the native constructs them) ----

// ScTileFont::ScTileFont 0x626b40 (3,848 B) is not ported (M2a-3 loads and renders the font): only its argument is kept.
class ScTileFont : public GameObject {
 public:
  ScTileFont(uint16_t type, std::string name) : GameObject(type, 0), name_(std::move(name)) {}
  const std::string& name() const { return name_; }

 private:
  std::string name_;
};

// ScTextObject::ScTextObject 0x610c40 (the 5-argument constructor sctextobject.create calls at 0x611d17): the font is held
// at +0x2c with one engine reference (inc at 0x610c92, skipped for a null font at 0x610c8d); the text is copied into the
// fixed string at +0x30 (ScFixedString::Init 0x401850 with npos, 0x610cce); x, y, z at +0x13c/+0x140/+0x144.
// ~ScTextObject 0x611070 (vtable 0x754758 slot 0 -> 0x6110c0) releases the font (dec at 0x611098).
class ScTextObject : public GameObject {
 public:
  ScTextObject(uint16_t type, float x, float y, float z, ScTileFont* font, std::string text)
      : GameObject(type, 0), x_(x), y_(y), z_(z), font_(font), text_(std::move(text)) {}
  ScTileFont* font() const { return font_; }
  const std::string& text() const { return text_; }
  float x() const { return x_; }
  float y() const { return y_; }
  float z() const { return z_; }
  void release_held(HostContext& ctx) override;

 private:
  float x_, y_, z_;
  ScTileFont* font_;
  std::string text_;
};

// GcBigSprite::LoadHudSprite 0x440460 news a GcBigSprite (0x44048a) and runs GcBigSprite::GcBigSprite 0x43f210 with
// (x0, y0, x1, y1, name, b5, b6); b5/b6 are the LOW BYTES of the native's arguments 5 and 6 (mov al, byte at 0x43daf4 /
// 0x43daf7). The constructor's file reads are M2a-3.
class GcBigSprite : public GameObject {
 public:
  GcBigSprite(uint16_t type, float x0, float y0, float x1, float y1, std::string name, uint8_t b5, uint8_t b6)
      : GameObject(type, 0), x0_(x0), y0_(y0), x1_(x1), y1_(y1), name_(std::move(name)), b5_(b5), b6_(b6) {}
  const std::string& name() const { return name_; }
  float x0() const { return x0_; }
  float y0() const { return y0_; }
  float x1() const { return x1_; }
  float y1() const { return y1_; }
  uint8_t b5() const { return b5_; }
  uint8_t b6() const { return b6_; }

 private:
  float x0_, y0_, x1_, y1_;
  std::string name_;
  uint8_t b5_, b6_;
};

// ScSLOSISLKCubicSpline::ScSLOSISLKCubicSpline 0x61a4d0: an SLK file looked up by name (GetSLKFile 0x61a580, call
// 0x61a514); a missing file still leaves the object (no null path in the wrapper 0x61a2e0 after a successful new).
class ScSLOSISLKCubicSpline : public GameObject {
 public:
  ScSLOSISLKCubicSpline(uint16_t type, std::string name) : GameObject(type, 0), name_(std::move(name)) {}
  const std::string& name() const { return name_; }

 private:
  std::string name_;
};

// ---- characters ----

// GcCharacter::GcCharacter 0x40b520: character id at +0xfc (0x40b728), controller id at +0x100 (0x40b73e); the
// constructor always builds a motion system (InitMotionSystem 0x40c600, called at 0x40beeb), whose id [ms+0x14] is the
// controller id [char+0x100] (read at 0x40c60b, stored by GcMotionSystem::GcMotionSystem at 0x5748ec).
class GcCharacter : public GameObject {
 public:
  GcCharacter(uint16_t type, uint32_t character_id, uint32_t controller_id)
      : GameObject(type, 0), character_id_(character_id), controller_id_(controller_id), node_id_(controller_id) {}
  uint32_t character_id() const { return character_id_; }    // +0xfc
  uint32_t controller_id() const { return controller_id_; }  // +0x100
  uint32_t node_id() const { return node_id_; }              // +0x10, the render node id (AddToa copies +0x100, 0x47e1eb)
  uint32_t motion_id() const { return controller_id_; }      // [ms+0x14]; no native on the port's path rebuilds it apart
  // [char+0x1d0], the holder: 0 from the constructor (0x40bbba); written by GcCharacter::SetHolder (gccharacter.setholder
  // 0x407a40 -> 0x410730, store at 0x410764), a stub in M2a-2 (review 9a m-2: modelling it changes MainCharacterID and the
  // SetupMoveable guard 0x574ef1, through main_character_id below). Raw, no reference.
  GcCharacter* holder = nullptr;

 private:
  uint32_t character_id_, controller_id_, node_id_;
};

// GcToa::GcToa 0x567d10: GcCharacter(name, id, [0x7cb1cc], [0x7cb1ec], [0x7cb1cc], p) at 0x567d7b (controller id 'cnt1');
// [toa+0x380] = 0xfa (0x567e27) is the ammo it copies to the saver (0x56898b) and the second OnEvent argument (0x5689a8).
class GcToa : public GcCharacter {
 public:
  GcToa(uint16_t type, std::string name, uint32_t character_id)
      : GcCharacter(type, character_id, kIdCnt1), name_(std::move(name)) {}
  const std::string& name() const { return name_; }
  uint16_t ammo() const { return 0xfa; }  // [toa+0x380], its only writer is 0x567e27

 private:
  std::string name_;
};

// GcModelDirector and the globals AddToa / Show / SetupMoveable read or write.
struct ModelDirector {
  explicit ModelDirector(HostContext&) {}
  std::map<uint32_t, GcCharacter*> map;  // ScIdentifierMap 0x782a04: each entry holds one engine reference
  std::set<uint32_t> toa_node;           // ids of the children of the Toa node [0x8390a8] (FindChild by id, 0x47f9b6)
  std::set<uint32_t> mainguy_node;       // ids of the children of the main-guy node
  GcCharacter* ba_character = nullptr;   // GcBAGroup's character [0x83903c] (one engine reference, 0x54b344)
  GcToa* s_toa = nullptr;                // GcGame::sToa [0x7032b8], raw (written at 0x47e527)
  // sSaveToa [0x74749c], the ammo [0x7474c8] and the flag [0x73c5a8] live in SaverState / GameGlobals (directors.h): one
  // store per global, whichever native group writes or reads it (F7a, review 9a m-5).
  uint16_t toa_type = 0;                 // the game type of GcToa (gcmodeldirector.gettoa's ret_type), set at bind
  uint16_t character_type = 0;           // the game type of GcCharacter (gcmodeldirector.getcharacter's ret_type), set at bind
  // GcModelDirector::TrackUniqueBySlkType 0x47ffe0 (AddCharacter 0x47ec81) / UnTrack 0x480160 (Remove 0x47f48e): the
  // character ids of the live area-loaded characters, as far as GetUniqueIDForType 0x480260 needs them
  std::multiset<uint32_t> tracked;
  GcCharacter* get(uint32_t id) const;   // GcModelDirector::Get 0x480f70: the stored pointer or null (no reference)
};

// GcGame::MainCharacterID 0x43af90: no Toa ([0x7032b8] null, 0x43af91) or no holder ([Toa+0x1d0] null, 0x43afb8) ->
// [0x7796f4] (GameGlobals::main_character_default); else [[Toa+0x1d0]+0x100], the holder's controller id (0x43afcb).
uint32_t main_character_id(HostContext& ctx);

// GcCharacterLoader::LoadCharacters 0x53f3f0, one record: GcModelDirector::AddCharacter 0x47f040 (id from
// GetUniqueIDForType 0x480260) -> new GcCharacter -> AddCharacter 0x47ec30 -> Add 0x480eb0. Returns the new character when
// Add took it (InitializeCollisionPhysics 0x40da60 then runs, 0x47efef: the caller makes the SetUnderWater check), null
// when the key was taken (0x47ef98: the new character is dropped). *modelled is false for a record whose id the tracker
// already holds: GetUniqueIDForType then builds a numbered id (0x480362-0x480490), which the port does not model.
GcCharacter* add_area_character(HostContext& ctx, uint32_t record_id, bool* modelled);

// GcModelDirector::Remove 0x47f320 for one loader entry (CleanUpArea 0x5017d0, call 0x501830). An error in its OnEvent
// propagates (no catch on the chain).
void remove_area_character(HostContext& ctx, uint32_t id);

// ---- HUD ----

// GcHudDirector::Add 0x47b940: the element id is sNextHudElementID [0x83aa3c] (static init 0 from [0x839124],
// 0x47cb54-0x47cb64), post-incremented (0x47b957-0x47b958) on every add; each element is a new ScRenderNode that holds its
// object (inc at 0x4ac7d3) in the map 0x782458.
struct HudDirector {
  explicit HudDirector(HostContext&) {}
  uint32_t next_id = 0;
  std::map<uint32_t, GameObject*> elements;  // id -> the object its render node holds (null allowed)
};

// ---- camera ----

// GcLegoCamera state the camera natives read back: the AI mode byte [cam+0x3a1] (0 from the constructor, 0x44f6a0) and
// the spline [cam+0x3a4] (null from the constructor, 0x44f6ad; one engine reference while held). Keyed by the camera's
// game handle: the camera comes from gcviewport.getcamera (another group), and a handle is one live object (D7).
struct CameraState {
  uint8_t ai_mode = 0;
  GameObject* spline = nullptr;
};
struct Cameras {
  explicit Cameras(HostContext&) {}
  std::map<uint32_t, CameraState> by_handle;
};

}  // namespace lomn::osi
