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

// The C string a native's `string` argument is read as: the bytes up to the first NUL. Exact for the natives whose wrapper
// copies the string with ScFixedString::assign(p, npos) (StringToIdentifier 0x617810, via 0x401850: npos = [0x726a44] =
// 0xffffffff selects strlen, 0x401881 -> 0x6573c0, and n >= the capacity 255 throws kOutOfRange, cmp/setae 0x401894-0x401897,
// SrThrow call 0x4018a8). DoesFileExist does NOT assign: its chain passes the raw data pointer (0x615ef6 -> 0x54ddc0), a C
// string with no 255 test of its own. A legal 0x16 string is at most 254 bytes, so the throw is unreachable from a script
// and the two readings agree; the model applies this one function to both.
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
  bool started() const { return trigger_added_; }    // [timer+0x78], set after Start's AddTrigger call (0x4459f1)

 private:
  int32_t id_;
  double duration_;
  bool trigger_added_ = false;
};

// ScProcessManager::UseRealDeltaTime 0x4a90e0 stores its byte argument in [0x712d78]. Under --deterministic the native
// leaves that byte at 0 (D3: the fake-time path stays on) and only records the request. ScProcessManager::Process 0x4a9170
// reads the real clock (GetTime(1), 0x4a91fe-0x4a9200) and runs the blend and clamp (0x4a920c-0x4a929f, constants at
// 0x712db0-0x712de0) on EVERY pass; the byte is read once, at 0x4a92a5, and only chooses which dt and process time are used
// (0x4a92ae-0x4a92e3). So outside --deterministic step() runs every frame and `effective` selects.
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
// on the mounted volumes only while no current block file is set. Reuses sage::BlockChain::find_exists over the host's
// ONE chain, HostContext::block_chain() (shared with every native that sets the current block file, m-7).
class BlockFiles {
 public:
  explicit BlockFiles(HostContext& c) : ctx_(c) {}
  bool exists(const std::string& name);
  sage::BlockChain* chain() { return ctx_.block_chain(); }  // null without a VFS

 private:
  HostContext& ctx_;
};

// ScSLOSIFileIO::DoesFileExist as BetterSaver hooks it (0x10001da0; natives.md §9.3), on the C string `name`. A path that
// holds "User/" (strstr, case sensitive) is resolved by ijl15.dll's ResolveGamePath and tested with GetFileAttributesA: from
// "Root/" or "User/" at its start onto the game or the user directory; from anything else RELATIVE TO THE CURRENT
// DIRECTORY, i.e. the Root/ mapping, after a modal "OSI Script Security Abort" box (a host UI effect, not modelled and not
// logged: deferred to the UI milestones). Every other path goes to SrBlockFileExists.
bool does_file_exist(HostContext& ctx, const std::string& name);

}  // namespace lomn::osi
