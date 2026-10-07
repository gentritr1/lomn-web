// Decompile every function and export, under OUT_DIR:
//   funcs/<addr>.c       "// 0xADDR Namespace::Name" + decompiled C
//   functions.jsonl      {"addr","name","size","ok","lines"}
//   calls.jsonl          {"from","to":[addrs]}   (direct calls only)
//   vtable_uses.jsonl    {"addr","iface","method"} for every Xxx->lpVtbl->Method field access (typed COM vtables)
// Uses Ghidra's ParallelDecompiler (one decompiler process per core); results are sorted by entry address before
// writing. Measured 2026-10-07 on 8 cores: 325 s vs ~20 min sequential, output byte-identical to the sequential export.
// Decompiler payload limit raised to 1024 MB: GcLegoCamera::Process (37 KB) exceeds the 50 MB default.
// DECOMP_TIMEOUT_S (default 300) per function; failures are retried once sequentially at 4x.
// Optional EXPORT_NAME_FILTER=substr exports only matching functions (debug runs).
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.app.decompiler.parallel.*;
import ghidra.program.model.data.*;
import ghidra.program.model.listing.*;
import ghidra.util.task.TaskMonitor;
import java.nio.file.*; import java.util.*;
public class ExportCorpus extends GhidraScript {
  static String esc(String s) { return s.replace("\\", "\\\\").replace("\"", "\\\""); }
  record R(Function f, boolean ok, String c, List<String> vt) {}
  static R toResult(Function f, DecompileResults r) {
    boolean ok = r != null && r.decompileCompleted();
    String c = ok ? r.getDecompiledFunction().getC() : "// DECOMPILE FAILED: " + (r == null ? "null" : r.getErrorMessage()) + "\n";
    List<String> vt = new ArrayList<>();
    if (ok) {
      ArrayList<ClangNode> toks = new ArrayList<>();
      r.getCCodeMarkup().flatten(toks);
      for (ClangNode t : toks) {
        if (!(t instanceof ClangFieldToken)) continue;
        DataType dt = ((ClangFieldToken) t).getDataType();
        if (dt == null || !dt.getName().endsWith("Vtbl")) continue;
        vt.add(dt.getName().substring(0, dt.getName().length() - 4) + "\u0000" + ((ClangFieldToken) t).getText());
      }
    }
    return new R(f, ok, c, vt);
  }
  static DecompileOptions opts() { DecompileOptions o = new DecompileOptions(); o.setMaxPayloadMBytes(1024); return o; }
  @Override public void run() throws Exception {
    Path out = Paths.get(System.getenv("OUT_DIR"));
    Files.createDirectories(out.resolve("funcs"));
    int timeout = System.getenv("DECOMP_TIMEOUT_S") == null ? 300 : Integer.parseInt(System.getenv("DECOMP_TIMEOUT_S"));
    List<Function> funcs = new ArrayList<>();
    String filter = System.getenv("EXPORT_NAME_FILTER");
    for (Function f : currentProgram.getFunctionManager().getFunctions(true))
      if (filter == null || f.getName(true).contains(filter)) funcs.add(f);
    DecompilerCallback<R> cb = new DecompilerCallback<>(currentProgram, d -> { d.setOptions(opts()); d.toggleCCode(true); d.toggleSyntaxTree(true); }) {
      @Override public R process(DecompileResults r, TaskMonitor m) { return toResult(r.getFunction(), r); }
    };
    cb.setTimeout(timeout);
    long t0 = System.currentTimeMillis();
    List<R> res = new ArrayList<>(ParallelDecompiler.decompileFunctions(cb, funcs, monitor));
    cb.dispose();
    long t1 = System.currentTimeMillis();
    res.sort(Comparator.comparing(r -> r.f().getEntryPoint()));
    DecompInterface di = new DecompInterface(); di.setOptions(opts()); di.openProgram(currentProgram);
    int retried = 0;
    for (int i = 0; i < res.size(); i++) if (!res.get(i).ok()) { retried++; res.set(i, toResult(res.get(i).f(), di.decompileFunction(res.get(i).f(), timeout * 4, monitor))); }
    StringBuilder fj = new StringBuilder(), cj = new StringBuilder(), vj = new StringBuilder();
    int ok = 0;
    for (R r : res) {
      Function f = r.f(); String addr = "0x" + f.getEntryPoint(); if (r.ok()) ok++;
      for (String v : r.vt()) { String[] p = v.split("\u0000"); vj.append("{\"addr\":\"").append(addr).append("\",\"iface\":\"").append(p[0]).append("\",\"method\":\"").append(esc(p[1])).append("\"}\n"); }
      Files.writeString(out.resolve("funcs/" + f.getEntryPoint() + ".c"), "// " + addr + " " + f.getName(true) + "\n" + r.c());
      fj.append("{\"addr\":\"").append(addr).append("\",\"name\":\"").append(esc(f.getName(true))).append("\",\"size\":")
        .append(f.getBody().getNumAddresses()).append(",\"ok\":").append(r.ok()).append(",\"lines\":").append(r.c().split("\n", -1).length).append("}\n");
      TreeSet<String> callees = new TreeSet<>();
      for (Function g : f.getCalledFunctions(monitor)) callees.add("\"0x" + g.getEntryPoint() + "\"");
      cj.append("{\"from\":\"").append(addr).append("\",\"to\":[").append(String.join(",", callees)).append("]}\n");
    }
    Files.writeString(out.resolve("functions.jsonl"), fj.toString());
    Files.writeString(out.resolve("calls.jsonl"), cj.toString());
    Files.writeString(out.resolve("vtable_uses.jsonl"), vj.toString());
    println("ExportCorpus functions=" + res.size() + " decompiled_ok=" + ok + " parallel_ms=" + (t1 - t0) + " retried=" + retried + " cpus=" + Runtime.getRuntime().availableProcessors());
  }
}
