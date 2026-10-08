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
- `fpu_pc` (optional in `expect`, `expect_error`) is the precision state after the call; after any error it is 64 (D8′/D10).
  `reset_after_error()` does not change `fpu_pc`, so a second entry point (`main2`) called after an error runs at 64.

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
