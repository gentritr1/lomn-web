# Security audit: game-data parsers (M1a-1), 2026-10-07

**Applies to:** commit `0a2782d` (`engine/src/sage/*`, `engine/src/fs/*`).

**Threat model.** Players bring their own game files, and a page may be handed a corrupted or deliberately crafted install. Every file is untrusted input.
- **Requirement (spec M1 §5):** a malformed or hostile file must produce an error. It must never crash, hang, read out of bounds, or exhaust memory.
- **Targets:** this holds natively (64-bit) **and** in the browser. The browser build is wasm32, so `size_t` is 32-bit, exceptions are disabled (an uncaught error aborts), and the heap is capped at 2 GB.

## Method

1. **Unit tests on malformed input.** Truncation at every prefix, overflowing counts, nesting at depth 64/65 and 100,000 deep, and LZSS overrun/short/ratio cases. They run natively and as wasm32 under Node.
2. **Whole-data run under sanitizers.** `lomn_datacheck` over the real Beta 1.5.0 install with ASan+UBSan gives rc 0 and 0 bytes on stderr. Its JSON is identical to the Release build except for timing.
3. **Mutation smoke.** 10 deliberate defects were injected into bounds, routing and decoding code. The unit tests caught 10/10.
4. **Independent oracles.** Python readers written separately from the C++ agree on every file: 39/39 checks.
5. **Adversarial review with executed probes and fuzzing.**
   - The reviewer was a separate model, told to attack.
   - **Fuzzing:** a deterministic mutation fuzzer under ASan+UBSan with `-fno-sanitize-recover`, seeded with the unit tests' synthetic files. It ran natively and as wasm32. Invariants were checked on every accepted input.
   - **Crafted probes:** a time/memory probe for every loop whose count an attacker controls.

## Fuzzing totals (all rounds)

| Target | Inputs | Sanitizer findings | Invariant failures |
|---|---|---|---|
| Native (blk, lzss, slb, x, dds, tga) | ≈ 120 M | 0 | 0 |
| wasm32 (same parsers) | ≈ 1.7 M | 0 | 0 |

**Memory safety.** No out-of-bounds access, use-after-free or undefined behaviour was found by fuzzing, tests or the sanitizer run over the real data. Every multi-byte read goes through a byte-wise little-endian cursor, so there are no unaligned casts. Every size check uses the `count ≤ remaining / stride` form, which is safe with a 32-bit `size_t`.

## Findings: resource exhaustion (denial of service)

**Why the earlier checks missed them:** every real file is benign, so none of the real-data checks (oracles, the whole-install ASan run, mutations) could see these. They were found only by crafted inputs.

| # | Defect | Crafted input | Before | After |
|---|---|---|---|---|
| 1 | BLK: each 40-byte unterminated name was copied up to the next NUL. That is quadratic in the number of entries. | 12,000 entries of `'A'`, a 672 KB file | wasm32 aborts at 2 GB; natively ≈ 4 GB | Instant error, 3.5 MB |
| 2 | `.x`: every material reference re-decoded the material and scanned all its children (O(references × members)) | 40,000 references, 880 KB | 4.8 s native, so multi-MB files effectively hang | 0.005 s |
| 3 | `.x`: no cap on the object arena | 120 MB of empty objects | wasm32 aborts | Error, 0.007 s |
| 4 | `.x`: a long texture name was copied into every material reference | 35,000 refs × 70 KB name, 460 KB | wasm32 aborts | Instant error |
| 5 | BLK: a failed `open` left partial entries visible | Valid open, then a hostile one | Stale `find()` hits | Empty state |
| 6 | BlockChain: a failed `enter_area` kept the previous area open | Missing level blockfile | Old area still searched | No area is entered |
| 7 | BLK: compressed `stored_size` uncapped | `stored_size` = cap + 1 | 1× file-size allocation | Error |

## Caps and the measurements behind them

Each cap is set from a value measured on the real install, with headroom. A cap that rejected real data would break fidelity, so the measurement came first and the oracles were re-run after every change: still 39/39.

| Cap | Measured maximum (Beta 1.5.0) | Cap | Headroom |
|---|---|---|---|
| BLK unterminated-name run past 40 bytes | 2 bytes (2 such names in the whole install) | 512 bytes | 256× |
| BLK entry `full_size` (and compressed `stored_size`) | 2,511,728 bytes | 64 MiB | ≈ 26× |
| `.x` objects per file | 5,489 (`vllg.blk` `main1.x`) | 131,072 | ≈ 24× |
| `.x` materials per mesh | 3,110 | 65,536 | ≈ 21× |
| `.x` texture-name length | 36 bytes | 576 bytes | 16× |
| `.x` nesting depth | — | 64 | from spec §5 |
| LZSS output ratio | — | 9× (2 input bytes → at most 18 output bytes) | format bound |
| DDS dimensions | — | 1..4096, mips ≤ log2 + 1, no cube or volume | spec §5 |

**Caveat:** only one build was measured (Beta 1.5.0). Other builds of the game could exceed these maxima. The headroom is meant to cover that, and a rejection returns a named error rather than corrupting anything.

## Remaining worst cases (bounded, linear)

| Path | Worst case measured | Notes |
|---|---|---|
| `.x` material path, sub-MB file | ≈ 48 MB wasm32 heap | 65,536 refs × 576 B names |
| `.x` vertex split, 64 MiB entry | ≈ 580 MB wasm32 heap, 1.6 s | ≈ 9× the file size; the largest allowed blockfile entry |
| `.x` many meshes | Bounded by the object cap | Only the last mesh is decoded ("last mesh wins") |

## Open items

- `parse_x` has no input-size cap of its own. Entries read from blockfiles are capped at 64 MiB, but a loose `.x` reached through the disk fallback is not, and at about 9× amplification a loose file of roughly 220 MB or more would hit the wasm32 2 GB limit. Fix: apply the same 64 MiB limit to loose-file reads, or check the size at the top of `parse_x`. This is tracked for the M1 fuzz task (spec A11).
- The wasm32 fuzz runs are thin for `.x` (≈ 30 k inputs per run): Node runs ASan-instrumented wasm about 100× slower than native. A longer wasm32 run belongs in the A11 fuzz task.

## Lessons

These are recorded in the reverse-engineering and porting playbooks.

- Oracles over real data prove fidelity, not robustness. Every review of a parser for an untrusted format needs a sanitizer fuzz on **both** targets, plus a crafted time/memory probe for each attacker-controlled loop.
- Set caps from measured real-data maxima with stated headroom, then re-run the oracles. That way a cap can never silently cost fidelity.
- Test the wasm32 build directly: a 2 GB memory cap and missing exceptions turn native "slow and big" into a browser abort.
