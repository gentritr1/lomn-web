# LOMN Web: roadmap and resume point

State as of 2026-11-13 (M2a-2 closed). Update this file whenever a milestone closes. The live board mirrors it at https://claude.ai/artifact/GyZGve8HMfmWHzJJhx3PyQ.

## 1. Where we are

| Milestone | What it gives you | State |
|---|---|---|
| M0 Foundations | toolchain, decompiler pipeline, native and browser builds | done |
| M1a-1…4 | read every game file format, render areas natively, faithful look, hardening | done |
| M1b Browser viewer | pick your game folder in Chrome and fly around any of the 123 areas | **done; you can test it now (§2)** |
| M2 Script VM to the main menu | the game's own script (Base.osi) runs, up to its main menu in the browser | **in progress: M2a-0…M2a-2 done; M2a-3 next** |
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
| M2a-2 | native framework; `Setup` runs to the first `Process`; `lomn_view --script` runs N frames headless | **done 2026-11-13** (gate green on 80bc109) |
| M2a-3 | menu systems: tile fonts, HUD sprites and text, camera spline, SDL3 input → `Input`/`OnEvent`, `Bionicle.ini`; the menu renders | **next; needs your menu screenshots** |
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

### 3.2 M2a-2: closed 2026-11-13
- Tasks 1–12 are merged on `m2-osi-vm` @ 80bc109. Task 10 (the A8–A10 gate) and Task 12 (the whole-branch review) ran on 2026-11-13 from the desktop session on the Max plan (owner's choice, for those two tasks only).
- **Task 10.** It added taint iteration, `mutate_natives.py` (6 mutants + control), per-file unit runs in `mutate_vm.py`, the unregistered-native counter, and the wasm stack measured per nesting level (1,984 bytes by trap). Ruling: wasm `STACK_SIZE` is 576 KiB (589,824 bytes), Decision 11 applied to that figure.
- **Task 12.** The blind Opus review was READY WITH FIXES (0 Critical, 2 Important, 8 minor). One fix dispatch had a C++ side and a Python side. A blind re-review found that the two sides had drifted (3 Important), and a third fix reconciled them against the cited bytes. The scoped re-check gave APPROVE. Modelled now in both engines:
  - the block-file slot [0x736128]: the swallowed-error leak in AddCharacter, the lazy freed slot after a failed `addtoa`, LoadHudSprite nulling the slot, and the level/area headers in the slots;
  - RemoveArea with no loader and fluff count 0 raise Undefined;
  - `doesfileexist` on the user route does a VFS lookup.
- **Exe ruling.** The install's `LEGOBionicle.exe` (sha256 b372afb9…) is authoritative. The reference copy was hand-patched after linking: 798 bytes in 7 ranges inside 6 functions. `tools/tests/test_exe_patched_ranges.py` pins those ranges and fails if a citation falls inside them without naming the copy.
- **Measured on frozen 80bc109 (executed):**
  - conformance: trace-cmp 541/541, byte-identical C++ vs Python;
  - `make osi-a9` with real natives: 4/4 OK, 601 D lines; coverage 69 natives (tier1 35, tier2 34, stub 0, tier3 0), 1,705 calls, 0 unregistered; taint 34 tainted, 0 sinks;
  - `osi-diff` 4/4; `lomn_view --script 1 --frames 1` rc 0; ctest unit|osi|view 26/26;
  - `make check` layers: test-tools 992 passed, test-native 45/45, test-wasm 17/17, datacheck 39/39 native and wasm, worklist 0 errors;
  - mutation: `mutate_natives` 6/6 caught, `mutate_vm` 46/46 caught, the controls missed as expected;
  - the A9 D lines are unchanged since M2a-1 (601 lines identical to 0f8be36).
- **Web layers (after the owner approved installing Playwright, 2026-11-13):** `make test-web` passes (59/59 unit tests, Metal smoke run with 0 console errors, byo_synthetic 123/123), and `make a13-real` passes 60/60 on the real install.
- **Incident 2026-11-13.** A subagent deleted the real install's `data/Base.osi`: `cp -Rl` copied the `$LOMN_DATA/install` symlink, and a later `rm` went through it. It was restored from the second LOMN-Beta copy (sha256 checked; the two trees are identical). Rule: build hard-link installs with `mkdir install && cp -Rl "$LOMN_DATA/install/." install/`, and never rm through a symlink.
- **Cost:** Tasks 10 and 12 ran on the Max plan, so there is no exact $ figure. The earlier M2a-2 runs on Console credits cost $35.94 + $34.91 (§4.1).
- Full ledger: `docs/resume/m2a2-ledger.md`.

### 3.3 M2a-3: menu systems
**Inherited from M2a-2 (open):**
- **The area_source seam.** Neither host loads area content yet. When it does:
  - run a 2-engine differential of area-loaded characters (Show/Hide, which are pinned only per engine now);
  - check RemoveArea's `OnEvent(0x2b,'vlgr')` for area-loaded characters (differential case x3 diverges only there);
  - check the hive-SLK lookups that depend on the block-file slot (C++ only today).
- **The refused-Add dangling slot.** A refused Add in AddCharacter (0x47ef9a → ret 0x47efda) frees the new character and leaves the slot dangling. Both engines leave the slot at its pre-call value. Rule on it with area data.
- **Missing files.** The missing-file branches of `loadhudsprite` and `sctilefont.create` (with the file manager and error ids).
- **Cheat gate.** The `addtoa` INI gate `[Cheat] Urinate` is modelled as absent.
- **A9 blind spots.** A9 cannot see per-handle refcounts or native state that is never read (`cs.playing`).
- **Script errors.** On a script error the C++ CLI returns rc 4 and the Python CLI returns 0 (a harness convention).

**Risk carried from M2a-2 (natives.md §6.3):** the front-end Toa runs its physics every frame, which the headless M2a-2 frame omits as a documented gap. A wall-closeness event (`OnEvent(0x28, 2, 'impc')`, CheckWallCloseness 0x587240) costs the Toa 20 code life; **the third one kills it** and puts the camera in mode 7, which cancels the menu fly-in unless the death lands by frame 3 (docs/osi/hud.md §8; re-corrected 2026-11-13 by the M2a-3 Task 2a review: the earlier "skips 26 HUD hides" was a recording artefact, and the event's handler performs those hides itself). M2a-3 Decision 7 settles it against your menu screenshots: the event is ruled out only by a screenshot showing the end of the fly-in **and** Onua standing (owner check O6).

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
  - **Billing (owner direction 2026-10-09, superseding the earlier split):** no Max-plan usage for this project. All work runs on Console credits through a headless controller started with `docs/resume/start.sh` (see resume/README.md). **Changed 2026-11-13:** the owner moved LOMN work back to the Max plan, run from the desktop session (controller plus Agent-tool subagents), including M2a-3, until they say otherwise.
  - Start a fresh session per sub-milestone and resume from this file. Fast mode stays off.
  - Record $ spent per sub-milestone from the Console dashboard.
- **Verification:**
  - run it on a frozen `git archive` copy;
  - the copy needs a `.toolchain` symlink, a real `.venv/` with symlinks and `.ok`, and `.env.local`;
  - delete the copy as soon as its result is in the ledger.
- **No foreground command over 2 minutes.** Long work runs in the background, and every wait matches failure too.

## 7. Where things live
- **Branches.** `main` @ ad8789a (fast-forwarded with the owner's yes on 2026-10-09 and pushed to the private GitHub repo `lomn-web`). `m2-osi-vm` is ahead of it by the M2a-2 close-out (Tasks 10 and 12, plus this doc commit); moving `main` again needs your yes. Work continues on `m2-osi-vm`; its worktree is `lomn-web-m2`. Task and experiment branches stay local.
- **Workspaces:**
  - plans in `docs/superpowers/plans/`, specs in `docs/superpowers/specs/`;
  - research in `docs/knowledge/research/`;
  - the OSI contracts in `docs/osi/`.
- **Ledgers:**
  - per-plan ledgers in `.superpowers/sdd/<plan>/progress.md`, git-ignored;
  - process metrics in `~/.claude/process-metrics/lomn-web.jsonl`.
