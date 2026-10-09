"""M2a-2 T2b (T2a-pc review M5): tools/osi/error_pc_crosscheck.py, the oracle cross-check of the error-PC table.

It runs every `expect_error` program of corpus groups a and b through the Python oracle (tools/research/osi_vm.py,
`Vm.call` + `snapshot()`) and compares `snapshot().pc - error.pc` with docs/osi/opcodes.md "Error PC (the PC at the
throw), per handler". The expected split is the one the T2a-pc implementer and its blind reviewer measured with their
own scratch scripts (2026-10-09): 178 error programs = 173 errors the original raises that match the table's "PC at
its errors" column, 2 port stack-cap errors that match its "PC at its ScOSIStack calls" column, and 3 rule cases
(conformance.md rule 15(c)/(b): the literal 0x84, SetPCAndArgs argc > 10, the nesting cap). No mismatch.

Group n is not checked: most of its programs are pending Tasks 3/4 (tests/osi/conformance/README.md)."""
import importlib.util
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]


@pytest.fixture(scope="module")
def xc():
    spec = importlib.util.spec_from_file_location("osi_error_pc_crosscheck", REPO / "tools" / "osi" / "error_pc_crosscheck.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules["osi_error_pc_crosscheck"] = mod
    spec.loader.exec_module(mod)
    return mod


def test_table_columns_are_parsed_from_opcodes_md(xc):
    t = xc.load_table(REPO / "docs" / "osi" / "opcodes.md")
    assert t[0x02]["errors"] == "+1" and t[0x50]["errors"] == "+2" and t[0x53]["errors"] == "+0"
    assert t[0x82]["errors"] == "+5 / +6" and t[0x83]["errors"] == "+3 / +4" and t[0x84]["errors"] == "+6"
    assert t[0x85]["errors"] == "+0" and t[0x01]["errors"] == "+0"          # the 160 unused ops share one row
    assert t[0x30]["stack"] == "pop +1" and t[0x40]["stack"] == "push +5"


@pytest.mark.parametrize("op, message, want", [
    (0x02, "Debugger not enabled! but encountered DebugOn", ("table", 1)),
    (0x82, "Call to unregistered Game Function", ("table", 5)),
    (0x82, "Script is calling t.echo", ("table", 6)),
    (0x83, "Invalid member type for function call (must be a string)", ("table", 3)),
    (0x83, "kOutOfRange", ("table", 3)),
    (0x83, "Script is calling t.x", ("table", 4)),
    (0x30, "Stack underflow", ("stack", 1)),
    (0x40, "Stack overflow (1000 slots)", ("stack", 5)),
    (0x84, "Direct call without a memoised site", ("rule", 0)),
    (0x52, "Too many args required (max 10) to call requested entry point.", ("rule", 0)),
])
def test_expected_delta(xc, op, message, want):
    t = xc.load_table(REPO / "docs" / "osi" / "opcodes.md")
    assert xc.expected(t, op, message, nested=False) == want


def test_nested_errors_are_rule_15b(xc):
    t = xc.load_table(REPO / "docs" / "osi" / "opcodes.md")
    assert xc.expected(t, 0x82, "Script is calling t.reenter", nested=True) == ("rule", None)


def test_corpus_groups_a_and_b_agree_with_the_table(xc):
    res = xc.crosscheck([REPO / "tests" / "osi" / "conformance" / g for g in ("a", "b")])
    assert [r for r in res if r["verdict"] == "MISMATCH"] == []
    counts = {}
    for r in res:
        counts[r["kind"]] = counts.get(r["kind"], 0) + 1
    assert len(res) == 178
    assert counts == {"table": 173, "stack": 2, "rule": 3}
