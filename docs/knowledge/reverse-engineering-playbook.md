# Reverse-engineering playbook (from the LOMN port)

Repeatable method for turning an old Windows game binary into a readable, structured decompilation corpus.

Every claim below was measured on `LEGOBionicle.exe` (MSVC6, built 2001-10-22) on 2026-10-06/07. The tools live in `tools/decomp/`.

## 1. Triage the binary before choosing a route

| Check | How | What it decides |
|---|---|---|
| Is there source anywhere? | Search the community org's repos for `.cpp/.h` of the engine (`gh api repos/<org>/<repo>/git/trees/HEAD?recursive=1`) | No source → emulate, rebuild, or decompile |
| What does the exe import? | `nb11.Nb11(...).imports()`, or any PE parser | Size of the platform layer you must replace (LOMN: 16 DLLs, 117 functions) |
| **Is there embedded debug info?** | PE debug directory → look for an `NB11`/`NB10`/`RSDS` CodeView signature | **Decisive.** LOMN had a 2.17 MB NB11 block: 7,057 names, 505 classes, ~4.8k type records, 580 functions with named locals. That makes "decompile + port" realistic. |
| Where does behaviour live? | Count script-callable natives vs engine classes | LOMN: scripts own puzzles/UI/cutscenes; movement, collision, AI, camera and rendering are native |
| Can it be emulated fast enough? | Run it under the best native emulator first (Boxedwine Mac), measure fps from a screen recording | LOMN: ~8 fps on an M1 Pro native, so the web build would be slower still. Emulation route rejected. |

**Measure fps from recordings, not by eye:** `ffmpeg -vf "scale=712:-2,mpdecimate=hi=200:lo=64:frac=0.05,showinfo" -f null -`, count `pts_time` per second, take the median of seconds with more than 2 unique frames. Exclude UI overlays: the screen-capture toolbar caused a fake 42-frame spike.

## 2. Decoding CodeView NB11 (MSVC 6)

The `tools/decomp/nb11.py` reader is the reference implementation. The traps:

- **Symbols are in three places.** `sstGlobalPub` (`S_PUB32` 0x1009), `sstGlobalSym`, and per-module `sstSymbols` (`S_GDATA32` 0x1008 records, which start after a 4-byte signature). Reading only GlobalPub gave 6,994 names instead of 7,057.
- **MSVC6 uses the `_ST` record variants:** `S_GPROC32_ST` 0x100B, `S_BPREL32_ST` 0x1006, `LF_CLASS_ST` 0x1004, `LF_MEMBER_ST` 0x1405, 32-bit type indices.
  - Proc layout: `pParent, pEnd, pNext, len, DbgStart, DbgEnd, typind, off, seg(2), flags, name`.
  - `S_BPREL32` offsets are EBP-relative. Ghidra stack offset = `ebp_off − 4`.
- **Template instances share one stripped name.** 4,819 class definitions collapse to 394 names; `ScAutoRefPtr` has dozens of layouts. Split by layout signature and name each by its first pointee (`ScAutoRefPtr_GcMoveInterpreter`). That turns `void*` members into typed pointers: raw-offset accesses went from 55 to 0 in the sample.
- **Field lists continue through `LF_INDEX` (0x1404).** Follow them, or large classes lose members.
- **Many classes have no type record at all** (e.g. `GcCollisionCylinderSystem`). Type info only exists for types the `/Zi` modules referenced. Recover those layouts from usage, never invent them.
- **Module → original source file:** `sstModule` gives each object file's `.text` ranges; `sstSrcModule` (37 modules here) gives real paths like `C:\Projects\lego\Character\Bosses\GfMudBoss.cpp`. Everything else goes under `_unplaced/<obj>.cpp`.
- **Read initialised globals straight from `.data`.** `ScProcessManager::sFakeDeltaTime` = 0.006666666828095913 (1/150 s through float) is the engine's real fixed step. Never guess constants the binary already holds.

## 3. Ghidra headless pipeline

Order matters:
1. **Import** with pre-scripts `ApplyCVTypes2` (structs) → `ApplyCVSymbols` (labels as IMPORTED, so the MSVC demangler runs).
2. **Auto-analysis.**
3. **Post-scripts** `ApplyCVLocals` (params/locals) → `ApplyD3D8Types` (typed COM vtables).
4. **Export** with `ExportCorpus` from a cached, read-only project.

Measured results:
- 5,429 of 5,618 functions keep real names.
- `make decomp` takes about 6 min on an M1 Pro; analysis runs once per exe sha (`analysis.ok` marker).
- **Deterministic:** two exports and a fresh re-analysis gave 0 differing files.
- **Typed COM vtables pay off twice:** the decompiler prints `mD3DDevice->lpVtbl->SetRenderState(...)`, and the platform surface becomes countable. LOMN calls 30 of 97 `IDirect3DDevice8` methods, all fixed-function. Method order comes from mingw-w64 `d3d8.h`, pinned by commit and sha256.

Traps:
- **`analyzeHeadless` exits 0 even when a script fails to compile or throws.** Scan the log for `REPORT SCRIPT ERROR` and `Script compile failed`.
- **Decompiler limits hide your most important functions.** `GcLegoCamera::Process` (37 KB of code) failed with `process: timeout` at 120 s, then with `Response buffer size exceeded` at the default 50 MB payload. With `DecompileOptions.setMaxPayloadMBytes(1024)` and a 900 s limit it decompiles to 3,372 lines in about 5 minutes.
- **Decompile in parallel.** `ghidra.app.decompiler.parallel.ParallelDecompiler` with a `DecompilerCallback` (one decompiler process per core), results sorted by entry address. Measured 325 s vs ~20 min sequential on 8 cores, byte-identical output. Wall time then equals the slowest single function (the camera, ~5 min).
- **Per-function timeouts make output load-dependent.** One run under load lost a second function: `decompiled_ok` 5616, `member_access` −432. Use a generous default (300 s) plus one 4× retry, and keep metric floors that fail when this happens.
- **Metric floors must measure Ghidra's side**, not the inputs you fed it: script log counters (labels/structs/locals applied) plus corpus counters (member accesses, raw offsets). Counting your own `types.txt` proves nothing about whether Ghidra applied it.
- **x87 compares decompile as status-word bit soup** (`NAN(a) || NAN(b)) << 10) >> 8 … == 0x40`). Rewrite them by evaluating the LT/EQ/GT truth table of C0/C2/C3 (`tools/decomp/x87fix.py`). That rewrote 695 sites, with 80 left in unhandled forms.
- **Garbled by-value struct arguments** (`in_stack_*`) come from wrong callee signatures (here `ScOSIVirtualMachine::Call` taking `ScOSIVariant` by value). Fix the signature; don't hand-edit the output.

## 4. Readability metrics (track them; don't eyeball)

`tools/decomp/metrics.py` counts, over the whole corpus: member accesses, raw-offset dereferences, unnamed `param_N` / `local_XX`, remaining x87 noise, and `in_stack` artefacts.

Measured before → after applying NB11 v2 types/locals and the x87 pass (same 36 functions):

| Metric | Before | After |
|---|---|---|
| Raw-offset dereferences | 55 | 0 |
| Member accesses | 457 | 595 |
| Unnamed params | 125 | 74 |
| Unnamed locals | 791 | 583 |
| x87 noise | 6 | 0 |

## 5. Hygiene

- **Commit only what you author.** Anything derived from the copyrighted binary (corpus, generated headers) is regenerated by `make decomp` into the gitignored `gen/`.
- **Never execute the original binary in automation.** Parse it as data. Original-game runs are launched by a human.
- **Validate tooling from a path containing spaces.** The real checkout lives under `Application Support`. The first validation, done from `/private/tmp`, missed two quoting bugs.
