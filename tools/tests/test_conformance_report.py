"""M2a-1 T0: tools/osi/conformance_report.py (docs/osi/conformance.md §5). Written after the script (not test-first);
the denominators were measured by running it on an empty corpus on 2026-10-08."""
import importlib.util
import json
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]


@pytest.fixture(scope="module")
def cr():
    spec = importlib.util.spec_from_file_location("conformance_report", REPO / "tools" / "osi" / "conformance_report.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _report(cr, d, group=None):
    return cr.report(d, REPO / "docs" / "osi" / "opcodes.md", REPO / "docs" / "osi" / "types.md", group)


def test_measured_denominators(cr, tmp_path):
    r = _report(cr, tmp_path)
    assert (r["handlers"], r["pairs"], r["falsifiers"], r["argc"], r["programs"]) == ((0, 96), (0, 111), (0, 31), (0, 20), 0)
    a, b = _report(cr, tmp_path, "a"), _report(cr, tmp_path, "b")
    assert (a["handlers"][1], a["pairs"][1], a["falsifiers"][1], a["argc"][1]) == (48, 60, 15, 0)
    assert (b["handlers"][1], b["pairs"][1], b["falsifiers"][1], b["argc"][1]) == (48, 51, 16, 20)


def test_counts_from_metadata_only(cr, tmp_path):
    progs = {
        "pop-1": {"rows": ["0x30", "0x40"], "expect": {"ok": True}},
        "pop-2": {"rows": ["0x30"], "expect": {"ok": True}, "falsifier_id": ["0x70#2", "0x70#3"]},
        "eq-rhs": {"rows": ["0x60"], "expect_error": {"message": "Invalid operand to operator '=' (rhs) (type:16)"}},
        "argc-1": {"rows": ["0x82"], "argc_violation": "gcareadirector.hide/1#3",
                   "expect_error": {"message": "Script is calling gcareadirector.hide"}},
    }
    for k, v in progs.items():
        (tmp_path / (k + ".expect.json")).write_text(json.dumps(v))
        (tmp_path / (k + ".asm")).write_text("this text is never read\n")
    r = _report(cr, tmp_path)
    assert r["handlers"] == (1, 96) and r["programs"] == 4
    assert r["pairs"] == (2, 111)            # 0x60 rhs (formats match any text) and 0x82 "Script is calling %s.%s"
    assert r["falsifiers"] == (2, 31) and r["argc"] == (1, 20)
    assert "0x30" not in r["missing_handlers"] and "0x40" in r["missing_handlers"]


def test_readme_repeats_the_authoring_rules():
    rules = (REPO / "docs" / "osi" / "conformance.md").read_text().split(
        "## 2. Authoring rules (independence of both implementations)\n", 1)[1].split("\n## 3.", 1)[0].strip("\n")
    assert rules in (REPO / "tests" / "osi" / "conformance" / "README.md").read_text()
