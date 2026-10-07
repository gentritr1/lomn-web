import hashlib

import pytest

from lomn_data import DataError, check, data_root, identify_exe


def builds_for(*blobs):
    shas = [hashlib.sha256(b).hexdigest() for b in blobs]
    return {"canonical": {"id": "c", "exe_sha256": shas[0], "source": "test"},
            "accepted": [{"id": f"b{i}", "exe_sha256": s} for i, s in enumerate(shas)]}


def make_tree(tmp_path, ref=b"REF", inst=b"INST", data=True):
    (tmp_path / "reference").mkdir()
    (tmp_path / "reference" / "LEGOBionicle.exe").write_bytes(ref)
    (tmp_path / "install" / "data").mkdir(parents=True)
    (tmp_path / "install" / "LEGOBionicle.exe").write_bytes(inst)
    if data:
        (tmp_path / "install" / "data" / "Main.blk").write_bytes(b"x")
        (tmp_path / "install" / "data" / "Base.osi").write_bytes(b"x")
    return tmp_path


def test_unset_env_is_error():
    with pytest.raises(DataError, match="not set"):
        data_root({})


def test_missing_dir_is_error(tmp_path):
    with pytest.raises(DataError, match="not a directory"):
        data_root({"LOMN_DATA": str(tmp_path / "nope")})


def test_valid_tree_identifies_install_build(tmp_path):
    root = make_tree(tmp_path)
    assert check(root, builds_for(b"REF", b"INST"))["install_build"] == "b1"


def test_wrong_reference_exe_rejected(tmp_path):
    root = make_tree(tmp_path, ref=b"OTHER")
    with pytest.raises(DataError, match="not the canonical"):
        check(root, builds_for(b"REF", b"INST"))


def test_unknown_install_build_rejected(tmp_path):
    root = make_tree(tmp_path, inst=b"REBUILT")
    with pytest.raises(DataError, match="unknown build"):
        check(root, builds_for(b"REF", b"INST"))


def test_missing_data_file_rejected(tmp_path):
    root = make_tree(tmp_path, data=False)
    with pytest.raises(DataError, match="Main.blk"):
        check(root, builds_for(b"REF", b"INST"))


def test_real_lomn_data_passes(lomn_data):
    r = check(lomn_data)
    assert r["install_build"] in ("beta-1.5.0-installer", "beta-repo-native")


def test_identify_real_installer(lomn_data):
    assert identify_exe(lomn_data / "install" / "LEGOBionicle.exe") == "beta-1.5.0-installer"
