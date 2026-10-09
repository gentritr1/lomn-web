"""M2a-2: the conformance runners FAIL CLOSED on expectation keys nobody compares (docs/osi/conformance.md amendment 18).
Each runner keeps its own list of the key paths it compares plus an explicit list of descriptive keys it ignores; a
non-pending program with any other key path fails with `FAIL <id>: uncompared key <path>` before it runs. These cases:
the two lists are the same set; the existing corpus has no uncompared key; a planted key fails on BOTH runners (and a
pending program carrying it is PENDING, since it is not run). The C++ side needs build/native (make test-tools builds it)."""
import json
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "research"))
sys.path.insert(0, str(REPO / "tools" / "osi"))
import osi_vm  # noqa: E402
import conform_assemble  # noqa: E402

CPP = REPO / "build" / "native" / "lomn_osi_conform"
ASM = ".function main 0\n    PushConstanti8 1\n    PushConstanti8 2\n    Add\n    Return\n"
BASE = {"id": "p", "rows": ["0x70"], "entry": "main", "args": [], "natives": [], "registry": "none",
        "expect": {"ok": True, "result": {"type": "0x10", "value": 3}, "depth": 0, "context_depth": 0, "fpu_pc": 24}}


def _cpp(*args, timeout=60):
    assert CPP.is_file(), "build/native is missing: run `make native` first"
    return subprocess.run([str(CPP), *map(str, args)], capture_output=True, text=True, timeout=timeout)


def test_the_two_runners_compare_the_same_key_set():
    cpp = _cpp("--list-keys")
    assert cpp.returncode == 0
    py = []
    for kind, keys in (("compared", osi_vm.COMPARED_KEYS), ("ignored", osi_vm.IGNORED_KEYS)):
        py += ["%s %s" % (kind, k) for k in sorted(keys)]
    assert cpp.stdout.splitlines() == py
    assert len(osi_vm.COMPARED_KEYS) == 105      # a change to the list must be a visible change to this number


def test_the_ignored_list_is_exactly_the_descriptive_keys():
    assert osi_vm.IGNORED_KEYS == {"id", "rows", "note", "alternative", "falsifier", "falsifier_id", "argc_violation", "pending"}
    assert not (osi_vm.IGNORED_KEYS & osi_vm.COMPARED_KEYS)


def test_the_existing_corpus_has_no_uncompared_key():
    files = sorted((REPO / "tests" / "osi" / "conformance").glob("*/*.expect.json"))
    assert len(files) > 500
    bad = {f.name: osi_vm.uncompared_keys(json.loads(f.read_text())) for f in files}
    assert {k: v for k, v in bad.items() if v} == {}


# (where the key is planted, the document edit, the reported path)
def _plant(doc, where):
    d = json.loads(json.dumps(doc))
    if where == "expect.pc_after":
        d["expect"]["pc_after"] = 5
    elif where == "top.then":
        d["then"] = []
    elif where == "expect.result.object":
        d["expect"]["result"]["object"] = 1
    elif where == "expect.top":
        d["expect"]["top"] = {"type": "0x10", "value": 3}
    elif where == "expect.globals.g.frob":
        d["expect"]["globals"] = {"g": {"type": "0x10", "value": 1, "frob": 2}}
    elif where == "expect.heap.live.x":
        d["expect"]["heap"] = {"live": {"x": 1}}
    elif where == "args[*].swallowed":
        d["args"] = [{"type": "0x10", "value": 1, "swallowed": True}]
    elif where == "natives[*].frob":
        d["natives"] = [{"cls": "t", "name": "f", "min": 0, "max": 0, "types": [], "frob": 1}]
    elif where == "natives[*].returns.object":
        d["natives"] = [{"cls": "t", "name": "f", "min": 0, "max": 0, "types": [], "returns": {"object": 1}}]
    elif where == "expect.found":
        d["expect"]["found"] = True
    elif where == "expect_error.pc_after":
        d["expect_error"] = {"message": "x", "pc_after": 1}
        del d["expect"]
    return d


WHERES = ["expect.pc_after", "top.then", "expect.result.object", "expect.top", "expect.globals.g.frob", "expect.heap.live.x",
          "args[*].swallowed", "natives[*].frob", "natives[*].returns.object", "expect.found", "expect_error.pc_after"]
PATHS = {"top.then": "then", "expect.globals.g.frob": "expect.globals.g.frob", "args[*].swallowed": "args[*].swallowed",
         "natives[*].frob": "natives[*].frob", "natives[*].returns.object": "natives[*].returns.object"}


def _write(root, rel, doc, asm=ASM):
    d = root / Path(rel).parent
    d.mkdir(parents=True, exist_ok=True)
    (root / (rel + ".asm")).write_text(asm)
    (root / (rel + ".expect.json")).write_text(json.dumps(doc))


def _run_both(tmp_path, doc):
    """(oracle rc, oracle out, C++ rc, C++ out, trace dirs) on a corpus holding just `doc` as g/x."""
    c = tmp_path / "c"
    _write(c, "g/x", doc)
    assert conform_assemble.main([str(c), str(tmp_path / "osi")]) == 0
    py = subprocess.run([sys.executable, str(REPO / "tools" / "research" / "osi_vm.py"), "--conform", str(c), "--trace-dir",
                         str(tmp_path / "tpy")], capture_output=True, text=True, timeout=60)
    cpp = _cpp(c, "--osi-dir", tmp_path / "osi", "--trace-dir", tmp_path / "tcpp")
    return py, cpp


@pytest.mark.parametrize("where", WHERES)
def test_an_unknown_key_fails_on_both_runners_and_leaves_no_trace(tmp_path, where):
    key = PATHS.get(where, where)
    py, cpp = _run_both(tmp_path, _plant(BASE, where))
    for r in (py, cpp):
        assert r.returncode == 1, r.stdout + r.stderr
        assert "uncompared key %s" % key in r.stdout, r.stdout
        assert "conformance: 0/1 pass, 0 pending" in r.stdout
    assert not list(tmp_path.glob("t*/**/*.trace"))          # failed closed before running


@pytest.mark.parametrize("where", ["expect.pc_after", "expect.top", "top.then"])
def test_the_same_program_marked_pending_is_reported_pending(tmp_path, where):
    doc = _plant(BASE, where)
    doc["pending"] = "T3"
    py, cpp = _run_both(tmp_path, doc)
    for r in (py, cpp):
        assert r.returncode == 0, r.stdout + r.stderr
        assert "PENDING g/x (T3)" in r.stdout.splitlines()
        assert "uncompared" not in r.stdout and "conformance: 0/0 pass, 1 pending" in r.stdout


def test_descriptive_keys_and_global_names_are_allowed(tmp_path):
    doc = json.loads(json.dumps(BASE))
    doc.update({"note": "n", "alternative": "a", "falsifier": "f", "falsifier_id": ["0x70#1"], "argc_violation": "k/1#1",
                "pending": ""})
    doc["expect"]["globals"] = {}
    py, cpp = _run_both(tmp_path, doc)
    for r in (py, cpp):
        assert r.returncode == 0, r.stdout + r.stderr
        assert "conformance: 1/1 pass, 0 pending" in r.stdout


def test_expect_error_state_keys_are_compared_by_both_runners(tmp_path):
    # context_depth / fpu_pc / globals / heap under expect_error: the oracle always compared them, C++ did not until amendment 18
    asm = ".function main 0\n    Pop\n    Return\n"
    err = {"id": "e", "entry": "main", "args": [], "natives": [], "registry": "none",
           "expect_error": {"message": "Stack underflow", "context_depth": 99}}
    py, cpp = _run_both_asm(tmp_path / "bad", err, asm)
    for r in (py, cpp):
        assert r.returncode == 1 and "context_depth" in r.stdout and "message" not in r.stdout, r.stdout
    err["expect_error"]["context_depth"] = 1
    py, cpp = _run_both_asm(tmp_path / "good", err, asm)
    for r in (py, cpp):
        assert r.returncode == 0, r.stdout


def _run_both_asm(tmp_path, doc, asm):
    c = tmp_path / "c"
    _write(c, "g/x", doc, asm)
    assert conform_assemble.main([str(c), str(tmp_path / "osi")]) == 0
    py = subprocess.run([sys.executable, str(REPO / "tools" / "research" / "osi_vm.py"), "--conform", str(c)],
                        capture_output=True, text=True, timeout=60)
    return py, _cpp(c, "--osi-dir", tmp_path / "osi")
