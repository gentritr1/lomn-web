// Post-analysis: ScOSIVariant (8 bytes) is returned through a hidden pointer, but Ghidra's __cdecl model returns an
// 8-byte value in EDX:EAX, so every caller of CreateVariant loses the value it wraps (M2a-0 probe 1, finding A11).
// Retype every function named CreateVariant (63 overloads) with custom storage:
//   ScOSIVariant * __cdecl CreateVariant(ScOSIVariant *__return_storage_ptr__ /*Stack[0x4]*/, <original params>)
// returning the same pointer in EAX; the original parameters keep their names and types and move up one 4-byte slot.
// The natives themselves are NOT retyped (probe 1: the registered fn-pointer type shifts every parameter by one, and
// retyping them loses the inferred float and object types). Prints "SRET retyped=<n> failed=<k>"; run_decomp fails the
// run when failed > 0.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.*;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.*;
import ghidra.program.model.listing.Function.FunctionUpdateType;
import ghidra.program.model.symbol.SourceType;
import java.util.*;
public class ApplySret extends GhidraScript {
  @Override public void run() throws Exception {
    DataTypeManager dtm = currentProgram.getDataTypeManager();
    DataType var = dtm.getDataType(CategoryPath.ROOT, "ScOSIVariant");  // the CV struct from ApplyCVTypes2
    Register eax = currentProgram.getRegister("EAX");
    if (!(var instanceof Structure) || var.getLength() != 8 || eax == null)
      throw new IllegalStateException("ScOSIVariant (8-byte CV struct) or EAX missing: " + var);
    DataType sretPtr = new PointerDataType(var, 4, dtm);
    int retyped = 0; List<String> failed = new ArrayList<>();
    for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
      if (!f.getName().equals("CreateVariant")) continue;
      try {
        List<Variable> ps = new ArrayList<>();
        int off = 4;  // Ghidra stack offsets are relative to the entry SP: the return address is at 0
        ps.add(new ParameterImpl(Function.RETURN_PTR_PARAM_NAME, sretPtr, new VariableStorage(currentProgram, off, 4), currentProgram));
        off += 4;
        for (Parameter p : f.getParameters()) {
          if (p.isAutoParameter()) continue;
          int len = p.getDataType().getLength();
          if (len <= 0) throw new IllegalStateException("parameter " + p.getName() + " has no fixed size");
          ps.add(new ParameterImpl(p.getName(), p.getDataType(), new VariableStorage(currentProgram, off, len), currentProgram));
          off += (len + 3) & ~3;
        }
        f.updateFunction("__cdecl", new ReturnParameterImpl(sretPtr, new VariableStorage(currentProgram, eax), currentProgram),
                         ps, FunctionUpdateType.CUSTOM_STORAGE, true, SourceType.USER_DEFINED);
        retyped++;
        println("sret 0x" + f.getEntryPoint() + " " + f.getSignature().getPrototypeString());
      } catch (Exception e) { failed.add("0x" + f.getEntryPoint() + " " + e); }
    }
    for (String s : failed) println("SRET failed " + s);
    println("SRET retyped=" + retyped + " failed=" + failed.size());
  }
}
