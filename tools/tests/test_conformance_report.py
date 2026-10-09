"""M2a-1 T0: tools/osi/conformance_report.py (docs/osi/conformance.md §5). Written after the script (not test-first);
the denominators were measured by running it on an empty corpus on 2026-10-08."""
import importlib.util
import json
import re
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


CORPUS = REPO / "tests" / "osi" / "conformance"
_NON_VALUE_FORMS = ("text", "hex", "game")   # docs/osi/conformance.md §1: these need no value/bits


def _value_specs(doc):
    """(where, spec) for every value expectation: expect.result, expect.globals.<name>, expect_error.top, and the
    native-framework fields of M2a-2 T2b: expect.top, then.result, then.top, <expect|expect_error>.swallowed[i].top."""
    ok, err, then = doc.get("expect") or {}, doc.get("expect_error") or {}, doc.get("then") or {}
    out = [("expect.result", ok["result"])] if "result" in ok else []
    out += [("expect.globals." + name, spec) for name, spec in (ok.get("globals") or {}).items()]
    if "top" in ok:
        out.append(("expect.top", ok["top"]))
    if "top" in err:
        out.append(("expect_error.top", err["top"]))
    out += [("then." + k, then[k]) for k in ("result", "top") if k in then]
    for where, block in (("expect", ok), ("expect_error", err)):
        out += [("%s.swallowed[%d].top" % (where, i), s["top"]) for i, s in enumerate(block.get("swallowed") or [])
                if "top" in s]
    return out


def _has_key(node, key):
    if isinstance(node, dict):
        return key in node or any(_has_key(v, key) for v in node.values())
    if isinstance(node, list):
        return any(_has_key(v, key) for v in node)
    return False


def _problems(doc):
    # null is "expected empty" (e.g. expect_error.top: null = depth 0), so it is not a value expectation
    out = [where + " has neither value nor bits" for where, spec in _value_specs(doc)
           if spec is not None and not any(k in spec for k in _NON_VALUE_FORMS + ("value", "bits"))]
    if _has_key(doc, "found"):
        out.append("found key present (not part of the format)")
    return out


def _scan(root):
    """{path: problems} for every <group>/<id>.expect.json under root that breaks the §1 rule."""
    bad = {}
    for path in sorted(root.glob("*/*.expect.json")):
        problems = _problems(json.loads(path.read_text()))
        if problems:
            bad[str(path.relative_to(root))] = problems
    return bad


def test_corpus_value_expectations_have_value_or_bits_and_no_found():
    assert len(list(CORPUS.glob("*/*.expect.json"))) > 0   # the scan must not be vacuous
    assert _scan(CORPUS) == {}


@pytest.mark.parametrize("doc, want", [
    ({"expect": {"result": {"type": "0x10"}}}, ["expect.result has neither value nor bits"]),
    ({"expect": {"globals": {"g": {"type": "0x12"}}}}, ["expect.globals.g has neither value nor bits"]),
    ({"expect_error": {"top": {"type": "0x12"}}}, ["expect_error.top has neither value nor bits"]),
    ({"expect": {"ok": True, "found": True}}, ["found key present (not part of the format)"]),
    ({"expect": {"result": {"type": "0x10", "value": 0}}}, []),
    ({"expect": {"result": {"type": "0x12", "bits": "3f800000"}}}, []),
    ({"expect": {"result": {"text": "abc"}}, "expect_error": {"top": {"game": "GcTimer"}}}, []),
    # M2a-2 T2b (conformance.md §1 "Native-framework fields"): the new value expectations follow the same rule
    ({"expect": {"top": {"type": "0x10"}}}, ["expect.top has neither value nor bits"]),
    ({"expect": {"top": None}}, []),
    ({"then": {"result": {"type": "0x10"}}}, ["then.result has neither value nor bits"]),
    ({"then": {"top": {"type": "0xff"}}}, ["then.top has neither value nor bits"]),
    ({"expect": {"swallowed": [{"top": {"type": "0x10", "value": 4}}, {"top": {"type": "0x10"}}]}},
     ["expect.swallowed[1].top has neither value nor bits"]),
    ({"expect_error": {"swallowed": [{"top": {"type": "0xff"}}]}},
     ["expect_error.swallowed[0].top has neither value nor bits"]),
])
def test_guard_flags_a_value_expectation_without_value_or_bits(doc, want):
    assert _problems(doc) == want


def test_guard_flags_a_planted_file(tmp_path):
    (tmp_path / "x").mkdir()
    (tmp_path / "x" / "bad.expect.json").write_text(json.dumps({"expect": {"result": {"type": "0x10"}}}))
    assert _scan(tmp_path) == {"x/bad.expect.json": ["expect.result has neither value nor bits"]}


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


def test_list_prints_each_program_with_its_status(cr, tmp_path, capsys):
    """--list (M2a-2 T2b): one line per program, sorted by id, `pending <task>` or `active` (conformance.md §1:
    `"pending": "<task id>"`, a string)."""
    (tmp_path / "x").mkdir()
    for k, v in {"n-b": {"id": "n-b", "pending": "T3/T4"}, "n-a": {"id": "n-a"}}.items():
        (tmp_path / "x" / (k + ".expect.json")).write_text(json.dumps(v))
    assert cr.main([str(tmp_path), "--list"]) == 0
    out = capsys.readouterr().out.splitlines()
    assert out[1:] == ["program n-a active", "program n-b pending T3/T4"]


TASK_IDS = re.compile(r"T\d+[ab]?(/T\d+[ab]?)*$")      # e.g. "T3/T4": the task(s) whose runner features are missing


def _readme_status():
    """{program id: "pending" | "active"} from the group-n table of tests/osi/conformance/README.md."""
    text = (CORPUS / "README.md").read_text()
    return dict(re.findall(r"^\| `(n-[a-z0-9-]+)` \| (pending|active) \|", text, re.M))


def test_pending_marks_are_task_ids_and_match_the_readme():
    """Fix 1 (review m-4): no hardcoded count. Every `pending` value is a task-id string, and the README's group-n
    status table agrees with the files program by program, so Tasks 3/4 only remove keys and README rows change."""
    files = {}
    for p in sorted(CORPUS.glob("*/*.expect.json")):
        doc = json.loads(p.read_text())
        if "pending" in doc:
            assert isinstance(doc["pending"], str) and TASK_IDS.match(doc["pending"]), (p.name, doc["pending"])
        if p.parent.name == "n":
            files[doc["id"]] = "pending" if "pending" in doc else "active"
    assert files and _readme_status() == files


def test_readme_repeats_the_authoring_rules():
    rules = (REPO / "docs" / "osi" / "conformance.md").read_text().split(
        "## 2. Authoring rules (independence of both implementations)\n", 1)[1].split("\n## 3.", 1)[0].strip("\n")
    assert rules in (REPO / "tests" / "osi" / "conformance" / "README.md").read_text()
