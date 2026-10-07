# Security audit addendum: OSI script reader and textures.slb parser (M1a-3), 2026-10-07

**Applies to:** branch `m1a3-faithful-states` at `0c06473`. Files: `engine/src/sage/osi.cpp`, `engine/src/sage/slb.cpp` (`parse_spec_textures`), and the area loader that consumes both.

**Threat model:** the same as `2026-10-07-m1a1-parser-security.md`. Every game file is untrusted, and wasm32 has a 32-bit `size_t`, no exceptions and a 2 GB heap. These two parsers are new in M1a-3:
- `Base.osi`, the game's script bytecode;
- `textures.slb`, the per-level texture overrides.

## How the defects were found

The first reference build (REF3 r1) applied none of the M1a-1 audit rules to the new OSI reader. Three adversarial reviews in turn each found a class of problem that the previous checks had missed:

1. **Fable reference review.** `find_calls` accumulated calls and arguments with no cap. A 600 KB crafted `Base.osi` reached 473 MB natively; scaled to the instruction cap, it would abort wasm32.
2. **Builder revision 2 measured real maxima and capped every accumulation.** While doing so it also found that `check_all` re-walked shared bytecode once per function: 65,535 functions sharing one subroutine hung for more than 60 s.
3. **Opus task review**, with crafted inputs and fuzzing on both targets:
   - `textures.slb` was uncapped, and each record copied a 255-character name. A 64 MiB table reached 1,273 MB on wasm32.
   - OSI classes and methods were uncapped, and each method copied its symbol name. An 8 MiB file reached 429 MB on wasm32.
   - A correctness bug: 32-bit push literals were decoded unsigned. A `cond ? int32_t : uint32_t` ternary decays to `uint32_t`, so 65 real `SetFog` colour arguments differed from Python by 2³² and folds were wrong (INT_MIN / −1 gave 0.5). The implementer and the controller had both accepted this mismatch as "the same 32-bit pattern".
4. **Opus loader review:** applying `textures.slb` was O(subsets × records). A 62 MB table caused an 11.9 s hang.

All of them are fixed, each with a crafted-input test that fails on the old code (Tasks 8A and 8B).

## Caps (measured on the real Beta 1.5.0 install, then ≥16× headroom unless the format bounds it)

| Quantity | Measured max | Cap | Headroom |
|---|---|---|---|
| `Base.osi` file size | 486,274 B | 8 MiB | 17× |
| Instructions per subroutine | 1,707 | 32,768 | 19× |
| Instructions per file (`check_all`) | 151,522 | 4,194,304 | 27× |
| Calls per file / per method | 4,402 / 78 | 131,072 / 2,048 | 29× / 26× |
| Arguments per call | 9 | 127 | the format's maximum |
| Arguments per method | 128 | 4,096 | 32× |
| String-argument bytes per method | 570 | 16,384 | 28× |
| Evaluation stack depth | 16 | 256 | 16× |
| Classes / methods | 395 / 8,406 | 8,192 / 262,144 | 20.7× / 31× |
| `textures.slb` records | 15 (over 18 real tables) | 1,024 | 68× |
| `textures.slb` name length | 35 | 63 | the exe's own `ScFixedString<0x40>` bound |

How the caps behave:
- Hitting a cap is an error and leaves **no partial state**. A failed `parse` leaves an empty file, `eval_calls` returns an empty list, and a `textures.slb` table is built locally and assigned only on success.
- Method names are stored as symbol indices, not as copies.
- Spec-texture records are indexed by name once, so applying them is O(subsets log records).

## Evidence (executed)

**Crafted probes,** native and wasm32 under Node:
- `osi_probe` covers 9 hostile inputs. Each stops with its named error in ≤ 0.031 s and ≤ 3.4–5 MB of native growth.
- `slb_probe` covers a 64 MiB table. Before the fix it was accepted at +375 MB; now it is rejected at +10.7 MB native and +12.9 MB on wasm32.
- The method flood (1.39 M methods sharing one 255-character symbol) is now rejected; before, it was accepted at +311 MB.

**Fuzzing** (ASan+UBSan, `-fno-sanitize-recover`), 0 sanitizer findings and 0 invariant failures throughout:

| Run | Native inputs | wasm32 inputs |
|---|---|---|
| REF3 r2 | 2.04 M | 303 k |
| T7 `make fuzz-osi` | 2.0 M synthetic + 2 k seeded from the real file | 300 k |
| Opus review, OSI | 34 k | 27.6 k |
| Opus review, `textures.slb` | 7.1 M | 58 k |

**Cross-implementation check:** C++ `eval_calls` against Python `osi.py --compare-all` over all 8,406 real class.method pairs (7,263 calls): 0 mismatches. This runs as the ctest `osi_all_methods` whenever `LOMN_DATA` is set.

## Lessons (added to the playbooks)

- A new parser in a later milestone needs the same audit as the first ones. The M1a-1 rules existed but were not applied until a reviewer applied them.
- Count every accumulation an attacker controls, not only the loops: per-record copies of shared strings, and per-method name copies.
- A numeric mismatch in a cross-check counts as a defect until it has been traced through every downstream use. Compare **all** entries, not one example.
- An "independent" oracle shares every constant it transcribes from the implementation. Cite those constants from the binary separately, and run the oracle over the full real corpus at least once per milestone.
