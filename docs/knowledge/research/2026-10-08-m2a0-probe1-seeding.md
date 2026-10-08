# M2a-0 probe 1: seeding Ghidra recovers the native registry (2026-10-08)

This is the controller's record of the subagent's hand-back; subagents could not write report files. The exe was parsed as data only. The cached Ghidra project was cloned and never written.

**Controller re-check (EXECUTED, own count):**
- The old corpus holds **135** `ScOSISystem::RegisterFunction` call sites: 139 matches minus 4 in the definition file `0x5fab30`. The spec's first version said 134.
- The seeded export holds 6,944 functions, with **1,056** call sites inside 70 functions.

## Facts

| Fact | Value |
|---|---|
| `RegisterFunction` arguments (from `0x5fab30` and `BCLCallGameFunctionFromString` `0x602160`) | `thiscall(this, class&, func&, fnptr, MAX @+0xc, MIN @+0xd, type0..type9)` |
| `.CRT` initializer pointers | 391, at `0x76f000`, 2,048 B, in ascending address order |
| The 215 `&LAB_` targets in the old corpus | 126 OSI natives, 86 `GcEmitterFunc` particle callbacks (`spFuncTable`), 3 unrelated |
| Native bodies already present as functions in the cache | 79 of 1,056 |
| Seeded run: 1,447 seeds (391 `.CRT` + 1,056 fn pointers) | 1,321 functions created; corpus 5,618 → 6,944; 6,944/6,944 decompiled |
| Registrations | 1,056 / 1,056 enclosed; class name, function name, fn pointer and max/min all constant; no names built at run time; 1,056 distinct bodies |
| Distinct (class, function) pairs, case-folded | 1,049 across 71 classes |
| Duplicates | 7 `GcHudSprite` functions registered twice with different bodies (Create, SetZDepth, SetPosition, EnableTransparency, SetTransparency, EnableAlphaChannel, SetLinear), from `0x5277d0` (`.CRT` index 227) and `0x536910` (index 242). Which copy wins is UNVERIFIED; INFERRED that the first insert wins. |
| Parameter types | 9,389 literals, 395 `theOSIType*` globals, 776 `GetVariantType<T>::GetIt()` calls. Type ids are assigned lazily at run time, so the registry must replay `.CRT` order. |

**Fn-pointer extractor** (raw bytes, no Ghidra): take the last `push imm32` of a `.text` address before each call site. Results:
- 988 sites have exactly one candidate.
- The first site in each initializer has 8–13 candidates (`ScInitFunc` pushes).
- The extractor agrees with the decomp on 1,054 / 1,054 numeric pointers.

## Native body shapes (54 sampled; S3 = 20 drawn uniformly from all 1,056)

| Shape | Uniform sample S3 | All 54 |
|---|---|---|
| One engine call, then an inline "nothing" variant | 7 | 20 |
| One engine call (or new + ctor), then `CreateVariant(result)` | 6 | 13 |
| Inline member read (often guarded), then `CreateVariant` | 3 | 8 |
| Inline member, flag or static write | 4 | 11 |
| Virtual call | 0 | 2 |
| Thunk, jump table, multi-call logic or decompile failure | 0 | 0 |

Every body is a single operation of 29 lines or fewer. The stub/binding generator therefore needs these shapes, not one.

## A11 (sret) finding

- **The problem:** `ScOSIVariant` (8 bytes) is returned through a hidden return pointer. Ghidra assumes EDX:EAX instead, so every `CreateVariant` call in a native loses the value it wraps.
- **The fix:** retyping the 63 `CreateVariant` overloads as `ScOSIVariant*(ScOSIVariant* sret, …)` recovered the value in 12/12 sampled natives.
- **What not to do:** don't apply the registered function-pointer type to the natives (it shifts every parameter by one), and don't retype the natives themselves (that loses the inferred float and object types).
- **Next:** 160 functions return a class by value (73 `ScOSIVariant`, 37 `ScIdentifier`, 13 `ScVector`, 13 `ScPoint3d`, …). Whether each class uses sret must be checked per class.
- **Still broken after A11:** 4–6 double/struct-by-value artefacts remain.

## Pipeline change for M2a-0

1. A data-only step writes `gen/decomp/seed_addrs.txt`: the `.CRT` pointers plus the fn pointers from the extractor, and also the site → fn-pointer map.
2. `SeedFunctions.java` runs as a pre-script after `ApplyCVSymbols.java`. Add it to `ANALYSIS_SCRIPTS`, and add the seed file to `analysis_key()`; otherwise the cached analysis is reused and the seeds never apply.
3. An A11 post-analysis script retypes the sret functions.
4. `seedset.py` does **not** seed Ghidra; it only tags functions after export (the consult's wording was misleading).

**Measured cost:** a 896 s wall run under heavy load (loadavg 7.7 → 28.8, swapping), including one function that timed out at 300 s and was retried sequentially. Disk: +16 MB project, 34 MB export. ESTIMATE: 8–25 min for the full seeded `make decomp`.

A plain second analysis pass over the cached project created 991 functions but still left 341 `.CRT` initializers unmade, so the `.CRT` seeds are mandatory.
