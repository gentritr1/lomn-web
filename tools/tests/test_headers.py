import os
import shutil
import subprocess
from pathlib import Path

import pytest

from headers import Emitter, generate, parse

LINES = ["S Vec 12", "M 0 prim:float 4 mX", "M 4 prim:float 4 mY", "M 8 prim:float 4 mZ",
         "S Thing 24", "M 0 ptr:prim:void 4 vftable", "M 4 struct:Vec 12 mPos", "M 16 ptr:struct:Thing 4 mNext",
         "M 16 prim:int 4 mAlias", "M 21 prim:bool 1 mFlag"]


def test_layout_text():
    em = Emitter(parse(LINES))
    t = em.emit("Thing")
    assert '#include "Vec.h"' in t
    assert "    Vec mPos;" in t and "    Thing* mNext;" in t
    assert "uint8_t _pad_14[1];" in t and "uint8_t _pad_16[2];" in t   # 20..21 and 22..24
    assert "overlapping/unsized members not laid out: mAlias@16" in t
    assert "static_assert(sizeof(Thing) == 24" in t


def test_generate_writes_all_cpp(tmp_path):
    assert generate(LINES, tmp_path) == 2
    assert (tmp_path / "all.cpp").read_text() == '#include "Thing.h"\n#include "Vec.h"\n'


@pytest.mark.skipif(shutil.which("em++") is None, reason="SKIPPED (em++ not on PATH; run via make)")
def test_synthetic_headers_compile_wasm32_with_asserts(tmp_path):
    generate(LINES, tmp_path)
    r = subprocess.run(["em++", "-std=c++17", "-fsyntax-only", str(tmp_path / "all.cpp")], capture_output=True, text=True, timeout=300)
    assert r.returncode == 0, r.stderr


def test_real_headers_compile_wasm32_and_native(gen_decomp):
    inc = gen_decomp.parent / "include" / "original"
    if shutil.which("em++") is None:
        pytest.skip("SKIPPED (em++ not on PATH; run via make)")
    for cxx in ("em++", "clang++"):
        r = subprocess.run([cxx, "-std=c++17", "-fsyntax-only", str(inc / "all.cpp")], capture_output=True, text=True, timeout=600)
        assert r.returncode == 0, f"{cxx}: {r.stderr[:2000]}"
