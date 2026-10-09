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
| M2a-1 | VM core, an independent Python oracle, a 528-program conformance corpus; Base.osi Setup plus 600 frames byte-identical across native, wasm32 and Python; mutation bite and fuzz seed | **done 2026-10-09** (gate green on 5734f02) |
| M2a-2 | native framework; `Setup` runs to the first `Process`; `lomn_view --script` runs N frames headless | **next** |
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

### 3.1 M2a-1: closed 2026-10-09
- The Fable final review was READY WITH FIXES. Every Critical and Important finding is fixed: I1 (726789c) and I2 (9a1b467). The minors M2, M3 and M6 and the atof overflow are fixed too (f7d859d, 0ccf4ee, 0a964d5).
- Each fix was verified by the controller, and I1 and I2 also had an Opus review.
- The gate on 5734f02 is all green.
- Items carried into M2a-2:
  - a native that catches a nested VmError and continues: C++ and the oracle resume at different PCs. This needs an asm ruling and a corpus program with a swallowing native.
  - re-measure the maximum engine-call nesting with real natives (cap 64; measured max 1 with stubs);
  - E3 rep 3 (Haiku vs Sonnet).

### 3.2 M2a-2: native framework, Setup to the first Process (IN PROGRESS: resume here)
**Paused 2026-10-09 and pushed to a private GitHub repo. Start with [resume/README.md](resume/README.md)**: the resume kit holds the ledger, the Task 10 brief, the controller brief template and the launcher.

**State 2026-10-09.**
- Merged on `m2-osi-vm` @ a794f04: Tasks 1–9 and 11, the error-PC table, the fail-closed runners, the docs batches and the F7 follow-up batch (setammo and one saver store, the RemoveArea → OnEvent removal chain, the loader's Show/Hide of area characters, the Toa branch of MainCharacterID, setfog/setnearestmax, `..` above a mount root, the Python VFS no longer following links). Each passed a blind review and controller verification.
- Measured on a794f04: both engines pass 541/541 programs with byte-identical traces. `make osi-a9` with real natives gives 4/4 OK: native == wasm32 == Python's own models == replay, over 600 frames. Coverage: 69 natives (tier1 35, tier2 34, stub 0, tier3 0), unregistered 0. Taint with real natives: 0 sinks. `make test-tools`: 974 passed.
- Remaining:
  - Task 10, the A8–A10 gate (brief ready: `task-10-brief.md` in the ledger directory): taint iterated, mutate_natives + the mutate_vm full re-run (incl. the S07 unit gap), wasm stack per level, swallow_count probe, event_probe with real natives;
  - Task 12, the close-out (Opus whole-branch review of 94e8dcc..HEAD).
- Open for M2a-3: the Show/Hide reference sequences of area-loaded characters are pinned only by each engine's own tests, because neither host loads area content yet. Run a 2-engine differential once the area source exists.
- **Billing (owner, 2026-10-09): the Max plan's weekly usage is at 80%, so NO more plan usage for this project.** Run everything from a terminal `claude` session logged into the Console account (credits), in a fresh session.
- **Credits:** about $122 left after 2026-10-09: pairs 8+9 cost $35.94, F7 cost $34.91, and an Explore A/B test about $1. Task 10 plus Task 12 are estimated at $35–45.
- Ledger with exact resume steps: `docs/resume/m2a2-ledger.md` (a scrubbed copy of the git-ignored `.superpowers/sdd/2026-10-09-lomn-web-m2a2/progress.md`; last section).
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
**Risk carried from M2a-2 (natives.md §6.3):** the front-end Toa runs its physics every frame, which the headless M2a-2 frame omits as a documented gap. A non-lethal wall-closeness event (`OnEvent(0x28, 2, 'impc')`, CheckWallCloseness 0x587240), if it fires repeatedly, skips 3 HUD `hide` calls at frame 4, so HUD visibility could differ from the original. M2a-3 must model CheckWallCloseness for the front-end Toa, or settle it against the owner's menu screenshots.

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
- merge to main (the 2026-10-09 fast-forward and private push were approved);
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
  - **Billing (owner direction 2026-10-09, superseding the earlier split):** no Max-plan usage for this project. All work runs on Console credits through a headless controller started with `docs/resume/start.sh` (see resume/README.md).
  - Start a fresh session per sub-milestone and resume from this file. Fast mode stays off.
  - Record $ spent per sub-milestone from the Console dashboard.
- **Verification:**
  - run it on a frozen `git archive` copy;
  - the copy needs a `.toolchain` symlink, a real `.venv/` with symlinks and `.ok`, and `.env.local`;
  - delete the copy as soon as its result is in the ledger.
- **No foreground command over 2 minutes.** Long work runs in the background, and every wait matches failure too.

## 7. Where things live
- **Branches.** `main` = `m2-osi-vm` (fast-forwarded with the owner's yes on 2026-10-09 and pushed to the private GitHub repo `lomn-web`). Work continues on `m2-osi-vm`; its worktree is `lomn-web-m2`. Task and experiment branches stay local.
- **Workspaces:**
  - plans in `docs/superpowers/plans/`, specs in `docs/superpowers/specs/`;
  - research in `docs/knowledge/research/`;
  - the OSI contracts in `docs/osi/`.
- **Ledgers:**
  - per-plan ledgers in `.superpowers/sdd/<plan>/progress.md`, git-ignored;
  - process metrics in `~/.claude/process-metrics/lomn-web.jsonl`.
