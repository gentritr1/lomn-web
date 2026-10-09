"""M2a-1 T5 (A7) mutation bite check of the C++ OSI VM: each deliberate one-line defect must make the check it NAMES fail on a
line its EXPECT regex matches (the targeted corpus program, unit case or trace record). An unrelated failure is NOT a bite
(the mutate4 lesson). Run ONLY in a scratch copy of the tree (it edits sources in place and restores them):

    python3 tools/research/oracle/mutate_vm.py <scratch_tree> [--only 1-33,C05,S02] [--list]

<scratch_tree> needs build/native configured (`cmake -S engine -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release
-DLOMN_WEBGPU=OFF`: the checks need no Dawn), LOMN_DATA in the environment (the `diff` check reads Base.osi), python3 on
PATH and .venv. Each mutant: apply, build only the targets its checks run, run every named check, restore, `cmp` the
restored file against the pristine copy taken at start (build/mut-pristine/), rebuild. Checks (all exactly the repo's own):
  unit      build/native/lomn_tests -tc='osi*'                       fail lines: doctest "TEST CASE:" / "ERROR:"
  asan      build/native/lomn_tests_asan -tc='osi*' (ASan + UBSan)    same, plus sanitizer reports
  conform   ctest -R '^osi_conform$' (assembles the corpus, runs lomn_osi_conform; 521 programs)   "FAIL <program>: ..."
  tracecmp  the `make conform-trace-cmp` recipe (C++ vs Python corpus traces, cmp per file)        "DIFF <program>.trace"
  diff      ctest -R 'osi_a6_(digest|window)' (Base.osi, Setup + 600 Process frames, C++ vs the Python oracle); the
            first differing record of the window/digest/natives.log the ctest wrote is reported as
            "WINDOW line N after '<previous common line>': cpp='...' py='...'" so EXPECT can name the record
  rerun     ctest -R osi_a6_rerun (the Process re-run / skip rule, both engines, the hand-computed pass-count line)
The wasm32 leg is not run: no mutant here targets wasm-only behaviour (native == wasm32 is `make osi-diff`'s job).
A mutant is CAUGHT when at least one named check bites; the per-check outcome is printed (BITE, pass, OFF-TARGET = failed
but no line matches EXPECT, TIMEOUT). A build failure is BUILD-FAILED (an invalid mutant), never CAUGHT. The no-op control
must be MISSED by every check. Logs: build/mutlogs/<id>-<check>.log, results: build/mutlogs/results.jsonl.
"""
from __future__ import annotations

import filecmp
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

VH = "engine/src/osi/vm_handlers.cpp"
VM = "engine/src/osi/vm.cpp"
CV = "engine/src/osi/convert.cpp"
HP = "engine/src/osi/heap.cpp"
RUN = "engine/tools/lomn_osi_run.cpp"
HOST = "engine/src/osi/host.cpp"  # M2a-2 T3: the frame loop moved from lomn_osi_run.cpp into ScriptHost

# (id, name, file, old, new, {check: EXPECT regex}). C01-C33 = consult §5 rows 1-33; S01.. = semantics settled after the
# consult (FindMemberID cache, atoi saturation, exact-zero sign, the site+6 marker, D8' switching, the Process rules ...).
M = [
    ("C01", "0x53: a float 0.0 condition branches instead of erroring", VH,
     "if (c.type == kNothing || (c.type == kInt && c.value == 0)) {",
     "if (c.type == kNothing || ((c.type == kInt || c.type == kFloat) && (c.value & 0x7fffffffu) == 0)) {",
     {"conform": r"FAIL a/0x53-err-float-zero"}),
    ("C02", "0x53: nothing is truthy (falls through)", VH,
     "if (c.type == kNothing || (c.type == kInt && c.value == 0)) {\n    pc_ = cur_pc_ + 3 + d;\n  } else if (c.type == kInt) {",
     "if (c.type == kInt && c.value == 0) {\n    pc_ = cur_pc_ + 3 + d;\n  } else if (c.type == kInt || c.type == kNothing) {",
     {"conform": r"FAIL a/0x53-nothing-branches", "diff": r"after 'I \S+ 53 "}),
    ("C03", "0x50: target relative to PC instead of the file base", VH,
     "  if (t.type != kInt) throw VmError(\"Internal error: Jump to non-integral address\");\n  stack_.insert_below(n, Variant{kRetMarker, cur_pc_ + 2});\n  if (stack_.depth() > high_water_) high_water_ = stack_.depth();\n  frame_ = stack_.find_frame();\n  pc_ = t.value;\n  return true;\n}\n// ==== row 0x51",
     "  if (t.type != kInt) throw VmError(\"Internal error: Jump to non-integral address\");\n  stack_.insert_below(n, Variant{kRetMarker, cur_pc_ + 2});\n  if (stack_.depth() > high_water_) high_water_ = stack_.depth();\n  frame_ = stack_.find_frame();\n  pc_ = cur_pc_ + t.value;\n  return true;\n}\n// ==== row 0x51",
     {"conform": r"FAIL a/0x50-file-relative-target", "diff": r"after 'I \S+ 50 "}),
    ("C04", "0x51: return marker inserted above the arguments instead of below", VH,
     "BCLJumpAbsolute 0x600c30: the target is the tagged file offset from 0x12/0x17/0x19\n  pc_ = cur_pc_ + 2;  // T2a-pc: the error PC (+2); the jump target is written last\n  const uint32_t n = op_u8(1);\n  const Variant t = pop();\n  if (t.type != kInt) throw VmError(\"Internal error: Jump to non-integral address\");\n  stack_.insert_below(n,",
     "BCLJumpAbsolute 0x600c30: the target is the tagged file offset from 0x12/0x17/0x19\n  pc_ = cur_pc_ + 2;  // T2a-pc: the error PC (+2); the jump target is written last\n  const uint32_t n = op_u8(1);\n  const Variant t = pop();\n  if (t.type != kInt) throw VmError(\"Internal error: Jump to non-integral address\");\n  stack_.insert_below(0 * n,",
     {"conform": r"FAIL a/0x51-(three-arguments|method-receives-this)", "diff": r"after 'I \S+ 51 "}),
    ("C05", "0x52: the result is disposed before it is re-pushed", VH,
     "    if (m.type == kRetMarker) {\n      push(r);",
     "    if (m.type == kRetMarker) {\n      heap_.dispose(r);\n      push(r);",
     {"diff": r"cpp='F \w [0-9a-f]{8}' py='I ", "conform": r"FAIL [ab]/\S*(0x52|b-own)"}),
    ("C06", "0x14: the pad count excludes `this` (off by one)", VH,
     "uint32_t k = (stack_.depth() - frame_) & 0xff;",
     "uint32_t k = (stack_.depth() - frame_ - 1) & 0xff;",
     {"conform": r"FAIL a/0x14-(method-this-plus-padding|exact-count-no-change|err-too-many|pad-)", "diff": r"after 'I \S+ 14 "}),
    ("C07", "0x21/0x22: the global bit 0x8000 is ignored", VM,
     "  if (idx & 0x8000) {\n    const auto& g = script_.globals();",
     "  if (false && (idx & 0x8000)) {\n    const auto& g = script_.globals();",
     {"conform": r"FAIL a/0x2[12]-global", "diff": r"after 'I \S+ 2[12] "}),
    ("C08", "0x22: a store into a global does not dispose the old value", VH,
     "  Variant& t = var_ref(idx);\n  heap_.dispose(t);",
     "  Variant& t = var_ref(idx);\n  if (!(idx & 0x8000)) heap_.dispose(t);",
     {"conform": r"FAIL a/0x22-global-overwrite-disposes-old"}),
    ("C09", "0x33: top-n off by one", VH,
     "  if (n == 0 || n > stack_.depth()) throw VmError(\"Undefined: 0x33 operand outside the stack\");\n  const Variant d = heap_.duplicate(stack_.at(stack_.depth() - n));",
     "  if (n == 0 || n > stack_.depth()) throw VmError(\"Undefined: 0x33 operand outside the stack\");\n  const Variant d = heap_.duplicate(stack_.at(stack_.depth() - n + 1));",
     {"conform": r"FAIL b/b-33-pull", "diff": r"after 'I \S+ 33 "}),
    ("C10", "0x35: Dup deep-copies arrays", VH,
     "  const Variant d = heap_.duplicate(stack_.top());",
     "  const Variant d = stack_.top().type == kArray ? [&] { Variant c{kArray, heap_.new_array()}; for (const Variant& x : heap_.elements(stack_.top().value)) heap_.elements(c.value).push_back(heap_.duplicate(x)); return c; }() : heap_.duplicate(stack_.top());",
     {"conform": r"FAIL b/b-35-dup-array-alias"}),
    ("C11", "0x60: string compare is case-sensitive", VH,
     "eq = lower_ascii(static_cast<unsigned char>(x[i])) == lower_ascii(static_cast<unsigned char>(y[i]));",
     "eq = x[i] == y[i];",
     {"conform": r"FAIL b/b-60-string-fold"}),
    ("C12", "0x60: the int,float path rounds the int to f32 first", VH,
     "push(make_int(cmp_exact(widen(a), widen(b), Op::Eq)));",
     "push(make_int(cmp_exact(static_cast<double>(static_cast<float>(widen(a))), static_cast<double>(static_cast<float>(widen(b))), Op::Eq)));",
     {"conform": r"FAIL b/b-60-if-falsifier"}),
    ("C13", "0x70: a mixed add computed as (float)((double)a + (double)b)", VH,
     "    return Variant{kFloat, x87_arith(op, num_of(a), num_of(b), pc)};",
     "    return Variant{kFloat, op == Op::Add ? f32_bits(static_cast<float>(widen(a) + widen(b))) : x87_arith(op, num_of(a), num_of(b), pc)};",
     {"conform": r"FAIL a/0x70-d8-"}),
    ("C14", "0x70: a string overflow truncates instead of raising kLenTooLong", VH,
     "if (x.size() + y.size() >= 255) throw VmError(\"kLenTooLong\");",
     "if (x.size() + y.size() >= 255) { x += y; x.resize(254); push(a); heap_.dispose(b); pc_ = cur_pc_ + 1; return true; }",
     {"conform": r"FAIL a/0x70-err-string-(too-long-260|total-255)"}),
    ("C15", "0x72: int x int saturates (64-bit product clamped) instead of wrapping", VH,
     "    case Op::Mul: r = a.value * b.value; break;",
     "    case Op::Mul: { const int64_t w = static_cast<int64_t>(x) * y; r = w > INT32_MAX ? 0x7fffffffu : w < INT32_MIN ? 0x80000000u : static_cast<uint32_t>(w); } break;",
     {"conform": r"FAIL a/0x72-int-int-wraps"}),
    ("C16", "0x73: int / int rounds to nearest", VH,
     "      r = static_cast<uint32_t>(x / y);  // idiv: truncation toward zero",
     "      r = static_cast<uint32_t>(static_cast<int32_t>(std::lround(static_cast<double>(x) / y)));",
     {"conform": r"FAIL a/0x73-int-int-(truncates|negative)"}),
    ("C17", "0x73: a -0.0 divisor passes the zero test", VH,
     "bool float_zero(Variant v) { return (v.value & 0x7fffffffu) == 0; }",
     "bool float_zero(Variant v) { return v.value == 0; }",
     {"conform": r"FAIL a/0x73-err-float-divide-by-negative-zero"}),
    ("C18", "0x75: signed %", VH,
     "push(Variant{a.type, a.value % b.value});",
     "push(Variant{a.type, static_cast<uint32_t>(static_cast<int32_t>(a.value) % static_cast<int32_t>(b.value))});",
     {"conform": r"FAIL a/0x75-unsigned"}),
    ("C19", "0x78: arithmetic shift right", VH,
     "    case 7: r = x >> (y & 31); break;  // logical (0x605a76)",
     "    case 7: r = static_cast<uint32_t>(static_cast<int32_t>(x) >> (y & 31)); break;",
     {"conform": r"FAIL b/b-78-shr-logical"}),
    ("C20", "0x82: the min/max argument check is skipped", VH,
     "  if (check_argc) {\n    if (n > e.max)",
     "  if (false) {\n    if (n > e.max)",
     {"conform": r"FAIL b/(b-82-err-too-(few|many)|b-argc-|b-83-err-too-many)"}),
    ("C21", "0x82: an unsupported source conversion errors instead of giving {0, 0} (opcodes.md 0x82/0xb2: it does not error)", VH,
     "      if (v.type == kString) return make_int(osi_atoi(heap_.text(v.value)));\n      return Variant{kNone, 0};",
     "      if (v.type == kString) return make_int(osi_atoi(heap_.text(v.value)));\n      throw VmError(\"Requested variant conversion unimplimented.  Recompile to debug for types\");",
     {"conform": r"FAIL b/(b-82-nothing-to-int|b-b2-nothing)"}),
    ("C22", "0x82: the arguments are popped without dispose", VH,
     "  for (uint32_t i = 0; i < n; ++i) pop_dispose();  // 0x601c5f: top first",
     "  for (uint32_t i = 0; i < n; ++i) pop();  // 0x601c5f: top first",
     {"conform": r"FAIL b/b-82-(arg-dispose|int-to-string)"}),
    ("C23", "0x82: a by-ref native gets a copy of the slot, its writes are lost", VH,
     "    args[i] = &slot;",
     "    args[i] = byref[i] ? new Variant(slot) : &slot;  // (leaks the copy: harmless for the length of a mutant run)",
     {"conform": r"FAIL b/b-82-byref"}),
    ("C24", "0x82: the trace prints 84 after memoisation", VM,
     "\"I %08x %02x %4u %4u \", pc, op, stack_.depth(), frame_);",
     "\"I %08x %02x %4u %4u \", pc, memo(pc) ? 0x84 : op, stack_.depth(), frame_);",
     {"tracecmp": r"DIFF \./b/b-84-memo", "diff": r"cpp='I \S+ 84 "}),
    ("C25", "0x91: the index is compared signed", VH,
     "  if (i.value >= el.size()) throw VmError(\"Reference beyond start/end of array\");\n  const Variant d = heap_.duplicate(el[i.value]);",
     "  if (static_cast<int32_t>(i.value) >= static_cast<int32_t>(el.size())) throw VmError(\"Reference beyond start/end of array\");\n  const Variant d = heap_.duplicate(el.at(i.value));",
     {"conform": r"FAIL b/b-91-err-negative"}),
    ("C26", "0x96: insert at count is allowed", VH,
     "if (i.value >= el.size()) throw VmError(\"insertion outside of array bounds\");",
     "if (i.value > el.size()) throw VmError(\"insertion outside of array bounds\");",
     {"conform": r"FAIL b/b-96-err-at-count"}),
    ("C27", "0xa0: a negative channel value clamps to 0", VH,
     "const uint32_t x = v.value > 255 ? 255 : v.value;  // unsigned clamp: negatives -> 255",
     "const uint32_t x = static_cast<int32_t>(v.value) < 0 ? 0 : v.value > 255 ? 255 : v.value;",
     {"conform": r"FAIL a/0xa[0-3]-set-\w+-clamps-negative"}),
    ("C28", "0xb2: float -> int rounds to nearest", CV,
     "  const int64_t t = static_cast<int64_t>(x);                            // truncation toward zero (chop)",
     "  const int64_t t = static_cast<int64_t>(std::nearbyint(x));",
     {"conform": r"FAIL b/b-b2-trunc"}),
    ("C29", "0xb0: %f with 5 decimals", CV,
     "std::snprintf(b, sizeof b, \"%f\", static_cast<double>(bits_f32(bits)));",
     "std::snprintf(b, sizeof b, \"%.5f\", static_cast<double>(bits_f32(bits)));",
     {"conform": r"FAIL b/b-b0-float"}),
    # Consult row 30 said "overflow check at 1001": with a 1,000-slot std::array that writes s_[1000] (UB on top_), so a
    # Release runner could crash instead of reporting. The same off-by-one in the safe direction (999) keeps the bite.
    ("C30", "Stack: the overflow check is off by one (999 slots)", HP,
     "void Stack::push(Variant v) {\n  if (top_ >= kStackSlots)",
     "void Stack::push(Variant v) {\n  if (top_ >= kStackSlots - 1)",
     {"conform": r"FAIL b/b-cap-overflow"}),
    ("C31", "CallWithReturn pushes all 10 engine arguments instead of argc", VM,
     "      if (i < f->argc) push(a[i]);",
     "      if (true) push(a[i]);",
     {"conform": r"FAIL b/b-eng-args-(exact-argc|surplus|disposed)", "diff": r"after 'C "}),
    ("C32", "FindFunction: exact match instead of prefix", VM,
     "    if (ok) return &fs[k];",
     "    if (ok && e.size() == q.size()) return &fs[k];",
     {"conform": r"FAIL b/b-eng-(prefix|short-query)"}),
    ("C33", "Heap: an object's destructor disposes its props front to back", HP,
     "      work.insert(work.end(), props.begin(), props.end());",
     "      work.insert(work.end(), props.rbegin(), props.rend());",
     {"unit": r"object dtor frees props back to front", "tracecmp": r"DIFF \./b/b-own-object-props", "diff": r"cpp='F "}),
    # ---- semantics settled after the consult ----
    ("S01", "FindMemberID: the cache is never consulted (no hit)", VM,
     "    if (upto_nul_copy(script_.symbols()[static_cast<size_t>(c.symbol)]) == low) return c.id;",
     "    if (false) return c.id;",
     {"unit": r"FindMemberID miss-cache poisoning"}),
    ("S02", "FindMemberID: a miss is not cached (no poisoning)", VM,
     "  member_cache_.push_back({last, id});",
     "  if (id != 0xffff) member_cache_.push_back({last, id});",
     {"unit": r"FindMemberID miss-cache poisoning"}),
    ("S03", "atoi wraps instead of saturating (positive)", CV,
     "  if (!neg) return (over || v > 0x7fffffffull) ? INT32_MAX : static_cast<int32_t>(v);",
     "  if (!neg) return static_cast<int32_t>(static_cast<uint32_t>(v));",
     {"conform": r"FAIL b/b-b2-atoi-saturates-high", "unit": r"atoi is _strtol"}),
    ("S04", "atoi wraps instead of saturating (negative)", CV,
     "  if (over || v > 0x80000000ull) return INT32_MIN;",
     "  if (over || v > 0x80000000ull) return static_cast<int32_t>(0u - static_cast<uint32_t>(v));",
     {"conform": r"FAIL b/b-82-atoi-saturates-low", "unit": r"atoi is _strtol"}),
    ("S05", "exact zero: x + (-x) takes the lhs sign instead of +0", CV,
     "      if (c == 0) return 0u;",
     "      if (c == 0) return pa.neg ? 0x80000000u : 0u;",
     {"conform": r"FAIL a/(0x70-signed-zero-x-plus-minus-x|0x70-float-negative-lhs-cancels-to-positive-zero|0x71-float-negative-lhs-minus-itself-positive-zero)", "unit": r"x87_arith"}),
    ("S06", "exact zero: a zero product loses its sign", CV,
     "      if (m == 0) return neg ? 0x80000000u : 0u;",
     "      if (m == 0) return 0u;",
     {"conform": r"FAIL a/0x72-signed-zero-negative-times-int-zero"}),
    ("S07", "0x82: PC is advanced after the native, so a nested call's marker is the site, not site+6", VH,
     "  pc_ = next_pc;  // the handler has consumed its operands before the native runs\n  // D14: the sequence number is assigned at native ENTRY (a nested native gets a higher one, its line comes first)\n  const uint64_t seq = native_seq_++;\n  if (observer_) observer_(e, seq);\n  active_seq_.push_back(seq);\n  NativeCall call{*this, e, site, args};\n  Variant r;\n  try {\n    r = e.fn ? e.fn(call, e.user) : natives_.stub_return(e, heap_, call);\n  } catch (...) {\n    active_seq_.pop_back();\n    throw;\n  }\n  active_seq_.pop_back();\n  cur_pc_ = site;",
     "  pc_ = site;  // M2a-2 T3: 0x82 now writes +6 before the native; the mutant puts the site back for the call\n  const uint64_t seq = native_seq_++;\n  if (observer_) observer_(e, seq);\n  active_seq_.push_back(seq);\n  NativeCall call{*this, e, site, args};\n  Variant r;\n  try {\n    r = e.fn ? e.fn(call, e.user) : natives_.stub_return(e, heap_, call);\n  } catch (...) {\n    active_seq_.pop_back();\n    throw;\n  }\n  active_seq_.pop_back();\n  pc_ = next_pc;\n  cur_pc_ = site;",
     {"unit": r"nested engine call's marker is the PC after", "tracecmp": r"DIFF \./b/b-eng-reenter"}),
    ("S08", "D8': a string -> float conversion does not switch fpu_pc to 64", VH,
     "        if (sw) set_pc64();                                              // ___dec2num 0x65f886 (D8')",
     "        (void)sw;",
     {"conform": r"FAIL a/0x70-d8-pc64-after-string-convert"}),
    ("S09", "lomn_osi_run: Process re-runs at most 9 extra passes (10 in all) instead of 11", HOST,
     "      for (int p = 0; p < 11; ++p) {",
     "      for (int p = 0; p < 10; ++p) {",
     {"rerun": r"RERUN pass counts: C\+\+ 'process passes"}),
    ("S10", "lomn_osi_run: an 11-pass frame skips the frame after next instead of the next", HOST,
     "      if (capped) skip_frame_ = k + 1;",
     "      if (capped) skip_frame_ = k + 2;",
     {"rerun": r"RERUN pass counts: C\+\+ 'process passes"}),
    ("S11", "0x82: the call site is never memoised (D4)", VH,
     "  memo_[site] = e;\n  return true;",
     "  return true;",
     {"conform": r"FAIL b/b-84-memo-err"}),
    ("S12", "0x60: two strings are disposed b then a", VH,
     "    push(make_int(eq ? 1 : 0));\n    heap_.dispose(a);\n    heap_.dispose(b);",
     "    push(make_int(eq ? 1 : 0));\n    heap_.dispose(b);\n    heap_.dispose(a);",
     {"unit": r"0x60 on two strings disposes a then b", "tracecmp": r"DIFF \./b/b-60-string"}),
    ("S13", "0x25: the global path says 'decrement' (not the binary's reused 'increment' text)", VH,
     "    if (idx & 0x8000) throw VmError(\"increment of non-integral type\");",
     "    if (idx & 0x8000) throw VmError(\"decrement of non-integral type\");",
     {"conform": r"FAIL a/0x25-err-global-(nothing-)?says-increment"}),
    ("CTRL", "no-op control (comment only; must be MISSED)", VM,
     "// Run (0x60b850): fetch, trace, dispatch until a handler returns false.",
     "// Run (0x60b850): fetch, trace, dispatch until a handler returns false (mutation control).",
     {"unit": None, "conform": None, "tracecmp": None, "diff": None}),
]

CHECKS = {
    "unit": (["lomn_tests"], "timeout 300 build/native/lomn_tests -tc='osi*'"),
    "asan": (["lomn_tests_asan"], "timeout 300 build/native/lomn_tests_asan -tc='osi*'"),
    "conform": (["lomn_osi_conform"], "timeout 300 ctest --test-dir build/native -R '^osi_conform$' --output-on-failure"),
    "tracecmp": (["lomn_osi_conform"], None),  # run by tracecmp() below (the Makefile recipe's commands)
    "diff": (["lomn_osi_run"], "timeout 900 ctest --test-dir build/native -R '^osi_a6_(digest|window)$' --output-on-failure"),
    "rerun": (["lomn_osi_run"], "timeout 300 ctest --test-dir build/native -R '^osi_a6_rerun$' --output-on-failure"),
}
FAIL_RE = {
    "unit": r"TEST CASE:|ERROR:|FATAL|Sanitizer|runtime error",
    "asan": r"TEST CASE:|ERROR:|FATAL|Sanitizer|runtime error|SUMMARY:",
    "conform": r"^FAIL |conformance: \d+/\d+",
    "tracecmp": r"^DIFF |^MISSING ",
    "diff": r"^WINDOW |^DIGEST |^NATIVES |stop: |differ|failed \(",
    "rerun": r"^RERUN |differ|failed \(",
}
LOGDIR: Path | None = None


def is_control(mid: str) -> bool:
    return mid == "CTRL"


def sh(cmd: str, cwd: Path, timeout: int, log: str | None = None) -> tuple[int, str, float]:
    t0 = time.time()
    try:
        p = subprocess.run(cmd, cwd=cwd, shell=True, capture_output=True, text=True, timeout=timeout, errors="replace")
        out, rc = p.stdout + p.stderr, p.returncode
    except subprocess.TimeoutExpired as e:
        out, rc = (e.stdout or "") if isinstance(e.stdout, str) else "", 124
        out += "\nTIMEOUT"
    if LOGDIR and log:
        (LOGDIR / log).write_text(out)
    return rc, out, time.time() - t0


def first_diff(a: Path, b: Path, tag: str) -> list[str]:
    """The first differing line of two files as one targeted fail line (plus the previous common line)."""
    if not a.is_file() or not b.is_file():
        return [f"{tag} missing {a.name if not a.is_file() else b.name}"]
    prev = ""
    with a.open("rb") as fa, b.open("rb") as fb:
        n = 0
        while True:
            la, lb = fa.readline(), fb.readline()
            n += 1
            if la != lb:
                d = lambda s: s.decode("latin-1").rstrip("\n")[:160]  # noqa: E731
                return [f"{tag} line {n} after '{d(prev)}': cpp='{d(la)}' py='{d(lb)}'"]
            if not la:
                return []
            prev = la


def oracle_window(tree: Path) -> Path:
    """The oracle's trace of Setup + frames 0-2 (the osi_a6_window cut), made once per run: when a mutant makes the C++
    run stop on a VM error, osi_a6_window fails before it runs the oracle, and the first differing record is still wanted.
    The oracle is never mutated, so one copy serves every mutant; it is rebuilt when osi_vm.py or Base.osi change."""
    d = tree / "build" / "mut-oracle"
    out = d / "py.window"
    base = Path(os.environ.get("LOMN_DATA", "")) / "install" / "data" / "Base.osi"
    stamp = f"{(tree / 'tools/research/osi_vm.py').stat().st_mtime_ns} {base.stat().st_size if base.is_file() else -1}"
    if out.is_file() and (d / "stamp").is_file() and (d / "stamp").read_text() == stamp:
        return out
    d.mkdir(parents=True, exist_ok=True)
    tables = "--natives data/osi/natives.json --mods data/osi/natives_beta150.json --types data/osi/types.json"
    sh(f".venv/bin/python tools/research/osi_vm.py --run '{base}' {tables} --frames 600 "
       f"--keys 'down@100 down@160 enter@220 esc@280' --trace build/mut-oracle/py.trace", tree, 900, None)
    with (d / "py.trace").open("rb") as src, out.open("wb") as dst:
        for l in src:
            if l.startswith(b"K      3 "):
                break
            dst.write(l)
    (d / "py.trace").unlink()
    (d / "stamp").write_text(stamp)
    return out


def rerun_pass_line(tree: Path) -> list[str]:
    """osi_a6_rerun compares the digests before the pass-count line, so a broken re-run/skip rule fails on `differ`. Re-run
    the C++ leg on the program the ctest assembled and compare its pass-count line with the hand-computed one in
    engine/tests/osi_a6.cmake (`set(want ...)`): the targeted line."""
    osi = tree / "build" / "native" / "osi_a6_rerun" / "rerun.osi"
    m = re.search(r'set\(want "([^"]+)"\)', (tree / "engine" / "tests" / "osi_a6.cmake").read_text())
    if not osi.is_file() or not m:
        return ["RERUN cannot re-run the C++ leg (no rerun.osi or no hand value)"]
    _, o, _ = sh(f"timeout 120 build/native/lomn_osi_run '{osi}' --data-dir data/osi --frames 6 --keys esc@1", tree, 150, None)
    got = next((l for l in o.splitlines() if l.startswith("process passes:")), "(none)")
    return [] if got == m.group(1) else [f"RERUN pass counts: C++ '{got}' want '{m.group(1)}'"]


def tracecmp(tree: Path, log: str) -> tuple[int, str, float]:
    """`make conform-trace-cmp` without its `native` prerequisite (the build is done by the caller)."""
    t0 = time.time()
    d = tree / "build" / "mut-trace"
    shutil.rmtree(d, ignore_errors=True)
    d.mkdir(parents=True)
    out = []
    steps = [
        "timeout 120 .venv/bin/python tools/osi/conform_assemble.py tests/osi/conformance build/mut-trace/osi",
        "timeout 300 build/native/lomn_osi_conform tests/osi/conformance --osi-dir build/mut-trace/osi --trace-dir build/mut-trace/cpp",
        "timeout 300 .venv/bin/python tools/research/osi_vm.py --conform tests/osi/conformance --trace-dir build/mut-trace/py",
    ]
    for s in steps:
        rc, o, _ = sh(s, tree, 400)
        out.append(f"$ {s}\n{o.strip()[-2000:]}\nrc={rc}")
        if rc == 124:
            break
    names = sorted({str(p.relative_to(d / side)) for side in ("cpp", "py") for p in (d / side).rglob("*.trace")})
    bad = 0
    for n in names:
        a, b = d / "cpp" / n, d / "py" / n
        if not a.is_file() or not b.is_file():
            out.append(f"MISSING ./{n} ({'cpp' if not a.is_file() else 'py'})")
            bad += 1
        elif not filecmp.cmp(a, b, shallow=False):
            out.append(f"DIFF ./{n}")
            bad += 1
    out.append(f"conform-trace-cmp: {len(names) - bad}/{len(names)} traces byte-identical (C++ vs Python)")
    text = "\n".join(out)
    if LOGDIR:
        (LOGDIR / log).write_text(text)
    return (1 if bad or not names else 0), text, time.time() - t0


def run_check(c: str, tree: Path, mid: str) -> tuple[int, list[str], float]:
    log = f"{mid}-{c}.log"
    if c == "tracecmp":
        rc, out, secs = tracecmp(tree, log)
    else:
        rc, out, secs = sh(CHECKS[c][1], tree, 960, log)
        if c == "diff" and rc != 0:  # name the first differing record of what the ctest wrote
            b = tree / "build" / "native"
            pyw = b / "osi_a6_window" / "py.window"
            if not pyw.is_file():  # the C++ run stopped on a VM error, so the ctest never ran the oracle: use its window
                pyw = oracle_window(tree)
            extra = first_diff(b / "osi_a6_window" / "cpp.window", pyw, "WINDOW")
            extra += first_diff(b / "osi_a6_digest" / "cpp.digest", b / "osi_a6_digest" / "py_stub.digest", "DIGEST")
            extra += first_diff(b / "osi_a6_digest" / "cpp.natives.log", b / "osi_a6_digest" / "py.natives.log", "NATIVES")
            out += "\n" + "\n".join(extra)
            if LOGDIR:
                (LOGDIR / log).write_text(out)
        if c == "rerun" and rc != 0:  # the digest cmp fails first; also report the pass-count line against the hand value
            out += "\n" + "\n".join(rerun_pass_line(tree))
            if LOGDIR:
                (LOGDIR / log).write_text(out)
    fails = [l.strip() for l in out.splitlines() if re.search(FAIL_RE[c], l)]
    if rc == 124 or "TIMEOUT" in out[-20:]:
        fails.append("TIMEOUT")
    return rc, fails, secs


def parse_only(spec: str) -> set[str]:
    ids = [e[0] for e in M]
    out: set[str] = set()
    for part in spec.split(","):
        part = part.strip()
        if re.fullmatch(r"\d+-\d+", part):
            a, b = (int(x) for x in part.split("-"))
            out |= {ids[i - 1] for i in range(a, b + 1) if 1 <= i <= len(ids)}
        elif part.isdigit():
            out.add(ids[int(part) - 1])
        elif part:
            out.add(part)
    return out


def anchor_line(text: str, old: str, new: str = "") -> int:
    """1-based line of the first line the mutation changes (the anchor's start plus its unchanged leading lines)."""
    same = 0
    for a, b in zip(old.split("\n"), new.split("\n")):
        if a != b:
            break
        same += 1
    return text[: text.index(old)].count("\n") + 1 + (same if new else 0)


def build(tree: Path, targets: list[str], log: str) -> tuple[int, float]:
    rc, _, secs = sh("timeout 900 cmake --build build/native --target " + " ".join(sorted(set(targets))), tree, 960, log)
    return rc, secs


def main() -> int:
    tree = Path(sys.argv[1]).resolve()
    only = parse_only(sys.argv[sys.argv.index("--only") + 1]) if "--only" in sys.argv else None
    if "--list" in sys.argv:
        for i, (mid, name, rel, old, new, checks) in enumerate(M, 1):
            line = anchor_line((tree / rel).read_text(), old, new) if (tree / rel).read_text().count(old) == 1 else -1
            print(f"{i:2d} {mid:4s} {rel}:{line} | {name} | " + ", ".join(f"{c}: {r}" for c, r in checks.items()))
        return 0
    global LOGDIR
    LOGDIR = tree / "build" / "mutlogs"
    LOGDIR.mkdir(parents=True, exist_ok=True)
    pristine = tree / "build" / "mut-pristine"
    active = tree / "build" / "mut-active"
    if active.exists():  # an interrupted earlier run: restore that file from its pristine copy first
        rel = active.read_text().strip()
        shutil.copyfile(pristine / rel, tree / rel)
        active.unlink()
        print(f"restored {rel} left mutated by an interrupted run", flush=True)
    for rel in sorted({e[2] for e in M}):
        (pristine / rel).parent.mkdir(parents=True, exist_ok=True)
        if not (pristine / rel).exists():
            shutil.copyfile(tree / rel, pristine / rel)
        if not filecmp.cmp(tree / rel, pristine / rel, shallow=False):
            print(f"ERROR {rel} differs from build/mut-pristine (stale pristine copy?)", flush=True)
            return 2
    results = (LOGDIR / "results.jsonl").open("a")
    caught = missed = invalid = 0
    survivors = []
    for i, (mid, name, rel, old, new, checks) in enumerate(M, 1):
        if only and mid not in only:
            continue
        f = tree / rel
        src = f.read_text()
        if src.count(old) != 1:
            print(f"[{i}] ERROR anchor of {mid} not found exactly once in {rel}: {name}", flush=True)
            return 2
        line = anchor_line(src, old, new)
        mutated = src.replace(old, new)
        targets = sorted({t for c in checks for t in CHECKS[c][0]})
        t0 = time.time()
        verdict, per, bsecs = "MISSED", {}, 0.0
        active.write_text(rel)
        f.write_text(mutated)
        try:
            rc, bsecs = build(tree, targets, f"{mid}-build.log")
            if rc:
                verdict = "BUILD-FAILED"
            else:
                for c, want in checks.items():
                    rc, fails, secs = run_check(c, tree, mid)
                    if rc == 0:
                        per[c] = {"outcome": "pass", "secs": round(secs, 1)}
                        continue
                    hit = [l for l in fails if want and re.search(want, l)]
                    outcome = "BITE" if hit else ("TIMEOUT" if "TIMEOUT" in fails else "OFF-TARGET")
                    per[c] = {"outcome": outcome, "rc": rc, "secs": round(secs, 1), "line": (hit or fails or ["(no fail line)"])[0][:300],
                              "fail_lines": len(fails)}
                    if hit or is_control(mid):
                        verdict = "CAUGHT"
        finally:
            f.write_text(src)
            ok_restore = filecmp.cmp(f, pristine / rel, shallow=False)
            if ok_restore:
                active.unlink()
            rrc, rsecs = build(tree, targets, f"{mid}-restore-build.log")
        total = time.time() - t0
        if verdict == "CAUGHT":
            caught += 1
        elif verdict == "MISSED":
            missed += 1
            survivors.append(mid)
        else:
            invalid += 1
        rec = {"id": mid, "name": name, "where": f"{rel}:{line}", "verdict": verdict, "checks": per, "build_s": round(bsecs, 1),
               "total_s": round(total, 1), "restore_cmp": ok_restore, "restore_build_rc": rrc}
        results.write(json.dumps(rec) + "\n")
        results.flush()
        detail = "; ".join(f"{c}: {v['outcome']}" + (f" ({v['line'][:160]})" if v.get("line") else "") for c, v in per.items())
        print(f"[{i}] {verdict} {mid} {rel}:{line} {name} :: {detail} [build {bsecs:.0f} s, total {total:.0f} s, "
              f"restore cmp {'OK' if ok_restore else 'FAILED'}, rebuild rc={rrc}]", flush=True)
        if not ok_restore or rrc:
            print("ERROR restore failed: stopping", flush=True)
            return 2
    print(f"mutants: {caught} CAUGHT, {missed} MISSED, {invalid} BUILD-FAILED; survivors: {' '.join(survivors) or 'none'} "
          "(the no-op control CTRL must be among the MISSED)", flush=True)
    for rel in sorted({e[2] for e in M}):
        same = filecmp.cmp(tree / rel, pristine / rel, shallow=False)
        print(f"final restore cmp {rel}: {'OK' if same else 'DIFFERS'}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
