// M2a-2 T3: ScriptHost, the headless engine host of the script (natives.md §6, spec §3.1): Setup, then one
// ScProcessManager frame at a time (the fake time step, the `K` records, that frame's KEYSPEC Input calls, then Process
// with the re-run and skip rules). The frame loop is the one M2a-1 T4 wrote in lomn_osi_run.cpp, moved here unchanged.
// No frame-hook API (C3): a host source is added only when natives.md §6 names one on the path.
#pragma once
#include <cstdint>
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
  uint64_t first_seq = 0, calls = 0;
  Tier tier = Tier::Stub;
};

struct HostStats {
  uint64_t instructions = 0, natives = 0, setup_instructions = 0;
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

class ScriptHost {
 public:
  struct Options {
    HostFlags flags;
    std::string keys;
    bool real_natives = false;  // bind every registered native group (bind_natives); otherwise all typed stubs
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
  long frame_ = 0;       // the next frame number
  long skip_frame_ = -2;
  long current_frame_ = -1;  // the frame being run (-1 before the first)
  bool setup_done_ = false;
  double time_ = 0.0;    // PM+8
  uint32_t counter_ = 0; // PM+0x44
  mutable HostStats stats_;
};

}  // namespace lomn::osi
