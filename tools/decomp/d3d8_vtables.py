"""Extract COM vtable method order for the Direct3D 8 interfaces from a pinned mingw-w64 d3d8.h.

Writes tools/decomp/data/d3d8_vtables.txt:
  I <Interface> <method_count>
  m <slot> <MethodName>
"""
import hashlib
import os
import re
import sys
import tempfile
import urllib.request

D3D8_H_COMMIT = "2bcf06abc9ec5f8f73c743896b815a7a7654f9ae"
D3D8_H_SHA256 = "e93563a03f30a83dae6afe20ad0061558f6a56fe7d101b3d883749b47d1edeef"
D3D8_H_URL = f"https://raw.githubusercontent.com/mingw-w64/mingw-w64/{D3D8_H_COMMIT}/mingw-w64-headers/include/d3d8.h"
INTERFACES = ["IDirect3D8", "IDirect3DDevice8", "IDirect3DResource8", "IDirect3DBaseTexture8", "IDirect3DTexture8",
              "IDirect3DVertexBuffer8", "IDirect3DIndexBuffer8", "IDirect3DSurface8", "IDirect3DSwapChain8"]


def parse_methods(header_text: str, iface: str) -> list[str]:
    m = re.search(r"#define INTERFACE " + re.escape(iface) + r"\b(.*?)#undef INTERFACE", header_text, re.S)
    if not m:
        raise ValueError(f"interface {iface} not found")
    return re.findall(r"STDMETHOD_?\(\s*(?:[^,()]+,\s*)?(\w+)\)\s*\(", m.group(1))


def render(header_text: str) -> str:
    lines = []
    for iface in INTERFACES:
        ms = parse_methods(header_text, iface)
        lines.append(f"I {iface} {len(ms)}")
        lines += [f"m {i} {name}" for i, name in enumerate(ms)]
    return "\n".join(lines) + "\n"


def fetch() -> str:
    raw = urllib.request.urlopen(D3D8_H_URL, timeout=60).read()
    got = hashlib.sha256(raw).hexdigest()
    if got != D3D8_H_SHA256:
        raise SystemExit(f"d3d8.h sha256 mismatch: {got}")
    return raw.decode("latin1")


def main(argv: list[str]) -> int:
    """Write D3D8 vtables to output file atomically.

    Args:
        argv: Command-line arguments; argv[0] is the output file path.

    Returns:
        0 on success; raises SystemExit on error.
    """
    out = argv[0]
    # Compute text first, before touching the output file
    text = render(fetch())
    # Write atomically: to a .tmp file first, then replace
    out_dir = os.path.dirname(out) or "."
    tmp = os.path.join(out_dir, os.path.basename(out) + ".tmp")
    with open(tmp, "w") as f:
        f.write(text)
    os.replace(tmp, out)
    print("wrote", out)
    return 0


if __name__ == "__main__":
    main(sys.argv[1:])
