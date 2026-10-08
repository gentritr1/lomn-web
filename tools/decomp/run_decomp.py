"""`make decomp`: canonical exe -> gen/decomp/* and gen/include/original/*.h (both gitignored; derived from the exe).

Pipeline (each step is a module in this folder):
  1 verify exe sha256 == tools/builds.json canonical
  2 nb11.main            -> symbols.txt, types.txt, modules.json
  2b seed_addrs.compute  -> seed_addrs.txt (.CRT initializers + script-native fn pointers + the functions enclosing
                            each RegisterFunction call), reg_sites.json
  3 Ghidra analysis      -> cached project $LOMN_CACHE/decomp/<sha>/proj (import + CV symbols/types + seeded functions,
                            then CV locals + D3D8 vtables + CreateVariant sret signatures)
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
import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path[:0] = [str(HERE), str(REPO / "tools")]

import corpus  # noqa: E402
import headers  # noqa: E402
import metrics  # noqa: E402
import nb11  # noqa: E402
import seed_addrs  # noqa: E402
import seedset  # noqa: E402
import x87fix  # noqa: E402
from lomn_data import BUILDS, sha256_file  # noqa: E402

GHIDRA_HOME = Path(os.environ.get("GHIDRA_HOME") or "/opt/homebrew/opt/ghidra/libexec")
JAVA_HOME = os.environ.get("JAVA_HOME") or "/opt/homebrew/opt/openjdk@21"
# Override only for tests (e.g. the negative case with a deliberately broken script copy).
SCRIPTS = Path(os.environ.get("LOMN_GHIDRA_SCRIPTS") or HERE / "ghidra")
STEP_TIMEOUT_S = 3600  # overridable via env LOMN_GHIDRA_TIMEOUT_S (tests)
STAT_PATTERNS = {
    "ghidra_structs": r"CV2 structs=(\d+)", "ghidra_members_applied": r"members applied=(\d+)",
    "ghidra_labels_applied": r"CV labels applied=(\d+)", "ghidra_local_funcs": r"CV locals: functions=(\d+)",
    "ghidra_params_applied": r"CV locals: functions=\d+ params=(\d+)", "ghidra_locals_applied": r" locals=(\d+) failed",
    "ghidra_d3d8_globals": r"globals typed=(\d+)",
    "ghidra_seeds_created": r"SEED created=(\d+)", "ghidra_seeds_existing": r"SEED created=\d+ existing=(\d+)",
    "ghidra_seeds_failed": r"SEED created=\d+ existing=\d+ failed=(\d+)",
    "ghidra_sret_retyped": r"SRET retyped=(\d+)", "ghidra_sret_failed": r"SRET retyped=\d+ failed=(\d+)",
}
# A stat that must be 0, or the analysis is incomplete and is neither cached nor exported.
MUST_BE_ZERO = ("ghidra_seeds_failed", "ghidra_sret_failed")


def default_cache() -> Path:
    """Ghidra rejects any project path element starting with '.', so ~/.cache is unusable."""
    if sys.platform == "darwin":
        return Path.home() / "Library" / "Caches" / "lomn-web"
    return Path.home() / "lomn-web-cache"


def check_cache_path(proj: Path) -> None:
    if any(part.startswith(".") for part in proj.resolve().parts):
        raise SystemExit(f"Ghidra cannot use a path element starting with '.': {proj.resolve()}; "
                         "set LOMN_CACHE to a dot-free directory")


# Scripts that shape the cached analysis project; ExportCorpus.java is export-only and deliberately excluded.
ANALYSIS_SCRIPTS = ("ApplyCVTypes2.java", "ApplyCVSymbols.java", "ApplyCVLocals.java", "ApplyD3D8Types.java",
                    "CVTypeSpec.java", "SeedFunctions.java", "ApplySret.java")
# The seed extraction rule: a change to it must force re-analysis even if today's seed list happens not to change.
SEED_EXTRACTOR = HERE / "seed_addrs.py"


def analysis_key(exe_sha: str, dec: Path, scripts_dir: Path) -> str:
    """Identity of everything the cached analysis depends on; a changed script or input forces re-analysis."""
    h = hashlib.sha256(exe_sha.encode())
    files = [dec / "types.txt", dec / "symbols.txt", dec / "seed_addrs.txt", HERE / "data" / "d3d8_vtables.txt",
             SEED_EXTRACTOR, *(scripts_dir / n for n in sorted(ANALYSIS_SCRIPTS))]
    for f in files:
        h.update(b"\0" + f.name.encode() + b"\0")
        h.update(f.read_bytes())
    return h.hexdigest()


def _kill_group(proc: subprocess.Popen, grace_s: float = 30.0) -> None:
    """SIGTERM the process group, wait up to grace_s for it to vanish (reaping the leader), then SIGKILL."""
    pgid = proc.pid  # start_new_session=True made the child its own group leader
    try:
        os.killpg(pgid, signal.SIGTERM)
    except (ProcessLookupError, PermissionError):
        return
    deadline = time.monotonic() + grace_s
    while time.monotonic() < deadline:
        proc.poll()  # reap the leader: a zombie keeps the group alive for killpg(pgid, 0)
        try:
            os.killpg(pgid, 0)
        except (ProcessLookupError, PermissionError):
            return
        time.sleep(0.2)
    try:
        os.killpg(pgid, signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
        pass
    proc.poll()


def ghidra(proj: Path, args: list[str], env_extra: dict, log: Path) -> None:
    timeout_s = int(os.environ.get("LOMN_GHIDRA_TIMEOUT_S") or STEP_TIMEOUT_S)
    cmd = [str(GHIDRA_HOME / "support" / "analyzeHeadless"), str(proj), "LOMN", *args, "-scriptPath", str(SCRIPTS)]
    env = {**os.environ, "JAVA_HOME": JAVA_HOME, **env_extra}
    # analyzeHeadless runs java as a child and we start a new session (so the terminal's SIGINT no longer reaches it):
    # on ANY exit from the wait (timeout, Ctrl-C, other exception) kill the whole process group, or a surviving JVM
    # keeps the project lock.
    with open(log, "w") as f:
        proc = subprocess.Popen(cmd, env=env, stdout=f, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            proc.wait(timeout=timeout_s)
        except BaseException as e:
            _kill_group(proc)
            if isinstance(e, subprocess.TimeoutExpired):
                raise SystemExit(f"Ghidra step timed out after {timeout_s}s; see {log}")
            raise
    text = log.read_text(errors="replace")
    if proc.returncode != 0 or "REPORT SCRIPT ERROR" in text or "Script compile failed" in text:
        raise SystemExit(f"Ghidra step failed (exit {proc.returncode}); see {log}")


def run(exe: Path, gen: Path, cache_root: Path, reanalyze: bool) -> dict:
    sha = sha256_file(exe)
    if sha != BUILDS["canonical"]["exe_sha256"]:
        raise SystemExit(f"{exe} is not the canonical exe (sha256 {sha})")
    dec = gen / "decomp"
    if dec.exists():
        shutil.rmtree(dec)
    dec.mkdir(parents=True)
    nb11.main([str(exe), str(dec)])
    seed_addrs.write(seed_addrs.compute(exe.read_bytes()), dec)
    cache = cache_root / "decomp" / sha
    proj = cache / "proj"
    check_cache_path(proj)
    key = analysis_key(sha, dec, SCRIPTS)
    marker = cache / "analysis.ok"  # holds analysis_key(); written only after analysis + stats check succeed
    fresh = marker.exists() and marker.read_text().strip() == key and (proj / "LOMN.gpr").exists()
    if (reanalyze or not fresh) and proj.exists():
        shutil.rmtree(proj)
    if reanalyze or not fresh:
        marker.unlink(missing_ok=True)
        proj.mkdir(parents=True, exist_ok=True)
        ghidra(proj, ["-import", str(exe), "-overwrite",
                      "-preScript", "ApplyCVTypes2.java", "-preScript", "ApplyCVSymbols.java",
                      "-preScript", "SeedFunctions.java",
                      "-postScript", "ApplyCVLocals.java", "-postScript", "ApplyD3D8Types.java",
                      "-postScript", "ApplySret.java"],
               {"CV_SYMBOLS": str(dec / "symbols.txt"), "CV_TYPES": str(dec / "types.txt"),
                "SEED_ADDRS": str(dec / "seed_addrs.txt"),
                "D3D8_VTABLES": str(HERE / "data" / "d3d8_vtables.txt")}, cache / "analyze.log")
    stats = {k: int(m.group(1)) for k, p in STAT_PATTERNS.items()
             if (m := re.search(p, (cache / "analyze.log").read_text(errors="replace")))}
    if len(stats) != len(STAT_PATTERNS):
        raise SystemExit(f"Ghidra script stats missing ({sorted(set(STAT_PATTERNS) - set(stats))}); see {cache / 'analyze.log'}")
    if bad := {k: stats[k] for k in MUST_BE_ZERO if stats[k]}:
        raise SystemExit(f"Ghidra analysis incomplete ({bad}); see {cache / 'analyze.log'}")
    marker.write_text(key + "\n")
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
    ap.add_argument("--cache", type=Path, default=Path(os.environ.get("LOMN_CACHE") or default_cache()))
    ap.add_argument("--reanalyze", action="store_true")
    a = ap.parse_args()
    print(json.dumps(run(a.exe.expanduser().resolve(), a.gen, a.cache.expanduser().resolve(), a.reanalyze), indent=1, sort_keys=True))
