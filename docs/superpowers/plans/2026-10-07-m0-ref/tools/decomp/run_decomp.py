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

GHIDRA_HOME = Path(os.environ.get("GHIDRA_HOME", "/opt/homebrew/opt/ghidra/libexec"))
JAVA_HOME = os.environ.get("JAVA_HOME", "/opt/homebrew/opt/openjdk@21")
STEP_TIMEOUT_S = 3600


def ghidra(proj: Path, args: list[str], env_extra: dict, log: Path) -> None:
    cmd = [str(GHIDRA_HOME / "support" / "analyzeHeadless"), str(proj), "LOMN", *args,
           "-scriptPath", str(HERE / "ghidra")]
    env = {**os.environ, "JAVA_HOME": JAVA_HOME, **env_extra}
    with open(log, "w") as f:
        r = subprocess.run(cmd, env=env, stdout=f, stderr=subprocess.STDOUT, timeout=STEP_TIMEOUT_S)
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
    if reanalyze and proj.exists():
        shutil.rmtree(proj)
    if not (proj / "LOMN.gpr").exists():
        proj.mkdir(parents=True, exist_ok=True)
        ghidra(proj, ["-import", str(exe), "-overwrite",
                      "-preScript", "ApplyCVTypes2.java", "-preScript", "ApplyCVSymbols.java",
                      "-postScript", "ApplyCVLocals.java", "-postScript", "ApplyD3D8Types.java"],
               {"CV_SYMBOLS": str(dec / "symbols.txt"), "CV_TYPES": str(dec / "types.txt"),
                "D3D8_VTABLES": str(HERE / "data" / "d3d8_vtables.txt")}, cache / "analyze.log")
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
    m = metrics.compute(dec)
    (dec / "metrics.json").write_text(json.dumps(m, indent=1, sort_keys=True) + "\n")
    return m


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", type=Path, required=True)
    ap.add_argument("--gen", type=Path, default=REPO / "gen")
    ap.add_argument("--cache", type=Path, default=Path(os.environ.get("LOMN_CACHE", Path.home() / ".cache" / "lomn-web")))
    ap.add_argument("--reanalyze", action="store_true")
    a = ap.parse_args()
    print(json.dumps(run(a.exe, a.gen, a.cache, a.reanalyze), indent=1, sort_keys=True))
