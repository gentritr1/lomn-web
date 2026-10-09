// M2a-2 T7a: the "services and time" native group (docs/osi/natives.md; citations on each binding in
// src/osi/natives/services.cpp): string <-> identifier, file-exists, GcTimer.create, UseRealDeltaTime, the game
// resolution, and the state these natives share with the host (HostContext::subsystem<T>()).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "lomn/osi/natives.h"
#include "lomn/sage/blockchain.h"

namespace lomn::osi {

// The C string a native's `string` argument is read as. The original passes the dynamic string's data pointer to
// ScFixedString::assign(p, npos) (0x401850; npos = [0x726a44] = 0xffffffff), whose npos branch is strlen (0x401881 ->
// 0x6573c0): the bytes up to the first NUL. It throws kOutOfRange when that length is >= the capacity 255 (cmp/setae
// 0x401894-0x401897, SrThrow call 0x4018a8). A legal 0x16 string is at most 254 bytes, so the throw is unreachable from
// a script; the model keeps it.
std::string osi_cstring(const std::string& text);

// ScOSISystem::ConvertStringToID 0x5fb060: a string of exactly 4 bytes packs first byte in the high byte; any other
// length gives 0xffffffff (0x5fb06e-0x5fb075). `cstr` is the string after osi_cstring.
uint32_t convert_string_to_id(const std::string& cstr);
// ScSLOSIFoundation::IdentifierToString 0x6178c0: the 4 bytes of the id, high byte first (a pure byte swap, no table),
// cut at the first NUL by the copy into the result string (0x617971 -> assign(p, npos)).
std::string identifier_to_string(uint32_t id);

// GcTimer (natives.md §2.4: constructed by the native). Fields are those GcTimer::GcTimer 0x445840 and GcTimer::Start
// 0x445990 write; the constants stored by the constructor are all 0.0 in .data and not kept. Start's substitution of the
// duration by [+0x70] when the duration is 0.0 (0x4459a0-0x4459c3) reads a field the constructor never writes (the object
// comes from SrMalloc 0x4e4350, not cleared): undefined in the original, modelled as no substitution. The host has no
// trigger source in M2a-2 (natives.md §6.2, C3): a started timer never fires.
class GcTimer : public GameObject {
 public:
  // `start` is the constructor's byte argument (low byte of the native's second argument, 0x445523 / 0x445906).
  GcTimer(uint16_t type, int32_t id, bool start, double duration);
  int32_t id() const { return id_; }                 // [timer+0xc], the GcEventTrigger event id
  double duration() const { return duration_; }      // [timer+0x68], a double
  bool started() const { return trigger_added_; }    // [timer+0x78], set by Start's AddTrigger call (0x4459f1)

 private:
  int32_t id_;
  double duration_;
  bool trigger_added_ = false;
};

// ScProcessManager::UseRealDeltaTime 0x4a90e0 stores its byte argument in [0x712d78]. Under --deterministic the native
// leaves that byte at 0 (D3: the fake-time path stays on) and only records the request. The smoothing is the real-time
// branch of ScProcessManager::Process 0x4a9170 (0x4a9200-0x4a92f2) with the constants at 0x712db0-0x712de0.
struct RealDeltaTime {
  explicit RealDeltaTime(HostContext&) {}
  bool requested = false;  // the last argument's low byte != 0, recorded even when deterministic
  bool effective = false;  // [0x712d78]
  // [0x712d70] (smoothed dt), [0x712d88] (frames counted), [pm+0x10] (previous real time, 0 in .bss until the first step)
  double smoothed = 0.0;
  int32_t count = 0;
  double last_real = 0.0;
  // One real-time step at `now` (seconds, GetTime(1) 0x4fa460 = timeGetTime() * 0.001): returns the smoothed dt that
  // replaces the fake 0x712d68 (and the caller sets the process time to `now`). Plain double arithmetic: the x87
  // precision-control rounding of the fake path (fadd_pc) is not modelled, this mode is non-deterministic by definition.
  double step(double now);
};

// GcGame::GetResolutionWidth / Height 0x43ae50 / 0x43ae70: [[0x70fa64] + 0x10] and [+0x14], floats. The screen's size is
// the INI's ResolutionW/H (INI -> M2a-3); until then the default INI values (640, 480).
struct DisplayState {
  explicit DisplayState(HostContext&) {}
  float width = 640.0f;
  float height = 480.0f;
};

// SrBlockFileExists 0x54ddc0 over the VFS: Art.blk (0x83b0b8, "Root/Data/Art/Art.blk") first, then the current/second
// block files (the slots [0x736128] / [0x73612c]; both 0 at every call on the Base.osi path), then FileExists 0x54de70
// on the mounted volumes only while no current block file is set. Reuses sage::BlockChain::find_exists. Art.blk is
// opened on first use (GcGame::LoadArtBlockHeader 0x43b030 has run before Setup).
class BlockFiles {
 public:
  explicit BlockFiles(HostContext& c);
  bool exists(const std::string& name);
  sage::BlockChain* chain() { return chain_.get(); }  // null without a VFS

 private:
  std::unique_ptr<sage::BlockChain> chain_;
  bool art_tried_ = false;
};

// ScSLOSIFileIO::DoesFileExist as BetterSaver hooks it (0x10001da0; natives.md §9.3), on the C string `name`.
bool does_file_exist(HostContext& ctx, const std::string& name);

}  // namespace lomn::osi
