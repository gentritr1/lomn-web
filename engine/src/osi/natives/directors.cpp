// M2a-2 T8a: the "directors, saver and Beta 1.5.0 mods" native group. One citation comment per binding (natives.md §1.3):
// the wrapper body (the registry `body`), the method it calls, the function that decides, and the asm pin that
// engine/tests/test_osi_directors.cpp checks against the exe (or BetterSaver.dll). A value that comes from an instruction
// immediate or a data constant is cited by instruction address; INFERRED marks a reading that was not run to a conclusion.
#include "lomn/osi/directors.h"

#include <string>
#include <vector>

#include "lomn/fs.h"
#include "lomn/osi/services.h"
#include "lomn/sage/slb.h"

namespace lomn::osi {

const char* const kFluffLangBlk = "Root/Data/Languages/Lang_eng.blk";   // 0x70b514 (GcAreaDirector::Init 0x487a42)
const char* const kFluffTable = "Root/Data/Levels/lev1/fluff_eng.slb";  // 0x70b3f4 (GcAreaDirector::Init 0x487855)

// ---------------------------------------------------------------------------------------------- the singletons

GcViewPort* GameSingletons::main_view(uint16_t type) {
  // ScScreen::GetViewPort 0x49d3c0 walks the screen's view list for the key [0x8391b0] (0x43ae26) and returns the view
  // (0x49d3f6), or 0 when no view has that key. SetUpView adds the main view before Setup (AddViewPort 0x43a85a), so the
  // walk always finds it on the path (INFERRED that nothing removes it).
  if (!view_) view_ = ctx_.make<GcViewPort>(type);
  return view_;
}

GcLegoCamera* GameSingletons::camera(GcViewPort* vp, uint16_t type) {
  // GcViewPort::GetCamera 0x448080: [[vp+0x30]+0x1c] (0x44809e, 0x4480ca). The temporary reference it takes on the camera
  // NODE (0x4480aa / 0x4480d6) is not on the camera, and the node never reaches a script: no H line.
  if (!vp->camera) vp->camera = ctx_.make<GcLegoCamera>(type);
  return vp->camera;
}

GcKeyMapper* GameSingletons::key_mapper(uint16_t type) {
  if (!keys_) keys_ = ctx_.make<GcKeyMapper>(type);  // SetUpControllers: new 0x5cef80, +1 0x439583, [0x83aa98] 0x4395a3
  return keys_;
}

GcMP3Player* GameSingletons::mp3_player(uint16_t type) {
  if (!mp3_) mp3_ = ctx_.make<GcMP3Player>(type);  // SetUpDatabase: new (0x43a3e8, unconditional), +1 0x43a40c, 0x43a42c
  return mp3_;
}

// ---------------------------------------------------------------------------------------------- the saver

bool SaverState::cheat_menu() const {
  // GcSaver::CheatMenu 0x5d4fc0: the INI string (at most 0xff bytes, 0x5d4fcf) copied into a ScFixedString and compared with
  // "LEGOTester" (0x74765f) by ScFixedString::compare 0x4f5990: memcmp 0x657820 (repe cmpsb, case sensitive) over the
  // shorter length, then the length difference (0x4f59d4). Equal -> 1 (0x5d503c-0x5d5040), else 0 (0x5d5060).
  return ini_cheat_menu == "LEGOTester";
}

void SaverState::set_quest(uint8_t lev, uint8_t quest) {
  // GcSaver::SetQuest 0x5d5e20: level 1..6 (cmp cl, 1 ; jb at 0x5d5e24-0x5d5e27, cmp cl, 6 ; ja at 0x5d5e29-0x5d5e2c), quest
  // 1..3 (0x5d5e33), [0x83b37f + level] |= 1 / 2 / 4 (0x5d5e45 / 0x5d5e53 / 0x5d5e63). Level 0 is ignored.
  if (lev < 1 || lev > 6 || quest < 1 || quest > 3) return;
  quests.at(lev) = static_cast<uint8_t>(quests.at(lev) | (1u << (quest - 1)));  // .at: never past the array
}

bool BetterSaverState::get(uint16_t key) const {
  // 0x10001f70: lower_bound on the map 0x1000a80c (0x10005ab0 at 0x10001f88); found when the node is not the head (byte
  // +0xd) and !(key < node.key) (0x10001f96). The byte at +0x10 (0x10001fb8), else al = 0 (0x10001fc8). The second lookup's
  // _Xout_of_range (0x10001fbd) is unreachable: the first lookup found the key.
  auto it = values_.find(key);
  return it != values_.end() && it->second;
}

void BetterSaverState::set(uint16_t key, bool value, SaverState& saver) {
  // 0x10001ff0 -> 0x10004730 (ecx = &key, edx = &value). value != 0: a present key gets the byte (0x100047c9), else a node
  // is inserted (0x1000481c). value == 0: the removal branch (0x1000476f -> 0x1000484c) erases a present key (0x10004889).
  if (value) values_[key] = true;
  else values_.erase(key);
  if (key >= 32) return;  // cmp word [edi], 0x20 ; jae (0x1000488e)
  // the legacy condition dword [[0x1000a040]] = [0x8391a0]: bts (0x100048ab) / btr (0x100048e1)
  if (!value) {
    saver.condition &= ~(1u << key);
    return;
  }
  saver.condition |= 1u << key;
  // keys 0xe / 0x10 / 0x12 set: GcSaver::SetQuest([[0x1000a07c]] = the level byte [0x74748c], 1 / 2 / 3) (0x100048b3-0x100048da)
  const uint8_t quest = key == 0xe ? 1 : key == 0x10 ? 2 : key == 0x12 ? 3 : 0;
  if (quest) saver.set_quest(saver.level, quest);
}

// ---------------------------------------------------------------------------------------------- the area director

namespace {
using sage::Source;
const char* const kUnloaded[2] = {"<block header 0x839238, not loaded>", "<block header 0x83aedc, not loaded>"};

bool header_opens(const fs::FileSystem* vfs, const std::string& path) {
  if (!vfs) return false;
  sage::BlockChain probe(*vfs);
  std::string err;
  return probe.open_current(path, &err);  // ScBlockFile::Open 0x610000 (its result is not tested; IsHeaderLoaded is)
}

// The {u32, n at +4, records at the pointer +8} tables of the character and hive loaders (LoadCharacters 0x53f543 /
// 0x53f54c; LoadHives 0x626411 / 0x62641a), each record's id at +0. With `kind2c`, a record whose dword +0x2c is 0 is
// dropped (LoadHives: cmp [edi+0x2c], 0 ; je at 0x62623c-0x626240 -> GcHive::GcHive 0x6239b0 at 0x6262fa, whose base
// GcMoveableObj 0x5441d0 reaches no VM function). False on a table that does not fit its buffer.
bool record_ids(const sage::Slb& slb, uint32_t stride, std::vector<uint32_t>& ids, bool kind2c = false) {
  uint32_t n = 0, base = 0;
  if (!slb.read_u32(4, n) || !slb.read_ptr(8, base)) return false;
  if (base > slb.data_end() || n > (slb.data_end() - base) / stride) return false;
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t id = 0;
    const uint64_t at = static_cast<uint64_t>(base) + static_cast<uint64_t>(i) * stride;
    if (!slb.read_u32(at, id)) return false;
    uint32_t k = 1;
    if (kind2c && !slb.read_u32(at + 0x2c, k)) return false;
    if (k != 0) ids.push_back(id);
  }
  return true;
}
}  // namespace

sage::BlockChain* AreaDirector::chain() { return ctx_.block_chain(); }

int AreaDirector::ref(Source s) {
  SlotRef& r = s == Source::Current ? cur_ : sec_;
  sage::BlockChain* c = chain();
  if (!c || r.header < 0) return -1;
  if (!c->blockfile(s) || c->path(s) != r.path) {  // another native has set the slot since: no longer one of the headers
    r.header = -1;
    return -1;
  }
  return r.header;
}

// SrSetCurBlockFile 0x54db20 / SrSetCurSecondBlockFile 0x54db40 with a header object: the slot shows the header's file, or
// nothing at all when the header is not loaded (set, but Find gives -1: Close zeroes the count, 0x60ffdb / 0x60fedb).
void AreaDirector::put(Source s, int h) {
  sage::BlockChain* c = chain();
  if (!c) return;
  std::string err;
  const bool ok = hdr_[h].loaded &&
                  (s == Source::Current ? c->open_current(hdr_[h].path, &err) : c->open_second(hdr_[h].path, &err));
  if (!ok) c->set_unloaded(s, kUnloaded[h]);
  SlotRef& r = s == Source::Current ? cur_ : sec_;
  r.header = h;
  r.path = c->path(s);
}

// ScBlockFile::Close 0x60ff70 on the header object: a slot that points at it stays set and finds nothing.
void AreaDirector::close(int h) {
  const bool cur = ref(Source::Current) == h, sec = ref(Source::Second) == h;
  hdr_[h].loaded = false;
  if (cur) put(Source::Current, h);
  if (sec) put(Source::Second, h);
}

// LoadLevelBlockHeader 0x48bd10 (h 0) / LoadAreaBlockHeader 0x48bc10 (h 1): a loaded header is not reopened
// (IsHeaderLoaded 0x48bd1f / 0x48bc1f); otherwise "Root/Data/Levels/%.4s/%.4s.blk" (0x70b7ec) with SrAsString of [0x83902c]
// twice, or of [0x83902c] and [0x83b0b4]. Then the slot: the level header goes to the current slot with argument 0
// (0x48be10) and to the second with 1 (0x48be01); the area header always to the current one (0x48bcfb).
void AreaDirector::load_header(int h, bool second) {
  if (!hdr_[h].loaded) {
    const std::string lev = identifier_to_string(level_);  // SrAsString 0x43d020 (byte swap), "%.4s" stops at a NUL
    hdr_[h].path = "Root/Data/Levels/" + lev + "/" + (h == 0 ? lev : identifier_to_string(area_)) + ".blk";
    hdr_[h].loaded = header_opens(ctx_.vfs(), hdr_[h].path);
    // a slot that still points at this header object sees the file it holds now
    const bool cur = ref(Source::Current) == h, sec = ref(Source::Second) == h;
    if (cur) put(Source::Current, h);
    if (sec) put(Source::Second, h);
  }
  put(second ? Source::Second : Source::Current, h);
}

// GcAreaDirector::RemoveArea 0x489560 (from SetCurrentLevel at 0x489add), then [0x83b0b4] = null (0x4895a1) and both block
// headers are closed (0x4895c4 / 0x4895ce).
// KNOWN GAP (review I-2, deferred to the follow-up batch by controller ruling): RemoveArea -> GcAreaLoader::CleanUpArea
// 0x5017d0 (0x489589) calls GcModelDirector::Remove 0x47f320 (0x501830) for every loader entry of type [0x83aa9c] (the
// characters, 0x501813), and Remove makes Call("OnEvent") at 0x47f680 (guards 0x47f33c / 0x47f376). run_block(kCharacters)
// treats _char.slb records as loaded characters, so the original would call OnEvent here for each of them; the port makes
// no call yet.
void AreaDirector::remove_area() {
  area_ = kNullId;
  close(1);
  close(0);
}

void AreaDirector::set_current_level(const std::string& name) {
  const uint32_t id = convert_string_to_id(name);  // StringToIdentifier 0x617810 (0x489a96)
  if (level_ != kNullId && area_ != kNullId) remove_area();  // 0x489ab4-0x489add (the nulls of 0x70b3bc / 0x70b3c0)
  level_ = id;                                              // 0x489af6
  close(0);                                                 // 0x489b84
  close(1);                                                 // 0x489b8e
  load_header(0, false);                                    // 0x489b93: LoadLevelBlockHeader(0) -> the CURRENT slot
  // The material, texture and language-string loads that follow are render and text data no native of this group reads;
  // the language load saves the current slot (0x48a25b) and restores it (0x48a449), so the slots end as set above.
  // GcSaver::SetLevel(name[3] - '0') (0x48a44f-0x48a45f -> 0x5d5744): a name shorter than 4 bytes reads its NUL (3 bytes)
  // or a byte past it (INFERRED 0: the dynamic string's buffer after the terminator).
  const uint8_t c3 = name.size() > 3 ? static_cast<uint8_t>(name[3]) : 0;
  ctx_.subsystem<SaverState>().level = static_cast<uint8_t>(c3 - 0x30);
}

void AreaDirector::set_current_area(const std::string& name) {
  const uint32_t id = convert_string_to_id(name);  // StringToIdentifier 0x617810 (0x48a55d)
  area_ = id;                                      // 0x48a57f
  load_area(name);                                 // GcAreaDirector::LoadArea(name, 1) 0x4892f0 (0x48a585-0x48a588)
  // GcSaver::SetCurrentArea 0x5d5500 (0x48a593): [0x747490] = id (0x5d5506), then CleanupLoad (0x5d550c), which
  // BetterSaver replaces with 0x10001ae0 (natives.md §9.3): GetAreaLoader 0x48a810, then RemoveObject 0x501dc0 for every
  // entry of its token vector [0x1000a81c..0x1000a820) and of the exe lists 0x8384a0 / 0x7f79f0 (counts [0x83b401] /
  // [0x83b3fa]) whose area is [0x747490]. Every list is empty in M2a-2 (no pickup and no load is modelled; ResetData zeroes
  // the counts, 0x5d54ab / 0x5d54b2), so nothing is removed and no call is made.
  ctx_.subsystem<SaverState>().area = id;
}

// GcAreaDirector::LoadArea 0x4892f0 -> GcAreaLoader::LoadArea 0x4fedd0 (0x48935a), as far as a script can see it.
void AreaDirector::load_area(const std::string& name) {
  ctx_.subsystem<RealDeltaTime>().count = 0;  // mov [0x712d88], 0 (0x489305)
  load_header(0, true);                       // LoadLevelBlockHeader(1) (0x4ff62c): the level file in the SECOND slot
  load_header(1, false);                      // LoadAreaBlockHeader (0x4ff63b): the area file in the CURRENT slot
  // "Root/Data/Levels/%.4s/%.4s/%.4s_<kind>.slb" (0x72990f, 0x7299b5, 0x729a88; built at 0x4ff512-0x4ff61d) with the level
  // as SrAsString([0x83902c]) (0x489344) and the area NAME argument (not its id), each cut to 4 bytes.
  const std::string dir =
      "Root/Data/Levels/" + identifier_to_string(level_) + "/" + name.substr(0, 4) + "/" + name.substr(0, 4);
  run_block(kObjects, dir + "_obj.slb");     // LoadObjects 0x53fc60 (0x4ff93e)
  run_block(kCharacters, dir + "_char.slb");  // LoadCharacters 0x53f3f0 (0x5002ec)
  run_block(kHives, dir + "_hive.slb");      // LoadHives 0x6261c0 (0x500c0b)
}

// One swallowing block of LoadArea (natives.md §5.2). GetSLKFile 0x4032e0 on a missing file throws (SrThrow 0x4d91f0 at
// 0x4033b2) inside the block's try: the pad catches it and nothing reaches the VM, so the block is skipped.
// GcMotionSystem::SetupMoveable 0x574b80 calls SetUnderWater 0x589050 (0x574efd) only when the motion id [ms+0x14] equals
// MainCharacterID() (0x574ee5-0x574ef3). How each block gets there (review C-1):
// - objects and hives build a GcMoveableObj 0x5432a0 (AddMoveableObj 0x489700 at 0x489937, GcNoLightDirector at 0x4842dc;
//   GcHive::GcHive 0x6236a0 at 0x6236d4). The ctor stores its id at [this+0x80] (0x54332c), which becomes the motion id
//   (0x543f44 -> GcMotionSystem 0x5742a0 -> [ms+0x14] 0x5748ec), and calls GcMoveableObj::SetupMoveable 0x545c00 (0x54408b)
//   ONLY when [this+0x80] == [0x734f10] = 'lbld' (cmp at 0x544065, jne 0x5440d3). So these fire only for an id that is
//   both 'lbld' and the main character's. The hive id is the record id (0x6236c1); the object id is AddMoveableObj's
//   (0x489925): the record id, renamed (0x4898b0 / 0x4898c1) only while an area loader with that id exists (GetAreaLoader
//   0x48a810 on the map 0x783a88), which the port does not track (INFERRED: no area is named 'lbld').
// - characters: AddCharacter 0x47f040 -> GcCharacter::SetupMoveable 0x418850 -> 0x574b80, no 'lbld' guard (INFERRED: the
//   motion id is the record id).
// SetUnderWater makes Call("OnEvent" 0x73df20, byte 0x36, int b) with b = ([0x73c5a8] != 0) on both of its branches
// (0x589154 / 0x589249). An error inside it unwinds out of the loader to the block's pads: the block's remaining records
// are not loaded, and LoadArea goes on with the next block (D16).
void AreaDirector::run_block(Block b, const std::string& file) {
  sage::BlockChain* c = chain();
  if (!c || !c->exists(file)) return;  // SrBlockFileExists 0x54ddc0 (0x40339e), then SrThrow
  std::vector<uint8_t> bytes;
  std::string err;
  if (!c->load(file, bytes, nullptr, &err)) return;  // SrBlockFileLoad 0x54db60 (0x4033d4)
  sage::Slb slb;
  if (!slb.parse(std::move(bytes), &err)) return;  // a table that does not fit its buffer: not modelled, skipped
  std::vector<uint32_t> ids;
  if (b == kObjects) {
    // {slot, n, n2 at +8, records at the pointer +0xc} x 0x3c (0x540704-0x540713, 0x5406fd); only flag 0x20 builds a
    // moveable: GcAreaDirector::AddMoveableObj 0x489700 with 0x10 set (0x53fe03), GcNoLightDirector::AddMoveableObj 0x4842a0
    // with 0x10 clear (0x5403a9); both construct a GcMoveableObj 0x5432a0
    std::vector<sage::ObjRecord> recs;
    if (!sage::parse_obj(slb, recs, &err)) return;
    for (const sage::ObjRecord& r : recs)
      if (r.flags & 0x20) ids.push_back(r.id);
  } else if (!record_ids(slb, b == kCharacters ? 0x34 : 0x3c, ids, b == kHives)) {  // strides 0x53f53c / 0x626408
    return;
  }
  LoadBlockStats& st = stats_[b];
  ++st.runs;
  GameGlobals& g = ctx_.subsystem<GameGlobals>();
  for (uint32_t id : ids) {
    if (b != kCharacters && id != kLbld) continue;  // GcMoveableObj ctor: cmp [0x734f10], [this+0x80] (0x544065)
    if (id != g.main_character_id) continue;        // MainCharacterID 0x43af90, read per moveable (0x574eeb-0x574ef3)
    ++st.nested_calls;
    bool swallowed = false;
    ctx_.engine_call_swallowing("OnEvent", {make_int(0x36), make_int(g.global_under_water ? 1 : 0)}, &swallowed);
    if (swallowed) {
      ++st.errors;
      return;  // the pads 0x4ff945 / 0x5002f3 / 0x500c12 (and their two siblings): the rest of the block is skipped
    }
  }
}

// GcAreaDirector::Init 0x4875b0, before Setup: the language block file is made the current slot when it opens (0x487bcf ->
// 0x487bdf; else the saved slot, 0x487be8), the table loads through GcStringTableLoader::GetSLKFile 0x487e10 -> 0x4032e0
// into [0x8391c0] (0x487c74), InitStrings 0x5e8600 runs once (0x487cb0), and the saved slot comes back (0x487daa). Both slots
// are 0 then (natives.md §8.2), so a private chain (Art.blk + the language file) models that lookup and the shared chain is
// not touched. A missing table throws inside Init's try (pads 0x487cb7 / 0x487cd6 / 0x487cf6): [0x8391c0] stays 0 (the
// static init's 0x48dc71).
void AreaDirector::load_fluff() {
  if (fluff_tried_) return;
  fluff_tried_ = true;
  const fs::FileSystem* vfs = ctx_.vfs();
  if (!vfs) return;
  sage::BlockChain c(*vfs);
  std::string err;
  c.open_art("Root/Data/Art/Art.blk", &err);
  c.open_current(kFluffLangBlk, &err);
  if (!c.exists(kFluffTable)) return;
  std::vector<uint8_t> bytes;
  if (!c.load(kFluffTable, bytes, nullptr, &err)) return;
  if (!fluff_.parse(std::move(bytes), &err)) return;
  fluff_present_ = true;
  // InitStrings: for each of the [data+0] entries (16 bytes, from the pointer at data+4: [this+0x98] / [this+0x9c], the
  // data starting at +0x98, 0x4033e2), `inc [slot]` on each of its n string slots (0x5e8675): every pointer steps over the
  // string's length byte. A slot array two entries share is stepped twice.
  uint32_t count = 0, entries = 0;
  if (!fluff_.read_u32(0, count) || !fluff_.read_ptr(4, entries)) return;
  for (uint32_t e = 0; e < count; ++e) {
    const uint64_t at = static_cast<uint64_t>(entries) + 16ull * e;
    uint32_t n = 0, arr = 0;
    if (!fluff_.read_u32(at + 8, n) || !fluff_.read_ptr(at + 12, arr)) break;
    for (uint32_t j = 0; j < n; ++j) {
      const uint64_t slot = static_cast<uint64_t>(arr) + 4ull * j;
      uint32_t v = 0;
      if (!fluff_.read_u32(slot, v)) break;
      ++fluff_inc_[static_cast<uint32_t>(slot)];
    }
  }
}

// GcStringTableLoader::GetString 0x5e86e0: the first entry with key0 == w0 (0x5e86fb) and key1 == w1 (0x5e8707); then
// strings[w2] when w2 <= n - 1, unsigned (0x5e870c-0x5e870f), else "" (0x74ae14, 0x5e8711); no entry: "" (0x5e877f).
// n == 0 makes n - 1 = 0xffffffff, so the original reads past the array (undefined): the model answers "".
std::string AreaDirector::fluff_get(uint32_t key0, uint32_t key1, uint32_t index) const {
  uint32_t count = 0, entries = 0;
  if (!fluff_.read_u32(0, count) || !fluff_.read_ptr(4, entries)) return "";
  for (uint32_t e = 0; e < count; ++e) {
    const uint64_t at = static_cast<uint64_t>(entries) + 16ull * e;
    uint32_t k0 = 0, k1 = 0, n = 0;
    if (!fluff_.read_u32(at, k0) || !fluff_.read_u32(at + 4, k1) || !fluff_.read_u32(at + 8, n)) return "";
    if (k0 != key0 || k1 != key1) continue;
    if (n == 0 || index > n - 1) return "";
    uint32_t arr = 0, target = 0;
    if (!fluff_.read_ptr(at + 12, arr)) return "";
    const uint64_t slot = static_cast<uint64_t>(arr) + 4ull * index;
    if (!fluff_.read_ptr(slot, target)) return "";
    auto inc = fluff_inc_.find(static_cast<uint32_t>(slot));
    uint64_t p = static_cast<uint64_t>(target) + (inc == fluff_inc_.end() ? 0 : inc->second);
    const std::vector<uint8_t>& d = fluff_.bytes();
    std::string s;
    for (; p < d.size() && d[static_cast<size_t>(p)] != 0; ++p) s += static_cast<char>(d[static_cast<size_t>(p)]);
    return s;
  }
  return "";
}

std::string AreaDirector::script_fluff_string(uint32_t key0, uint32_t key1, uint32_t index) {
  load_fluff();
  std::string s;
  // no table, w1 == 0 or w0 == 0 -> "XXX" (0x48ad29-0x48ad3a, 0x70b7e7)
  if (!fluff_present_ || key1 == 0 || key0 == 0) s = "XXX";
  else s = fluff_get(key0, key1, index);
  if (s.empty()) s = "XXX";  // compared with "" (0x70b7eb) by 0x4f5990 at 0x48ad85: equal -> "XXX" (0x48ad97)
  // the copies into the 0xff-capacity result: operator= 0x4f5c00 (len > 255 throws) and Init 0x401850 (n >= 255 throws
  // kOutOfRange, 0x48adf2)
  return osi_cstring(s);
}

// ---------------------------------------------------------------------------------------------- the native bodies

namespace {
HostContext& host(void* user) { return *static_cast<HostContext*>(user); }

std::string string_arg(HostContext& ctx, NativeCall& c, size_t i) {
  if (i >= c.args.size() || c.args[i]->type != kString) throw VmError("Undefined: string argument expected");
  return osi_cstring(ctx.heap().text(c.args[i]->value));
}

// body 0x4366e0, method 0x43ae20 <GcGame::GetMainView> -> 0x49d3c0 <ScScreen::GetViewPort>; asm pin: 0x43ae26 the key
// [0x8391b0], 0x49d3ea/0x49d3f2 the key compare, 0x436730 the overload's AddRef. Engine-owned, G 1 (natives.md §2.4).
Variant n_getmainview(NativeCall& c, void* user) {
  HostContext& ctx = host(user);
  return ctx.create_variant(ctx.subsystem<GameSingletons>().main_view(c.entry.ret_type));  // CreateVariant 0x436700
}

// body 0x4367f0, method 0x43ae90 <GcGame::GetKeyMapper>; asm pin: 0x43ae90 mov eax, [0x83aa98] (set at 0x4395a3 after the
// +1 at 0x439583); G 1. The queued AddProcess (0x4395b7) is applied by the first ScProcessManager::Process (0x4a95da): a
// host-side +1 that M2a-2 does not model (Decision 8, natives.md §3.1).
Variant n_getkeymapper(NativeCall& c, void* user) {
  HostContext& ctx = host(user);
  return ctx.create_variant(ctx.subsystem<GameSingletons>().key_mapper(c.entry.ret_type));  // CreateVariant 0x436810
}

// body 0x436940, method 0x43aea0 <GcGame::GetMP3Player>; asm pin: 0x43aea0 mov eax, [0x83879c] (set at 0x43a42c after the
// unconditional new at 0x43a3e8 and the +1 at 0x43a40c): never null on the path, G 1
Variant n_getmp3player(NativeCall& c, void* user) {
  HostContext& ctx = host(user);
  return ctx.create_variant(ctx.subsystem<GameSingletons>().mp3_player(c.entry.ret_type));  // CreateVariant 0x436960
}

// body 0x4474e0, method 0x448080 <GcViewPort::GetCamera>; asm pin: 0x44809e mov edi, [ebx+0x30] (no null test of this),
// 0x4480ca mov edi, [edx+0x1c]; the node's temporary +1/-1 (0x4480aa / 0x4480d6) is not on the camera. G 1 (INFERRED).
Variant n_getcamera(NativeCall& c, void* user) {
  HostContext& ctx = host(user);
  GcViewPort* vp = ctx.object_arg<GcViewPort>(c, 0);
  if (!vp) throw VmError("Undefined: gcviewport.getcamera on nothing (the original reads [this+0x30] of null, 0x44809e)");
  return ctx.create_variant(ctx.subsystem<GameSingletons>().camera(vp, c.entry.ret_type));  // CreateVariant 0x447500
}

// body 0x486c40, method 0x486c40 <GcAreaDirector::GetCurrentLevel (inline)>; asm pin: 0x486c46 mov edx, [0x83902c], the
// null id stored by the static init at 0x48db4b; CreateVariant 0x407df0 {0x10, the dword} (0x407dfb)
Variant n_getcurrentlevel(NativeCall&, void* user) {
  return make_int(static_cast<int32_t>(host(user).subsystem<AreaDirector>().current_level()));
}

// body 0x486c10, method 0x489a60 <GcAreaDirector::SetCurrentLevel> -> 0x617810 <ScSLOSIFoundation::StringToIdentifier>; asm
// pin: 0x489af6 mov [0x83902c], edx after the RemoveArea guard 0x489aba-0x489add; 0x489b93 LoadLevelBlockHeader(0) (the
// current slot). No engine call on the path: the guard needs a level AND an area set (natives.md §4).
Variant n_setcurrentlevel(NativeCall& c, void* user) {
  HostContext& ctx = host(user);
  ctx.subsystem<AreaDirector>().set_current_level(string_arg(ctx, c, 0));
  return nothing();  // {0x0f, 0} (0x486c21)
}

// body 0x486ef0, method 0x48a540 <GcAreaDirector::SetCurrentArea> -> 0x4fedd0 <GcAreaLoader::LoadArea> (three swallowing
// blocks, D16) and 0x5d5500 <GcSaver::SetCurrentArea> -> BetterSaver 0x10001ae0 <CleanupLoad>; asm pin: 0x48a57f mov
// [0x83b0b4], edx, 0x574eeb-0x574efd the MainCharacterID guard of SetUnderWater, 0x58914f/0x589244 Call("OnEvent")
Variant n_setcurrentarea(NativeCall& c, void* user) {
  HostContext& ctx = host(user);
  ctx.subsystem<AreaDirector>().set_current_area(string_arg(ctx, c, 0));
  return nothing();  // {0x0f, 0} (0x486f01)
}

// body 0x487250, method 0x48acc0 <GcAreaDirector::GetScriptFluffString> -> 0x5e86e0 <GcStringTableLoader::GetString>; asm
// pin: 0x48ad29 cmp [0x8391c0], 0 / 0x48ad32 the w1, w0 tests, "XXX" 0x70b7e7, 0x5e8709 the unsigned index test, 0x5e8675
// InitStrings' inc; the table "Root/Data/Levels/lev1/fluff_eng.slb" 0x70b3f4 in Lang_eng.blk 0x70b514 (language 0)
Variant n_getscriptfluffstring(NativeCall& c, void* user) {
  HostContext& ctx = host(user);
  const std::string s = ctx.subsystem<AreaDirector>().script_fluff_string(c.args[0]->value, c.args[1]->value, c.args[2]->value);
  return Variant{kString, ctx.heap().new_string(s)};  // CreateVariant(string) 0x46de00 (0x487287)
}

// body 0x5d35b0, method 0x5d4fc0 <GcSaver::CheatMenu> -> 0x4f5990 <ScFixedString::compare>; asm pin: 0x5d4fdb/0x5d4fe0/
// 0x5d4fe5 "" 0x74764b / "CheatMenu" 0x747679 / "Misc" 0x74761d, 0x5d4fea call [0x771424] (GetPrivateProfileStringA),
// 0x5d502c "LEGOTester" 0x74765f; int 0 for this install (no CheatMenu key; INI -> M2a-3)
Variant n_cheatmenu(NativeCall&, void* user) {
  return make_int(host(user).subsystem<SaverState>().cheat_menu() ? 1 : 0);  // CreateVariant 0x407f30 (0x5d35bd)
}

// body 0x5d3910, method 0x5d3910 <GcSaver::GetAmmo (inline)>; asm pin: 0x5d3913 mov ax, [0x7474c8] (0 in .data),
// CreateVariant 0x4183f0 movzx word (0x4183fa)
Variant n_getammo(NativeCall&, void* user) { return make_int(host(user).subsystem<SaverState>().ammo); }

// body 0x47b100, method 0x47b820 <GcHudDirector::PlayCinematic> -> 0x47b920 <GcHudDirector::KillCinematic>; asm pin:
// 0x47b867 cmp byte [0x70a3b0], 0 (the video switch), 0x47b8fd KillCinematic before the return. D1: the cinematic is
// complete when the native returns; PlayCinematic reaches no Call (its calls.jsonl closure, 71 functions), so no OnEvent.
Variant n_playcinematic(NativeCall& c, void* user) {
  HostContext& ctx = host(user);
  const std::string name = string_arg(ctx, c, 0);  // the 0xff-capacity copy (0x47b849 / 0x47b862)
  CinematicState& cs = ctx.subsystem<CinematicState>();
  cs.last = name;
  cs.playing = true;
  // --skip-video is the original's [0x70a3b0] = 0 route: je 0x47b902 at 0x47b86e skips the load, the play AND the
  // KillCinematic call; nothing was started, so nothing is in flight. With it off the original loads and plays (0x47b8ca /
  // 0x47b8fa), which M2a-2 does not render (M2a-3), and KillCinematic (0x47b8fd) ends it before the return.
  if (ctx.flags().skip_video) ++cs.skipped;
  cs.playing = false;
  ++cs.completed;
  return nothing();  // {0x0f, 0} (0x47b119)
}

// body BetterSaver.dll 0x10001f70, method 0x10005ab0 <std::map<u16, bool>::lower_bound>; asm pin: 0x10001f77 movzx esi,
// word [ebp+0x10] (the u16 key), 0x10001fc8 xor al, al (absent), 0x10001fd1 setne, 0x10001fd8 type 0x10
Variant n_getbooleanvalue(NativeCall& c, void* user) {
  return make_int(host(user).subsystem<BetterSaverState>().get(static_cast<uint16_t>(c.args[0]->value)) ? 1 : 0);
}

// body BetterSaver.dll 0x10001ff0, method 0x10004730 <BetterSaver map set>; asm pin: 0x10002005 setne byte [ebp+0x13] (w1 !=
// 0 as a dword, 0x10001ff4), 0x10001ffb the u16 key, 0x1000476f the erase branch, 0x1000488e/0x100048ab/0x100048e1 the
// legacy condition bits of keys < 32, 0x100048b3-0x100048d5 GcSaver::SetQuest 0x5d5e20 for keys 0xe/0x10/0x12
Variant n_setbooleanvalue(NativeCall& c, void* user) {
  HostContext& ctx = host(user);
  ctx.subsystem<BetterSaverState>().set(static_cast<uint16_t>(c.args[0]->value), c.args[1]->value != 0,
                                        ctx.subsystem<SaverState>());
  return nothing();  // {0x0f, 0} (0x10002011-0x1000201d)
}

// body BetterSaver.dll 0x10001f40, method 0x10002b00 <BetterSaver save routine>; asm pin: 0x10001f43 call 0x10002b00,
// 0x10001f4d OutputDebugStringW, 0x10001f56 type 0x0f. The file write (INFERRED User/data/player%d.sav) is M4: nothing here.
Variant n_save(NativeCall&, void*) { return nothing(); }
}  // namespace

void bind_directors_group(NativeRegistry& reg, HostContext& ctx) {
  struct Row {
    const char* cls;
    const char* fn;
    NativeFn f;
  };
  static const Row rows[] = {
      {"gcgame", "getmainview", n_getmainview},
      {"gcgame", "getkeymapper", n_getkeymapper},
      {"gcgame", "getmp3player", n_getmp3player},
      {"gcviewport", "getcamera", n_getcamera},
      {"gcareadirector", "getcurrentlevel", n_getcurrentlevel},
      {"gcareadirector", "setcurrentlevel", n_setcurrentlevel},
      {"gcareadirector", "setcurrentarea", n_setcurrentarea},
      {"gcareadirector", "getscriptfluffstring", n_getscriptfluffstring},
      {"gcsaver", "cheatmenu", n_cheatmenu},
      {"gcsaver", "getammo", n_getammo},
      {"gchuddirector", "playcinematic", n_playcinematic},
      {"bettersaver", "getbooleanvalue", n_getbooleanvalue},
      {"bettersaver", "setbooleanvalue", n_setbooleanvalue},
      {"bettersaver", "save", n_save},
  };
  for (const Row& r : rows)
    if (reg.find(r.cls, r.fn)) reg.bind(std::string(r.cls) + "." + r.fn, r.f, &ctx, Tier::Implemented);
}

}  // namespace lomn::osi
