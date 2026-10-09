// M2a-1 T1: the VM's engine-call protocol (opcodes.md "Engine calls"), the Run loop, error recording (D10) and the
// helpers the handlers share. The handlers themselves are in vm_handlers.cpp.
#include "lomn/osi/vm.h"

#include <cstdio>
#include <exception>

namespace lomn::osi {

Vm::Vm(const sage::OsiFile& script, NativeRegistry& natives, Heap& heap, TraceSink* trace)
    : script_(script), img_(script.bytes()), natives_(natives), heap_(heap), trace_(trace) {}

void Vm::trace_line(const std::string& s) {
  if (trace_) trace_->line(s);
}

void Vm::push(Variant v) {
  stack_.push(v);
  if (stack_.depth() > high_water_) high_water_ = stack_.depth();
}
Variant Vm::pop() { return stack_.pop(); }
void Vm::pop_dispose() { stack_.pop_dispose(heap_); }

void Vm::set_pc64() {
  if (fpu_pc_ == 64) return;
  fpu_pc_ = 64;
  char b[24];
  std::snprintf(b, sizeof b, "P %08x 64", cur_pc_);
  trace_line(b);
}

const Variant* Vm::find_global(std::string_view name) const {
  auto it = globals_.find(NativeRegistry::fold(name));
  return it == globals_.end() ? nullptr : &it->second;
}

Variant& Vm::global(std::string_view name) { return globals_[NativeRegistry::fold(name)]; }

const NativeEntry* Vm::memo(uint32_t site_pc) const {
  auto it = memo_.find(site_pc);
  return it == memo_.end() ? nullptr : it->second;
}

VmState Vm::snapshot() const {
  VmState s;
  s.pc = pc_;
  s.depth = stack_.depth();
  s.frame = frame_;
  s.context_depth = context_depth_;
  s.refcount = refcount_;
  if (s.depth) s.top = stack_.top();
  s.halted = halted_;
  s.fpu_pc = fpu_pc_;
  return s;
}

// ScOSIScript::FindFunction (0x609ce0): the first of the (u8 count) entries whose name is a case-insensitive (A-Z) PREFIX
// of the query (__strnicmp over the entry's own length; a shorter query fails at its NUL).
const sage::OsiFunction* Vm::find_function(std::string_view q) const {
  const auto& fs = script_.functions();
  const size_t n = fs.size() & 0xff;
  auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c; };
  for (size_t k = 0; k < n; ++k) {
    const std::string& e = fs[k].name;
    bool ok = true;
    for (size_t i = 0; i < e.size(); ++i) {
      const unsigned char qc = i < q.size() ? static_cast<unsigned char>(q[i]) : 0;
      const unsigned char ec = static_cast<unsigned char>(e[i]);
      if (lower(qc) != lower(ec)) {
        ok = false;
        break;
      }
      if (qc == 0) break;  // a shared NUL ends the comparison
    }
    if (ok) return &fs[k];
  }
  return nullptr;
}

void Vm::record_error(const VmError& e) {
  VmError r = e;
  r.pc = cur_pc_;
  r.op = cur_op_;
  r.depth = stack_.depth();
  last_error_ = r;
  trace_line(error_line(r.pc, r.op, r.depth, r.message));
  set_pc64();  // the landing pad's fninit (D8')
}

CallResult Vm::call(std::string_view name, const std::vector<Variant>& args) {
  // Port cap (D10): never true for the outermost call, so the error always unwinds through the calling native's
  // 0x82/0x83 catch.
  if (nesting_ >= kMaxEngineNesting) throw VmError("Undefined: engine-call nesting limit (64)");
  const bool outer = nesting_ == 0;
  if (outer) {
    last_error_.reset();
    swallowed_.clear();
    saved_ = {stack_.depth(), pc_, frame_, context_depth_, refcount_};
  }
  if (++nesting_ > max_nesting_) max_nesting_ = nesting_;  // D15: the measured maximum M
  try {
    CallResult r = call_impl(name, args);
    --nesting_;
    return r;
  } catch (const VmError& e) {
    --nesting_;
    if (!outer) throw;
    record_error(e);
  } catch (const std::exception& e) {
    --nesting_;
    if (!outer) throw;
    record_error(VmError(std::string("Undefined: host exception: ") + e.what()));
  } catch (...) {
    --nesting_;
    if (!outer) throw;
    record_error(VmError("Undefined: host exception: unknown"));
  }
  CallResult r;
  r.found = true;
  r.ok = false;
  return r;
}

// CallWithReturn (0x60b450) + SetPCAndArgs (0x60b5e0), then Call's disposal of the result (0x60b417).
CallResult Vm::call_impl(std::string_view name, const std::vector<Variant>& in) {
  if (in.size() > 10) throw VmError("Undefined: more than 10 engine-call arguments");
  Variant a[10];
  for (size_t i = 0; i < in.size(); ++i) a[i] = in[i];
  CallResult res;
  res.result = nothing();
  ++refcount_;
  push(Variant{kFrameMarker, pc_});
  frame_ = stack_.find_frame();
  ++context_depth_;
  const sage::OsiFunction* f = find_function(name);
  halted_ = false;
  if (!f) {
    trace_line("M " + fmt_name(name));  // Message only; Run skipped, the arguments neither pushed nor disposed
  } else {
    res.found = true;
    std::string c = "C " + fmt_name(name) + " " + std::to_string(f->argc);
    for (const Variant& v : a) c += " " + fmt_variant(v);
    trace_line(c);
    pc_ = f->offset;
    if (f->argc > 10) {
      cur_pc_ = pc_;
      cur_op_ = pc_ < img_.size() ? img_[pc_] : 0;
      throw VmError("Too many args required (max 10) to call requested entry point.");
    }
    for (uint32_t i = 0; i < 10; ++i) {
      if (i < f->argc) push(a[i]);
      else heap_.dispose(a[i]);
    }
    run();
    res.result = pop();
  }
  Variant m;
  do {
    m = pop();
    heap_.dispose(m);
  } while (m.type != kFrameMarker);
  if (--context_depth_ != 0) frame_ = stack_.find_frame();
  pc_ = m.value;
  --refcount_;
  res.ok = true;
  if (!res.found) return res;  // no R line: the call did not happen (only the M line)
  trace_line("R " + fmt_variant(res.result));
  if (res.result.type == kString && heap_.live(kKindString, res.result.value)) res.result_text = heap_.text(res.result.value);
  heap_.dispose(res.result);  // Call disposes the result (0x60b417); our host does the same for CallWithReturn
  return res;
}

// Must not throw (fix round 1, M7): a dangling handle met while disposing is recorded in internal_errors() and the
// slot is dropped; the trace gets no record for it (it is our bookkeeping, not VM behaviour).
void Vm::reset_after_error() {
  if (!last_error_) return;
  while (stack_.depth() > saved_.depth) {
    const Variant v = stack_.pop();  // depth > saved depth >= 0: cannot underflow
    try {
      heap_.dispose(v);
    } catch (const VmError& e) {
      internal_errors_.push_back("reset_after_error: " + e.message);
    } catch (const std::exception& e) {
      internal_errors_.push_back(std::string("reset_after_error: ") + e.what());
    }
  }
  context_depth_ = saved_.context;
  refcount_ = saved_.refcount;
  pc_ = saved_.pc;
  frame_ = saved_.frame;
  nesting_ = 0;
  active_seq_.clear();
}

// Run (0x60b850): fetch, trace, dispatch until a handler returns false.
void Vm::run() {
  for (;;) {
    const uint32_t pc = pc_;
    cur_pc_ = pc;
    if (pc >= img_.size()) {
      cur_op_ = 0;
      throw VmError("Undefined: PC outside the script");
    }
    const uint8_t op = img_[pc];
    cur_op_ = op;
    if (trace_) {
      char b[48];
      std::snprintf(b, sizeof b, "I %08x %02x %4u %4u ", pc, op, stack_.depth(), frame_);
      std::string l = b;
      l += stack_.depth() ? fmt_variant(stack_.top()) : std::string(kEmptySlot());
      trace_->line(l);
    }
    ++instructions_;
    if (!dispatch(op)) break;
  }
}

uint8_t Vm::op_u8(uint32_t off) const {
  const uint64_t at = static_cast<uint64_t>(cur_pc_) + off;
  if (at >= img_.size()) throw VmError("Undefined: operand runs past the end of the script");
  return img_[at];
}
uint16_t Vm::op_u16(uint32_t off) const { return static_cast<uint16_t>(op_u8(off) | (op_u8(off + 1) << 8)); }
uint32_t Vm::op_u32(uint32_t off) const { return op_u16(off) | (static_cast<uint32_t>(op_u16(off + 2)) << 16); }

std::string Vm::str_entry(uint32_t idx) const {
  const auto& s = script_.strings();
  if (idx >= s.size()) throw VmError("Undefined: string-table index out of range");
  return s[idx];
}

std::string Vm::symbol_name(uint32_t idx) const {
  const auto& s = script_.symbols();
  return idx < s.size() ? s[idx] : std::string();
}

std::string Vm::string_text(Variant s) {
  if (s.type == kStringId) return str_entry(s.value & 0xffff);
  return heap_.text(s.value);
}

// ConvertStringIDToDynamicString (0x60b880): entry value & 0xffff; its string ctor (0x60b8d0) throws kOutOfRange at 255+.
Variant Vm::to_dynamic_string(Variant v) {
  std::string t = str_entry(v.value & 0xffff);
  const size_t z = t.find('\0');
  if (z != std::string::npos) t.resize(z);
  if (t.size() >= 255) throw VmError("kOutOfRange");
  return Variant{kString, heap_.new_string(t)};
}

Variant& Vm::frame_slot(uint32_t idx) {
  const uint64_t i = static_cast<uint64_t>(frame_) + idx;
  if (i >= stack_.depth()) throw VmError("Undefined: frame slot above the top of the stack");
  return stack_.at(static_cast<uint32_t>(i));
}

// 0x21/0x22/0x24/0x25 addressing: bit 15 selects a global (only idx & 0xff is used: GetGlobalVariable 0x609d90), whose
// name is looked up system-wide (created as nothing when absent, 0x609e10); otherwise framePtr[idx].
Variant& Vm::var_ref(uint16_t idx) {
  if (idx & 0x8000) {
    const auto& g = script_.globals();
    const uint32_t k = idx & 0xff;
    if (k >= g.size()) throw VmError("Undefined: global index out of range");
    return global(g[k]);
  }
  return frame_slot(idx);
}

const sage::OsiClass& Vm::class_of(uint32_t obj) {
  const uint16_t c = heap_.class_id(obj);
  if (c >= script_.classes().size()) throw VmError("Undefined: class index out of range");
  return script_.classes()[c];
}

// FindMethod (0x608600): the first method record with that symbol; "Call to undefined member method" (SrOSIError).
uint32_t Vm::find_method(uint32_t obj, uint16_t sym, const char* undefined_msg) {
  for (const auto& m : class_of(obj).methods)
    if (m.symbol == sym) return m.offset;
  throw VmError(undefined_msg);
}

// FindMemberID (0x609ba0): the query is lower-cased (tolower table 0x762a18, A-Z only, into a static buffer) and
// strcmp'd first against the script's lookup cache {symbol text, id} (count at script+0x1d8, entries from +0x48), then
// against each raw symbol-table entry. A cache HIT returns its id with no insert (0x609c51-0x609c55). Every cache MISS
// inserts {the last symbol compared, the id found} (0x609ca7-0x609cc8); when the symbol scan finds nothing the id is the
// initial 0xffff (0x609baf), so the LAST symbol's text is cached as 0xffff and later lookups of it fail (poisoning,
// opcodes.md row 0x18). The cache has no bound in the original: the 51st insert (entry 50) lands on the count at +0x1d8,
// which we raise as undefined behaviour. With an empty symbol table the cached pointer is null; a later lookup would
// strcmp a null pointer, also raised.
uint16_t Vm::find_member_id(const std::string& name) {
  std::string low = NativeRegistry::fold(name);
  const size_t z = low.find('\0');
  if (z != std::string::npos) low.resize(z);
  for (const MemberCache& c : member_cache_) {
    if (c.symbol < 0) throw VmError("Undefined behaviour: FindMemberID cache entry without a symbol");
    if (upto_nul_copy(script_.symbols()[static_cast<size_t>(c.symbol)]) == low) return c.id;
  }
  const auto& s = script_.symbols();
  int last = -1;
  uint16_t id = 0xffff;
  for (size_t i = 0; i < s.size() && i < 0xffff; ++i) {
    last = static_cast<int>(i);
    if (upto_nul_copy(s[i]) == low) {
      id = static_cast<uint16_t>(i);
      break;
    }
  }
  if (member_cache_.size() >= 50) throw VmError("Undefined behaviour: FindMemberID cache overflow (51st miss)");
  member_cache_.push_back({last, id});
  return id;
}

// ---- M2a-2 T3: the native framework's view of the VM ----
uint64_t Vm::native_seq() const { return active_seq_.empty() ? 0 : active_seq_.back(); }

uint16_t Vm::member_id(std::string_view text) { return find_member_id(std::string(text)); }

// D16 rules 1-2 (natives.md §5.4). Unwinding ran no VM code, so cur_pc_/cur_op_ still name the instruction that raised
// (or the 0x82/0x83/0x84 that re-raised, rule 15(b)) and the stack is as at the throw.
void Vm::swallow(const VmError& e) {
  Swallowed s;
  s.error = e;
  s.error.pc = cur_pc_;
  s.error.op = cur_op_;
  s.error.depth = stack_.depth();
  s.has_top = stack_.depth() > 0;
  if (s.has_top) s.top = stack_.top();
  trace_line(error_line(s.error.pc, s.error.op, s.error.depth, s.error.message));
  set_pc64();  // the catch pad's fninit (0x657245)
  swallowed_.push_back(s);
}

// natives.md §8.3: a 0x11 name is read C-string style from the entry's text in the image (ScOSIScript's string pointer
// table points at the bytes after the length byte, 0x609634-0x60966c; the terminator byte is never checked), so a
// hostile entry whose terminator is not NUL reads on into the next entry. 0x16: its own length.
std::string Vm::name_copy(Variant s) {
  if (s.type == kStringId) {
    const uint32_t idx = s.value & 0xffff;
    const auto& offs = script_.string_offsets();
    if (idx >= offs.size()) throw VmError("Undefined: string-table index out of range");
    std::string t;
    for (size_t at = static_cast<size_t>(offs[idx]) + 1;; ++at) {
      if (at >= img_.size()) throw VmError("Undefined: string-table entry runs past the end of the script");
      const char c = static_cast<char>(img_[at]);
      if (c == 0) break;
      t += c;
      if (t.size() > 255) throw VmError("kOutOfRange");  // len > capacity 0xff (seta 0x4f5c27); strlen would go on
    }
    return t;
  }
  const std::string& t = heap_.text(s.value);
  if (t.size() > 255) throw VmError("kOutOfRange");  // seta 0x4f5b16
  return upto_nul_copy(t);
}
std::string Vm::error_line(uint32_t pc, uint8_t op, uint32_t depth, const std::string& msg) const {
  char b[40];
  std::snprintf(b, sizeof b, "E %08x %02x %4u \"", pc, op, depth);
  return std::string(b) + fmt_message(msg) + "\"";
}

// The binary's "(type:%02.2X)" messages: the token is replaced by the type in at least 2 upper-case hex digits.
std::string Vm::upto_nul_copy(const std::string& s) {
  const size_t z = s.find('\0');
  return z == std::string::npos ? s : s.substr(0, z);
}

std::string Vm::fmt_err(const char* fmt, uint32_t type) const {
  std::string s(fmt);
  const size_t at = s.find("%02.2X");
  if (at == std::string::npos) return s;
  char b[16];
  std::snprintf(b, sizeof b, "%02X", type);
  return s.replace(at, 6, b);
}

}  // namespace lomn::osi
