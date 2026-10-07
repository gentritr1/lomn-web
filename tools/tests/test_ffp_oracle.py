"""M1a-3 Task 8B B1/B2: tools/research/oracle/ffp_oracle.py on hand-made cases (no build, no game data).

Cases the D3D8 formulas leave undefined (FOGSTART == FOGEND, a point light with all-zero attenuation) and cases sitting on
a discontinuity (8-bit alpha rounding tie, a vertex exactly at a point light's Range) are SKIPPED and counted, never a
crash or a false mismatch. EXP fog with a negative density is clamped to [0, 1] (the rule shared with the GPU shader
and the C++ reference).
M1a-4 (c): every input starts with a {"wgsl": ...} line and every case carries its packed uniform block; the oracle
takes the field offsets from the WGSL struct U, so these tests use their own (shorter, reordered) struct."""
import json
import struct
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
ORACLE = REPO / "tools" / "research" / "oracle" / "ffp_oracle.py"


def bits(f):
    return struct.unpack("<I", struct.pack("<f", f))[0]


# A uniform struct with another field order than the uber-shader's: the oracle must read the offsets from it.
TEST_WGSL = """struct U {
  d: vec4f,
  fog: vec4f,
  world: mat4x4f,
  tfactor: vec4f,
  wvp: mat4x4f,
  tex0: mat4x4f,
  lights: array<vec4f, 40>,
}
@group(0) @binding(0) var<uniform> u: U;
"""
LAYOUT = {"d": 0, "fog": 4, "world": 8, "tfactor": 24, "wvp": 28, "tex0": 44}
IDENTITY = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]


def pack(c, fog):
    """The 256-float block a correct pack_uniforms writes for case c (identity view/proj: wvp = world)."""
    u = [0.0] * 256
    rs = c["rs"]
    put = lambda name, vals: u.__setitem__(slice(LAYOUT[name], LAYOUT[name] + len(vals)), vals)
    put("wvp", c["world"])
    put("world", c["world"])
    put("tex0", c["tex0"])
    t = rs["TEXTUREFACTOR"]
    put("tfactor", [(t >> 16 & 255) / 255.0, (t >> 8 & 255) / 255.0, (t & 255) / 255.0, (t >> 24 & 255) / 255.0])
    put("fog", fog)
    put("d", [float(rs["ALPHAREF"] & 0xFF), 1.0 if rs["FOGENABLE"] else 0.0, 1.0, 0.0])
    return u


def case(fog=(0.0, 1.0, 1.0, 0.0), **over):
    """An unlit, untextured, unfogged case whose outputs are the D3D8 values (white vertex colour, white texel)."""
    c = {
        "what": "base",
        "rs": {"LIGHTING": 0, "COLORVERTEX": 1, "DIFFUSEMATERIALSOURCE": 1, "AMBIENT": 0, "TEXTUREFACTOR": 0xFFFFFFFF,
               "ALPHATESTENABLE": 0, "ALPHAFUNC": 8, "ALPHAREF": 0, "ALPHABLENDENABLE": 0, "FOGENABLE": 0,
               "FOGTABLEMODE": 0, "FOGVERTEXMODE": 0, "FOGSTART": bits(0.0), "FOGEND": bits(1.0), "FOGDENSITY": bits(1.0),
               "FOGCOLOR": 0},
        "tss0": {"COLOROP": 4, "COLORARG1": 2, "COLORARG2": 0, "ALPHAOP": 2, "ALPHAARG1": 2, "ALPHAARG2": 0},
        "ttff": 0, "tex0": [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1], "uv": [0.5, 0.5], "out_uv": [0.5, 0.5],
        "tex2x2": False, "world": [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1],
        "material": {"diffuse": [1, 1, 1, 1], "ambient": [0, 0, 0, 1], "emissive": [0, 0, 0, 1]}, "lights": [],
        "has_vcol": False, "vcol": [1, 1, 1, 1], "p": [0, 0, 0], "n": [0, 0, -1], "d": [1, 1, 1, 1],
        "texel": [1, 1, 1, 1], "bound": True, "w": 1.0, "out_vertex": [1, 1, 1, 1], "out_kept": True,
        "out_fragment": [1, 1, 1, 1],
    }
    for k, v in over.items():
        if isinstance(v, dict) and isinstance(c.get(k), dict):
            c[k] = {**c[k], **v}
        else:
            c[k] = v
    c["view"], c["proj"] = IDENTITY, IDENTITY
    c["uniforms"] = pack(c, list(fog))  # fog: (start, end, density, mode) as the block must carry them
    return c


def run(tmp_path, cases, header=True):
    p = tmp_path / "cases.jsonl"
    p.write_text((json.dumps({"wgsl": TEST_WGSL}) + "\n" if header else "") + "".join(json.dumps(c) + "\n" for c in cases))
    return subprocess.run([sys.executable, str(ORACLE), str(p)], capture_output=True, text=True, timeout=60)


def test_undefined_cases_are_counted_skips(tmp_path):
    fog_eq = case(what="fog LINEAR start == end",
                  rs={"FOGENABLE": 1, "FOGTABLEMODE": 3, "FOGSTART": bits(10.0), "FOGEND": bits(10.0), "FOGCOLOR": 0xFFFF0000},
                  out_fragment=[1, 0, 0, 1], fog=(1e30, 0.0, 1.0, 3.0))  # the declared start == end substitute
    light0 = {"type": 1, "diffuse": [1, 1, 1, 1], "ambient": [0, 0, 0, 1], "position": [0, 0, -1],
              "direction": [0, 0, 1], "range": 10.0, "att": [0.0, 0.0, 0.0]}
    att0 = case(what="point light, all-zero attenuation", rs={"LIGHTING": 1}, lights=[light0])
    r = run(tmp_path, [case(), fog_eq, att0])
    assert r.returncode == 0, r.stdout + r.stderr
    assert "Traceback" not in r.stderr
    assert "1 fog start == end" in r.stdout, r.stdout
    assert "1 zero attenuation" in r.stdout, r.stdout


def test_ties_within_1e6_are_counted_skips(tmp_path):
    # Alpha 0.5/255 * 255 + 0.5 = 1.0 (+- float noise): the 8-bit alpha is 0 or 1. The shim's answer (discarded) is
    # not compared; the case is a counted tie.
    tie_a = 0.5 / 255.0
    alpha_tie = case(what="alpha rounding tie", rs={"ALPHATESTENABLE": 1, "ALPHAFUNC": 5, "ALPHAREF": 0},
                     texel=[1, 1, 1, tie_a], out_kept=False, out_fragment=None)
    # A vertex at exactly the point light's Range: lit or not depends on the last bit of the distance.
    light = {"type": 1, "diffuse": [1, 1, 1, 1], "ambient": [0, 0, 0, 1], "position": [0, 0, -2],
             "direction": [0, 0, 1], "range": 2.0, "att": [1.0, 0.0, 0.0]}
    range_tie = case(what="vertex at the light's range", rs={"LIGHTING": 1},
                     material={"diffuse": [1, 1, 1, 1], "ambient": [0, 0, 0, 1], "emissive": [0, 0, 0, 1]},
                     lights=[light], out_vertex=[0, 0, 0, 1], out_fragment=[0, 0, 0, 1], d=[0, 0, 0, 1])
    r = run(tmp_path, [alpha_tie, range_tie])
    assert r.returncode == 0, r.stdout + r.stderr
    assert "2 ties" in r.stdout, r.stdout


def test_exp_fog_negative_density_is_clamped(tmp_path):
    # f = exp(-density * w) = e^1 > 1 is clamped to 1: the colour is unfogged (GPU shader and C++ reference agree).
    neg = case(what="EXP fog, density -0.5 at w 2",
               rs={"FOGENABLE": 1, "FOGTABLEMODE": 1, "FOGDENSITY": bits(-0.5), "FOGCOLOR": 0xFF00FF00},
               w=2.0, out_fragment=[1, 1, 1, 1], fog=(0.0, 1.0, -0.5, 1.0))
    r = run(tmp_path, [neg])
    assert r.returncode == 0, r.stdout + r.stderr
    assert "0 mismatches" in r.stdout, r.stdout


def test_uniform_block_is_checked_against_the_wgsl_layout(tmp_path):
    """M1a-4 (c): a block laid out as the WGSL struct reads it passes; a wrong slot, a mode taken from FOGVERTEXMODE
    while FOGTABLEMODE is set, an unmasked ALPHAREF or a missing WGSL header line are mismatches."""
    fogged = dict(rs={"FOGENABLE": 1, "FOGTABLEMODE": 3, "FOGVERTEXMODE": 1, "FOGSTART": bits(2.0), "FOGEND": bits(6.0),
                      "TEXTUREFACTOR": 0xB2336699, "ALPHAREF": 0x1A7},
                  w=0.0, out_fragment=[1, 1, 1, 1])
    good = case(what="good", fog=(2.0, 6.0, 1.0, 3.0), **fogged)
    r = run(tmp_path, [good])
    assert r.returncode == 0, r.stdout + r.stderr
    assert "1 uniform blocks laid out as the WGSL reads them, 0 mismatches" in r.stdout, r.stdout
    swapped = case(what="fog start/end swapped", fog=(6.0, 2.0, 1.0, 3.0), **fogged)
    vertex_mode = case(what="vertex mode", fog=(2.0, 6.0, 1.0, 1.0), **fogged)
    unmasked = case(what="alpharef unmasked", fog=(2.0, 6.0, 1.0, 3.0), **fogged)
    unmasked["uniforms"][LAYOUT["d"]] = float(0x1A7)
    tf = case(what="tfactor bgra", fog=(2.0, 6.0, 1.0, 3.0), **fogged)
    o = LAYOUT["tfactor"]
    tf["uniforms"][o], tf["uniforms"][o + 2] = tf["uniforms"][o + 2], tf["uniforms"][o]
    r = run(tmp_path, [swapped, vertex_mode, unmasked, tf])
    assert r.returncode == 1, r.stdout + r.stderr
    assert "4 mismatches" in r.stdout, r.stdout
    for what, field in (("fog start/end swapped", "fog"), ("vertex mode", "fog"), ("alpharef unmasked", "d.x"),
                        ("tfactor bgra", "tfactor")):
        assert "MISMATCH uniforms %s: %s" % (what, field) in r.stdout, r.stdout
    r = run(tmp_path, [good], header=False)
    assert r.returncode == 1 and "no WGSL header line" in r.stdout, r.stdout
