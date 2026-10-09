# OSI VM trace format (M2a-1)

Source: the M2a-1 design consult §2 (`docs/knowledge/research/2026-10-08-m2a1-design-consult.md`), with the controller's
rulings applied: D7–D10 as ratified in the M2 spec, and **D8′** (2026-10-08: x87 precision control is VM state, which
adds the `P` record). Fix round 1 (2026-10-08) made every field format exact and fixed the event order. Every
implementation (C++ native, C++ wasm32 under node, the Python oracle) writes these exact bytes; a run fails if any two
differ. Nothing is "normalised" after the fact.

## 1. Lexical rules

- **Text**, ASCII only, one record per line, each line ends with exactly one LF (`\n`), no CR. Fields are separated by
  exactly **one space**, except where a format below pads a number with spaces (`%4d`, `%3d`, `%6d`, `%8d`, which are C
  `printf` conversions and may produce runs of spaces). No line has trailing spaces. No JSON.
- **Hex** is always lowercase (`%x` family). Widths are fixed where given (`%08x`, `%04x`, `%02x`), never shorter.
- **Decimal** counts (`argc`, lengths, sequence numbers) use the `printf` conversion given; `%d` means no padding.
- **A variant `<v>`** is `%04x:%08x` of (u16 type, u32 value): int bits (two's complement), f32 bits, string-table index,
  colour, handle (D7), or PC. A game object is `<game type id>:<handle>`, e.g. `0020:00000003`. The empty slot is the
  literal `----:--------`.
- **A name token** (`C`, `M` and `N` lines: function, class and native names) prints each byte in 0x21–0x7e literally,
  except `\`, which prints as `\\`; every other byte (space, control, ≥ 0x80) prints as `\x%02x`.
- **An error message** (`E` line) is wrapped in `"`. Inside, `"` prints as `\"`, `\` as `\\`, bytes 0x20–0x7e literally
  (space included), every other byte as `\x%02x`.
- **Byte strings** on `S` lines and in `natives.log` are the raw bytes as lowercase hex, two digits per byte, with no
  separators (latin-1, never decoded), so 0x80+ bytes and NULs are exact.

## 2. Records

Angle brackets mark fields; everything else on a line is literal. `[a|b]` in the descriptions chooses one form; the
brackets are not printed.

| record | exact format (C `printf`) | meaning |
|---|---|---|
| header | `# lomn-osi-trace 1 script=%s natives=%s mods=%s types=%s fpu=pc24` | the four values are lowercase sha256 hex (64 digits) of the `.osi`, `natives.json`, `natives_beta150.json`, `types.json`; A6 runs append ` keys=%s` (the KEYSPEC with each space replaced by `,`) |
| `C` | `C <name> %d <a0> <a1> … <a9>` | engine → script call, after FindFunction. `%d` = the argc the script declares. **Always all 10 supplied variants**, in index order; unused ones are whatever the engine passed (normally `000f:00000000`) |
| `M` | `M <name>` | Message "Unable to locate function": the call does not happen (no `C` line) |
| `I` | `I %08x %02x %4d %4d <top>` | before each instruction executes: pc (file offset), the pristine opcode byte (0x82, never 0x84), stack depth, frame base index (0 when none), the top slot (`----:--------` at depth 0) |
| `N` | `N %08x <cls>.<fn> %d <a0> … <aN-1> -> <r>` | after the native returns: the 0x82/0x83/0x84 pc, the class and function names as in the string table (case preserved; 0x83: the popped name), argc `%d`, the converted arguments, `->`, the result. A **by-reference (0x200) parameter** prints as `0200:%08x` where the value is the **stack slot index** (0 = bottom of the stack) of the argument it refers to — never a host address and never the slot's contents; the value the native wrote into that slot appears only in `natives.log` as a `W` record; no `S` line is written for a by-ref argument (a string the native creates for the slot is an `A s` event in the native's own events, step 4). With argc 0: `N %08x <cls>.<fn> 0 -> <r>` |
| `S` | `S %08x %3d <hex>`; empty string: `S %08x %3d` | the text of string handle h, length `%3d`, bytes as hex; for length 0 nothing follows the length (no trailing space) |
| `A` | `A o %08x %04x`, `A a %08x %08x`, `A s %08x %3d`, `A g %08x %02x` | allocation of an object (class id), an array (element count), a dynamic string (length), a game handle (game type id) |
| `F` | `F <k> %08x` | free, k ∈ {`o`,`a`,`s`,`g`}: a reference count reached zero or a string was deleted |
| `P` | `P %08x 64` | precision control switched to 64 bits (D8′); pc = the instruction during which it happened |
| `R` | `R <v>` | the engine call's result, after the marker pop. The **host disposes the result immediately after writing `R`** (the original's `Call` disposes it; for `CallWithReturn` our host does the same), so any `F` lines for it follow `R`, and `heap.live` in a conformance expectation is read after that disposal |
| `E` | `E %08x %02x %4d "<message>"` | an error: pc, opcode and depth **at the throw**, then the message |
| `K` | `K %6d %08x %08x %08x` | A6 frame header: frame number, fake time and dt as f32 bits, the Process counter |

Digest mode (`--digest`) replaces each `K` block by `D %6d %8d %016x` (frame, line count, FNV-1a 64 of all bytes of the
block, LFs included) and ends with `T %d %016x` (total lines, FNV-1a 64 of the whole trace). The header line is
included verbatim.

## 3. Event order

The trace is written in the order things happen; a record is never moved to "tidy" a block.

**The `P` record (D8′).**
- The header's `fpu=pc24` is the initial mode (`fpu_pc` = 24). `P` is written **at the moment of the switch**, in event
  order with `A`, `F`, `S` and `N`, and only when the mode actually changes, so a trace holds at most one `P` line.
- The moments are: inside a `ConvertVariant` string → float, where `_atof` would run (before that conversion's `F` of the
  old value); at the return of a native whose row has `sets_fpu_pc64`, before that call's `S`/`N` lines (the switch
  happens inside the native in the original); and after an error's `E` line (below).

**An error.** `E`, then `P` if the mode was still 24 (the landing-pad `fninit`), then the next `C` or `M` line, or the
end of the trace. No `R` line is written for the failed call, and no `F` line for what the error left on the stack
(D10: nothing is disposed until the host resets).

**A swallowed nested error (D16, M2a-2).** A native that swallows its nested call's error (the three `LoadArea` blocks;
a `swallow` test native) writes that error's `E` line, and `P` if the mode was still 24, at the swallow point. No `R`
line is written for the nested call. The trace then goes on in event order: the native's `S`/`N` lines, its pops' `F`
lines, and the next `I` line at the error PC (conformance.md rule 17).

**A native call (0x82; 0x83 and 0x84 the same after their own operand handling)**, pinned to the 0x82 row of
`docs/osi/opcodes.md` (BCLCallGameFunction 0x6015a0):
1. `I` for the instruction.
2. The lookup and the min/max check (errors here: `E` …).
3. Argument coercion, for i = 0 (the deepest) … n−1, each complete before the next: a by-ref parameter (0x200) gets its
   box and nothing is traced; otherwise a type that differs is converted in place by `ConvertVariant` (0x6018db),
   which writes `A s` for a new dynamic string (0x11 → string, int/float → `"%d"`/`"%f"`) and `P` for a string → float
   switch; then `PushInto` (0x60191b) disposes the old value: `F` if it owned something (a 0x16 string, a refcount
   reaching zero).
4. The native runs (0x601af5). Whatever it causes appears here, in order: nested engine calls (`C … R` blocks, whose own
   records nest completely), `A g` for game handles it creates, `A s` for a string it returns, `P` at its return if its
   row has `sets_fpu_pc64`.
5. `S` lines: one per argument of type 0x16, in index order, then one for the result if it is a 0x16.
6. `N`.
7. The n pops **with dispose** (0x601c5f), top first: `F` lines for argument n−1 down to 0 (and for whatever the native
   wrote into a by-ref slot).
8. The result is pushed (0x601c8d); nothing is traced for it. The next `I` follows.

**Other instructions.** An instruction's records follow its `I` line in the order the handler's row describes; e.g.
0xb1 on a 0x11: `A s` (the in-place dynamic string, 0x60bce9–0x60bd02), `P` (the `_atof`, if still 24), `F s` (the
string disposed with the operand, 0x60702b); 0x30 on a string: `F s`.

**Engine calls.** `C`, then the instructions, then `R` (or `E` …). `CallWithReturn` disposes surplus arguments (index
argc … 9) when it pushes the arguments, before the first `I`: their `F` lines come between `C` and the first `I`, in
index order. The marker pop after `Run` disposes nothing on the normal path (0x52 left `{0xff, r}` and r was popped).

**Ownership order** is observable and must match: object and array destructors dispose back to front, `0x52` pops with
dispose from the top down, `CallWithReturn` disposes surplus arguments in index order.

## 4. Worked example

The program (assembled with `tools/osi/osiasm.py`; the addresses are its listing):

```
.strings "t" "echo"
.function main 0
    PushConstantString "abc"        ; 0x2d   {0x11, 2}
    CallGameFunction t.echo 1       ; 0x30   t.echo(string) -> string
    Pop                             ; 0x36
    PushConstantf32 1.0             ; 0x37
    CompareAndBranchIfFalse @end    ; 0x3c   a float condition: error
end:
    Return                          ; 0x3f
```

with the test native `{"cls": "t", "name": "echo", "min": 1, "max": 1, "types": ["string"], "returns": {"text": "abc"}}`
and the engine call `main()` on a fresh VM (PC 0, so the frame marker is `{0xff, 0}`):

```
# lomn-osi-trace 1 script=<64 hex> natives=<64 hex> mods=<64 hex> types=<64 hex> fpu=pc24
C main 0 000f:00000000 000f:00000000 000f:00000000 000f:00000000 000f:00000000 000f:00000000 000f:00000000 000f:00000000 000f:00000000 000f:00000000
I 0000002d 46    1    1 00ff:00000000
I 00000030 82    2    1 0011:00000002
A s 00000001   3
A s 00000002   3
S 00000001   3 616263
S 00000002   3 616263
N 00000030 t.echo 1 0016:00000001 -> 0016:00000002
F s 00000001
I 00000036 30    2    1 0016:00000002
F s 00000002
I 00000037 44    1    1 00ff:00000000
I 0000003c 53    2    1 0012:3f800000
E 0000003c 53    1 "Results of comparison expression is not integral"
P 0000003c 64
```

- `A s 00000001` is step 3: the 0x11 argument converted in place to a dynamic string (the old 0x11 owns nothing, so no
  `F`). `A s 00000002` is step 4: the string the native returns.
- Step 5 writes the argument's `S`, then the result's; step 7 frees the argument (`F s 00000001`).
- 0x30 disposes the result (`F s 00000002`).
- 0x53 pops its condition without dispose and then raises: depth 1 at the throw, so the `E` line shows depth 1. The
  call has no `R` line. The mode was 24, so the landing pad's `P` follows the `E` line. The trace ends there (or the
  host's next `C`/`M` line follows).

## 5. Rules that make three implementations byte-identical

- **Floats bit-exact:** always the 8 hex digits of the binary32 pattern (`std::memcpy` into `uint32_t`; Python
  `struct.unpack('<I', struct.pack('<f', x))[0]` on a value already rounded to f32). NaN is one pattern by D9
  (`ffc00000`). `-0.0` prints `80000000`. `K` prints time/dt as the f32 the script receives.
- **Ints:** two's complement u32 (`-1` = `ffffffff`); Python masks with `& 0xffffffff` everywhere a value is stored.
- **Why this is achievable:**
  - no host pointers (D7: handles from per-kind monotonic counters starting at 1);
  - no printf of floats (`%08x` of bits only). `%f` is used by the VM semantics of `0xb0`, where the text goes through an
    `S` line as bytes. Consult M21 shows Python's `%f` equals libc's on f32-widened doubles for the ties tried; it is
    still UNVERIFIED against the original's CRT, so it stays a listed blind spot;
  - canonical NaN (D9);
  - `-ffp-contract=off` for native == wasm32 (`engine/CMakeLists.txt`);
  - `std::fma` only inside the mixed-arithmetic helpers (wasm32 emulates it in software, correctly rounded);
  - the precision mode is explicit state with its own record (`P`, D8′), never host FPU state.
- **Size:** at the static upper bound (consult M13, 122k instructions/frame) 600 frames would be 73 M `I` lines, about
  3 GB per implementation; the digest is 600 lines. The likely size (ESTIMATE from consult M12/M18) is 2k–20k lines per
  frame, 50–500 MB for 600 frames. The full-trace window defaults to frames 0–2 plus any frame the digest flags.

## 6. The native-return replay log (A6/A9)

The C++ run writes `natives.log` next to the trace, one line per native call, in call order:

```
%08d <cls>.<fn> %d -> <r>[ S %d <hex>][ W %d <v>[ S %d <hex>]]…
```

- `%08d` is the 0-based call sequence number; `<cls>.<fn>` uses the name-token rule; `%d` is argc; `<r>` and `<v>` use
  the `<v>` notation of §1 (a game handle is `<type>:<handle>`, e.g. `0022:00000004`).
- ` S %d <hex>` follows `<r>` only when the result is a 0x16 string: its length in decimal (no padding) and its bytes.
  For an empty string it is ` S 0` with nothing after it.
- ` W %d <v>` records, in argument index order, a value the native wrote into a by-ref (0x200) slot before the pops: the
  argument index in decimal, then the slot's new value; a 0x16 value is followed by ` S %d <hex>` as above.

Python's `NativeRegistry.replay_from(log)` replaces every native body:
- On call `seq` it asserts that `cls.fn/argc` match the log; otherwise it raises
  `VmError("replay desync at seq N: expected a.b/2, got c.d/3")`. That is a VM-semantics finding, since the call order is
  VM behaviour.
- It returns the logged variant. A game handle is re-created by asking the heap for the **next** game handle and
  asserting that it equals the logged one (handles are allocated in the same order on both sides, D7); a string result
  is re-created from its ` S` bytes.
- It applies the `W` writes, and the row's `sets_fpu_pc64` exactly as a live call would (registry data, not native
  behaviour), so the `P` line appears at the same place.

So any A6/A9 disagreement is VM semantics, never native semantics. For A6 the log is produced by the typed stubs (D11);
the replay path is used anyway so that A9 (M2a-2, real natives) needs no new mechanism.

### Version 2 (D14, M2a-2)

The full contract, with a worked example, is `docs/osi/natives.md` §3. In short:
- **` G %d`** follows every game value: a result, a ` W` value, or a `C` argument. It is the number of references the
  engine holds on that object after the native returns, not counting this variant. The heap stores total = engine +
  variants. A v1 line is a v2 line with no game value, or with `G` absent, which means 0.
- **`C %08d <name> <a0> … <a9>`**, its own line: native seq N starts an engine → script call at this point. The 10
  arguments use the `<v>` notation, a 0x16 is followed by ` S %d <hex>` and a game value by ` G %d`.
- **`K %6d`**, its own line, at the start of each frame (the trace's `K` frame number), before that frame's first
  native line. It is written for every frame, also for a frame that calls no native.
- **` W` after `-> <r>`.** A native's by-reference writes happen before its return, but they are printed after the
  result, in the v1 layout: `%08d <cls>.<fn> %d -> <r>[ S …][ G …][ W %d <v>[ S …][ G …]]…`.
- **Reserved:** `C -1 <name> <a0> … <a9>` (a host-originated call) and `H <type>:<handle> %+d` (a host-side reference
  change). No M2a-2 code writes them.
- **Order.** Sequence numbers are assigned at native entry, and lines are written in event order. So a native's own
  line is written at its return, after its `C` lines and the lines of the natives they caused.
- **Replay (I6).** The order assertion is as in v1. A live logged handle is returned with one more variant reference
  and no `A g`; otherwise the logged handle must equal the next handle, and it is minted with `G` engine references.
  The replay performs each `C N` call inside native N. It never performs `C -1` or `H`, and it asserts that its own
  host's next call or reference change matches them.

**The merge (controller ruling, M2a-2 T2b, accepted as proposed; `natives.md` §7.2):** an override sets `tier`, and
`default` when it has one, on the **winner row** of its key only (exe keys in `natives.json`, mod keys in
`natives_beta150.json`); `cite` and `note` stay in `data/osi/native_overrides.json`; a `tier1` row has no `default`.

**Header hash (C1).** The overrides merge (`natives.md` §7.2) changes `natives.json` and therefore the header's
`natives=` hash and the digest's `T` line, but not the trace. Digest guards compare `D` lines only:
`grep '^D' a | cmp - <(grep '^D' b)`. Headers are compared with the `natives=` field stripped:
`diff <(grep '^#' a | sed 's/natives=[0-9a-f]*//') <(grep '^#' b | sed 's/natives=[0-9a-f]*//')`.
