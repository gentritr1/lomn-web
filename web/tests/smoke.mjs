// L3 browser smoke test for the M0 build. Usage: node tests/smoke.mjs <site_dir> <profile_dir>
// Real GPU: Chromium channel "chromium" (new headless) + --enable-unsafe-webgpu. The headless shell falls back to
// SwiftShader whose canvas screenshots come out blank, so pixels (not just state) are always checked.
// Exit 0 pass, 1 fail. Prints adapter, frame count, dt reconciliation, requests.
import { chromium } from 'playwright';
import fs from 'node:fs';
import path from 'node:path';
import { serve } from './serve.mjs';

const [siteDir, profileDir] = process.argv.slice(2);
const site = path.resolve(siteDir);
const allowed = new Set(fs.readdirSync(site).map((f) => '/' + f).concat(['/', '/favicon.ico']));
const srv = await serve(site);
const fail = (msg) => { console.error('FAIL:', msg); process.exitCode = 1; };
const ctx = await chromium.launchPersistentContext(path.resolve(profileDir), {
  channel: 'chromium', args: ['--enable-unsafe-webgpu'], timeout: 30000 });
try {
  const page = ctx.pages()[0] ?? (await ctx.newPage());
  const consoleMsgs = []; const pageErrors = [];
  page.on('console', (msg) => {
    if (!['error', 'warning'].includes(msg.type())) return;
    if (msg.location().url.endsWith('/favicon.ico')) return; // the test server answers 404 for the favicon
    consoleMsgs.push(`${msg.type()}: ${msg.text()}`);
  });
  page.on('pageerror', (err) => pageErrors.push(err.message));
  await page.goto(srv.url + 'index.html', { timeout: 30000 });
  await page.waitForFunction(() => ['running', 'error'].includes(window.__lomn?.state), null, { timeout: 20000 });
  const st = await page.evaluate(() => window.__lomn);
  if (st.state !== 'running') throw new Error('engine state ' + st.state + ': ' + st.error);
  const adapter = await page.evaluate(async () => { const a = await navigator.gpu.requestAdapter();
    return `${a.info.vendor}/${a.info.architecture}`; });
  await page.waitForFunction(() => window.__lomn.frames >= 120, null, { timeout: 20000 });
  const m = await page.evaluate(() => ({ frames: window.__lomn.frames, dts: window.__lomn.metrics.dts }));
  const windowMs = m.dts.reduce((a, b) => a + b, 0);
  const p50 = [...m.dts].sort((a, b) => a - b)[Math.floor(m.dts.length / 2)];
  console.log(`adapter=${adapter} frames=${m.frames} dt_samples=${m.dts.length} window_ms=${windowMs.toFixed(0)} ` +
              `p50_dt_ms=${p50.toFixed(2)} expected_at_60hz=${(windowMs / 16.667).toFixed(0)}`);
  if (m.dts.length !== m.frames - 1) fail(`dt samples ${m.dts.length} != frames-1 ${m.frames - 1}`);
  const png = await page.locator('#canvas').screenshot();
  fs.writeFileSync(path.join(path.resolve(profileDir), '..', 'smoke.png'), png);
  const px = await page.evaluate(async (b64) => {
    const img = new Image(); img.src = 'data:image/png;base64,' + b64; await img.decode();
    const c = new OffscreenCanvas(img.width, img.height); const g = c.getContext('2d'); g.drawImage(img, 0, 0);
    const at = (x, y) => [...g.getImageData(x, y, 1, 1).data];
    return { centre: at(img.width >> 1, img.height >> 1), corner: at(2, 2) };
  }, png.toString('base64'));
  console.log('pixels', JSON.stringify(px));
  const near = (a, b) => a.slice(0, 3).every((v, i) => Math.abs(v - b[i]) <= 2);
  if (!near(px.centre, [255, 128, 0])) fail('centre pixel not the orange triangle');
  if (!near(px.corner, [0, 0, 64])) fail('corner pixel not the clear colour');
  console.log('console_errors', JSON.stringify(consoleMsgs), 'page_errors', JSON.stringify(pageErrors));
  const webgpuMsgs = consoleMsgs.filter((t) => t.includes('[webgpu]'));
  if (webgpuMsgs.length) fail('WebGPU console errors: ' + webgpuMsgs.join(' | '));
  if (pageErrors.length) fail('page exceptions: ' + pageErrors.join(' | '));
  const foreign = srv.requests.filter((r) => !allowed.has(r));
  console.log('requests', JSON.stringify(srv.requests));
  if (foreign.length) fail('requests outside the built site: ' + foreign.join(', '));
} catch (e) { fail(e.message); }
finally { await ctx.close(); srv.close(); }
