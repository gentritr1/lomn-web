"""Independent check of the C++ CPU reference of the D3D8 fixed-function formulas (engine/src/d3d/ffp.cpp
reference_vertex_color / reference_fragment, which is also the expectation of the GPU probe gpu_ffp).

Written from the DirectX 8.1 SDK documentation ("Mathematics of Lighting", "Light Attenuation Over Distance",
"Fog Formulas", "Alpha Testing State", "Texture Blending" / D3DTEXTUREOP, D3DTA, D3DMATERIALCOLORSOURCE), NOT from
ffp.cpp; float64 throughout. Input: the JSON lines of lomn_ffp_cases (engine/tests/ffp_cases.cpp): gpu_ffp's cases plus
seeded random samples, each with every input the reference reads and its results.

Conventions this oracle shares with the shim on purpose (they are declared deviations or render-target conventions,
not D3D8 facts): an unbound texture samples opaque white (deviation V3); with blending off the written alpha is 1.
Cases outside the formulas modelled here are SKIPPED and counted, never silently passed:
  FOGENABLE with both FOGTABLEMODE and FOGVERTEXMODE = NONE (D3D8 then takes the fog factor from the vertex specular
  alpha; the area path never does this: AppMain sets FOGTABLEMODE LINEAR).
Usage: ffp_oracle.py CASES.jsonl   (exit 0 when every non-skipped case agrees within TOL; prints a summary)
"""
import json, math, sys

TOL = 1e-4
D3DTOP_DISABLE, D3DTOP_SELECTARG1, D3DTOP_SELECTARG2, D3DTOP_MODULATE = 1, 2, 3, 4
D3DTA_DIFFUSE, D3DTA_CURRENT, D3DTA_TEXTURE, D3DTA_TFACTOR = 0, 1, 2, 3
D3DLIGHT_POINT, D3DLIGHT_DIRECTIONAL = 1, 3
D3DMCS_COLOR1 = 1
D3DFOG_NONE, D3DFOG_EXP, D3DFOG_EXP2, D3DFOG_LINEAR = 0, 1, 2, 3


def argb(v):
    return ((v >> 16 & 255) / 255.0, (v >> 8 & 255) / 255.0, (v & 255) / 255.0, (v >> 24 & 255) / 255.0)


def bits_float(v):
    import struct
    return struct.unpack('<f', struct.pack('<I', v & 0xffffffff))[0]


def inv3(m):
    """Inverse of a 3x3 (list of rows)."""
    a, b, c = m[0]; d, e, f = m[1]; g, h, i = m[2]
    det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
    return [[(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det],
            [(f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det],
            [(d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det]]


def lit_vertex(c):
    rs = c['rs']
    vcol = c['vcol']
    if not rs['LIGHTING']:
        # Unlit: the vertex diffuse colour, or opaque white when the vertex format has none.
        return list(vcol) if c['has_vcol'] else [1.0, 1.0, 1.0, 1.0]
    mat = c['material']
    # D3DRS_DIFFUSEMATERIALSOURCE = COLOR1 takes the vertex diffuse colour when COLORVERTEX is on and the vertex has one;
    # a missing vertex colour falls back to the material.
    cd = vcol if (c['has_vcol'] and rs['COLORVERTEX'] and rs['DIFFUSEMATERIALSOURCE'] == D3DMCS_COLOR1) else mat['diffuse']
    W = c['world']
    rows = [W[0:4], W[4:8], W[8:12], W[12:16]]
    p = c['p']
    P = [p[0] * rows[0][j] + p[1] * rows[1][j] + p[2] * rows[2][j] + rows[3][j] for j in range(3)]
    # Normals go through the inverse transpose of the upper 3x3 (row-vector convention: n' = n * (M^-1)^T).
    Mi = inv3([r[0:3] for r in rows[0:3]])
    n = c['n']
    N = [sum(n[k] * Mi[j][k] for k in range(3)) for j in range(3)]
    ga = argb(rs['AMBIENT'])
    amb = [ga[0], ga[1], ga[2]]
    dif = [0.0, 0.0, 0.0]
    for L in c['lights']:
        if L['type'] == D3DLIGHT_DIRECTIONAL:
            dx = L['direction']
            ln = math.sqrt(sum(x * x for x in dx))
            Ldir = [-x / ln for x in dx] if ln > 0 else [0.0, 0.0, 0.0]
            atten = 1.0
        elif L['type'] == D3DLIGHT_POINT:
            V = [L['position'][k] - P[k] for k in range(3)]
            dist = math.sqrt(sum(x * x for x in V))
            if dist > L['range']:  # beyond Range the light has no effect
                continue
            Ldir = [x / dist for x in V] if dist > 0 else [0.0, 0.0, 0.0]
            a0, a1, a2 = L['att']
            atten = 1.0 / (a0 + a1 * dist + a2 * dist * dist)
        else:
            raise SystemExit('light type %d not modelled' % L['type'])
        ndotl = max(0.0, sum(N[k] * Ldir[k] for k in range(3)))
        for k in range(3):
            amb[k] += atten * L['ambient'][k]           # Ambient = Ca * (Ga + sum(Atten * La))
            dif[k] += cd[k] * L['diffuse'][k] * ndotl * atten  # Diffuse = sum(Cd * Ld * (N.Ldir) * Atten)
    out = [min(1.0, max(0.0, mat['emissive'][k] + mat['ambient'][k] * amb[k] + dif[k])) for k in range(3)]
    return out + [cd[3]]  # the lit colour's alpha is the diffuse alpha


def fragment(c):
    """Returns (kept, rgba)."""
    rs, ts = c['rs'], c['tss0']
    d = c['d']
    texel = c['texel'] if c['bound'] else [1.0, 1.0, 1.0, 1.0]
    tf = argb(rs['TEXTUREFACTOR'])

    def arg(a):
        a &= 0xf
        if a == D3DTA_TEXTURE: return texel
        if a == D3DTA_TFACTOR: return tf
        return d  # DIFFUSE, and CURRENT (= diffuse on stage 0)

    def op(o, a1, a2, ch):
        if o == D3DTOP_SELECTARG1: return a1[ch]
        if o == D3DTOP_SELECTARG2: return a2[ch]
        if o == D3DTOP_MODULATE: return a1[ch] * a2[ch]
        return d[ch]  # DISABLE: stage 0 passes the diffuse (current) value through

    col = [op(ts['COLOROP'], arg(ts['COLORARG1']), arg(ts['COLORARG2']), ch) for ch in range(3)]
    alpha = op(ts['ALPHAOP'], arg(ts['ALPHAARG1']), arg(ts['ALPHAARG2']), 3)
    if rs['ALPHATESTENABLE']:
        a8 = math.floor(min(1.0, max(0.0, alpha)) * 255.0 + 0.5)  # the 8-bit alpha the test compares
        ref = rs['ALPHAREF'] & 0xff
        f = rs['ALPHAFUNC']
        ok = {1: False, 2: a8 < ref, 3: a8 == ref, 4: a8 <= ref, 5: a8 > ref, 6: a8 != ref, 7: a8 >= ref, 8: True}.get(f, True)
        if not ok:
            return False, None
    out = col + [alpha if rs['ALPHABLENDENABLE'] else 1.0]
    if rs['FOGENABLE']:
        mode = rs['FOGTABLEMODE'] if rs['FOGTABLEMODE'] != D3DFOG_NONE else rs['FOGVERTEXMODE']
        z = c['w']
        st, en, de = bits_float(rs['FOGSTART']), bits_float(rs['FOGEND']), bits_float(rs['FOGDENSITY'])
        if mode == D3DFOG_LINEAR:
            fo = min(1.0, max(0.0, (en - z) / (en - st)))
        elif mode == D3DFOG_EXP:
            fo = math.exp(-(z * de))  # 1 / e^(z * density)
        elif mode == D3DFOG_EXP2:
            fo = math.exp(-((z * de) ** 2))  # 1 / e^((z * density)^2)
        else:
            raise ValueError('skip')
        fc = argb(rs['FOGCOLOR'])
        out = [fo * out[k] + (1.0 - fo) * fc[k] for k in range(3)] + [out[3]]
    return True, out


def main():
    n = skipped = bad = 0
    worst_v = worst_f = 0.0
    kept_cases = discards = 0
    for line in open(sys.argv[1]):
        c = json.loads(line)
        n += 1
        v = lit_vertex(c)
        dv = max(abs(v[k] - c['out_vertex'][k]) for k in range(4))
        worst_v = max(worst_v, dv)
        try:
            kept, f = fragment(c)
        except ValueError:
            skipped += 1
            if dv > TOL:
                bad += 1
                print('MISMATCH vertex %s: oracle %s shim %s' % (c['what'], v, c['out_vertex']))
            continue
        msg = None
        if dv > TOL:
            msg = 'vertex: oracle %s shim %s' % (v, c['out_vertex'])
        if kept != c['out_kept']:
            msg = 'alpha test: oracle kept=%s shim kept=%s' % (kept, c['out_kept'])
        elif kept:
            kept_cases += 1
            df = max(abs(f[k] - c['out_fragment'][k]) for k in range(4))
            worst_f = max(worst_f, df)
            if df > TOL:
                msg = 'fragment: oracle %s shim %s' % (f, c['out_fragment'])
        else:
            discards += 1
        if msg:
            bad += 1
            if bad <= 10:
                print('MISMATCH case %d (%s): %s' % (n, c['what'], msg))
    print('ffp_oracle: %d cases (%d fragments kept, %d discarded by the alpha test, %d fragment checks skipped: fog with '
          'no fog mode), max |vertex diff| %.2e, max |fragment diff| %.2e, %d mismatches (tolerance %g)'
          % (n, kept_cases, discards, skipped, worst_v, worst_f, bad, TOL))
    sys.exit(1 if bad or n == 0 else 0)


if __name__ == '__main__':
    main()
