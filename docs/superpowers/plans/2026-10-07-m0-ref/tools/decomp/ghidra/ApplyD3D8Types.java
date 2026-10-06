// Give the Direct3D 8 COM interfaces real vtables so the decompiler prints dev->lpVtbl->SetRenderState(...).
// Input D3D8_VTABLES (d3d8_vtables.txt). Retypes every global data symbol whose MSVC-mangled type is
// "pointer to <Interface>" (e.g. ?mD3DDevice@ScDrawableContext@@2PAUIDirect3DDevice8@@A).
import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.*;
import ghidra.program.model.symbol.*;
import java.nio.file.*; import java.util.*; import java.util.regex.*;
public class ApplyD3D8Types extends GhidraScript {
  @Override public void run() throws Exception {
    DataTypeManager dtm = currentProgram.getDataTypeManager();
    Map<String, Structure> ifaces = new LinkedHashMap<>();
    String cur = null; StructureDataType vt = null; int count = 0;
    List<String> lines = Files.readAllLines(Paths.get(System.getenv("D3D8_VTABLES")));
    for (int i = 0; i <= lines.size(); i++) {
      String l = i < lines.size() ? lines.get(i) : "I _end 0";
      String[] p = l.split(" ");
      if (p[0].equals("I")) {
        if (cur != null) {
          DataType vtr = dtm.addDataType(vt, DataTypeConflictHandler.REPLACE_HANDLER);
          StructureDataType s = new StructureDataType(CategoryPath.ROOT, cur, 0, dtm);
          s.add(new PointerDataType(vtr, 4, dtm), 4, "lpVtbl", null);
          ifaces.put(cur, (Structure) dtm.addDataType(s, DataTypeConflictHandler.REPLACE_HANDLER));
        }
        cur = p[1]; count = Integer.parseInt(p[2]);
        vt = new StructureDataType(CategoryPath.ROOT, cur + "Vtbl", 0, dtm);
      } else if (p[0].equals("m")) {
        vt.add(new PointerDataType(VoidDataType.dataType, 4, dtm), 4, p[2], "slot " + p[1]);
      }
    }
    Pattern pat = Pattern.compile("@@2PAU(\\w+)@@A$");
    int typed = 0;
    SymbolIterator it = currentProgram.getSymbolTable().getAllSymbols(true);
    while (it.hasNext()) {
      Symbol s = it.next();
      Matcher m = pat.matcher(s.getName());
      if (!m.find() || !ifaces.containsKey(m.group(1))) continue;
      PointerDataType pt = new PointerDataType(ifaces.get(m.group(1)), 4, dtm);
      clearListing(s.getAddress(), s.getAddress().add(3));
      createData(s.getAddress(), pt);
      typed++;
      println("typed " + s.getName() + " @ " + s.getAddress());
    }
    println("D3D8 interfaces=" + ifaces.size() + " globals typed=" + typed);
  }
}
