### Task 10: A8/A9/A10 integration, the worklist loop, nesting re-measurement, the mutation bite (Opus 5.5 medium)

**Files:**
- Create:
  - `tools/research/oracle/mutate_natives.py`
  - `tools/tests/test_mutate_natives_anchors.py`
  - `perf/osi-a9.jsonl` (the first row)
- Modify:
  - `docs/knowledge/measurements.md`
  - `docs/osi/natives.md` (the measured cap)
  - and, only if Decision 11 changes the cap or `STACK_SIZE`: `vm.h`, `osi_vm.py`, `CMakeLists.txt`

**Steps:**
- [ ] **Measure first.** Run the native and Python legs of `osi-a9` yourself (the controller runs the full target):
  - `build/native/lomn_view --script 1 --frames 600 --keys …`;
  - `osi_vm.py --run --real-natives …`;
  - `osi_vm.py --run --replay …`.

  Record:
  - the `D`-line `cmp` results;
  - the coverage report;
  - the taint report;
  - `nesting high-water` and `stack high-water`;
  - the number of `P` lines. The atof rounding chain is an A9 risk: if there are any `P` lines, list every converted text and check that each is exactly representable in f32, or open a finding;
  - instructions per frame p50/p95, with `n` against 600.
- [ ] **Worklist loop.** Each hit native that is UNSAFE and not tier 1, or is tier 3, gets a row and is dispatched as a pair with the Task 7–9 template.
  - **Escalation:** if the first real run's worklist exceeds 16 tier-1 candidates (the flip probe's CHANGED + CHANGED-NULL count, which Tasks 7–9 were sized for), stop and send the controller a replan.
- [ ] **Nesting (carried).** Record the measured maximum M. Measure the wasm bytes per level on the deepest real chain with `-sSTACK_OVERFLOW_CHECK=2`, by binary-searching the depth that traps at a reduced `STACK_SIZE`. Apply Decision 11, and record M, the bytes per level, the cap and `STACK_SIZE`.
- [ ] **Host sources (C3).** Over 900 frames without keys, count the `C -1` records. The expected count is 0 unless Task 2a §6 says otherwise; if there are any, compare each with Task 2a's hand computation. The Python own-models `D` lines must match over the 900 frames.
- [ ] **Mutation bite:** run `mutate_natives.py` (6 mutants + control). Each mutant is CAUGHT on its named `cmp`; the control is MISSED. After each run, the source is restored and checked with `cmp`.
- [ ] Write the A15 row into `perf/osi-a9.jsonl`, with `loadavg_start`/`loadavg_end` (Task 11) and no thresholds.
- [ ] Commit: `M2a-2 T10: A8-A10 on Base.osi with real natives (native == wasm32 == Python own models == replay), nesting re-measured, mutation bite`.

**Verification (the controller, on a frozen copy; this is also the M2a-2 gate):**
- `make check` (background, `timeout 2400`, with `LOMN_DATA`): every layer green.
- `make conform-trace-cmp`: N/N.
- `make osi-diff`: 4/4, and the `D` lines equal the pre-M2a-2 digest (the A6 differential).
- `make osi-a9`: 4/4. The coverage report shows `tier3 0` and `unregistered 0`, and the taint report rates every native safe or tier 1 (A10).
- `build/native/lomn_view --install "$LOMN_DATA/install" --script 1 --frames 1` exits 0 and prints the coverage report (A8).
- `mutate_natives.py --all`: 6 CAUGHT, control MISSED.
- Hygiene:
  - `git ls-tree -r HEAD | grep -E '\.(osi|exe|dll|png|tga|bik)$'` is empty;
  - no new personal-path hits from `git grep`;
  - the decomp-block grep (`in_EAX|in_stack_|undefined4|uVar|iVar|puVar`) over the added lines finds 0.



## Controller additions (binding; they supersede the plan text where they differ)
Context: Tasks 1–9 and the F7 follow-up batch are merged on m2-osi-vm (base a794f04 or later; SHA in your dispatch). At a794f04 osi-a9 REAL: 4/4 OK, coverage tier1 35, tier2 34, stub 0, tier3 0; taint 34 tainted, 0 sinks. Both engines (C++
`engine/src/osi/`, Python oracle `tools/research/osi_vm.py` + `tools/research/osi_natives/`) have real natives for every
native on the A9 path; at 2b84505 `make osi-a9 OSI_A9_NATIVES=real` gave cmp 1–4/4 OK, coverage distinct 69 (tier1 34,
tier2 33, stub 2), calls 1705, unregistered 0, stop none; taint with real natives: 35 tainted, 0 sinks. F7 targets the
2 stubs. Read `implementer-rules.md` (same directory) — binding. You may read BOTH engines (this is the integration task).

Split of the heavy runs (budget + host load):
- YOU run: the measure-first legs (native lomn_view and Python legs, not the `make osi-a9` target), taint, event_probe,
  the probes, the nesting/wasm stack measurement (one wasm build in your worktree's build dir is fine; run it in the
  background), unit tests and the anchors test, and ONE mutant of mutate_natives.py end to end (show CAUGHT + restore cmp)
  plus the control (MISSED).
- The CONTROLLER runs `mutate_natives.py --all`, the full `mutate_vm.py` re-run and the gate list. Make both tools runnable
  headless from a scratch tree path argument, logging per mutant, exit 0 only when every mutant is CAUGHT and the control MISSED.

Extra items carried into Task 10 by earlier rulings (all binding):
1. **Taint iterated with real natives** (`tools/research/osi_taint.py … --real-natives`) until no new UNSAFE; any UNSAFE native that is not tier 1, and any tier3/stub hit, goes on the worklist (rows + a report line; escalate per the plan if > 16).
2. **mutate_vm S07 unit gap:** `engine/tests/test_osi_vm.cpp` ~line 1427 ("a nested engine call's marker is the PC after") no longer bites mutant S07 after the 0x82 PC-code move. Restore a unit test that bites S07 (RED on the S07 mutant, GREEN on the real code); show `mutate_vm.py <scratch> --only S07` reports the `unit` check as BITE. Re-point any other stale anchors you find with `--list`.
3. **swallow_count probe:** count swallowed nested errors (D16; `X` records / the swallow path) on the real 600-frame A9 run, both engines, and print the count (expected 0: frnt has no swallow per natives.md §5.3 — a non-zero count is a finding to report, not to fix).
4. **event_probe with real natives:** re-run `tools/osi/event_probe.py` (the D8 probe) with real natives; compare with the Task 2a result (235 injections: 229 SAME, 6 CHANGED, all confined to frames 0–4; plus the repeated wall-closeness case). Report the new counts and any CHANGED outside frames 0–4 (that is a STOP: hand back a partial report).
5. **T6 m2, coverage on error runs:** lomn_view's coverage report must count a registered native that throws mid-call (as `coverage.py --trace` does), so a native failing on the A9 path shows in the report. Test-first; document it in the usage comment.
6. **N-1:** on the M-7 refcount-invariant mismatch path the native's returned variant is not disposed before the throw — dispose it first (test under the ASan target). **N-2:** measure the M-7 check's cost per native return on A9 (instructions or wall time with/without, n reps, print n) and record it in measurements.md.
7. **Nesting / D15:** use the real deepest chain (the merged real natives), binary-search the trapping depth at reduced STACK_SIZE with `-sSTACK_OVERFLOW_CHECK=2` on the wasm build, record M, bytes/level, the cap and STACK_SIZE, apply Decision 11.
8. Every number you write comes from a command you ran; print the sample count next to every rate or percentile (instructions per frame: n against 600).
9. Commit subject per the plan (several commits allowed, each starting `M2a-2 T10:`).

Report path: `.superpowers/sdd/2026-10-09-lomn-web-m2a2/task-10-report.md` in ~/Desktop/lomn-web/lomn-web-m2. Return only a <=15-line summary.
