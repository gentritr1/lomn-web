"""Tests for d3d8_vtables module."""
import os
import tempfile
from pathlib import Path
import pytest
from d3d8_vtables import render, parse_methods, main


def test_failed_fetch_leaves_existing_output_untouched(monkeypatch):
    """Verify that a failed fetch does not destroy an existing output file."""
    with tempfile.TemporaryDirectory() as tmpdir:
        out_file = Path(tmpdir) / "vtables.txt"
        out_file.write_text("KEEP\n")

        # Monkeypatch fetch to raise
        def mock_fetch():
            raise SystemExit("sha256 mismatch")

        monkeypatch.setattr("d3d8_vtables.fetch", mock_fetch)

        # Run main with the temp file
        with pytest.raises(SystemExit):
            main([out_file.as_posix()])

        # Verify original file is unchanged
        assert out_file.read_text() == "KEEP\n"

        # Verify no .tmp file remains
        tmp_file = Path(tmpdir) / "vtables.txt.tmp"
        assert not tmp_file.exists()


def test_render_committed_data_roundtrip():
    """Verify render() of a tiny synthetic header works correctly."""
    tiny_header = """
    #define INTERFACE IDirect3D8
    STDMETHOD(QueryInterface)(THIS_ REFIID, void **) PURE;
    STDMETHOD(AddRef)(THIS) PURE;
    STDMETHOD(Release)(THIS) PURE;
    STDMETHOD(RegisterSoftwareDevice)(THIS_ void *) PURE;
    #undef INTERFACE

    #define INTERFACE IDirect3DDevice8
    STDMETHOD(QueryInterface)(THIS_ REFIID, void **) PURE;
    STDMETHOD(AddRef)(THIS) PURE;
    STDMETHOD(Release)(THIS) PURE;
    STDMETHOD(TestCooperativeLevel)(THIS) PURE;
    #undef INTERFACE

    #define INTERFACE IDirect3DResource8
    STDMETHOD(QueryInterface)(THIS_ REFIID, void **) PURE;
    STDMETHOD(AddRef)(THIS) PURE;
    STDMETHOD(Release)(THIS) PURE;
    #undef INTERFACE

    #define INTERFACE IDirect3DBaseTexture8
    STDMETHOD(QueryInterface)(THIS_ REFIID, void **) PURE;
    STDMETHOD(AddRef)(THIS) PURE;
    STDMETHOD(Release)(THIS) PURE;
    #undef INTERFACE

    #define INTERFACE IDirect3DTexture8
    STDMETHOD(QueryInterface)(THIS_ REFIID, void **) PURE;
    STDMETHOD(AddRef)(THIS) PURE;
    STDMETHOD(Release)(THIS) PURE;
    #undef INTERFACE

    #define INTERFACE IDirect3DVertexBuffer8
    STDMETHOD(QueryInterface)(THIS_ REFIID, void **) PURE;
    STDMETHOD(AddRef)(THIS) PURE;
    STDMETHOD(Release)(THIS) PURE;
    #undef INTERFACE

    #define INTERFACE IDirect3DIndexBuffer8
    STDMETHOD(QueryInterface)(THIS_ REFIID, void **) PURE;
    STDMETHOD(AddRef)(THIS) PURE;
    STDMETHOD(Release)(THIS) PURE;
    #undef INTERFACE

    #define INTERFACE IDirect3DSurface8
    STDMETHOD(QueryInterface)(THIS_ REFIID, void **) PURE;
    STDMETHOD(AddRef)(THIS) PURE;
    STDMETHOD(Release)(THIS) PURE;
    #undef INTERFACE

    #define INTERFACE IDirect3DSwapChain8
    STDMETHOD(QueryInterface)(THIS_ REFIID, void **) PURE;
    STDMETHOD(AddRef)(THIS) PURE;
    STDMETHOD(Release)(THIS) PURE;
    #undef INTERFACE
    """

    result = render(tiny_header)
    lines = result.strip().split("\n")

    # Verify format: interfaces followed by methods
    assert lines[0] == "I IDirect3D8 4"
    assert lines[1] == "m 0 QueryInterface"
    assert lines[2] == "m 1 AddRef"
    assert lines[3] == "m 2 Release"
    assert lines[4] == "m 3 RegisterSoftwareDevice"

    assert "I IDirect3DDevice8 4" in lines
    assert "I IDirect3DTexture8 3" in lines
