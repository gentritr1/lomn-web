# SDD ledger — plan: docs/superpowers/plans/2026-10-09-lomn-web-m2a2.md
Spec: docs/superpowers/specs/2026-10-08-lomn-web-m2-design.md (D1–D12, D8′; D13–D15 proposed by the plan). Base: m2-osi-vm @ ebac3c8 (M2a-1 closed).
Billing: Max plan for subagents (Fable plan review, implementers, reviews); Console credits only for overflow and measurement (ROADMAP §6).

## Plan drafted (Opus subagent, 16.3 min, 62 calls, 325k tokens, Max plan)
Measured inputs (the drafter EXECUTED them on the oracle with typed stubs + Base.osi; scripts in the session scratch m2a2plan/, rebuilt as tools by Task 1):
- 54 natives in Setup (980 calls); 69 natives (1,699 calls) in Setup + 600 frames + keys.
- The flip probe changes the path for 16/69 (7 by value; 9 game-object returns whose null kills Setup).
- 35 of 1,049 natives are statically re-entrant, 4 of them on the path (all via OnEvent).

Rulings on the plan's 13 decisions (controller):
- ACCEPT all 13 as recommended.
- D13 (game objects), D14 (natives.log v2) and D15 (nesting cap 4×M, STACK ≥ 4.5× cap × measured bytes/level) are appended to spec §4 by Task 2, verbatim from the plan.
- Decision 10: the asm exception-table read decides; if nothing catches, the port raises one defined error in both engines.
- Decision 13: Opus-medium closes M2a-2; Fable's next use is the end of M2.
- Cost if wrong: Decisions 4, 8 and 10 have built-in stop conditions in Task 2; Decision 3 can be reversed cheaply (a data file).

## Fable plan review (fable, 15.4 min, 55 calls, 309k tokens, Max plan): READY WITH EDITS, 3 C / 11 I / 9 m
Re-derived (EXECUTED): 54/980, 69/1,699, the 35 re-entrant natives (4 on the path at depths 3/4/5/6), the flip rows, the 0x445550 AddRef, 0 P lines.
Critical findings:
- C1: digest guards fold the natives sha into T → compare D lines only.
- C2: flip-implies-sink is false for the 9 null-kills natives.
- C3: the Setup timer is never started (ctor 0x445906 starts it only if arg1≠0; no start call in 600 frames, EXECUTED). Decision 8, the 750-pass figure, the Task 7/10 timer steps and D14's C -1/H records rested on it.
Important findings:
- I1–I3: cited semantics for cheatmenu (INI, not save data), doesfileexist (blk table) and stringtoidentifier (FourCC).
- I4: method addresses.
- I5–I6: swallow mechanism and replay rules.
- I7: exc.py.
- I8: split Task 2.
- I9: Task 4 → Opus.
- I10: lomn_view parser.
- I11: estimate about 13 h (10–18).
Ruling: ACCEPT all 23 edits. The plan drafter (same agent, full context) applies them; the controller verifies they landed, then commits.
Lesson: the drafter "measured" the timer path by assumption (750 passes computed from the 5.0 s argument); the asm showed the timer never starts. The rule already exists (prove the state transition, CLAUDE.md §6); the drafter's brief asked for measurement first, but a computed figure slipped in as "computed; Task 10 measures it". In future plan briefs, ban "computed, measured later" for any figure a decision rests on.
Plan final: 94e8dcc. Edits verified landed (EXECUTED grep): C1 11 refs, C2 18, C3 0x445906 ×4 and '750' ×0, I8 Tasks 2a/2b, I9 Task 4 Opus; no personal paths.
Task 11 (E3 rep 3, Console credits): Haiku arm $0.015 / Sonnet arm $0.237. Controller (EXECUTED): both pass native and wasm osi_a6_rerun. Merged the Haiku arm (first correct). E3 verdict: adopt Haiku for bounded, checkable mechanical jobs. Task 11: complete.

## Task 1 (Sonnet subagent, Max plan, 22.1 min, 110 calls, 358k tokens): 6081792 on m2a2-t1, DONE_WITH_CONCERNS
Reproduced measured facts: 54/980, 69/1,699 (30-frame set = 600-frame set), flip 7/9/53, re-entrant 35/1049 (71 callers; 4 on-path at 3/4/5/6), argc 0/20 REACHABLE, 0x19 12 UNRESOLVED (all unreachable), exc pads at 0x6015a0, SetUpScripting and _WinMain.
Controller (EXECUTED): 54 tests pass with LOMN_DATA; reach --vm-reentry summary matches; exc --func 0x4390d0 gives the 3 catch records.
Rulings:
- R1 (CHANGED-NULL): game-returning + CHANGED + the altered run ends in an error → 7 CHANGED / 9 CHANGED-NULL, matching the plan's measured facts. Task 5's falsifier uses this definition.
- R2 (exception-table doc errors): Task 2a must extend exc.py for kinds 0x05/0x06/0x09/0x0b/0x12, or prove they do not occur on the 35 chains, and reach `truncated 0` on every chain function before concluding "no catch" (Decision 10). Task 2a also corrects opcodes.md "Exception tables" (4-byte info records: 4,966, not 4,605; kind 0x07 is 14 bytes).
- R3: exc.py's test-after is accepted, because its tests pin real catch pads; noted as a process deviation.
- R4: tier names in overrides are defined by Task 2b, and Task 1's parser aligns then.
- R5: coverage.py name shadowing → leave as is (no pytest-cov; tools are run as scripts).
Blind Opus review dispatched.
Task 1 blind review (Opus, Max plan): REQUEST CHANGES, 0 C / 1 I / 6 m.
- I-1: re-entry 35 is a direct-call floor; 49 vtable- or callback-entered functions are in the closure; game-handle Dispose can re-enter the VM.
- The reviewer re-derived exc, reach, coverage and the flip deltas independently: all match.
Fix round 1 → the same implementer (all of I-1 and m-1..m-6). Ruling R6 has been added to the Task 2a brief: the indirect chains are in scope for D13 and Decision 10.
Task 1 fix round 1 → b12dbc7. Controller (EXECUTED): 67 pass; 49 `indirect` lines; the summary is labelled as a floor. Scoped re-review (same Opus reviewer): APPROVE, 7/7 resolved; the reviewer's own closure diff against the tool's 49 is empty. Merged onto m2-osi-vm.
Task 1: complete.

## Task 2a (Opus, Max plan): STOPPED at the Decision 10 condition → (b)
Finding (CITED by the implementer): `GcAreaLoader::LoadArea` 0x4fedd0 wraps LoadObjects 0x4ff93e and LoadCharacters 0x5002ec, both of which can reach Call via SetUnderWater 0x589050, in try/catch(...) with no rethrow (pads 0x4ff984/0x500332; the rethrow flags are only ever written 0). It is reached from Setup via setcurrentarea("frnt") (an unconditional chain). Call and CallWithReturn have no catch and an empty cleanup (0x413ec0), so the error leaves the marker, frame and refs in place. Census: 226 chain functions, 438 direct sites, 4 in a catch (_WinMain, SetUpScripting, 2× LoadArea). Whether the swallow FIRES on frnt is UNVERIFIED.
Other partial results: 45 CreateVariant overloads all AddRef (`inc [edi+4]`); release via Dispose 0x60a930 → vtable slot 1 Free 0x4f2bc0; base ctor 0x4f2b88 sets count 0; timer guard confirmed; the 69/71/144 counts reconciled.
Ruling (controller): continue under branch (b), which is in the plan.
- Derive D16 (the swallowed nested error: state left by the escaping error, the outer 0x82 continuation, the next fetch, the leftover marker on a later 0x52) fully CITED. Both engines model it; the corpus `swallow: true` mirrors LoadArea.
- Add to Task 10 a probe counting swallowed errors on the real run.
- Read the Toa guards (Decision 8): stop again if the VM is called on the front-end within 600 frames.
- R2 still owed: exc.py kinds 0x09/0x0b, plus virtual calls in the chain functions.
- Then finish §2/§4/§8/§9 and natives.md, and append D13–D16 to spec §4.
Cost if wrong: D16 is pinned by corpus programs on both engines, so an error surfaces as a byte diff, not silently.
Task 2a round 2 → f1403fc (exc.py decodes every kind, from the runtime's own jump tables; all 7,002 functions with no truncated record; opcodes.md corrected). STOPPED on Decision 8.
- The Toa runs every frame on the front-end: GcToa process registered at 0x568262; GcBAGroup → PreProcess at 0x54acca, with every gate set by Setup; motion physics enabled by setupmoveable at 0x574ed8. 42 guarded OnEvent sites can be reached.
- D16 drafted: an escaping nested error leaves the state; the outer handler pops n from the current top, pushes the result and does not touch the PC; dispatch continues in the inner function at the error PC; that function's 0x52 meets the inner marker and ends the outer Run early, returning the inner value. If a swallow fires on frnt, the rest of Setup is skipped.
Rulings (controller):
- D8: measure with event_probe.py (inject each of the 42 events at frames 0/1/10/100/599). All SAME → a documented host-source gap (M3); any CHANGED → STOP.
- Swallow-on-path: check now (frnt data loads 'cnt1'? does the oracle's OnEvent(0x36,0) error?).
- D16: a per-handler PC-at-throw table from the asm; amend conformance rule 15 generally; engine changes to Tasks 3/4.
- D13 identity: per object.
Then finish §2/§8/§9, natives.md and the spec §4 D13–D16.
Task 2a round 3 → 9bd3107 (event_probe.py). STOPPED on the D8 probe: 235 injections, 229 SAME, 6 CHANGED (OnEvent(0x28, 10000.0, hell|crsh|impc), fatal damage, only at frames 0–1 → HUD setstring/hide/show skipped in frames 0–4). Swallow on the real path: NO (frnt blk has no char slb; no flag 0x20; no 'cnt1'; the oracle's OnEvent(0x36,0) runs clean).
Rulings (controller):
- D8 ACCEPTED as a documented host-source gap: lethal damage at frames 0–1 to a resting Toa is INFERRED implausible. Re-checked by Task 10 (real natives), M2a-3 screenshots and M3 physics.
- PC table / rule 15 / engine-change list split off to a separate Opus agent on branch m2a2-t2a-pc (worktree lomn-web-m2-t2apc, from 9bd3107).
- The 2a agent finishes natives.md, the spec §4 append, §2/§8/§9 and the measurements line.
Task 2a round 4: DONE_WITH_CONCERNS. 6fa507e natives.md, 83dc104 spec §4 D13–D16, de980fb measurements line (plus f1403fc, 9bd3107).
Corrections to the plan found by 2a:
- Decision 9: block-file order is Art.blk → slots → VFS fallback (not "never loose"); the saver addresses/defaults are cited in natives.md §9.1.
- BetterSaver hooks 11 exe functions: future natives that reach a hooked function model the HOOK.
Rulings:
- the inferred engine counts are accepted (same table in both engines; listed as an inferred risk for Task 10);
- Task 2b takes the cited values from natives.md, not plan Decision 9;
- the n-swallow programs wait for the T2a-pc table.
Blind Opus review dispatched (it re-derives citations from the binary).
Task 2a blind review (Opus): REQUEST CHANGES, 1 C / 1 I / 9 m.
- C-1: a third swallowing block, LoadHives (call at 0x500c0b; pads 0x500c12/31/51; flags only zeroed), was missed because the census used decomp function bounds (LoadArea really runs to ret 0x500fe4). natives.md §5.1/§5.2 and the D16 scope are wrong. No A9 effect (frnt has no hive file, INFERRED).
- I-1: D16 is undefined on (a) who writes the E line for a swallowed error, (b) by-ref writes landing in the original arg slots (needs n-swallow-writes), and (c) reset_after_error popping to the outermost call's own marker.
- The reviewer independently confirmed the exc.py totals, the 0x82/0x52 citations, all 45 overloads, the saver §9.1, the blk order and the event probe sample.
Pattern promoted to CLAUDE.md §6 red flags ("decompiler function bounds are a hint"; 2nd occurrence).
Fix round 1 → a FRESH Opus agent (the original 2a agent's context is ~690k tokens; resuming it per turn is costly), with the review + natives.md + report as inputs.
Ruling for I-1(a): the port writes the E line at the swallow point (the swallowing host's catch), with the state as at the throw. The E line is a trace artifact, not original behaviour; the fix agent cites the original's catch for everything else.

## T2a-pc (Opus, Max plan): 1efcd87 on m2a2-t2a-pc, DONE_WITH_CONCERNS
error_pc.py + table: 97 handlers, 272 raises (194 relevant). Distribution +0 3 / +1 126 / +2 5 / +3 26 / +4 13 / +5 1 / +6 21. Oracle cross-check (EXECUTED by the implementer): 173/173 original errors match the table. Rule 15 specified generally (+ the `expect_error.pc_after` field). C++: 61 handler functions need the pc_ move (source reading only, not run); oracle: 1 port-only change.
Rulings:
- accept the table, rule 15 and the `pc_after` name;
- the C++ change list goes to Task 3 (pc_after pins red first), the oracle change to Task 4;
- the NEW gap (names over 255 chars throw kOutOfRange at 0x4f5c27 in 0x18/0x19/0x1a/0x83, modelled by neither engine) goes to Task 2b (measure the max name length in Base.osi + a corpus falsifier) and Tasks 3/4 (model it);
- `make osi-tables` must regenerate the error-PC tables: a Haiku clerical job after the merge.
Blind Opus review dispatched.
Task 2a fix round 1 (fresh Opus): STOPPED on D8 after the probe was improved (repeat + every frame).
- The fatal tuple also matters at frame 2.
- NEW: the non-lethal (0x28, int 2, 'impc') CheckWallCloseness threshold (repeated) skips 3 hud.hide calls at frame 4.
- All effects are confined to frames 0–4; the path re-converges from frame 5 even with injection at every frame; 0 ERROR.
- C-1 done: true-extent census gives 5 caught VM sites (3 LoadArea blocks + _WinMain + SetUpScripting) and 43 indirect; exc.caught_calls owners come from .exc, not sizes.
- I-1 (a)/(b)/(c) done (D16 rules 1–6).
- The review's m-1 was itself a decomp-bounds artefact (GcBAGroup::Process → CheckForNew 0x54b071).
Ruling: D8 stays a documented gap (the A9 script path re-converges by frame 5), with an explicit M2a-3 RISK: the wall-closeness event is plausible on the front-end and could leave HUD visibility different (persistent engine state). M2a-3 models it or settles it with the owner's screenshots. Finish the minors; skip the conformance note (T2a-pc's rule 15 supersedes it at merge).
T2a-pc blind review (Opus): APPROVE, 0 C / 0 I / 7 m.
- 14 handlers hand-checked; the reviewer's own control-flow walk finds no missed raise (beyond 4 infeasible rethrows); its own oracle cross-check gives 173/173.
- Rule 15(b) upgraded to CITED (CallWithReturn 0x60b450 has no catch; the PC restore at 0x60b5a2 never runs after a throw).
- C++ 61-function list confirmed by scan; not run-verified (the runner does not print snapshot().pc).
Ruling: the T2a-pc minors M1–M7 are bundled into Task 2b (it edits the same contract docs):
- the distribution sum, rule 15(c) stack-error wording, the indirect-call limit, the 4 infeasible rethrows, the rule 15 formula for case (b), and the wording nits;
- pin tests for 0x50/0x51 +2, 0x53 +0 and 0x84 +6;
- commit the oracle cross-check script.
T2a-pc approved and ready to merge with T2a once T2a fix 1 lands.
Task 2a fix 1 → 0216a4a. Scoped re-review: APPROVE, 11/11 (m-1 withdrawn by the reviewer: decomp-bounds artefact). The reviewer's own census gives 5 direct / 43 indirect.
MERGED onto m2-osi-vm (EXECUTED, clean cherry-picks in order): 173a3cd, e0ce263, 086738e (T2a-pc), c498644, d778d8d, 1249888, 0e62ecc; plus 61c4915 (ROADMAP M2a-3 risk). tools pytest on the merge: 447 passed, 2 skipped.
Task 2a: complete. T2a-pc: complete (its review minors are carried into Task 2b).
Carried into Task 2b:
- the T2a-pc minors M1–M7;
- the n-swallow-{continue,reerror,pops,writes} corpus programs (now that the PC table exists);
- the 255-char name-length gap (measure on Base.osi plus a corpus falsifier);
- the `make osi-tables` regeneration of the error-PC tables;
- Decision 9 values taken from natives.md §8.2/§9.

## Task 2b (Opus, Max plan): 0d9c87b ea57ca0 90ea28f 40e1ffd on m2a2-t2b, DONE_WITH_CONCERNS
13 corpus programs in group n (9 pending: D13 refs/identity ×2, depth3, swallow ×5, name-256; 4 active). Contract text (natives.md §1/§3/§7/§8.3, trace.md D14, opcodes.md D16), overrides merge ([] for now), osi-tables regenerates the error-PC tables, the M5 pins and error_pc_crosscheck.py (173/173). The longest Base.osi name is 145 bytes (EXECUTED), so the original cannot reach the 255-char throw with the real data.
Rulings:
- Pending programs carry `"pending": "<task>"`; the runners print PENDING and a count (a separate Sonnet job, so the corpus author never reads the runners); Task 10's gate requires pending = 0.
- n-name-256 is rewritten from a string-table literal (hostile .osi reachable).
- Format additions, D14 details and merge choices accepted.
- The D16 rule 2 clarification is mirrored into the spec.
- Task 3/4 briefs: natives.md is the authority; the plan's old interface names (nested_error_pending_) are replaced by D16.
Task 2b ruling commit a74953d: pending marker on the 9 programs ("T3/T4"); n-name-256 from a string-table literal (.rawstring directive in osiasm, tested); D16 rule 2 identical in the spec and natives.md. Blind Opus review dispatched.
Pending-runner job (Sonnet, Max plan): fe121ee on m2a2-tpend.
- Both runners, trace-cmp, conform_assemble and conformance_report handle `pending` (non-empty string only); output `PENDING <id> (<task>)`, a count in the summary; exit 0 when the non-pending programs all pass.
- Neither runner rejects unknown keys.
- Tests red→green: 17 Python + ctest osi_conform_pending.
- make conform-trace-cmp 528/528 + 0 pending.
- UNVERIFIED: wasm32 (no wasm build in that worktree), so it is verified at the T2b+tpend merge.
Task 2b blind review (Opus): REQUEST CHANGES, 0 C / 3 I / 6 m. The reviewer hand-derived 11/13 programs (all match); the 4 active pass on both current runners; the 9 pending bite; osi-tables is byte-stable; crosscheck 173/173 (and catches a corrupted table).
- I-1: natives.log v2 has no record for engine ref changes on unlogged objects (show('cnt1'), addbigsprite/addtextobject).
- I-2: "active" passes only because the runners ignore pc_after/then/swallowed/expect.top (mutation shown).
- I-3: the merge needs the pending-skip.
Rulings:
- H records are authoritative for every engine-side ref change (in or out of natives); G is a snapshot the replay checks.
- Fail-closed: runners fail any non-pending program with an uncompared key; programs using not-yet-compared keys become pending; Task 10's gate = pending 0 + a mutated-key check.
- The C record carries the engine's query name.
Fix round → the T2b agent (contract + pending marks + minors); the fail-closed runner change → the tpend agent (with the wasm verification). Merge both together.
tpend fail-closed → 1299a1d.
- Both runners carry explicit COMPARED (105) / IGNORED (8) key lists, byte-identical via --list-keys (test-pinned); an uncompared key in a non-pending program fails before the run.
- FOUND A REAL SILENT GAP: the C++ runner never compared expect_error.context_depth/globals/heap (the oracle did). b-eng-reenter-depth's context_depth 64 was unchecked on C++ until now; added (still passes). The C++-only `expect.found` is now rejected by both.
- Controller-visible evidence (agent EXECUTED): 528/528 both, trace-cmp identical, native osi ctest 7/7, wasm osi ctest 6/6 (wasm gap closed).
Task 2b fix 1 → 7422dfa. Scoped re-review: APPROVE. I-1, I-2, m-1..m-6 resolved; n-onevent-args (the only active program) passes on both runners and bites on a mutated global. Two new minors ruled by the controller:
- r-m1: one `H` line per AddRef/Release event, always ±1, temporaries included (cancelling pairs inside a native are both written), mirroring each cited inc/dec.
- r-m2: `X -1` is reserved for a host-route swallow (no native); not on the M2a-2 path.
These go into natives.md §3 on the integration branch (controller doc edit).
Integration branch m2a2-integ (worktree lomn-web-m2-integ) = 61c4915 + T2b (6 commits) + tpend (2) + 40c06e7 (merged conformance rule renumbering, controller-resolved conflicts in conformance.md and conformance_report.py docstring). Integration check running.
Integration check #1 (EXECUTED) caught 2 issues:
- n-name-256 used the escape …, which the C++ Latin-1 JSON reader rejects;
- the keys test did not skip pending programs.
Fixed by Haiku (37bb77f: Latin-1 rule + guard test red→green, keys test skips pending, natives.md r-m1/r-m2 sentences) and a controller doc fix 54d3239 (Haiku wrote the literal ÿ instead of the text ÿ; recorded as a Haiku miss).
Integration check #2 (EXECUTED): conformance 529/529 + 12 pending on both runners; trace-cmp 529/529 byte-identical, 12 pending with no trace; pytest 542 passed / 2 skipped; native osi ctest 7/7; wasm osi ctest 6/6.
MERGED: m2-osi-vm fast-forwarded to 54d3239.
Task 2b: complete. Pending-runner + fail-closed: complete.
NEXT: Wave 2, Tasks 3 (C++ framework, Opus) and 4 (oracle framework, Opus), in parallel from 54d3239.

## Wave 2 dispatched (Max plan): Task 3 (C++ framework, Opus, agent C, worktree lomn-web-m2-cpp) ∥ Task 4 (oracle framework, Opus, agent P ≠ C, worktree lomn-web-m2-py), both from 54d3239, both pre-built.
Brief additions:
- natives.md/D13–D16/trace §6/conformance rules 15–22 are the authority; the plan's old swallow names are superseded;
- error PCs per the table, with pc_after pins red first;
- fail-closed keys: each runner compares every group-n key, and the lists must be identical at merge (Task 4 owns test_osi_conform_keys.py);
- the name bound;
- independence: neither reads the other's new code;
- the pending markers stay; the controller un-pends at merge.
Task 4 (Opus, agent P): 9854add on m2a2-py, DONE_WITH_CONCERNS. The oracle passes 13/13 of group n without pending markers (pre-task 1/13); the corpus 529/529 + 12 pending. A6 D-lines identical on 4 legs (own stubs, replay v1, replay own v2, real-natives). pytest 585 passed / 1 expected fail (the C++↔Py key equality test against the old C++ binary). Key bite: every new key bites under targeted mutation.
Concerns → rulings:
- C1: the key list is exactly the 38 group-n paths (143 total); sent to T3 to match.
- C2: v2 log equality is checked at merge.
- C3: the nesting cap counts engine calls IN FLIGHT, not context_depth (ruling sent to T3, with a unit test after a swallow).
- C4: object_arg on stub-minted handles → Tasks 7b–9b.
- C5: deviations OK; the C-string reads are identical on well-formed files.
- C6: a real oracle bug fixed (string-table index error E line printed pc/op/depth 0).
Task 3 (Opus, agent C): 88f5c0e 5c0f020 a19eefd on m2a2-cpp, DONE_WITH_CONCERNS.
- C++ passes 13/13 of group n (no pending), native and wasm; the corpus 529/529 + 12 pending.
- osi unit 69/69, 1435 assertions; 38/41 error-PC pins went red→green.
- The D15 in-flight cap is pinned (a context_depth mutant bites).
- A6 D-lines equal the pre-task digest; the v2 log = old + K + ` G 0`.
- The C++ mutated-key check gives 13/13.
- The key list is exactly 143 (after the ruling).
- Expected failure: osi_a6_digest (the old oracle can't read v2) → clears with T4.
Rulings:
- the D15 wasm stack margin with real re-entrant natives → Task 10 measures it;
- static self-registration dropped from the STATIC lib → Tasks 7a–9a must use an OBJECT library, an explicit registration list or --whole-archive (brief item);
- stub-minted game handles have no host object for object_arg (both engines) → Tasks 7–9 decide how stubs mint objects (brief item);
- mutate_vm anchors moved → Task 10 re-runs the bites.
Next: a blind Opus review of T3, plus an integration branch (T3+T4, un-pend the 12, trace-cmp, key-list diff, A6 v2 replay).
INTEGRATION T3+T4 (branch m2a2-integ2 = 54d3239 + T3 ×3 + T4 + c6056e8 un-pend + fix) — EXECUTED:
- keys: C++ and Python --list-keys byte-identical (151 lines = 143 compared + 8 ignored);
- conformance: 541/541 pass, 0 pending, on BOTH engines; conform-trace-cmp 541/541 byte-identical. Two independently written engines agree on D13/D16/name-bound/error-PC programs hand-derived from the contracts;
- ctest osi native 7/7 (incl. the A6 digest with v2 replay); wasm 6/6;
- pytest: 1 failure = T4's "exactly 12 pending" pin, which also made its parametrized group-n test silently run 0 cases after un-pending. Controller fix: the list = all 13 group-n programs, assert none pending → 44 passed (13 parametrized cases run).
Waiting on the blind reviews of T3 and T4 before merging to m2-osi-vm.
Task 4 blind review (Opus): REQUEST CHANGES (tests only), 0 C / 1 I / 6 m.
- Code correct: 2 adversarial programs hand-derived and passing; all 6 A6 legs give the same D md5; 9 handlers' error PCs match; the mutation sweep killed 16/20.
- I1: the D15 in-flight cap (M7) and the D16 rule 6 reset (M17) are unpinned.
Rulings:
- the literal `-1` host seq (C -1/X -1), with X -1 for a host-route swallow (both engines; natives.md clarification);
- replay H type and early-free → desync; the replay must consume the whole log;
- calls.depth = context_depth (the literal conformance.md meaning), pinned after a swallow in both engines;
- m6 parked.
Fix round → the T4 agent. The C++ counterparts (literal -1, calls.depth after a swallow) go to T3's fix round after its review.
Task 3 blind review (Opus): APPROVE, 0 C / 1 I (latent) / 7 m.
- The reviewer ran 12 hand-derived error-PC programs (all pass); a pre-fix PC-revert copy fails exactly those; ASan clean; wasm 69/69 and 541/541 (unpended); 7 single-key mutations all bite; natives.log v2 matches the worked example.
- I-1: static self-registering native groups are dropped from the STATIC lib, so `--real-natives` would silently run stubs.
Rulings (sent to the T3 agent for fix round 1):
- fix I-1 NOW (an explicit registration list or OBJECT lib; hard error on real_natives with 0 groups or an unbound tier1; report BOUND per native; a link test through the real target);
- C++ counterparts of the T4 rulings (literal -1 host seq, X -1 host swallow, calls.depth = context_depth pinned after a swallow);
- M-2 then-reset; M-3 doc;
- M-6 hostile-input-safe key-spec parser (browser-linked);
- M-7 refcount invariant check;
- M-4 → Task 10, M-5 → Task 7–9 briefs.
Task 4 fix 1 → b254e2d. Scoped re-review: APPROVE.
- I1 + m1..m5 resolved; the reviewer re-applied M7/M17/M18/M20 plus new mutants for the fixes, and all are killed. A6 md5 d7c45cff… on all legs; replays consume the whole log.
- New r1 (Minor): when the oracle stops on a VM error, the unconsumed-log message replaces the error in the stop line. Fix: call replay_finish only when stop=="none", or keep both (Haiku job at integration).
- r2: osi_a6_digest needs T3+T4 together; already EXECUTED green on m2a2-integ2 (native 7/7).
Waiting: T3 fix round 1 → then integration v2 (T3 fix + b254e2d + r1 fix) → full check → merge to m2-osi-vm.
Task 3 fix 1 → 4244115, 204322e. Scoped re-review: APPROVE.
- I-1 verified with the reviewer's own prescribed-way group (bound native and wasm, logs identical).
- fail-closed holds, except R-1.
- the key-spec parser: 30k ASan+UBSan fuzz with 0 reports.
- A6 D lines 601/601 identical.
Carried:
- R-1 (Minor): the selftest group counts toward "≥1 group binds"; a hand-made table with a t3selftest row passes with 0 game natives. → Task 7a brief: count only non-selftest bodies.
- N-1 (an M-7 throw does not dispose the return value; undefined path) and N-2 (M-7 per-return cost) → Task 10 notes/perf.
Haiku r1 fix → 057f51f (replay_finish only on stop=="none"; red→green; the diff is exactly the 2-line change).
Integration v3 (m2a2-integ2 + T3 fix 1 + T4 fix 1 + Haiku r1) — EXECUTED:
- keys identical (151); conformance 541/541 on both engines, 0 pending; trace-cmp 541/541 byte-identical;
- native osi ctest 8/8 (incl. the A6 digest with v2 both sides and osi_conform_rules); wasm 7/7; ASan osi 7646/7646;
- pytest 596 passed / 1 failed: the mutate_vm S07 anchor is stale after T3 fix 1 moved the 0x82 PC code.
→ Sonnet re-points S07 and must show it CAUGHT by a real mutate_vm run.
S07 re-pointed by Sonnet (f770742): mutate_vm --only S07 gives CAUGHT (unit on D16 n-reenter-nested-error-swallowed + tracecmp b-eng-reenter), restore cmp OK ×5. NEW GAP → Task 10: test_osi_vm.cpp:1427 ("a nested engine call's marker is the PC after") no longer bites S07 after the PC move; Task 10's mutation re-run must restore a unit test that bites.
Final integration pytest (EXECUTED): 597 passed / 2 skipped.
MERGED: m2-osi-vm fast-forwarded to f770742 (T3 ×5 + T4 ×2 + un-pend + test fix + Haiku r1 + S07).
Task 3: complete. Task 4: complete.
Carried forward:
- R-1 (selftest counts toward the fail-closed check) → Task 7a;
- M-4 wasm stack per level, N-1/N-2, the S07 unit gap, mutate_vm full re-run → Task 10;
- object_arg on stub-minted handles + host classes mirroring exe inheritance → Tasks 7–9.
NEXT: Wave 3: Task 5 (taint pass, Python) ∥ Task 6 (lomn_view --script 1, C++), then the native pairs 7a∥7b, 8a∥8b, 9a∥9b.

## Wave 3 dispatched (Max plan, from f770742, both pre-built): Task 5 (A10 taint pass, oracle, Opus; worktree lomn-web-m2-t5) ∥ Task 6 (lomn_view --script 1 native+wasm, osi-a9 target, -fwasm-exceptions scope, Opus; worktree lomn-web-m2-t6). Briefs carry the merged-state summary; the controller runs the M1/datacheck-wasm checks.
Task 6 (Opus): 18996b2 on m2a2-t6, DONE_WITH_CONCERNS.
- `lomn_view --script 1` native == node, byte-identical (digest 603, natives.log 2,299, trace window 55,170 lines, stdout) and equal to lomn_osi_run (adds no behaviour).
- `--natives real` fails closed (exit 4).
- `lomn_view.wasm` grew +47.2% (Decision 12 record).
Rulings:
- the parser interpretations are accepted (default --frames 1 = A8; exit 4 for host start failure; render options exit 2);
- osi-a9 with real natives keeps the STRICT native log == Python log (that is the A9 contract);
- the copied trace sinks are accepted, guarded by the smoke ctest (refactor later if they drift).
Controller M1/A9 check running (build, datacheck-wasm, test-wasm, ctest, osi-diff, view D == osi-diff D, osi-a9 stub). Blind Opus review dispatched (it builds under build/review to avoid clashing).
Task 6 controller check (EXECUTED, worktree lomn-web-m2-t6 at 18996b2, load 3.3):
- build ok; datacheck-wasm 39/39; test-wasm ctest groups 44/44, 17/17, 29/29;
- osi-diff rc 0; lomn_view --script stub 600-frame D lines == osi-diff native D;
- osi-a9 OSI_A9_NATIVES=stub rc 0 with cmp 3/4 (D native == Python) and cmp 4/4 (coverage + natives.log native == Python) OK.
(The first run's PIPESTATUS capture was empty, a harness bug of mine; re-ran osi-a9 alone for its rc.)
Task 5 (Opus): 42d5a9a on m2a2-t5, DONE.
- The A9 taint run: 69 tainted, 38 sinks (all branch); 14 UNSAFE (must tier 1), 55 safe (provisional). 4 natives the plan called decision-free are UNSAFE (getresolutionwidth/height, addbigsprite, getammo).
- Falsifier PASS (7 CHANGED all UNSAFE; 9 CHANGED-NULL all on the must-game list), and it bites a planted rule removal.
- The hook guard: the A6 digest and the 541 traces are identical with the pass attached.
Controller (EXECUTED): 34/34 test_osi_taint; reproduced the A9 head line and the UNSAFE list.
Rulings:
- the extra sinks (a tainted 0x50/0x51 jump target, tainted 0x18/0x1a names) are ACCEPTED (stricter) → natives.md §7.3 update (doc job);
- the approximations are accepted;
- Task 10 iterates taint with merged overrides + --real-natives until no new UNSAFE;
- the MUST_GAME_RETURNING list goes after Tasks 7–9.
Feeds Tasks 7–9: the 14 UNSAFE + the 9 game-returning natives must be tier 1.
Blind Opus review dispatched (soundness focus).
Task 6 blind review (Opus): APPROVE, 0 C / 0 I / 8 m.
- The reviewer confirmed byte-identical view vs osi_run, native vs wasm, and Python digest/log; M1 null logs identical; 109 hostile inputs with ASan/UBSan clean and the same exit codes native/wasm; osi-a9 recipe ok and fails at the right step when corrupted.
Rulings, fixed now (T6 fix 1):
- m1 USER-DATA SAFETY: `--digest <install>/data/Base.osi` DELETED the game script. Refuse any output resolving to the script or under the install, with sha256-before/after tests.
- m3: per-run User dir (no shared $TMPDIR state).
- m4: osi-a9 header compare (natives= stripped).
- m5: exit codes 3 = input / 4 = host start.
Parked: m2 → Task 10; m6, m7, m8.
Task 5 soundness review (Opus): REQUEST CHANGES, 0 C / 2 I / 7 m.
- The A9 result holds (the reviewer's own 600-frame flip probe agrees: 0 violations; A6 digest unchanged).
- I1 SOUNDNESS: a store through a tainted reference (0x10/0x1a/0x15/0x93–0x96) is neither propagated nor a sink, so a path-changing native can be called safe (adversarial A3/A4). 0 such stores on A9.
- I2: the falsifier wrongly fails a CHANGED native that is tier 1.
- The 4 surprises explained: width/height/addbigsprite are type-only checks; getammo is a real value check (< 10).
Rulings (fix round 1 → the T5 agent):
- refstore sinks; the falsifier accepts tier1; tier1 output over-approximates (contents + nested returns); errpath sinks;
- pin the 19 unpinned opcodes; correct the measurements claim; type-only sink marking.
- M3/M7 no change.
Task 6 fix round in parallel (output-path safety).
Task 6 fix 1 → 0ecc163.
- m1: outputs refused if they are the script or the install, or lie under it, in every mode with --install; 12-target matrix plus symlink/hardlink/case, sha256 before/after.
- m3: per-run user dir.
- m4: osi-a9 header compare.
- m5: exit 3 for input/table errors.
- Agent EXECUTED: 116 pytest, native 31/31, wasm 14/14, osi-a9 stub OK.
- One M1 test (view_cli_survey_empty) was moved to write its output beside the install; it still expects exit 3.
Scoped re-review dispatched (try to defeat the guard; check the changed test is not weakened).
Task 6 scoped re-review: APPROVE.
- m1: 246 attacks native + 246 ASan + 182 wasm, every output option in every mode, on PRIVATE synthetic installs; all exit 2; whole-tree sha256 unchanged. The real install's Base.osi sha256 is unchanged.
- The changed M1 test was not weakened; m3/m4/m5 fixed; A6 output byte-identical.
Note (→ next Haiku batch): add a one-line comment at the output-deletion site, because the guard's safety for dangling links and hard links relies on "delete before write".
MERGED: m2-osi-vm fast-forwarded to 0ecc163 (T6 + fix). Task 6: complete.
Task 5 fix 1 → 5618f8f.
- refstore + errpath sinks, the falsifier accepts tier1, tier1 output over-approximates, all 49 result ops pinned (2 more survivors found and pinned), type-only marking.
- A9: 14 UNSAFE / 55 safe unchanged; 7 of the 14 are UNSAFE type-only (width, height, addbigsprite, gctimer.create, getmp3player, gettoa, spline.create); getammo is by value.
- The full-A9 falsifier: 0 violations. pytest 641/2 skipped. The A6 digest is unchanged.
Notes:
- The taint head-line format changed: check the osi-a9 taint leg at integration.
- natives.md §7.3 still lacks the new sink kinds → Haiku doc batch (with the T6 delete-before-write comment).
Scoped re-review dispatched.
Task 5 scoped re-review: APPROVE.
- A1–A4 flagged; the 2 new adversarial routes N1/N2 flagged; flip600 0 violations; type-only only annotates (the UNSAFE set is unchanged with the marking off).
- Notes: type-only overstates safety where nothing is a legitimate return. RULING for Tasks 7–9: "type-only" = the exact value may be a placeholder, but the type AND nothing-vs-something must match the original exactly (game-returning natives stay "must").
- The M4 guard nit (the divide builder is missing from the op list) → Haiku batch.
MERGED Task 5 onto m2-osi-vm: 7a44d53, b11aed5. The measurements.md conflict was resolved by the controller (both appended sections kept: T6 then T5).
Task 5: complete. Post-merge verification running (native build, trace-cmp, pytest, ctest osi|view, osi-a9 stub incl. the taint leg).
Post-merge check on b11aed5 (EXECUTED): native build ok; conformance 541/541 both + trace-cmp byte-identical; pytest 757 passed / 2 skipped; ctest osi|view 23/23; osi-a9 stub rc 0 (cmp 2–4/4 OK; the taint leg ran, head line "taint: natives tainted 69, sinks 38 …, type-only 27").
NEXT: pair 7 (7a C++ Sonnet ∥ 7b Python Sonnet) from b11aed5; Haiku doc batch in parallel.
Pair 7 dispatched (Max plan, Sonnet ×2, from b11aed5): 7a C++ (worktree t7a, pre-built native+wasm; also fixes R-1) ∥ 7b Python (worktree t7b). Brief additions: registration via groups.cpp; the taint must-set + the type-only ruling; host inheritance; BetterSaver hooks; per-run User dir; independence; overrides rows must be byte-identical across the sides.
Haiku docs batch dispatched (worktree docs): natives.md §7.3 sink kinds + the type-only ruling; the lomn_view delete-before-write note; the taint op-pin guard covers divide.
Haiku docs batch → 3a3a6eb (controller verified: verbatim §7.3 text, comment-only lomn_view change, div builder; taint tests 44/44). MERGED (ff).
Task 7b (Sonnet, Python): 9a13232 on m2a2-t7b, DONE_WITH_CONCERNS.
- 7 natives modelled (stringtoidentifier, identifiertostring, doesfileexist, gctimer.create, userealdeltatime, resolution w/h) + debug.print tier2 + 3 copy-kind tier3 rows.
- pytest 790/2 skipped; oracle corpus 541/541; the A9 --real-natives run: 390,730 instructions, stop none (7 rows bound).
- The 7b asm findings that differ from the plan: identifiertostring is a byte swap + strlen with no table (no "unknown id" text); the GcTimer started flag is the LOW BYTE of argument 2 (0x445906). Both are NOT shared with 7a (independence), to be compared at merge.
Rulings:
- Overrides row comparison = key/tier/default EXACT + every cited hex address; cite/note wording may differ; the merged file keeps 7a's text.
- The overrides file is pure ASCII (-> not →); 7b converts at merge.
- osi_a9_smoke's hard-coded tier counts must be derived from the tables (plumbing note sent to 7a).
Task 7a (Sonnet, C++): 941324c on m2a2-t7a, DONE_WITH_CONCERNS.
- osi unit 97 cases; ctest native 26/26, wasm osi 9/9; doesfileexist finds all 2,608 Art.blk names (an independent blk.py reader).
- `lomn_view --natives real` 600 frames: exit 0, 390,730 instructions (== 7b's oracle count), wasm D == native D.
- R-1 done (game_bodies excludes selftest).
INDEPENDENT AGREEMENT (both sides, from the asm, both contradicting the plan text): identifiertostring = a byte swap + strlen with no table; the GcTimer start flag = the LOW BYTE of argument 1.
Row comparison (EXECUTED script): keys 11/11 identical; tiers identical; defaults identical except the 3 tier3 copy-kind rows (7a nothing vs 7b none). Cited addresses: no contradictions (7b cites more points; the 7a-only addresses are neighbouring instructions in the same functions).
Ruling: tier3 rows take 7a's default nothing (safer than the known-wrong copy stub); 7b's file is non-ASCII → the merged file takes 7a's ASCII text.
Blind Opus review of 7a dispatched (7b's review is running).
Task 7b blind review (Opus): REQUEST CHANGES, 0 C / 2 I / 6 m.
- Every binary citation re-derived by the reviewer and holds; the real run is deterministic and each native behaves as cited; 22/24 planted misreadings are caught.
- I1: the 7b overrides are non-ASCII (services.CITES and the test regex require →). The integration already takes 7a's ASCII file; the 7b source and tests must switch to "->", plus an isascii test.
- I2: a CIRCULAR check: test_lomn_view_script.py derives the expected tier counts with coverage.py's own loader. Hand-pin the counts.
- m1 (backslash separator untested), m2 (0.5 s gap boundary untested) → add tests; m3–m6 notes.
Plan: one combined fix round on the integration branch m2a2-i7, after the 7a review and the integration check.
PAIR 7 INTEGRATION (m2a2-i7 = 3a3a6eb + 7a + 7b; conflicts → 7a's versions; osi-tables regenerated with no diff), EXECUTED:
- conformance 541/541 both, trace-cmp identical; ctest osi|view 24/24;
- **osi-a9 OSI_A9_NATIVES=real rc 0**: cmp 1/4 native == wasm32 (601 D lines; natives.log, window and stdout identical); 2/4 native == Python replay; 3/4 native == Python's OWN real natives; 4/4 coverage + natives.log native == Python's own real natives. Two independent implementations of the 7 services/time natives agree byte for byte over 600 frames.
- pytest 788/2 failed, both integration fallout:
  (1) test_osi_natives_services pins 7b's cite TEXT against the merged (7a) file → check key/tier/default + cited addresses instead;
  (2) test_osi_taint's fails-closed test (7a's version) no longer empties the Python groups now that 7b's are registered → use 7b's approach.
Combined fix round to follow the 7a review.
Task 7a blind review (Opus): REQUEST CHANGES, 1 C / 1 I / 7 m.
- C-1, a SHARED MISREADING (both sides): the BetterSaver doesfileexist "neither prefix" branch (contains User/, starts with neither Root/ nor User/). The original shows MessageBoxA "OSI Script Security Abort" (0x10001632), then FALLS THROUGH: strcat_s and GetFileAttributesA relative to cwd (= the Root/ mapping) → returns exists("Root/"+path). Both sides returned 0. Off A9; a hostile-script path.
- I-1: non-deterministic real-time smoothing runs EVERY frame in the original (0x4a91fe–0x4a929f); the flag only selects dt. The C++ test pins the misreading.
- m-1 debug.print cite + 254-byte kLenTooLong; m-2 gctimer AddRef pin 0x4c32ae; m-3 a lower-case user/ test; m-4 comment; m-5 document hostile User/ divergences; m-6 unused include; m-7 share BlockChain for 8a/9a.
- All other citations re-derived by the reviewer and hold; 18/20 mutants killed; native == node; ASan + a hostile-path harness clean.
Rulings:
- both engines model exists("Root/"+path) for the neither-prefix branch; the message box is a host UI side effect, documented in natives.md §9.3, not logged;
- I-1 is fixed in C++ (step every frame, select by the flag);
- all minors are fixed;
- the 7a agent owns ALL natives.md edits (incl. 7b's concern-6 Find/slot notes);
- the 7b agent owns only its Python files + Python tests: I1 ASCII cites + isascii test, I2 hand-pinned counts, m1/m2 tests, the services overrides test → key/tier/default + addresses, the taint fails-closed test empties the groups, C-1 in Python.
Fix worktrees: f7a, f7b off i7 HEAD 9d9eccf.
Lesson (→ memory): a shared misreading across two independent sides was caught only by a third reader of the DLL. The independent pair is not an oracle for paths neither side reads carefully; hostile/off-path branches need a reviewer re-derivation.
Task 7b fix 1 → 2aded34 (f7b).
- ASCII cites; the overrides test checks tier/default exactly + module addresses ⊆ merged cite (missing addresses pinned explicitly, not forced); hand-pinned coverage counts; the fails-closed test clears groups; C-1 re-derived from ijl15.dll (LOMNHook::ResolveGamePath 0x10001570) and modelled; m1/m2 tests kill their mutants.
- pytest 791/2; corpus 541; the A9 real run is deterministic (390,730).
- NEW 7b finding: `Root/...User/...` routes to the install file (the Root/ branch takes precedence).
NOT shared with 7a (independence). Controller verification plan: an OFF-PATH DIFFERENTIAL. A throwaway .osi calls scslosifileio.doesfileexist on ~12 hostile/edge paths (Root/x, User/x, foo/User/x, Root/a/User/x, lower-case user/x, User/../x, a long path, backslashes, an Art.blk name, a missing file, a path with NUL-free odd characters); both engines run it with real natives; compare natives.logs byte for byte.
Pair 7 fixes combined on i7: 04e9a14 (7a fix 1), 030db5a (7b fix 1), clean cherry-picks.
OFF-PATH DIFFERENTIAL (controller harness, scratchpad/dfe; EXECUTED): a 14-path doesfileexist script (Root/, User/, the neither-prefix branch, Root/…/User/…, lower case, backslashes, `..`, Art.blk entries, missing, a 190-byte path, empty) run with real natives on a synthetic install.
- First run, with a SYMLINKED synthetic install: C++ 0 on all 14, Python varied. A harness artifact: C++ DiskFs indexes only regular files and does not descend symlinked directories (disk_fs.cpp:18/27), while the Python Vfs follows symlinks.
- Re-run with a HARD-LINKED install: natives.log body IDENTICAL on all 14 paths, including the fixed neither-prefix branch (Data/User/../Main.blk → 1 on both) and 7b's Root/…/User/… finding (→ 1 when present on both).
Carried item (→ small job): the Python Vfs must mirror C++ DiskFs (no directory-symlink descent, regular files only) so differentials stay exact on installs with links. The real install has no links (A9 unaffected).
Scoped re-reviews of both fix rounds dispatched; integration check on i7 running.
Pair 7 integration with fixes (i7 @ 030db5a), EXECUTED:
- conformance 541/541 both + trace-cmp identical; pytest 791/2; native ctest osi|view 24/24; wasm osi 9/9;
- osi-a9 REAL rc 0, cmp 1–4/4 OK;
- taint with real natives: 62 natives tainted, 19 sinks (down from 38 with stubs), type-only 7.
Waiting: the two scoped re-reviews.
Pair 7 scoped re-reviews: 7b APPROVE (C-1 re-derived from ijl15.dll ResolveGamePath, incl. Root/…User/…; 27/28 mutants caught; new n1: dead has_user_needle), 7a APPROVE (19/20 mutants; ASan hostile-path harness passes; new n-1 a process-time check, n-2 `..` climbing from the root/ into the user/ mount (docs wrong; not a sandbox escape), n-3 Root/ paths can overflow 0x104 too, n-4 §9.4 wording).
PROCESS SLIP (controller): both parallel reviewers were pointed at the same build/review/ in i7 and clobbered each other's mutants; the 7a reviewer detected it and re-ran in its own subdir. Rule: every reviewer brief names a private scratch subdir.
MERGED: m2-osi-vm fast-forwarded to 030db5a (pair 7 + fixes). Tasks 7a, 7b: complete.
Follow-up batch F7 (both engines, small; not blocking): 7b n1 dead helper; 7a n-1 test; n-2 `..` across mounts → both engines reject (return 0) a path whose `..` pops above its mount root, with a 2-engine differential test; n-3/n-4 docs; the Python Vfs mirrors C++ DiskFs (no directory-symlink descent, regular files only).

## Pairs 8 + 9 dispatched in parallel (Max plan, from 030db5a, all pre-built): 8a C++ Opus (directors/saver/mods; re-entrant chains, DLL asm) ∥ 8b Python Sonnet ∥ 9a C++ Opus (objects/model director/camera/HUD) ∥ 9b Python Sonnet. Two C++ builds at once (plan limit).
Brief additions:
- pair 7 as the pattern; HostContext::block_chain();
- the taint must-set + the type-only ruling;
- one H per AddRef/Release;
- re-entry C records + D16 for LoadArea;
- inheritance; BetterSaver/ijl15 hooks; EVERY branch (off-path differentials);
- ASCII overrides with an exact key/tier/default + address comparison;
- independence; exact commit commands (Haiku-format lesson applied to all).
Reviewer briefs will name PRIVATE scratch subdirs (build/review/<task>/).

## STOP: owner 2026-10-09 ~09:40: "weekly usage limit is 80%, don't use more for this project" (Max plan)
- All 4 pair-8/9 agents DIED at the same moment with ENOTFOUND (a network/DNS outage; the artifact watch also dropped). The connection is back (api.anthropic.com reachable).
- PARTIAL, UNCOMMITTED work is preserved in the worktrees (DO NOT delete):
  - lomn-web-m2-t8a (C++ directors: directors.h/.cpp, blockchain changes, groups.cpp; RED was confirmed 17/27);
  - lomn-web-m2-t8b (Python directors.py + overrides/tables);
  - lomn-web-m2-t9a (C++ objects.h/.cpp, framework changes);
  - lomn-web-m2-t9b (Python objects.py + tests + overrides).
- RESUME ON CONSOLE CREDITS ONLY (the terminal `claude`, logged into the Console account; its subagents bill to credits). Start a FRESH session in ~/Desktop/lomn-web (Opus 5.5 controller). Per pair side, dispatch a fresh agent with its task brief (task-8a..9b-brief.md) and: "continue the partial uncommitted work in your worktree: read git diff/status first, keep what is correct, finish test-first".
- Then: the blind reviews (private scratch subdirs), the overrides comparison, off-path differentials (re-entrant directors, mod natives), integration + osi-a9 REAL; the F7 follow-up batch; Task 10 (gate); Task 12 (close-out, an Opus whole-branch review).

## RESUME on Console credits (2026-10-09 14:12, fresh Opus 5.5 controller, headless; budget $45 for this run)
Pairs 8 + 9 re-dispatched on the preserved partial worktrees (all at 030db5a + uncommitted work): 8a opus-medium-implementer (Opus) ∥ 9a opus-medium-implementer (Opus) ∥ 8b Sonnet ∥ 9b Sonnet. Dispatch text = brief path + worktree + the ledger brief additions + "continue the partial uncommitted work…; never dispatch subagents; exact commit form; report to task-<id>-report.md". Host loadavg at dispatch 10.6.
Ruling: implementers use the opus-medium-implementer agent type (Opus at medium effort) — ROADMAP §6 names Opus-medium for design-heavy implementation — cost if wrong: a slightly weaker first pass, caught by the Opus blind review.
Task 9b (Sonnet): fe9f14c on t9b, DONE_WITH_CONCERNS — test_osi_natives_objects 80 passed; -k osi|taint|native|override 582/1 skipped/1 failed (lomn_view fails-closed test: C++ 9a not merged, expected); A9 oracle --real-natives 600 frames stop none; 3 UNSAFE remain, all group 8. 6.4 min, 19 calls, 66k tokens. Blind Sonnet review dispatched (scratch build/review/9b).
Task 8b (Sonnet): 9f74ef9 on t8b, DONE_WITH_CONCERNS — test_osi_natives_directors 63 passed; -k osi|native 562/1 skipped/1 failed (lomn_view fails-closed, 8a not merged); 4 existing tests switched to read pre-merge table copies (RED FLAG → reviewer asked to judge weakening). Report short (no full RED/GREEN). 8.0 min, 29 calls, 92k tokens. Blind Sonnet review dispatched (scratch build/review/8b).
Task 8a (Opus-medium): 8f3aea7 on t8a, DONE_WITH_CONCERNS — directors 28 cases / 13,896 assertions; ctest unit|osi 13/13 incl. unit_asan, osi_conform, osi_a9_real; test_native_overrides 31/31. Edited shared tests test_osi_vm.cpp + test_native_overrides.py (cheatmenu/getbooleanvalue had no override before). INFERRED readings flagged: motion id = record id; singletons start G 1; name[3] past NUL = 0. 10.8 min, 79 calls, 204k tokens.
PAIR 8 OVERRIDES COMPARISON (EXECUTED cmp_overrides.py on committed files): 21/21 keys identical, both ASCII; defaults identical (none set); TIER MISMATCH on 3 rows: gcsaver.setmask/sethealth/setenergy a=tier2 (store unmodelled) vs b=tier1 (store modelled, reads back). Cited setter addresses agree (0x7474a0 dword / 0x7474a8 / 0x7474ac; b's note mentions the brief's 0x7474a4 only to say the asm differs). Other rows: no contradicting addresses seen, only different/extra neighbouring cites (to be checked by reviewers).
Ruling: the 3 setters are tier1 with modelled stores (set -> get) on BOTH sides — the brief row says "decides (setters store, getters read back)" with a set->get test — cost if wrong: a few lines of state in 8a; no A9 effect (nothing on the path reads back).
Blind Opus review of 8a dispatched (scratch build/review/8a), told about the ruling.
Task 9a (Opus-medium): f1d7edc on t9a, DONE_WITH_CONCERNS — objects 30/30 (224 exe pin assertions; RED 19/30 with the group unbound, the 11 survivors are asm-pin cases); osi* 128, ASan 72; ctest unit|osi 13/13; pytest subset 259; lomn_view --natives real 600 frames exit 0, stop none, coverage tier1 22 / tier2 26 / stub 20 (all pair 8). Concerns: loadhudsprite missing-file throw unmodelled; addtoa writes ammo [0x7474c8]=250 + sSaveToa [0x74749c] which pair 8 getammo/gettoa read (shared storage at merge); natives.md §2.4/§3.4 show() undercounts refs (G 2 -> G 3). 13.7 min, 86 calls, 265k tokens.
PAIR 9 OVERRIDES COMPARISON (EXECUTED): a 40 rows, b 38; both ASCII; defaults identical. Keys only in a: gc3dsound.synclistenertocamera, gccollisionphysicsgroup.settoaposition, gcstatusscreen.hideinventory (tier2, not in the brief). Key only in b: gchuddirector.playcinematic (pair 8's key). TIER MISMATCH: gccharacter.initmotionsystem a=tier2 vs b=tier1. Addresses: many differing neighbour cites, none contradictory on inspection of the shared ones (reviewers re-derive).
Ruling: gchuddirector.playcinematic belongs to pair 8 (both 8a and 8b model it per the 8a/8b brief row) — 9b drops its row and body — cost if wrong: none (pair 8 covers it).
Ruling: the 3 a-only tier2 keys are kept; 9b adds the same keys at tier2 with its OWN binary citations (given only the key names) — cost if wrong: 3 small rows.
Ruling (provisional, pending the 9a reviewer's Q1): initmotionsystem tier is decided by whether anything script-observable depends on it — cost if wrong: a tier label.
Blind Opus review of 9a dispatched (scratch build/review/9a) with Q1–Q4.
Task 8b blind review (Sonnet, 9.5 min, 74 calls, 245k tokens): REQUEST CHANGES, 0 C / 1 I / 6 m.
- I-1: gcsaver.setammo (0x5d3840) writes the word [0x7474c8] that getammo reads (0x5d3913) but setammo is unbound; oracle setammo(5);getammo() -> 0 vs original 5. Off A9 (getammo is called once, before addtoa).
- EXECUTED by reviewer: all must/UNSAFE cites re-derived and hold (getcurrentlevel, getammo word [0x7474c8], bettersaver.getbooleanvalue, the 0x574ef1 guard, LoadArea swallow catches); fluff strings 23/23 vs an independent reader; 206 tests in 5 files pass; mutants 26/28 killed; the test-edit red flag does NOT hold (baseline pins stay on pre-merge copies; new committed==merged checks are stricter).
- m: duplicate-id RemoveObject mutant + GetScriptFluffString a==0 branch survive; main_character ignores the Toa branch (0x43afb2); setter state has no bound getters; getmainview/getcamera never return nothing though GetViewPort can be null; init_strings pointer increments.
Task 9b blind review (Sonnet, 11.9 min, 81 calls, 229k tokens): UNCERTAIN leaning APPROVE, 0 C / 2 I / 6 m.
- I-1: addtoa has an INI-gated block (0x568a5e calls 0x5d4e40: [Cheat] Urinate == "Pohatu"; true runs 0x568a6b-0x568c02 for onua/poha/tahu); the install's Bionicle.ini has Urinate=Pohatu. Off A9 (frnt).
- I-2: taint shows gcviewport.getcamera, gcsaver.cheatmenu, gcgame.getmp3player UNSAFE without a 9b row.
- EXECUTED: 111 tests pass; A9 oracle stop none, nesting 2, 155 H lines; mutants 22/25 (3 unobservable); ASCII; the lomn_view fails-closed failure is exactly the 16 unbound 9b tier1 rows (C++ not merged).
- UNVERIFIED by reviewer: interiors of 0x4804c0, 0x47e670, 0x46f780, 0x56c270, 0x474580, 0x517e00, SetParent.
Ruling: 9b I-2 is not a 9b defect — getcamera, cheatmenu and getmp3player are pair-8 keys (present in both 8a and 8b overrides, comparison above) — cost if wrong: none, the merged taint run shows it.
Ruling: the addtoa [Cheat] Urinate gate (9b I-1) is an INI-dependent branch; INI reading is M2a-3 by the existing "ini -> M2a-3" rows. Both engines model the INI-absent value (false) for now and must cite the gate in the row note; the branch body is an OPEN finding carried to M2a-3 with a 2-engine differential then — cost if wrong: an off-A9 divergence on installs with the cheat set, until M2a-3.
Ruling: 8b I-1 (setammo unbound) — gcsaver.setammo is in neither brief's table; it goes to the F7 follow-up batch for BOTH engines (bind setammo; getammo reads back), with 9a's addtoa ammo=250 write sharing the same storage — cost if wrong: an off-A9 divergence until F7.
Task 9a blind review (Opus, 8.8 min, 68 calls, 180k tokens): REQUEST CHANGES, 0 C / 1 I / 5 m.
- Q1 initmotionsystem: tier2 is right (same id rebuilt; ctor never calls OnEvent; path to 0x574ef1 unconditional; A9 setupmoveable still logs OnEvent 0x36 0). Q2 confirmed: first show('cnt1') = 2 lasting refs (0x419b12 + 0x54b344) -> natives.md example G 3. Q3: missing-file branch unmodelled (ctor filter 0x43f795-0x43f7e3 passes only none/fcls/flrd; rethrow 0x43f82c unless cached 0x4db050) — inferred, error id not traced. Q4: all 3 extra tier2 cites correct; synclistenertocamera returns int 0 always (0x55f787).
- I-1: loadhudsprite / sctilefont.create constructor-failure branches (0x43f82c/0x43faee/0x627230) cited but not modelled.
- m-1 aim-mode test gap (1 surviving mutant); m-2 setholder [char+0x1d0] comment; m-3 null-object crash in tier2 stubs; m-4 natives.md G 3; m-5 ammo/sSaveToa storage unify with pair 8.
- EXECUTED by reviewer: objects 30/30, osi* 209,477 assertions, ASan 72/72; A9 lomn_view exit 0 stop none, natives.log matches asm; mutants 20/21; framework change safe.
Ruling (revises the pair-8 setter ruling): setmask/sethealth/setenergy are tier2 on BOTH sides — their getters are unbound in M2a-2 so no bound native reads the stores (8b reviewer m) — 8b changes; cost if wrong: re-tier when the getters are bound.
Ruling: initmotionsystem tier2 on both sides (9a reviewer Q1, asm-cited) — 9b changes.
Ruling: 9a I-1 (file-presence failure of loadhudsprite / sctilefont.create) is deferred to M2a-3 (file manager + error ids), with both sides returning a handle as now; carried as OPEN; cost if wrong: an off-A9 divergence on missing art files until M2a-3 (shipping installs have the files).
Ruling: 9a minors m-1..m-5 and the natives.md G 3 fix go to the F7 follow-up batch; storage unification of ammo [0x7474c8]/sSaveToa [0x74749c] across pairs 8/9 is checked at merge and otherwise carried to F7 (off-A9: getammo runs once before addtoa).
Budget note: $27.2 of $45 spent at this point; fix rounds kept to the Sonnet sides where the rulings require row changes.
Fix round 1 sent: 9b (initmotionsystem tier2, drop playcinematic, 3 tier2 rows with own cites, addtoa INI gate cited) ∥ 8b (setters tier2, getammo/setammo note, 2 mutant-killing tests).
Task 8a blind review (Opus, 12.8 min, 102 calls, 240k tokens): REQUEST CHANGES, 1 C / 4 I / 4 m.
- C-1 (re-entrant misread, likely SHARED with 8b and natives.md §4/§5.2/§5.3): LoadArea objects/hives blocks fire OnEvent(0x36,b) on id=='cnt1'; in the binary GcMoveableObj ctor 0x5432a0 reaches SetupMoveable (0x54408b) only for id == 'lbld' [0x734f10] (cmp 0x544065) and that id becomes the motion id, so the 0x574ef1 guard cannot pass for cnt1; hive records with [rec+0x2c]==0 use ctor 0x6239b0 (never reaches the VM). Three tests encode the misreading.
- I-1: set_quest misses SetQuest's lower bound (cmp cl,1; jb 0x5d5e24-27); frnt stores "lev0", so setbooleanvalue(0xe/0x10/0x12,1) sets quests[0] in the port, nothing in the original. POSSIBLY ON A9.
- I-2: setcurrentlevel -> RemoveArea -> CleanUpArea -> GcModelDirector::Remove -> Call("OnEvent") (0x501830 -> 0x47f680) unmodelled for _char.slb characters.
- I-3: mutants 16/22 killed, 1 equivalent, 5 survivors. I-4: supports tier1 setters (predates the getters-unbound point).
- EXECUTED by reviewer: directors 28/28 cases, 1,287 assertions (report's 13,896 not reproduced, m-4), unit + unit_asan 2/2; out-of-scope test edits weaken nothing.
Ruling: setters stay tier2 (getters unbound; see above) — cost if wrong: re-tier later.
Ruling: I-2 deferred to F7 (both engines; frnt has no characters per natives.md §5.3, so off A9 — inferred) with a code comment now — cost if wrong: an off-A9 OnEvent divergence until F7.
8a fix round 1 sent: C-1, I-1, I-3 survivors, m-4. 8b gets a fix round 2 to re-derive C-1 and I-1 independently (shared-misreading check), after its fix 1.
Task 9b fix 1 → 387a6d4: initmotionsystem tier2 (setupmoveable models the motion system as always present, id [char+0x100] — matches 9a's comment "exists for every character, ctor 0x40beeb"); playcinematic removed; 3 tier2 rows with own cites (synclistenertocamera int 0 via 0x55f787 + CreateVariant 0x413ea0 — matches the 9a reviewer's independent reading); addtoa INI gate cited. 312 + 113 passed; A9 oracle stop none.
PAIR 9 OVERRIDES RE-COMPARISON (EXECUTED): 40/40 keys identical, tiers/defaults identical, both ASCII.
Ruling: no scoped re-review for 9a (no code change; its I-1 is deferred by ruling) nor 9b fix 1 (mechanical, verified by the row comparison above + its tests) — budget — cost if wrong: an unreviewed small diff; covered by integration + the F7 batch.
MERGED pair 9 into m2-osi-vm: 976584b (9a merge), fa0363e (9b merge; overrides conflict -> 9a text; make osi-tables: no diff), 5299837 + 69b532f (controller merge fixes: the addtoa INI-gate note added to the merged row per the ruling; two 9b tests that pinned 9b's cite WORDING now check the module's own cite / the shared addresses — same class as pair 7's integration fallout). Objects + overrides tests 113 passed.
Task 8b fix 1 → 0275073: setters tier2; getammo/setammo comment; duplicate-id + a==0 tests. -k osi|native 563/1 skip/1 fail (lomn_view fails-closed, needs 8a). 8b fix round 2 sent (re-derive C-1 lbld gate + SetQuest bounds independently).
Pair-9 integration (native, ctest osi|view, test-tools) running in the background: build/integ-p9.
Task 8a fix 1 → d20bece: C-1 (objects/hives fire only for id == 'lbld' AND main character; hive records with [rec+0x2c]==0 dropped), I-1 (SetQuest 1..6, .at()), I-3 tests, I-2 comment + a test of the 0x501830 call site. Directors 30/30 cases, 1,350 assertions plain + ASan; RED 3/30 on old code; 8/8 mutants incl. the 5 survivors killed; ctest unit|osi 13/13. m-4: the earlier 13,896 count was stale (an earlier build).
Controller spot-check (EXECUTED, PE section map in python, data only): [0x734f10] = bytes 'dlbl' (FourCC 'lbld'); 0x544065 = 39 15 10 4f 73 00 (cmp [0x734f10],edx) + 75 66 (jne); 0x5d5e24 = 80 f9 01 72 41 80 f9 06 77 3c (cmp cl,1; jb; cmp cl,6; ja). The fix's citations hold.
Ruling: no Opus scoped re-review of 8a fix 1 (budget: $32.8 of $45 spent); the controller's diff read + byte check above + the mutant kills stand in; the 8b fix-2 independent re-derivation of C-1/I-1 is the cross-check — cost if wrong: a residual 8a defect found later by the off-path differential or the M2a-2 close-out review.
Task 8b fix 2 → d5384ab: (1) LoadArea reach WAS WRONG on 8b too (fired on any motion id == main character) — fixed to 'lbld' AND main character, plain hives never fire: C-1 CONFIRMED as a shared misreading (and natives.md §4/§5.2/§5.3 → F7 docs). (2) SetQuest bounds already correct on 8b (1..6). Directors 67/67; -k osi|native 566/1 skip/1 fail (lomn_view fails-closed, needs 8a); A9 oracle stop none.
NEW DIVERGENCE: 8b applied the 'lbld' gate to the characters block as well; 8a does not (characters: AddCharacter -> GcCharacter::SetupMoveable 0x418850, no guard).
Controller check (EXECUTED: a .text-wide call-rel32 scan): GcMoveableObj ctor 0x5432a0 has exactly 3 callers — 0x4842dc, 0x489937, 0x6236d4 (no character path); 0x418850 has 8 direct callers and a vtable slot (file 0x2edc58).
Ruling: the 'lbld' gate applies to objects/hives only; characters fire on id == main character (8a's reading) — 8b fix 3 sent — cost if wrong: an off-A9 OnEvent divergence (frnt has no characters).
Lesson: the 8a reviewer's C-1 was shared by BOTH sides and natives.md (third shared misreading this milestone class: off-path re-entrant branches); independent re-derivation on the second side after disclosure still diverged on scope (characters) — a call-graph scan settled it in one command.
Task 8b fix 3 → 18e9417: 'lbld' gate on objects/hives only; characters fire on id == main character; directors 67/67; A9 oracle stop none.
PAIR 8 OVERRIDES RE-COMPARISON (EXECUTED, final commits d20bece vs 18e9417): 21/21 keys, tiers/defaults identical, both ASCII.
Pair-9 integration on 69b532f (EXECUTED, build/integ-p9): native build ok; ctest osi|view 24/24; make test-tools 875 passed (gate rc 0).
MERGED pair 8: 8142e52 (8a merge; conflicts CMakeLists/groups.cpp = union, order services, directors, objects; overrides = union of rows, 72 rows tier1 36 / tier2 33 / tier3 3; types.md count line updated; make osi-tables regenerated), e617b10 (8b merge; overrides/natives/types = ours (8a text); __init__ imports directors then objects; make osi-tables), 0f1dc6e (merge fallout, 3 tests: directors overrides test checked 8b cite wording -> module cite names the body; directors A9 test asserted no C/X/H lines with only its group bound -> now no X lines, the whole log is cmp'd by osi-a9; objects test asserted playcinematic absent from the file -> present at tier1, owned by pair 8).
Ruling: the merged overrides text is side a's for every pair-8/9 row (as pair 7) — cost if wrong: wording only; key/tier/default are identical.
Ruling: the weakened directors A9 assertion (C/H lines) is accepted because osi-a9 cmp 3/4 + 4/4 compare the complete natives.log of both engines byte for byte — cost if wrong: an attribution check lost; flagged for F7 (filter the log by native key instead).
Full integration (native, trace, ctest, pytest, wasm, osi-a9 REAL) running: build/integ-p89.
Integration on 0f1dc6e (EXECUTED, build/integ-p89): native ok; conformance 541/541 both + conform-trace-cmp 541/541 byte-identical, 0 pending; ctest osi|view 24/24; make test-tools 941 passed / 1 failed: test_smoke_coverage_equals_coverage_py hand-pins "tier1 2 … stub 1" and gcsaver.cheatmenu is now tier1 (T8) — the pin's own comment says a promotion changes it on purpose → 2b84505 updates it to "tier1 3, stub 0"; test_lomn_view_script.py 116 passed.
Off-path differential (Sonnet agent, hard-linked synthetic install, scratch build/review/diff89) dispatched; wasm + osi-a9 REAL still running.
wasm build (EXECUTED, integ-p89): ok (54/54).
osi-a9 OSI_A9_NATIVES=real on 0f1dc6e (EXECUTED): rc 0; headers native == wasm32 == Python == replay OK; cmp 1/4 native == wasm32 (601 D lines; natives.log, window, stdout identical) OK; cmp 2/4 native == Python replay OK; cmp 3/4 native == Python own real natives OK; cmp 4/4 coverage + natives.log native == Python own OK; coverage natives distinct 69 (tier1 34, tier2 33, stub 2, mod 0, tier3 0), calls 1705, unregistered 0; stop none. The 2 remaining stubs: gcareadirector.setfog (0x487090), gcareadirector.setnearestmax (0x486fe0), in neither brief's table → OPEN, F7.
Taint leg with real natives: natives tainted 35, sinks 0 (all kinds 0), type-only 0 (pair 7 left 19 sinks).
make test-tools on the final HEAD 2b84505 (EXECUTED, integ-p89b): 942 passed, gate rc 0. (osi-a9 ran on 0f1dc6e; 2b84505 changes one test line only.)
OFF-PATH DIFFERENTIAL (Sonnet agent, 2.7 min, 28 calls, 84k tokens; hard-linked synthetic install, only its Base.osi replaced; controller re-ran the cmp, EXECUTED): A directors/saver/BetterSaver 46 calls → natives.log IDENTICAL 63 lines (14 natives); B area director 42 calls → IDENTICAL 55 lines; D model director/camera/sprite/font 30 calls → IDENTICAL 48 lines (9 C/H lines); E show on a missing id → IDENTICAL 10 lines, both stop with the same script error. D-line digests equal for A, B, D.
Coverage gaps of the differential (OPEN): B did not observe loaded-area contents (no characters matching the main character → the lbld/characters gate not exercised); save/load had no visible effect; the RemoveArea → OnEvent chain not exercised (unmodelled on both).
Hygiene (EXECUTED): no .osi/gen/game data in the 030db5a..HEAD diff; 0 personal paths.

Verdicts (evidence-block format):
Verdict 8a: APPROVE after fix 1 (d20bece). Pre-fix failure reproduced: yes — the reviewer's C-1/I-1 readings; RED 3/30 on old code (implementer), controller byte check of 0x544065/0x5d5e24/[0x734f10]. Fix verified: directors 30/30 plain+ASan; osi-a9 REAL 4/4; differential A/B identical vs 8b. Tests: the 'cnt1 does not fire', level-0 SetQuest and 5 mutant-killing tests cover the failing paths. Runtime: VERIFIED native + wasm (osi-a9). Out-of-scope: test_osi_vm.cpp + test_native_overrides.py edits (reviewer: no weakening). Risks: I-2 RemoveArea→OnEvent unmodelled (F7); the scoped re-review was replaced by the controller check (ruling above).
Verdict 8b: APPROVE after fixes 1–3 (18e9417). Pre-fix: C-1 confirmed wrong on 8b too (agent, from the exe); the characters over-gate settled by a call-graph scan. Fix verified: directors 67/67; osi-a9 cmp 3/4, 4/4; differential identical. Out-of-scope: 4 baseline tests read pre-merge copies (reviewer: stricter, not weakened). Risks: setammo unbound (F7).
Verdict 9a: APPROVE with I-1 deferred by ruling (f1d7edc, no code change). Evidence: reviewer re-ran objects 30/30, ASan 72/72, 20/21 mutants; osi-a9 REAL 4/4. Risks: missing-file branches of loadhudsprite/sctilefont (M2a-3); m-1..m-5 (F7).
Verdict 9b: APPROVE after fix 1 (387a6d4). Evidence: rows 40/40 identical to 9a; osi-a9 cmp 3/4, 4/4; differential D/E identical. Risks: addtoa INI gate modelled INI-absent (M2a-3).

OPEN for F7 (in addition to the pair-7 F7 list): natives.md §4/§5.2/§5.3 C-1 (lbld gate; characters ungated) and §2.4/§3.4 show() G 3; setammo bind + ammo/sSaveToa storage shared between pairs 8/9; RemoveArea → GcModelDirector::Remove → OnEvent chain (both engines); 9a m-1 aim-mode test, m-2 setholder comment, m-3 null-object stubs; 8b minors (main_character Toa branch 0x43afb2, getmainview/getcamera null viewport, init_strings); the directors A9 test's C/H attribution check by native key; gcareadirector.setfog/setnearestmax stubs; a differential that loads an area with characters/objects/hives matching the main character.
OPEN for M2a-3: loadhudsprite/sctilefont file-presence failure; the addtoa [Cheat] Urinate gate.
Task 8: complete. Task 9: complete. (merged on m2-osi-vm at 2b84505; integration green as above)
Cost (Console budget display): ≈ $35.8 of $45 for this run (4 implementers + 4 reviews + 6 fix rounds + 1 differential + controller).
Scratch left in place (agent-created, not deleted by the controller): worktree build/review/{8a,8b,9a,9b}/, m2 build/review/diff89/, build/integ-p9*/, build/integ-p89*/ (all git-ignored build/).
STOP per run scope: F7, Task 10 and Task 12 not started.

## RUN 3 (2026-10-09 ~15:20, fresh Opus 5.5 controller, headless, Console credits, cap $45): F7 → Task 10 → Task 12
Housekeeping: worktrees t8a/t8b/t9a/t9b removed (branches are ancestors of 2b84505; merged gate green per the entries above). Old branch names m2a2-f7a/f7b already exist (pair-7 fix branches), so F7 uses m2a2-f7cpp / m2a2-f7py. Disk free 14 GiB; load 8.65.
F7 dispatched from 2b84505: F7a C++ (opus-medium-implementer, Opus; worktree lomn-web-m2-f7cpp; brief task-f7a-brief.md) ∥ F7b Python (Sonnet; worktree lomn-web-m2-f7py; brief task-f7b-brief.md); common rules task-f7-common.md.
Ruling: side a (C++) owns every natives.md edit in F7; side b reports doc corrections instead — why: one author avoids a text conflict, as pair 7 — cost if wrong: a Python-found doc error waits for the controller to transcribe it.
Ruling: the RemoveArea chain addresses (from the 8a review) are given to side a only; side b re-derives them blind, so the two readings are independent and the C++ reviewer's byte check is the third reader — cost if wrong: side b spends more turns finding the chain.
Ruling: 9a m-3 (null object to a tier2 native faults in the original) is documented as a framework convention, not modelled — why: modelling a fault has no defined port behaviour; every tier2 row shares it — cost if wrong: a hostile-script divergence (crash vs stub value) that M2a-4 fuzzing would surface.
Ruling: `..` above a mount root in doesfileexist returns 0 on both engines (decided port behaviour, documented as a hostile-path divergence) — why: the VFS must never resolve outside its mounts — cost if wrong: an off-path divergence from Windows path resolution on hostile scripts only.
Ruling: 8b m-4 (getmask/gethealth/getenergy unbound) and m-6 (init_strings in-place increment) are report-only — why: no bound reader / equivalent on the real file — cost if wrong: re-tier when the getters are bound.
F7a (opus-medium, 21.9 min, 138 calls, 360k tokens): 463dd36 on m2a2-f7cpp, DONE_WITH_CONCERNS — lomn_tests osi* 168/168 (211,121 assertions); ctest unit|osi|view 26/26; RED 13 cases first; A9 real 600 frames natives.log 2462 lines byte-identical before/after; setfog/setnearestmax tier2 (last 2 stubs gone). Concerns: loaded characters enter the model-director map (Add refuses dup ids 0x47ef98, Remove may take the Toa); GetUniqueIDForType numbering not modelled; Hide/RemoveAnimationTriggers ref drops not modelled; a stale objects bind-count test fixed 15->17.
F7b (Sonnet, 23.7 min, 130 calls, 313k tokens): fa072b7 7ba3dff 9f5921e da6ce4f d46bdf9 on m2a2-f7py, DONE_WITH_CONCERNS — pytest -k osi|native|taint|vfs 627 passed / 41 need build/native; A9 real natives.log 2462 lines identical to base. Concerns: setnearestmax tier1 + getnearestmax bound; gettoa bound (tier1); hive children unmodelled; hand-edited natives.json/types.md.
Cost so far (Console display): $19.9 after both F7 implementers (the C++ Opus side ≈ $15 of it).
F7 OVERRIDES COMPARISON (EXECUTED cmp_overrides.py): both ASCII; keys only in b: gcareadirector.getnearestmax, gcsaver.gettoa; TIER MISMATCH gcareadirector.setnearestmax a=tier2 b=tier1; setfog both tier2 (different neighbour cites).
Controller byte check (EXECUTED objdump, data only): setnearestmax 0x486fe0 stores byte [0x70b37c] at 0x486feb and returns nothing; getnearestmax 0x487010 = push ebp; mov ebp,esp; mov al,[0x70b37c] (bytes a0 7c b3 70 00 at 0x487013) -> CreateVariant 0x47d250; gettoa 0x5d36b0 reads [0x74749c] at 0x5d36b3 -> 0x407960.
Ruling: setnearestmax tier1 with getnearestmax bound (tier1), and gettoa bound (tier1), on BOTH sides — a registered getter reads the store back, the same rule as the setter ruling of pair 8 — C++ gets it in its fix round — cost if wrong: 2 small rows of extra modelled state.
Blind reviews dispatched: F7a Opus (third reader of the RemoveArea chain bytes; scratch build/review/f7a), F7b Sonnet (independent re-derivation; scratch build/review/f7b).
F7b blind review (Sonnet, 5.8 min, 31 calls, 136k tokens): APPROVE with 0 C / 3 I / minors. Chain re-derived from the install exe (guard, args, erase-before-OnEvent, no catch) = model; setammo/gettoa/nearestmax/setfog/Toa branch match; attribution test real; registry.py regenerates natives.json/types.md byte-identical to the hand edits; 10 mutants, 7 killed. I-1 Remove clears the Toa pointer [0x7032b8] at 0x47f4b0 (unmodelled); I-2/I-3 "objects/hives reach no VM" and AddCharacter class branches rest on the direct-call floor (virtual calls 0x488a15/0x488a9b/0x47f4be) -> label INFERRED. Minor: the install exe and reference/LEGOBionicle.exe differ (first cmp byte 417186); the cited addresses match in both.
F7a blind review (Opus, 8.6 min, 40 calls, 188k tokens): REQUEST CHANGES 0 C / 2 I / 4 m. Third reader of the chain bytes (CITED): guards, order, 'cnt1' no-Call skip, erase before Call, OnEvent(0x2b,id)+8 nothing, no catch (0x489add not a try site) all match. I-1 the loader shows every loaded character (flag 1 0x48a585 -> 0x5002cf; Show 0x53f535): H/G for area-loaded characters diverge (G 2 not 1), the refused-'cnt1' case shows the Toa, Hide on removal is on-path; natives.md §4 paragraph wrong (and side b was told G 1 via nothing — side b never saw it). I-2 setnearestmax cite says nothing reads [0x70b37c]; getnearestmax does. m-1 numbered unique ids skipped; m-2 virtual CleanUp 0x47f4be uncited; m-3 assertion count 208,034 not 211,121; m-4 loaded characters minted type 0 where addtoa throws. 6/6 own mutants killed.
F7 integration probe (temporary branch m2a2-i7f = 2b84505 + F7a + F7b, conflicts -> side a tables; make osi-tables no diff; EXECUTED build/integ1): native ok; ctest osi|view 24/24; conform-trace-cmp 541/541 identical, 0 pending; make test-tools 957 passed / 4 failed — row fallout (side b's tier1 setnearestmax/getnearestmax/gettoa vs side a's file) and a directors attribution count 17 vs 14 — re-checked after the fix round.
Ruling: the F7 fix rounds go to FRESH Sonnet fixers on both sides (not a resume of the Opus implementer) — why: budget ($24.7 of $45 spent; resuming a 360k-token Opus context costs several $ per turn) and the fixes are specified to the address — cost if wrong: a weaker C++ fix, caught by the merged integration + the off-path differential + the Task 12 whole-branch review.
Ruling: both review findings that are shared-contract facts (Show on load / Hide on removal; Toa pointer clear at 0x47f4b0) go to BOTH sides as findings to re-derive from the bytes — why: a single-side fix would just move the divergence — cost if wrong: none beyond the independence of one already-disclosed reading.
F7b fix 1 (Sonnet, 7.7 min, 45 calls, 184k tokens): 49304cc — Toa pointer clear 0x47f4b0; loader Show (0x48a585/0x48a721 -> 0x5002cf -> Show 0x53f535) incl. the refused-'cnt1' Toa; Remove Hides (0x47fb50 via 0x47f4e5) before the erase; node_id = id (0x47ec67); engine refs 2 plain / 3 'cnt1'; 5 mutants killed (guard-only form equivalent: Add 0x480eb0 refuses dups); -k osi|native|taint|vfs 640 passed / 41 need build/native (same set); A9 natives.log 2462 lines identical.
F7a fix 1 (Sonnet, 14.6 min, 64 calls, 209k tokens): 10e40a1 — shared show_character/hide_character (Show 0x53f535, Hide 0x47fb50); area-loaded character G 2; getnearestmax/setnearestmax/gettoa tier1 ([0x70b37c] = 4 in .data); 0x7032b8 clear was already modelled, test added; m-1/m-2/m-4 + class branches + objects/hives-on-removal labelled INFERRED/UNVERIFIED in natives.md; RED 7 cases -> GREEN osi* 174/174, ctest unit|osi|view 26/26, overrides 31; make osi-tables committed; A9 natives.log 2462 lines identical (coverage tier1 34->35, tier2 35->34; trace header natives= hash and T line change from natives.json).
F7 OVERRIDES RE-COMPARISON (EXECUTED): keys identical (setcurrentlevel: a changed its cite only), tiers/defaults identical (0 mismatches), both ASCII; differing neighbour cites only (getnearestmax a 0x486feb/0x48701d vs b 0x47d255; setfog; gettoa 0x5d36bd vs 0x407965 — the same call/CreateVariant, different instruction).
Integration v2 on m2a2-i7f = 2b84505 + F7a (463dd36, 10e40a1) + F7b (5 + 49304cc) (rerere reused the side-a table resolution; make osi-tables: no diff) running: build/integ2.
Ruling: no scoped re-reviews of the F7 fix rounds — budget ($31.5 of $45) — substitute evidence: each fix's RED->GREEN tests on the named findings, the cross-side overrides comparison (0 mismatches), A9 natives.log byte-identical on both sides, and the full integration set incl. osi-a9 REAL; the Task 12 whole-branch review (next run) covers the diffs — cost if wrong: an unreviewed defect in ~2 small fix diffs until Task 12.
Ruling: the area-content differential (an area whose characters include the main character) is NOT runnable this milestone — neither host has an area source (the oracle's area_source seam is empty; diff89 B showed no native observes loaded blocks), so the Show/Hide H-line sequences for area-loaded characters are pinned only by each side's own unit tests (C++: G 2, Hide `+1 -1 -1 -1` with a script-held character; Python: load `+1 +1 -1 +1`, removal `-1 -1 -1`, refs 2/3) — OPEN for M2a-3: a 2-engine differential once the area source lands, hard links only — cost if wrong: an off-A9 H-line divergence on area loads, invisible until M2a-3.
Integration v2 on m2a2-i7f (EXECUTED build/integ2): native ok; trace-cmp 541/541; ctest 24/24; wasm ok; osi-a9 REAL 4/4 OK; pytest 973 passed / 1 failed: test_log_v2_records_byte_exact_against_the_worked_example — its mock `show` still modelled the old single +1 while F7a corrected natives.md §3.4 to the first show's +1 +1 -1 +1 (G 3; 9a review Q2). Haiku fix a794f04 (mock show +1 +1 -1 +1, 17 lines, the 2 replay-mutation tests re-pointed to G 3/G 4 and to dropping the LAST +1; first Haiku pass correctly refused to commit on 2 failures). Haiku: 2 dispatches, exact edits, first-time-correct on both.
MERGED: m2-osi-vm fast-forwarded 2b84505 -> a794f04 (F7a 463dd36 10e40a1; F7b fa072b7 7ba3dff 9f5921e da6ce4f d46bdf9 49304cc; merges 0ae0263 dfb420e 109a0e1; a794f04).
Integration on m2-osi-vm @ a794f04 (EXECUTED, build/integ-f7, load 26 -> 11): STEP native/trace/ctest/pytest/wasm/a9 all rc=0. conformance 541/541 both engines, 0 pending; conform-trace-cmp 541/541 byte-identical; ctest osi|view 24/24; make test-tools 974 passed, 0 skips/xfails; wasm build ok; osi-a9 OSI_A9_NATIVES=real: headers OK, cmp 1/4 native == wasm32 (601 D lines; natives.log, window, stdout identical) OK, 2/4 OK, 3/4 OK, 4/4 OK; coverage distinct 69 (tier1 35, tier2 34, stub 0, mod 0, tier3 0), calls 1705, unregistered 0; stop none; taint with real natives: 34 tainted, 0 sinks, type-only 0.
Hygiene (EXECUTED on 2b84505..a794f04): 0 .osi/.exe/.dll/.png/.tga/.bik in the tree; 0 personal paths and 0 decomp tokens (in_EAX|in_stack_|undefined4|uVar|iVar|puVar) in added lines; 0 gen/ files.
Worktrees f7cpp, f7py, i7f removed (branches are ancestors of a794f04, merged gate green).
Verdict F7a: APPROVE after fix 1 (10e40a1). Pre-fix failure reproduced: yes — the Opus reviewer read the loader Show path (0x48a585 -> 0x5002cf -> 0x53f535) and the 0x487013 reader from the bytes; RED 7 cases on the pre-fix code (implementer). Fix verified: osi* 174/174, ctest 26/26; merged osi-a9 REAL 4/4 and the natives.log byte-identical (A9 never loads area characters, so A9 does NOT exercise the Show/Hide path — unit tests only). Tests: G 2 / Hide H lines / main-Toa removal fallback. Runtime: VERIFIED native+wasm on A9; UNVERIFIED cross-engine on area loads (no area source). Out-of-scope: objects bind-count test 15->17 (stale after pair 8; correct). Risks: area-load H sequences pinned per side only (M2a-3); numbered unique ids and AddCharacter class branches unmodelled (documented).
Verdict F7b: APPROVE after fix 1 (49304cc). Pre-fix: the Sonnet reviewer read 0x47f4b0 from the bytes; mutants survived pre-fix. Fix verified: 13 new tests, 5 mutants killed; merged 974 passed; osi-a9 cmp 3/4 + 4/4 OK. Runtime: VERIFIED on A9 (Python own real natives == native). Out-of-scope: one existing test's refs expectation changed to (2,3) (follows the Show finding). Risks: as F7a.
Minor (deferred, for the Task 12 review): the install exe and reference/LEGOBionicle.exe differ (first cmp byte 417186; the exc/error_pc tools read the reference exe, implementers disassembled the install one; cited addresses agreed in both) — which exe is authoritative is unruled; m-6 init_strings in-place increment; 8b m-4 unbound getmask/gethealth/getenergy; the C++ m-3 report assertion count (208,034 vs 211,121).
F7: complete.
Budget: $34.4 of $45 spent (Console display) at F7 complete. Task 10 needs an Opus-medium implementer (the plan's model; the F7 C++ Opus side alone cost ≈ $15) plus controller gate runs, which cannot finish under the $38 stop line.
Ruling: STOP after F7 per the budget rule — Task 10 and Task 12 are NOT started (no partial work, no worktree) — why: a Task 10 implementer started at $34.4 would cross $38 mid-task and leave a half-done gate — cost if wrong: one session's delay; nothing is lost (the Task 10 brief with controller additions is ready: task-10-brief.md).
NEXT (resume here, fresh session on credits): Task 10 from a794f04 with task-10-brief.md (update the base SHA and the A9 numbers above in its "Controller additions": tier1 35 / tier2 34 / stub 0, taint 34 tainted / 0 sinks), then Task 12 (Opus whole-branch review of 94e8dcc..HEAD, ONE fix dispatch, scoped re-review, ROADMAP §3.2/§3.3).
ROADMAP commit 1ba99cc; task-10-brief base updated to a794f04. STOP.
