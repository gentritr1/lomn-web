# Cost test on Console API credits (2026-10-09)
Billing verified: the probe request req_011CfqXxcYkMmRa5vQrwK8AE appears in Console Logs (claude-haiku-5-5, 16,293 in / 4 out), equal to the local JSON usage.

| Run | Task | Model | Turns | Wall | Out tok | Cache read | Cache write | $ | Verdict |
|---|---|---|---|---|---|---|---|---|---|
| A | small: S05 regex + guard test | Sonnet 5.5 | 8 | 1.1 min | 3,600 | 288k | 37k | 0.187 | correct (tests 5/5; regex matches the real failure line) |
| B | same input as A | Haiku 5.5 | 13 | 1.5 min | 10,505 | 564k | 51k | 0.017 | correct (same checks) |

Both flagged that the brief's premise was wrong: the old program exists but has a positive lhs, so it cannot catch S05. Neither faked a red test. The premise came from the final review's M2 wording.
| C | medium: I1 strtold overflow on adjusted exponent (C++, expect_calc, unit tests, doc) | Sonnet 5.5 | 16 | 3.7 min | 11,536 | 779k | 53k | 0.405 | reported green (53/53 unit, 527/527 trace-cmp); pending review D |
| D | review of C: 24,052-string C++ vs oracle differential, end-to-end on both VMs, asm check | Opus 5.5 (medium) | 26 | 3.6 min | 13,611 | 1,429k | 61k | 0.861 | APPROVE; C++ 5/5 and Python 5/5 re-run by controller |

**Total: $1.47** for 4 runs plus the probe ($0.187 + $0.017 + $0.405 + $0.861 + $0.002). The Console reconciliation is still pending: the extension disconnected, and the probe matched the Console exactly.

## What the numbers say
- **Cost formula.** Cost per run ≈ cache reads × read price + cache writes × write price + output × output price. Cache reads dominate the token counts, but at $0.10 (Sonnet), $0.20 (Opus) and $0.01 (Haiku) per MTok they are cheap. Output and cache writes are a large share of the $.
- **Haiku vs Sonnet on one small mechanical task:** the same quality and 11× cheaper. This is E3 rep 1 of 3; it is not yet enough to adopt.
- **Review vs implementation.** A thorough Opus review cost 2.1× the Sonnet implementation it reviewed, and it is the part that found and closed the real risk. Keep it.
- **These tasks were small** (8–26 turns). Large M2a-1 tasks ran 70–100+ turns with 300–500k contexts. Scaling with the same formula gives these per-task costs:
  - large Sonnet implementer: about $3–6;
  - large Opus implementer: about $6–12;
  - Fable whole-branch review: about $12–16;
  - Opus controller session per sub-milestone, started fresh: about $10–25.
- **Risk to watch:** headless runs use the 5-minute cache. A build or test wait longer than 5 minutes inside an agent turn forces a full cache re-write on the next turn, costing about $0.75 (Sonnet) to $1.50 (Opus) at 300k context. Long builds must stay short or run outside the agent.
| m-sonnet | review minors batch (M6 dead code, M3 seam doc + guard test, expect_calc atof overflow) | Sonnet 5.5 | 13 | 1.4 min | 6,782 | 660k | 49k | 0.321 | correct |
| m-haiku | the same batch | Haiku 5.5 | 32 | 2.5 min | 24,089 | 1,642k | 73k | 0.038 | correct, merged |

**E3 after 2 of 3 reps (mechanical fixes):** Haiku matched Sonnet both times, at 8.5–11× lower cost, 1.5–2.5× the turns and 1.4–1.8× the wall time.
