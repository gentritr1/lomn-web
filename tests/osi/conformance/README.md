# OSI VM conformance corpus

The format, the coverage targets and the report are specified in [`docs/osi/conformance.md`](../../../docs/osi/conformance.md).
Group `a/` (consult §1.4: nop/line, members, members-by-string, variables, calls/branch, arith, colour, type tests) and
group `b/` (stack, push, compare, logic/bitwise, inc/dec, game-var, native calls, arrays, convert, halt, BCLUnused) each
hold `<id>.asm` + `<id>.expect.json`; `.osi` files are assembled at test time and never committed.
Coverage: `.venv/bin/python tools/osi/conformance_report.py tests/osi/conformance/<group> --group <group> --missing`.

## Group `n/` (M2a-2 Task 2b): the native framework

The fields are in `docs/osi/conformance.md` §1 ("Native-framework fields") and §6 rules 17–20. Every expectation was
derived by hand from `docs/osi/natives.md`, D13–D16 and the error-PC table in `docs/osi/opcodes.md`, and each `note`
shows the derivation. Neither engine was run.

**Pending Tasks 3/4.** A program with `"pending": "T3/T4"` needs runner features that Tasks 3 (C++) and 4 (oracle)
add. Both runners skip it (they print `PENDING <id> (<task>)` and count it in their summary), so it is not a gate until
the key is removed. **The M2a-2 gate (Task 10) requires the pending count to be 0:** Tasks 3 and 4 remove the key from
each program once it passes on both runners. `conformance_report.py DIR --list` prints each program with its status.

| program | status | needs |
|---|---|---|
| `n-game-engine-ref-survives` | pending | `engine_refs` |
| `n-game-engine-ref-zero` | active | (control: `engine_refs` 0 is today's behaviour) |
| `n-game-identity` | pending | `engine_refs`, `returns.object` |
| `n-reenter-depth3` | pending | `calls.depth` |
| `n-reenter-nested-error-propagates` | active | (`pc_after` and `then` are extra comparisons) |
| `n-reenter-nested-error-swallowed` | pending | `swallow`, `swallowed`, `expect.top`, `pc_after`, `then` |
| `n-onevent-args` | active | — |
| `n-swallow-continue` | pending | D16: `swallow`, `swallowed`, `expect.top`, `pc_after`, `then` |
| `n-swallow-reerror` | pending | D16: `swallow`, `swallowed`, `pc_after`, `then` |
| `n-swallow-pops` | pending | D16: `swallow`, `swallowed`, `expect.top`, `pc_after`, `then` |
| `n-swallow-writes` | pending | D16: `swallow`, `swallowed`, `expect.top`, `pc_after`, `then` |
| `n-name-256` | pending | the strlen name read of a 0x11 literal with the 255 bound (kOutOfRange; a hostile file's non-NUL terminator), `pc_after` |
| `n-name-255` | active | (the boundary control for `n-name-256`, same 0x11 path; `pc_after` and `then` are extra comparisons) |

"Active" means that a runner which ignores the new comparison keys should already pass. That is INFERRED from the rules,
not run: authoring rule 3 forbids running an engine to check an expectation.

## Authoring rules (copied from docs/osi/conformance.md §2; that file wins if they ever differ)

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
