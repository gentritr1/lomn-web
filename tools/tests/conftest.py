"""Shared fixtures. Tests needing game data skip with 'SKIPPED (no LOMN_DATA)'; tests needing `make decomp`
output skip with 'SKIPPED (run make decomp first)'. The fixture-free subset is what `make check` gates on."""
import os
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(REPO / "tools"), str(REPO / "tools" / "decomp")]


@pytest.fixture(scope="session")
def lomn_data() -> Path:
    value = os.environ.get("LOMN_DATA")
    if not value:
        pytest.skip("SKIPPED (no LOMN_DATA)")
    return Path(value)


@pytest.fixture(scope="session")
def canonical_exe(lomn_data) -> Path:
    return lomn_data / "reference" / "LEGOBionicle.exe"


@pytest.fixture(scope="session")
def nb(canonical_exe):
    from nb11 import Nb11
    return Nb11.from_file(canonical_exe)


@pytest.fixture(scope="session")
def gen_decomp() -> Path:
    p = REPO / "gen" / "decomp"
    if not (p / "metrics.json").exists():
        pytest.skip("SKIPPED (run make decomp first)")
    return p
