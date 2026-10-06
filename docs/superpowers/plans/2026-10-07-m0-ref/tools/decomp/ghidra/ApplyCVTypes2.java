// Pre-analysis: create all structures from types2.txt (S/M lines), with typed pointers and template instances.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.*;
import java.nio.file.*; import java.util.*;
public class ApplyCVTypes2 extends GhidraScript {
  @Override public void run() throws Exception {
    DataTypeManager dtm = currentProgram.getDataTypeManager();
    Map<String, Structure> structs = new HashMap<>();
    List<String> lines = Files.readAllLines(Paths.get(System.getenv("CV_TYPES")));
    for (String l : lines) if (l.startsWith("S ")) {
      String[] p = l.split(" "); int sz = Integer.parseInt(p[2]); if (sz <= 0) continue;
      structs.put(p[1], (Structure) dtm.addDataType(new StructureDataType(CategoryPath.ROOT, p[1], sz, dtm), DataTypeConflictHandler.REPLACE_HANDLER));
    }
    CVTypeSpec ts = new CVTypeSpec(dtm, structs);
    int ok = 0, fail = 0; Structure cur = null;
    for (String l : lines) {
      String[] p = l.split(" ", 5);
      if (p[0].equals("S")) { cur = structs.get(p[1]); continue; }
      if (!p[0].equals("M") || cur == null) { if (p[0].equals("F")) cur = null; continue; }
      int off = Integer.parseInt(p[1]), size = Integer.parseInt(p[3]);
      DataType dt = ts.resolve(p[2], size);
      try { if (dt == null || off + dt.getLength() > cur.getLength()) throw new Exception();
        cur.replaceAtOffset(off, dt, dt.getLength(), p[4], null); ok++; } catch (Exception e) { fail++; }
    }
    println("CV2 structs=" + structs.size() + " members applied=" + ok + " failed=" + fail);
  }
}
