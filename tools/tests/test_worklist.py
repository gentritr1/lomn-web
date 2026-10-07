import json

from worklist import build, check, dashboard, main


def gen_dir(tmp_path):
    g = tmp_path / "gen"
    g.mkdir()
    (g / "functions.jsonl").write_text('{"addr":"0x1","name":"A::f"}\n{"addr":"0x2","name":"A::g"}\n')
    (g / "function_map.json").write_text(json.dumps({"0x1": "Area/A.cpp", "0x2": "Area/A.cpp"}))
    (g / "seeds.json").write_text(json.dumps({"m1-required": ["0x1"], "m1-candidate": ["0x2"]}))
    return g


def test_build_defaults_and_tags(tmp_path):
    rows = build(gen_dir(tmp_path), {})
    assert rows[0] == {"addr": "0x1", "name": "A::f", "source": "Area/A.cpp", "tags": ["m1-required"],
                       "status": "decompiled", "port_file": None, "test": None}


def test_set_then_check_ported_requires_file(tmp_path):
    g, st = gen_dir(tmp_path), tmp_path / "status.json"
    assert main(["set", str(st), "0x1", "ported", "--port-file", "engine/src/A.cpp"]) == 0
    errs = check(build(g, json.loads(st.read_text())), tmp_path, None)
    assert len(errs) == 1 and "port_file missing" in errs[0]
    (tmp_path / "engine" / "src").mkdir(parents=True)
    (tmp_path / "engine" / "src" / "A.cpp").write_text("//")
    assert check(build(g, json.loads(st.read_text())), tmp_path, None) == []


def test_tested_without_test_name_fails(tmp_path):
    g = gen_dir(tmp_path)
    (tmp_path / "x.cpp").write_text("")
    errs = check(build(g, {"0x1": {"status": "tested", "port_file": "x.cpp"}}), tmp_path, None)
    assert errs and "without a test name" in errs[0]


def test_dashboard_counts(tmp_path):
    html = dashboard(build(gen_dir(tmp_path), {"0x1": {"status": "cleaned"}}))
    assert "<td>cleaned</td><td>1</td>" in html and "M1-required (1)" in html


def test_set_merges_fields(tmp_path):
    st = tmp_path / "status.json"
    assert main(["set", str(st), "0x1", "ported", "--port-file", "X"]) == 0
    assert main(["set", str(st), "0x1", "tested", "--test", "T"]) == 0
    assert json.loads(st.read_text())["0x1"] == {"status": "tested", "port_file": "X", "test": "T"}


def test_check_escapes_test_name(tmp_path, monkeypatch):
    import subprocess
    seen = []

    def fake_run(argv, **kw):
        seen.append(argv)
        return subprocess.CompletedProcess(argv, 0, "", "")

    monkeypatch.setattr("worklist.subprocess.run", fake_run)
    (tmp_path / "x.cpp").write_text("")
    rows = build(gen_dir(tmp_path), {"0x1": {"status": "tested", "port_file": "x.cpp", "test": "a.b+c"}})
    assert check(rows, tmp_path, tmp_path / "b") == []
    assert seen[0][seen[0].index("-R") + 1] == r"^a\.b\+c$"


def test_check_missing_ctest_reports_error(tmp_path, monkeypatch):
    def fake_run(argv, **kw):
        raise FileNotFoundError("ctest")

    monkeypatch.setattr("worklist.subprocess.run", fake_run)
    (tmp_path / "x.cpp").write_text("")
    rows = build(gen_dir(tmp_path), {"0x1": {"status": "tested", "port_file": "x.cpp", "test": "t"}})
    errs = check(rows, tmp_path, tmp_path / "b")
    assert len(errs) == 1 and "ctest not found" in errs[0]
