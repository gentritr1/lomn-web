"""M1b: the BYO page's build identification (web/app/fingerprint.js, run through web/tools/fingerprint_cli.mjs) equals
the independent Python oracle (tools/byo_fingerprint.py) on crafted installs, and on the real install (LOMN_DATA) it is
the fingerprint tools/builds.json accepts."""
from __future__ import annotations

import json
import os
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


@pytest.mark.skipif(NODE is None or not os.environ.get("LOMN_DATA"), reason="needs node and LOMN_DATA")
def test_real_install_is_the_accepted_build() -> None:
    root = Path(os.environ["LOMN_DATA"]) / "install"
    py = byo_fingerprint.fingerprint(root)
    assert py == js_fingerprint(root)
    builds = json.loads((REPO / "tools" / "builds.json").read_text())
    assert py["exe_sha256"] in {b["exe_sha256"] for b in builds["accepted"]}
    assert py["blk_tables_sha256"] in {d["blk_tables_sha256"] for d in builds["data"]}
    assert py["blk_files"] == 189
