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
