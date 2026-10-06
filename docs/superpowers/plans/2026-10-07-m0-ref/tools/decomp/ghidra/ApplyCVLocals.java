// Post-analysis: name/type EBP-relative params (+off) and locals (-off) from F/V lines.
// Ghidra stack offsets are relative to the entry SP (return address at 0): ghidra = ebpOffset - 4.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.SourceType;
import java.nio.file.*; import java.util.*;
public class ApplyCVLocals extends GhidraScript {
  @Override public void run() throws Exception {
    DataTypeManager dtm = currentProgram.getDataTypeManager();
    Map<String, Structure> structs = new HashMap<>();
    dtm.getAllStructures().forEachRemaining(s -> structs.put(s.getName(), s));
    CVTypeSpec ts = new CVTypeSpec(dtm, structs);
    int params = 0, locals = 0, fail = 0, funcs = 0; Function f = null;
    for (String l : Files.readAllLines(Paths.get(System.getenv("CV_TYPES")))) {
      String[] p = l.split(" ", 5);
      if (p[0].equals("F")) { f = getFunctionAt(toAddr(Long.parseLong(p[1].substring(2), 16))); if (f != null) funcs++; continue; }
      if (!p[0].equals("V") || f == null) continue;
      int off = Integer.parseInt(p[1]) - 4, size = Integer.parseInt(p[3]); String name = p[4];
      DataType dt = ts.resolve(p[2], size);
      try {
        if (off >= 0) {
          Parameter hit = null;
          for (Parameter q : f.getParameters()) if (q.isStackVariable() && q.getStackOffset() == off) hit = q;
          if (hit == null) throw new Exception("no param");
          hit.setName(name, SourceType.IMPORTED);
          if (dt != null && dt.getLength() == hit.getLength()) hit.setDataType(dt, SourceType.IMPORTED);
          params++;
        } else {
          StackFrame fr = f.getStackFrame();
          for (int o = off; o < off + Math.max(1, size); o++) { Variable v = fr.getVariableContaining(o); if (v != null) fr.clearVariable(v.getStackOffset()); }
          fr.createVariable(name, off, dt != null ? dt : Undefined4DataType.dataType, SourceType.IMPORTED); locals++;
        }
      } catch (Exception e) { fail++; }
    }
    println("CV locals: functions=" + funcs + " params=" + params + " locals=" + locals + " failed=" + fail);
  }
}
