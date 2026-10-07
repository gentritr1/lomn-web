# fuzz_formats: the A11 fuzz corpus (M1a-4)

`engine/tests/fuzz_formats.cpp` (target `lomn_fuzz_formats`) is one deterministic mutation fuzzer over every file format
the engine parses from a player's (untrusted) install. Spec M1 §4 A11 and §5; earlier defects and caps:
`docs/knowledge/audits/*.md`.

## How to run

| What | Command | Where the output goes |
|---|---|---|
| Fast subset (part of `make test-native` / `make test-wasm`) | `ctest --test-dir build/native -R fuzz_formats_smoke` (and `build/wasm`) | ctest log |
| Full run, ASan+UBSan, native **and** wasm32 under Node | `make fuzz-formats FUZZ_N=200000` (opt-in; not part of `make check`) | `build/asan/{native,wasm}/fuzz_formats.txt` |
| Print this page | `make fuzz-formats-help` | stdout |

- **The smoke test** uses synthetic seeds only and runs 20,000 inputs per format. It fails on any finding, or when deep
  reach falls below 5 % of the inputs of any format. Its ctest timeout is 20 s; it takes well under a second natively.
- **`make fuzz-formats`** configures `build/asan/native` and `build/asan/wasm` as Debug builds with
  `-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all` and `-DLOMN_WEBGPU=OFF`. It
  builds only `lomn_fuzz_formats`. The wasm32 binary has a 2 GB memory cap and runs under Node with NODERAWFS.
- **Variables:**
  - `FUZZ_N`: inputs per format natively (default 20,000).
  - `FUZZ_N_WASM`: inputs per format as wasm32 (default `FUZZ_N`).
  - `FUZZ_SEED`: the RNG seed (default 1).
  - `FUZZ_TIMEOUT`: per-run timeout in seconds (default 10,800).
- **Real seeds:** with `LOMN_DATA` set (make reads `.env.local`), real files are sampled from `$LOMN_DATA/install`, read
  only. Without it, the run is synthetic only.
- **Native == wasm32:** when `FUZZ_N == FUZZ_N_WASM`, the target `cmp`s the deterministic `fuzz_formats:` lines of the
  two logs. Every input is a pure function of (seed, format, input index, seed pool), so the per-format totals must be
  byte-identical between the targets.
- The run is long and the host may be shared: start it detached, with a completion check that also matches failure
  (`grep -qE "PASS|FAIL|ERROR|Error"` on the log, or the make exit status).

## Driver options

`lomn_fuzz_formats [--n N] [--seed S] [--data INSTALL] [--formats a,b] [--real-per-format K] [--crash-dir D] [--min-deep F] [--smoke] [--replay FMT ITER --dump FILE]`

- `--formats blk,tga` runs only the named formats. Each format's input stream is independent of the others, so input
  `i` of a format is the same whether or not other formats run.
- `--real-per-format K` sets how many real files are sampled per format (default 16; 8 for the LZSS sources). Only
  files of at most 256 KiB are sampled.
- `--crash-dir D` receives up to 8 finding inputs per format, as `D/<format>-<input>.bin`. Inputs derived from real
  files are game data, so `D` must stay under `build/`.
- **Replay** regenerates one input and runs it alone:
  `lomn_fuzz_formats --replay blk 227 --seed 1 [--data "$LOMN_DATA/install"] --dump build/asan/blk-227.bin`.
  Pass the same `--seed`, `--data` and `--real-per-format` as the run that reported it.
- **Exit codes:**
  - 0: no finding;
  - 1: any finding, or deep reach below `--min-deep` (default 0.05);
  - 2: usage error;
  - 3: `--data` is unreadable or gives no real seed for a format;
  - 4: the native watchdog fired (10 s of CPU time on one input or seed);
  - otherwise: a sanitizer abort. ASan/UBSan print their report, and the driver's death callback prints
    `FINDING <format> input <i>: sanitizer report above (replay: ...)`.

## Formats, seeds and deep reach

| Format | Entry point(s) | Synthetic seeds (from the unit tests) | Real seeds | Deep reach = the body loop ran |
|---|---|---|---|---|
| `blk` | `BlockFile::open_bytes`, `is_sorted`, `find` (6 names), `read` (4 entries) | 3-entry stored, stored + LZSS, unterminated 40-byte name, 16-entry sorted table | whole `.blk` files ≤ 256 KiB | the entry-table loop: accepted with ≥ 1 entry, or failed on the name-run cap |
| `lzss` | `decode_lzss` on `[u32 declared size][stream]` | the hand-built streams + an encoded 600-byte text | the Beta install has **no** compressed entry: the first ≤ 768 bytes of sampled real entries, encoded with the test's independent encoder | past the ratio check into the decode loop, with a non-empty stream |
| `slb.obj` / `slb.pos` / `slb.light` / `slb.tex` | `Slb::parse`, then all four typed views on every input | `make_obj`, the POS/LIGHT tables, `make_textures` | `<area>_obj/_pos/_light.slb` and `textures.slb` entries | the format's own view accepted ≥ 1 record (textures: also a failure inside its record loop) |
| `x` | `parse_x` | the unit tests' `sample()`, two meshes, material references, a mesh without a material list | `.x` entries | mesh decoding entered (`decode_mesh`): accepted with a mesh, or failed with one of `decode_mesh`'s own errors. The old "past the header into the token stream" count is printed apart as `token_stream=` (fix round 1: it read 90-95 %, so its 5 % floor could not fail) |
| `dds` | `decode_dds` | DXT1 5x3, DXT3, DXT5, A8R8G8B8 | `.dds` entries | accepted (every rejection happens before the block loop) |
| `tga` | `decode_tga` | 24/32 bpp uncompressed, RLE 32 bpp, RLE 24 bpp right-to-left, a 4096x4096 RLE header over 10 bytes (the M1a-4 finding) | loose `.tga` files and `.tga` entries | accepted, or failed inside the RLE packet loop (not the pre-allocation "too short for the image" check) |
| `osi` | `OsiFile::parse`, `check_all`, up to 16 sampled `eval_calls` | `statt_osi()`, the `fuzz_osi` cap-edge program | `Base.osi` (1 in 16 inputs) | accepted: every table parsed, every offset/symbol checked, and `check_all` run |

- **Sampling:** real seeds are sampled per format with a deterministic RNG (`--seed`) from the sorted file list.
- **Mixing:** half of the inputs start from a real seed (OSI: 1 in 16, because `Base.osi` is 486 KB and costs about 0.2 s per input as ASan wasm32) and half from a
  synthetic one.
- **Deep reach** comes from the parser's result and error string, never from the input. The driver lists the header
  errors (or the body-loop errors) per format in `deep_*`/`run_*`.

## Mutations (1 to 4 per input; 1 to 3 on real seeds)

The weights below are out of 16.
- **Truncation** at a random prefix (1).
- **Bit flip** (4).
- **Length-field inflation** (3). A u32 or u16 field is set to one of:
  - the input size ± k, or the remaining bytes ± k (k ≤ 8);
  - 0;
  - 0xffffffff (0xffff for u16);
  - 0x80000000 (0x8000);
  - 2^k.

  Three times out of four the field is one of the seed's known count/size/offset fields. These are found by walking the
  seed: BLK count and entry sizes, SLB header counts and the fix-up trailer, `.x` list and name counts, the OSI table
  counts and function offsets, DDS height/width/depth/mips, TGA colour-map length/width/height. Otherwise it is a random
  aligned offset.
- **Chunk duplication** (2): 1 to 64 bytes, 1 to 16 copies.
- **Chunk deletion** (2): 1 to 64 bytes, or up to a quarter of the input.
- **Interesting byte** (2).
- **Run of one non-NUL byte** (1): up to 1,024 bytes. This reaches unterminated names and strings.
- **Byte insert** (1).

## Invariants (checked on every input; a violation is a finding)

- **No sanitizer report.** The ASan+UBSan build aborts on the first one.
- **Inputs are exactly sized.** Pointer-based parsers get an exactly-sized heap copy, so a one-byte over-read is caught.
- **Accepted parses satisfy the format invariants:**
  - BLK:
    - entries == header count, and the table fits in the file;
    - names ≤ 40 bytes with no NUL, and `name_terminated` is consistent;
    - `lookup_name` starts with the name, equals it when terminated, and is ≤ 40 + `kMaxNameRun`;
    - `find` stays in range and hits an equal name; on a sorted table, `find(name)` of a plain terminated name returns
      its own index;
    - `read` returns `full_size` bytes, stored bytes equal the file, and offset + size lie inside the file.
  - LZSS: output == declared size ≤ 9 × the stream.
  - SLB:
    - fix-up fields are strictly ascending, 4-aligned, inside the data area, and their targets lie inside the buffer;
    - each view's records == its header count and lie before `data_end`;
    - textures: ≤ `kMaxSpecTextures` records, names ≤ `kMaxSpecTextureName` with no NUL, and every record name is a
      fix-up slot.
  - `.x`:
    - positions/normals == 3·v, uvs ∈ {0, 2·v}, colours == 4·v iff `has_colors`;
    - `source_vertex` < `file_vertices`, and the first `file_vertices` outputs are the identity;
    - indices == 3·t and every index < v;
    - triangles sorted by attribute;
    - subsets contiguous, cover every triangle, with ascending attribute ids, each with a material (or id 0 without a
      material list), and their vertex ranges hold their indices;
    - ≤ `kXMaxMaterials`, and texture names ≤ `kXMaxTextureName`.
  - DDS/TGA:
    - `rgba == w·h·4`, with dimensions 1..4096 that match the info;
    - DDS: mips ≤ log2 + 1, and the `alpha_on` rule;
    - TGA: type 2/10, 24/32 bpp, and alpha 255 at 24 bpp.
  - OSI:
    - major 4;
    - method offsets inside the file;
    - class and method caps;
    - `check_all` within its caps;
    - every `eval_calls` within the call, argc, argument and string-byte caps.
- **A rejected parse leaves its output default and has a non-empty error string:**
  - an empty `BlockFile`, and an empty `read`/`decode_lzss` output;
  - a default `Slb` and empty view vectors;
  - a default `XFile`;
  - a default `Image` with untouched `DdsInfo`/`TgaInfo`;
  - an empty `OsiFile` and an empty `eval_calls` list.
- **Per-input CPU time < 2 s.** This is the brief's "wall time < 2 s", measured as CPU time; it is a deviation, see
  below.
  - Natively it is the thread's CPU time (`CLOCK_THREAD_CPUTIME_ID`). Under Node it is `process.cpuUsage()`
    (Node 20 has no per-thread counter).
  - An input that reaches 2 s is re-run up to twice, with the same bytes, the same RNG and findings muted; the minimum
    counts. A one-off spike, such as V8 background compilation counted in Node's process CPU, is therefore not a
    finding.
  - Wall time is still measured and printed (`max_wall_ms`).
  - **Why CPU time:** on a shared host, wall time measures the other load.
    - One real-seeded OSI input took 0.3-0.6 s of wall time as ASan wasm32 at loadavg about 7, and 2.04-2.34 s at
      loadavg about 27.
    - At loadavg 35-73 on 8 cores, even native OSI inputs reached 1.94 s of wall time.
    - Replays of identical work varied from 14 s to 52 s.
  - **Hangs are still caught:** a hang or a quadratic path burns CPU.
    - A scratch `decode_dds` that spins for 2.1 s of CPU is reported as
      `CPU time 2.100140 s >= 2 s on 3 runs (minimum shown)`.
  - **Native watchdog:** 10 s of user + system CPU (`ITIMER_PROF`, `SIGPROF`). It also covers the seed warm-up and
    `--replay`.
    - A scratch infinite loop exits 4 with `FINDING dds input 22: no result after 10 s of CPU time (hang)`.
    - A hang in a seed is reported as `seed #0`.
  - wasm32 has no watchdog: a synchronous wasm loop cannot be interrupted. A hang there ends at the make target's
    `timeout`. The wasm32 leg runs with `--last-input build/asan/wasm/last_input.txt` (the format and index of the
    input about to run, overwritten before every input), and the target prints it when the leg fails, so the hanging
    input is named. Replay it with `--replay`.
  - `retimed=` in the resources lines counts the re-run inputs.
- **Per-input memory growth:**
  - peak live heap bytes (every `operator new` is counted) and process-memory growth are each ≤ 64 MiB natively
    (`ru_maxrss`) and ≤ 256 MiB as wasm32 (linear memory);
  - every byte an input allocated is released afterwards. A warm-up pass over the seeds runs first, so function-local
    statics are not counted.

## Known blind spots (and what covers them instead)

Reviewed in fix round 1. The review re-introduced 10 single-check defects ("bites") and found these gaps.

| Blind spot | Example (bite) | Why the fuzzer misses it | Covered instead by |
|---|---|---|---|
| Bounds that need **two coordinated field changes** | `.x` `MeshNormals floats short` (`xfile.cpp`), removed: missed at 2 x 20,000 inputs, synthetic and real | A count must be raised past its list **and** an index must use the extra range in the same input | Unit tests in `test_sage_xfile.cpp`: `MeshNormals floats short`, `MeshTextureCoords floats short`, `MeshVertexColors truncated` (both halves). In a scratch copy each removed check fails its own test |
| **Count caps** that need hundreds or thousands of consistent structures | OSI `kMaxOsiCallsPerMethod` and `kMaxOsiStackDepth`, removed: 0 findings; `kMaxSpecTextures`, removed: 0 in 200,000 | Mutations of seeds ≤ 256 KiB cannot build 2,049 calls, a 257-deep literal stack, or 1,025 records that each have a fix-up slot | `test_sage_osi.cpp` (calls-per-method and stack-depth cases), `osi_probe` (crafted calls and stack inputs, native and wasm32), `slb_probe`, `make fuzz-osi` |
| Large structures behind other caps | `.x` object arena, material-reference amplification, BLK name runs over a 672 KB table | Same as above | The `test_sage_xfile.cpp` and `test_sage_blockfile.cpp` cap tests |
| **Memory-safety bites in the ctest smoke** | TGA RLE bound removed: Release smoke reports 0 findings, rc 0 | The smoke subset runs in the Release build (`make test-native` / `make test-wasm`). An out-of-bounds read or write there is undefined behaviour, not a report | Only `make fuzz-formats` (ASan + UBSan, native and wasm32) sees out-of-bounds bites. The Release smoke still catches invariant and heap bites (name-run cap, the TGA pre-allocation check) |
| wasm32 hangs | — | No per-input watchdog on wasm32 | The native leg's CPU watchdog runs first on the same inputs; `--last-input` names the wasm32 input |

## Reading the memory numbers

- **`max_heap_growth`** is the per-input peak of live `operator new` bytes. It is exact and deterministic, and it is
  the number to trust.
- **`max_mem_growth`** is the process high-water mark:
  - natively, `ru_maxrss` grows only when an input beats the previous peak, so a later large input can show 0;
  - as wasm32, the linear memory grows in steps of up to about 96 MiB (Emscripten's geometric-growth cap), whenever
    the allocator runs out. ASan's quarantine makes that frequent.

  It is a backstop against allocations the heap counter cannot see. It is not a per-input measure of the parser.

## ASan quarantine on wasm32 (32 MiB)

The wasm32 ASan build sets `quarantine_size_mb=32` (`__asan_default_options` in the driver; override it with
`-DLOMN_FUZZ_QUARANTINE_MB=<n>`). Native keeps ASan's default.

**Why (measured 2026-10-07):** with the default quarantine (256 MiB), the wasm32 linear memory grew at every format
change and never shrank. The ASan allocator keeps per-size-class regions, and the geometric growth step is about
97 MiB.

| After 200,000 inputs of | blk | lzss | slb.obj | slb.pos | slb.light | slb.tex |
|---|---|---|---|---|---|---|
| default quarantine | 1,111 MiB | 1,402 MiB | 1,693 MiB | 1,887 MiB | 2,048 MiB | 2,048 MiB |
| 32 MiB | 630 MiB | 630 MiB | 727 MiB | 727 MiB | 727 MiB | 727 MiB |

- **Default:** the run then aborted with `AddressSanitizer: out of memory` at `x` input 442, the same input in two
  runs. Replayed alone, that input is a benign 24 KB rejection, so the abort is accumulation, not the parser.
- **32 MiB:** the run stays far below the 2 GB cap.
- **Detection is kept:** 32 MiB is still many inputs' worth of freed memory, so a use-after-free within one input is
  still caught. Out-of-bounds detection (redzones) does not depend on the quarantine.

## Findings so far (M1a-4 Task 1)

The first runs reported 4 classes of finding against the M1a-3 code. All were fixed in the same task, each with a
unit test that fails on the old code (commits `74a5a69`, `2856ce9`):
- **Partial output on failure:** `decode_lzss` and `BlockFile::read` left a partial decode (or the caller's old bytes)
  in `out`. Found by the synthetic smoke run.
- **Partial state on rejection:** `Slb::parse` kept its bytes and the fix-up fields read so far after a rejected
  trailer. Found by the synthetic smoke run.
- **Allocation before validation:** `decode_tga` allocated the `w·h·bpp` pixel buffer before checking that the input
  could hold the pixels. A real-seeded input (`tga` 49594, seed 1: an 8,949-byte RLE TGA inflated to 4096x4096x32)
  grew the heap by 67,119,136 bytes before failing. Found by the real-seed run.
  - The fix requires the exact data length (type 2), or at least ceil(npix/128)·(1+bpp) bytes (type 10), before the
    allocation.
  - The crafted seed "4096x4096 RLE header" is now part of the smoke subset.

**Remaining bounded worst case:** an *accepted* 4096x4096x32 TGA still costs the pixel buffer plus the output, about
128 MiB. That needs at least about 655 KB of RLE input (or 64 MiB uncompressed), and the 4096 dimension cap allows
it. A valid 4096x4096 DXT1 DDS (8 MiB) decodes to a 64 MiB image the same way. Mutation of the ≤ 256 KiB seeds
cannot build such inputs.

Totals and the reintroduced-defect ("bite") evidence: `docs/knowledge/measurements.md` (A11 fuzz corpus).
