"""M2a-2: `"pending": "<task id>"` in an .expect.json (docs/osi/conformance.md "Pending programs"). The program is not run,
the runner prints `PENDING <group>/<id> (<task>)`, the summary reads `conformance: X/Y pass, P pending` (Y excludes the
pending ones), and a pending program never fails the gate and never leaves a trace. These cases cover the Python oracle
(osi_vm.py --conform), tools/osi/conform_assemble.py, tools/osi/conformance_report.py and `make conform-trace-cmp`; the C++
runner has the same case as the ctest `osi_conform_pending` (engine/tests/osi_conform_pending.cmake)."""
import importlib.util
import json
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "research"))
sys.path.insert(0, str(REPO / "tools" / "osi"))
import osi_vm  # noqa: E402
import conform_assemble  # noqa: E402

PASSING_ASM = ".function main 0\n    PushConstanti8 1\n    PushConstanti8 2\n    Add\n    Return\n"
PASSING_EXPECT = {"id": "p", "rows": ["0x70"], "entry": "main", "args": [], "natives": [], "registry": "none",
                  "expect": {"ok": True, "result": {"type": "0x10", "value": 3}, "depth": 0, "context_depth": 0, "fpu_pc": 24}}
# a pending program is for behaviour the engines do not implement: its .asm may not even assemble, and its expectation
# is one that running it could not meet
PENDING_ASM = "this is not assembly\n"
PENDING_EXPECT = {"id": "q", "pending": "T3", "rows": ["0x70"], "entry": "main", "registry": "none",
                  "expect": {"ok": True, "result": {"type": "0x10", "value": 99}}}


def _put(root, rel, asm, expect):
    d = root / Path(rel).parent
    d.mkdir(parents=True, exist_ok=True)
    stem = Path(rel).name
    if asm is not None:
        (d / (stem + ".asm")).write_text(asm)
    (d / (stem + ".expect.json")).write_text(json.dumps(expect))


def _corpus(root, pending=PENDING_EXPECT, pending_asm=PENDING_ASM):
    _put(root, "g/p", PASSING_ASM, PASSING_EXPECT)
    _put(root, "n/q", pending_asm, pending)
    return root


def test_oracle_reports_pending_and_counts_it_separately(tmp_path, capsys):
    _corpus(tmp_path)
    assert osi_vm.main(["--conform", str(tmp_path)]) == 0
    out = capsys.readouterr().out.splitlines()
    assert "PENDING n/q (T3)" in out
    assert "conformance: 1/1 pass, 1 pending" in out
    assert not [l for l in out if l.startswith("FAIL")]


def test_oracle_writes_no_trace_for_a_pending_program(tmp_path):
    _corpus(tmp_path / "c")
    assert osi_vm.main(["--conform", str(tmp_path / "c"), "--trace-dir", str(tmp_path / "t")]) == 0
    assert (tmp_path / "t" / "g" / "p.trace").is_file()
    assert not (tmp_path / "t" / "n").exists()


def test_oracle_pending_does_not_need_an_asm(tmp_path, capsys):
    _corpus(tmp_path, pending_asm=None)
    assert osi_vm.main(["--conform", str(tmp_path)]) == 0
    assert "PENDING n/q (T3)" in capsys.readouterr().out.splitlines()


def test_oracle_pending_never_hides_a_real_failure(tmp_path, capsys):
    _corpus(tmp_path)
    bad = dict(PASSING_EXPECT, expect=dict(PASSING_EXPECT["expect"], result={"type": "0x10", "value": 4}))
    _put(tmp_path, "g/bad", PASSING_ASM, bad)
    assert osi_vm.main(["--conform", str(tmp_path)]) == 1
    out = capsys.readouterr().out.splitlines()
    assert any(l.startswith("FAIL g/bad.asm") for l in out) or any(l.startswith("FAIL g/bad") for l in out)
    assert "PENDING n/q (T3)" in out
    assert "conformance: 1/2 pass, 1 pending" in out


@pytest.mark.parametrize("value", ["", None, 3, True, ["T3"]])
def test_oracle_only_a_non_empty_string_is_pending(tmp_path, capsys, value):
    # anything else runs the program: it must be able to FAIL, so a typo cannot silently skip a program
    _corpus(tmp_path, pending=dict(PENDING_EXPECT, pending=value), pending_asm=PASSING_ASM)
    assert osi_vm.main(["--conform", str(tmp_path)]) == 1
    out = capsys.readouterr().out
    assert "PENDING" not in out and "conformance: 1/2 pass, 0 pending" in out


def test_oracle_only_pending_programs_is_success(tmp_path, capsys):
    _put(tmp_path, "n/q", PENDING_ASM, PENDING_EXPECT)
    assert osi_vm.main(["--conform", str(tmp_path)]) == 0
    assert "conformance: 0/0 pass, 1 pending" in capsys.readouterr().out


def test_oracle_empty_corpus_is_still_a_failure(tmp_path, capsys):
    assert osi_vm.main(["--conform", str(tmp_path)]) == 1
    assert "conformance: 0/0 pass, 0 pending" in capsys.readouterr().out


def test_oracle_summary_keeps_the_old_prefix(tmp_path, capsys):
    _put(tmp_path, "g/p", PASSING_ASM, PASSING_EXPECT)
    osi_vm.main(["--conform", str(tmp_path)])
    assert "conformance: 1/1 pass, 0 pending" in capsys.readouterr().out
    # every consumer matches on the prefix "conformance: N/M" (tools/research/oracle/mutate_vm.py FAIL_RE)
    import re
    assert re.search(r"conformance: \d+/\d+", "conformance: 1/1 pass, 0 pending")


def test_assembler_step_skips_a_pending_program(tmp_path, capsys):
    _corpus(tmp_path / "c")
    assert conform_assemble.main([str(tmp_path / "c"), str(tmp_path / "o")]) == 0
    assert "assembled 1 program(s), 0 failed, 1 pending skipped" in capsys.readouterr().out
    assert (tmp_path / "o" / "g" / "p.osi").is_file()
    assert not (tmp_path / "o" / "n" / "q.osi").exists()


def _load(name, rel):
    spec = importlib.util.spec_from_file_location(name, REPO / rel)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_report_counts_pending_separately(tmp_path, capsys):
    cr = _load("conformance_report", "tools/osi/conformance_report.py")
    _corpus(tmp_path)
    r = cr.report(tmp_path, REPO / "docs" / "osi" / "opcodes.md", REPO / "docs" / "osi" / "types.md")
    assert r["programs"] == 1 and r["pending"] == 1
    assert r["handlers"][0] == 0                  # row 0x70 is cited by both, but a pending program covers nothing
    assert cr.main([str(tmp_path)]) == 0
    assert capsys.readouterr().out.strip().endswith("programs: 1, pending: 1")


def test_report_without_pending_says_pending_zero(tmp_path, capsys):
    cr = _load("conformance_report", "tools/osi/conformance_report.py")
    _put(tmp_path, "g/p", PASSING_ASM, PASSING_EXPECT)
    assert cr.main([str(tmp_path)]) == 0
    assert capsys.readouterr().out.strip().endswith("programs: 1, pending: 0")


# ---- make conform-trace-cmp over a temporary corpus ----------------------------------------------------------------

def _make_trace_cmp(corpus, outdir):
    assert (REPO / "build" / "native" / "lomn_osi_conform").is_file(), "build/native is missing: run `make native` first"
    assert shutil.which("make")
    # -o: the prerequisites (cmake build, venv) are done by the caller; this runs only the comparison recipe
    return subprocess.run(["make", "-o", "native", "-o", ".venv/.ok", "conform-trace-cmp", "CONFORM_CORPUS=" + str(corpus),
                           "CONFORM_TRACE_DIR=" + str(outdir)], cwd=REPO, capture_output=True, text=True, timeout=110)


def test_trace_cmp_skips_a_pending_program_on_both_sides(tmp_path):
    _corpus(tmp_path / "c")
    p = _make_trace_cmp(tmp_path / "c", tmp_path / "o")
    out = p.stdout
    assert p.returncode == 0, out + p.stderr
    assert "PENDING n/q (T3)" in out.splitlines()
    assert "conformance: 1/1 pass, 1 pending" in out
    assert "conform-trace-cmp: 1/1 traces byte-identical (C++ vs Python)" in out
    assert "conform-trace-cmp: 1 pending (no trace on either side)" in out
    assert "DIFF" not in out and "MISSING" not in out
    assert not list((tmp_path / "o").rglob("q.trace"))


def test_trace_cmp_reports_zero_pending_when_there_is_none(tmp_path):
    _put(tmp_path / "c", "g/p", PASSING_ASM, PASSING_EXPECT)
    p = _make_trace_cmp(tmp_path / "c", tmp_path / "o")
    assert p.returncode == 0, p.stdout + p.stderr
    assert "conform-trace-cmp: 1/1 traces byte-identical (C++ vs Python)" in p.stdout
    assert "conform-trace-cmp: 0 pending (no trace on either side)" in p.stdout
