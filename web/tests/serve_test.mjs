// Unit tests for the test static server: path containment, malformed escapes, `/` -> index.html.
// Run: cd web && timeout 60 node --test tests/serve_test.mjs
import { test, before, after } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { serve } from './serve.mjs';

let tmp; let srv;
before(async () => {
  tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'lomn-serve-'));
  fs.mkdirSync(path.join(tmp, 'site'));
  fs.mkdirSync(path.join(tmp, 'site-secret'));
  fs.writeFileSync(path.join(tmp, 'site', 'index.html'), '<!doctype html>ok');
  fs.writeFileSync(path.join(tmp, 'site-secret', 'x.txt'), 'secret');
  srv = await serve(path.join(tmp, 'site'));
});
after(() => { srv.close(); fs.rmSync(tmp, { recursive: true, force: true }); });

const get = (p) => fetch(srv.url.replace(/\/$/, '') + p);

test('serves index.html for / and by name', async () => {
  assert.equal((await get('/')).status, 200);
  assert.equal((await get('/index.html')).status, 200);
});

test('sibling directory sharing the root prefix is not reachable (..%2f)', async () => {
  assert.equal((await get('/..%2fsite-secret/x.txt')).status, 404);
});

test('sibling directory is not reachable (%2e%2e%2f)', async () => {
  assert.equal((await get('/%2e%2e%2fsite-secret/x.txt')).status, 404);
});

test('malformed percent escape answers 400 and the server keeps serving', async () => {
  assert.equal((await get('/%E0%A4%A')).status, 400);
  assert.equal((await get('/index.html')).status, 200);
});
