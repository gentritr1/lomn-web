# Resume kit (paused 2026-10-09)

Everything needed to pick the work up again. The day-to-day controller workspace (`.superpowers/`) is git-ignored, so the parts that matter are copied here with personal paths scrubbed.

## Where we stopped
- `main` @ the "resume kit" commit = the end of the F7 follow-up batch of **M2a-2** (merged a794f04, roadmap 1ba99cc).
- Verified on that state (re-run by the controller, not just reported):
  - `make conform-trace-cmp`: 541/541 programs pass, traces byte-identical between C++ and the Python oracle;
  - `make osi-a9` with real natives: 4/4 OK. Native == wasm32 == Python's own models == replay over 600 frames of the real menu script;
  - coverage: 69 natives (tier1 35, tier2 34, stub 0), 0 unregistered; taint with real natives: 0 sinks;
  - `make test-tools`: 974 passed.
- M2a-2 needs two more steps:
  1. **Task 10, the A8–A10 gate.** The brief is ready: [task-10-brief.md](task-10-brief.md). Its base is a794f04, and the A9 numbers in its "Controller additions" are current.
  2. **Task 12, close-out.** A blind Opus whole-branch review of `94e8dcc..HEAD`, ONE fix dispatch, a scoped re-review, then ROADMAP §3.2/§3.3. The plan text is in `docs/superpowers/plans/2026-10-09-lomn-web-m2a2.md` ("### Task 10", "### Task 12").
- Then M2a-3 (menu systems) → M2a-4 → M2b (browser boots to the menu). See [../ROADMAP.md](../ROADMAP.md) §3.3–3.5.

## Open items to carry
- **Which exe is authoritative.** The install's `LEGOBionicle.exe` and the reference copy differ from byte 417186 on; every cited address agreed in both. Rule on this in Task 12.
- **Area-loaded characters.** Their Show/Hide sequences are pinned only by each engine's own tests, so they need a 2-engine differential once the hosts load area content (M2a-3).
- **HUD loading failures.** The missing-file paths of `loadhudsprite` / `sctilefont.create` are deferred to M2a-3, when the file manager and error ids land.
- **`addtoa` cheat.** The INI gate (`[Cheat] Urinate`) is modelled as absent.
- **Front-end Toa.** Its CheckWallCloseness may skip 3 HUD hides (ROADMAP §3.3 risk).
- **Owner checks:** menu screenshots, Esc/Backspace/right-click on Options, G1–G4 shots, a quiet Mac for perf baselines, Safari/Firefox.

## How the last runs worked (Console API credits)
- **Setup.** One headless Opus 5.5 controller (`claude -p`) per run, with a $45 cap. It dispatches Opus/Sonnet implementers and blind reviewers as subagents, merges locally, re-runs integration and appends to the ledger. The owner then re-verifies.
- **Start command:** `bash docs/resume/start.sh <label>`, run from `~/Desktop/lomn-web/lomn-web-m2`.
  - It reads `brief-ctl-<label>.md` next to the script, so copy and adapt [controller-brief-template.md](controller-brief-template.md) first.
  - It runs detached (`nohup`) under `env -i`, so it bills the Console account and not a desktop-app session. Inside the Claude desktop app's terminal pane, an interactive `claude` reported "Credit balance too low" even with credits; headless runs worked.
- **Measured costs:**

  | Run | Cost |
  |---|---|
  | Pairs 8+9 | $35.94 |
  | F7 batch | $34.91 (one Opus C++ implementer ≈ $15) |
  | Task 10 + 12 (estimate) | $35–45 |

  Details: [api-cost-results.md](api-cost-results.md).
- **Models:**
  - Opus 5.5 medium: controller, C++ and RE-heavy implementation, blind reviews.
  - Sonnet 5.5: Python side, mechanical fixes.
  - Haiku 5.5: code search (the Explore subagent: same accuracy as Opus on a 6-question test at ~10× lower cost) and exact-list bookkeeping.
  - Fable 5.1: only plan reviews and milestone ends.

## Files here
- [m2a2-ledger.md](m2a2-ledger.md): the full M2a-2 ledger. Every dispatch, verdict, ruling and integration number, in order. The last sections are the resume point.
- [task-10-brief.md](task-10-brief.md): the next implementer brief.
- [controller-brief-template.md](controller-brief-template.md): the last controller brief, a template for the next one (scope, budget rule, hard rules).
- [start.sh](start.sh): the detached headless launcher.
- [api-cost-results.md](api-cost-results.md): the measured per-task API costs.

## Hard rules (unchanged)
- Never run the game's .exe or any DLL; parse them as data only.
- Never commit game data, decompiled code blocks, disassembly listings or assembled .osi.
- No push, merge to main, deploy, licence or public visibility without the owner's explicit yes.
- The 3 README screenshots are renders of the game's assets. Decide on them before making this repo public.
