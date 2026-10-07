"""M1a-3 Task 8B B1/B2: tools/research/oracle/ffp_oracle.py on hand-made cases (no build, no game data).

Cases the D3D8 formulas leave undefined (FOGSTART == FOGEND, a point light with all-zero attenuation) and cases sitting on
a discontinuity (8-bit alpha rounding tie, a vertex exactly at a point light's Range) are SKIPPED and counted, never a
crash or a false mismatch. EXP fog with a negative density is clamped to [0, 1] (the rule shared with the GPU shader
and the C++ reference)."""
import json
import struct
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
ORACLE = REPO / "tools" / "research" / "oracle" / "ffp_oracle.py"


def bits(f):
    return struct.unpack("<I", struct.pack("<f", f))[0]


def case(**over):
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
    return c


def run(tmp_path, cases):
    p = tmp_path / "cases.jsonl"
    p.write_text("".join(json.dumps(c) + "\n" for c in cases))
    return subprocess.run([sys.executable, str(ORACLE), str(p)], capture_output=True, text=True, timeout=60)


def test_undefined_cases_are_counted_skips(tmp_path):
    fog_eq = case(what="fog LINEAR start == end",
                  rs={"FOGENABLE": 1, "FOGTABLEMODE": 3, "FOGSTART": bits(10.0), "FOGEND": bits(10.0), "FOGCOLOR": 0xFFFF0000},
                  out_fragment=[1, 0, 0, 1])
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
               w=2.0, out_fragment=[1, 1, 1, 1])
    r = run(tmp_path, [neg])
    assert r.returncode == 0, r.stdout + r.stderr
    assert "0 mismatches" in r.stdout, r.stdout
