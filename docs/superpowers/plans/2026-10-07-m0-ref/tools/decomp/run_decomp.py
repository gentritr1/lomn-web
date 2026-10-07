"""`make decomp`: canonical exe -> gen/decomp/* and gen/include/original/*.h (both gitignored; derived from the exe).

Pipeline (each step is a module in this folder):
  1 verify exe sha256 == tools/builds.json canonical
  2 nb11.main            -> symbols.txt, types.txt, modules.json
  3 Ghidra analysis      -> cached project $LOMN_CACHE/decomp/<sha>/proj (import + CV symbols/types/locals + D3D8 vtables)
  4 Ghidra ExportCorpus  -> funcs/*.c, functions.jsonl, calls.jsonl, vtable_uses.jsonl
  5 x87fix               -> rewrites x87 compare idioms in funcs/*.c
  6 corpus.build         -> corpus/<original path>.cpp, function_map.json
  7 seedset.compute      -> seeds.json (m1-required / m1-candidate)
  8 headers.generate     -> gen/include/original/*.h + all.cpp
  9 metrics.compute      -> metrics.json
Analysis is cached per exe sha (only redone with --reanalyze); export + later steps always rerun.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path[:0] = [str(HERE), str(REPO / "tools")]

import corpus  # noqa: E402
import headers  # noqa: E402
import metrics  # noqa: E402
import nb11  # noqa: E402
import seedset  # noqa: E402
import x87fix  # noqa: E402
from lomn_data import BUILDS, sha256_file  # noqa: E402

GHIDRA_HOME = Path(os.environ.get("GHIDRA_HOME") or "/opt/homebrew/opt/ghidra/libexec")
JAVA_HOME = os.environ.get("JAVA_HOME") or "/opt/homebrew/opt/openjdk@21"
# Override only for tests (e.g. the negative case with a deliberately broken script copy).
SCRIPTS = Path(os.environ.get("LOMN_GHIDRA_SCRIPTS") or HERE / "ghidra")
STEP_TIMEOUT_S = 3600
STAT_PATTERNS = {
    "ghidra_structs": r"CV2 structs=(\d+)", "ghidra_members_applied": r"members applied=(\d+)",
    "ghidra_labels_applied": r"CV labels applied=(\d+)", "ghidra_local_funcs": r"CV locals: functions=(\d+)",
    "ghidra_params_applied": r"CV locals: functions=\d+ params=(\d+)", "ghidra_locals_applied": r" locals=(\d+) failed",
    "ghidra_d3d8_globals": r"globals typed=(\d+)",
}


def ghidra(proj: Path, args: list[str], env_extra: dict, log: Path) -> None:
    cmd = [str(GHIDRA_HOME / "support" / "analyzeHeadless"), str(proj), "LOMN", *args, "-scriptPath", str(SCRIPTS)]
    env = {**os.environ, "JAVA_HOME": JAVA_HOME, **env_extra}
    try:
        with open(log, "w") as f:
            r = subprocess.run(cmd, env=env, stdout=f, stderr=subprocess.STDOUT, timeout=STEP_TIMEOUT_S)
    except subprocess.TimeoutExpired:
        raise SystemExit(f"Ghidra step timed out after {STEP_TIMEOUT_S}s; see {log}")
    text = log.read_text()
    if r.returncode != 0 or "REPORT SCRIPT ERROR" in text or "Script compile failed" in text:
        raise SystemExit(f"Ghidra step failed (exit {r.returncode}); see {log}")


def run(exe: Path, gen: Path, cache_root: Path, reanalyze: bool) -> dict:
    sha = sha256_file(exe)
    if sha != BUILDS["canonical"]["exe_sha256"]:
        raise SystemExit(f"{exe} is not the canonical exe (sha256 {sha})")
    dec = gen / "decomp"
    if dec.exists():
        shutil.rmtree(dec)
    dec.mkdir(parents=True)
    nb11.main([str(exe), str(dec)])
    cache = cache_root / "decomp" / sha
    proj = cache / "proj"
    marker = cache / "analysis.ok"  # written only after a fully successful analysis; LOMN.gpr alone proves nothing
    if (reanalyze or not marker.exists()) and proj.exists():
        shutil.rmtree(proj)
        marker.unlink(missing_ok=True)
    if not marker.exists():
        proj.mkdir(parents=True, exist_ok=True)
        ghidra(proj, ["-import", str(exe), "-overwrite",
                      "-preScript", "ApplyCVTypes2.java", "-preScript", "ApplyCVSymbols.java",
                      "-postScript", "ApplyCVLocals.java", "-postScript", "ApplyD3D8Types.java"],
               {"CV_SYMBOLS": str(dec / "symbols.txt"), "CV_TYPES": str(dec / "types.txt"),
                "D3D8_VTABLES": str(HERE / "data" / "d3d8_vtables.txt")}, cache / "analyze.log")
        marker.write_text(sha + "\n")
    stats = {k: int(m.group(1)) for k, p in STAT_PATTERNS.items()
             if (m := re.search(p, (cache / "analyze.log").read_text()))}
    if len(stats) != len(STAT_PATTERNS):
        raise SystemExit(f"Ghidra script stats missing ({sorted(set(STAT_PATTERNS) - set(stats))}); see {cache / 'analyze.log'}")
    export = cache / "export"
    if export.exists():
        shutil.rmtree(export)
    ghidra(proj, ["-process", exe.name, "-noanalysis", "-readOnly", "-postScript", "ExportCorpus.java"],
           {"OUT_DIR": str(export)}, cache / "export.log")
    shutil.copytree(export / "funcs", dec / "funcs")
    for name in ("functions.jsonl", "calls.jsonl", "vtable_uses.jsonl"):
        shutil.copy(export / name, dec / name)
    rewrites = 0
    for f in sorted((dec / "funcs").glob("*.c")):
        text, n = x87fix.fix(f.read_text())
        if n:
            f.write_text(text)
            rewrites += n
    corpus.build(dec, json.loads((dec / "modules.json").read_text()), dec)
    patterns = [l.strip() for l in (HERE / "data" / "m1_seeds.txt").read_text().splitlines()
                if l.strip() and not l.startswith("#")]
    seeds = seedset.compute(*(seedset.load_jsonl(dec / n) for n in ("functions.jsonl", "calls.jsonl", "vtable_uses.jsonl")),
                            patterns)
    (dec / "seeds.json").write_text(json.dumps(seeds, indent=0))
    inc = gen / "include" / "original"
    if inc.exists():
        shutil.rmtree(inc)
    headers.generate((dec / "types.txt").read_text().splitlines(), inc)
    m = {**metrics.compute(dec), **stats}
    (dec / "metrics.json").write_text(json.dumps(m, indent=1, sort_keys=True) + "\n")
    return m


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", type=Path, required=True)
    ap.add_argument("--gen", type=Path, default=REPO / "gen")
    # An exported-but-empty LOMN_CACHE must mean "default", never Path("") (= the repo root).
    ap.add_argument("--cache", type=Path, default=Path(os.environ.get("LOMN_CACHE") or Path.home() / ".cache" / "lomn-web"))
    ap.add_argument("--reanalyze", action="store_true")
    a = ap.parse_args()
    print(json.dumps(run(a.exe, a.gen, a.cache, a.reanalyze), indent=1, sort_keys=True))
