// REF4 revision 2: unit tests of the new untrusted-input helpers (each imports its module itself, so on code without a
// helper only that test fails). Run: cd web && timeout 60 node --test tests/unit_r2_test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';

const mkFile = (rel, n) => {
  const f = new File([new Uint8Array(n)], rel.split('/').pop());
  Object.defineProperty(f, 'webkitRelativePath', { value: rel });
  return f;
};

test('M12 checkRelPath: no empty, ".", ".." or over-long segments; byte length counted in UTF-8', async () => {
  const { checkRelPath, MAX_PATH_BYTES } = await import('../app/paths.js');
  assert.equal(checkRelPath('data/Base.osi'), '');
  for (const bad of ['', '/data/x', 'data//x', 'data/./x', 'data/../x', '..', 'data/x/']) assert.notEqual(checkRelPath(bad), '', bad);
  assert.equal(MAX_PATH_BYTES, 1024);
  const nonAscii = 'data/' + 'é'.repeat(600);  // 605 UTF-16 units, 1205 UTF-8 bytes
  assert.ok(nonAscii.length < 1024);
  assert.match(checkRelPath(nonAscii), /bytes/);
});

test('review fix 5 checkRelPath: backslash and NUL are rejected with a named reason (the engine reads "\\" as a separator)', async () => {
  const { checkRelPath } = await import('../app/paths.js');
  assert.match(checkRelPath('data\\foo\\..\\Base.osi'), /backslash/);
  assert.match(checkRelPath('data/x\\..\\..\\..\\evil.blk'), /backslash/);
  assert.match(checkRelPath('data/a\u0000b.blk'), /NUL/);
  assert.match(checkRelPath('\u0000'), /NUL/);
  assert.equal(checkRelPath('data/ok name.blk'), '');
  // They are skipped with a warning at both entry points, and a completion manifest naming one is refused.
  const { filesSource, checkCopyManifest } = await import('../app/byo.js');
  const s = filesSource([mkFile('inst/LEGOBionicle.exe', 4), mkFile('inst/data/foo\\..\\Base.osi', 2), mkFile('inst/data/a\u0000b.osi', 1)]);
  assert.deepEqual(s.entries.map((e) => e.path), ['LEGOBionicle.exe']);
  assert.match(s.warnings.join('\n'), /backslash/);
  assert.match(s.warnings.join('\n'), /NUL/);
  assert.match(checkCopyManifest({ format: 'lomn-opfs-copy', version: 1, files: [{ path: 'data\\x', size: 1 }] }), /entry/);
});

test('final I-2 checkRelPath: control characters (TAB, LF, anything < 0x20, DEL) are rejected with a reason naming the character', async () => {
  const { checkRelPath } = await import('../app/paths.js');
  // The engine<->JS protocols are tab/newline separated ("lev\tarea\n", "path\toffset\tlength\n"): such a name must never get in.
  assert.match(checkRelPath('data/levels/levt/a\tb.blk'), /control character U\+0009/);
  assert.match(checkRelPath('data/levels/levt/a\nb.blk'), /control character U\+000A/);
  assert.match(checkRelPath('data/levels/levt/a\rb.blk'), /control character U\+000D/);
  assert.match(checkRelPath('data/a\u0001b.blk'), /control character U\+0001/);
  assert.match(checkRelPath('data/a\u001fb.blk'), /control character U\+001F/);
  assert.match(checkRelPath('data/a\u007fb.blk'), /control character U\+007F/);
  assert.match(checkRelPath('d\tata/x'), /control character/);          // in a folder name too
  assert.equal(checkRelPath('data/a b.blk'), '');                        // a space (0x20) is fine
  assert.equal(checkRelPath('data/éあ.blk'), '');               // non-ASCII is fine (only < 0x20 and 0x7f are refused)
  // Skipped with a warning at both entry points (the picked folder, the <input>), naming the character; the rest of the install stays usable.
  const files = [mkFile('inst/LEGOBionicle.exe', 4), mkFile('inst/data/levels/levt/ok.blk', 2), mkFile('inst/data/levels/levt/a\tb.blk', 1), mkFile('inst/data/x\ny.osi', 1)];
  return import('../app/byo.js').then(async ({ filesSource, walkHandle, checkCopyManifest }) => {
    // A completion manifest naming such a file (a tampered OPFS copy) is refused too.
    assert.match(checkCopyManifest({ format: 'lomn-opfs-copy', version: 1, files: [{ path: 'data/a\tb.blk', size: 1 }] }), /entry/);
    const s = filesSource(files);
    assert.deepEqual(s.entries.map((e) => e.path), ['LEGOBionicle.exe', 'data/levels/levt/ok.blk']);
    assert.match(s.warnings.join('\n'), /control character U\+0009/);
    assert.match(s.warnings.join('\n'), /control character U\+000A/);
    const fakeFile1 = (n) => ({ kind: 'file', getFile: async () => ({ size: n }) });
    const dir = (entries) => ({ kind: 'directory', entries: async function* () { for (const e of entries) yield e; } });
    const w = await walkHandle(dir([['LEGOBionicle.exe', fakeFile1(4)], ['data', dir([['a\tb.blk', fakeFile1(1)], ['ok.blk', fakeFile1(2)], ['f\u0002', dir([['x.blk', fakeFile1(1)]])]])]]));
    assert.deepEqual(w.entries.map((e) => e.path).sort(), ['LEGOBionicle.exe', 'data/ok.blk']);
    assert.match(w.warnings.join('\n'), /control character U\+0009/);
    assert.match(w.warnings.join('\n'), /skipped folder .*control character U\+0002/);
  });
});

test('M11 filesSource: over-long (UTF-8) paths and case duplicates are skipped with a warning, never fatal', async () => {
  const { filesSource } = await import('../app/byo.js');
  const s = filesSource([mkFile('inst/LEGOBionicle.exe', 4), mkFile('inst/data/Base.osi', 2), mkFile('inst/data/base.OSI', 3),
                         mkFile('inst/data/' + 'é'.repeat(600) + '.blk', 1)]);
  assert.deepEqual(s.entries.map((e) => e.path), ['LEGOBionicle.exe', 'data/Base.osi']);
  assert.equal(s.warnings.length, 2);
  assert.match(s.warnings.join('\n'), /same name/);
  assert.match(s.warnings.join('\n'), /bytes/);
});

test('I6 checkCopyManifest / sameFiles: the completion manifest is shape-checked and compared with the copy', async () => {
  const { checkCopyManifest, sameFiles } = await import('../app/byo.js');
  const ok = { format: 'lomn-opfs-copy', version: 1, files: [{ path: 'LEGOBionicle.exe', size: 4 }, { path: 'data/Base.osi', size: 2 }] };
  assert.equal(checkCopyManifest(ok), '');
  assert.match(checkCopyManifest({ ...ok, files: [{ path: 5, size: 1 }] }), /entry/);
  assert.match(checkCopyManifest({ ...ok, files: [{ path: 'a', size: -1 }] }), /entry/);
  assert.match(checkCopyManifest({ ...ok, files: [{ path: 'data/../x', size: 1 }] }), /entry/);
  assert.match(checkCopyManifest({ ...ok, files: new Array(70000).fill({ path: 'a', size: 1 }) }), /files/);
  assert.match(checkCopyManifest({ ...ok, version: 2 }), /format/);
  const entries = [{ path: 'LEGOBionicle.exe', size: 4 }, { path: 'data/Base.osi', size: 2 }];
  assert.equal(sameFiles(entries, ok.files), '');
  assert.match(sameFiles(entries.slice(0, 1), ok.files), /missing.*data\/Base\.osi/);
  assert.match(sameFiles([{ path: 'LEGOBionicle.exe', size: 4 }, { path: 'data/Base.osi', size: 3 }], ok.files), /size/);
});

test('M2 filterCached: a cached prefetch set over the byte cap is dropped (discovery runs instead)', async () => {
  const { filterCached, MAX_CACHED_BYTES } = await import('../app/engine.js');
  const byNorm = new Map([['root/a', { size: 1 << 30 }]]);
  assert.equal(MAX_CACHED_BYTES, 64 * 1024 * 1024);
  assert.equal(filterCached([{ path: 'root/a', offset: 0, length: 100 }, { path: 'root/zz', offset: 0, length: 1 }], byNorm).length, 1);
  assert.deepEqual(filterCached([{ path: 'root/a', offset: 0, length: MAX_CACHED_BYTES + 1 }], byNorm), []);
  assert.deepEqual(filterCached([{ path: 'root/a', offset: 2 ** 30 - 5, length: 10 }], byNorm), []);
});

// ---- review fix 7: walkHandle (the showDirectoryPicker / OPFS-copy source) on large, deep or unrelated folders ----
const fakeFile = (n) => ({ kind: 'file', getFile: async () => ({ size: n }) });
const fakeDir = (entries) => ({ kind: 'directory', entries: async function* () { for (const e of entries) yield e; } });
const nested = (levels, leaf) => { let d = leaf; for (let i = 0; i < levels; ++i) d = fakeDir([['d' + i, d]]); return d; };

test('review fix 7 walkHandle: folders beside data/ are never opened; too-deep subtrees and bad names are skipped with a warning', async () => {
  const { walkHandle, MAX_DEPTH } = await import('../app/byo.js');
  const opened = [];
  const trap = (name) => ({ kind: 'directory', entries: () => { opened.push(name); throw new Error('must not be opened: ' + name); } });
  const root = fakeDir([
    ['LEGOBionicle.exe', fakeFile(4)],
    ['Documents', trap('Documents')],       // an unrelated (huge, deep, or unreadable) folder next to data/
    ['.git', trap('.git')],
    ['data', fakeDir([
      ['Base.osi', fakeFile(2)],
      ['base.OSI', fakeFile(3)],            // case duplicate
      ['bad\\name.blk', fakeFile(1)],       // backslash (review fix 5)
      ['deep', nested(MAX_DEPTH + 2, fakeDir([['x.blk', fakeFile(1)]]))],   // deeper than MAX_DEPTH
      ['levels', fakeDir([['lev1.blk', fakeFile(7)]])],
    ])],
  ]);
  const w = await walkHandle(root);
  assert.deepEqual(opened, []);
  assert.deepEqual(w.entries.map((e) => e.path).sort(), ['LEGOBionicle.exe', 'data/Base.osi', 'data/levels/lev1.blk']);
  const text = w.warnings.join('\n');
  assert.match(text, /same name/);
  assert.match(text, /backslash/);
  assert.match(text, /nested deeper than/);
});

test('review fix 7 walkHandle: more than MAX_FILES entries is its own named error; a moved folder is a different one', async () => {
  const { walkHandle, WalkError, MAX_FILES, describeWalkFailure } = await import('../app/byo.js');
  const many = { kind: 'directory', entries: async function* () { for (let i = 0; i <= MAX_FILES; ++i) yield ['f' + i + '.osi', fakeFile(1)]; } };
  let tooMany;
  await assert.rejects(walkHandle(fakeDir([['data', many]])), (e) => { tooMany = e; return e instanceof WalkError && e.name === 'WalkError' && /more than 65536 entries/.test(e.message); });
  // The folder is gone (what entries() throws for a moved or deleted handle): not a WalkError.
  const gone = Object.assign(new Error('A requested file or directory could not be found'), { name: 'NotFoundError' });
  let moved;
  await assert.rejects(walkHandle({ kind: 'directory', entries: () => { throw gone; } }), (e) => { moved = e; return !(e instanceof WalkError); });
  const a = describeWalkFailure('lomn', tooMany), b = describeWalkFailure('lomn', moved);
  assert.match(a, /"lomn".*not a usable game folder.*more than 65536 entries/);
  assert.match(b, /"lomn" could not be read \(moved or deleted\)/);
  assert.doesNotMatch(a, /moved or deleted/);
});

// T8 P3: Engine.rounds() against a fake wasm module and a fake file source (no engine, no browser). Two error paths no
// other test drives: rounds exhausted ("did not converge") and a round that moved nothing ("made no progress"). The fake
// probe throws past PROBE_BOUND calls and each test has a deadline, so a removed guard fails the test instead of hanging.
const PROBE_BOUND = 64;
function fakeEngine(Engine, fileBytes) {
  const strings = new Map();  // fake heap strings: pointer -> JS string
  let nextPtr = 1;
  const resident = new Set(), reserved = new Map();
  const key = (p, o, l) => `${p}\t${o}\t${l}`;
  const M = {
    HEAPU8: new Uint8Array(1 << 16),
    stringToNewUTF8: (s) => (strings.set(nextPtr, s), nextPtr++),
    UTF8ToString: (p) => strings.get(p) ?? '',
    _free: (p) => strings.delete(p),
    _lomn_last_error: () => M.stringToNewUTF8('fake: no error'),
    _lomn_fs_is_resident: (p, o, l) => (resident.has(key(strings.get(p), o, l)) ? 1 : 0),
    _lomn_fs_reserve: (p, o, l) => (reserved.set(4096, key(strings.get(p), o, l)), 4096),
    _lomn_fs_commit: (ptr) => resident.add(reserved.get(ptr)),
    _lomn_fs_abort: (ptr) => reserved.delete(ptr),
    misses: '',
    _lomn_fs_misses: () => M.stringToNewUTF8(M.misses),
  };
  const entry = { path: 'data/fake.blk', size: fileBytes };
  const e = new Engine(M, { entries: [entry], file: async () => new Blob([new Uint8Array(fileBytes)]) }, () => {});
  e.byNorm.set('root/data/fake.blk', entry);
  return { e, M, resident };
}
// C-a2: rounds() runs under its own deadline (default 5 s): a stall (a file read whose promise never settles) is an error with a
// message that names it, not a silent wait for the test timeout or the Makefile's 60 s `timeout`. The timer is always cleared.
async function roundsError(e, probe, deadlineMs = 5000) {
  let timer;
  const stall = new Promise((_, reject) => { timer = setTimeout(() => reject(new Error(`rounds(levt/fake) did not settle within ${deadlineMs} ms (a stalled read, or a guard that never fires)`)), deadlineMs); });
  try {
    await Promise.race([e.rounds(probe, 'levt/fake'), stall]);
  } catch (err) {
    return err.message;
  } finally {
    clearTimeout(timer);
  }
  return 'resolved without an error';
}

test('T8 P3 rounds: a source that misses a NEW range every round stops at MAX_ROUNDS with "did not converge"', { timeout: 10000 }, async () => {
  const { Engine, MAX_ROUNDS } = await import('../app/engine.js');
  const { e, M, resident } = fakeEngine(Engine, 4 * PROBE_BOUND);
  let calls = 0;
  const probe = () => {
    if (++calls > PROBE_BOUND) throw new Error(`fake probe called ${calls} times (the round cap did not stop the load)`);
    M.misses = `root/data/fake.blk\t${4 * (calls - 1)}\t4\n`;  // a range never fetched before
    return 1;
  };
  const msg = await roundsError(e, probe);
  assert.equal(msg, `levt/fake: prefetch did not converge in ${MAX_ROUNDS} rounds`);
  assert.equal(calls, MAX_ROUNDS);
  assert.equal(resident.size, MAX_ROUNDS);  // every round made progress: the cap, not the progress guard, stopped it
});

test('T8 P3 rounds: a source that keeps missing the SAME range fails with "made no progress" in round 2', { timeout: 10000 }, async () => {
  const { Engine } = await import('../app/engine.js');
  const { e, M, resident } = fakeEngine(Engine, 64);
  let calls = 0;
  const probe = () => {
    if (++calls > PROBE_BOUND) throw new Error(`fake probe called ${calls} times (the progress guard did not stop the load)`);
    M.misses = 'root/data/fake.blk\t8\t16\n';  // fetched in round 1, still reported missing in round 2
    return 1;
  };
  const msg = await roundsError(e, probe);
  assert.equal(msg, 'levt/fake: prefetch made no progress (root/data/fake.blk)');
  assert.equal(calls, 2);
  assert.equal(resident.size, 1);
});

// M1 close-out C-a2: a rounds() that never settles (a file read whose promise never resolves) used to fail only at the test's own
// 10 s timeout, and if the stalled thing held a handle (a timer, a pending fetch) the node process then lived on until the Makefile's
// 60 s `timeout`. The error path now has its own bounded failure with a message that names the stall.
test('C-a2 rounds: a file read that never settles fails with its own message at the bound, long before the 60 s make timeout', { timeout: 15000 }, async () => {
  const { Engine } = await import('../app/engine.js');
  const { e, M } = fakeEngine(Engine, 64);
  e.source.file = () => new Promise(() => {});  // the read never settles
  const hold = setInterval(() => {}, 1000);       // ... and keeps a handle open, as a pending fetch does
  let calls = 0;
  const probe = () => {
    if (++calls > PROBE_BOUND) throw new Error('fake probe called too often');
    M.misses = 'root/data/fake.blk\t0\t16\n';
    return 1;
  };
  const t0 = Date.now();
  try {
    const msg = await roundsError(e, probe, 1500);
    assert.match(msg, /rounds\(levt\/fake\) did not settle within 1500 ms/);
    assert.ok(Date.now() - t0 < 5000, `took ${Date.now() - t0} ms`);
  } finally {
    clearInterval(hold);
  }
});

// M1 close-out C-f2: what Engine.showArea writes back to the prefetch-set cache. lomn_fs_touched() lists the RESIDENT ranges a load
// hit, so a tampered cache holding one whole-file range "touches" that range with every read inside it, and persisting `touched`
// kept the tampered superset for ever (final review m-2: it only costs a fetch, but on every visit). A load that used a cached set now
// writes back the ranges the engine actually READ (lomn_fs_reads: exact lines, zero-length reads excluded). Fake wasm module, fake
// store (a Map instead of IndexedDB), no browser.
const R1 = 'root/data/fake.blk\t16\t32', R2 = 'root/data/fake.blk\t100\t8', WHOLE = 'root/data/fake.blk\t0\t4096';
async function areaEngine(initialStore) {
  const { Engine } = await import('../app/engine.js');
  const { e, M, resident } = fakeEngine(Engine, 4096);
  const reads = [R1, R2];
  const have = (line) => resident.has(line) || resident.has(WHOLE);
  M._lomn_area_begin = () => {};
  M._lomn_area_probe = () => { const miss = reads.filter((l) => !have(l)); M.misses = miss.map((l) => l + '\n').join(''); return miss.length; };
  M._lomn_area_show = () => 1;
  M._lomn_fs_touched = () => M.stringToNewUTF8(resident.has(WHOLE) ? WHOLE + '\n' : reads.filter((l) => resident.has(l)).map((l) => l + '\n').join(''));
  M._lomn_fs_reads = () => M.stringToNewUTF8(reads.map((l) => l + '\n').join(''));
  const map = new Map(Object.entries(initialStore));
  e.store = { get: async (k) => map.get(k), set: async (k, v) => { map.set(k, v); }, delete: async (k) => { map.delete(k); }, keys: async () => [...map.keys()] };
  e.cacheKey = 'k|';
  return { e, map, resident };
}
test('C-f2 a tampered superset cache (one whole-file range) is NOT written back: the persisted set is the exact reads', { timeout: 10000 }, async () => {
  const { e, map } = await areaEngine({ 'k|area:lev/ar': WHOLE + '\n' });
  const load = await e.showArea('lev', 'ar');
  assert.equal(load.cached, true);
  assert.equal(load.rounds, 1);                                   // the superset satisfied every read: one round, nothing to discover
  assert.equal(load.touched.trim(), WHOLE);                       // ... and it IS "touched": this is why touched cannot be persisted
  assert.equal(map.get('k|area:lev/ar'), `${R1}\n${R2}\n`);       // the exact reads, the superset is gone
});
test('C-f2 a cache that misses a read heals: the discovered range joins the persisted set; an honest cache stays as it is; a first visit persists what it fetched', { timeout: 10000 }, async () => {
  const sub = await areaEngine({ 'k|area:lev/ar': R1 + '\n' });   // a subset: R2 is missed, discovered in round 2
  const ls = await sub.e.showArea('lev', 'ar');
  assert.equal(ls.rounds, 2);
  assert.equal(sub.map.get('k|area:lev/ar'), `${R1}\n${R2}\n`);
  const honest = await areaEngine({ 'k|area:lev/ar': `${R1}\n${R2}\n` });
  const lh = await honest.e.showArea('lev', 'ar');
  assert.equal(lh.rounds, 1);
  assert.equal(honest.map.get('k|area:lev/ar'), `${R1}\n${R2}\n`);
  const first = await areaEngine({});
  const lf = await first.e.showArea('lev', 'ar');
  assert.equal(lf.cached, false);
  assert.equal(first.map.get('k|area:lev/ar'), `${R1}\n${R2}\n`);
});
