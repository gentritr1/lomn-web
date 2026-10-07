// Decompile every function and export, under OUT_DIR:
//   funcs/<addr>.c       "// 0xADDR Namespace::Name" + decompiled C
//   functions.jsonl      {"addr","name","size","ok","lines"}
//   calls.jsonl          {"from","to":[addrs]}   (direct calls only)
//   vtable_uses.jsonl    {"addr","iface","method"} for every Xxx->lpVtbl->Method field access (typed COM vtables)
// Decompiles in fixed, size-balanced contiguous chunks (8 fixed chunks, DECOMP_WORKERS overrides; each chunk has its own DecompInterface); results are sorted by entry address before
// writing. Measured 2026-10-07 on 8 cores: ~210 s vs ~20 min sequential; three runs pairwise byte-identical. A work-stealing
// pool (ParallelDecompiler) was NOT run-to-run deterministic: local-variable numbering depends on a worker's prior functions.
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
  // Chunk boundaries are part of the output contract; changing this changes local-variable numbering in some functions.
  static final int DEFAULT_CHUNKS = 8;
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
    // Deterministic parallelism: fixed contiguous chunks (by entry address, balanced by body size), each decompiled
    // in order by its own DecompInterface. A decompiler's output can depend on the functions it handled before
    // (local-variable numbering), so a work-stealing pool is NOT run-to-run deterministic; fixed chunks are.
    int workers = System.getenv("DECOMP_WORKERS") == null ? DEFAULT_CHUNKS : Integer.parseInt(System.getenv("DECOMP_WORKERS"));
    if (workers < 1) throw new IllegalArgumentException("DECOMP_WORKERS must be >= 1");
    long total = 0; for (Function f : funcs) total += f.getBody().getNumAddresses();
    List<List<Function>> chunks = new ArrayList<>(); List<Function> cur = new ArrayList<>(); long acc = 0, per = total / workers + 1;
    for (Function f : funcs) { cur.add(f); acc += f.getBody().getNumAddresses(); if (acc >= per && chunks.size() < workers - 1) { chunks.add(cur); cur = new ArrayList<>(); acc = 0; } }
    chunks.add(cur);
    long t0 = System.currentTimeMillis();
    R[][] chunkResults = new R[chunks.size()][];
    List<Thread> threads = new ArrayList<>(); List<Throwable> errors = Collections.synchronizedList(new ArrayList<>());
    for (int c = 0; c < chunks.size(); c++) {
      final int ci = c; final List<Function> chunk = chunks.get(c);
      Thread t = new Thread(() -> {
        DecompInterface d = new DecompInterface();
        try {
          d.setOptions(opts()); d.toggleCCode(true); d.toggleSyntaxTree(true); d.openProgram(currentProgram);
          R[] r = new R[chunk.size()];
          for (int i = 0; i < chunk.size(); i++) r[i] = toResult(chunk.get(i), d.decompileFunction(chunk.get(i), timeout, TaskMonitor.DUMMY));
          chunkResults[ci] = r;
        } catch (Throwable e) { errors.add(e); } finally { d.dispose(); }
      }, "decomp-" + c);
      threads.add(t); t.start();
    }
    for (Thread t : threads) t.join();
    if (!errors.isEmpty()) throw new RuntimeException("decompile worker failed", errors.get(0));
    long t1 = System.currentTimeMillis();
    List<R> res = new ArrayList<>(); for (R[] r : chunkResults) res.addAll(Arrays.asList(r));
    if (res.size() != funcs.size()) throw new IllegalStateException("decompiled " + res.size() + " of " + funcs.size());
    res.sort(Comparator.comparing(r -> r.f().getEntryPoint()));
    DecompInterface di = new DecompInterface(); di.setOptions(opts()); di.openProgram(currentProgram);
    int retried = 0;
    try {
      for (int i = 0; i < res.size(); i++) if (!res.get(i).ok()) { retried++; res.set(i, toResult(res.get(i).f(), di.decompileFunction(res.get(i).f(), timeout * 4, monitor))); }
    } finally { di.dispose(); }
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
