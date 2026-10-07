// Decompile every function and export, under OUT_DIR:
//   funcs/<addr>.c       "// 0xADDR Namespace::Name" + decompiled C
//   functions.jsonl      {"addr","name","size","ok","lines"}
//   calls.jsonl          {"from","to":[addrs]}   (direct calls only)
//   vtable_uses.jsonl    {"addr","iface","method"} for every Xxx->lpVtbl->Method field access (typed COM vtables)
// Output is ordered by entry address, so two runs over the same program are byte-identical.
// Optional EXPORT_LIMIT=N exports only the first N functions; EXPORT_NAME_FILTER=substr only matching names (debug runs).
// Decompile timeout: DECOMP_TIMEOUT_S (default 300) per function, and one retry at 4x for any that fail, so
// results do not depend on machine load (a 120 s limit made one extra function fail under load on 2026-10-07).
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
    String filter = System.getenv("EXPORT_NAME_FILTER");
    int timeout = System.getenv("DECOMP_TIMEOUT_S") == null ? 300 : Integer.parseInt(System.getenv("DECOMP_TIMEOUT_S"));
    DecompInterface di = new DecompInterface();
    DecompileOptions opts = new DecompileOptions();
    // GcLegoCamera::Process (37 KB of code) exceeds the default 50 MB decompiler response buffer.
    opts.setMaxPayloadMBytes(1024);
    di.setOptions(opts);
    di.openProgram(currentProgram);
    StringBuilder fj = new StringBuilder(), cj = new StringBuilder(), vj = new StringBuilder();
    int n = 0, ok = 0;
    for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
      if (n >= limit || monitor.isCancelled()) break;
      String name = f.getName(true);
      if (filter != null && !name.contains(filter)) continue;
      n++;
      String addr = "0x" + f.getEntryPoint().toString();
      DecompileResults r = di.decompileFunction(f, timeout, monitor);
      if (!r.decompileCompleted()) r = di.decompileFunction(f, timeout * 4, monitor);
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
