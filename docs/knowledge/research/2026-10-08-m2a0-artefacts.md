# M2a-0: the remaining `in_stack_` / `in_EAX` artefacts in the VM and `CreateVariant` (2026-10-08)

Spec A1 asks for the before and after counts of the decomp artefacts in `ScOSIVirtualMachine::*` and `CreateVariant`,
"with every remaining artefact listed". This file is that list. Names, addresses and counts only; no decompiled code.

**Method (EXECUTED).** The regex is `metrics.py`'s `in_stack_artifact` pattern, `\bin_(?:stack_\w+|E[A-Z]X)\b`
(`tools/decomp/metrics.py`), imported rather than copied. A scratch script splits every `gen/decomp/corpus/**/*.cpp`
file at its `// 0x<addr> <name>` function headers and counts the matches per function. A function is in scope when
its name starts with `ScOSIVirtualMachine::` or is exactly `CreateVariant`.
- **After** = `gen/decomp`, the M2a-0 re-run corpus: 6,940 functions.
- **Before** = `lomn-web/gen/decomp`, the corpus before the re-run: 5,618 functions.

## Totals, before → after

| scope | functions | hits before | hits after | functions with a hit, before → after |
|---|---|---|---|---|
| whole corpus (`metrics.json` `in_stack_artifact`) | 5,618 → 6,940 | 3,649 | 3,209 | n/a |
| `ScOSIVirtualMachine::*` | 110 → 110 | 205 | 205 | 76 → 76 |
| `CreateVariant` (all overloads) | 63 → 63 | 452 | 2 | 60 → 2 |

- The VM count did not change at all, and the per-function counts are identical before and after. The A11
  by-value signature fix removed the `CreateVariant` artefacts, not the VM ones.
- The consult (`2026-10-08-m2-osi-vm-consult.md` §1.6) quoted "CreateVariant 274" and 3,161 in total with its own
  counter. With `metrics.py`'s regex the before figures are 452 and 3,649. The VM figure, 205, agrees.

## Likely cause, per class

Each class was read in the asm (`objdump -d --x86-asm-syntax=intel` on the exe as data) at the addresses named. The
assignment of all 78 functions to a class is by offset sign and callee; it is EXECUTED as a classification and
INFERRED as a cause beyond the samples.

- **A: a class-typed parameter passed by hidden reference, declared by value.** 191 hits in 71 VM functions; all are
  negative `in_stack_ffffff..` offsets.
  - `ScOSIStack::Push(ScOSIVariant)` takes a *pointer* to an 8-byte temporary: Halt 0x603840 does `lea eax,
    [ebp-8]; push eax; call 0x5fd0c0`, and Push reads `mov edi, [esp+0x10]` and then `[edi]`.
  - Ghidra declares the parameter by value. It models an 8-byte argument copy whose two bytes the code never writes,
    and names them `in_stack_…`.
  - 67 of the 71 functions call `Push` 0x5fd0c0 or `CallWithReturn` 0x60b450, whose 10 variant arguments are passed
    the same way. Of the other four:
    - `BCLJumpRelative`/`BCLJumpAbsolute` (0x600ae0/0x600c30) pass the marker to `ScOSIStack::Insert` (0x600ba0);
    - `BCLSetVariableValue` (0x607b50) passes the value to `ScOSIStack::PushIntoFrame` (0x607c00);
    - `BCLSetArrayValue` (0x5fd360) reads the high half of a `Pop` result. That is the by-value *return* of class B
      (INFERRED).
- **B: a by-value class return.** 10 hits in 3 functions: `ConvertStringIDToDynamicString` 0x60b880, `ConvertVariant`
  0x60b9d0 and `ConstructObject` 0x60be00, all positive offsets.
  - Each returns `ScOSIVariant` through a hidden return pointer at entry+4: 0x60b880 stores type 0x16 through
    `[ebp+8]`, and 0x60be00 reads the class index at `[ebp+0xc]`.
  - Ghidra's `__thiscall` signature lacks that pointer, so each real parameter appears 4 bytes further on as
    `in_stack_00000008`/`0c`.
- **C: a variadic `__cdecl` member.** 4 hits in `Error` 0x60be80 and `Message` 0x60bef0.
  - The mangled names say `__cdecl` with `this` on the stack. Ghidra applied `__thiscall`, so the format string at
    entry+8 becomes `in_stack_00000008`.
  - The asm builds the `va_list` from `[ebp+0xc]`.
- **D: a class-typed parameter passed by hidden reference, in `CreateVariant`.** 2 hits, at 0x447890 and 0x46de00.
  - `CreateVariant(StRGBABytes)` (0x447890) loads `ebx = [esp+0x10]` and reads the bytes `[ebx]..[ebx+3]`: it gets a
    pointer, not 4 bytes by value.
  - `CreateVariant(ScFixedStringTemplate<255>)` (0x46de00) pushes `[ebp+0xc]` as the source pointer of the string
    copy.
  - 0x407df0 (`ScIdentifier`: `mov edx, [esp+0xc]; mov edx, [edx]`) and 0x6159e0 (`ScOSIVariant`: `mov ebx,
    [esp+0x10]; mov dx, [ebx]`) do the same. They have no artefact left, but their decomp misreads the argument.

None of the four classes changes a fact in `docs/osi/opcodes.md`: every row whose decomp shows one of these artefacts
was read from the asm (that file, "How the generated columns are derived").

## Every remaining artefact (after; 78 functions, 207 hits)

| function | address | hits | artefact names (×count) | class |
|---|---|---|---|---|
| `CreateVariant` | 0x447890 | 1 | in_stack_00000009 | D |
| `CreateVariant` | 0x46de00 | 1 | in_stack_00000009 | D |
| `ScOSIVirtualMachine::BCLCreateArray` | 0x5fd070 | 2 | in_stack_ffffffee×2 | A |
| `ScOSIVirtualMachine::BCLElementsInArray` | 0x5fd2d0 | 2 | in_stack_ffffffda×2 | A |
| `ScOSIVirtualMachine::BCLSetArrayValue` | 0x5fd360 | 2 | in_stack_ffffffc0×2 | A |
| `ScOSIVirtualMachine::BCLCreateObject` | 0x5fe530 | 2 | in_stack_ffffffec×2 | A |
| `ScOSIVirtualMachine::BCLGetMemberFunction` | 0x5fe590 | 2 | in_stack_ffffffde×2 | A |
| `ScOSIVirtualMachine::BCLMemberFunctionArgumentCheck` | 0x5feb90 | 2 | in_stack_ffffffe0×2 | A |
| `ScOSIVirtualMachine::BCLGetThisMemberValue` | 0x5fecf0 | 2 | in_stack_ffffffe0×2 | A |
| `ScOSIVirtualMachine::BCLGetThisMemberFunction` | 0x5fedb0 | 2 | in_stack_ffffffe0×2 | A |
| `ScOSIVirtualMachine::BCLGetMemberValueFromString` | 0x5fee40 | 3 | in_stack_fffffe30×3 | A |
| `ScOSIVirtualMachine::BCLGetMemberFunctionFromString` | 0x5ff210 | 2 | in_stack_fffffe80×2 | A |
| `ScOSIVirtualMachine::BCLSetRedValue` | 0x5fff30 | 2 | in_stack_ffffffce×2 | A |
| `ScOSIVirtualMachine::BCLSetGreenValue` | 0x5ffff0 | 2 | in_stack_ffffffce×2 | A |
| `ScOSIVirtualMachine::BCLSetBlueValue` | 0x6000b0 | 2 | in_stack_ffffffce×2 | A |
| `ScOSIVirtualMachine::BCLSetAlphaValue` | 0x600170 | 2 | in_stack_ffffffce×2 | A |
| `ScOSIVirtualMachine::BCLGetRedValue` | 0x600230 | 2 | in_stack_ffffffde×2 | A |
| `ScOSIVirtualMachine::BCLGetGreenValue` | 0x6002a0 | 2 | in_stack_ffffffde×2 | A |
| `ScOSIVirtualMachine::BCLGetBlueValue` | 0x600310 | 2 | in_stack_ffffffde×2 | A |
| `ScOSIVirtualMachine::BCLGetAlphaValue` | 0x600380 | 2 | in_stack_ffffffde×2 | A |
| `ScOSIVirtualMachine::BCLJumpRelative` | 0x600ae0 | 2 | in_stack_ffffffda×2 | A |
| `ScOSIVirtualMachine::BCLJumpAbsolute` | 0x600c30 | 2 | in_stack_ffffffda×2 | A |
| `ScOSIVirtualMachine::BCLReturn` | 0x600cf0 | 4 | in_stack_ffffffc2×4 | A |
| `ScOSIVirtualMachine::BCLCallGameFunctionFromString` | 0x602160 | 3 | in_stack_fffffb38×3 | A |
| `ScOSIVirtualMachine::BCLHalt` | 0x603840 | 2 | in_stack_fffffff6×2 | A |
| `ScOSIVirtualMachine::BCLAdd` | 0x603ff0 | 4 | in_stack_ffffff68×4 | A |
| `ScOSIVirtualMachine::BCLSubtract` | 0x6042b0 | 3 | in_stack_ffffff9c×3 | A |
| `ScOSIVirtualMachine::BCLMultiply` | 0x604450 | 3 | in_stack_ffffff9c×3 | A |
| `ScOSIVirtualMachine::BCLDivide` | 0x6045f0 | 3 | in_stack_ffffff9c×3 | A |
| `ScOSIVirtualMachine::BCLPower` | 0x6047f0 | 4 | in_stack_ffffff84×4 | A |
| `ScOSIVirtualMachine::BCLModulus` | 0x604a00 | 2 | in_stack_ffffffce×2 | A |
| `ScOSIVirtualMachine::BCLGreaterOrEqual` | 0x604ad0 | 3 | in_stack_ffffffa8×3 | A |
| `ScOSIVirtualMachine::BCLLessOrEqual` | 0x604c80 | 3 | in_stack_ffffffa8×3 | A |
| `ScOSIVirtualMachine::BCLGreaterThan` | 0x604e30 | 3 | in_stack_ffffffa8×3 | A |
| `ScOSIVirtualMachine::BCLLessThan` | 0x604fd0 | 3 | in_stack_ffffffa8×3 | A |
| `ScOSIVirtualMachine::BCLEqualTo` | 0x605180 | 9 | in_stack_ffffff68×9 | A |
| `ScOSIVirtualMachine::BCLAnd` | 0x6055b0 | 2 | in_stack_ffffffce×2 | A |
| `ScOSIVirtualMachine::BCLOr` | 0x605670 | 2 | in_stack_ffffffce×2 | A |
| `ScOSIVirtualMachine::BCLBitwiseAnd` | 0x605730 | 4 | in_stack_ffffffce×2, in_stack_ffffffd4×2 | A |
| `ScOSIVirtualMachine::BCLBitwiseOr` | 0x6057e0 | 4 | in_stack_ffffffce×2, in_stack_ffffffd4×2 | A |
| `ScOSIVirtualMachine::BCLBitwiseXor` | 0x605890 | 4 | in_stack_ffffffce×2, in_stack_ffffffd4×2 | A |
| `ScOSIVirtualMachine::BCLShiftLeft` | 0x605940 | 4 | in_stack_ffffffce×2, in_stack_ffffffd4×2 | A |
| `ScOSIVirtualMachine::BCLShiftRight` | 0x6059f0 | 4 | in_stack_ffffffce×2, in_stack_ffffffd4×2 | A |
| `ScOSIVirtualMachine::BCLBitwiseNot` | 0x605aa0 | 3 | in_stack_ffffffd6×3 | A |
| `ScOSIVirtualMachine::BCLNot` | 0x605b40 | 3 | in_stack_ffffffd6×3 | A |
| `ScOSIVirtualMachine::BCLSwap` | 0x606390 | 3 | in_stack_ffffffca×3 | A |
| `ScOSIVirtualMachine::BCLPull` | 0x606420 | 2 | in_stack_fffffff0×2 | A |
| `ScOSIVirtualMachine::BCLDup` | 0x606470 | 2 | in_stack_fffffff0×2 | A |
| `ScOSIVirtualMachine::BCLDupN` | 0x6064b0 | 2 | in_stack_ffffffe4×2 | A |
| `ScOSIVirtualMachine::BCLPushConstant0` | 0x606510 | 2 | in_stack_fffffff6×2 | A |
| `ScOSIVirtualMachine::BCLPushConstanti8` | 0x606540 | 2 | in_stack_fffffff2×2 | A |
| `ScOSIVirtualMachine::BCLPushConstanti16` | 0x606580 | 2 | in_stack_ffffffee×2 | A |
| `ScOSIVirtualMachine::BCLPushConstanti32` | 0x606600 | 2 | in_stack_fffffff2×2 | A |
| `ScOSIVirtualMachine::BCLPushConstantf32` | 0x606680 | 2 | in_stack_fffffff2×2 | A |
| `ScOSIVirtualMachine::BCLPushConstantString` | 0x606700 | 2 | in_stack_ffffffee×2 | A |
| `ScOSIVirtualMachine::BCLPushConstantColor5551` | 0x606760 | 2 | in_stack_ffffffee×2 | A |
| `ScOSIVirtualMachine::BCLPushConstantColor8888` | 0x6067c0 | 2 | in_stack_fffffff2×2 | A |
| `ScOSIVirtualMachine::BCLPushNothing` | 0x606840 | 2 | in_stack_fffffff6×2 | A |
| `ScOSIVirtualMachine::BCLConvertToString` | 0x606f60 | 2 | in_stack_ffffffd2×2 | A |
| `ScOSIVirtualMachine::BCLConvertToFloat` | 0x606fe0 | 2 | in_stack_ffffffd2×2 | A |
| `ScOSIVirtualMachine::BCLConvertToInteger` | 0x607060 | 2 | in_stack_ffffffd2×2 | A |
| `ScOSIVirtualMachine::BCLIsInteger` | 0x6070e0 | 2 | in_stack_ffffffe2×2 | A |
| `ScOSIVirtualMachine::BCLIsFloat` | 0x607140 | 2 | in_stack_ffffffe2×2 | A |
| `ScOSIVirtualMachine::BCLIsString` | 0x6071a0 | 3 | in_stack_ffffffda×3 | A |
| `ScOSIVirtualMachine::BCLIsAnObject` | 0x607220 | 2 | in_stack_ffffffe2×2 | A |
| `ScOSIVirtualMachine::BCLIsGameObject` | 0x607280 | 2 | in_stack_ffffffe2×2 | A |
| `ScOSIVirtualMachine::BCLIsArray` | 0x6072f0 | 2 | in_stack_ffffffe2×2 | A |
| `ScOSIVirtualMachine::BCLGetObjectClassID` | 0x607350 | 3 | in_stack_ffffffd6×3 | A |
| `ScOSIVirtualMachine::BCLCreateStackVariables` | 0x607b00 | 2 | in_stack_ffffffe8×2 | A |
| `ScOSIVirtualMachine::BCLSetVariableValue` | 0x607b50 | 2 | in_stack_ffffffd8×2 | A |
| `ScOSIVirtualMachine::BCLGetVariableValue` | 0x607c50 | 2 | in_stack_ffffffe6×2 | A |
| `ScOSIVirtualMachine::Call` | 0x60b320 | 14 | in_stack_ffffff88×2, in_stack_ffffff90×2, in_stack_ffffff92×2, in_stack_ffffff94×2, in_stack_ffffff9a×2, in_stack_ffffffa2×2, in_stack_ffffffaa×2 | A |
| `ScOSIVirtualMachine::CallWithReturn` | 0x60b450 | 2 | in_stack_ffffffca×2 | A |
| `ScOSIVirtualMachine::ConvertStringIDToDynamicString` | 0x60b880 | 2 | in_stack_00000008×2 | B |
| `ScOSIVirtualMachine::ConvertVariant` | 0x60b9d0 | 4 | in_stack_0000000a, in_stack_0000000c×3 | B |
| `ScOSIVirtualMachine::ConstructObject` | 0x60be00 | 4 | in_stack_00000006, in_stack_00000008×3 | B |
| `ScOSIVirtualMachine::Error` | 0x60be80 | 2 | in_stack_00000008×2 | C |
| `ScOSIVirtualMachine::Message` | 0x60bef0 | 2 | in_stack_00000008×2 | C |
