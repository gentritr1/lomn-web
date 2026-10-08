# LOMN Web: roadmap and resume point

State as of 2026-10-08, late evening. Update this file whenever a milestone closes. The live board mirrors it at https://claude.ai/artifact/GyZGve8HMfmWHzJJhx3PyQ.

## 1. Where we are

| Milestone | What it gives you | State |
|---|---|---|
| M0 Foundations | toolchain, decompiler pipeline, native and browser builds | done |
| M1a-1…4 | read every game file format, render areas natively, faithful look, hardening | done |
| M1b Browser viewer | pick your game folder in Chrome and fly around any of the 123 areas | **done; you can test it now (§2)** |
| M2 Script VM to the main menu | the game's own script (Base.osi) runs, up to its main menu in the browser | **in progress, about 34 %** |
| M3 One playable area | character, collision, camera, animation | later |
| M4 Audio, video, saves | sound, cutscenes, browser saves with export | later |
| M5 The full game | every area, progression, bosses, AI | later |
| M6 Public release | browser matrix, performance, public URL (needs your yes) | later |
| M7 Extras (optional) | REBUILT content, gamepad, touch | later |

Inside M2:

| Sub-milestone | Acceptance | State |
|---|---|---|
| M2a-0 | completed decompiled code plus citation tables for 96 opcodes and 1,056 natives | done |
| M2a-1 | VM core, an independent Python oracle, a 527-program conformance corpus; Base.osi Setup plus 600 frames byte-identical across native, wasm32 and Python; mutation bite and fuzz seed | done; **final review running** (§3) |
| M2a-2 | native framework; `Setup` runs to the first `Process`; `lomn_view --script` runs N frames headless | next |
| M2a-3 | menu systems: tile fonts, HUD sprites and text, camera spline, SDL3 input → `Input`/`OnEvent`, `Bionicle.ini`; the menu renders | after M2a-2; **needs your menu screenshots** |
| M2a-4 | hardening: VM fuzz under ASan/UBSan, native and wasm32; caps measured; perf entries | after M2a-3 |
| M2b | the browser page boots to the main menu; keys drive the menu | ends M2 |

**First "start the game" moment.** At the end of M2 the browser opens the real main menu, and you can move through it with the keyboard. Nothing is playable before M3.

## 2. What you can test right now: the area viewer (M1b)

1. In a terminal at the repo root (this worktree), run:
   ```bash
   cd web && node -e "import('./tests/serve.mjs').then(async m=>{const s=await m.serve('../build/site');console.log(s.url)})"
   ```
   It prints a `http://127.0.0.1:<port>/` URL. Open it in Chrome. If `build/site` is missing, run `make site` first.
2. Click **Choose game folder…** and pick your LOMN Beta 1.5.0 folder (the one containing `LEGOBionicle.exe` and `data`). Nothing is uploaded.
3. Check and report back:
   - the folder picker works, and the permission prompt appears when you revisit;
   - mouse look and flying feel smooth;
   - areas look right;
   - Activity Monitor GPU memory plateaus over about 30 area switches;
   - Safari and Firefox (only Chromium was tested);
   - the 4 comparison shots G1–G4 against the real game: ghut net, vllg pink fog, vertex-colour clamp, field of view.

Controller-verified 2026-10-08: the page loads to the folder picker with 0 console errors. The real-data browser suite passed 59/59 at M1b. Your eyes on a real folder are still outstanding.

## 3. Resume steps, in order

### 3.1 Close M2a-1
Workspace: `.superpowers/sdd/2026-10-08-lomn-web-m2a1/`. It is git-ignored; the ledger is `progress.md`.
1. **Fable final review: done 2026-10-08, READY WITH FIXES** (0 Critical, 2 Important, 7 Minor; full report in `final-review-report.md`).
   - **I1:** atof's overflow test. The C++ VM and `expect_calc` decide by the raw decimal exponent, but the binary uses the exponent adjusted after digit normalisation (0x65c5c1–0x65c624). Example: `"10e32767"` wrongly switches fpu_pc to 64. Fix: port the oracle's `_strtold_overflow` model into `engine/src/osi/convert.cpp` and `tools/osi/expect_calc.py`, reword `docs/osi/opcodes.md`, and add unit and trace-cmp cases.
   - **I2:** deep re-entrant engine calls crash wasm32 ("memory access out of bounds") before the 1000-slot cap fires; native is correct. Fix: an explicit engine-call nesting cap in both engines, `-sSTACK_SIZE`/`STACK_OVERFLOW_CHECK` on the wasm targets, and a corpus program. This must land before M2a-2 adds re-entrant natives.
   - **Minors:** the oracle's RecursionError on deep re-entry; the stale S05 regex below, plus an anchors test that FAIL regexes name existing programs; type-only expectation compare; dead `Heap::duplicate_in_place`; divergences on undefined inputs. The report lists them all.

2. **One fix dispatch** covering every Critical and Important finding. Also add the ledgered ruling: widen S05's `conform` EXPECT regex in `tools/research/oracle/mutate_vm.py:196` to the new programs `a/0x70-float-negative-lhs-cancels-to-positive-zero` and `a/0x71-float-negative-lhs-minus-itself-positive-zero`. Then:
   - a scoped re-review;
   - a frozen-copy check: anchors, `make conform-trace-cmp`, `mutate_vm --only S05`, the A6 differential. Delete the copy once its result is ledgered.
3. **Close-out:**
   - ledger, metrics (`~/.claude/process-metrics/lomn-web.jsonl`), board, memory;
   - E2 round 3 notes in `docs/knowledge/process-experiments.md`;
   - numbers in `docs/knowledge/measurements.md`;
   - this file.
4. Remove the finished task worktrees: `lomn-web-m2-{vm,oracle,ca,cb,t6,t7}`.

### 3.2 M2a-2: native framework, Setup to the first Process
Process: the controller (Opus 5.5) writes the spec and plan, Fable 5.1 reviews them once for precision, then subagent-driven development with Sonnet 5.5 / Opus 5.5 medium.
- Generated typed stubs for every native, with real returns for the 3 copy-kind natives.
- The natives `Setup` needs, implemented against the decomp with a citation per native.
- `lomn_view --script`: Setup plus N Process frames, headless, native and wasm32.
- A tier list of menu natives from the A8/A10 run trace (A3: the 728 `class.func` pairs Base.osi calls).
- Parked items that land here:
  - the static reachability walk and the 0x19 residual;
  - the 4th FindMemberID caller 0x616519 (GetVariantFromBuffer) must use the VM's miss cache;
  - a `next_[]` handle-wrap guard;
  - a unit test for the empty-symbol-table NULL case;
  - the bodies of the Beta 1.5.0 mod natives (read as data, never run);
  - the atof rounding-chain blind spot (A6/A9 risk);
  - a `-fwasm-exceptions` scope revisit;
  - a loadavg field in perf rows.

### 3.3 M2a-3: menu systems
Tile fonts (binary `.fnt`), HUD sprites, text and rectangles through the D3D8 shim (FVF 0x42, textured quads, HUD transform, drawn after the area), camera spline, SDL3 input, and `Bionicle.ini` via the VFS.
- Acceptance: a pixel comparison against **your screenshots of the real menu**. This review cannot close without them.
- It carries most of M2's size variance.

### 3.4 M2a-4 and M2b
- M2a-4: VM fuzz of bytecode and tables under ASan/UBSan, native and wasm32; caps measured on Base.osi; perf entries.
- M2b:
  - the BYO page boots to the menu;
  - `window.__lomn.script` exposes the script state;
  - Playwright drives the menu with keys;
  - you test it in your browser.

### 3.5 M3 onward
Each milestone gets a spec and a plan (Opus 5.5), one Fable 5.1 precision review of them before implementation, and a Fable final review at its end; none is planned yet.
- M3, one playable area: needs traces from the original game as its oracle. Expect to be asked for recordings.
- M4: audio, cutscenes, saves.
- M5: the full game.
- M6, release: needs your yes on everything in §5.

## 4. Size and pace (estimates, low confidence)
- **Spent on M2 so far, measured:** M2a-0 took 5.4 h and M2a-1 took 4.4 h of logged agent time (the sum of agent wall-minutes; controller time not included).
- **M2 remaining, estimated:** the spec's total was 33–45 agent-hours, which leaves roughly 23–35 for M2a-2 → M2b. M2a-3 is the widest range. In sessions like today's, that is a few days of work, plus your screenshot round-trip.
- **Whole game, estimated:** the board's sizing puts M2 → M6 at 21–39 milestone-sized units. An earlier Fable review put the gameplay work at 12–24 person-months in human terms. The playable-area milestone (M3) has no oracle as strong as the renderer's, so expect it to be slower per unit.

### 4.1 Measured API cost (2026-10-09, Console credits)
Four real tasks were run headless with `claude -p --output-format json`. The first request matched the Console log exactly. Details are in `.superpowers/sdd/cost-test/results.md`.

| Run | Task | Model | Turns | Cost |
|---|---|---|---|---|
| A | small mechanical fix + guard test | Sonnet 5.5 | 8 | $0.19 |
| B | the same task | Haiku 5.5 | 13 | $0.02 (same quality) |
| C | medium bug fix (I1) with tests and doc | Sonnet 5.5 | 16 | $0.40 |
| D | thorough review of C: 24k-input differential, asm check | Opus 5.5 medium | 26 | $0.86 (APPROVE) |

**Budget estimate** (from the measured formula, scaled to the larger tasks of earlier steps; medium-low confidence):
- One sub-milestone costs about $45–90: a Fable plan review, 5–8 Sonnet tasks, Opus reviews, Haiku bookkeeping, and an Opus controller in a fresh session.
- **$200 covers about 2–4 sub-milestones,** roughly closing M2a-1 plus M2a-2 and M2a-3.
- Reaching the first playable area (all of M2 plus M3) is about 8–11 sub-milestones, so about **$400–900**.
- Re-estimate after the first full sub-milestone on credits, from the Console spend.

## 5. Needed from you
**Checks you can do any time:**
- the §2 viewer checks;
- screenshots of the real main menu after the intro: 640×480 and your usual resolution, cursor on entries 1 and 2, plus the Options screen (before the M2a-3 review);
- whether Esc, Backspace and right-click work on the Options screen;
- G1–G4 comparison shots plus the original game's file-access log;
- about 15 quiet minutes on the Mac (load under 6) for the performance baselines and experiment E1;
- `claude plugin update agent-playbooks@agent-playbooks-local`.

**Actions that wait for your explicit yes** (never done without it):
- merge to main;
- any push (a separate yes each time);
- deploy;
- licence and README;
- making the repo public;
- a git history rewrite (an early commit contains a username path);
- publishing the research notes;
- pushing the PORT-001 playbook correction.

## 6. Standing rules for whoever resumes
- **Never run** the game's .exe or any DLL; parse them as data only.
- **Never commit:** game data, renders, decompiled code blocks, or assembled .osi files. `gen/` stays out of git.
- **Scrub personal paths** before any commit.
- **Roles:** Claude subagents implement, never Codex. The controller specs, reviews and owns git.
- **Models (owner direction 2026-10-08, on API credits):**
  - **Fable 5.1:** only for one precise review of each sub-milestone's spec and plan before implementation, and for the final review at the end of each milestone (M2, M3…).
  - **Opus 5.5 at medium effort:** the controller, design-heavy implementation, risky-code reviews, and sub-milestone final reviews.
  - **Sonnet 5.5:** well-specified implementation, corpus authoring, mechanical fixes, and scoped re-reviews.
  - **Haiku 5.5 (`claude-haiku-5-5`): proposed, to be confirmed by experiment E3.** Use it only for bounded jobs that stay under 100k tokens (its price rises 5× above that) and whose output a script or the controller checks:
    - code searches and lookups that return a short summary;
    - triage of long build and test logs (counts plus the first error with file:line);
    - ledger, metrics, board and README bookkeeping from given facts;
    - mechanical edits from an exact list;
    - hygiene scans: personal paths, game data in git.

    Never use it for reviews, design, asm or RE readings, rulings, or code whose mistakes tests could miss.
  - **E3 (run in the first sub-milestone on credits):** Haiku 5.5 vs Sonnet 5.5 on the same inputs for log triage, bookkeeping and mechanical edits, interleaved, 3 runs each. Adopt Haiku for a class only if its first-time-correct rate matches Sonnet's.
  - **Billing split (owner direction 2026-10-09):**
    - **Max 20x plan,** via subagents from the desktop session: Fable 5.1 reviews, the controller, and by default implementation and Opus reviews.
    - **$200 Console credits,** via headless `claude -p` with `.superpowers/sdd/cost-test/run.sh`: overflow when plan limits are hit, parallel bursts, and runs where exact $ per task is wanted.
  - Start a fresh session per sub-milestone and resume from this file. Fast mode stays off.
  - Record $ spent per sub-milestone from the Console dashboard.
- **Verification:**
  - run it on a frozen `git archive` copy;
  - the copy needs a `.toolchain` symlink, a real `.venv/` with symlinks and `.ok`, and `.env.local`;
  - delete the copy as soon as its result is in the ledger.
- **No foreground command over 2 minutes.** Long work runs in the background, and every wait matches failure too.

## 7. Where things live
- **Branches.** `m2-osi-vm` is the current work; its worktree is `lomn-web-m2`. It contains `m1-integration`, the merged M1. `main` is untouched until you say so.
- **Workspaces:**
  - plans in `docs/superpowers/plans/`, specs in `docs/superpowers/specs/`;
  - research in `docs/knowledge/research/`;
  - the OSI contracts in `docs/osi/`.
- **Ledgers:**
  - per-plan ledgers in `.superpowers/sdd/<plan>/progress.md`, git-ignored;
  - process metrics in `~/.claude/process-metrics/lomn-web.jsonl`.
