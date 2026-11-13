# Resume kit (updated 2026-11-13: M2a-2 closed)

Everything needed to pick the work up again. The day-to-day controller workspace (`.superpowers/`) is git-ignored, so the parts that matter are copied here with personal paths scrubbed.

## Where we stopped
> **Commit IDs changed on 2026-10-09.** History was rewritten once (privacy) before going public. Everything written before then (this kit, the ledger, ROADMAP) cites the OLD short IDs. Translate them with [commit-id-map.txt](commit-id-map.txt).

- **M2a-2 is closed (2026-11-13).** `m2-osi-vm` @ 80bc109 holds Tasks 1–12. The gate was re-run by the controller on a frozen copy of 80bc109:
  - trace-cmp 541/541; `make osi-a9` with real natives 4/4 OK (601 D lines);
  - coverage 69 natives (tier1 35, tier2 34, stub 0), 0 unregistered; taint 0 sinks;
  - test-tools 992 passed, test-native 45/45, test-wasm 17/17; datacheck 39/39 native and wasm; worklist 0 errors;
  - mutate_natives 6/6 caught, mutate_vm 46/46 caught.
  - test-web passes (59/59 unit tests, smoke, byo_synthetic 123/123) and a13-real passes 60/60, after Playwright was installed with the owner's yes.
- `main` is NOT updated: it is still at ad8789a (the commit-ID map), 13 commits behind `m2-osi-vm`. Fast-forwarding `main` to `m2-osi-vm` and pushing need the owner's yes.
- **Next: M2a-3 (menu systems).** It needs the owner's menu screenshots before its review. What it inherits from M2a-2: [../ROADMAP.md](../ROADMAP.md) §3.3. The full ledger is [m2a2-ledger.md](m2a2-ledger.md).
- **Billing:** the Max plan, from the desktop session (owner, 2026-11-13), including M2a-3. `start.sh` and Console credits are only for overflow.

## Open items to carry
See ROADMAP §3.3 (area_source seam, refused-Add slot, HUD missing files, `addtoa` cheat gate, front-end Toa wall closeness, A9 blind spots). Exe ruling (settled in Task 12): the install exe is authoritative; the reference copy is hand-patched in 7 ranges, pinned by `tools/tests/test_exe_patched_ranges.py`.
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

## Learning pages
Plain-English lessons (ELI5 + ASD-STE100 style). Live versions are private claude.ai artifacts; copies are in [../learn/](../learn/).
1. [LOMN Web Mission](https://claude.ai/artifact/Fwq8bSGV7cngnydcXEyF6w): goal, route, where we are ([local](../learn/lomn-web-mission.html))
2. [LOMN Reverse Engineering 101](https://claude.ai/artifact/55NmU1TZ2vhcwb1qPtxroQ): RE, .exe files, assembly, debug symbols, Ghidra ([local](../learn/lomn-reverse-engineering-101.html))
3. [Inside the LOMN Engine](https://claude.ai/artifact/73T8LoCANhM6ddGojXdsqs): file formats, the OSI script VM, natives, D3D8 to WebGPU ([local](../learn/inside-the-lomn-engine.html))
4. [Proving the LOMN Port](https://claude.ai/artifact/GJRp9GWiDg3QWzouwaJMay): oracles, differential tests, taint, mutation, blind reviews ([local](../learn/proving-the-lomn-port.html))
5. [LOMN Team, Costs and Limits](https://claude.ai/artifact/K3LoXLjjiKJQc9Kv45WK6U): agent roles, measured costs, limits ([local](../learn/lomn-team-costs-and-limits.html))
- [LOMN Project Map](https://claude.ai/artifact/4yApp6C7MGDmPJo1VuBmdp): where every file, repo and source lives ([local](../learn/lomn-project-map.html))

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
