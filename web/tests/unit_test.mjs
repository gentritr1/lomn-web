// Node unit tests of the page's untrusted-input code (no browser): build identification, builds.json shape checks, the
// copy/index file rule, <input webkitdirectory> path handling and the engine's range-list parser.
// Run: cd web && timeout 60 node --test tests/unit_test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { blkTablesFingerprint, checkBuilds, classify, MAX_BLK_ENTRIES, MAX_TOTAL_TABLE_BYTES } from '../app/fingerprint.js';
import { filesSource, wanted } from '../app/byo.js';
import { parseRanges } from '../app/engine.js';

const H = (n) => n.toString(16).padStart(2, '0').repeat(32);
const builds = checkBuilds({
  accepted: [{ id: 'good', exe_sha256: H(1) }],
  data: [{ id: 'gooddata', blk_tables_sha256: H(2) }],
  rejected: [{ id: 'rb', exe_sha256: H(3), name: 'REBUILT', reason: 'needs LOMNHook' }],
});

function blk(count, extra = 0) {
  const b = new Uint8Array(12 + count * 56 + extra);
  b.set([0x42, 0x4c, 0x4b, 0x46]);
  new DataView(b.buffer).setUint32(8, count, true);
  return b;
}
const fromBytes = (files) => ({
  entries: Object.entries(files).map(([path, bytes]) => ({ path, size: bytes.length, bytes })),
  read: async (e, o, l) => e.bytes.subarray(o, o + l),
});

test('classify: accepted, rejected-list (named), unknown exe, unknown data', () => {
  assert.equal(classify(builds, H(1), H(2)).ok, true);
  assert.match(classify(builds, H(3), H(2)).message, /^This is REBUILT \(LEGOBionicle\.exe SHA-256 030303030303…\), which is not supported: needs LOMNHook$/);
  assert.match(classify(builds, H(9), H(2)).message, /^Unknown build: LEGOBionicle\.exe SHA-256 090909090909…/);
  // C2 / D13: accepted exe + unknown tables is soft-accepted with a warning naming the fingerprint (no hard rejection).
  const soft = classify(builds, H(1), H(8));
  assert.equal(soft.ok, true);
  assert.equal(soft.data, 'unknown');
  assert.match(soft.warning, /^Game data differs from the known build — rendering may differ \(\.blk table fingerprint 080808080808…/);
});

test('checkBuilds rejects malformed lists', () => {
  assert.throws(() => checkBuilds({ accepted: [{ id: 'x', exe_sha256: 'nothex' }] }), /bad accepted entry/);
  assert.throws(() => checkBuilds({ accepted: [], data: [{ id: '', blk_tables_sha256: H(1) }] }), /bad data entry/);
  assert.throws(() => checkBuilds({ accepted: [], rejected: [{ id: 'r', exe_sha256: H(1) }] }), /bad rejected entry/);
  assert.throws(() => checkBuilds({ accepted: new Array(65).fill({ id: 'a', exe_sha256: H(1) }) }), /bad accepted list/);
  assert.throws(() => checkBuilds(null), /bad accepted list/);
});

test('blkTablesFingerprint: order-independent, path-case-insensitive, bounded', async () => {
  const a = fromBytes({ 'data/A.blk': blk(2, 100), 'data/levels/x.blk': blk(0), 'data/b.txt': new Uint8Array(3) });
  const b = fromBytes({ 'data/levels/X.BLK': blk(0), 'DATA/a.blk': blk(2, 100) });
  const fa = await blkTablesFingerprint(a.entries, a.read);
  const fb = await blkTablesFingerprint(b.entries, b.read);
  assert.equal(fa.fingerprint, fb.fingerprint);
  assert.equal(fa.blkFiles, 2);
  assert.equal(fa.tableBytes, 12 + 2 * 56 + 12);
  // The entry bytes after the table do not count; the table does.
  const c = fromBytes({ 'data/A.blk': (() => { const x = blk(2, 100); x[12 + 2 * 56 + 5] = 7; return x; })(), 'data/levels/x.blk': blk(0) });
  assert.equal((await blkTablesFingerprint(c.entries, c.read)).fingerprint, fa.fingerprint);
  const d = fromBytes({ 'data/A.blk': (() => { const x = blk(2, 100); x[20] = 7; return x; })(), 'data/levels/x.blk': blk(0) });
  assert.notEqual((await blkTablesFingerprint(d.entries, d.read)).fingerprint, fa.fingerprint);
  const bad = async (files, re) => { const s = fromBytes(files); await assert.rejects(blkTablesFingerprint(s.entries, s.read), re); };
  await bad({ 'data/a.blk': new Uint8Array(20) }, /no BLKF magic/);
  await bad({ 'data/a.blk': new Uint8Array(5) }, /too small/);
  await bad({ 'data/a.blk': blk(3).subarray(0, 12 + 2 * 56) }, /entry table larger than the file/);
  const huge = blk(0);
  new DataView(huge.buffer).setUint32(8, MAX_BLK_ENTRIES + 1, true);
  await bad({ 'data/a.blk': huge }, /cap/);
  await bad({ 'other/a.blk': blk(0) }, /not a LOMN install/);
});

test('blkTablesFingerprint: the total table byte cap (review fix 4: a mutant without it survived)', async () => {
  // Each file declares the largest legal table (MAX_BLK_ENTRIES entries = 3,670,028 bytes); the bytes are made up on read, so
  // nothing big is held. 64 MiB holds 18 such tables and not 19.
  const per = 12 + MAX_BLK_ENTRIES * 56;
  assert.equal(Math.floor(MAX_TOTAL_TABLE_BYTES / per), 18);
  const files = (n) => Array.from({ length: n }, (_, i) => ({ path: `data/t${String(i).padStart(2, '0')}.blk`, size: per }));
  const read = async (e, o, l) => {
    const b = new Uint8Array(l);
    b.set([0x42, 0x4c, 0x4b, 0x46]);
    new DataView(b.buffer).setUint32(8, MAX_BLK_ENTRIES, true);
    return b;
  };
  const ok = await blkTablesFingerprint(files(18), read);
  assert.equal(ok.tableBytes, 18 * per);
  await assert.rejects(blkTablesFingerprint(files(19), read), /blockfile tables over the size cap/);
});

test('blkTablesFingerprint: names sort by UTF-8 bytes, as the Python oracle does (review fix 6)', async () => {
  // Same install as tools/tests/test_byo_fingerprint.py::test_non_ascii_names_sort_by_utf8_bytes_in_js_and_python: tables are
  // "BLKF", u32 7, u32 count, bytes 0..55 per entry; the oracle's fingerprint for it is hard-coded there and here.
  // U+FF21 (UTF-8 EF BC A1) sorts before U+1F600 (F0 9F 98 80) by bytes, after it by UTF-16 code unit.
  const mk = (count, tail) => {
    const b = new Uint8Array(12 + count * 56 + tail);
    b.set([0x42, 0x4c, 0x4b, 0x46]);
    new DataView(b.buffer).setUint32(4, 7, true);
    new DataView(b.buffer).setUint32(8, count, true);
    for (let i = 0; i < count; ++i) for (let j = 0; j < 56; ++j) b[12 + i * 56 + j] = j;
    return b;
  };
  const f = fromBytes({ 'data/\u{1F600}.blk': mk(1, 2), 'data/\u00e9.blk': mk(0, 3), 'data/Zed.blk': mk(2, 0), 'data/\uff21.blk': mk(1, 1) });
  const r = await blkTablesFingerprint(f.entries, f.read);
  assert.equal(r.blkFiles, 4);
  assert.equal(r.fingerprint, '1d3c18ccade5d471383564557ea58b48b0231674b0ff062047d8e0a50bd5175a');
});

test('wanted(): the exe at the top and data/ except dot files, exes and DLLs', () => {
  assert.equal(wanted('LEGOBionicle.exe'), true);
  assert.equal(wanted('data/levels/lev1/vllg.blk'), true);
  assert.equal(wanted('Data/Base.osi'), true);
  assert.equal(wanted('data/Cinematics/Binkplay.exe'), false);
  assert.equal(wanted('data/Cinematics/binkw32.DLL'), false);  // a DLL inside data/ (mutation 17 of mutate4.py)
  assert.equal(wanted('mods/BetterSaver.dll'), false);
  assert.equal(wanted('data/.DS_Store'), false);
  assert.equal(wanted('binkw32.dll'), false);
  assert.equal(wanted('sub/LEGOBionicle.exe'), false);
});

test('filesSource: strips the picked folder, keeps only wanted files', () => {
  const mk = (rel, n) => {
    const f = new File([new Uint8Array(n)], rel.split('/').pop());
    Object.defineProperty(f, 'webkitRelativePath', { value: rel });
    return f;
  };
  const s = filesSource([mk('install/LEGOBionicle.exe', 4), mk('install/data/Base.osi', 2), mk('install/mods/x.dll', 1), mk('install/data/.DS_Store', 1)]);
  assert.equal(s.name, 'install');
  assert.deepEqual(s.entries.map((e) => [e.path, e.size]), [['LEGOBionicle.exe', 4], ['data/Base.osi', 2]]);
});

test('parseRanges: integers only, malformed lines rejected', () => {
  assert.deepEqual(parseRanges('root/a\t0\t12\nroot/b\t12\t560\n'), [{ path: 'root/a', offset: 0, length: 12 }, { path: 'root/b', offset: 12, length: 560 }]);
  assert.throws(() => parseRanges('root/a\t-1\t12\n'), /malformed/);
  assert.throws(() => parseRanges('root/a\t1.5\t12\n'), /malformed/);
  assert.throws(() => parseRanges('root/a\t1\n'), /malformed/);
  assert.throws(() => parseRanges('\t1\t2\n'), /malformed/);
});
