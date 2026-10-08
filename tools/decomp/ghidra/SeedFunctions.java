// Pre-analysis (after ApplyCVSymbols): create a function at every address in SEED_ADDRS (seed_addrs.txt, one 0x%08x per
// line: the .CRT static initializers, the script-native fn pointers passed to ScOSISystem::RegisterFunction and the
// functions enclosing those calls, see tools/decomp/seed_addrs.py). Auto-analysis never reaches these (they are only
// referenced from a pointer table or a push imm32), so without seeds the corpus misses 921 of the 1,056 registrations
// (M2a-0 probe 1). Prints "SEED created=<n> existing=<m> failed=<k>";
// run_decomp fails the run when failed > 0.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import java.nio.file.*; import java.util.*;
public class SeedFunctions extends GhidraScript {
  @Override public void run() throws Exception {
    FunctionManager fm = currentProgram.getFunctionManager();
    int created = 0, existing = 0; List<String> failed = new ArrayList<>();
    for (String l : Files.readAllLines(Paths.get(System.getenv("SEED_ADDRS")))) {
      l = l.trim(); if (l.isEmpty()) continue;
      Address a = toAddr(Long.decode(l));
      if (fm.getFunctionAt(a) != null) { existing++; continue; }
      if (!currentProgram.getMemory().contains(a)) { failed.add(l + " not in memory"); continue; }
      if (getInstructionAt(a) == null && !disassemble(a)) { failed.add(l + " disassembly failed"); continue; }
      if (createFunction(a, null) == null) failed.add(l + " createFunction failed"); else created++;
    }
    for (String s : failed) println("SEED failed " + s);
    println("SEED created=" + created + " existing=" + existing + " failed=" + failed.size());
  }
}
