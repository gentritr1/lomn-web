// M2a-2 T3: ScriptHost, the headless engine host of the script (natives.md §6, spec §3.1): Setup, then one
// ScProcessManager frame at a time (the fake time step, the `K` records, that frame's KEYSPEC Input calls, then Process
// with the re-run and skip rules). The frame loop is the one M2a-1 T4 wrote in lomn_osi_run.cpp, moved here unchanged.
// No frame-hook API (C3): a host source is added only when natives.md §6 names one on the path.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "lomn/osi/heap.h"
#include "lomn/osi/natives.h"
#include "lomn/osi/registry.h"
#include "lomn/osi/trace.h"
#include "lomn/osi/vm.h"
#include "lomn/sage/osi.h"

namespace lomn::osi {

struct NativeHit {
  long first_frame = -1;  // -1 = Setup
  uint64_t first_seq = 0, calls = 0;  // calls: every attempt, including those that failed before the native's entry
  bool first_pre_entry = false;  // T10: the first hit failed before entry; it sorts before an entered hit of the same seq
  Tier tier = Tier::Stub;  // the registry row's tier (the merged overrides): what the row DECLARES
  bool bound = false;      // whether a body is bound (fix 1, I-1): a tier1 row run as its stub shows bound = false
};

struct HostStats {
  uint64_t instructions = 0, natives = 0, setup_instructions = 0;
  uint64_t unregistered_0x83 = 0;  // fix 1 (m1): 0x83 calls of an unregistered name (each also counted in `natives`)
  int max_nesting = 0;        // D15: the engine-call nesting high-water (the outermost call counts 1)
  uint32_t stack_high_water = 0;
  std::map<std::string, NativeHit> hits;  // by folded registry key "cls.name"
  // the frame loop's own counts (lomn_osi_run prints them; the oracle prints the same line)
  long frames = 0;  // frames run (a frame that stopped on an error counts)
  uint64_t passes_total = 0, max_passes = 0, frames_rerun = 0, frames_skipped = 0;
  std::vector<std::string> first_reruns, first_skips;  // at most 5 each: "frame:passes" / "frame"
  std::string stop = "none";  // or `error "<message>" in Setup` / `error "<message>" at frame k`
};

// KEYSPEC (consult §4.1; tools/osi/keys.py has the same expansion): a tap is two frames, state 2 on frame f and state 1 on
// frame f+1. "down"/"enter": Input('cnt1', VK, state, 0.0, 0.0); "esc": Input('cnt1', 251, state).
struct KeyCall {
  int frame = 0;
  std::vector<Variant> args;
};
bool expand_keys(const std::string& spec, std::vector<KeyCall>& out, std::string* err);

// The fake-time step: fadd qword at the precision-control state (24 or 64 bits, D8'), then fstp qword.
double fadd_pc(double a, double b, int pc);

// ---- M2a-3 T4a: the host sources (engine/src/osi/natives/host_sources.cpp; hud.md §1, §2, §5, §9, §11) ----

// MOUSESPEC (hud.md §11; tools/osi/keys.py expand_mouse): `x,y@f` (0..65535 each) and `lmb|rmb|mmb|xmb` + `+`/`-` `@f`.
struct MouseSpec {
  std::map<long, std::pair<int32_t, int32_t>> pos;  // frame -> cursor
  std::array<std::map<long, bool>, 4> button;        // buttons 1..4: frame -> down
  bool any = false;                                  // a token was given (no MOUSESPEC -> no mouse source)
};
// false (and *err) on a malformed token, a negative frame, a coordinate outside 0..65535, an unknown button, or two
// tokens of one kind (the position, or one button) at one frame.
bool parse_mouse(const std::string& spec, MouseSpec& out, std::string* err);
struct MouseState {
  int32_t x = 0, y = 0;  // (0, 0) before the first position token (a host choice, hud.md §11)
  std::array<bool, 4> down{};
};
// The mouse source's calls of one frame (GcMouseDriver::Process 0x55c160): OnEvent(26, x, y) every frame, then for
// buttons 1..4 OnEvent(23, b) on the first frame down, 24 while held, 25 on the first frame up. Each entry is the
// argument list (ints); `st` carries the cursor and the buttons from frame to frame. Nothing when !spec.any.
std::vector<std::vector<Variant>> mouse_frame(const MouseSpec& spec, MouseState& st, long frame);
// keys.py --mouse "MOUSESPEC" N: one `<frame> OnEvent <v> ...` line (with LF) per call of frames 0..frames-1.
std::string mouse_listing(const MouseSpec& spec, long frames);

// The viewport camera's step (GcLegoCamera::Process 0x451990, modes 2 and 3; hud.md §1.3) at pass time `time`, after the
// script's Process (priority 3, §2.4). A guard that fires sets start = time and calls OnEvent(6, cam, 0) through
// HostContext::engine_call (natives.log `C -1`). Returns false only when that call raised an error.
bool camera_process(HostContext& ctx, double time);

// The frame's `Z` lines (hud.md §9.2), sorted bytewise, without the `Z %6d` header: `Z ref` for every live game handle,
// the `Z camera.*` lines when the viewport camera exists, `Z cs.playing` after the first playcinematic.
std::vector<std::string> snapshot_lines(HostContext& ctx);

class ScriptHost {
 public:
  struct Options {
    HostFlags flags;
    std::string keys;
    std::string mouse;  // M2a-3 T4a: MOUSESPEC (hud.md §11); empty = no mouse source
    bool real_natives = false;  // bind every registered native group (bind_natives); otherwise all typed stubs
    // M2a-2 T7a: the real-time clock in seconds (GetTime(1) 0x4fa460 = timeGetTime() * 0.001). Read only when the host is
    // not deterministic and the script has switched UseRealDeltaTime on (natives.md D3); empty = std::chrono::steady_clock.
    std::function<double()> clock;
  };
  // The tables are the JSON texts of data/osi (natives.json, natives_beta150.json, types.json, native_overrides.json;
  // overrides may be empty). Throws std::runtime_error on a malformed table, a bad KEYSPEC or an override that disagrees
  // with the merged tier of its row (natives.md §7.2).
  ScriptHost(const sage::OsiFile& script, std::string_view natives_json, std::string_view mods_json,
             std::string_view types_json, std::string_view overrides_json, const fs::FileSystem* vfs, TraceSink* trace,
             ReturnLog* log, Options opt);
  ~ScriptHost();
  CallResult setup();  // Call("Setup")
  bool frame();        // one ScProcessManager frame; false once an error has stopped the run
  const HostStats& stats() const;
  Vm& vm() { return *vm_; }
  HostContext& context() { return *ctx_; }
  NativeRegistry& registry() { return reg_; }
  Heap& heap() { return *heap_; }
  const TypeTable& types() const { return types_; }

 private:
  const sage::OsiFile& script_;
  TraceSink* trace_;
  ReturnLog* log_;
  Options opt_;
  TypeTable types_;
  NativeRegistry reg_;
  std::unique_ptr<Heap> heap_;
  std::unique_ptr<Vm> vm_;
  std::unique_ptr<HostContext> ctx_;
  std::vector<KeyCall> keys_;
  size_t kc_ = 0;
  MouseSpec mouse_;
  MouseState mouse_state_;
  long frame_ = 0;       // the next frame number
  long skip_frame_ = -2;
  long current_frame_ = -1;  // the frame being run (-1 before the first)
  bool setup_done_ = false;
  double time_ = 0.0;    // PM+8
  uint32_t counter_ = 0; // PM+0x44
  mutable HostStats stats_;
};

}  // namespace lomn::osi
