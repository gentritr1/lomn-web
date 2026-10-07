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
  alpha; the area path never does this: AppMain sets FOGTABLEMODE LINEAR);
  (Task 8B B1) LINEAR fog with FOGSTART == FOGEND (the documented formula divides by end - start = 0; the shim's choice
  is a declared deviation, lomn/d3d/ffp.h) and a point light within range with all-zero attenuation (1 / 0);
  TIES: a value within 1e-6 (relative, at least absolute 1e-6) of a discontinuity of the formulas - the 8-bit alpha
  rounding point a * 255 + 0.5 = integer, or a vertex at |distance - Range| of a point light - where float32 (shim) and
  float64 (here) may legitimately land on different sides.
Fog factor (Task 8B B2): every mode's factor is clamped to [0, 1], as the GPU shader does (an EXP fog with a negative
density would otherwise give f > 1); the C++ reference applies the same clamp.
Texture coordinates (Task 8A B1): D3DTSS_TEXTURETRANSFORMFLAGS = D3DTTFF_COUNT2 transforms the stage's coordinates by
D3DTS_TEXTURE0 and passes the first two results to the rasterizer. A 2-D coordinate (u, v) enters the 4x4 multiply as
(u, v, 1, 0): the documentation's "texture coordinate translation" for 2-D coordinates is the matrix's third row
(_31, _32), and the fourth row never applies (Wine's compute_texture_matrix moves the same rows). Cases flagged tex2x2
also check the texel the GPU probe expects: the 2x2 texture below point-sampled (wrap) at that coordinate.
Uniform block (M1a-4 c): every case also carries the per-draw block pack_uniforms wrote for it ("uniforms", 256
floats) and the first line carries the uber-shader's WGSL ({"wgsl": ...}). The field offsets come from the shader's
own `struct U` declaration (parsed below: mat4x4f = 16 floats, vec4f = 4, array<vec4f, N> = 4 N, in declaration
order), never from ffp.cpp's constants, so a block that pack_uniforms fills differently from what the shader reads
fails here. Checked per case: wvp = world * view * proj (float64 here; |diff| <= 1e-5 * sum |w_ik v_kl p_lj|, a
float32 bound), world and tex0 bit-exact copies of D3DTS_WORLD / D3DTS_TEXTURE0; tfactor = D3DRS_TEXTUREFACTOR as
(r, g, b, a) = byte / 255 (float32); fog = (start, end, density, mode): mode = FOGTABLEMODE, else FOGVERTEXMODE
(D3D8: the table mode wins), start/end/density the FOGSTART/FOGEND/FOGDENSITY bits, with the shim's declared
deviations (lomn/d3d/ffp.cpp fog_inputs, Task 8B E4): LINEAR with start == end -> (1e30, 0); a non-finite parameter
the mode uses -> (0, 0, 0, NONE); any other non-finite one -> 0; d.x = ALPHAREF & 0xff (D3DRS_ALPHAREF: "a value
ranging from 0x00 to 0xFF"), d.y = FOGENABLE.
Usage: ffp_oracle.py CASES.jsonl   (exit 0 when every non-skipped case agrees within TOL; prints a summary)
"""
import json, math, sys

TOL = 1e-4
D3DTOP_DISABLE, D3DTOP_SELECTARG1, D3DTOP_SELECTARG2, D3DTOP_MODULATE = 1, 2, 3, 4
D3DTA_DIFFUSE, D3DTA_CURRENT, D3DTA_TEXTURE, D3DTA_TFACTOR = 0, 1, 2, 3
D3DLIGHT_POINT, D3DLIGHT_DIRECTIONAL = 1, 3
D3DMCS_COLOR1 = 1
D3DFOG_NONE, D3DFOG_EXP, D3DFOG_EXP2, D3DFOG_LINEAR = 0, 1, 2, 3
D3DTTFF_COUNT2 = 2
TEX2X2 = [[(1.0, 0.0, 0.0, 1.0), (0.0, 1.0, 0.0, 1.0)], [(0.0, 0.0, 1.0, 1.0), (128 / 255.0, 64 / 255.0, 192 / 255.0, 1.0)]]  # [row v][column u]


def texcoord(c):
    """The stage-0 texture coordinate after D3DTS_TEXTURE0 (see the module docstring)."""
    u, v = c['uv']
    if c['ttff'] & 0xff != D3DTTFF_COUNT2:
        return [u, v]
    M = c['tex0']  # row-major _11.._44
    vec = (u, v, 1.0, 0.0)
    return [sum(vec[i] * M[i * 4 + j] for i in range(4)) for j in range(2)]


class Skip(Exception):
    """A case (or its fragment part) outside the modelled formulas; the message is the counted reason."""


TIE = 1e-6


def near_tie(x, at):
    return abs(x - at) <= TIE * max(1.0, abs(at))


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
            if near_tie(dist, L['range']):
                raise Skip('tie')
            if dist > L['range']:  # beyond Range the light has no effect
                continue
            Ldir = [x / dist for x in V] if dist > 0 else [0.0, 0.0, 0.0]
            a0, a1, a2 = L['att']
            den = a0 + a1 * dist + a2 * dist * dist
            if den == 0.0:
                raise Skip('zero attenuation')
            atten = 1.0 / den
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
        x = min(1.0, max(0.0, alpha)) * 255.0 + 0.5
        if near_tie(x, round(x)) and 0.5 < x < 255.5:
            raise Skip('tie')  # on the rounding point: float32 and float64 may round to different bytes
        a8 = math.floor(x)  # the 8-bit alpha the test compares
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
            if en == st:
                raise Skip('fog start == end')
            fo = (en - z) / (en - st)
        elif mode == D3DFOG_EXP:
            fo = math.exp(-(z * de))  # 1 / e^(z * density)
        elif mode == D3DFOG_EXP2:
            fo = math.exp(-((z * de) ** 2))  # 1 / e^((z * density)^2)
        else:
            raise Skip('fog with no fog mode')
        fo = min(1.0, max(0.0, fo))  # the factor is clamped in every mode (see the module docstring)
        fc = argb(rs['FOGCOLOR'])
        out = [fo * out[k] + (1.0 - fo) * fc[k] for k in range(3)] + [out[3]]
    return True, out


def f32(x):
    import struct
    return struct.unpack('<f', struct.pack('<f', x))[0]


def wgsl_layout(wgsl):
    """{field: (offset in floats, size in floats)} of the uniform `struct U` of the uber-shader."""
    import re
    body = re.search(r'struct U \{([^}]*)\}', wgsl)
    if not body:
        raise SystemExit('ffp_oracle: no `struct U` in the WGSL')
    out, at = {}, 0
    for name, ty in re.findall(r'(\w+)\s*:\s*(array<[^>]*>|\w+)\s*,', body.group(1) + ','):
        m = re.fullmatch(r'array<vec4f,\s*(\d+)>', ty)
        size = 16 if ty == 'mat4x4f' else 4 if ty == 'vec4f' else 4 * int(m.group(1)) if m else None
        if size is None:
            raise SystemExit('ffp_oracle: uniform field %s has an unmodelled type %s' % (name, ty))
        out[name] = (at, size)
        at += size
    return out


def fog_block(rs):
    """(start, end, density, mode) as the uniform block must carry them (see the module docstring)."""
    st, en, de = bits_float(rs['FOGSTART']), bits_float(rs['FOGEND']), bits_float(rs['FOGDENSITY'])
    mode = rs['FOGTABLEMODE'] if rs['FOGTABLEMODE'] != D3DFOG_NONE else rs['FOGVERTEXMODE']
    fin = math.isfinite
    if mode == D3DFOG_LINEAR and not (fin(st) and fin(en) and fin(f32(en - st))):
        return [0.0, 0.0, 0.0, 0.0]
    if mode in (D3DFOG_EXP, D3DFOG_EXP2) and not fin(de):
        return [0.0, 0.0, 0.0, 0.0]
    if mode == D3DFOG_LINEAR and st == en:
        st, en = f32(1e30), 0.0
    return [st if fin(st) else 0.0, en if fin(en) else 0.0, de if fin(de) else 0.0, float(mode)]


def uniform_mismatches(c, lay):
    """Every way the case's packed uniform block differs from its state (list of messages)."""
    u = c.get('uniforms')
    if u is None or len(u) != 256:
        return ['no 256-float uniform block in the case']
    # The JSON prints float32 values with %.9g (round-trips): f32() recovers each value exactly.
    u = [f32(x) for x in u]
    field = lambda name: u[lay[name][0]:lay[name][0] + lay[name][1]]
    bad = []
    W, V, P = ([f32(x) for x in c[k]] for k in ('world', 'view', 'proj'))
    wvp = field('wvp')
    for i in range(4):
        for j in range(4):
            exact = sum(W[i * 4 + k] * V[k * 4 + l] * P[l * 4 + j] for k in range(4) for l in range(4))
            bound = sum(abs(W[i * 4 + k] * V[k * 4 + l] * P[l * 4 + j]) for k in range(4) for l in range(4))
            if abs(wvp[i * 4 + j] - exact) > 1e-5 * bound + 1e-30:
                bad.append('wvp[%d][%d] %r, world*view*proj %r' % (i, j, wvp[i * 4 + j], exact))
    if field('world') != W:
        bad.append('world %s != D3DTS_WORLD %s' % (field('world'), W))
    if field('tex0') != [f32(x) for x in c['tex0']]:
        bad.append('tex0 %s != D3DTS_TEXTURE0 %s' % (field('tex0'), c['tex0']))
    tf = [f32(x) for x in argb(c['rs']['TEXTUREFACTOR'])]
    if field('tfactor') != tf:
        bad.append('tfactor %s != TEXTUREFACTOR 0x%08x -> %s' % (field('tfactor'), c['rs']['TEXTUREFACTOR'], tf))
    fog = fog_block(c['rs'])
    if field('fog') != fog:
        bad.append('fog %s != expected %s' % (field('fog'), fog))
    d = field('d')
    if d[0] != float(c['rs']['ALPHAREF'] & 0xff):
        bad.append('d.x (alpharef) %r != ALPHAREF %d & 0xff' % (d[0], c['rs']['ALPHAREF']))
    if d[1] != (1.0 if c['rs']['FOGENABLE'] else 0.0):
        bad.append('d.y (fog enable) %r != FOGENABLE %d' % (d[1], c['rs']['FOGENABLE']))
    return bad


def main():
    n = skipped = bad = 0
    lay = None
    blocks = 0
    reasons = {}
    worst_v = worst_f = 0.0
    kept_cases = discards = texcases = 0
    worst_uv = 0.0
    for line in open(sys.argv[1]):
        c = json.loads(line)
        if 'wgsl' in c:  # header line: the shader whose uniform layout every block must follow
            lay = wgsl_layout(c['wgsl'])
            continue
        n += 1
        um = uniform_mismatches(c, lay) if lay else ['no WGSL header line before the cases']
        if um:
            bad += 1
            if bad <= 10:
                print('MISMATCH uniforms %s: %s' % (c['what'], '; '.join(um[:3])))
        else:
            blocks += 1
        tuv = texcoord(c)
        duv = max(abs(tuv[k] - c['out_uv'][k]) for k in range(2))
        worst_uv = max(worst_uv, duv)
        if duv > TOL * max(1.0, max(abs(x) for x in tuv)):
            bad += 1
            if bad <= 10:
                print('MISMATCH texcoord %s: oracle %s shim %s' % (c['what'], tuv, c['out_uv']))
        if c['tex2x2']:
            tx = [min(1, int(math.floor((x - math.floor(x)) * 2))) for x in tuv]
            want = TEX2X2[tx[1]][tx[0]]
            if any(abs(want[k] - c['texel'][k]) > TOL for k in range(4)):
                bad += 1
                if bad <= 10:
                    print('MISMATCH texel %s: oracle %s probe expectation %s' % (c['what'], want, c['texel']))
            texcases += 1
        try:
            v = lit_vertex(c)
        except Skip as e:  # the whole case: its fragment input is the vertex colour
            skipped += 1
            reasons[str(e)] = reasons.get(str(e), 0) + 1
            continue
        dv = max(abs(v[k] - c['out_vertex'][k]) for k in range(4))
        worst_v = max(worst_v, dv)
        try:
            kept, f = fragment(c)
        except Skip as e:
            skipped += 1
            reasons[str(e)] = reasons.get(str(e), 0) + 1
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
    why = ', '.join('%d %s' % (reasons.get(k, 0), k if k != 'tie' else 'ties')
                    for k in ('fog with no fog mode', 'fog start == end', 'zero attenuation', 'tie'))
    print('ffp_oracle: %d cases (%d fragments kept, %d discarded by the alpha test, %d checks skipped: %s), '
          'max |vertex diff| %.2e, max |fragment diff| %.2e, max |texcoord diff| %.2e, %d texel probes, '
          '%d uniform blocks laid out as the WGSL reads them, %d mismatches (tolerance %g)' %
          (n, kept_cases, discards, skipped, why, worst_v, worst_f, worst_uv, texcases, blocks, bad, TOL))
    sys.exit(1 if bad or n == 0 else 0)


if __name__ == '__main__':
    main()
