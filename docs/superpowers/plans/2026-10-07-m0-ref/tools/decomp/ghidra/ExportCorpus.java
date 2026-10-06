// Decompile every function and export, under OUT_DIR:
//   funcs/<addr>.c       "// 0xADDR Namespace::Name" + decompiled C
//   functions.jsonl      {"addr","name","size","ok","lines"}
//   calls.jsonl          {"from","to":[addrs]}   (direct calls only)
//   vtable_uses.jsonl    {"addr","iface","method"} for every Xxx->lpVtbl->Method field access (typed COM vtables)
// Output is ordered by entry address, so two runs over the same program are byte-identical.
// Optional EXPORT_LIMIT=N exports only the first N functions (smoke runs).
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.data.*;
import ghidra.program.model.listing.*;
import java.nio.file.*; import java.util.*;
public class ExportCorpus extends GhidraScript {
  static String esc(String s) { return s.replace("\\", "\\\\").replace("\"", "\\\""); }
  @Override public void run() throws Exception {
    Path out = Paths.get(System.getenv("OUT_DIR"));
    Files.createDirectories(out.resolve("funcs"));
    int limit = System.getenv("EXPORT_LIMIT") == null ? Integer.MAX_VALUE : Integer.parseInt(System.getenv("EXPORT_LIMIT"));
    DecompInterface di = new DecompInterface(); di.openProgram(currentProgram);
    StringBuilder fj = new StringBuilder(), cj = new StringBuilder(), vj = new StringBuilder();
    int n = 0, ok = 0;
    for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
      if (n++ >= limit || monitor.isCancelled()) break;
      String addr = "0x" + f.getEntryPoint().toString();
      String name = f.getName(true);
      DecompileResults r = di.decompileFunction(f, 120, monitor);
      boolean good = r.decompileCompleted();
      String c = good ? r.getDecompiledFunction().getC() : "// DECOMPILE FAILED: " + r.getErrorMessage() + "\n";
      if (good) {
        ok++;
        ArrayList<ClangNode> toks = new ArrayList<>();
        r.getCCodeMarkup().flatten(toks);
        for (ClangNode t : toks) {
          if (!(t instanceof ClangFieldToken)) continue;
          DataType dt = ((ClangFieldToken) t).getDataType();
          if (dt == null || !dt.getName().endsWith("Vtbl")) continue;
          String iface = dt.getName().substring(0, dt.getName().length() - 4);
          vj.append("{\"addr\":\"").append(addr).append("\",\"iface\":\"").append(iface)
            .append("\",\"method\":\"").append(esc(((ClangFieldToken) t).getText())).append("\"}\n");
        }
      }
      Files.writeString(out.resolve("funcs/" + f.getEntryPoint().toString() + ".c"), "// " + addr + " " + name + "\n" + c);
      fj.append("{\"addr\":\"").append(addr).append("\",\"name\":\"").append(esc(name)).append("\",\"size\":")
        .append(f.getBody().getNumAddresses()).append(",\"ok\":").append(good).append(",\"lines\":")
        .append(c.split("\n", -1).length).append("}\n");
      TreeSet<String> callees = new TreeSet<>();
      for (Function g : f.getCalledFunctions(monitor)) callees.add("\"0x" + g.getEntryPoint() + "\"");
      cj.append("{\"from\":\"").append(addr).append("\",\"to\":[").append(String.join(",", callees)).append("]}\n");
    }
    Files.writeString(out.resolve("functions.jsonl"), fj.toString());
    Files.writeString(out.resolve("calls.jsonl"), cj.toString());
    Files.writeString(out.resolve("vtable_uses.jsonl"), vj.toString());
    println("ExportCorpus functions=" + Math.min(n, limit) + " decompiled_ok=" + ok);
  }
}
