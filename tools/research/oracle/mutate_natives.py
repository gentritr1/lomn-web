"""M2a-2 T10 (Review focus 3) mutation bite of the C++ native models: each deliberate one-line defect in ONE native must
make the A9 run's own-models comparison fail on a line its EXPECT regex matches. The A9 replay leg compares VM semantics
only (a C++ native wrong in the same way as the replayed log passes), so the bite is on osi-a9's cmp 3/4 (D lines, C++
lomn_view vs the Python oracle running its OWN models, `--real-natives`) and on the natives.log half of cmp 4/4. Run
ONLY in a scratch copy of the tree (it edits sources in place and restores them):

    python3 tools/research/oracle/mutate_natives.py <scratch_tree> [--all | --only M1,M3,CTRL] [--list]

<scratch_tree> needs build/native configured (`cmake -S engine -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release
-DLOMN_WEBGPU=OFF`: lomn_view --script needs no Dawn; only the lomn_view target is built),
LOMN_DATA in the environment (the run reads $LOMN_DATA/install), and .venv. Start: the Python own-models leg and the
pristine C++ leg are run once (build/mutn-oracle/, rebuilt when the oracle, the tables or Base.osi change). Each mutant:
apply, build lomn_view, run the C++ leg (osi-a9's exact arguments: --script 1 --natives real, 600 frames, the A9 keys),
compare, restore, `cmp` the restored file against the pristine copy taken at start (build/mutn-pristine/), rebuild.
The check `a9own` prints its first differing record per file:
  CMP3 D line N after '<previous common line>': cpp='...' py='...'          (digest D lines vs Python own models)
  CMP4 natives.log line N after '...': cpp='...' py='...'                  (natives.log vs Python own models)
plus `stop: <...>` when the C++ run stopped on a VM error. Verdicts: CAUGHT (a fail line matches EXPECT), MISSED (both
cmps equal), OFF-TARGET (a cmp failed, no line matches), NO-EFFECT (the C++ outputs equal the PRISTINE C++ outputs: the
mutant does not change the native trace on the A9 path, so it is replaced, not counted), BUILD-FAILED (invalid). The
no-op control must be MISSED. Logs: build/mutlogs-natives/<id>-*.log, results: build/mutlogs-natives/results.jsonl.
Exit 0 only when every mutant run is CAUGHT and the control (when run) is MISSED; 1 otherwise; 2 on a setup/restore error.
"""
from __future__ import annotations

import filecmp
import hashlib
import json
import os
import re
import shutil
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mutate_vm  # noqa: E402  (sh, first_diff, anchor_line: the same helpers, so both tools report alike)

SV = "engine/src/osi/natives/services.cpp"
DR = "engine/src/osi/natives/directors.cpp"
FW = "engine/src/osi/natives/framework.cpp"
OB = "engine/src/osi/natives/objects.cpp"
KEYS = "down@100 down@160 enter@220 esc@280"
FRAMES = 600

# (id, name, native key, file, old, new, {check: EXPECT regex}). The six of the plan (Review focus 3); T10 replaced two that
# changed nothing on the A9 path (NO-EFFECT, 2026-11-13): the later-pass AddRef (M4, now the first pass) and playcinematic not
# instant (M6; no A9 read of the playing flag), now getcameraaimode + 1.
M = [
    ("M1", "stringtoidentifier with the byte order reversed", "scslosifoundation.stringtoidentifier", SV,
     "  return make_int(static_cast<int32_t>(convert_string_to_id(string_arg(host(user), c, 0))));",
     "  return make_int(static_cast<int32_t>(__builtin_bswap32(convert_string_to_id(string_arg(host(user), c, 0)))));",
     {"a9own": r"^CMP4 natives\.log line \d+ .*cpp='\d{8} scslosifoundation\.stringtoidentifier 1 -> "}),
    ("M2", "cheatmenu returns 1", "gcsaver.cheatmenu", DR,
     "  return make_int(host(user).subsystem<SaverState>().cheat_menu() ? 1 : 0);",
     "  return make_int(1);",
     {"a9own": r"^CMP4 natives\.log line \d+ .*cpp='\d{8} gcsaver\.cheatmenu 0 -> 0010:00000001'"}),
    ("M3", "doesfileexist returns 1", "scslosifileio.doesfileexist", SV,
     "  return make_int(does_file_exist(ctx, string_arg(ctx, c, 0)) ? 1 : 0);",
     "  return make_int(1);",
     {"a9own": r"^CMP4 natives\.log line \d+ .*cpp='\d{8} scslosifileio\.doesfileexist 1 -> 0010:00000001'"}),
    ("M4", "CreateVariant's first pass without the variant's AddRef (total = engine refs, not engine + 1)", "(CreateVariant)", FW,
     "    const uint32_t h = heap_.new_game(o->type_, o->engine_refs_);",
     "    const uint32_t h = heap_.new_game(o->type_, o->engine_refs_ - 1);  // mutant: no +1 for the variant",
     {"a9own": r"^CMP3 D line \d+ |^CMP4 natives\.log line \d+ "}),
    ("M5", "getcurrentlevel + 1", "gcareadirector.getcurrentlevel", DR,
     "  return make_int(static_cast<int32_t>(host(user).subsystem<AreaDirector>().current_level()));",
     "  return make_int(static_cast<int32_t>(host(user).subsystem<AreaDirector>().current_level()) + 1);",
     {"a9own": r"^CMP4 natives\.log line \d+ .*cpp='\d{8} gcareadirector\.getcurrentlevel 0 -> "}),
    ("M6", "getcameraaimode + 1", "gclegocamera.getcameraaimode", OB,
     "  return make_int(camera(host(user), c, \"GcLegoCamera::GetCameraAIMode (0x44ed18)\").ai_mode);",
     "  return make_int(camera(host(user), c, \"GcLegoCamera::GetCameraAIMode (0x44ed18)\").ai_mode + 1);",
     {"a9own": r"^CMP4 natives\.log line \d+ .*cpp='\d{8} gclegocamera\.getcameraaimode 1 -> "}),
    ("CTRL", "no-op control (comment only; must be MISSED)", "-", SV,
     "// ---- the native bodies ----", "// ---- the native bodies ---- (mutation control)",
     {"a9own": None}),
]
FAIL_RE = r"^CMP[34] |^stop: (?!none)|^MISSING |TIMEOUT"
LOGDIR: Path | None = None


def is_control(mid: str) -> bool:
    return mid == "CTRL"


def sh(cmd: str, tree: Path, timeout: int, log: str | None = None):
    mutate_vm.LOGDIR = LOGDIR
    return mutate_vm.sh(cmd, tree, timeout, log)


def lomn_data() -> Path:
    d = os.environ.get("LOMN_DATA", "")
    if not d or not (Path(d) / "install" / "data" / "Base.osi").is_file():
        raise SystemExit("mutate_natives: LOMN_DATA must name a data root with install/data/Base.osi")
    return Path(d)


def cpp_leg(tree: Path, out: Path, log: str | None) -> tuple[int, str]:
    """osi-a9's C++ leg (lomn_view --script 1, real natives): digest and natives.log into `out`."""
    out.mkdir(parents=True, exist_ok=True)
    for f in ("cpp.digest", "cpp.natives.log"):
        (out / f).unlink(missing_ok=True)
    rc, o, _ = sh(f"timeout 300 build/native/lomn_view --install '{lomn_data() / 'install'}' --script 1 --natives real "
                  f"--frames {FRAMES} --keys '{KEYS}' --digest '{out / 'cpp.digest'}' --natives-log '{out / 'cpp.natives.log'}'",
                  tree, 330, log)
    return rc, o


def d_lines(p: Path) -> Path:
    q = p.with_suffix(".D")
    q.write_bytes(b"".join(l for l in p.read_bytes().splitlines(keepends=True) if l.startswith(b"D ")) if p.is_file() else b"")
    return q


def oracle(tree: Path) -> Path:
    """The Python own-models leg and the pristine C++ leg, made once per (oracle, tables, Base.osi, C++ natives) state."""
    d = tree / "build" / "mutn-oracle"
    base = lomn_data() / "install" / "data" / "Base.osi"
    h = hashlib.sha256()
    srcs = [tree / "tools/research/osi_vm.py", *sorted((tree / "tools/research/osi_natives").glob("*.py")),
            *sorted((tree / "data/osi").glob("*.json")), *sorted((tree / "engine/src/osi").rglob("*.cpp")), base]
    for p in srcs:
        h.update(str(p.relative_to(tree) if p.is_relative_to(tree) else p.name).encode() + b"\0" + p.read_bytes())
    stamp = h.hexdigest()
    if (d / "stamp").is_file() and (d / "stamp").read_text() == stamp and (d / "py.D").is_file() and (d / "cpp.D").is_file():
        return d
    shutil.rmtree(d, ignore_errors=True)
    d.mkdir(parents=True)
    tables = "--natives data/osi/natives.json --mods data/osi/natives_beta150.json --types data/osi/types.json"
    rc, o, _ = sh(f".venv/bin/python tools/research/osi_vm.py --run '{base}' {tables} --real-natives --install "
                  f"'{lomn_data() / 'install'}' --frames {FRAMES} --keys '{KEYS}' --digest build/mutn-oracle/py.digest "
                  f"--log-out build/mutn-oracle/py.natives.log", tree, 900, "oracle-py.log")
    if rc or "stop: none" not in o:
        raise SystemExit("mutate_natives: the Python own-models leg failed (rc %d); see build/mutlogs-natives/oracle-py.log" % rc)
    rc, _ = cpp_leg(tree, d, "oracle-cpp.log")
    if rc:
        raise SystemExit("mutate_natives: the pristine C++ leg failed (rc %d); see build/mutlogs-natives/oracle-cpp.log" % rc)
    d_lines(d / "py.digest")
    d_lines(d / "cpp.digest")
    if not filecmp.cmp(d / "py.D", d / "cpp.D", shallow=False) or not filecmp.cmp(d / "py.natives.log", d / "cpp.natives.log", shallow=False):
        raise SystemExit("mutate_natives: the PRISTINE tree already fails cmp 3/4 or 4/4: fix that first")
    (d / "stamp").write_text(stamp)
    return d


def check_a9own(tree: Path, mid: str, ref: Path) -> tuple[int, list[str], bool, float]:
    """(rc, fail lines, native-trace-changed vs the pristine C++ leg, seconds)."""
    t0 = time.time()
    out = tree / "build" / "mutn-run"
    rc, o = cpp_leg(tree, out, f"{mid}-a9own.log")
    cd = d_lines(out / "cpp.digest")
    lines = [l for l in o.splitlines() if re.match(r"^stop: ", l) and l != "stop: none"]
    if rc == 124:
        lines.append("TIMEOUT")
    lines += mutate_vm.first_diff(cd, ref / "py.D", "CMP3 D")
    lines += mutate_vm.first_diff(out / "cpp.natives.log", ref / "py.natives.log", "CMP4 natives.log")
    changed = not (filecmp.cmp(cd, ref / "cpp.D", shallow=False) and (out / "cpp.natives.log").is_file()
                   and filecmp.cmp(out / "cpp.natives.log", ref / "cpp.natives.log", shallow=False))
    if LOGDIR:
        with (LOGDIR / f"{mid}-a9own.log").open("a") as f:
            f.write("\n".join(lines) + f"\nnative trace changed vs pristine C++: {changed}\n")
    fails = [l for l in lines if re.search(FAIL_RE, l)]
    return (1 if fails else 0), fails, changed, time.time() - t0


def build(tree: Path, log: str) -> tuple[int, float]:
    rc, _, secs = sh("timeout 900 cmake --build build/native --target lomn_view", tree, 960, log)
    return rc, secs


def parse_only(spec: str) -> set[str]:
    ids = {e[0] for e in M}
    out = {p.strip() for p in spec.split(",") if p.strip()}
    bad = out - ids
    if bad:
        raise SystemExit("mutate_natives: unknown mutant id(s): " + ", ".join(sorted(bad)))
    return out


def main(argv: list[str]) -> int:
    if len(argv) < 2 or argv[1].startswith("--"):
        print(__doc__.split("\n\n")[1].strip())
        return 2
    tree = Path(argv[1]).resolve()
    if "--list" in argv:
        for i, (mid, name, key, rel, old, new, checks) in enumerate(M, 1):
            text = (tree / rel).read_text()
            line = mutate_vm.anchor_line(text, old, new) if text.count(old) == 1 else -1
            print(f"{i} {mid:4s} {rel}:{line} [{key}] | {name} | " + ", ".join(f"{c}: {r}" for c, r in checks.items()))
        return 0
    only = parse_only(argv[argv.index("--only") + 1]) if "--only" in argv else None
    if only is None and "--all" not in argv:
        print("mutate_natives: name --all or --only <ids>")
        return 2
    global LOGDIR
    LOGDIR = tree / "build" / "mutlogs-natives"
    LOGDIR.mkdir(parents=True, exist_ok=True)
    pristine = tree / "build" / "mutn-pristine"
    active = tree / "build" / "mutn-active"
    if active.exists():  # an interrupted earlier run: restore that file from its pristine copy first
        rel = active.read_text().strip()
        shutil.copyfile(pristine / rel, tree / rel)
        active.unlink()
        print(f"restored {rel} left mutated by an interrupted run", flush=True)
    for rel in sorted({e[3] for e in M}):
        (pristine / rel).parent.mkdir(parents=True, exist_ok=True)
        if not (pristine / rel).exists():
            shutil.copyfile(tree / rel, pristine / rel)
        if not filecmp.cmp(tree / rel, pristine / rel, shallow=False):
            print(f"ERROR {rel} differs from build/mutn-pristine (stale pristine copy?)", flush=True)
            return 2
    rc, _ = build(tree, "pristine-build.log")
    if rc:
        print("ERROR the pristine tree does not build lomn_view", flush=True)
        return 2
    ref = oracle(tree)
    print(f"oracle: Python own models == pristine C++ ({sum(1 for _ in (ref / 'py.D').open())} D lines, natives.log "
          f"{sum(1 for _ in (ref / 'py.natives.log').open())} lines)", flush=True)
    results = (LOGDIR / "results.jsonl").open("a")
    tally: dict[str, int] = {}
    ok = True
    for i, (mid, name, key, rel, old, new, checks) in enumerate(M, 1):
        if only and mid not in only:
            continue
        f = tree / rel
        src = f.read_text()
        if src.count(old) != 1:
            print(f"[{i}] ERROR anchor of {mid} not found exactly once in {rel}: {name}", flush=True)
            return 2
        line = mutate_vm.anchor_line(src, old, new)
        t0 = time.time()
        verdict, per, bsecs = "MISSED", {}, 0.0
        active.write_text(rel)
        f.write_text(src.replace(old, new))
        try:
            brc, bsecs = build(tree, f"{mid}-build.log")
            if brc:
                verdict = "BUILD-FAILED"
            else:
                want = checks["a9own"]
                crc, fails, changed, secs = check_a9own(tree, mid, ref)
                hit = [l for l in fails if want and re.search(want, l)]
                if crc == 0:
                    per["a9own"] = {"outcome": "pass", "secs": round(secs, 1), "native_changed": changed}
                else:
                    outcome = "BITE" if hit else ("TIMEOUT" if "TIMEOUT" in fails else "OFF-TARGET")
                    per["a9own"] = {"outcome": outcome, "rc": crc, "secs": round(secs, 1), "native_changed": changed,
                                    "line": (hit or fails)[0][:300], "fail_lines": len(fails)}
                    if hit or is_control(mid):
                        verdict = "CAUGHT"
                    else:
                        verdict = "OFF-TARGET"
                if not changed and not is_control(mid):
                    verdict = "NO-EFFECT"
        finally:
            f.write_text(src)
            ok_restore = filecmp.cmp(f, pristine / rel, shallow=False)
            if ok_restore:
                active.unlink()
            rrc, _ = build(tree, f"{mid}-restore-build.log")
        total = time.time() - t0
        tally[verdict] = tally.get(verdict, 0) + 1
        ok &= (verdict == "MISSED") if is_control(mid) else (verdict == "CAUGHT")
        rec = {"id": mid, "name": name, "native": key, "where": f"{rel}:{line}", "verdict": verdict, "checks": per,
               "build_s": round(bsecs, 1), "total_s": round(total, 1), "restore_cmp": ok_restore, "restore_build_rc": rrc}
        results.write(json.dumps(rec) + "\n")
        results.flush()
        detail = "; ".join(f"{c}: {v['outcome']}" + (f" ({v['line'][:160]})" if v.get("line") else "") +
                           f" [native trace changed: {v['native_changed']}]" for c, v in per.items())
        print(f"[{i}] {verdict} {mid} {rel}:{line} {name} :: {detail} [build {bsecs:.0f} s, total {total:.0f} s, "
              f"restore cmp {'OK' if ok_restore else 'FAILED'}, rebuild rc={rrc}]", flush=True)
        if not ok_restore or rrc:
            print("ERROR restore failed: stopping", flush=True)
            return 2
    print("mutants: " + ", ".join(f"{v} {n}" for v, n in sorted(tally.items())) +
          " (every mutant must be CAUGHT, the no-op control CTRL MISSED; NO-EFFECT = replace the mutant)", flush=True)
    for rel in sorted({e[3] for e in M}):
        same = filecmp.cmp(tree / rel, pristine / rel, shallow=False)
        ok &= same
        print(f"final restore cmp {rel}: {'OK' if same else 'DIFFERS'}", flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
