# Security audit addendum: M1a-4 hardening (fuzz corpus, sanitizer gate), 2026-10-08

**Applies to:** branch `m1a4-hardening` (range `af5a77c..343af0d` plus the final fixes). Threat model, caps and method: `2026-10-07-m1a1-parser-security.md` and `2026-10-07-m1a3-osi-textures-parser-security.md` (every game file is untrusted; wasm32 has a 32-bit `size_t`, no exceptions and a 2 GB heap). This file only records what M1a-4 added.

## What was added

- **A11 fuzz corpus** (`engine/tests/fuzz_formats.cpp`, `make fuzz-formats`): one mutation driver over BLK + LZSS, SLB (obj, pos, light, textures), binary `.x`, DDS, TGA and OSI, with per-input invariants (no crash, 2 s CPU limit, 64 MiB heap-growth limit, no partial output after a rejection) and per-format inputs / accepted / deep-reach / findings totals. 200,000 inputs per format with real seeds, natively and as wasm32, 0 findings, per-format totals identical (`docs/knowledge/measurements.md`, A11, including which driver commit the wasm32 200k leg ran on). A 20,000-input synthetic-seed subset runs in ctest (`fuzz_formats_smoke`) on both targets with a deep-reach floor of 5 % per format.
- **`unit_asan`** (ctest, native only, ASan + UBSan, `-fno-sanitize-recover=all`; part of `make test-native` and so `make check`): cleanup S1 created it for the `.x` cases (crafted rows included) and the fuzz-fragment producers; final F1 added `test_sage_blockfile.cpp`, `test_sage_slb.cpp` and `test_sage_image.cpp`, the no-partial-state tests below. 20 -> 53 test cases and 1,271 -> 3,620 assertions, about 1.4 s either way. wasm32 has no sanitizers.
- **Crafted `.x` rows** (cleanup C1-a, C1-b, C2): 13 previously unguarded `decode_mesh` / `MeshVertexColors` / `MeshMaterialList` / `Material` count and length checks are each pinned by a crafted input that asserts the exact message and a default `XFile`. Deleting any one check in a scratch copy fails exactly its rows; the Release build alone could not see several of them (an out-of-bounds read there is undefined behaviour, not a failure).

## Defects the fuzzer found (all fixed, each with a unit test that fails on the old code)

| Commit | Defect | Found by |
|---|---|---|
| `74a5a69` | `decode_lzss` / `BlockFile::read` left a partial decode (or the caller's old bytes) in the output after a failure | synthetic smoke, 8,261 and 1,185 findings of 20,000 |
| `74a5a69` | `Slb::parse` kept its bytes and the fix-up fields read so far after a rejected trailer | synthetic smoke, 1,816-3,522 of 20,000 per SLB view |
| `2856ce9` | `decode_tga` allocated `w*h*bpp` (up to 64 MiB) before checking that the input could hold the pixels | real-seeded `tga` input 49594: a real 8,949-byte RLE TGA with its header inflated to 4096x4096x32 |

These are failure paths only: every caller checks the return value, and the real install never takes them. No cap, limit or timeout constant was changed in M1a-4 (checked by the final review against `af5a77c`). The `.x` size cap (64 MiB, `parse_x_impl`) predates M1a-4 (`f307347`).

## Verification notes

- **Bites:** known defects re-introduced in scratch copies are listed with their results in `docs/knowledge/measurements.md` (A11) and `tools/research/fuzz_formats.md` (known blind spots). The final review removed the two TGA pre-allocation checks: the Release smoke reported `FINDING tga input 14: heap growth 67108928 bytes` and exited non-zero.
- **Final F1 proof:** in a scratch `git archive` copy, deleting the `pos >= size` check in `decode_tga`'s RLE loop (`image.cpp`) leaves Release `lomn_tests` at 180/180 (177,014 assertions): the stream is rejected by a later `in_range` check with the same message. With the three test files in `unit_asan` and the TGA truncation loops copying each prefix into an exact-size vector (a prefix view into a longer vector hides a read past the end from ASan), `unit_asan` reports `heap-buffer-overflow` at `image.cpp:173`. Seven other mutations (LZSS pair-length check, two LZSS ring-index masks, TGA row flip, DDS data length, SLB slot range, SLB slot length) fail or crash in Release already and are also caught by `unit_asan`; the added coverage matters for reads that leave the result unchanged.
- **Hostile inputs, native vs wasm32:** the final review fed 21 crafted DDS / SLB / BLK inputs (oversized dimensions, wrapped 32-bit counts, out-of-range slots and offsets, a 64 MiB declaration from 5 LZSS bytes) to `image.cpp`, `slb.cpp` and `blockfile.cpp` built with ASan + UBSan natively and as wasm32. Verdicts, error strings and post-rejection state were identical on both, with a peak heap growth of 156-164 B (the error string) on every rejected input. That probe is reviewer scratch and is not committed.

## Remaining limits

- wasm32 has no per-input hang watchdog (`--last-input` names the input), and `unit_asan` is native only.
- The fuzzer cannot build structures that need many coordinated field changes or hundreds of consistent records; those are covered by unit tests and probes (blind-spot table in `tools/research/fuzz_formats.md`).
- The A9 oracle is not independent of the shim for three light cases that the real install never exercises (`tools/research/oracle/a9_oracle.py`, header comment).
