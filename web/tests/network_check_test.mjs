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
import { networkCheck, watch } from './lib.mjs';
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
