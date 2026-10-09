// M2a-2 T8a: the "directors, saver and Beta 1.5.0 mods" native group (docs/osi/natives.md §2, §4, §5, §9; citations on
// each binding in src/osi/natives/directors.cpp): the engine-owned singletons (main view, its camera, key mapper, MP3
// player), the area director (current level and area, the area load with its three swallowing LoadArea blocks, the
// script fluff strings), the HUD director's cinematics (D1), the GcSaver fields, the INI's CheatMenu, and BetterSaver's
// boolean map. Every value is cited by instruction address in directors.cpp; INFERRED marks a reading not run to a
// conclusion.
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "lomn/osi/natives.h"
#include "lomn/sage/blockchain.h"
#include "lomn/sage/slb.h"

namespace lomn::osi {

// The null ScIdentifier '????' (.data 0x70b3bc / 0x70b3c0; the static init stores it into [0x83902c] at 0x48db4b and
// into [0x83b0b4] at 0x48db5f, GcAreaDirector::Init again at 0x48771e).
constexpr uint32_t kNullId = 0x3f3f3f3fu;
// 'lbld', the dword [0x734f10] (.data bytes 64 6c 62 6c) the GcMoveableObj ctor compares its id with (0x544065).
constexpr uint32_t kLbld = 0x6c626c64u;

// ---- the engine-owned singletons (natives.md §2.4: each is held by one engine holder at the call, G 1) ----
class GcViewPort;
class GcLegoCamera : public GameObject {
 public:
  explicit GcLegoCamera(uint16_t type) : GameObject(type, 1) {}  // the camera node [vp+0x30] holds it (G 1, INFERRED)
};
class GcViewPort : public GameObject {
 public:
  explicit GcViewPort(uint16_t type) : GameObject(type, 1) {}  // ScScreen's view list (AddViewPort 0x43a85a; G 1, INFERRED)
  GcLegoCamera* camera = nullptr;                              // [[vp+0x30]+0x1c] (GcViewPort::GetCamera 0x448080)
};
class GcKeyMapper : public GameObject {
 public:
  explicit GcKeyMapper(uint16_t type) : GameObject(type, 1) {}  // the global ScAutoRefPtr [0x83aa98] (+1 at 0x439583)
};
class GcMP3Player : public GameObject {
 public:
  explicit GcMP3Player(uint16_t type) : GameObject(type, 1) {}  // the global ScAutoRefPtr [0x83879c] (+1 at 0x43a40c)
};

// One instance of each per host context, created on first use under the type id of the native that first returns it
// (its CreateVariant overload, natives.md §2.1). They are never freed: the engine's holder keeps the count above 0.
class GameSingletons {
 public:
  explicit GameSingletons(HostContext& c) : ctx_(c) {}
  GcViewPort* main_view(uint16_t type);
  GcLegoCamera* camera(GcViewPort* vp, uint16_t type);
  GcKeyMapper* key_mapper(uint16_t type);
  GcMP3Player* mp3_player(uint16_t type);

 private:
  HostContext& ctx_;
  GcViewPort* view_ = nullptr;
  GcKeyMapper* keys_ = nullptr;
  GcMP3Player* mp3_ = nullptr;
};

// ---- process-global game state read by more than one native group ----
struct GameGlobals {
  explicit GameGlobals(HostContext&) {}
  // GcGame::MainCharacterID 0x43af90: [0x7796f4] = 'cnt1' (stored at 0x43c193) while [0x7032b8] (the Toa AddToa stores,
  // 0x47e527) is 0. The Toa route (0x43afb2-0x43afd1) belongs to the model-director group, which updates this field.
  uint32_t main_character_id = 0x636e7431u;
  // byte [0x73c5a8]: 0 in .data; its only writer, GcMotionSystem::SetGlobalUnderWater 0x589264, is no M2a-2 native.
  bool global_under_water = false;
};

// GcSaver's fields (natives.md §9.1; the .data bytes, then GcSaver::ResetData 0x5d5490 run by InitData before Setup).
struct SaverState {
  explicit SaverState(HostContext&) {}
  uint32_t mask = kNullId;          // dword [0x7474a0]
  uint8_t glyph = 0;                // byte [0x7474a4]
  uint8_t health = 0;               // byte [0x7474a8]
  uint8_t energy = 0;               // byte [0x7474ac]
  uint16_t ammo = 0;                // u16 [0x7474c8] (the GcToa ctor stores [toa+0x380] = 250 there, 0x56898b)
  uint8_t level = 0;                // byte [0x74748c] (ResetData 0x5d5493; GcSaver::SetLevel 0x5d5740)
  uint32_t area = kNullId;          // dword [0x747490] (ResetData 0x5d549a; GcSaver::SetCurrentArea 0x5d5506)
  uint32_t condition = 0;           // dword [0x8391a0], the legacy condition bits (ResetData 0x5d54ec)
  std::array<uint8_t, 7> quests{};  // bytes [0x83b37f + level], GcSaver::SetQuest 0x5d5e20 (bits 1/2/4)
  // GetPrivateProfileStringA("Misc", "CheatMenu", "", buf, 0xff, [0x83b410]) (0x5d4fca-0x5d4fea). The owner's INI has no
  // CheatMenu key, so the default "" is what the call yields; reading the INI is M2a-3.
  std::string ini_cheat_menu;
  bool cheat_menu() const;                     // GcSaver::CheatMenu 0x5d4fc0
  void set_quest(uint8_t lev, uint8_t quest);  // GcSaver::SetQuest 0x5d5e20
};

// BetterSaver.dll's boolean map (the std::map at 0x1000a80c: u16 key at node+0xe, byte value at node+0x10). Empty on the
// path (natives.md §9.2: the ResetData hook 0x10001680 clears it; only BetterSaver's GcSaver::Load fills it from disk).
class BetterSaverState {
 public:
  explicit BetterSaverState(HostContext&) {}
  bool get(uint16_t key) const;                                // 0x10001f70
  void set(uint16_t key, bool value, SaverState& saver);       // 0x10001ff0 -> 0x10004730
  const std::map<uint16_t, bool>& values() const { return values_; }

 private:
  std::map<uint16_t, bool> values_;  // a value of 0 is never stored: the set erases the key (0x10004889)
};

// GcHudDirector::PlayCinematic 0x47b820 (D1): the cinematic completes before the native returns.
struct CinematicState {
  explicit CinematicState(HostContext&) {}
  uint64_t completed = 0;  // cinematics run to completion (played, or skipped behind --skip-video)
  uint64_t skipped = 0;    // of those, skipped by --skip-video (the original's [0x70a3b0] = 0 route, 0x47b86e)
  std::string last;        // the last name passed
  bool playing = false;    // a cinematic in flight: never true when a native returns (KillCinematic 0x47b8fd)
};

// One swallowing block of GcAreaLoader::LoadArea 0x4fedd0 (natives.md §5.2): the nested engine calls made inside it and
// the errors swallowed there (Task 10's swallow_count probe reads these).
struct LoadBlockStats {
  uint64_t runs = 0;          // times the block found its file and ran its loader
  uint64_t nested_calls = 0;  // OnEvent calls made inside it
  uint64_t errors = 0;        // of those, raised and swallowed (D16, `X N`)
};

// GcAreaDirector and GcAreaLoader as the natives reach them.
class AreaDirector {
 public:
  enum Block { kObjects = 0, kCharacters = 1, kHives = 2 };
  explicit AreaDirector(HostContext& c) : ctx_(c) {}

  uint32_t current_level() const { return level_; }  // [0x83902c]
  uint32_t current_area() const { return area_; }    // [0x83b0b4]
  void set_current_level(const std::string& name);    // GcAreaDirector::SetCurrentLevel 0x489a60
  void set_current_area(const std::string& name);     // GcAreaDirector::SetCurrentArea 0x48a540
  // GcAreaDirector::GetScriptFluffString 0x48acc0 -> GcStringTableLoader::GetString 0x5e86e0 on [0x8391c0]
  std::string script_fluff_string(uint32_t key0, uint32_t key1, uint32_t index);
  const LoadBlockStats& block_stats(Block b) const { return stats_[b]; }

  // The block headers 0x839238 (the level's) and 0x83aedc (the area's): loaded or not, and the path they were opened
  // from. A slot may point at a header that is closed or failed to open: it is then set but empty (Find gives -1).
  struct Header {
    bool loaded = false;
    std::string path;
  };
  const Header& level_header() const { return hdr_[0]; }
  const Header& area_header() const { return hdr_[1]; }

 private:
  struct SlotRef {
    int header = -1;  // 0 level, 1 area, -1 none (or changed since by another native)
    std::string path;
  };
  sage::BlockChain* chain();
  void close(int h);
  void load_header(int h, bool second);
  void put(sage::Source s, int h);
  int ref(sage::Source s);
  void remove_area();
  void load_area(const std::string& name);
  void run_block(Block b, const std::string& file);
  void load_fluff();
  std::string fluff_get(uint32_t key0, uint32_t key1, uint32_t index) const;

  HostContext& ctx_;
  uint32_t level_ = kNullId;
  uint32_t area_ = kNullId;
  Header hdr_[2];
  SlotRef cur_, sec_;
  std::array<LoadBlockStats, 3> stats_{};
  // [0x8391c0]: the fluff string table GcAreaDirector::Init 0x4875b0 loads before Setup
  bool fluff_tried_ = false;
  bool fluff_present_ = false;
  sage::Slb fluff_;
  std::map<uint32_t, uint32_t> fluff_inc_;  // pointer slot offset -> InitStrings 0x5e8600 increments (`inc [ebx]` 0x5e8675)
};

// The language block file and fluff table GcAreaDirector::Init 0x4875b0 picks for language byte [0x70b384] = 0 (the
// owner's INI: Language=0; INI -> M2a-3): "Root/Data/Languages/Lang_eng.blk" (0x70b514) and
// "Root/Data/Levels/lev1/fluff_eng.slb" (0x70b3f4).
extern const char* const kFluffLangBlk;
extern const char* const kFluffTable;

}  // namespace lomn::osi
