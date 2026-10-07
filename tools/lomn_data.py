"""Locate and validate $LOMN_DATA, the local, never-committed game data folder.

Layout:
  $LOMN_DATA/reference/LEGOBionicle.exe   canonical decomp input (sha256 in tools/builds.json "canonical")
  $LOMN_DATA/install/                      a player's install: LEGOBionicle.exe (an "accepted" build) + data/
"""
from __future__ import annotations

import hashlib
import json
import os
import sys
from pathlib import Path

BUILDS = json.loads((Path(__file__).resolve().parent / "builds.json").read_text())


class DataError(Exception):
    pass


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def data_root(env: dict | None = None) -> Path:
    env = os.environ if env is None else env
    value = env.get("LOMN_DATA")
    if not value:
        raise DataError("LOMN_DATA is not set (copy .env.local.example to .env.local)")
    root = Path(value)
    if not root.is_dir():
        raise DataError(f"LOMN_DATA={value} is not a directory")
    return root


def identify_exe(path: Path, builds: dict = BUILDS) -> str:
    sha = sha256_file(path)
    for b in builds["accepted"]:
        if b["exe_sha256"] == sha:
            return b["id"]
    raise DataError(f"{path}: unknown build (sha256 {sha})")


def check(root: Path, builds: dict = BUILDS) -> dict:
    ref = root / "reference" / "LEGOBionicle.exe"
    if not ref.is_file():
        raise DataError(f"missing {ref}")
    if sha256_file(ref) != builds["canonical"]["exe_sha256"]:
        raise DataError(f"{ref} is not the canonical decomp input ({builds['canonical']['source']})")
    install = root / "install"
    exe = install / "LEGOBionicle.exe"
    if not exe.is_file():
        raise DataError(f"missing {exe}")
    build = identify_exe(exe, builds)
    for rel in ("data/Main.blk", "data/Base.osi"):
        if not (install / rel).is_file():
            raise DataError(f"missing {install / rel}")
    return {"reference": str(ref), "install": str(install), "install_build": build}


if __name__ == "__main__":
    try:
        print(json.dumps(check(data_root()), indent=2))
    except DataError as e:
        print(f"LOMN_DATA check failed: {e}", file=sys.stderr)
        sys.exit(1)
