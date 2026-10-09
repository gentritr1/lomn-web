// M2a-2 T9a: the "objects, model director, camera, HUD" native group. One citation comment per binding (natives.md §1.3):
// the wrapper body (the registry `body`), the method it calls, the function that decides, and the asm pin that
// engine/tests/test_osi_objects.cpp checks against the exe. A value that comes from an instruction immediate or a data
// constant is cited by instruction address; INFERRED marks a reading that was not run to a conclusion. Every engine
// reference a body takes or drops is one add_ref/release here, so natives.log gets one `H` line per event (temporaries
// included, natives.md §3.1).
#include "lomn/osi/objects.h"

#include <cstring>
#include <string>
#include <vector>

#include "lomn/osi/directors.h"
#include "lomn/osi/services.h"

namespace lomn::osi {

// ~ScTextObject 0x611070 (vtable 0x754758 slot 0 = 0x6110c0 -> call 0x6110e7): a held font is released (cmp at 0x61108f,
// dec [font+4] at 0x611098; at 0 its Free, 0x6110a3).
void ScTextObject::release_held(HostContext& ctx) {
  if (font_) {
    ScTileFont* f = font_;
    font_ = nullptr;
    ctx.release(f);
  }
}

// GcModelDirector::Get 0x480f70: find in the ScIdentifierMap 0x782a04 (0x480f8b-0x480f90); not found -> null (edi = 0,
// 0x480f7c); found -> the stored pointer (0x480ff7). No reference is taken.
GcCharacter* ModelDirector::get(uint32_t id) const {
  auto it = map.find(id);
  return it == map.end() ? nullptr : it->second;
}

// GcGame::MainCharacterID 0x43af90: sToa [0x7032b8] null (cmp at 0x43af91, jne 0x43af98) -> [0x7796f4] ('cnt1', static
// init 0x43c193; 0x43af9e); else [sToa+0x1d0] null (0x43afb8, je 0x43afbf) -> the same; else [[sToa+0x1d0]+0x100]
// (0x43afc1 / 0x43afcb). [char+0x1d0] is 0 from the GcCharacter constructor (0x40bbba); its only native writer,
// gccharacter.setholder (0x410764), is a stub in M2a-2 (review 9a m-2), so the last branch is modelled but not reached on
// the A9 path. Every MainCharacterID guard of the port reads this one function (SetupMoveable 0x574ef1 in both groups).
uint32_t main_character_id(HostContext& ctx) {
  const ModelDirector& md = ctx.subsystem<ModelDirector>();
  if (md.s_toa && md.s_toa->holder) return md.s_toa->holder->controller_id();
  return ctx.subsystem<GameGlobals>().main_character_default;
}

namespace {
HostContext& host(void* user) { return *static_cast<HostContext*>(user); }

// wi = args[i] ? *(args[i]+4) : 0 (0x82, 0x60198c-0x601abc): an absent optional argument is 0
uint32_t w(NativeCall& c, size_t i) { return i < c.args.size() ? c.args[i]->value : 0u; }
float wf(NativeCall& c, size_t i) {
  const uint32_t b = w(c, i);
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}
// a `string` argument as the constructors copy it: ScFixedString::Init(p, npos) = the bytes up to the first NUL
std::string string_arg(HostContext& ctx, NativeCall& c, size_t i) {
  if (i >= c.args.size() || c.args[i]->type != kString) throw VmError("Undefined: string argument expected");
  return osi_cstring(ctx.heap().text(c.args[i]->value));
}
// The original reads through a null `this` in these bodies (an access violation, not an OSI error): no counterpart
// message, so the port raises an Undefined error, which the calling handler re-reports as "Script is calling %s.%s".
[[noreturn]] void null_this(const char* what) { throw VmError(std::string("Undefined: null object in ") + what); }

std::vector<Variant> event_args(int32_t a0, int32_t a1) {
  std::vector<Variant> v = {make_int(a0), make_int(a1)};
  v.resize(10, nothing());  // a2..a9 are literal {0x0f, 0} (e.g. 0x5689bb-0x568a1c)
  return v;
}

// ---- constructed objects ----

// body 0x611910, method 0x626b40 <ScTileFont::ScTileFont> (not ported: M2a-3); asm pin: 0x611918 push 0x190, 0x61192c the
// constructor call, 0x611980 CreateVariant(ScTileFont*)'s AddRef. G 0 (SxReferenceCountable count 0 at 0x4f2b88). The
// constructor saves, sets and restores the current block file (0x6276dc / 0x6276e9 / 0x627b07): no net change of the slot.
// A null `new` (0x611927) would give nothing; the port's allocation does not fail. The constructor's own error paths (a
// missing font file, its throw at 0x627230) are M2a-3 (not ported).
Variant n_sctilefont_create(NativeCall& c, void* user) {
  HostContext& ctx = host(user);
  return ctx.create_variant(ctx.make<ScTileFont>(c.entry.ret_type, string_arg(ctx, c, 0)));
}

// body 0x611cd0, method 0x610c40 <ScTextObject::ScTextObject> (the 5-argument constructor, called at 0x611d17); asm pin:
// 0x610c92 the font's +1 (skipped for a null font, 0x610c8d), 0x611d70 CreateVariant(ScTextObject*)'s AddRef. G 0.
// The font's +1 happens inside the constructor, before the result exists: its H line precedes the native's own line.
Variant n_sctextobject_create(NativeCall& c, void* user) {
  HostContext& ctx = host(user);
  ScTileFont* font = ctx.object_arg<ScTileFont>(c, 3);
  const std::string text = string_arg(ctx, c, 4);  // ScFixedString::Init(p, npos) at 0x610cce
  ScTextObject* t = ctx.make<ScTextObject>(c.entry.ret_type, wf(c, 0), wf(c, 1), wf(c, 2), font, text);
  if (font) ctx.add_ref(font);  // inc [font+4] (0x610c92)
  return ctx.create_variant(t);
}

// body 0x43dad0, method 0x440460 <GcBigSprite::LoadHudSprite> -> 0x43f210 <GcBigSprite::GcBigSprite>; asm pin: 0x44048a
// operator_new, 0x440494 the null test, 0x4404c4 the result is the new object (null only when new failed), 0x43da50 the
// AddRef. w5/w6 are passed as their low bytes (0x43daf7 / 0x43daf4). G 0. LoadHudSprite saves the current block file,
// sets 0 and restores it (0x440468 / 0x440471 / 0x4404be): no net change. The constructor's file reads (and their throws,
// 0x43f82c / 0x43faee) are M2a-3; the brief's "null exactly when the method returns null" is the failed new only.
Variant n_gcbigsprite_loadhudsprite(NativeCall& c, void* user) {
  HostContext& ctx = host(user);
  const std::string name = string_arg(ctx, c, 4);
  GcBigSprite* s = ctx.make<GcBigSprite>(c.entry.ret_type, wf(c, 0), wf(c, 1), wf(c, 2), wf(c, 3), name,
                                         static_cast<uint8_t>(w(c, 5)), static_cast<uint8_t>(w(c, 6)));
  return ctx.create_variant(s);
}

// body 0x61a2e0, method 0x61a4d0 <ScSLOSISLKCubicSpline::ScSLOSISLKCubicSpline> (-> GetSLKFile 0x61a580, call 0x61a514);
// asm pin: 0x61a2e8 push 0xc, 0x61a2f4 the null test, 0x61a350 the AddRef. G 0 (0x61a4de: the base constructor). A
// missing SLK file leaves the object (its [this+8] null): the result is never nothing after a successful new.
Variant n_spline_create(NativeCall& c, void* user) {
  HostContext& ctx = host(user);
  return ctx.create_variant(ctx.make<ScSLOSISLKCubicSpline>(c.entry.ret_type, string_arg(ctx, c, 0)));
}

// ---- the model director ----

// body 0x47cc30, method 0x47e170 <GcModelDirector::AddToa> (second argument 0, pushed at 0x47cc36) -> 0x567d10
// <GcToa::GcToa> -> 0x60b320 <Call> at 0x568a59, then 0x480eb0 <GcModelDirector::Add>; asm pin: 0x568992 push 0x32,
// 0x567e27 [toa+0x380] = 0xfa, 0x568a59 the Call (unconditional), 0x47e4c8 the Add test, 0x47e657 the result byte.
// Order of the original: the Toa's id (StringToIdentifier at 0x47e194); the constructor (controller id 'cnt1', 0x40b73e;
// ammo [0x7474c8] = 250 at 0x56898b; OnEvent(0x32, 250) at 0x568a59, an error propagates); node id = controller id
// (0x47e1eb); sSaveToa = id (0x47e221, both branches); Add under [0x782884] = 'cnt1' (0x47e4af): a temporary +1
// (0x480ec7), the insert's copy on success (0x480ede), the temporary -1 (0x480f08). Add false (the key is taken) -> int 0
// (0x47e4ca): the new Toa drops to 0 and is freed, sToa unchanged. Add true -> sToa = the Toa (0x47e527), int 1. The Toa
// has no handle yet, so none of these reference events writes an H line; G at its minting (gettoa) is 1, the map
// (natives.md §2.4, INFERRED there: no other lasting holder). Not modelled: the block-file slot (LoadCharBlockHeader sets
// it at 0x47e1fe; restored only on success, 0x47e61e; after a failed Add the original leaves it pointing into the freed
// Toa, and its queued AddProcess (0x568262) is a dangling pointer: undefined), the camera, the collision physics, the
// event handlers and the sounds.
Variant n_addtoa(NativeCall& c, void* user) {
  HostContext& ctx = host(user);
  ModelDirector& md = ctx.subsystem<ModelDirector>();
  const std::string name = string_arg(ctx, c, 0);
  const uint32_t id = convert_string_to_id(name);  // 0x47e194 -> 0x617810 (-> 0x5fb060)
  if (md.toa_type == 0) throw VmError("Undefined: no GcToa game type in the registry");
  GcToa* toa = ctx.make<GcToa>(md.toa_type, name, id);
  SaverState& saver = ctx.subsystem<SaverState>();               // the saver's globals (getammo / setammo read them)
  saver.ammo = toa->ammo();                                      // 0x56898b
  ctx.engine_call("OnEvent", event_args(0x32, toa->ammo()));    // 0x568a59 (name 0x73b964)
  saver.save_toa = id;                                           // 0x47e221
  ctx.add_ref(toa);                                              // Add: temporary +1 (0x480ec7)
  const bool inserted = md.map.find(kIdCnt1) == md.map.end();    // the insert at 0x480ede
  if (inserted) {
    md.map[kIdCnt1] = toa;
    ctx.add_ref(toa);  // the map's ScAutoRefPtr copy
  }
  ctx.release(toa);  // the temporary -1 (0x480f08): a failed Add frees the new Toa here
  if (!inserted) return make_int(0);  // 0x47e4ca
  md.s_toa = toa;                     // 0x47e527
  return make_int(1);                 // CreateVariant 0x407f30: {0x10, movzx byte}
}

// body 0x47cf80, method 0x480f70 <GcModelDirector::Get> on the key [0x782884] ('cnt1', read at 0x47cf86); asm pin: 0x47cf86
// the key load, 0x482d43 its static init, 0x47cfe0 CreateVariant(GcToa*)'s AddRef. The same object as getcharacter('cnt1')
// (natives.md §2.3): one handle, minted under the object's class (GcToa).
Variant n_gettoa(NativeCall& c, void* user) {
  HostContext& ctx = host(user);
  return ctx.create_variant(ctx.subsystem<ModelDirector>().get(kIdCnt1), c.entry.ret_type);
}

// body 0x47ce90, method 0x480f70 <GcModelDirector::Get> on w0 (w1 is loaded and overwritten, 0x47ce9e / 0x47cea0); asm pin:
// 0x47cea0 the key, 0x47cf00 CreateVariant(GcCharacter*)'s AddRef. Not found -> nothing (0x47cee3).
Variant n_getcharacter(NativeCall& c, void* user) {
  HostContext& ctx = host(user);
  return ctx.create_variant(ctx.subsystem<ModelDirector>().get(w(c, 0)), c.entry.ret_type);
}

// body 0x47cd70, method 0x47f830 <GcModelDirector::Show> (-> 0x54b330 <GcBAGroup::SetCharacter> for 'cnt1'); asm pin:
// 0x47f9df / 0x419b12 / 0x47f9f9 the node's +1 / copy +1 / -1, 0x54b344 SetCharacter's +1.
// - Get(id) null: the original reads [null+0x10] (0x47f99b, or 0x47fa33): a fault -> Undefined.
// - The vine branch (a GcVine's type info, 0x47f86c-0x47f8a5) needs a vine in the map; no native of this port inserts one.
// - id == 'cnt1' (0x47f98a): FindChild(id) on the Toa node [0x8390a8] (0x47f9b6) found -> return, nothing changes. Else
//   SetParentNode (0x47f9d3, no reference), the temporary +1 (0x47f9df), the node's copy +1 (push_back 0x419890 -> copy
//   ctor 0x419b00, inc at 0x419b12), the temporary -1 (0x47f9f9), AddCollisionPhysics (0x47fa1d; no reference on the
//   character), AddTarget, then SetCharacter: the same character -> nothing (0x54b33e); else +1 on it (0x54b344), -1 on
//   the old one (0x54b356), store (0x54b364). So a first show adds two lasting references (natives.md §2.4 names only
//   SetCharacter's).
// - another id: the main-guy node [0x83aa4c] (0x47fa41), the same FindChild / +1 / copy +1 / -1 (0x47fa7a / 0x47fa86 /
//   0x47fa94), sCharacterIDs, SetShadowType, AddTarget; no SetCharacter.
// - A push_back that reallocates (0x419a29) copies the node's existing children (+1 each) and frees the old buffer (-1
//   each). Only a script-visible child would write H lines for that pair: the Toa node only ever holds the one 'cnt1'
//   character, and only addtoa inserts into the map in this port, so no reachable show has a script-visible sibling. Not
//   modelled (INFERRED unreachable here; M3 when other characters load).
Variant n_md_show(NativeCall& c, void* user) {
  HostContext& ctx = host(user);
  ModelDirector& md = ctx.subsystem<ModelDirector>();
  const uint32_t id = w(c, 0);
  GcCharacter* ch = md.get(id);
  if (!ch) null_this("GcModelDirector::Show (the id is not in the model-director map, 0x47f99b)");
  show_character(ctx, id, ch);
  return nothing();  // 0x47cd9b
}

// body 0x4093c0, method 0x418850 <GcCharacter::SetupMoveable> -> 0x574b80 <GcMotionSystem::SetupMoveable> -> 0x589050
// <GcMotionSystem::SetUnderWater> -> 0x60b320 <Call> (0x589154 / 0x589249); asm pin: 0x574ef1 the guard [ms+0x14] ==
// MainCharacterID(), 0x574ef5 the byte [0x73c5a8], 0x58907b / 0x589194 push 0x36, 0x58908b / 0x5891a1 push 1 / 0.
// A null character faults (0x418856). The motion system exists for every character (the constructor builds it,
// 0x40beeb); its id is the controller id (0x40c60b -> 0x5748ec). The other effects (motion flags, the collision handler
// at 0x574f46, the strcmp branches of 0x418883-0x4189c0) are not script-visible here.
Variant n_setupmoveable(NativeCall& c, void* user) {
  HostContext& ctx = host(user);
  GcCharacter* ch = ctx.object_arg<GcCharacter>(c, 0);
  if (!ch) null_this("GcCharacter::SetupMoveable (0x418856)");
  if (ch->motion_id() == main_character_id(ctx))
    ctx.engine_call("OnEvent", event_args(0x36, ctx.subsystem<GameGlobals>().global_under_water ? 1 : 0));  // 0x73df20
  return nothing();  // 0x409403
}

}  // namespace

// ---- characters an area loads and removes (natives.md §4, F7a) ----

// The body of GcModelDirector::Show 0x47f830 for a non-vine character (its comment at n_md_show above has the addresses).
void show_character(HostContext& ctx, uint32_t id, GcCharacter* ch) {
  ModelDirector& md = ctx.subsystem<ModelDirector>();
  std::set<uint32_t>& node = id == kIdCnt1 ? md.toa_node : md.mainguy_node;
  if (node.count(ch->node_id())) return;  // FindChild found it (0x47f9bb / 0x47fa53)
  ctx.add_ref(ch);   // the temporary ScAutoRefPtr
  node.insert(ch->node_id());
  ctx.add_ref(ch);   // the node's copy
  ctx.release(ch);   // the temporary released
  if (id == kIdCnt1 && md.ba_character != ch) {  // GcBAGroup::SetCharacter 0x54b330
    ctx.add_ref(ch);
    if (md.ba_character) ctx.release(md.ba_character);
    md.ba_character = ch;
  }
}

// GcModelDirector::Hide 0x47fb50 (non-vine: the vine test 0x47fbc5 jumps to 0x47fca0), called by Remove at 0x47f4e5 with the
// character Get(id) found (0x47fcb9; the area argument is unused on this path).
// - id == 'cnt1' (0x47fcf9-0x47fd06): GcBAGroup::SetCharacter(0) (0x47fce9): the old character -1 (0x54b356), the slot
//   nulled (0x54b364). Then the Toa node [0x8390a8]: FindChild(id) (0x47fd2a, virtual slot 0x40) not found -> return
//   (0x47fd2f); found -> the temporary +1 (0x47fd51), the search, the temporary -1 (0x47fd77-0x47fd82), SetParentNode(0)
//   (0x47fda0) and the child's erase (0x4197c0 at 0x47fdab: the shifted siblings' copies, then the last slot's -1).
// - another id: the same on the main-guy node [0x83aa4c] (0x47fdc6-0x47fe4f), then the id's entry is dropped from the list
//   [0x83a9c0] (0x47fe54-0x47fe76, ids only: no reference), then the node [0x838778] (0x47fe9f), which Show never fills
//   (no reference to it in Show 0x47f830-0x47fb4f): its FindChild finds nothing (INFERRED: the char is not in it).
// Not modelled: 0x418d00 (0x47ff29), 0x4745f0 (0x47fcd2) and 0x516050 (0x47ff3a), reached on this path with the character or
// its id: INFERRED reference-free (no Call in their closures; they take no ScAutoRefPtr). A child erase that shifts
// siblings (0x419813-0x419847: +1 on the moved copy, -1 on the overwritten one) writes H lines for a sibling a script
// holds: INFERRED unreachable in the tests of this port (one handle-holding child at a time); node order is not kept.
void hide_character(HostContext& ctx, uint32_t id, GcCharacter* ch) {
  ModelDirector& md = ctx.subsystem<ModelDirector>();
  if (id == kIdCnt1 && md.ba_character) {
    GcCharacter* old = md.ba_character;
    md.ba_character = nullptr;
    ctx.release(old);
  }
  std::set<uint32_t>& node = id == kIdCnt1 ? md.toa_node : md.mainguy_node;
  if (node.count(ch->node_id()) == 0) return;
  ctx.add_ref(ch);  // the temporary (0x47fd51 / 0x47fdf3)
  ctx.release(ch);  // the temporary released (0x47fd77 / 0x47fe1f)
  node.erase(ch->node_id());
  ctx.release(ch);  // the erase destroys the child's copy
}

GcCharacter* add_area_character(HostContext& ctx, uint32_t record_id, bool* modelled) {
  ModelDirector& md = ctx.subsystem<ModelDirector>();
  // LoadCharacters sets the id to '????' (0x53f426), so AddCharacter 0x47f040 asks GetUniqueIDForType (0x47f059-0x47f082):
  // the record id itself when no tracked type matches (0x480341 -> 0x480352); a numbered id otherwise (not modelled).
  *modelled = md.tracked.count(record_id) == 0;
  if (!*modelled) return nullptr;
  // new GcCharacter (0x47f084 / 0x47f0ca): character id = the record id (+0xfc, 0x40b728), controller id = the unique id
  // (+0x100, 0x40b73e); AddCharacter 0x47ec30 tracks it (0x47ec81) and Adds it under [char+0x100] (0x47ef81-0x47ef8f)
  GcCharacter* ch = ctx.make<GcCharacter>(md.character_type, record_id, record_id);
  md.tracked.insert(record_id);
  ctx.add_ref(ch);                                   // Add: temporary +1 (0x480ec7)
  const bool inserted = md.map.find(record_id) == md.map.end();
  if (inserted) {
    md.map[record_id] = ch;
    ctx.add_ref(ch);                                 // the map's copy (0x480ede)
  }  // a refused character stays tracked: the failure path 0x47ef9c-0x47efda calls no UnTrack (its dtor: INFERRED none)
  ctx.release(ch);                                   // the temporary -1 (0x480f08): a refused character is freed here
  return inserted ? ch : nullptr;                    // Add false -> return before 0x47efef (0x47ef98)
}

void remove_area_character(HostContext& ctx, uint32_t id) {
  ModelDirector& md = ctx.subsystem<ModelDirector>();
  // [0x83aa4c], the main-guy node, is set by GcModelDirector::Init (0x47d46d) before Setup: the 0x47f32c guard passes
  // (INFERRED: Init's node is non-null). Get(id) null -> return (0x47f365-0x47f376).
  GcCharacter* ch = md.get(id);
  if (!ch) return;
  // The vine branch (type info [0x838b44], 0x47f39e-0x47f420) needs a vine in the map: no native of this port inserts one.
  if (id != kIdCnt1) {
    auto t = md.tracked.find(ch->character_id());  // UnTrackUniqueBySlkType 0x480160 (0x47f48e)
    if (t != md.tracked.end()) md.tracked.erase(t);
  } else {
    md.s_toa = nullptr;                            // UnitializeAnimationEvents 0x480aa0, then sToa = 0 (0x47f4b0)
  }
  // CleanUp, the virtual call [esi+0x50] at 0x47f4be (GcCharacter::CleanUp 0x40e740 via the vtable 0x700908 set at 0x40b54b;
  // GcToa::CleanUp 0x56b5a0 via 0x73c464): no Call in either direct closure (102 / 146 functions); a virtual call inside them is
  // INFERRED not to reach the VM. Then Hide 0x47fb50 (0x47f4e5; the loader's Show 0x53f535 gave the character its node
  // child) and RemoveAnimationTriggers 0x4177c0 (0x47f4ee; INFERRED reference-free).
  hide_character(ctx, id, ch);
  md.map.erase(id);                                // the map erase 0x47f5a9, before the Call
  ctx.release(ch);                                 // the map's reference
  if (id == kIdCnt1) return;                       // [0x782884] (0x47f5b3, je 0x47f685): no Call for the Toa's key
  // Call("OnEvent" 0x70a8b6, byte 0x2b (CreateVariant 0x413ed0 at 0x47f5c7), int id (0x407df0 at 0x47f5de), 8 x nothing
  // (0x47f5e5-0x47f646)) at 0x47f680; an error propagates (no catch on Remove, CleanUpArea, RemoveArea or SetCurrentLevel)
  ctx.engine_call("OnEvent", event_args(0x2b, static_cast<int32_t>(id)));
}

namespace {

// ---- the camera ----

// The camera argument's handle (the state is keyed by it, objects.h). A null camera: the original reads [null+0x3a1]
// (0x44ed18) or calls through null (0x45b49b, 0x45b38b, 0x45b40b): a fault -> Undefined.
CameraState& camera(HostContext& ctx, NativeCall& c, const char* what) {
  const Variant v = *c.args.at(0);
  if (!is_game_type(v.type)) null_this(what);
  return ctx.subsystem<Cameras>().by_handle[v.value];
}

// body 0x44ed10, the field [cam+0x3a1] read directly (0x44ed18); asm pin: 0x44ed18 the byte, 0x44ed4a CreateVariant
// 0x44ed40's movzx; 0x44f6a0 the constructor's 0.
Variant n_getcameraaimode(NativeCall& c, void* user) {
  return make_int(camera(host(user), c, "GcLegoCamera::GetCameraAIMode (0x44ed18)").ai_mode);
}

// body 0x44ece0, method 0x45b490 <GcLegoCamera::SetCameraAIMode>; asm pin: 0x44ecee the low byte of w1, 0x45b4a7 the store,
// 0x45b558-0x45b5b6 the spline release: old in {2, 3} (old - 2 <= 1 unsigned) and new != [0x706fdc] (2) and new !=
// [0x706fe0] (3) and a spline held -> -1 (0x45b5a8) and dropped. The other effects (0x45b4b5-0x45b545: flags, the spline
// timer) are not script-visible here.
Variant n_setcameraaimode(NativeCall& c, void* user) {
  HostContext& ctx = host(user);
  CameraState& cam = camera(ctx, c, "GcLegoCamera::SetCameraAIMode (0x45b49b)");
  const uint8_t old = cam.ai_mode;
  const uint8_t now = static_cast<uint8_t>(w(c, 1));
  cam.ai_mode = now;
  if (static_cast<uint8_t>(old - 2) <= 1 && now != 2 && now != 3 && cam.spline) {
    GameObject* s = cam.spline;
    cam.spline = nullptr;
    ctx.release(s);
  }
  return nothing();  // 0x44ecf8
}

// SetCameraSpline 0x45b380 / SetTargetSpline 0x45b400 both hold the spline in [cam+0x3a4]: the same one -> nothing
// (0x45b391 / 0x45b411); else +1 on the new (0x45b397 / 0x45b417, null skipped), -1 on the old (0x45b3a9 / 0x45b429),
// store (0x45b3b7 / 0x45b437). The other fields (+0x3a8, +0x3b0, +0x3b8, +0x460...) are not read back by a native.
void hold_spline(HostContext& ctx, CameraState& cam, ScSLOSISLKCubicSpline* s) {
  if (cam.spline == s) return;
  if (s) ctx.add_ref(s);
  GameObject* old = cam.spline;
  cam.spline = s;
  if (old) ctx.release(old);
}

// body 0x44f180, method 0x45b380 <GcLegoCamera::SetCameraSpline>; asm pin: 0x45b38b the same-spline test, 0x45b397 +1,
// 0x45b3a9 -1, 0x45b3b7 the store.
Variant n_setcameraspline(NativeCall& c, void* user) {
  HostContext& ctx = host(user);
  CameraState& cam = camera(ctx, c, "GcLegoCamera::SetCameraSpline (0x45b38b)");
  hold_spline(ctx, cam, ctx.object_arg<ScSLOSISLKCubicSpline>(c, 1));
  return nothing();  // 0x44f1b9
}

// body 0x44f1e0, method 0x45b400 <GcLegoCamera::SetTargetSpline>; asm pin: 0x45b40b the same field [cam+0x3a4], 0x45b417
// +1, 0x45b429 -1, 0x45b437 the store.
Variant n_settargetspline(NativeCall& c, void* user) {
  HostContext& ctx = host(user);
  CameraState& cam = camera(ctx, c, "GcLegoCamera::SetTargetSpline (0x45b40b)");
  hold_spline(ctx, cam, ctx.object_arg<ScSLOSISLKCubicSpline>(c, 1));
  return nothing();
}

// ---- the HUD ----

// GcHudDirector::AddBigSprite 0x47b730 / AddTextObject 0x47b780: a new ScRenderNode holding the object (0x4ac7c4, +1 at
// 0x4ac7d3 unless null) goes to GcHudDirector::Add 0x47b940: id = sNextHudElementID (0x47b94f), the counter + 1
// (0x47b957-0x47b958), the map insert (0x47b989 -> 0x650500); success -> the id (0x47b9b0), failure -> 0 (0x47b995). The
// ids are unique, so the insert fails only after the counter wraps (INFERRED: GcIdentifierMap::insert refuses a taken key).
// The counter starts at 0 (static init 0x47cb54-0x47cb64) and the engine's own HUD adds (GcScreenTransition,
// GcConversationEngine, GcStatusScreen, GcAreaLoader::LoadArea's loading bar 0x4ff1b9 / 0x4ff25b / 0x4ff2a4) also advance it;
// the port counts only the script's adds, so the value is a cited placeholder (the taint ruling: type-only).
Variant hud_add(HostContext& ctx, GameObject* o) {
  HudDirector& hud = ctx.subsystem<HudDirector>();
  if (o) ctx.add_ref(o);  // the render node's reference (0x4ac7d3)
  const uint32_t id = hud.next_id++;
  hud.elements[id] = o;
  return make_int(static_cast<int32_t>(id));  // CreateVariant 0x407df0: {0x10, the id}
}

// body 0x47afe0, method 0x47b730 <GcHudDirector::AddBigSprite> -> 0x47b940 <GcHudDirector::Add>; asm pin: 0x4ac7d3 the
// node's +1 on the sprite, 0x47b94f-0x47b958 the id counter, 0x47b9b0 the id returned.
Variant n_addbigsprite(NativeCall& c, void* user) {
  HostContext& ctx = host(user);
  return hud_add(ctx, ctx.object_arg<GcBigSprite>(c, 0));
}

// body 0x47b010, method 0x47b780 <GcHudDirector::AddTextObject> -> 0x47b940 <GcHudDirector::Add>; asm pin: 0x47b7aa the
// node constructor, 0x4ac7d3 its +1, 0x47b9b0 the id returned.
Variant n_addtextobject(NativeCall& c, void* user) {
  HostContext& ctx = host(user);
  return hud_add(ctx, ctx.object_arg<ScTextObject>(c, 0));
}
}  // namespace

void bind_objects_group(NativeRegistry& reg, HostContext& ctx) {
  struct Row {
    const char* cls;
    const char* fn;
    NativeFn f;
  };
  static const Row rows[] = {
      {"sctilefont", "create", n_sctilefont_create},
      {"sctextobject", "create", n_sctextobject_create},
      {"gcbigsprite", "loadhudsprite", n_gcbigsprite_loadhudsprite},
      {"scslosislkcubicspline", "create", n_spline_create},
      {"gcmodeldirector", "addtoa", n_addtoa},
      {"gcmodeldirector", "gettoa", n_gettoa},
      {"gcmodeldirector", "getcharacter", n_getcharacter},
      {"gcmodeldirector", "show", n_md_show},
      {"gccharacter", "setupmoveable", n_setupmoveable},
      {"gclegocamera", "getcameraaimode", n_getcameraaimode},
      {"gclegocamera", "setcameraaimode", n_setcameraaimode},
      {"gclegocamera", "setcameraspline", n_setcameraspline},
      {"gclegocamera", "settargetspline", n_settargetspline},
      {"gchuddirector", "addbigsprite", n_addbigsprite},
      {"gchuddirector", "addtextobject", n_addtextobject},
  };
  // The GcToa game type: gcmodeldirector.gettoa's declared return (types.json "type:GcToa").
  if (const NativeEntry* e = reg.find("gcmodeldirector", "gettoa")) ctx.subsystem<ModelDirector>().toa_type = e->ret_type;
  // The GcCharacter game type (gcmodeldirector.getcharacter's declared return): area-loaded characters are minted under it.
  if (const NativeEntry* e = reg.find("gcmodeldirector", "getcharacter"))
    ctx.subsystem<ModelDirector>().character_type = e->ret_type;
  for (const Row& r : rows)
    if (reg.find(r.cls, r.fn)) reg.bind(std::string(r.cls) + "." + r.fn, r.f, &ctx, Tier::Implemented);
}

}  // namespace lomn::osi
