"""Compare lomn_datacheck (C++) against the independent Python oracles for M1a-1 acceptance A1-A6.

Usage: python3 oracle/check_datacheck.py --exe build/native/lomn_datacheck --install "$LOMN_DATA/install"
       [--json out/datacheck.json]   (reuse a saved run instead of executing the binary)
Exit code 0 only if every check passes. Oracles: blk.py (A1), slb.py relocs + a routing written here from the
decompiled GcObjectLoader::LoadObjects (A3, A6), xbin.py (A4), texres_ext.py (A2), bcdec.py (A5).
"""
import argparse
import collections
import json
import os
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)
import blk  # noqa: E402
import slb  # noqa: E402
import xbin  # noqa: E402
import bcdec  # noqa: E402
import texres_ext  # noqa: E402

AREAS = ['lev1/vllg', 'lev2/ghut', 'lev1/l1a7', 'lev3/boss', 'lev4/crss', 'lev5/le08', 'lev6/ta11', 'lev7/kpka', 'lev0/frnt']
results = []


def check(name, ok, detail=''):
    results.append((name, bool(ok), detail))
    print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f"  -- {detail}" if detail else ''), flush=True)


def walk_blks(install):
    out = []
    for dp, dn, fn in os.walk(os.path.join(install, 'data')):
        for f in fn:
            if f.lower().endswith('.blk'):
                p = os.path.join(dp, f)
                out.append((os.path.relpath(p, install), p))
    return sorted(out)


def entries(path):
    d = open(path, 'rb').read()
    n = struct.unpack_from('<I', d, 8)[0]
    for i in range(n):
        o = 12 + i * 56
        name = d[o:o + 40].split(b'\0')[0].decode('latin1')
        bs, fs, fl, off = struct.unpack_from('<IIII', d, o + 40)
        yield name, d[off:off + fs]


def route(flags):  # GcObjectLoader::LoadObjects 0x0053fc60, test order as decompiled
    if flags & 0x10:
        return 'moveable' if flags & 0x20 else ('trans' if flags & 0x100 else 'main')
    if flags & 0x20:
        return 'nolight'
    return 'back' if flags & 0x8 else 'ignored'


def fourcc(v):
    return bytes([(v >> 24) & 255, (v >> 16) & 255, (v >> 8) & 255, v & 255]).split(b'\0')[0].decode('latin1')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    ap.add_argument('--install', required=True)
    ap.add_argument('--json')
    a = ap.parse_args()
    install = a.install
    t0 = time.time()
    if a.json:
        cpp = json.load(open(a.json))
    else:
        r = subprocess.run([a.exe, install], capture_output=True, timeout=600)
        check('lomn_datacheck exits 0', r.returncode == 0, r.stderr.decode()[-300:])
        cpp = json.loads(r.stdout)
    print(f'(C++ wall {cpp["timing"]["wall_ms"]:.0f} ms)')
    blks = walk_blks(install)

    # ---- A1: every .blk opens; entry count and order equal blk.py; sorted the way Find needs.
    cblk = {b['path']: b for b in cpp['blk']}
    bad = []
    unsorted = []
    for rel, p in blks:
        m, v, e = blk.read(p)
        c = cblk.get(rel)
        if not c or not c['ok'] or c['entries'] != [x[0] for x in e] or c['version'] != v or m != b'BLKF':
            bad.append(rel)
        if not blk.sortedci(e):
            unsorted.append(rel)
    check('A1 all .blk found by both walkers', len(blks) == len(cpp['blk']) == 189, f'python {len(blks)}, c++ {len(cpp["blk"])}')
    check('A1 per-file entry names and order equal blk.py', not bad, f'mismatches {bad[:5]}')
    check('A1 every table sorted (blk.py sortedci == c++ sorted)', not unsorted and cpp['blk_summary']['sorted'] == 189,
          f'python-unsorted {unsorted[:5]}')
    print(f"      entries {cpp['blk_summary']['entries']}, unterminated 40-byte names {cpp['blk_summary']['unterminated_names']}, "
          f"entries unreachable by their own name (StrCmp runs past mName) {cpp['blk_summary']['find_misses']}")

    # ---- A3 + A6: OBJ routing, recomputed from raw bytes with slb.py's reloc reader.
    tot = collections.Counter()
    m131 = 0
    files = 0
    cin = 0
    per_file = {}
    for rel, p in blks:
        parts = rel.replace('\\', '/').split('/')
        if len(parts) != 4 or parts[1].lower() != 'levels':
            continue
        for name, b in entries(p):
            ln = name.lower()
            if not ln.endswith('_obj.slb'):
                continue
            if ln.startswith('cin'):
                cin += 1
                continue
            files += 1
            cnt, offs = slb.relocs(b)
            assert cnt is not None and 12 in offs, (rel, name)
            n, ptr = struct.unpack_from('<II', b, 8)
            routes = collections.Counter()
            drawn = []
            for i in range(n):
                rid, = struct.unpack_from('<I', b, ptr + 60 * i)
                fl, = struct.unpack_from('<I', b, ptr + 60 * i + 0x38)
                rt = route(fl)
                routes[rt] += 1
                tot[rt] += 1
                if rt == 'moveable' and fl in (0x131, 0x132):
                    m131 += 1
                if rt != 'ignored':
                    drawn.append(fourcc(rid))
            per_file[(rel, name)] = (dict(routes), drawn)
    cobj = {(o['blk'], o['entry']): (o['routes'], o['drawn']) for o in cpp['obj']}
    check('A3 117 area OBJ tables, 11 cin*_obj.slb skipped', files == 117 == cpp['obj_summary']['files'] and cin == 11 == cpp['obj_summary']['cin_obj_skipped'],
          f'python files {files} cin {cin}; c++ {cpp["obj_summary"]["files"]} / {cpp["obj_summary"]["cin_obj_skipped"]}')
    want = {'main': 183, 'trans': 136, 'back': 35, 'moveable': 943}
    check('A3 routing totals 183 main / 136 trans / 35 back / 943 moveable (python)', dict(tot) == want, str(dict(tot)))
    check('A3 routing totals equal in c++', cpp['obj_summary']['routes'] == want, str(cpp['obj_summary']['routes']))
    check('A3 34 records 0x131/0x132 routed to moveable', m131 == 34 == cpp['obj_summary']['moveable_0x131_0x132'], f'python {m131}')
    check('A3/A6 per-file routes and drawn-id lists equal', per_file == cobj,
          f'differ {[k for k in set(per_file) | set(cobj) if per_file.get(k) != cobj.get(k)][:5]}')
    vllg = per_file[('data/levels/lev1/vllg.blk', 'vllg_OBJ.slb')][1]
    vx = [n[:-2].lower() for n, _ in entries(os.path.join(install, 'data/levels/lev1/vllg.blk')) if n.lower().endswith('.x')]
    check('A6 vllg draws only OBJ-routed models (not main1/tre1/rock/drll)',
          not ({'main1', 'tre1', 'rock', 'drll'} & set(vllg)) and set(vllg) < set(vx),
          f'drawn {sorted(set(vllg))}; blk has {len(vx)} .x')
    check('A6 every drawn id has <id>.x in its area blk', cpp['obj_summary']['missing_models'] == 0)

    # ---- A4: every .x against xbin.py (last mesh: vertices, faces, used subsets, vertex colours) + vertex split.
    cx = {(x['blk'], x['entry']): x for x in cpp['x']}
    nx = 0
    fails = []
    diffs = []
    multi = []
    for rel, p in blks:
        for name, b in entries(p):
            if not name.lower().endswith('.x'):
                continue
            nx += 1
            try:
                ms, top = xbin.load(b)
            except Exception as e:  # noqa: BLE001
                fails.append((rel, name, str(e)))
                continue
            meshes = [o for o in xbin.walk(top) if o.type == 'Mesh']
            last, lm = ms[-1], meshes[-1]
            if len(ms) != 1:
                multi.append((rel, name, len(ms)))
            # Output vertices: each position keeps its first normal; every further distinct normal adds a copy.
            iv = xbin.ints(lm.values)
            used = collections.defaultdict(set)
            mn = [c for c in lm.children if c.type == 'MeshNormals']
            if mn:
                ni = xbin.ints(mn[-1].values)
                k, q = 2, 2
                for _ in range(iv[1]):
                    c = iv[k]
                    for j in range(c):
                        used[iv[k + 1 + j]].add(ni[q + 1 + j])
                    k += 1 + c
                    q += 1 + c
            out_v = iv[0] + sum(len(s) - 1 for s in used.values())
            exp = dict(meshes=len(ms), vertices=len(last.positions), faces=len(last.faces),
                       subsets=len(set(last.face_material or [])), out_vertices=out_v,
                       has_colors=any(c.type == 'MeshVertexColors' for c in lm.children))
            c = cx.get((rel, name), {})
            got = {k: c.get(k) for k in exp}
            if got != exp:
                diffs.append((rel, name, exp, got))
    check('A4 2,653 .x files; xbin.py failed == []', nx == 2653 and not fails and cpp['x_summary']['ok'] == 2653, f'{nx} files, fails {fails[:3]}')
    check('A4 meshes per file = 1 except characters/bull.blk/mklf.x = 3', multi == [('data/characters/bull.blk', 'mklf.x', 3)], str(multi))
    check('A4 per-file meshes/vertices/faces/subsets/has_colors/out_vertices equal xbin.py', not diffs, f'{len(diffs)} differ, e.g. {diffs[:2]}')
    print(f"      with vertex colours {cpp['x_summary']['with_colors']}, file vertices {cpp['x_summary']['vertices']}, "
          f"after normal split {cpp['x_summary']['out_vertices']}")

    # ---- A2: texture resolution, Load and Exists orders, against texres_ext.py.
    ctex = {t['area']: t for t in cpp['textures']}
    for area in AREAS:
        lev, ar = area.split('/')
        py = texres_ext.resolve_area(lev, ar)
        c = {n['name']: {'chosen': n['chosen'], 'load': n['load'], 'exists': n['exists']} for n in ctex[area]['names']}
        same = py == c
        resolved = [v for v in py.values() if v['load']]
        agree = all(v['load'].split(':')[:2] == v['exists'].split(':')[:2] for v in resolved)
        missing = sorted(k for k, v in py.items() if not v['load'])
        check(f'A2 {area}: per-name chosen/load/exists equal texres_ext.py', same,
              f'{len(py)} names, {len(resolved)} resolved, missing {len(missing)}' + ('' if same else f'; differ {[k for k in set(py) | set(c) if py.get(k) != c.get(k)][:4]}'))
        check(f'A2 {area}: Load and Exists pick the same blockfile for every resolved name', agree)
    # The 40-byte-name entry that the game cannot find (lev3.blk, used by lev3/boss).
    boss = texres_ext.resolve_area('lev3', 'boss')
    print('      lev3/boss po_lev_10xx_rockwall4eground01_trans ->', boss.get('po_lev_10xx_rockwall4eground01_trans'))

    # ---- A5: DDS decode counts and bit-exact BC1-3 on 20 sampled textures (every block) + all A8R8G8B8.
    dds = []
    for rel, p in blks:
        for name, b in entries(p):
            if name.lower().endswith('.dds'):
                fmt = None
                if len(b) >= 128 and b[:4] == b'DDS ':
                    pf, cc, bits = struct.unpack_from('<I4sI', b, 80)
                    fmt = cc.decode('latin1') if pf & 4 else ('A8R8G8B8' if bits == 32 else '?')
                    w, h = struct.unpack_from('<II', b, 16)[0], struct.unpack_from('<I', b, 12)[0]
                dds.append((rel, name, fmt, len(b), (w, h) if fmt else None))
    pyfmt = collections.Counter(f for _, _, f, _, _ in dds if f)
    empty = sorted((r, n) for r, n, f, sz, _ in dds if f is None)
    cfail = sorted((f['blk'], f['entry']) for f in cpp['dds_summary']['failures'])
    check('A5 3,247 .dds entries; c++ decodes every one that is a DDS', len(dds) == 3247 and cpp['dds_summary']['ok'] == len(dds) - len(empty),
          f'{len(dds)} entries, {len(empty)} are not DDS files, c++ ok {cpp["dds_summary"]["ok"]}')
    check('A5 the entries c++ rejects are exactly the 0-byte ones', cfail == empty and all(sz == 0 for _, _, f, sz, _ in dds if f is None), str(empty))
    check('A5 format counts equal', dict(pyfmt) == cpp['dds_summary']['formats'], f'python {dict(pyfmt)}')
    unreachable = {('data/levels/lev3/lev3.blk', 'po_lev_10xx_rockwall4eground01_trans.dds'),
                   ('data/characters/Textures.blk', 'ns_cha_20xx_mask_of_translation_bump.dds')}
    pool = [d for d in dds if d[2] and (d[0], d[1]) not in unreachable]
    sample = []
    for fmt, k in (('DXT1', 2), ('DXT3', 8), ('DXT5', 10)):
        cand = sorted((d for d in pool if d[2] == fmt), key=lambda d: (d[0], d[1]))
        big = max(cand, key=lambda d: d[4][0] * d[4][1])
        pick = [cand[(i * len(cand)) // k] for i in range(k)]
        if big not in pick:
            pick[-1] = big
        nonsq = next((d for d in cand if d[4][0] != d[4][1] and d not in pick), None)
        if nonsq and fmt != 'DXT1':
            pick[0] = nonsq
        sample += pick
    sample += [d for d in pool if d[2] == 'A8R8G8B8']
    mism = []
    blocks = 0
    with tempfile.TemporaryDirectory() as td:
        args = [a.exe, install]
        outs = []
        for i, (rel, name, fmt, sz, wh) in enumerate(sample):
            o = os.path.join(td, f'{i}.rgba')
            args += ['--dump', 'Root/' + rel, name, o]
            outs.append(o)
        r = subprocess.run(args, capture_output=True, timeout=300)
        check('A5 lomn_datacheck --dump of the sample exits 0', r.returncode == 0, r.stderr.decode()[-200:])
        for (rel, name, fmt, sz, wh), o in zip(sample, outs):
            data = dict(entries(os.path.join(install, rel)))[name]
            w, h, f2, rgba = bcdec.decode_dds(data)
            got = open(o, 'rb').read()
            if struct.unpack_from('<II', got, 0) != (w, h) or got[8:] != rgba:
                diff = sum(1 for i in range(0, len(rgba)) if got[8 + i] != rgba[i]) if len(got) == len(rgba) + 8 else -1
                mism.append((rel, name, fmt, diff))
            if fmt != 'A8R8G8B8':
                blocks += ((w + 3) // 4) * ((h + 3) // 4)
    nbc = sum(1 for d in sample if d[2] != 'A8R8G8B8')
    check(f'A5 BC1-3 bit-exact vs bcdec.py on every block of {nbc} sampled textures (+{len(sample) - nbc} A8R8G8B8)', not mism and nbc == 20,
          f'{blocks} blocks; sizes {sorted(set(d[4] for d in sample))}; mismatches {mism[:3]}')

    # ---- extra: textures the drawn models load (spec 5 quotes vllg 23 / 1.0 MB, crss 37 / 5.0 MB).
    for area in ('lev1/vllg', 'lev4/crss'):
        t = ctex[area]
        print(f"      {area}: drawn models {len(t['drawn_models'])}, drawn textures {t['drawn_textures']} "
              f"({t['drawn_texture_bytes'] / 1e6:.2f} MB)")
    tga = cpp['tga_summary']
    check('TGA: every .tga entry decodes', tga['ok'] == tga['files'], f"{tga['files']} files, kinds {tga['kinds']}")
    check('SLB: every .slb passes the bounded fix-up reader', not cpp['slb_summary']['failures'],
          f"{cpp['slb_summary']['total']} tables, {cpp['slb_summary']['with_trailer']} with 0x00C0FFEE trailer")

    nfail = sum(1 for _, ok, _ in results if not ok)
    print(f'\n{len(results) - nfail}/{len(results)} checks passed ({time.time() - t0:.1f} s)')
    return 1 if nfail else 0


if __name__ == '__main__':
    sys.exit(main())
