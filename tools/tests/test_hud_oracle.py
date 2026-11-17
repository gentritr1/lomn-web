"""M2a-3 T9: tools/research/oracle/hud_oracle.py on a hand-made HUD (no game data): an opaque sprite and an alpha sprite
shown in the order alpha, opaque. The alpha one defers to priority 100 (hud.md §3.5), so the opaque one draws first.
The pinned lines are the §3.2/§3.5/§10 grammar. The corner boxes come from the rects alone, which the matrix code does
not share. The two menu frames (M1 665, M2 1520) are diffed against the native log by `make osi-menu`."""
import struct
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "research" / "oracle"))
import hud_oracle as H  # noqa: E402


def hx(v):
    return "%08x" % struct.unpack("<I", struct.pack("<f", v))[0]


def z_block(alpha_color=None, b6="00"):
    name = b"Cursor".hex()
    z = ["Z hud.node 00000001 sprite 0026:00000001 0", "Z hud.node 00000002 sprite 0026:00000002 1",
         "Z hud.node 00000003 sprite 0026:00000003 -"]  # not shown: no draw
    for who, rect, alpha in (("0026:00000001", (10, 20, 42, 52), "01"), ("0026:00000002", (100, 200, 164, 216), "00")):
        z += ["Z sprite.rect %s %s" % (who, " ".join(hx(v) for v in rect)), "Z sprite.file %s 6 %s" % (who, name),
              "Z sprite.b6 %s %s" % (who, b6), "Z sprite.alpha %s %s" % (who, alpha)]
    if alpha_color:
        z.append("Z sprite.color 0026:00000001 " + " ".join(hx(v) for v in alpha_color))
    return z


def run(z, rs=None, tex=("Cursor.tga",)):
    nodes, fields = H.parse_z(z)
    hud = H.Hud(H.Device(rs), fields, lambda p: None, lambda n: n in tex)
    return hud, hud.run(nodes, 0x10034)


def test_two_sprites_exact_lines():
    hud, out = run(z_block())
    head = ["# viewport HdDr", "# HUD view: ScRenderer::Start", "# SrPushRenderMode 0x8c034",
            # D3D8 defaults: only the differing dx states are logged (ALPHAOP 2, ALPHAARG1 2, DESTBLEND... are set)
            "RS ALPHABLENDENABLE 1", "RS SRCBLEND 5", "RS DESTBLEND 6", "RS ALPHATESTENABLE 1", "RS ZWRITEENABLE 0",
            "RS ZENABLE 0", "RS LIGHTING 0", "RS CULLMODE 1"]
    assert out[:len(head)] == head
    assert out[len(head)].startswith("XFORM PROJECTION sum=") and out[len(head) + 1].startswith("XFORM VIEW sum=")
    body = out[len(head) + 2:]
    draws = [ln for ln in body if ln.startswith("DRAW")]
    assert [d.split()[2] for d in draws] == ["0026:00000002", "0026:00000001"]  # the alpha sprite deferred
    opaque = body[:body.index(draws[0]) + 2]
    assert [ln.split(" sum=")[0] for ln in opaque] == [H.MATERIAL, "TEXTURE 0 Cursor.tga", "XFORM WORLD", draws[0],
                                                       "TEXTURE 0 NULL"]
    alpha = body[len(opaque):]
    assert alpha[:3] == ["RS ALPHAREF 1", "RS ALPHAFUNC 5", "TSS0 COLORARG2 0"]  # gated: ALPHABLENDENABLE already 1
    assert all(d.endswith("call=DIP prim=5 count=2 fvf=0x112 vsum=" + d.split("vsum=")[1]) for d in draws)
    # the quad corners are the rect (pixels under the HUD ortho camera, §3.3)
    assert hud.boxes == {"0026:00000001": [10, 20, 42, 52], "0026:00000002": [100, 200, 164, 216]}
    # regression pins of the 31-hashes (hand-made rects; the menu runs check them against the native log)
    assert [d.split("vsum=")[1] for d in draws] == ["24e678ca", "0a5558e8"]
    assert [ln for ln in out if ln.startswith("XFORM")] == [
        "XFORM PROJECTION sum=6a5e2574", "XFORM VIEW sum=1777bf7c", "XFORM WORLD sum=07e8b376", "XFORM WORLD sum=a625ea69"]


def test_projection_matrix_bits():
    # §3.3: m0 2/W, m5 2/(0 - H), m10 2/15000, m12 -1, m13 1, m14 1, m15 1 at 640x480
    P = [0.0] * 16
    P[0], P[5], P[10] = H.f(2 / 640), H.f(2 / H.f(-480.0)), H.f(2 / 15000)
    P[12], P[13], P[14], P[15] = -1, 1, 1, 1
    assert [hx(v) for v in (P[0], P[5], P[10])] == ["3b4ccccd", "bb888889", "390bcf65"]


def test_dx_gating_against_prior_state():
    # an area that left ALPHABLENDENABLE 1 and CULLMODE 1: those dx writes are not logged
    _, out = run(z_block(), rs={"ALPHABLENDENABLE": 1, "CULLMODE": 1})
    assert "RS ALPHABLENDENABLE 1" not in out and "RS CULLMODE 1" not in out


@pytest.mark.parametrize("kw,why", [({"alpha_color": (1.0, 0.5, 1.0, 1.0)}, "coloured"), ({"b6": "01"}, "b6")])
def test_refusals(kw, why):
    with pytest.raises(H.Refused, match=why):
        run(z_block(**kw))


def test_dds_refused_and_missing_tga_is_null():
    with pytest.raises(H.Refused, match="dds"):
        run(z_block(), tex=("Cursor.dds", "Cursor.tga"))
    _, out = run(z_block(), tex=())
    assert "TEXTURE 0 Cursor.tga" not in out and out.count("TEXTURE 0 NULL") == 4


def test_fog_refused():
    with pytest.raises(H.Refused, match="FOG"):
        run(z_block(), rs={"FOGENABLE": 1})
