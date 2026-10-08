// Node tests of lib.mjs' networkCheck against the real test server, no browser (T8 P10). A `window.open('/index.html?d=...')`
// popup is a top-level navigation (Sec-Fetch-Mode: navigate, Sec-Fetch-Dest: document), which the check used to treat as
// "the page's own navigation" and let carry a query string. Now only the navigations the test itself made (page.goto /
// page.reload, recorded by watch()) are allowed; any other top-level navigation is a finding.
// Run: cd web && timeout 60 node --test tests/network_check_test.mjs
import { test, before, after } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';
import { EventEmitter } from 'node:events';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { importSpecifiers, networkCheck, watch, workerSharedFiles } from './lib.mjs';
import { serve } from './serve.mjs';

let tmp, site, srv;
before(async () => {
  tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'lomn-netcheck-'));
  site = path.join(tmp, 'site');
  fs.mkdirSync(site);
  fs.writeFileSync(path.join(site, 'index.html'), '<!doctype html>ok');
  fs.writeFileSync(path.join(site, 'asset-manifest.json'), JSON.stringify({ total_bytes: 17, files: [{ path: 'index.html', bytes: 17 }] }));
  srv = await serve(site);
});
after(() => { srv.close(); fs.rmSync(tmp, { recursive: true, force: true }); });

// A request as Chromium makes it: a top-level navigation carries Sec-Fetch-Mode navigate + Sec-Fetch-Dest document.
const request = (pathAndQuery, headers = {}) => new Promise((resolve, reject) => {
  const u = new URL(srv.url);
  http.get({ host: u.hostname, port: u.port, path: pathAndQuery, headers }, (res) => { res.resume(); res.on('end', resolve); }).on('error', reject);
});
const NAV = { 'sec-fetch-mode': 'navigate', 'sec-fetch-dest': 'document' };
// A stand-in for a Playwright Page: watch() only needs on() and goto()/reload()/url().
const fakePage = () => {
  const page = new EventEmitter();
  page.visited = [];
  page.goto = async (u) => { page.visited.push(u); page.current = u; await request(new URL(u).pathname + new URL(u).search, NAV); };
  page.reload = async () => { await request(new URL(page.current).pathname + new URL(page.current).search, NAV); };
  page.url = () => page.current;
  return page;
};

test('the test\'s own navigations (goto, reload) with a query string stay clean', async () => {
  const page = fakePage();
  const w = watch(page);
  const r0 = srv.responses.length;
  await page.goto(srv.url + 'index.html?hud=1&area=levt/test');
  await page.reload();
  const n = networkCheck(srv, site, w, r0, 0);
  assert.equal(n.responses, 2);
  assert.deepEqual(n.navigations, []);
  assert.deepEqual(n.queries, []);
  assert.equal(n.ok, true, JSON.stringify(n));
});

test('positive control: a window.open popup (top-level navigation with a query string) the test did not make fails the check', async () => {
  const page = fakePage();
  const w = watch(page);
  await page.goto(srv.url + 'index.html?hud=1&area=levt/test');
  const r1 = srv.responses.length;
  await request('/index.html?d=' + 'A'.repeat(100), NAV);  // what window.open('/index.html?d=AAAA...') sends
  const n = networkCheck(srv, site, w, r1, 0);
  assert.equal(n.ok, false);
  assert.equal(n.navigations.length, 1, JSON.stringify(n.navigations));
  assert.equal(n.navigations[0].side, 'server');
  assert.match(n.navigations[0].url, /^\/index\.html\?d=A{100}$/);
});

test('a popup without a query string, and one to the page\'s own path with another query, are findings too; the page\'s own URL repeated is not', async () => {
  const page = fakePage();
  const w = watch(page);
  await page.goto(srv.url + 'index.html?hud=1');
  const r1 = srv.responses.length;
  await request('/index.html?hud=1', NAV);  // the same URL the test opened: nothing new can leak through it
  assert.equal(networkCheck(srv, site, w, r1, 0).ok, true);
  const r2 = srv.responses.length;
  await request('/index.html', NAV);
  assert.equal(networkCheck(srv, site, w, r2, 0).ok, false);
  const r3 = srv.responses.length;
  await request('/index.html?hud=1&d=secret', NAV);
  assert.equal(networkCheck(srv, site, w, r3, 0).ok, false);
});

test('a non-navigation request with a query string is still a query finding (unchanged rule)', async () => {
  const page = fakePage();
  const w = watch(page);
  const r1 = srv.responses.length;
  await request('/index.html?d=1', { 'sec-fetch-mode': 'cors', 'sec-fetch-dest': 'empty' });
  const n = networkCheck(srv, site, w, r1, 0);
  assert.equal(n.ok, false);
  assert.equal(n.queries.length, 1);
});

test('a browser-side top-level request the test did not make is a finding as well (a popup the opener\'s page sees)', () => {
  const page = fakePage();
  const w = watch(page);
  w.expected_navs.add('/index.html?hud=1');
  w.reqs.push({ url: srv.url + 'index.html?hud=1', method: 'GET', bytes: 0, top_nav: true });
  w.urls.push(srv.url + 'index.html?hud=1');
  assert.deepEqual(networkCheck(srv, site, w, srv.responses.length, 0).navigations, []);
  w.reqs.push({ url: srv.url + 'index.html?d=' + 'A'.repeat(50), method: 'GET', bytes: 0, top_nav: true });
  w.urls.push(srv.url + 'index.html?d=' + 'A'.repeat(50));
  const n = networkCheck(srv, site, w, srv.responses.length, 0);
  assert.equal(n.ok, false);
  assert.equal(n.navigations.length, 1);
  assert.equal(n.navigations[0].side, 'browser');
});

// ---- M1 close-out C-N2: per-path accounting -------------------------------------------------------------------------------
// Each built file is fetched at most once by a page, except the files opfs_worker.js imports (the page and the copy worker are
// separate module graphs, so both fetch them): at most twice. The byte ceiling is the built files' size plus the shared files' size
// once more (asset-manifest.json is not fetched by the page, so it is no headroom). The shared list is DERIVED from the worker's
// import statements; importSpecifiers() is cross-checked below against V8's own parser (vm.SourceTextModule).
const here = path.dirname(fileURLToPath(import.meta.url));
const mkSite = (dir) => {
  const files = {
    'index.html': '<!doctype html>ok',
    'app/main.1111111111.js': "import './paths.2222222222.js';\nexport const main = 1;\n",
    'app/opfs_worker.3333333333.js': "// the copy worker\nimport { checkRelPath } from './paths.2222222222.js';\nself.onmessage = () => checkRelPath('a');\n",
    'app/paths.2222222222.js': 'export const checkRelPath = () => "";\n',
    'app/other.4444444444.js': 'export const other = 1;\n',
  };
  fs.mkdirSync(path.join(dir, 'app'), { recursive: true });
  const list = [];
  for (const [p, text] of Object.entries(files)) {
    fs.writeFileSync(path.join(dir, p), text);
    list.push({ path: p, bytes: Buffer.byteLength(text) });
  }
  fs.writeFileSync(path.join(dir, 'asset-manifest.json'), JSON.stringify({ total_bytes: list.reduce((n, f) => n + f.bytes, 0), files: list }));
  return { list, size: (p) => list.find((f) => f.path === p).bytes, total: list.reduce((n, f) => n + f.bytes, 0) };
};
const WORKER = 'app/opfs_worker.3333333333.js', SHARED = 'app/paths.2222222222.js', OTHER = 'app/other.4444444444.js';

test('C-N2: the shared list is derived from the worker\'s import statements', () => {
  const dir = fs.mkdtempSync(path.join(tmp, 'derive-'));
  mkSite(dir);
  assert.deepEqual([...workerSharedFiles(dir, JSON.parse(fs.readFileSync(path.join(dir, 'asset-manifest.json'), 'utf8')))].sort(), [SHARED]);
});

test('C-N2: a first visit (every file once, the worker\'s import twice) is clean and sits exactly at the byte ceiling', async () => {
  const dir = fs.mkdtempSync(path.join(tmp, 'visit-'));
  const m = mkSite(dir);
  const s = await serve(dir);
  try {
    const get = (p) => new Promise((res, rej) => http.get(new URL(p, s.url), (r) => { r.resume(); r.on('end', res); }).on('error', rej));
    const w = watch(fakePage());
    for (const f of m.list) await get('/' + f.path);
    await get('/' + SHARED);  // the second fetch, by the worker
    const n = networkCheck(s, dir, w, 0, 0);
    assert.equal(n.ok, true, JSON.stringify(n));
    assert.equal(n.bytes, m.total + m.size(SHARED));
    assert.equal(n.ceiling, m.total + m.size(SHARED));
    assert.deepEqual(n.repeats, []);
    // positive control 1: a third fetch of a shared file fails (and only by the per-path rule)
    await get('/' + SHARED);
    const third = networkCheck(s, dir, w, 0, 0);
    assert.equal(third.ok, false);
    assert.deepEqual(third.repeats.map((r) => [r.path, r.count, r.max]), [[SHARED, 3, 2]]);
  } finally { s.close(); }
});

test('C-N2: positive controls: a second fetch of a non-shared file fails; the byte ceiling bites on its own', async () => {
  const dir = fs.mkdtempSync(path.join(tmp, 'ctl-'));
  const m = mkSite(dir);
  const s = await serve(dir);
  try {
    const get = (p) => new Promise((res, rej) => http.get(new URL(p, s.url), (r) => { r.resume(); r.on('end', res); }).on('error', rej));
    const w = watch(fakePage());
    for (const f of m.list) await get('/' + f.path);
    await get('/' + OTHER);  // a non-shared file twice
    const dup = networkCheck(s, dir, w, 0, 0);
    assert.equal(dup.ok, false);
    assert.deepEqual(dup.repeats.map((r) => [r.path, r.count, r.max]), [[OTHER, 2, 1]]);
    assert.ok(dup.bytes <= dup.ceiling, 'the byte ceiling alone would have let it through: the per-path rule is what fires');
    // the byte ceiling: nothing repeated, but asset-manifest.json (not a built file the page fetches) pushes the total over
    const r1 = s.responses.length;
    const w2 = watch(fakePage());
    for (const f of m.list) await get('/' + f.path);
    const base = networkCheck(s, dir, w2, r1, 0);
    assert.equal(base.ok, true, JSON.stringify(base));
    await get('/asset-manifest.json');
    const over = networkCheck(s, dir, w2, r1, 0);
    assert.equal(over.ok, false);
    assert.deepEqual(over.repeats, []);
    assert.ok(over.bytes > over.ceiling);
  } finally { s.close(); }
});

// importSpecifiers() against V8 (a child with --experimental-vm-modules): the static imports of one source text.
const v8Specifiers = (src) => {
  const r = spawnSync(process.execPath, ['--experimental-vm-modules', '--no-warnings', '-e',
    'const vm = require("node:vm"); const m = new vm.SourceTextModule(require("node:fs").readFileSync(0, "utf8"));' +
    'console.log(JSON.stringify((m.moduleRequests ? m.moduleRequests.map((x) => x.specifier) : m.dependencySpecifiers)));'], { input: src, encoding: 'utf8' });
  assert.equal(r.status, 0, r.stderr);
  return JSON.parse(r.stdout);
};
const FORMS = [
  ["import a from './a.js';", ['./a.js']],
  ["import {b, c as d} from \"./b.js\";", ['./b.js']],
  ["import * as ns from './c.js'", ['./c.js']],
  ["import './side.js';", ['./side.js']],
  ["import x, { y } from './d.js';", ['./d.js']],
  ["import {\n  e,\n  f,\n} from './e.js';", ['./e.js']],
  ["export * from './f.js';", ['./f.js']],
  ["export { g } from './g.js';", ['./g.js']],
  ["export * as h from './h.js';", ['./h.js']],
  ["// import z from './comment.js';\n/* import './block.js'; */\nconst s = \"import q from './string.js'\"; const t = `import './tmpl.js'`;", []],
  ["export const from = 'nothing'; export function f() { return import.meta.url; }", []],
  ["import a from './a.js'; import b from './a.js'; import './b.js';", ['./a.js', './b.js']],
];
test('C-N2: importSpecifiers: every static import form, comments and strings ignored; equal to V8\'s own list', () => {
  for (const [src, want] of FORMS) {
    assert.deepEqual([...new Set(importSpecifiers(src))].sort(), [...new Set(want)].sort(), src);
    assert.deepEqual([...new Set(v8Specifiers(src))].sort(), [...new Set(want)].sort(), 'V8 disagrees with the table for: ' + src);
  }
});
test('C-N2: importSpecifiers: a literal dynamic import counts; a computed one (or importScripts) cannot be derived and throws', () => {
  assert.deepEqual(importSpecifiers("const m = await import('./dyn.js');"), ['./dyn.js']);
  assert.throws(() => importSpecifiers('const m = await import(name);'), /cannot derive/);
  assert.throws(() => importSpecifiers("importScripts('./x.js');"), /cannot derive/);
  assert.throws(() => importSpecifiers('const t = `${await import("./t.js")}`;'), /cannot derive/);
});
test('C-N2: the real opfs_worker.js: its derived imports equal V8\'s list for it', () => {
  const src = fs.readFileSync(path.join(here, '../app/opfs_worker.js'), 'utf8');
  const got = importSpecifiers(src), v8 = v8Specifiers(src);
  assert.ok(v8.length > 0, 'the worker imports something (checkRelPath): an empty list would make the shared list vacuous');
  assert.deepEqual([...new Set(got)].sort(), [...new Set(v8)].sort());
});
