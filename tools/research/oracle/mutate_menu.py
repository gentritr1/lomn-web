"""M2a-3 T10 mutation bite of the menu path (plan Task 10). Each deliberate one-line defect must be CAUGHT on the check it
names; the comment-only control must be MISSED; a mutant whose menu-run outputs equal the pristine ones is NO-EFFECT
(replaced, not counted). Run ONLY in a scratch copy of the tree (it edits sources in place and restores them):

    python3 tools/research/oracle/mutate_menu.py <scratch_tree> [--all | --only K1,K7,CTRL] [--list]

<scratch_tree> needs build/native configured (Release, -DLOMN_WEBGPU=OFF), .venv and LOMN_DATA. Checks:
  menu    lomn_view on M1 and M2 (`make osi-menu`'s arguments): D lines, natives.log and Z lines against the Python
          oracle's OWN models (made once, build/mutm-oracle/), the A12 state log against the pristine C++ leg (which
          hud_oracle.py matches). Fail lines: `<run> D|LOG|Z|STATE line N after '...': cpp='...' ref='...'`.
  ctest   build/native/lomn_tests -tc=<case>: a failing doctest case is `CTEST <case> failed`.
  pytest  the named Python test: `PYTEST <id> failed`.
Logs and results: build/mutlogs-menu/. Exit 0 only when every mutant is CAUGHT and CTRL MISSED; 2 on a setup/restore error.
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
import mutate_vm  # noqa: E402

RUNS = {  # the Makefile's OSI_M1 / OSI_M2: frames, keys, mouse, shots
    "M1": (905, "down@700 down@760", "450,330@0 450,370@755", "665,904"),
    "M2": (1761, "esc@1580", "450,330@0 450,370@665 lmb+@670 lmb-@672 rmb+@1700 rmb-@1702", "1520"),
}
HF = "engine/src/view/hud_frame.cpp"
HUD = "engine/src/osi/natives/hud.cpp"
OBJ = "engine/src/osi/natives/objects.cpp"
HS = "engine/src/osi/natives/host_sources.cpp"
INI = "engine/src/fs/ini.cpp"
PINI = "tools/research/osi_natives/ini.py"

# (id, name, file, old, new, {check: (arg, EXPECT regex)})
M = [
    # K1: BionicleFont (the menus' only font) has kerning 0, so the menu runs cannot see it (NO-EFFECT there, 2026-11-17):
    # the A11 unit case "kerning enters each advance with its sign" is the named check.
    ("K1", "kerning sign flipped (ScTileFont::Draw)", HF,
     "  const float kern = static_cast<float>(tf.kerning);",
     "  const float kern = -static_cast<float>(tf.kerning);",
     {"ctest": ("hud frame: kerning*", r"^CTEST ")}),
    ("K2", "sethudposition x/y swapped", HUD,
     '"rect", hex32({w(c, 1), w(c, 2), w(c, 3), w(c, 4)}));',
     '"rect", hex32({w(c, 2), w(c, 1), w(c, 4), w(c, 3)}));',
     {"menu": (None, r"^M[12] Z line \d+ ")}),  # the digest's Z lines: one hash per frame
    ("K3", "GcHudDirector::Hide a no-op", HUD,
     '  GameObject* o = node_object(ctx, id, "GcHudDirector::Hide");\n  if (!object_shown(ctx, o)) return nothing();',
     '  GameObject* o = node_object(ctx, id, "GcHudDirector::Hide");\n  if (o || !o) return nothing();',
     {"menu": (None, r"^M[12] Z line \d+ ")}),
    ("K4", "camera spline duration x 2", OBJ,
     "  cam.duration = static_cast<double>(wf(c, dur));",
     "  cam.duration = static_cast<double>(wf(c, dur)) * 2.0;",
     {"menu": (None, r"^M[12] (LOG|D) line \d+ .*(OnEvent|cpp='[KD] )")}),
    ("K5", "OnEvent(26) skipped on odd frames", HS,
     "  calls.push_back({make_int(26), make_int(st.x), make_int(st.y)});",
     "  if (frame % 2 == 0) calls.push_back({make_int(26), make_int(st.x), make_int(st.y)});",
     {"menu": (None, r"^M[12] (D|LOG) line \d+ ")}),
    ("K6", "the Urinate gate's compare with \"Pohatu\" inverted (BadCheat 0x5d4e40)", PINI,
     '        return self._is(b"Cheat", b"Urinate", b"Pohatu")',
     '        return not self._is(b"Cheat", b"Urinate", b"Pohatu")',
     {"pytest": ("tools/tests/test_ini.py", r"^PYTEST ")}),
    ("K7", "INI: last match instead of the first on a duplicated key", INI,
     "  for (const Entry& e : entries_)  // file order: the first match wins\n    if (e.section == s && e.key == k) return &e;\n  return nullptr;",
     "  const Entry* r = nullptr;\n  for (const Entry& e : entries_)\n    if (e.section == s && e.key == k) r = &e;\n  return r;",
     {"ctest": ("ini crafted table*", r"^CTEST ")}),
    ("CTRL", "no-op control (comment only; must be MISSED)", HUD,
     "// M2a-3 T6a: the HUD setters", "// M2a-3 T6a (mutation control): the HUD setters",
     {"menu": (None, None), "ctest": ("hud frame*", None)}),
]
LOGDIR: Path | None = None


def is_control(mid: str) -> bool:
    return mid == "CTRL"


def sh(cmd, tree, timeout, log=None):
    mutate_vm.LOGDIR = LOGDIR
    return mutate_vm.sh(cmd, tree, timeout, log)


def install() -> Path:
    d = os.environ.get("LOMN_DATA", "")
    p = Path(d) / "install"
    if not d or not (p / "data" / "Base.osi").is_file():
        raise SystemExit("mutate_menu: LOMN_DATA must name a data root with install/data/Base.osi")
    return p


def grep_prefix(src: Path, prefix: bytes) -> Path:
    q = src.with_suffix(src.suffix + "." + prefix.decode().strip())
    q.write_bytes(b"".join(l for l in src.read_bytes().splitlines(keepends=True) if l.startswith(prefix))
                  if src.is_file() else b"")
    return q


def cpp_menu(tree: Path, out: Path, tag: str) -> bool:
    ok = True
    for r, (n, k, m, s) in RUNS.items():
        d = out / r
        d.mkdir(parents=True, exist_ok=True)
        for f in d.glob("cpp.*"):  # only this leg's files: the oracle dir also holds the Python legs
            f.unlink()
        rc, _, _ = sh(f"timeout 300 build/native/lomn_view --install '{install()}' --script 1 --natives real --frames {n} "
                      f"--keys '{k}' --mouse '{m}' --digest '{d}/cpp.digest' --natives-log '{d}/cpp.natives.log' "
                      f"--state-log '{d}/cpp.state' --shot-frames {s} > '{d}/cpp.summary'", tree, 330, f"{tag}-{r}.log")
        ok &= rc == 0
        grep_prefix(d / "cpp.digest", b"D ")
        grep_prefix(d / "cpp.digest", b"Z ")
    return ok


def oracle(tree: Path) -> Path:
    """Python own-models legs and the pristine C++ legs of M1/M2, made once per source state."""
    d = tree / "build" / "mutm-oracle"
    base = install() / "data" / "Base.osi"
    h = hashlib.sha256()
    srcs = [tree / "tools/research/osi_vm.py", *sorted((tree / "tools/research/osi_natives").glob("*.py")),
            *sorted((tree / "data/osi").glob("*.json")), *sorted((tree / "engine/src").rglob("*.cpp")), base]
    for p in srcs:
        h.update(p.name.encode() + b"\0" + p.read_bytes())
    stamp = h.hexdigest()
    if (d / "stamp").is_file() and (d / "stamp").read_text() == stamp:
        return d
    shutil.rmtree(d, ignore_errors=True)
    tables = "--natives data/osi/natives.json --mods data/osi/natives_beta150.json --types data/osi/types.json"
    for r, (n, k, m, _) in RUNS.items():
        (d / r).mkdir(parents=True)
        rc, o, _ = sh(f".venv/bin/python tools/research/osi_vm.py --run '{base}' {tables} --real-natives --install "
                      f"'{install()}' --frames {n} --keys '{k}' --mouse '{m}' --digest build/mutm-oracle/{r}/py.digest "
                      f"--log-out build/mutm-oracle/{r}/py.natives.log", tree, 1800, f"oracle-py-{r}.log")
        if rc or "stop: none" not in o:
            raise SystemExit(f"mutate_menu: the Python own-models leg {r} failed (rc {rc})")
        grep_prefix(d / r / "py.digest", b"D ")
        grep_prefix(d / r / "py.digest", b"Z ")
    if not cpp_menu(tree, d, "oracle-cpp"):
        raise SystemExit("mutate_menu: the pristine C++ menu legs failed")
    for r in RUNS:
        for a, b in (("cpp.digest.D", "py.digest.D"), ("cpp.digest.Z", "py.digest.Z"), ("cpp.natives.log", "py.natives.log")):
            if not filecmp.cmp(d / r / a, d / r / b, shallow=False):
                raise SystemExit(f"mutate_menu: the PRISTINE tree already differs on {r} {a}: fix that first")
    (d / "stamp").write_text(stamp)
    return d


def check_menu(tree: Path, mid: str, ref: Path) -> tuple[list[str], bool]:
    out = tree / "build" / "mutm-run"
    lines = [] if cpp_menu(tree, out, mid) else ["STOP lomn_view failed"]
    changed = False
    for r in RUNS:
        a, b = out / r, ref / r
        lines += mutate_vm.first_diff(a / "cpp.digest.D", b / "py.digest.D", f"{r} D")
        lines += mutate_vm.first_diff(a / "cpp.natives.log", b / "py.natives.log", f"{r} LOG")
        lines += mutate_vm.first_diff(a / "cpp.digest.Z", b / "py.digest.Z", f"{r} Z")
        lines += mutate_vm.first_diff(a / "cpp.state", b / "cpp.state", f"{r} STATE")
        changed |= any(not filecmp.cmp(a / f, b / f, shallow=False)
                       for f in ("cpp.digest.D", "cpp.digest.Z", "cpp.natives.log", "cpp.state") if (a / f).is_file())
    return [l.replace("' py='", "' ref='") for l in lines], changed


def check_ctest(tree: Path, mid: str, case: str) -> list[str]:
    rc, o, _ = sh(f"timeout 300 build/native/lomn_tests -tc='{case}'", tree, 330, f"{mid}-ctest.log")
    return [] if rc == 0 else [f"CTEST {case} failed (rc {rc})"]


def check_pytest(tree: Path, mid: str, node: str) -> list[str]:
    rc, o, _ = sh(f"timeout 300 .venv/bin/python -m pytest -q -x {node}", tree, 330, f"{mid}-pytest.log")
    return [] if rc == 0 else [f"PYTEST {node} failed (rc {rc})"]


def build(tree: Path, rel: str, log: str) -> int:
    if not rel.startswith("engine/"):
        return 0
    rc, _, _ = sh("timeout 900 cmake --build build/native --target lomn_view lomn_tests", tree, 960, log)
    return rc


def main(argv: list[str]) -> int:
    if len(argv) < 2 or argv[1].startswith("--"):
        print(__doc__.split("\n\n")[1].strip())
        return 2
    tree = Path(argv[1]).resolve()
    if "--list" in argv:
        for i, (mid, name, rel, old, new, checks) in enumerate(M, 1):
            text = (tree / rel).read_text()
            line = mutate_vm.anchor_line(text, old, new) if text.count(old) == 1 else -1
            print(f"{i} {mid:4s} {rel}:{line} | {name} | " + ", ".join(f"{c}: {r}" for c, r in checks.items()))
        return 0
    ids = {e[0] for e in M}
    only = {p for p in argv[argv.index("--only") + 1].split(",") if p} if "--only" in argv else None
    if only is None and "--all" not in argv:
        print("mutate_menu: name --all or --only <ids>")
        return 2
    if only and only - ids:
        raise SystemExit("mutate_menu: unknown mutant id(s): " + ", ".join(sorted(only - ids)))
    global LOGDIR
    LOGDIR = tree / "build" / "mutlogs-menu"
    LOGDIR.mkdir(parents=True, exist_ok=True)
    pristine, active = tree / "build" / "mutm-pristine", tree / "build" / "mutm-active"
    if active.exists():
        rel = active.read_text().strip()
        shutil.copyfile(pristine / rel, tree / rel)
        active.unlink()
        print(f"restored {rel} left mutated by an interrupted run", flush=True)
    for rel in sorted({e[2] for e in M}):
        (pristine / rel).parent.mkdir(parents=True, exist_ok=True)
        if not (pristine / rel).exists():
            shutil.copyfile(tree / rel, pristine / rel)
        if not filecmp.cmp(tree / rel, pristine / rel, shallow=False):
            print(f"ERROR {rel} differs from build/mutm-pristine (stale pristine copy?)", flush=True)
            return 2
    if build(tree, "engine/", "pristine-build.log"):
        print("ERROR the pristine tree does not build", flush=True)
        return 2
    ref = oracle(tree)
    print("oracle: Python own models == pristine C++ on M1 and M2 (D, Z, natives.log)", flush=True)
    results = (LOGDIR / "results.jsonl").open("a")
    tally: dict[str, int] = {}
    ok = True
    for i, (mid, name, rel, old, new, checks) in enumerate(M, 1):
        if only and mid not in only:
            continue
        f = tree / rel
        src = f.read_text()
        if src.count(old) != 1:
            print(f"[{i}] ERROR anchor of {mid} not found exactly once in {rel}", flush=True)
            return 2
        line = mutate_vm.anchor_line(src, old, new)
        t0 = time.time()
        per, verdict = {}, "MISSED"
        active.write_text(rel)
        f.write_text(src.replace(old, new))
        try:
            if build(tree, rel, f"{mid}-build.log"):
                verdict = "BUILD-FAILED"
            else:
                caught, any_fail, no_effect = False, False, False
                for c, (arg, want) in checks.items():
                    if c == "menu":
                        fails, changed = check_menu(tree, mid, ref)
                        no_effect = not changed
                    elif c == "ctest":
                        fails = check_ctest(tree, mid, arg)
                    else:
                        fails = check_pytest(tree, mid, arg)
                    hit = [l for l in fails if want and re.search(want, l)]
                    per[c] = {"outcome": "BITE" if hit else ("OFF-TARGET" if fails else "pass"),
                              "line": (hit or fails or [""])[0][:300]}
                    caught |= bool(hit)
                    any_fail |= bool(fails)
                verdict = "CAUGHT" if caught else ("OFF-TARGET" if any_fail else "MISSED")
                if no_effect and not is_control(mid) and not caught:
                    verdict = "NO-EFFECT"
        finally:
            f.write_text(src)
            restored = filecmp.cmp(f, pristine / rel, shallow=False)
            if restored:
                active.unlink()
            rrc = build(tree, rel, f"{mid}-restore-build.log")
        total = time.time() - t0
        tally[verdict] = tally.get(verdict, 0) + 1
        ok &= (verdict == "MISSED") if is_control(mid) else (verdict == "CAUGHT")
        results.write(json.dumps({"id": mid, "name": name, "where": f"{rel}:{line}", "verdict": verdict, "checks": per,
                                  "total_s": round(total, 1), "restore_cmp": restored, "restore_build_rc": rrc}) + "\n")
        results.flush()
        detail = "; ".join(f"{c}: {v['outcome']}" + (f" ({v['line'][:160]})" if v["line"] else "") for c, v in per.items())
        print(f"[{i}] {verdict} {mid} {rel}:{line} {name} :: {detail} [{total:.0f} s, restore cmp "
              f"{'OK' if restored else 'FAILED'}, rebuild rc={rrc}]", flush=True)
        if not restored or rrc:
            print("ERROR restore failed: stopping", flush=True)
            return 2
    print("mutants: " + ", ".join(f"{v} {n}" for v, n in sorted(tally.items())) +
          " (every mutant must be CAUGHT, CTRL MISSED; NO-EFFECT = replace the mutant)", flush=True)
    for rel in sorted({e[2] for e in M}):
        same = filecmp.cmp(tree / rel, pristine / rel, shallow=False)
        ok &= same
        print(f"final restore cmp {rel}: {'OK' if same else 'DIFFERS'}", flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
