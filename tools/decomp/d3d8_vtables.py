"""Extract COM vtable method order for the Direct3D 8 interfaces from a pinned mingw-w64 d3d8.h.

Writes tools/decomp/data/d3d8_vtables.txt:
  I <Interface> <method_count>
  m <slot> <MethodName>
"""
import hashlib
import re
import sys
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


if __name__ == "__main__":
    out = sys.argv[1]
    with open(out, "w") as f:
        f.write(render(fetch()))
    print("wrote", out)
