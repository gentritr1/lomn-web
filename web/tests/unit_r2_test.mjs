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
