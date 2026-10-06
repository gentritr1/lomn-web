// Pre-analysis: apply CodeView NB11 public symbols (addr name per line) as IMPORTED labels so the MSVC demangler runs.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.symbol.SourceType;
import java.nio.file.*;
public class ApplyCVSymbols extends GhidraScript {
  @Override public void run() throws Exception {
    int ok = 0, bad = 0;
    for (String line : Files.readAllLines(Paths.get(System.getenv("CV_SYMBOLS")))) {
      int sp = line.indexOf(' '); if (sp < 0) continue;
      Address a = toAddr(Long.parseLong(line.substring(2, sp), 16));
      if (currentProgram.getMemory().contains(a)) {
        try { createLabel(a, line.substring(sp + 1), true, SourceType.IMPORTED); ok++; } catch (Exception e) { bad++; }
      } else bad++;
    }
    println("CV labels applied=" + ok + " skipped=" + bad);
  }
}
