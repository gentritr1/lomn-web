# Process experiments (owner-approved 2026-10-07)

Rules: CLAUDE.md §10. Each experiment states its expected saving, exact commands, the precondition, and a quality guard at least as strict as the gates the work already passes. Results are recorded here with numbers, whether the route is adopted or rejected. Ledger: `~/.claude/process-metrics/lomn-web.jsonl`.

## E1: warm build copies for frozen-copy verification

**Why.** Every reviewer and every controller check extracts a fresh `git archive` and rebuilds native, wasm32 and (often) ASan from nothing. With several checks per task, rebuilds are a large share of review time. This is the baseline cost, measured below, not assumed.

**Route.** Extract the archive as before, then clone a finished `build/` of a nearby commit into it with APFS copy-on-write (`cp -c -R`). CMake then rebuilds only what changed. The source tree is still the frozen archive, so the evidence class is unchanged; only build time changes.

**Precondition.** The cloned build directory comes from a build of the same toolchain (same `.toolchain`, same `deps.lock`), and CMake reconfigures in the new directory, so absolute paths are updated. If CMake reports a changed compiler or cache path, delete the clone and build fresh.

**Quality guard.** The same tests must pass with identical counts. The native == wasm32 `cmp` lines and the A7/A9 golden comparisons must be byte-identical, so no stale object may leak in. Null control: fresh vs fresh.

**Method.** At least 3 interleaved reps per arm (ABAB) of `make native test-native` on the same commit, with loadavg logged per rep. Adopt only if the median saving exceeds the fresh-vs-fresh null spread and every rep passes the guard. An agent may adopt this route: it changes no artifact (§10).

**Result.** Pending.

## E2: implementer model (Sonnet 5.5 vs Opus 5.5 at medium effort)

**Question.** For planned, test-first fix lists, does an Opus-medium implementer reach approval with fewer rounds or less time than Sonnet, at the same quality?

**Arms.**
- Sonnet: the `general-purpose` agent with `model: sonnet`.
- Opus-medium: the `opus-medium-implementer` agent (`model: opus`, `effort: medium`).

**Tasks.** The two next cleanup lists, each split into two batches of similar size and type:
- M1a-4 cleanup (C1–C4) on branch `m1a4-hardening`;
- M1b Task 8 (P1–P10) on branch `m1b-viewer`.

Arm order is counterbalanced: in M1a-4, batch 1 goes to Sonnet and batch 2 to Opus-medium; in M1b, batch 1 goes to Opus-medium and batch 2 to Sonnet.

**Quality guard (unchanged gates).**
- Every batch gets the same reviewer: Opus at default effort, same rubric. The reviewer is not told which arm wrote the code.
- Every item is test-first, and the mutations must bite.
- The controller runs its frozen-copy verification.
- A faster arm with more findings or more fix rounds is a downgrade.

**Measures, per batch.**
- wall-clock minutes, tool calls and tokens (from harness usage);
- review verdict, and number of findings by severity;
- fix rounds until approval;
- whether the controller verification passed first time;
- loadavg at start and end.

**Decision rule.** With 4 batches this is a first signal, not proof. A route change needs a replicated difference in approved-first-time per hour beyond batch-to-batch noise. If the signal is ambiguous, continue the split through the first M2 tasks before changing defaults. Reviews stay on Opus at default effort whatever the outcome; the deep-review findings this milestone came from that depth.

**Result.** Pending.

**Deviation (2026-10-07 ~22:35).** The `opus-medium-implementer` agent definition was not loaded by the running session ("Agent type ... not found"; definitions load at session start), and per-dispatch effort cannot be set. Round 1 therefore compares **Sonnet vs Opus at default effort**, labelled as such; the Opus-medium arm runs after a session restart picks up `.claude/agents/opus-medium-implementer.md`.


**Session restart (2026-10-07 22:24).** The controller session restarted mid-round; the three running agents were stopped and resumed from their transcripts (M1b batch A had committed P3; M1a-4 batch 1 had uncommitted edits). Round-1 wall-clock figures therefore include the stop and are flagged `interrupted: true` in the ledger; tool calls, tokens, review findings and fix rounds stay comparable. After the restart the `opus-medium-implementer` agent type is available, so round 2 runs as designed: M1a-4 batch 2 → Opus-medium, M1b batch B → Sonnet.

**Blinding flaw (found by the batch-1 reviewer).** Implementers' commit trailers name the model (`Co-Authored-By: Claude Sonnet 5.5`), and review packages include `git log`, so the batch-1 review was not blind. Remaining E2 reviews get a diff-only package (no commit list, trailers stripped) plus the brief; the reviewer is asked not to run `git log`. Batch-1 results are reported as unblinded.

### E2 round 1 result (2026-10-08 00:20)

| Batch | Arm | Items | Review | Verdict | Findings (crit/imp/minor) | Wall | Tool calls | Tokens |
|---|---|---|---|---|---|---|---|---|
| M1a-4 cleanup 1 | Sonnet | 3 (7 checks) | unblinded (trailers) | approved first time | 0 / 1* / 4 | interrupted by restart | ≥29 | 239k |
| M1a-4 cleanup 2 | Opus, medium | 4 (6 checks) | blind | approved first time | 0 / 0 / 4 | 14.7 min | 67 | 224k |
| M1b T8 batch A | Opus, default | 5 | blind | approved first time | 0 / 2† / 4 | interrupted by restart | ≥16 | 218k |
| M1b T8 batch B | Sonnet | 11 (9 + 2 added mid-run) | blind | approved first time | 0 / 0 / 7 | 69.0 min | 131 | 342k |

\* a project gap (no sanitizer build in `make check`), not an implementation defect. † both pre-existing (older timing-test flakes), not introduced by the batch.

**Reading.** Quality: no difference is detectable — every arm was approved first time, with no Critical findings and no Important finding attributable to the implementer. Speed and cost: not comparable this round — two batches were interrupted by a session restart, the batches differ in scope (batch B grew to 11 items), and host load ranged 7–150. Per item, Sonnet's batch B used ~31k tokens and ~6.3 min, Opus-medium's batch ~56k tokens and ~3.7 min, on different kinds of work.

**Decision (per the rule above).** No replicated difference, so the default does not change: Sonnet stays the implementer for planned copy/fix-list work; Opus at default effort stays for reference builds and design-heavy tasks; Opus at medium effort is not adopted. Reviews stay on Opus. The split continues on the first M2 fix lists with matched scope and no restarts, which is when time and cost become comparable. Also learned: blind reviews need diff-only packages (commit trailers name the model).

### E2 round 2 (M2a-0, 2026-10-08)

| Task | Arm | Size | Review | Fix rounds | Wall | Tool calls |
|---|---|---|---|---|---|---|
| M2a-0 T1 seed addresses | Opus, medium | 121-line module + 140-line test | blind APPROVE; 1 Important caused by the brief (a test pin that the next task invalidates), 4 Minor test gaps | 1 (tests + docstring) | 4.7 min (+3.3) | 21 (+7) |
| M2a-0 T3 native registry | Sonnet | 917-line generator + 500-line test + generated doc | blind APPROVE; 0 Important, 6 Minor | 1 | 28.6 min (+7.6) | 102 (+22) |

**Reading.** Both arms were approved first time with no Critical findings and no implementer-caused Important finding. The Sonnet arm disclosed a TDD deviation: it drafted the module before the tests. It also added an unrequested third independent reader that made the registry stronger. The tasks differ about 6× in size, so time and cost per item are still not comparable. **Decision:** no change of default. M2a-1 pairs two deliberately matched tasks, one per arm, so time can be compared.

### E2 round 3 (M2a-1, matched corpus halves, 2026-10-08)

| Task | Arm | Size | Review | Fix rounds | Wall | Tool calls | Tokens |
|---|---|---|---|---|---|---|---|
| M2a-1 corpus group a | Sonnet | 262 programs | APPROVE; 0 wrong expectations; 1 Important (a coverage gap) | 1 (2.9 min) | 21.3 min | 34 | 310k |
| M2a-1 corpus group b | Opus, medium | 239 programs | APPROVE; 0 wrong expectations; minors only | 1 (1.8 min) | 15.6 min | 25 | 253k |

**Reading.** On matched work both arms were approved first time, with equal correctness. Opus-medium was 27 % faster with fewer calls. A later, independent check found that both arms shared one misreading with every other party: atof wraps where `_strtold` actually saturates, and the sign of exact-zero results. The cause was the shared tables, not the model.

## E3: Haiku 5.5 vs Sonnet 5.5 on bounded mechanical jobs (2026-10-09, Console credits, headless `claude -p`)

| Rep | Task | Sonnet | Haiku | Verdict |
|---|---|---|---|---|
| 1 | S05 regex + anchors guard test | $0.187, 8 turns, 1.1 min | $0.017, 13 turns, 1.5 min | both correct (controller re-ran tests; regex vs the real failure line) |
| 2 | review minors M3/M6 + expect_calc atof overflow | $0.321, 13 turns, 1.4 min | $0.038, 32 turns, 2.5 min | both correct (controller: native build, osi 1028/1028, pytest); Haiku's merged |

| 3 | M2a-2 T11: loadavg fields on the osi perf row (C++ + cmake assertion) | $0.237, 11 turns, 0.8 min | $0.015, 13 turns, 0.7 min | both correct; controller: native + wasm `osi_a6_rerun` passed for both; Haiku's merged (first correct). Design note: Haiku's -1 'unavailable' sentinel is less robust than Sonnet's 0, but it never triggers on our targets |

**Reading after 2 of 3 reps.** Quality was equal both times. Haiku costs 8.5–11× less and uses 1.5–2.5× the turns and 1.4–1.8× the wall time. Rep 3 is still needed before Haiku is adopted for this class. On the owner's plan (2026-10-09 billing split), the $ saving matters only for credit-billed runs.

## API cost reference (2026-10-09, Console credits; the JSON cost matches the Console log)
- Medium fix, Sonnet: $0.40.
- Opus-medium review: $0.86–1.37.
- I2 (two engines, CMake, measurement), Opus-medium implementer: $2.25.

Details in `.superpowers/sdd/cost-test/results.md` (git-ignored) and ROADMAP §4.1.

**E3 verdict (3/3 reps, 2026-10-09): ADOPT Haiku 5.5 for bounded, checkable mechanical jobs.** It was first-time correct 3 of 3, the same as Sonnet. It costs 8.5–16× less, with similar wall time (0.7–1.8×). The guard holds: the controller verifies every Haiku job with the same commands as a Sonnet one, and anything needing judgement (design, reviews, asm/RE, rulings) stays on Sonnet, Opus or Fable. Watch rep 3's pattern: on robustness details Haiku made the weaker of two valid choices, so its edge-case choices get one extra controller read.
