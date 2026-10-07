"""M1b: the BYO page's build identification (web/app/fingerprint.js, run through web/tools/fingerprint_cli.mjs) equals
the independent Python oracle (tools/byo_fingerprint.py) on crafted installs, and on the real install (LOMN_DATA) it is
the fingerprint tools/builds.json accepts."""
from __future__ import annotations

import json
import shutil
import struct
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
import byo_fingerprint  # noqa: E402

NODE = shutil.which("node")


def blk(count: int, tail: bytes = b"") -> bytes:
    return b"BLKF" + struct.pack("<II", 7, count) + bytes(range(56)) * count + tail


def js_fingerprint(root: Path) -> dict:
    out = subprocess.run([NODE, str(REPO / "web/tools/fingerprint_cli.mjs"), str(root)], capture_output=True, text=True, timeout=120)
    assert out.returncode == 0, out.stderr
    return json.loads(out.stdout)


@pytest.mark.skipif(NODE is None, reason="node not found")
def test_js_equals_python_on_crafted_installs(tmp_path: Path) -> None:
    root = tmp_path / "inst"
    (root / "data" / "Levels" / "lev1").mkdir(parents=True)
    (root / "data" / "Art").mkdir(parents=True)
    (root / "LEGOBionicle.exe").write_bytes(b"not a real exe")
    (root / "data" / "Art" / "Art.blk").write_bytes(blk(3, b"x" * 100))
    (root / "data" / "Levels" / "lev1" / "VLLG.blk").write_bytes(blk(0))
    (root / "data" / "Levels" / "lev1" / "lev1.blk").write_bytes(blk(1, b"entry bytes"))
    (root / "data" / ".hidden.blk").write_bytes(b"junk: dot files are skipped")
    (root / "data" / "Base.osi").write_bytes(b"osi")
    py = byo_fingerprint.fingerprint(root)
    assert py == js_fingerprint(root)
    assert py["blk_files"] == 3
    # Entry bytes after the table do not change it; a table byte does.
    (root / "data" / "Levels" / "lev1" / "lev1.blk").write_bytes(blk(1, b"other bytes"))
    assert byo_fingerprint.fingerprint(root)["blk_tables_sha256"] == py["blk_tables_sha256"]
    t = bytearray(blk(1, b"other bytes"))
    t[20] ^= 1
    (root / "data" / "Levels" / "lev1" / "lev1.blk").write_bytes(bytes(t))
    changed = byo_fingerprint.fingerprint(root)
    assert changed["blk_tables_sha256"] != py["blk_tables_sha256"]
    assert changed == js_fingerprint(root)


# Review fix 6: names sort by their UTF-8 bytes in both implementations. U+FF21 (UTF-8 EF BC A1) sorts before U+1F600
# (F0 9F 98 80) by bytes but after it by UTF-16 code unit (D83D vs FF21), which is what the JS sort used to do.
# NON_ASCII_FP is the oracle's value for exactly this install (the JS test in web/tests/unit_test.mjs builds the same one).
NON_ASCII_FP = "1d3c18ccade5d471383564557ea58b48b0231674b0ff062047d8e0a50bd5175a"
NON_ASCII_FILES = [("\uff21.blk", 1, b"a"), ("\U0001F600.blk", 1, b"bb"), ("\u00e9.blk", 0, b"ccc"), ("Zed.blk", 2, b"")]


@pytest.mark.skipif(NODE is None, reason="node not found")
def test_non_ascii_names_sort_by_utf8_bytes_in_js_and_python(tmp_path: Path) -> None:
    root = tmp_path / "inst"
    (root / "data").mkdir(parents=True)
    (root / "LEGOBionicle.exe").write_bytes(b"x")
    for name, count, tail in NON_ASCII_FILES:
        (root / "data" / name).write_bytes(blk(count, tail))
    py = byo_fingerprint.fingerprint(root)
    assert py["blk_tables_sha256"] == NON_ASCII_FP
    assert py["blk_files"] == 4
    assert js_fingerprint(root) == py


# Review fix N4: a name that is a byte prefix of another sorts first (a.blk before a.blk.blk). PREFIX_FP is the oracle's value
# for exactly this install (web/tests/unit_test.mjs builds the same one).
PREFIX_FP = "05283d38aaad7e482b04ac1f8c2b558523e80c41509b4fd1a05395cc6a72e82b"
PREFIX_FILES = [("a.blk", 1, b"a"), ("a.blk.blk", 0, b"bb"), ("ab.blk", 1, b""), ("\u00e9.blk", 2, b"c"), ("\u00e9.blk.blk", 1, b"dd")]


@pytest.mark.skipif(NODE is None, reason="node not found")
def test_prefix_names_sort_shorter_first(tmp_path: Path) -> None:
    root = tmp_path / "inst"
    (root / "data").mkdir(parents=True)
    (root / "LEGOBionicle.exe").write_bytes(b"x")
    for name, count, tail in PREFIX_FILES:
        (root / "data" / name).write_bytes(blk(count, tail))
    py = byo_fingerprint.fingerprint(root)
    assert py["blk_tables_sha256"] == PREFIX_FP
    assert py["blk_files"] == 5
    assert js_fingerprint(root) == py


@pytest.mark.skipif(NODE is None, reason="node not found")
def test_real_install_is_the_accepted_build(lomn_data: Path) -> None:  # lomn_data skips with "SKIPPED (no LOMN_DATA)" (T8 P15)
    root = lomn_data / "install"
    py = byo_fingerprint.fingerprint(root)
    assert py == js_fingerprint(root)
    builds = json.loads((REPO / "tools" / "builds.json").read_text())
    assert py["exe_sha256"] in {b["exe_sha256"] for b in builds["accepted"]}
    assert py["blk_tables_sha256"] in {d["blk_tables_sha256"] for d in builds["data"]}
    assert py["blk_files"] == 189
