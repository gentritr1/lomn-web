# OSI VM conformance corpus (M2a-1, acceptance A5)

Source: the M2a-1 design consult §3 (`docs/knowledge/research/2026-10-08-m2a1-design-consult.md`), with the controller's
rulings applied (D7–D10 in the M2 spec; **D8′**, 2026-10-08: x87 precision control is VM state `fpu_pc` ∈ {24, 64}).
The corpus lives in `tests/osi/conformance/<group>/`; `tests/osi/conformance/README.md` repeats the authoring rules.
Both interpreters (C++ `lomn_osi_conform`, Python `osi_vm.py --conform`) must match every expectation.

## 1. Format

One program is `<id>.asm` plus `<id>.expect.json`. The `.asm` is assembled by `tools/osi/osiasm.py` into `<id>.osi` at
test time; the binary is never committed.

```json
{ "id": "0x53-float-cond", "rows": ["0x53"], "falsifier": "a float 0.0 condition is an ERROR, not a branch",
  "alternative": "branches as if 0", "falsifier_id": "0x53#notes",
  "entry": "main", "args": [],
  "natives": [ {"cls": "t", "name": "echo", "min": 1, "max": 1, "types": ["int"], "returns": {"type": "0x10", "value": 7}},
               {"cls": "t", "name": "obj", "min": 0, "max": 0, "types": [], "returns": {"game": "GcTimer"}},
               {"cls": "t", "name": "byref", "min": 1, "max": 1, "types": ["byref"], "writes": [{"arg": 0, "type": "0x10", "value": 5}]},
               {"cls": "t", "name": "pc64", "min": 0, "max": 0, "types": [], "sets_fpu_pc64": true} ],
  "registry": "none",
  "expect": { "ok": true, "result": {"type": "0x10", "value": 1}, "depth": 0, "context_depth": 0, "fpu_pc": 24,
              "globals": {"g": {"type": "0x12", "bits": "3f800000"}},
              "heap": {"live": {"o": 0, "a": 0, "s": 0, "g": 0}, "allocs": {"o": 1, "a": 0, "s": 2, "g": 0}} },
  "note": "how the numbers were computed"
}
```

- `entry`, `args`: the engine call, a name and at most 10 variants. A test may pass more than the declared argc, to test
  disposal.
- `natives`: declarative test natives, so both runners implement them from the same JSON and no program depends on an
  engine native.
  - `types` uses the registry vocabulary of `data/osi/natives.json` (`docs/osi/types.md`): `"int"`, `"float"`,
    `"string"`, `"colour"`, `"byref"`, `"type:<Class>"` (a game type named in `data/osi/types.json`), and `"nothing"`;
    `"string-id"`, `"colour16"` and `"unknown:0x0"` exist in the table but no native declares them, so programs avoid
    them.
  - `returns` is one of: a variant (`{"type": "0x10", "value": 7}`, `{"type": "0x12", "bits": "3f800000"}`),
    `{"game": "<Class>"}` (a fresh game handle of that type), `{"text": "abc"}` (a fresh 0x16 dynamic string with these
    latin-1 bytes; `{"hex": "00ff"}` for bytes that are not printable), or `{"arg": i}` (the converted argument i,
    duplicated). Omitted: nothing.
  - `writes` changes a by-ref argument; `sets_fpu_pc64: true` applies D8′ like the registry flag of
    `gcscripttools.stringtofloat`.
- `registry`: `"none"` (only the test natives) or `"natives.json"` (the 1,056-row registry plus the mod rows of
  `data/osi/natives_beta150.json`; this enables the argc-violation keys and the unregistered path).
- An error program replaces `expect` with:

```json
"expect_error": { "message": "Results of comparison expression is not integral", "pc": "@L1", "op": "0x53", "depth": 1,
                  "top": {"type": "0x12", "bits": "00000000"}, "fpu_pc": 64 }
```

- `pc` may be a label from the `.asm` (the runner resolves it through the assembler's label table).
- Values are written as `value` (decimal u32) or `bits` (8 hex digits). Floats only as bits, so an author must compute the
  f32 pattern with `tools/osi/expect_calc.py` and the row's formula.
- **`value` or `bits` is MANDATORY in every value expectation** (`expect.result`, `expect.globals.<name>`,
  `expect_error.top`). `{"type": X}` alone is not a valid expectation: the C++ runner compares it as value 0 and the
  oracle compares the type only, so the two runners would disagree. `text`, `hex` and `game` are the other forms and
  need no `value`. `found` is not part of the format: the C++ runner checks an `expect.found` key if present, the oracle
  ignores it, and no corpus file may contain one. `tools/tests/test_conformance_report.py` fails on both rules.
- `fpu_pc` (optional in `expect`, `expect_error`) is the precision state after the call; after any error it is 64 (D8′/D10).
  `reset_after_error()` does not change `fpu_pc`, so a second entry point (`main2`) called after an error runs at 64.

### Native-framework fields (M2a-2, Task 2b; runners implement them in Tasks 3 and 4)

These fields serve corpus group `n` (`tests/osi/conformance/n/`). The rules they follow are §6 rules 17–20.

**Test natives:**
- `returns: {"game": "<Class>", "engine_refs": k}`: a game object that the host also holds k times (D13, `natives.md`
  §2). The heap stores total = k + 1 (the returned variant). `engine_refs` omitted is 0, which is today's
  `{"game": …}`.
- `returns: {…, "object": "<name>"}`: the native returns the host object called `<name>`. The first return that names
  it creates the object (with its `engine_refs`) and mints its handle (`A g`). Every later return that names it while
  it is live returns **the same handle** with one more variant reference and writes no `A g` (D13 identity per object).
  If the object was freed (its count reached 0), the next return that names it creates a new object. `game` may differ
  between two returns of one object: the variant's type is the declared class, the handle is the object's
  (`natives.md` §2.3).
- `calls: {"entry": …, "args": […], "depth": d}` (rule 6.1 plus `depth`): the nested call is made only while the VM's
  `context_depth` (the engine calls in flight, the outermost one included) is **below** d when the native runs. When it
  is not made, the native just returns its `returns`. `depth` omitted means no limit (the port's nesting cap of 64
  still applies).
- `swallow: true` (D16, rule 17): if the `calls` nested call raises, the runner catches the error inside the native,
  writes its `E` line there, then applies the native's `writes` and returns its `returns`.
- A test native's `{"text": …}` return longer than 255 bytes is a value the original cannot hold (a 0x16 is an
  `ScFixedStringTemplate<255>`, `natives.md` §8.3). No program uses one; `n-name-256` reaches the name-copy bound
  through a string-table literal instead.

**Expectations** (controller ruling, M2a-2 T2b: all of these are accepted):
- `expect.top`: the top slot after the call, after the host has disposed the result; `null` means depth 0. It is
  written like `expect_error.top`.
- `pc_after` (in `expect`, `expect_error`, `then` and `swallowed` entries): `snapshot().pc`, compared under rule 15. It
  is an absolute file offset: an integer, a label `"@L"`, or a label plus a decimal byte count, `"@L+k"`.
- `swallowed`: a list, in order, of the errors swallowed during the call. Each entry is `{message, pc, op, depth, top}`,
  with the meanings of `expect_error`, taken at the throw (D16 rule 2). It holds exactly the swallowed errors, so an
  empty list (or no key) means none.
- `then: {"entry", "args", "ok", "result", "depth", "top", "context_depth", "pc_after"}`: a second engine call on the
  same VM, made after the first call has been compared (rule 19). The fields present are compared like `expect`.

**Assembler:** `tools/osi/osiasm.py` `.rawstring "text" TERM` writes one string-table entry whose terminator byte is
TERM instead of NUL, as a hostile file can (`n-name-256`; `natives.md` §8.3).

**Metadata:**
- `pending`: `"pending": "<task id>"`, a string naming the task whose runner features the program needs (every
  pending program of group `n` carries `"T3/T4"`: both runners). Both runners skip such a program: they do not run it,
  they print `PENDING <id> (<task>)`, and their summary line shows the pending count. The feature each program needs
  is listed in `tests/osi/conformance/README.md`. The key is removed when the program passes on both runners. The
  M2a-2 gate (Task 10) requires the pending count to be 0 **and** the mutated-key check below.

**Fail-closed keys (controller ruling, M2a-2 T2b fix 1).** Each runner keeps an explicit list of the keys it compares
or honours: the expectation keys (`expect.*`, `expect_error.*`, `then.*`, `swallowed[].*`) and the test-native keys
(`returns.*`, `calls.*`, `swallow`, `writes`, …). A program **without** `pending` that carries any key outside that list
FAILS with `uncompared key <path>` (for example `uncompared key expect_error.pc_after`). Metadata keys (`id`, `rows`,
`falsifier`, `alternative`, `falsifier_id`, `argc_violation`, `note`, `pending`) are not expectations and are always
allowed. So a key that a runner silently ignores can never make a program pass. A program that uses a key today's
runners do not compare carries `"pending": "T3/T4"` until both runners compare it. **Mutated-key check (Task 10 gate):**
each key added by Task 2b (`pc_after`, `then`, `swallowed`, `expect.top`, `returns.object`, `returns.engine_refs`,
`calls.depth`, `swallow`), corrupted once in a copy of a program that uses it, must make that program fail on both
runners.

### Coverage metadata (read by `tools/osi/conformance_report.py`, which reads nothing else)

| field | meaning |
|---|---|
| `rows` | every opcode row the program tests, `"0x53"` style (lowercase hex, two digits) |
| `falsifier`, `alternative` | the alternative reading the program kills, and what that reading would give |
| `falsifier_id` | a string or list naming the falsifier targets below the program covers |
| `argc_violation` | `"<key>/<argc>#<i>"`, one of the 20 argc-violation sites (below) |
| `expect_error.message` | with the row in `rows`, covers that (row, message) pair |

## 2. Authoring rules (independence of both implementations)

1. The author reads **only** `docs/osi/opcodes.md`, `docs/osi/types.md`, this file, `docs/osi/trace.md`,
   `tools/osi/osiasm.py` and `tools/osi/expect_calc.py`. Forbidden: `engine/` (all of it), `tools/research/osi_vm.py`,
   and the other corpus group's work in progress. The reviewer checks the agent's tool log for them.
2. Every expectation cites the row cell it comes from (`"rows"`), and a falsifier program states the alternative reading
   it kills (`"falsifier"`, `"alternative"`). The reviewer's 5-sample check asks "does the alternative reading give a
   different `expect`?"; if not, it is not a falsifier.
3. Numbers come from `expect_calc.py` (exact rational → the D8′ rounding at an explicit `pc`; `"%d"`/`"%f"`;
   `atoi`/`atof`; 32-bit wrap; unsigned div/mod; logical shifts; chop float → int) or from integer arithmetic the author
   shows in the file's `"note"`. Running any interpreter to produce an expectation is forbidden: that would make the
   corpus a copy of one implementation.
4. Programs never depend on behaviour the table marks undefined: 0x11 on a non-object, `0x33 0`, a literal 0x84 except the
   D10 error program, INT_MIN / −1, stack over/underflow except our two error programs, behaviour after an error.
   Also excluded, because the reading is not settled or not the original's:
   - decimal text given to `atof` that is not exactly representable in f32, or whose decimal exponent is outside the
     normal range (`___strtold` skips `___dec2num` beyond ±32767, so the precision would not switch; INFERRED). Use
     `"0.5"`, `"-2.25"`, `"3"`. `%f` programs are tagged `"UNVERIFIED-CRT"` in `note` (consult risk 3);
   - `%f` (0xb0) of a float with |x| ≥ 1e17: the original's `___num2dec` keeps 18 BCD digits (0x65fa25, 0x65fa57) and
     `expect_calc.to_string` refuses it;
   - NaN operands: D9 (store NaN results as 0xffc00000) is our choice, while the x87 propagates an operand NaN's
     payload, so the original's result bits for a NaN operand differ from ours. A NaN produced by an invalid operation
     on ordinary operands (inf × 0) is fine.
5. Programs are minimal: one row under test, the fewest other instructions, and those from the "trusted core" (0x40–0x47,
   0x30, 0x52), which get their own programs first.
6. **Precision (D8′).** A program that does mixed int/float arithmetic states the `fpu_pc` it runs at. It is 24 unless the
   program (or its test natives) performed a string → float conversion before (0xb1 on a 0x11/0x16; a float-declared
   argument given a string; a native with `sets_fpu_pc64`). Compute the value with `expect_calc.arith(op, a, b, pc=…)`.

## 3. From rows and falsifiers to programs

- **Each of the 96 handlers: ≥ 2 programs.** Write one normal-path program per distinct stack-effect case in the row (e.g.
  0x60 has 7 cases: nothing/nothing, nothing/other, object identity, game identity, string fold, float cases, int/int → 7
  programs), and one per **row×message pair**. 0x82 alone has 9 inner paths that all surface as "Script is calling %s.%s",
  so the program asserts that text and shows the inner cause only through the post-error snapshot (e.g. argc kept,
  nothing pushed).
- **The falsifier targets** (`falsifier_id`):
  - each line of opcodes.md's "Conformance falsifiers for M2a-1" table, numbered per row in table order: `0x60#1`,
    `0x61#1`, `0x62#1`, `0x63#1`, `0x64#1`, `0x64#2`, `0x70#1`, `0x71#1`, `0x72#1`, `0x73#1`, `0x73#2`, and the four D8′
    lines: `0x70#2` (`16777217 1.0e-15 +` at `fpu_pc` 24 → `4b800001`; kills the 53-bit formula), `0x70#3` (the same
    after a 0xb1 on the string `"0.5"`, at 64 → `4b800000`; kills "24 forever"), `0x70#4` (`16777217 1.0e-10 +` at 64 →
    `4b800001`; kills the 53-bit formula, which gives `4b800000`) and `0x72#2` (`f32:0x24368a23 f32:0x1af99066 *` at 24
    → `002c7cd4`, a subnormal product rounded twice; kills one rounding, `002c7cd5`). Record the "old reading" column
    as `"alternative"`. One program may do `0x70#2`–`0x70#4` together (`falsifier_id: ["0x70#2", "0x70#3",
    "0x70#4"]`): run the 1.0e-15 addition, convert `"0.5"`, run both additions. The low bytes of the three results
    (1.0e-15 before, 1.0e-15 after, 1.0e-10 after) are 01/00/01 under D8′, 00/00/00 under the 53-bit formula,
    00/00/01 under "64 from the start" and 01/01/01 under "24 forever": all four readings differ;
  - the two guards: `guard:0xb1` (`16777217` → 16777216.0) and `guard:0xb2` (3.0e9 → −1294967296);
  - every row whose notes cell states a falsifier (`0x60#notes` … `0xb2#notes`; the report lists them). Those include:
    `0x74` `2.0 1 ^` → `4f000000`, `3 2 ^` → 81; `0x75` `-1 3 %` → 0; `0x78` `-8 1 >>` → `7ffffffc`; `0xb2` 2.7 → 2,
    −2.7 → −2; `0x70` 200+60 chars → kLenTooLong (asserted as our error text for the non-OSI throw, `"kLenTooLong"`, with
    the post-error depth = before − 2, nothing pushed), 200+54 ok; `0x60` "ABC" = "abc" → 1, `1 = "1"` → error,
    nothing = 0 → 0.
  - Also (not counted by the report, but listed by the consult): `0x53` float condition → error; `0x50` file-relative
    target (a program whose target would be wrong under a PC-relative or script-base+4 reading).
- **The 20 argc-violation sites** (types.md, "Argument-count violations"): `argc_violation` = `"<key>/<argc>#<i>"` with
  i = 1..sites of that row (e.g. `gcareadirector.hide/1#1` … `#8`). Each program uses `"registry": "natives.json"` and
  expects "Script is calling <cls>.<fn>" (the inner "Too many/few" message is overwritten). They are free falsifiers: a
  VM that runs them is wrong.
- **Engine-call protocol programs:**
  - `call("Setup…")` resolves to `setup` by prefix; a query shorter than the entry fails (`M` line);
  - 10 arguments supplied to a 0-argc function are all disposed (heap `allocs`/`live` show the strings freed);
  - the `argc > 10` error text;
  - a `0x52` on the `0xff` marker leaves `{0xff, r}` for the caller (asserted via `depth` 0 after the call and `result`);
  - re-entrancy: a test native `t.reenter` calls `vm.call("inner")`, and the outer frame's locals are intact afterwards
    (context depth 2 → 0; trace `C … R` nesting).
- **Ownership programs:** array aliasing through 0x35 (shared, not copied); an object owns its props (free order
  o → props back to front in the `F` lines, asserted via `heap.allocs`/`live` and, between implementations, via the trace
  `cmp`); string deep copy; 0x82 pops dispose the converted argument (string alloc 1, free 1).
- **Our caps (D10):** recursion through 0x51 until depth 1,000 → `"Stack overflow (1000 slots)"` at depth exactly 1000 and
  no write beyond; a 0x30 on an empty stack → `"Stack underflow"`.

## 4. Error-path assertions

- After the error the precision state is 64 (D8′) and `reset_after_error()` leaves it there, so the `main2` check runs
  at 64.
- `expect_error.message` is compared byte for byte, typos included. The `%02.2X` type formats are spelled out by the
  author, e.g. `(type:12)`.
- `pc` is the failing instruction's own address (the table says "PC not advanced" for pre-check errors; for 0x70's
  kLenTooLong it is the 0x70 itself).
- `depth` and `top` are the stack **at the throw**, as "After an error" documents: operands already popped stay popped,
  nothing is pushed, and for 0x82 converted arguments stay converted. `fpu_pc` is 64 (D8′).
- The runner takes `snapshot()` before any reset, then calls `reset_after_error()` and asserts the VM is usable again
  (`call("main2")` on a second entry point succeeds). That last part is our behaviour, documented as such, not
  conformance with the original.

## 5. Coverage, measured not invented

The acceptance is the output of `tools/osi/conformance_report.py DIR [--group a|b] [--missing]`, which reads only the
`.expect.json` metadata:

```
handlers ≥2: x/96, row×message pairs: y/111, falsifiers: k/31, argc violations: z/20, programs: N
```

- The denominators are measured from the tables by the script (2026-10-08, empty corpus):
  - 96 handlers;
  - **111** row×message pairs. The consult's estimate was 114. The script counts the quoted messages of each named row's
    errors cell, expands abbreviated cells such as "… '=' (rhs) …" from the row's own lhs message, skips the section
    reference "The game-variable map" and skips 0xb0, whose cell is "none";
  - **31** falsifier targets (15 table lines, 2 guards, 14 notes rows). The 15 lines are the M2a-0 11 plus the four D8′
    lines of fix round 1 (`0x70#2`–`0x70#4`, `0x72#2`);
  - 20 argc-violation sites.
- Group A: 48 handlers, 60 pairs, 15 falsifiers. Group B: 48 handlers, 51 pairs, 16 falsifiers, 20 argc sites.
- N is whatever the authors write; the ledger records it. ESTIMATE from the consult: 280–340 programs.

**The 29 opcodes `Base.osi` never uses** (consult M3) all have authored rows and get programs like any other:
- 0x00 and 0x04–0x06 are no-ops: depth unchanged, PC +1 or +5.
- 0x02, 0x03 and 0x41 always error: assert the text, PC not advanced and depth unchanged; 2 programs each, varying the
  stack content.
- 0x18 and 0x1a access an object member by string, with 0x11 and 0x16 names; there are 4 error texts.
- 0x24 and 0x25 act on frame and global variables. A global decrement raises the copy-pasted "increment" message.
- 0x31, 0x32, 0x34; 0x49, and 0xa4 erroring on what 0x49 pushes; 0x6f, 0x78.
- 0x7a and 0x7b add to or subtract from raw bits on a float: `3f800000 → 3f800001`.
- 0x74 needs its 4 case programs. 0x80 and 0x81 change no state (PC +5).
- 0x83 takes the name from the stack (0x11 and 0x16), plus the "Invalid member type" error and the argc errors.
- 0x84 needs the D10 literal error, plus the memoised-second-execution program: the trace must print `82` both times and
  the result must be identical.
- 0xb8, 0xb9, 0xbc and 0xbf are type tests; on a game handle 0xbf gives the type + 0x10000.
- 0xff pushes nothing and stops Run: a `main` that halts leaves `{0xff marker, nothing}` for `CallWithReturn`, so the
  result is nothing and the depth 0.
- `BCLUnused`: 2 programs on two different unused bytes (`.byte`).

## 6. Format amendments (controller rulings, 2026-10-08, after corpus group B)

Both runners (`lomn_osi_conform`, `osi_vm.py --conform`) implement these. They close gaps the corpus authors met.

1. **Re-entrant test natives:** a test native may carry `"calls": {"entry": "<name>", "args": [<v>…]}`. When the native runs, the runner performs `vm.call(entry, args)` **before** the native returns. The nested call's result is disposed by the host right after its `R`, as for any engine call. The native then returns its own `returns`.
2. **Host-supplied engine arguments** may be written `{"text": "…"}` (and `{"hex": "…"}`), like test-native `returns`. The runner allocates them on the VM heap as dynamic strings (type 0x16, `A s` events, in argument index order) before the call. They count in `heap.allocs` and `heap.live`.
3. **Empty stack top:** in `expect_error`, `"top": null` means depth 0. The trace prints `----:--------`.
4. **Errors at a memoised 0x82 site:** `op` is the pristine byte `0x82`, never `0x84`. This is the same rule as the `I` line.
5. **Errors raised before `Run` starts** (SetPCAndArgs: argc > 10): `pc` is the entry point's code offset (SetPCAndArgs sets PC before the check), `op` is the byte at that offset, and `depth`/`top` describe the stack at the throw.
6. **Game type ids** always come from `data/osi/types.json`, also under `"registry": "none"`.
7. **`heap` is optional.** A runner compares only the fields present in the expectation.
8. **Entry-point names and FindFunction's prefix match:** "main" matches the query "main2". A program that uses a second entry point therefore declares the longer name first (e.g. `.function main2 …` before `.function main …`). This is not a runner rule, but authors of both groups must follow it.
9. **Unreachable row×message pairs.** A pair whose message the original can never show as the final error is exempt from coverage when listed here with its row-text justification. The path that would raise it is still covered by a program asserting the actual final message.
   - 0x82 "Requested variant conversion unimplimented…", "Too few parameters…" and "Too many parameters…", and 0x83 "Argument type mismatch…": all of these are raised inside the catch(…) and overwritten by Error("Script is calling …") (0x82/0x83 errors cells).
   - 0x91 "Reference beyond end of string table", 0x93/0x94/0x96 "Assigning into string table is illegal" and 0x95 "Removal from string table is illegal": these need a 0x13 value, which no handler and no CreateVariant overload creates (opcodes.md variant types).

   Further pairs may be added only with the same kind of citation.
10. **Heap values in expectations** (`result`, `top`, `globals`) are written as `{"type": <kind or game type>, "value": <handle>}`. The handle is the per-kind monotonic id of D7, starting at 1 for each kind (o, a, s, g) in a fresh VM. Example: the first object a program creates is `{"type": "0x01", "value": 1}`.
11. **The engine marker** that `Vm::call` pushes is `{0xff, saved PC}`. In a fresh VM the saved PC is 0. **Corrected 2026-10-08 (oracle second-reader, controller re-read):** a nested call saves the PC **as already advanced past the invoking instruction's operands** (0x82 increments PC six times at 0x6015c6–0x601780 before the native call at 0x601af5, so the marker is site+6; 0x83 → site+4), not the instruction's own address. Expectations write the top-level marker as `{"type": "0xff", "value": 0}`.
12. **Pop-versus-check order on error paths** (members 0x10/0x15/0x18–0x1a, colour 0xa0–0xa3, arithmetic with one invalid operand). The asm settles each case, and opcodes.md records the outcome. An expectation follows opcodes.md as amended, never a guess.
13. **0x02/0x03 advance PC before erroring** (`inc` at 0x6038a6 / 0x6038d6). This corrects §4's "PC not advanced" for these two rows. The `E` line and `expect_error.pc` still give the instruction's own address, while `snapshot().pc` is pc+1. 0x41 and BCLUnused really do not advance.
14. **Unspecified runner details**, now fixed:
    - (a) An `M` line is not followed by `R`. CallResult is `found=false, ok=true`.
    - (b) `--conform` trace headers always hash the three data files in use, also under `"registry": "none"`.
    - (c) `reset_after_error()` disposes from the top down to and including the call's outer marker and emits the `F` lines. This is host behaviour, but it is visible in traces.
    - (d) A test native's by-ref `writes` overwrite the slot without disposing the old value. The following Pop(true) disposes the new value (0x601c5f).
    - (e) ConvertStringIDToDynamicString on a string of 255 or more characters throws before any `A s` line, with message `kOutOfRange` (0x60b95c–0x60b970), in the same style as kLenTooLong.
    - (f) Engine-protocol programs with no opcode row may use `"rows": []` and must name the opcodes.md section ("Engine calls") in `note`.
15. **`snapshot().pc` after an error** (amended 2026-10-09, M2a-2 T2a-pc; it was "unspecified and never compared";
    wording fixed in M2a-2 T2b after the T2a-pc review, minors M2, M6 and M7).
    `snapshot().pc` is the failing instruction's address plus the PC its handler has written at the throw, **except in
    case (b)**, where it is the address of the inner instruction that raised first plus that instruction's own delta.
    The values are in opcodes.md "Error PC (the PC at the throw), per handler", which was derived from the asm. The rule
    applies to every error, not only to swallowed ones (D16):
    - (a) **An error the original raises:** the "PC at throw" of its row. In short, every handler is at
      +(1 + operand bytes), the PC past its whole instruction. 0x50/0x51 (+2) follow that rule too; only their normal
      exit differs (a jump target). The exceptions:
      - +0 for BCLUnused, 0x41 and 0x53;
      - 0x82: +5 for "Call to unregistered Game Function", and +6 for everything after the lookup;
      - 0x83: +3 for "Invalid member type…" and for the kOutOfRange name copies (0x60226a, 0x602299), and +4 for its
        "Script is calling" re-raise;
      - a memoised 0x82 site (the original's 0x84): +6.
    - (b) **A "Script is calling…" re-raise after a native whose nested engine call raised** keeps the PC that the
      nested error left in the inner function, because `CallWithReturn`'s restore (0x60b5a2) never runs (CITED by the
      T2a-pc review: 0x60b450 has no catch record). Both engines already rethrow the nested error without touching
      `pc`. So `snapshot().pc` is the inner instruction's address plus its case-(a) delta, while the `E` line keeps the
      outer 0x82 / 0x83 / 0x84's own address.
    - (c) **The port's own errors** take the PC the original handler holds at the operation they stand for:
      - stack underflow and overflow at a ScOSIStack call: the "PC at its ScOSIStack calls" column. For 0x83 the name
        pop (0x6021c9) is +3 and every later stack operation is +4;
      - a stack error at an access that is not a ScOSIStack call takes the handler's normal-exit PC. These are 0x35
        (the port's `top()` underflow; its column value happens to agree), and 0x7a and 0x7b, which read
        `[VM+0x1f50]−8` directly after their `inc` (0x605be8 / 0x605c08), so their column is "—". All three give +1;
      - any other error raised inside a handler body (an "Undefined: …" index, type or operand check, INT_MIN / −1,
        more native arguments than stack slots): +0 for 0x52/0x53, +2 for 0x50/0x51, +5 / +6 for 0x82 before / after
        the lookup, +3 / +4 for 0x83 before / after the n byte, and the normal-exit PC for every other handler;
      - the engine-call nesting cap: the PC of the native call that re-entered;
      - a literal 0x84 in the file: +0;
      - SetPCAndArgs argc > 10: the entry point's offset (rule 5).
    - (d) **Unspecified, never compared:** an operand that runs past the end of the script image, a PC outside the
      image, and the C++ VM's "Undefined: host exception…" (a `std::exception` from host code). The original reads
      past its buffer in the first two, the two engines check at different moments, and the third has no original
      counterpart.

    The `E` line and `expect_error.pc` stay the instruction's own address. `pc_after` is absolute, like `pc` (§1). A
    runner compares `snapshot().pc` when an expectation carries `"pc_after"`, and every `swallow` program (D16)
    carries it.
16. **0x74 float^float uses the host `pow`** in both implementations. That is not bit-reproducible for inexact powers, so corpus programs use exact powers only (rule 4). Base.osi never executes 0x74.
17. **Swallowing test natives (D16, M2a-2 T2b).** A native with `"swallow": true` follows D16 (spec §4; `natives.md`
    §5.4). It mirrors the three `GcAreaLoader::LoadArea` blocks, the only swallowing call sites in the exe.
    - (a) The nested call's error leaves the VM as at the throw: the inner marker, the arguments and the partial frame
      stay; `context_depth` stays raised; `pc` is the error PC (rule 15, including case (b) for a re-raise); `fpu_pc`
      becomes 64. Nothing is reset.
    - (b) The runner writes the error's `E` line at that point (pc and op of the failing instruction, depth at the
      throw), followed by `P <pc> 64` if `fpu_pc` was 24. It does not set the outer call's error, does not reset and
      ends no engine call. It records the error for `swallowed`.
    - (c) The native then applies its `writes` to its **original** argument slots, the ones the handler addressed
      before the native ran. It returns its `returns`.
    - (d) The calling handler pops n slots with dispose **from the current top** (the inner leftovers), pushes the
      result and leaves `pc` alone. Dispatch continues in the inner function at the error PC. The `N` line prints the
      arguments as they were converted before the native ran (what the native received); the `F` lines of the pops are
      the leftovers'.
    - (e) The first `0x52` that meets an `0xff` marker stops the running `Run`. The engine call that owns that loop
      pops the result, pops with dispose to the first `0xff` marker (the inner one), decrements `context_depth`,
      rescans `frame_ptr` if `context_depth` is not 0, sets `pc` from that marker and returns the value.
    - (f) After a later error, `reset_after_error()` pops down to and including **the outermost call's own marker**
      (the slot at the depth that call recorded at its entry), through any inner markers left by a swallow, and
      restores `context_depth`, `frame_ptr` and `pc` to their values at that entry.
    - If the nested call does not raise, `swallow` changes nothing.
18. **Engine references and identity (D13, M2a-2 T2b).** A `{"game": …}` return goes through the host's
    `CreateVariant`: +1 reference, and the handle is minted (`A g`) only when the object first passes through it. The
    count is the host's `engine_refs` plus the variants' references. `Dispose` takes 1; at 0 the runner writes `F g` and
    deletes the host object. `heap.live.g` counts objects whose total is above 0, so an object the host still holds is
    live after the call. The host's references are never released by the program (the corpus has no host release).
19. **`then`, a second engine call.** After the first call's expectation has been compared:
    - if the first call raised, the runner calls `reset_after_error()` first, as §4 does for `main2`;
    - if it did not, nothing is reset: the second call runs above whatever the first one left (D16 rule 5).

    Then it calls `then.entry` with `then.args` and compares the fields given. `then` replaces the implicit `main2`
    check of §4 for the programs that carry it.
20. **Pending programs.** A program with a `pending` list (§1) needs runner features that Tasks 3/4 add. Its
    expectations are hand-derived like every other's. They are not adjusted to whatever a runner does, and they are not
    a gate until the list is empty.
