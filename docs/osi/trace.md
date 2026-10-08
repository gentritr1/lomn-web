# OSI VM trace format (M2a-1)

Source: the M2a-1 design consult §2 (`docs/knowledge/research/2026-10-08-m2a1-design-consult.md`), with the controller's
rulings applied: D7–D10 as ratified in the M2 spec, and **D8′** (2026-10-08: x87 precision control is VM state, which
adds the `P` record). Every implementation (C++ native, C++ wasm32 under node, the Python oracle) writes these exact
bytes; a run fails if any two differ. Nothing is "normalised" after the fact.

## Format

**Text**, LF-terminated, ASCII only, one record per line, fixed field order and widths, no JSON. JSON key order and float
printing are the two ways a comparison tool could hide a difference; `cmp` on bytes cannot.

A variant `<v>` is printed as `TTTT:VVVVVVVV`:
- `TTTT` is the u16 type in 4 lowercase hex digits (`000f` nothing, `0010` int, `0200` by-ref);
- `VVVVVVVV` is the u32 value in 8 lowercase hex digits: int bits, f32 bits, string-table index, colour, handle (D7) or PC;
- `----:--------` means empty.

```
# lomn-osi-trace 1 script=<sha256 of the .osi> natives=<sha256 of natives.json> mods=<sha256> types=<sha256> fpu=pc24
C <name> <argc_declared> <a0> … <a9>                 engine->script call entry (after FindFunction; name as passed)
M <name>                                             Message "Unable to locate function" (no C line follows)
I <pc:08x> <op:02x> <depth:4d> <frame:4d> <top:v>    one line BEFORE each instruction executes; op is the pristine byte (0x82 never 0x84)
N <pc:08x> <cls>.<fn> <argc> <a0:v> … <aN-1:v> -> <r:v>   emitted after the native returns; cls/fn as in the string table, case preserved
S <h:08x> <len:3d> <hex bytes>                       the text of string handle h when it is passed to or returned by a native (one S per 0x16 argument/return, before its N line)
A <k> <id:08x> [<class:04x>|<n:08x>|<len:3d>|<type:02x>]   allocation: k in {o,a,s,g}; the bracket shows class id / element count / length / game type
F <k> <id:08x>                                       free (refcount reached zero or string deleted)
P <pc:08x> 64                                        x87 precision control switched to 64 bits (D8'); pc = the instruction that caused it
R <v>                                                 the engine call's result, after the marker pop
E <pc:08x> <op:02x> <depth:4d> "<message bytes, \" and \\ escaped>"   an error; the line after it is the next C/M or EOF
K <frame:6d> <t:08x> <dt:08x> <n:08x>                frame header for A6: fake time and dt as f32 bits, Process counter
```

### The `P` record (D8′)

- The header's `fpu=pc24` is the **initial** precision mode of the run (`fpu_pc` = 24, the `CreateDevice` reading).
- `P <pc> 64` is written once, at the moment `fpu_pc` changes from 24 to 64, **before the next `I` line**. `pc` is the
  address of the instruction during which the switch happened:
  - a string → float conversion in `ConvertVariant` (0xb1 on a 0x11/0x16, or a float-declared native argument given a
    0x11/0x16 in 0x82/0x83/0x84): the `P` line comes after that instruction's other records (`N`, `S`, `A`/`F`);
  - a call of a native whose registry row has `sets_fpu_pc64` (`gcscripttools.stringtofloat`): after its `N` line;
  - an error (the landing-pad `fninit`, D8′/D10): directly after the `E` line, with the `E` line's pc.
- The mode never returns to 24 within a run, so a trace holds at most one `P` line. A run that starts at 64 (not used in
  M2a-1) would say `fpu=pc64` in the header.

## Rules that make three implementations byte-identical

- **Floats bit-exact:** always the 8 hex digits of the IEEE-754 binary32 pattern (`std::memcpy` into `uint32_t`; Python
  `struct.unpack('<I', struct.pack('<f', x))[0]` on a value already rounded to f32). NaN is one pattern by D9
  (`ffc00000`). `-0.0` prints `80000000`. `K` prints time/dt as the f32 the script receives.
- **Ints:** two's complement u32 (`-1` = `ffffffff`); Python masks with `& 0xffffffff` everywhere a value is stored.
- **Strings:** the `S` line carries bytes as hex (latin-1, never decoded), so 0x80+ bytes and NULs are exact.
- **Top of stack** on `I` lines: the slot below `top_`, or `----:--------` at depth 0. `frame` is the frame-base index
  (0 when none).
- **Alloc/free order** is observable and must match: object and array destructors dispose back to front (table),
  `0x52` pops with dispose from the top down, `CallWithReturn` disposes surplus arguments in index order 0..9. The Python
  oracle models exactly this; a disagreement here is the ownership blind spot the spec names, and it is the point of the
  `A`/`F` lines.
- **Why byte-identical across three implementations is achievable:**
  - no host pointers (D7: handles from per-kind monotonic counters starting at 1);
  - no printf of floats (`%08x` of bits only). `%f` is used by the VM semantics of `0xb0`, where the text goes through an
    `S` line as bytes. Consult M21 shows Python's `%f` equals libc's on f32-widened doubles for the ties tried; it is
    still UNVERIFIED against the original's CRT, so it stays a listed blind spot;
  - canonical NaN (D9);
  - `-ffp-contract=off` is already set for native == wasm32 (`engine/CMakeLists.txt`);
  - `std::fma` only inside the mixed-arithmetic helpers (wasm32 emulates it in software, correctly rounded);
  - the precision mode is explicit state with its own record (`P`, D8′), never host FPU state.
- **Digest mode** (`--digest`): one line per `K` block, `D <frame:6d> <lines:8d> <fnv1a64 of all bytes of the
  block:016x>`; the header line and a final `T <total lines> <fnv1a64 of the whole trace>` are included. The digest is
  computed over the same bytes the full trace would contain, so "digests equal" ⟺ "full traces equal" up to a 64-bit hash
  collision; a mismatch names the frame to re-run with `--trace-frames a:b`.
  - Size bound: at the static upper bound (consult M13, 122k instructions/frame) 600 frames would be 73 M `I` lines, about
    3 GB per implementation; the digest is 600 lines.
  - The likely size (ESTIMATE from consult M12/M18: a frame walks the screen-list dispatch, not the whole program) is
    2k–20k lines per frame, 50–500 MB for 600 frames. The full-trace window defaults to frames 0–2 plus any frame the
    digest flags.

## The native-return replay log (A6/A9)

The C++ run writes `natives.log` next to the trace:

```
<seq:08d> <cls>.<fn> <argc> -> <r:v> [S <len> <hex>] [W <i> <v>]…
```

`W` records what the native wrote into a by-ref slot (0x200) before the pop. Python's `NativeRegistry.replay_from(log)`
replaces every native body:
- On call `seq` it asserts that `cls.fn/argc` match the log; otherwise it raises
  `VmError("replay desync at seq N: expected a.b/2, got c.d/3")`. That is a VM-semantics finding, since the call order is
  VM behaviour.
- It returns the logged variant. A game handle `g:<id>` is re-created by asking the heap for the **next** id and
  asserting that it equals the logged one (handles are allocated in the same order on both sides, D7).
- It applies the `W` writes.
- It applies the row's `sets_fpu_pc64` exactly as a live call would (the flag is registry data, not native behaviour), so
  the `P` line appears at the same place.

So any A6/A9 disagreement is VM semantics, never native semantics. For A6 the log is produced by the typed stubs (D11),
so Python could also just run the same stub policy; the replay path is used anyway so that A9 (M2a-2, real natives) needs
no new mechanism.
