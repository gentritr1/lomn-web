// M1b data-free browser test (runs in `make check`): the whole BYO page + viewer on a SYNTHETIC install that
// `lomn_view --write-install` writes (the A7/A9 scenes as blockfiles + Base.osi; no game data), with a stand-in exe.
// Usage: node tests/byo_synthetic.mjs <wasm_build_dir> <work_dir> <native lomn_view>
//
// Which path each step exercises (and what stays UNVERIFIED) is listed in web/tests/README.md. In short:
//  - rejections: unknown exe, a "rejected" (REBUILT-style) exe, accepted exe + foreign data, quota-short (estimate()
//    stubbed); all through the real <input webkitdirectory> (setInputFiles with a directory path);
//  - OPFS fallback: input -> validate -> quota -> worker copy -> engine; revisit skips the copy (persistent profile);
//  - handle path: an OPFS directory handle handed to __lomn.useDirectoryHandle (= what showDirectoryPicker returns),
//    persisted in IndexedDB, revisit without a picker; the permission re-request with queryPermission stubbed 'prompt';
//  - pixels vs the native renders of the same install; area switching with flat object counts; discovery == the native
//    `lomn_view --manifest` access log; HUD metrics with reconciliation; resize; free-fly input; network discipline.
import { execFileSync } from 'node:child_process';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { blkTablesFingerprint, sha256hex } from '../app/fingerprint.js';
import { serve } from './serve.mjs';
import { decodePng, diffRgb, launch, networkCheck, waitFrames, waitState, watch } from './lib.mjs';
import { dropSamplesVerdict } from './a13_checks.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const [wasmDir, workArg, lomnView] = process.argv.slice(2);
if (!wasmDir || !workArg || !lomnView) {
  console.error('usage: byo_synthetic.mjs <wasm_build_dir> <work_dir> <lomn_view>');
  process.exit(2);
}
const work = path.resolve(workArg);
const results = [];
const check = (ok, what, detail = '') => {
  results.push({ ok: !!ok, what, detail });
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}${detail ? ' :: ' + detail : ''}`);
};
// REF4 r2 sections run independently: a failure (or a missing feature on pre-fix code) is recorded and the run goes on.
const section = async (name, fn) => {
  try {
    await fn();
  } catch (e) {
    check(false, `${name} (threw)`, String(e.message || e).split('\n')[0]);
  }
};
// Page-side helpers (run in the page through evaluate).
const opfsHas = async (p, dir, file) => p.evaluate(async ([d, f]) => {
  try {
    const h = await (await navigator.storage.getDirectory()).getDirectoryHandle(d);
    if (f) await h.getFileHandle(f);
    return true;
  } catch { return false; }
}, [dir, file]);
// A flat listing of the origin-private file system ("dir/", "dir/file:size:mtime"): two listings are equal only if nothing was written.
const opfsList = async (p) => p.evaluate(async () => {
  const out = [];
  const walk = async (d, prefix) => {
    for await (const [n, h] of d.entries()) {
      if (h.kind === 'directory') { out.push(prefix + n + '/'); await walk(h, prefix + n + '/'); }
      else { const f = await h.getFile(); out.push(`${prefix}${n}:${f.size}:${f.lastModified}`); }
    }
  };
  await walk(await navigator.storage.getDirectory(), '');
  return out.sort();
});
const idbDeleteHandle = (p) => p.evaluate(() => new Promise((res) => {
  const r = indexedDB.open('lomn-web', 1);
  r.onupgradeneeded = () => r.result.createObjectStore('kv');
  r.onsuccess = () => { const t = r.result.transaction('kv', 'readwrite'); t.objectStore('kv').delete('install-handle'); t.oncomplete = () => { r.result.close(); res(true); }; };
  r.onerror = () => res(false);
}));
// I1 probe: counts GPUTextures/GPUBuffers created minus explicitly destroyed (a GC'd wrapper never counts as freed).
const gpuLiveHook = () => {
  const live = { textures: 0, buffers: 0, created: 0, destroyed: 0 };
  window.__gpuLive = live;
  const D = GPUDevice.prototype, ct = D.createTexture, cb = D.createBuffer;
  D.createTexture = function (d) { live.textures++; live.created++; return ct.call(this, d); };
  D.createBuffer = function (d) { live.buffers++; return cb.call(this, d); };
  const td = GPUTexture.prototype.destroy, bd = GPUBuffer.prototype.destroy;
  GPUTexture.prototype.destroy = function () { if (!this.__lomnGone) { this.__lomnGone = 1; live.textures--; live.destroyed++; } return td.call(this); };
  GPUBuffer.prototype.destroy = function () { if (!this.__lomnGone) { this.__lomnGone = 1; live.buffers--; } return bd.call(this); };
};

// final I-1 (a, b, c): after the device is lost nothing that starts work works. The picker is disabled; a programmatic change event and a
// direct showArea() are refused; the engine's own lomn_area_show refuses with a device-lost error; the page stays 'error', Reload visible.
const afterLoss = async (p, label, st0, f0) => {
  const r = await p.page.evaluate(async () => {
    const L = window.__lomn, M = L.engine.M;
    const pick = document.getElementById('area-picker');
    const o = { pickerDisabled: pick.disabled, resetDisabled: document.getElementById('reset-camera').disabled };
    o.engineShow = M._lomn_area_show();              // (c) the engine refuses (pre-fix: "no completed probe", which is not a loss error)
    o.engineError = L.engine.lastError();
    o.direct = await L.showArea('levt/stat');       // (b) the page refuses
    pick.value = 'levt/stat';
    pick.dispatchEvent(new Event('change'));         // (a) the picker's own handler, bypassing the disabled attribute
    return o;
  });
  const flipped = await p.page.waitForFunction(() => window.__lomn.state !== 'error', null, { timeout: 3000 }).then(() => true, () => false);
  const o = await p.page.evaluate(() => ({ state: window.__lomn.state, message: window.__lomn.message, frames: window.__lomn.metrics?.frames ?? -1, area: window.__lomn.load?.area,
                                           reload: !document.getElementById('reload').hidden }));
  check(r.pickerDisabled && r.resetDisabled, `final I-1 (b): ${label}: after the loss the area picker and the camera reset are disabled`, JSON.stringify(r));
  check(r.engineShow === 0 && /device lost/i.test(r.engineError), `final I-1 (c): ${label}: lomn_area_show refuses on a lost device with a device-lost error`, `${r.engineShow}: ${r.engineError}`);
  check(r.direct === false && !flipped && o.state === 'error' && o.reload && o.frames === f0 && o.area === 'levt/test',
        `final I-1 (a): ${label}: a switch attempt after the loss is refused: showArea() false, state stays error, Reload visible, no frames`,
        `direct ${r.direct}; left error ${flipped}; ${JSON.stringify(o)}; frames at loss ${f0}`);
};

// ---- fixtures ----
fs.rmSync(work, { recursive: true, force: true });
fs.mkdirSync(work, { recursive: true });
const inst = path.join(work, 'inst');
execFileSync(lomnView, ['--synthetic', '2', '--backend', 'null', '--out', path.join(work, 'syn.txt'), '--write-install', inst], { timeout: 60000 });
fs.writeFileSync(path.join(inst, 'LEGOBionicle.exe'), 'LOMN-WEB SYNTHETIC TEST EXE (not a game file)\n');
const variant = (name, exe, mutate) => {
  const dir = path.join(work, name);
  fs.cpSync(inst, dir, { recursive: true });
  if (exe) fs.writeFileSync(path.join(dir, 'LEGOBionicle.exe'), exe);
  if (mutate) mutate(dir);
  return dir;
};
const unknownDir = variant('unknown', 'LOMN-WEB SYNTHETIC UNKNOWN EXE\n');
const rebuiltDir = variant('rebuilt', 'LOMN-WEB SYNTHETIC REBUILT EXE\n');
const foreignDataDir = variant('foreign-data', null, (d) => {
  const p = path.join(d, 'data/levels/levt/test.blk');
  const b = fs.readFileSync(p);
  b[12] ^= 0x01;  // first byte of the first entry name: the table changes, the file still parses
  fs.writeFileSync(p, b);
});
// final I-2: a legal macOS/Linux name with a TAB (and one with LF) beside the real blockfiles: the engine<->JS lists are tab/newline separated.
const tabDir = variant('tab-name', null, (d) => {
  fs.copyFileSync(path.join(d, 'data/levels/levt/test.blk'), path.join(d, 'data/levels/levt/a\tb.blk'));
  fs.copyFileSync(path.join(d, 'data/levels/levt/test.blk'), path.join(d, 'data/levels/levt/c\nd.blk'));
});
// Non-game folders (M8 / C2: these still hard-reject): an empty folder; an exe-only folder with no data/**/*.blk.
const emptyDir = path.join(work, 'empty');
fs.mkdirSync(emptyDir, { recursive: true });
const nonGameDir = path.join(work, 'non-game');
fs.mkdirSync(path.join(nonGameDir, 'data'), { recursive: true });
fs.writeFileSync(path.join(nonGameDir, 'LEGOBionicle.exe'), 'LOMN-WEB SYNTHETIC TEST EXE (not a game file)\n');
fs.writeFileSync(path.join(nonGameDir, 'data', 'readme.txt'), 'not a game\n');
const nodeEntries = (root) => {
  const out = [];
  const walk = (dir, prefix) => {
    for (const f of fs.readdirSync(dir)) {
      const p = path.join(dir, f);
      if (fs.statSync(p).isDirectory()) walk(p, prefix + f + '/');
      else out.push({ path: prefix + f, size: fs.statSync(p).size, abs: p });
    }
  };
  walk(root, '');
  return out;
};
const readRange = async (e, o, l) => new Uint8Array(fs.readFileSync(e.abs).subarray(o, o + l));
const fp = await blkTablesFingerprint(nodeEntries(inst), readRange);
const exeSha = (dir) => crypto.createHash('sha256').update(fs.readFileSync(path.join(dir, 'LEGOBionicle.exe'))).digest('hex');
fs.writeFileSync(path.join(work, 'extra-builds.json'), JSON.stringify({
  accepted: [{ id: 'synthetic-test', exe_sha256: exeSha(inst) }],
  data: [{ id: 'synthetic-data', blk_tables_sha256: fp.fingerprint }],
  rejected: [{ id: 'synthetic-rebuilt', exe_sha256: exeSha(rebuiltDir), name: 'REBUILT (test fixture)',
               reason: 'REBUILT needs the LOMNHook mod natives (e.g. BetterSaver), which a browser cannot load' }],
}));
const site = path.join(work, 'site');
execFileSync('node', [path.join(here, '../tools/build_site.mjs'), wasmDir, site, '--extra-builds', path.join(work, 'extra-builds.json')], { timeout: 60000 });
const loadavg = () => os.loadavg().map((x) => x.toFixed(2)).join(' ');
const loadavgStart = loadavg();
// Hashed assets (deploy prep): every JS/wasm name carries its content hash; index.html (and the JSON) do not.
const assets = JSON.parse(fs.readFileSync(path.join(site, 'asset-manifest.json'), 'utf8'));
const unhashed = assets.files.filter((f) => /\.(js|wasm|css)$/.test(f.path) && !/\.[0-9a-f]{10}\.(js|wasm|css)$/.test(f.path)).map((f) => f.path);
check(unhashed.length === 0 && assets.files.some((f) => f.path === 'index.html') && assets.files.some((f) => /^lomn_web\.[0-9a-f]{10}\.wasm$/.test(f.path)),
      'hashed assets: JS and wasm published under content-hashed names, index.html unhashed', unhashed.join(', ') || assets.files.map((f) => f.path).join(' '));
// The build under test: the wasm this test built (build dir) must be the one the page instantiates.
const builtWasmSha = crypto.createHash('sha256').update(fs.readFileSync(path.join(wasmDir, 'lomn_web.wasm'))).digest('hex');
// Native references of the same install (default camera, 1280x720, steady state after 3 frames) and the access log.
const refs = {};
for (const area of ['test', 'stat']) {
  const out = path.join(work, `native_${area}.png`);
  execFileSync(lomnView, ['--install', inst, '--level', 'levt', '--area', area, '--frames', '3', '--out', out], { timeout: 60000 });
  refs[area] = decodePng(fs.readFileSync(out));
}
execFileSync(lomnView, ['--install', inst, '--manifest', '1', '--out', path.join(work, 'manifest.json')], { timeout: 60000 });
const manifest = JSON.parse(fs.readFileSync(path.join(work, 'manifest.json'), 'utf8'));
const nativeRanges = (name) => {
  const a = manifest.areas.find((x) => x.area === name);
  return a.ranges.map(([f, o, l]) => `${manifest.files[f].path}\t${o}\t${l}`).sort().join('\n');
};

// ---- browser ----
const srv = await serve(site);
const ctx = await launch(path.join(work, 'profile'));
const base = srv.url + 'index.html';
const shots = path.join(work, 'shots');
fs.mkdirSync(shots, { recursive: true });
const canvasPng = async (page, name) => {
  // A clip on whole CSS pixels (deviceScaleFactor 1): the drawing buffer exactly. A fractional box would resample.
  const box = await page.locator('#canvas').boundingBox();
  if (![box.x, box.y, box.width, box.height].every(Number.isInteger)) throw new Error('canvas not on whole pixels: ' + JSON.stringify(box));
  const png = await page.screenshot({ clip: box });
  fs.writeFileSync(path.join(shots, name), png);
  return decodePng(png);
};
const pageWatchers = [];
// Review fix 2: every unspun page that showed an area is checked, just before it closes, against the display rate measured
// on that same page: the engine's DROPPING flag must be false and must agree with fps < 0.95 x display_hz. Pages that resize,
// crash or spin on purpose opt out ({drop: false}; the spin pages are recognised by their URL / init script).
const dropSamples = [];
const open = async (url, init, { drop = true } = {}) => {
  const page = await ctx.newPage();
  if (init) await page.addInitScript(typeof init === 'string' ? { content: init } : init);
  const w = watch(page);
  pageWatchers.push(w);
  const r0 = srv.responses.length;
  const spun = /debug_spin/.test(url) || (typeof init === 'string' && /spin/.test(init));
  if (drop && !spun) {
    const close = page.close.bind(page);
    page.close = async (...args) => {
      try {
        await waitFrames(page, 40, 15000);  // >= 30 dt samples in the window
        const read = () => page.evaluate(() => (window.__lomn?.state === 'area-ready' ? window.__lomn.metrics?.fps ?? null : null));
        const pick = (m) => ({ dropping: m.dropping, fps: m.fps, hz: m.display_hz, snapped: m.display_snapped, loadavg: loadavg() });
        const m = await read();
        if (m && m.display_hz > 0 && m.dt_samples >= 30) {
          const sample = { url: url.replace(srv.url, ''), ...pick(m) };
          // T8 P16: a page that reads DROPPING (host load) is re-measured ONCE on a fresh window (its area shown again) and
          // fails only if it drops again; both readings and the load average are reported. Spun pages never get here.
          if (m.dropping || m.fps < 0.95 * m.display_hz) {
            try {
              await page.evaluate(() => window.__lomn.showArea(window.__lomn.load.area));
              await waitFrames(page, 60, 20000);
              const m2 = await read();
              if (m2 && m2.display_hz > 0 && m2.dt_samples >= 30) sample.remeasure = pick(m2);
            } catch { /* no second reading: the first one stands and fails the check */ }
          }
          dropSamples.push(sample);
        }
      } catch { /* page already gone or not showing an area: nothing to sample */ }
      return close(...args);
    };
  }
  await page.goto(url, { timeout: 30000 });
  return { page, w, r0 };
};
// Waits for a condition on the node side (a deadline, never a fixed sleep).
const until = async (cond, ms = 10000) => {
  const t0 = Date.now();
  while (!cond() && Date.now() - t0 < ms) await new Promise((r) => setTimeout(r, 20));
  return cond();
};
const netOk = (p, label) => {
  const n = networkCheck(srv, site, p.w, p.r0, 0);
  check(n.ok, `network discipline: ${label}`, `${n.responses} responses, ${n.bytes} of ${n.buildBytes} build bytes` +
        (n.foreignPaths.length ? ` foreign paths ${n.foreignPaths}` : '') + (n.foreignUrls.length ? ` foreign urls ${n.foreignUrls}` : ''));
};
// Pixel gate: tolerance = max per-channel delta accepted. Measured first (null = the same build twice on fresh pages:
// 0; native vs browser: 0), so 0; a positive control (fog off) must exceed it. LOMN_PIXEL_TOLERANCE overrides it.
const tolerance = Number(process.env.LOMN_PIXEL_TOLERANCE ?? 0);
const gate = { tolerance, native_vs_browser: [] };
const comparePixels = (img, ref, label) => {
  const d = diffRgb(img, ref);
  const ok = d.sameSize && d.over(tolerance) === 0;
  // M7: tolerance 0 is a same-machine (same GPU) gate; a failure names the adapter so a cross-GPU run is recognisable.
  check(ok, `pixels ${label} vs native`, (d.sameSize ?
        `${d.differing} of ${d.pixels} differ, max delta ${d.max}, over tolerance ${tolerance}: ${d.over(tolerance)}` : 'size differs') +
        (ok ? '' : `; adapter ${summary.measured_config?.adapter ?? 'unknown'} (set LOMN_PIXEL_TOLERANCE after measuring null + native-vs-browser on this GPU)`));
  if (d.sameSize) gate.native_vs_browser.push({ label, differing: d.differing, max: d.max });
  return d;
};
const shotsKept = {};
const summary = { refs: Object.keys(refs) };
try {
  // A. Rejections through the real directory input.
  {
    const p = await open(base);
    let st = await waitState(p.page, ['need-files']);
    check(st.state === 'need-files', 'first visit asks for the folder', st.message);
    const cases = [
      [unknownDir, /Unknown build: LEGOBionicle\.exe SHA-256 [0-9a-f]{12}/, 'unknown exe -> named rejection'],
      [rebuiltDir, /This is REBUILT \(test fixture\).*not supported: REBUILT needs/, 'rejected-list (REBUILT-style) exe -> named rejection'],
      [nonGameDir, /not a LOMN install/, 'non-game folder (exe, no .blk) -> hard rejection (C2)'],
      [emptyDir, /LEGOBionicle\.exe was not found/, 'empty folder -> rejection (M8, the empty-fixture negative test)'],
    ];
    for (const [dir, re, what] of cases) {
      await section(what, async () => {
        await p.page.evaluate(() => { window.__lomn.state = 'test-reset'; });
        await p.page.setInputFiles('#byo-input', dir);
        st = await waitState(p.page, ['rejected'], 20000);
        check(st.state === 'rejected' && re.test(st.message), what, st.message);
      });
    }
    // I4: the page carries a Content-Security-Policy (the browser then blocks any foreign request itself).
    await section('CSP meta', async () => {
      const csp = await p.page.evaluate(() => document.querySelector('meta[http-equiv="Content-Security-Policy"]')?.content ?? '');
      summary.csp = csp;
      check(/default-src 'self'/.test(csp) && /connect-src 'self'/.test(csp) && /object-src 'none'/.test(csp), 'CSP meta present (default-src/connect-src self, object-src none)', csp || 'none');
    });
    const copies = await p.page.evaluate(async () => { try { await (await navigator.storage.getDirectory()).getDirectoryHandle('lomn-install'); return true; } catch { return false; } });
    check(!copies, 'rejected folders copied nothing into OPFS');
    netOk(p, 'rejections');
    await p.page.close();
  }
  // B. Quota-short: estimate() stubbed to 10 bytes free.
  {
    const p = await open(base, () => {
      navigator.storage.estimate = async () => ({ quota: 1000, usage: 990 });
    });
    await waitState(p.page, ['need-files']);
    await p.page.setInputFiles('#byo-input', inst);
    const st = await waitState(p.page, ['quota-short', 'copying']);
    check(st.state === 'quota-short' && /Not enough browser storage.*needed, 0\.0 MiB available/.test(st.message), 'quota short -> message, no copy', st.message);
    const copied = await p.page.evaluate(async () => { try { await (await navigator.storage.getDirectory()).getDirectoryHandle('lomn-install'); return true; } catch { return false; } });
    check(!copied, 'quota-short: nothing written to OPFS');
    await p.page.close();
  }
  // C. OPFS fallback path: input -> validate -> quota -> copy -> engine -> area.
  let opfsMetrics;
  {
    const p = await open(base + '?size=1280x720&area=levt/test');
    await waitState(p.page, ['need-files']);
    await p.page.setInputFiles('#byo-input', inst);
    const st = await waitState(p.page, ['area-ready'], 120000);
    check(st.state === 'area-ready', 'OPFS path: copied, validated and showing levt/test', st.message || st.error);
    const info = await p.page.evaluate(() => ({ source: window.__lomn.source, copy: window.__lomn.copy, quota: window.__lomn.quota, load: window.__lomn.load, boot: window.__lomn.boot }));
    check(info.source?.kind === 'opfs' && info.source.how === 'copied' && info.copy?.bytes > 0, 'OPFS path: source is the copy', JSON.stringify(info.source));
    summary.copy = info.copy;
    summary.boot_first = info.boot;
    await waitFrames(p.page, 3);
    shotsKept.opfs_test = await canvasPng(p.page, 'opfs_test.png');
    comparePixels(shotsKept.opfs_test, refs.test, 'levt/test (OPFS path)');
    const mb = await p.page.evaluate(() => window.__lomn.metrics);
    summary.measured_config = { build: mb.build, canvas: mb.canvas, adapter: mb.adapter, loadavg: loadavg() };
    check(mb.build.wasm_sha256 === builtWasmSha && mb.build.type === 'Release' && /-O\d/.test(mb.build.cxx_flags),
          'measured configuration: the page runs the wasm this test built (Release)', `${mb.build.type} [${mb.build.cxx_flags}] ${mb.build.wasm_sha256.slice(0, 12)} vs built ${builtWasmSha.slice(0, 12)}`);
    const touched = info.load.touched.trim().split('\n').sort().join('\n');
    check(touched === nativeRanges('levt/test'), 'discovery: the browser fetched exactly the native access log of levt/test',
          `${info.load.rounds} rounds, ${info.load.ranges} ranges, ${info.load.bytes} bytes`);
    summary.first_load = info.load;
    // Area switching: alternate test/stat 6 times; counts flat per area; stat pixels vs native.
    const per = {};
    for (let i = 0; i < 6; ++i) {
      const name = i % 2 ? 'levt/test' : 'levt/stat';
      const ok = await p.page.evaluate((n) => window.__lomn.showArea(n), name);
      await waitFrames(p.page, 3);
      const m = await p.page.evaluate(() => window.__lomn.metrics);
      const key = JSON.stringify(m.live_objects) + ` fs=${m.heap.fs_resident_bytes}`;
      if (!per[name]) per[name] = key;
      check(ok && per[name] === key && m.release_failures === 0 && m.rejected_last_frame === 0, `switch ${i + 1} to ${name}: live objects + resident bytes flat`, key);
      // Release on switch: what stays resident is exactly this area's read set (it includes the pinned boot reads).
      const own = (await p.page.evaluate(() => window.__lomn.load.touched)).trim().split('\n').reduce((n, l) => n + Number(l.split('\t')[2]), 0);
      check(m.heap.fs_resident_bytes === own, `switch ${i + 1}: resident file bytes == ${name}'s own read set (previous area released)`, `${m.heap.fs_resident_bytes} vs ${own}`);
      if (i === 0) {
        comparePixels(await canvasPng(p.page, 'opfs_stat.png'), refs.stat, 'levt/stat (OPFS path, after a switch)');
        const t2 = (await p.page.evaluate(() => window.__lomn.load.touched)).trim().split('\n').sort().join('\n');
        check(t2 === nativeRanges('levt/stat'), 'discovery: levt/stat fetched exactly its native access log');
      }
    }
    opfsMetrics = await p.page.evaluate(() => window.__lomn.metrics);
    summary.after_switches = { live_objects: opfsMetrics.live_objects, heap: opfsMetrics.heap, areas_shown: opfsMetrics.areas_shown };
    netOk(p, 'OPFS first visit');
    await p.page.close();
  }
  // D. OPFS revisit: no input, no copy, cached prefetch set (1 round).
  {
    const p = await open(base + '?size=1280x720&area=levt/test');
    const st = await waitState(p.page, ['area-ready'], 60000);
    const info = await p.page.evaluate(() => ({ source: window.__lomn.source, copy: window.__lomn.copy, load: window.__lomn.load }));
    check(st.state === 'area-ready' && info.source?.kind === 'opfs' && info.source.how === 'revisit' && !info.copy,
          'OPFS revisit: no picker, no copy', JSON.stringify(info.source));
    check(info.load.cached && info.load.rounds === 1, 'revisit: the cached prefetch set loads the area in 1 round', `rounds ${info.load.rounds}`);
    await waitFrames(p.page, 3);
    shotsKept.revisit_test = await canvasPng(p.page, 'revisit_test.png');
    comparePixels(shotsKept.revisit_test, refs.test, 'levt/test (OPFS revisit)');
    // Null control: the same build and area on two fresh pages (C and D).
    const n = diffRgb(shotsKept.opfs_test, shotsKept.revisit_test);
    gate.null = { differing: n.differing, max: n.max };
    check(n.sameSize && n.max <= tolerance, 'pixel gate null: levt/test on two fresh pages', `${n.differing} differ, max delta ${n.max}`);
    netOk(p, 'OPFS revisit');
    await p.page.close();
  }
  // I6: a copy whose completion manifest no longer matches the files (Base.osi deleted) is treated as incomplete:
  // cleared and the folder is asked for again (pre-fix: it loads with default fog). Then the copy is made again.
  await section('I6 incomplete copy', async () => {
    const p = await open(base + '?pick=1');
    await waitState(p.page, ['need-files']);
    await p.page.evaluate(async () => {
      const d = await (await (await navigator.storage.getDirectory()).getDirectoryHandle('lomn-install')).getDirectoryHandle('data');
      await d.removeEntry('Base.osi');
    });
    await p.page.close();
    const q = await open(base + '?size=1280x720&area=levt/test');
    const st = await waitState(q.page, ['need-files', 'area-ready'], 60000);
    const gone = !(await opfsHas(q.page, 'lomn-install'));
    check(st.state === 'need-files' && /incomplete/.test(st.message) && gone, 'I6: a copy missing a file is cleared and the folder asked for again',
          `${st.state}: ${st.message}; copy removed ${gone}`);
    await q.page.close();
    const r = await open(base + '?size=1280x720&area=levt/test&pick=1');
    await waitState(r.page, ['need-files']);
    await r.page.setInputFiles('#byo-input', inst);
    const st2 = await waitState(r.page, ['area-ready'], 120000);
    check(st2.state === 'area-ready', 'I6: copied again for the following steps');
    await r.page.close();
  });
  // E. Handle path (what showDirectoryPicker returns): an OPFS directory handle, persisted in IndexedDB, read in place.
  {
    const p = await open(base + '?size=1280x720&area=levt/stat&pick=1');
    await waitState(p.page, ['need-files']);
    const ok = await p.page.evaluate(async () => {
      const d = await (await navigator.storage.getDirectory()).getDirectoryHandle('lomn-install');
      return window.__lomn.useDirectoryHandle(d);
    });
    const st = await waitState(p.page, ['area-ready'], 60000);
    const src = await p.page.evaluate(() => window.__lomn.source);
    check(ok && st.state === 'area-ready' && src.kind === 'handle' && src.how === 'picked', 'handle path: picked handle read in place (no copy)', JSON.stringify(src));
    await waitFrames(p.page, 3);
    comparePixels(await canvasPng(p.page, 'handle_stat.png'), refs.stat, 'levt/stat (handle path)');
    await p.page.close();
    const q = await open(base + '?size=1280x720&area=levt/stat');
    await waitState(q.page, ['area-ready'], 60000);
    const src2 = await q.page.evaluate(() => window.__lomn.source);
    check(src2.kind === 'handle' && src2.how === 'revisit', 'handle revisit: the IndexedDB handle is used without a picker', JSON.stringify(src2));
    netOk(q, 'handle revisit');
    await q.page.close();
  }
  // E2. Pixel gate positive control: fog off (debug switch) on levt/stat (script fog 5..50) must exceed the tolerance.
  {
    const p = await open(base + '?size=1280x720&area=levt/stat&debug=1&fog=off', () => {
      window.__csp = [];
      document.addEventListener('securitypolicyviolation', (e) => window.__csp.push(e.blockedURI));
    });
    await waitState(p.page, ['area-ready'], 60000);
    await waitFrames(p.page, 3);
    const d = diffRgb(await canvasPng(p.page, 'positive_fog_off_stat.png'), refs.stat);
    gate.positive = { what: 'levt/stat with fog off vs native (fog on)', differing: d.differing, max: d.max, over_tolerance: d.over(tolerance) };
    check(d.sameSize && d.over(tolerance) > 1000, 'pixel gate positive control: fog off exceeds the tolerance', `${d.over(tolerance)} pixels over tolerance ${tolerance}, max delta ${d.max}`);
    // Network-discipline positive control: one request outside the asset manifest and one to another origin are flagged.
    const r0 = srv.responses.length, u0 = p.w.urls.length;
    await p.page.evaluate(async () => {
      await fetch('/not-in-manifest.bin').catch(() => {});
      await fetch('http://127.0.0.1:9/elsewhere').catch(() => {});
    });
    await p.page.waitForFunction(() => true);
    const n = networkCheck(srv, site, p.w, r0, u0);
    const blocked = await p.page.evaluate(() => window.__csp);
    // A request the CSP blocks may never reach the network layer Playwright watches: the violation report flags it.
    const foreignFlagged = n.foreignUrls.some((u) => u.includes('127.0.0.1:9/')) || blocked.some((u) => u.includes('127.0.0.1:9'));
    check(!n.ok && n.foreignPaths.includes('/not-in-manifest.bin') && foreignFlagged,
          'network positive control: a non-manifest path and a foreign origin are flagged', `foreign paths ${n.foreignPaths}, urls ${n.foreignUrls}, CSP-blocked ${blocked}`);
    check(blocked.some((u) => u.includes('127.0.0.1:9')), 'I4: the CSP blocks the foreign-origin request itself', `blocked ${JSON.stringify(blocked)}`);
    // Review fix 3: an upload to a path that IS in the manifest (the reviewer's probe: POST bytes to builds.json) is flagged
    // by method and request-body size. Control: the same path fetched with GET is clean.
    await section('upload probes', async () => {
      const probe = async (fn, arg) => {
        const r1 = srv.responses.length, u1 = p.w.urls.length;
        const wait = arg === 'beacon' ? p.page.waitForRequest((q) => q.method() === 'POST', { timeout: 10000 }) : Promise.resolve();
        await p.page.evaluate(fn, arg);
        await wait;
        await until(() => srv.responses.length > r1);
        return networkCheck(srv, site, p.w, r1, u1);
      };
      const get = await probe(async () => { await (await fetch('/builds.json')).arrayBuffer(); });
      check(get.ok && (get.uploads ?? []).length === 0, 'upload probe control: GET of /builds.json is clean', `${get.responses} responses`);
      const post = await probe(async () => { await (await fetch('/builds.json', { method: 'POST', body: new Uint8Array(4096).fill(7) })).arrayBuffer(); });
      check(!post.ok && post.foreignPaths.length === 0 && post.foreignUrls.length === 0 && (post.uploads ?? []).some((u) => u.method === 'POST' && u.bytes === 4096),
            'upload positive control: POSTing 4096 bytes to /builds.json (a manifest path) fails the network check', JSON.stringify(post.uploads ?? 'no upload record'));
      // Review fix N2: data in a query string to a manifest path is an upload too (the page itself never puts a query on an asset
      // request; only its own navigation URL, ?hud=1&area=..., carries one, and every page of this test is that).
      const query = await probe(async () => { await (await fetch('/builds.json?d=' + 'A'.repeat(200))).arrayBuffer(); });
      check(!query.ok && query.foreignPaths.length === 0 && query.foreignUrls.length === 0 && (query.uploads ?? []).length === 0 && (query.queries ?? []).length >= 1,
            'query-string positive control: fetch("/builds.json?d=" + 200 bytes) fails the network check', JSON.stringify(query.queries ?? 'no query record').slice(0, 300));
      const img = await probe(async () => { await new Promise((res) => { const i = new Image(); i.onload = i.onerror = res; i.src = '/builds.json?img=' + 'B'.repeat(120); }); });
      check(!img.ok && (img.queries ?? []).length >= 1, 'query-string positive control: an <img> request with a query string to a manifest path fails the network check',
            JSON.stringify(img.queries ?? 'no query record').slice(0, 300));
      // T8 P10: a window.open popup with a query string is a top-level navigation by Sec-Fetch-*, which the check used to treat
      // as the page's own. Only navigations the TEST made (page.goto / reload, recorded by watch()) are the page's own now;
      // the popup is flagged as a `navigations` finding. Control: the test's own navigations are clean (every netOk above and
      // below, and tests/network_check_test.mjs).
      const popupPage = ctx.waitForEvent('page', { timeout: 15000 });  // the popup is its own Page: close it as soon as it exists
      const popup = await probe(() => { window.open('/index.html?d=' + 'A'.repeat(100)); });
      check(!popup.ok && popup.foreignPaths.length === 0 && (popup.navigations ?? []).some((x) => /^\/index\.html\?d=A{100}$/.test(x.url)),
            'navigation positive control: window.open("/index.html?d=" + 100 bytes) fails the network check', JSON.stringify(popup.navigations ?? 'no navigation record').slice(0, 300));
      await (await popupPage).close();
      const beacon = await probe((kind) => navigator.sendBeacon('/builds.json', new Blob([new Uint8Array(512)])), 'beacon');
      check(!beacon.ok && (beacon.uploads ?? []).some((u) => u.method === 'POST' && u.bytes === 512),
            'upload positive control: navigator.sendBeacon to a manifest path fails the network check', JSON.stringify(beacon.uploads ?? 'no upload record'));
    });
    // Leak-detector positive control: one deliberately leaked texture changes the per-area live-object key.
    const before = (await p.page.evaluate(() => window.__lomn.metrics)).live_objects;
    await p.page.evaluate(() => window.__lomn.engine.M._lomn_debug_leak_texture());
    await p.page.evaluate(() => window.__lomn.showArea('levt/stat'));
    await waitFrames(p.page, 3);
    const after = (await p.page.evaluate(() => window.__lomn.metrics)).live_objects;
    check(JSON.stringify(before) !== JSON.stringify(after) && after.textures === before.textures + 1,
          'leak positive control: one leaked texture is counted by the flat-count check', `${JSON.stringify(before)} -> ${JSON.stringify(after)}`);
    await p.page.close();
  }
  summary.pixel_gate = gate;
  // M13: the unfaithful debug switch needs ?debug=1 (a pasted ?fog=off URL renders faithfully).
  await section('M13 debug gate', async () => {
    const p = await open(base + '?size=1280x720&area=levt/stat&fog=off');
    await waitState(p.page, ['area-ready'], 60000);
    const m = await p.page.evaluate(() => window.__lomn.metrics);
    check(m.render.fog === true, 'M13: ?fog=off without ?debug=1 is ignored', `fog ${m.render.fog}`);
    const badge = await p.page.locator('#debug-badge').isVisible().catch(() => false);
    check(!badge, 'final m-1: no debug badge when no debug switch is active (?fog=off without ?debug=1)', `badge visible ${badge}`);
    await p.page.close();
  });
  // final m-1: an unfaithful render must not be silent: with ?debug=1&fog=off and NO ?hud=1 the page still says it is in a debug mode.
  await section('m-1 debug badge', async () => {
    const p = await open(base + '?size=1280x720&area=levt/stat&debug=1&fog=off', undefined, { drop: false });
    await waitState(p.page, ['area-ready'], 60000);
    const m = await p.page.evaluate(() => window.__lomn.metrics);
    const hud = await p.page.locator('#hud').isVisible();
    const badge = await p.page.locator('#debug-badge').isVisible().catch(() => false);
    const text = badge ? await p.page.locator('#debug-badge').innerText() : '';
    check(m.render.fog === false && !hud && badge && /debug/i.test(text) && /fog off/i.test(text),
          'final m-1: ?debug=1&fog=off without ?hud=1 shows a visible "debug" badge naming the switch', `fog ${m.render.fog}; hud visible ${hud}; badge ${badge} "${text}"`);
    await p.page.close();
    const q = await open(base + '?size=1280x720&area=levt/stat&debug=1&debug_spin=3', undefined, { drop: false });
    await waitState(q.page, ['area-ready'], 60000);
    const t2 = await q.page.locator('#debug-badge').isVisible().catch(() => false) ? await q.page.locator('#debug-badge').innerText() : '';
    check(/debug/i.test(t2) && /spin 3 ms/.test(t2), 'final m-1: the badge also names ?debug_spin', `"${t2}"`);
    await q.page.close();
  });
  // final I-3: a browser without WebGPU is told so BEFORE a gigabyte is copied into OPFS (pre-fix: it copies, then fails with a Reload button).
  await section('I-3 no WebGPU', async () => {
    const p = await open(base + '?pick=1', `
      Object.defineProperty(Navigator.prototype, 'gpu', { configurable: true, get() { return undefined; } });
      window.__workers = 0; const W = window.Worker; window.Worker = class extends W { constructor(...a) { super(...a); window.__workers++; } };`, { drop: false });
    const before = await opfsList(p.page);
    const st = await waitState(p.page, ['need-files'], 15000);
    check(st.state === 'error' && /^This browser has no WebGPU/.test(st.message) && /Chrome|Edge/.test(st.message),
          'final I-3: no navigator.gpu -> a named, final message before any file is chosen', `${st.state}: ${st.message}`);
    const noWork = await p.page.evaluate(() => window.__lomn.validation === undefined && !window.__lomn.engine);
    check(noWork, 'final I-3: no validation or engine work ran');
    await p.page.setInputFiles('#byo-input', inst);
    const moved = await p.page.waitForFunction(() => window.__lomn.state !== 'error' && window.__lomn.state !== 'need-files', null, { timeout: 4000 }).then(() => true, () => false);
    const after = await opfsList(p.page);
    const workers = await p.page.evaluate(() => window.__workers);
    const final = await p.page.evaluate(() => ({ state: window.__lomn.state, copy: window.__lomn.copy ?? null, byoHidden: document.getElementById('byo').hidden }));
    check(!moved && final.state === 'error' && !final.copy && workers === 0 && JSON.stringify(before) === JSON.stringify(after),
          'final I-3: choosing a folder anyway copies nothing (no copy worker, OPFS listing unchanged) and the page stays in the final error',
          `state ${final.state}; left error ${moved}; workers ${workers}; OPFS entries ${before.length} -> ${after.length}; copy ${JSON.stringify(final.copy)}`);
    check(final.byoHidden, 'final I-3: the folder choice is hidden', `#byo hidden ${final.byoHidden}`);
    await p.page.close();
  });
  // F. Permission re-request (queryPermission STUBBED to 'prompt'; the real browser prompt stays UNVERIFIED).
  {
    const p = await open(base + '?size=1280x720&area=levt/test', () => {
      FileSystemHandle.prototype.queryPermission = async () => 'prompt';
      FileSystemHandle.prototype.requestPermission = async () => 'granted';
    });
    let st = await waitState(p.page, ['need-permission', 'area-ready']);
    check(st.state === 'need-permission' && /needs your permission again/.test(st.message), 'revisit with permission "prompt": asks again', st.message);
    await p.page.click('#regrant');
    st = await waitState(p.page, ['area-ready'], 60000);
    check(st.state === 'area-ready', 'after the re-grant click the area shows');
    await p.page.close();
  }
  // G. HUD + metrics reconciliation, free-fly camera, resize.
  {
    const p = await open(base + '?hud=1&area=levt/test', undefined, { drop: false });  // resizes below
    await waitState(p.page, ['area-ready'], 60000);
    await waitFrames(p.page, 120);
    const m = await p.page.evaluate(() => window.__lomn.metrics);
    const hudText = await p.page.locator('#hud').innerText();
    const f = m.fps;
    check(f.dt_samples === f.frames_in_window - 1 && f.dt_samples >= 119, 'HUD reconciliation: dt samples == frames in window - 1',
          `${f.frames_in_window} frames, ${f.dt_samples} dt samples, window ${f.window_ms} ms, expected ${f.expected_intervals} intervals at ${f.nominal_hz} Hz`);
    check(f.display_hz > 0 && f.dropping === false && f.fps >= 0.95 * f.display_hz, 'I2: the HUD page (unspun) is not DROPPING against its own display rate',
          `fps ${f.fps} vs display ${f.display_hz} Hz`);
    check(/fps \d/.test(hudText) && /expected/.test(hudText) && /replay p50/.test(hudText) && /heap: wasm/.test(hudText), 'HUD overlay shows fps, reconciliation, breakdown, heap', hudText.split('\n')[0]);
    summary.hud = m;
    await p.page.screenshot({ path: path.join(shots, 'hud.png') });
    // M3: keys reach the camera only while the canvas has focus (or pointer lock): W with the page body focused does nothing.
    await section('M3 key scope', async () => {
      await p.page.locator('header h1').click();
      await p.page.keyboard.down('KeyW');
      const f1 = (await p.page.evaluate(() => window.__lomn.metrics)).frames;
      await p.page.waitForFunction((k) => window.__lomn.metrics.frames > k + 20, f1, { timeout: 10000 });
      await p.page.keyboard.up('KeyW');
      const cam = (await p.page.evaluate(() => window.__lomn.metrics)).area.camera;
      check(!cam.moved, 'M3: W without canvas focus leaves the camera alone', `moved ${cam.moved}`);
      await p.page.click('#reset-camera');
    });
    // Free-fly: W moves the eye, a drag turns the camera, reset restores the default view.
    const before = (await p.page.evaluate(() => window.__lomn.metrics)).area.camera;
    await p.page.locator('#canvas').focus();
    await p.page.keyboard.down('KeyW');
    await p.page.waitForFunction((z) => window.__lomn.metrics.area.camera.moved && window.__lomn.metrics.area.camera.eye[2] < z - 0.5, before.eye[2], { timeout: 10000 });
    await p.page.keyboard.up('KeyW');
    const box = await p.page.locator('#canvas').boundingBox();
    await p.page.mouse.move(box.x + 100, box.y + 100);
    await p.page.mouse.down();
    await p.page.mouse.move(box.x + 300, box.y + 100, { steps: 5 });
    await p.page.mouse.up();
    await p.page.waitForFunction((y) => Math.abs(window.__lomn.metrics.area.camera.yaw_deg - y) > 5, before.yaw_deg, { timeout: 10000 });
    const after = (await p.page.evaluate(() => window.__lomn.metrics)).area.camera;
    check(after.moved && after.eye[2] < before.eye[2] && after.yaw_deg !== before.yaw_deg, 'free-fly: W moves forward, drag turns',
          `eye z ${before.eye[2]} -> ${after.eye[2]}, yaw ${before.yaw_deg} -> ${after.yaw_deg}`);
    await p.page.click('#reset-camera');
    await p.page.waitForFunction(() => !window.__lomn.metrics.area.camera.moved, null, { timeout: 10000 });
    check(true, 'reset camera returns to the POS-start view');
    // Resize: the canvas follows its element (ResizeObserver -> lomn_resize: surface + depth recreated).
    const s0 = (await p.page.evaluate(() => window.__lomn.metrics)).surface;
    await p.page.setViewportSize({ width: 900, height: 700 });
    await p.page.waitForFunction((w) => window.__lomn.metrics.surface.width !== w, s0.width, { timeout: 10000 });
    const f0 = (await p.page.evaluate(() => window.__lomn.metrics)).frames;
    await p.page.waitForFunction((k) => window.__lomn.metrics.frames > k + 10, f0, { timeout: 10000 });
    const m2 = await p.page.evaluate(() => ({ m: window.__lomn.metrics, cw: document.getElementById('canvas').width, ch: document.getElementById('canvas').height }));
    check(m2.m.surface.width === m2.cw && m2.m.surface.height === m2.ch && m2.m.rejected_last_frame === 0 && m2.m.draws > 0,
          'resize: surface follows the canvas, frames keep drawing', `${s0.width}x${s0.height} -> ${m2.cw}x${m2.ch}, draws ${m2.m.draws}`);
    await p.page.screenshot({ path: path.join(shots, 'resized.png') });
    netOk(p, 'HUD page');
    await p.page.close();
  }
  // H. Responsive sweep 360..1920 px (40 px steps): the BYO page and the viewer (HUD on). No horizontal scroll, no
  // overlap, minimum clearance reported; after every resize the surface + depth buffer follow the canvas with no WebGPU
  // message, 0 rejected calls and all draws; at three widths the canvas equals a native render of that size.
  {
    const widths = [];
    for (let w = 360; w <= 1920; w += 40) widths.push(w);
    const layout = (sels) => {
      const boxes = [];
      for (const sel of sels)
        for (const e of document.querySelectorAll(sel)) {
          const r = e.getBoundingClientRect();
          if (r.width > 0 && r.height > 0 && getComputedStyle(e).visibility !== 'hidden') boxes.push({ id: sel + '#' + boxes.length, l: r.left, r: r.right, t: r.top, b: r.bottom });
        }
      let min = Infinity;
      const overlaps = [];
      for (let i = 0; i < boxes.length; ++i)
        for (let j = i + 1; j < boxes.length; ++j) {
          const a = boxes[i], b = boxes[j];
          const sep = Math.max(Math.max(b.l - a.r, a.l - b.r), Math.max(b.t - a.b, a.t - b.b));
          if (sep < 0) overlaps.push(`${a.id}/${b.id}`);
          min = Math.min(min, sep);
        }
      const doc = document.documentElement;
      return { hscroll: doc.scrollWidth > doc.clientWidth, offRight: boxes.filter((x) => x.r > doc.clientWidth + 0.5).map((x) => x.id), min, overlaps };
    };
    const sweep = { byo: { min: Infinity, fails: [] }, viewer: { min: Infinity, fails: [] }, pixels: [] };
    const pb = await open(base + '?pick=1', undefined, { drop: false });
    await waitState(pb.page, ['need-files']);
    for (const w of widths) {
      await pb.page.setViewportSize({ width: w, height: 900 });
      const r = await pb.page.evaluate(layout, ['header h1', '#status', '#byo > p:not(.pick)', '#byo .pick > *:not([hidden])', '#message']);
      sweep.byo.min = Math.min(sweep.byo.min, r.min);
      if (r.hscroll || r.offRight.length || r.overlaps.length) sweep.byo.fails.push({ w, ...r });
    }
    check(sweep.byo.fails.length === 0, `responsive sweep, BYO page (${widths.length} widths 360..1920): no horizontal scroll, no overlap`,
          `min clearance ${sweep.byo.min.toFixed(1)} px` + (sweep.byo.fails.length ? ' ' + JSON.stringify(sweep.byo.fails.slice(0, 3)) : ''));
    await pb.page.close();
    const pv = await open(base + '?hud=1&area=levt/stat', undefined, { drop: false });
    await waitState(pv.page, ['area-ready'], 60000);
    const msgs0 = pv.w.console.length;
    const pixelWidths = new Set([360, 1000, 1920]);
    for (const w of widths) {
      await pv.page.setViewportSize({ width: w, height: 1000 });
      const f0 = (await pv.page.evaluate(() => window.__lomn.metrics)).frames;
      await pv.page.waitForFunction((k) => {
        const m = window.__lomn.metrics, c = document.getElementById('canvas');
        return m.frames > k + 3 && m.surface.width === c.width && m.surface.height === c.height && c.width === Math.round(c.parentElement.clientWidth);
      }, f0, { timeout: 15000 });
      const m = await pv.page.evaluate(() => window.__lomn.metrics);
      const r = await pv.page.evaluate(layout, ['header h1', '#status', '#message', '#viewer .bar > *', '#canvas']);
      const hudInside = await pv.page.evaluate(() => {
        const h = document.getElementById('hud').getBoundingClientRect(), c = document.getElementById('canvas').getBoundingClientRect();
        return h.left >= c.left && h.right <= c.right && h.top >= c.top && h.bottom <= c.bottom;
      });
      sweep.viewer.min = Math.min(sweep.viewer.min, r.min);
      if (r.hscroll || r.offRight.length || r.overlaps.length || !hudInside || m.rejected_last_frame || m.draws !== m.area.draws_expected)
        sweep.viewer.fails.push({ w, ...r, hudInside, rejected: m.rejected_last_frame, draws: m.draws });
      if (pixelWidths.has(w)) {
        await pv.page.evaluate(() => { document.getElementById('hud').hidden = true; });
        const f1 = m.frames;
        await pv.page.waitForFunction((k) => window.__lomn.metrics.frames > k + 2, f1, { timeout: 10000 });
        const img = await canvasPng(pv.page, `sweep_${w}.png`);
        const out = path.join(work, `native_stat_${m.surface.width}x${m.surface.height}.png`);
        execFileSync(lomnView, ['--install', inst, '--level', 'levt', '--area', 'stat', '--frames', '3', '--width', String(m.surface.width),
                                '--height', String(m.surface.height), '--out', out], { timeout: 60000 });
        const d = diffRgb(img, decodePng(fs.readFileSync(out)));
        sweep.pixels.push({ viewport: w, canvas: `${m.surface.width}x${m.surface.height}`, differing: d.differing, max: d.max });
        check(d.sameSize && d.over(tolerance) === 0, `resize to ${m.surface.width}x${m.surface.height} (viewport ${w}): pixels == native render of that size (depth valid)`,
              `${d.differing} differ, max delta ${d.max}`);
        await pv.page.evaluate(() => { document.getElementById('hud').hidden = false; });
      }
    }
    const gpuMsgs = pv.w.console.slice(msgs0).filter((t) => t.includes('[webgpu]'));
    check(sweep.viewer.fails.length === 0 && gpuMsgs.length === 0,
          `responsive sweep, viewer (${widths.length} widths): no h-scroll/overlap, HUD inside the canvas, every resize draws all draws, 0 rejected, no WebGPU message`,
          `min clearance ${sweep.viewer.min.toFixed(1)} px` + (sweep.viewer.fails.length ? ' ' + JSON.stringify(sweep.viewer.fails.slice(0, 3)) : '') + (gpuMsgs.length ? ' ' + gpuMsgs.slice(0, 3) : ''));
    summary.responsive = { widths: widths.length, byo_min_clearance_px: sweep.byo.min, viewer_min_clearance_px: sweep.viewer.min, pixels: sweep.pixels };
    await pv.page.close();
  }
  // I2: the display rate is measured once before the first area; a frame-time spin that halves the frame rate must be
  // reported as DROPPING, where the old expectation (p50 snapped to a refresh rate) reconciles the same run as clean.
  // Review fix 2: the measurement itself must not wander (median of >= 120 empty frames, snapped only within +-3 %).
  await section('I2 dropping', async () => {
    // The display rate the page gets depends on the host (a loaded machine runs headless Chromium's compositor at 30 Hz),
    // so the spin is sized from the rate measured first: 2.0 display periods per frame at least halves the frame rate.
    // T8 P11: it also puts the frame time on a standard rate (33 ms at 60 Hz -> p50 snaps to 30 Hz), which the old circular
    // rule (DROPPING = fps < 0.95 x the snapped p50) reconciles as clean, so the DROPPING checks bite on that rule (mutation
    // 29 of tools/research/oracle/mutate4.py). The former 1.5 periods (25 ms -> 40 fps) snapped to 48 Hz and still read as
    // DROPPING under the old rule: mutation 29 was MISSED. (Measured at 30 Hz, either spin lands below 24 Hz, the lowest
    // snap rate, and the old rule also reports DROPPING: this check bites only where the display measures >= ~48 Hz.)
    const r = await open(base + '?hud=1&area=levt/test', undefined, { drop: false });
    await waitState(r.page, ['area-ready'], 60000);
    await waitFrames(r.page, 90, 20000);
    const h = (await r.page.evaluate(() => window.__lomn.metrics)).fps;
    const how = (f) => `${f.display_snapped === true ? 'snapped' : f.display_snapped === false ? 'UNSNAPPED' : 'snap flag absent'} from median ${f.display_median_ms ?? f.display_p10_ms} ms, ${f.display_samples} samples`;
    check(h.display_hz > 0 && h.dropping === false, 'I2: the display rate is measured before the first area; the unspun run is not DROPPING',
          `display ${h.display_hz} Hz (${how(h)}), fps ${h.fps}`);
    // The snap rule, deterministically (the pure function behind the measurement): standard rates only within +-3 %, otherwise
    // the raw rate, marked unsnapped; and no median in 58..62 Hz ever reads as 72 Hz (the old 66 Hz boundary).
    const classify = (ms) => r.page.evaluate((x) => { const M = window.__lomn.engine.M; return JSON.parse(M.UTF8ToString(M._lomn_debug_classify_display(x))); }, ms);
    const rule = [[1000 / 60, 60, true], [16.2, 60, true], [1000 / 59.4, 60, true], [16.1, 62.112, false], [1000 / 66, 66, false],
                  [1000 / 72, 72, true], [1000 / 30, 30, true], [1000 / 140, 144, true], [1000 / 300, 300, false]];
    const bad = [];
    for (const [ms, hz, snapped] of rule) {
      const c = await classify(ms);
      if (c.snapped !== snapped || Math.abs(c.hz - hz) > 0.01) bad.push(`${ms.toFixed(2)} ms -> ${JSON.stringify(c)} (want ${hz} ${snapped ? 'snapped' : 'unsnapped'})`);
    }
    const flips = await r.page.evaluate(() => {
      const M = window.__lomn.engine.M, out = [];
      for (let ms = 14.4; ms <= 17.6; ms += 0.01) {
        const c = JSON.parse(M.UTF8ToString(M._lomn_debug_classify_display(ms)));
        if (c.hz === 72 || (ms >= 16.2 && ms <= 17.1 && c.hz !== 60)) out.push(`${ms.toFixed(2)} ms -> ${c.hz} Hz`);
      }
      return out;
    });
    check(bad.length === 0 && flips.length === 0, 'I2: snap rule: standard rates only within +-3 %, raw and unsnapped otherwise; 58..62 Hz never reads as 72',
          bad.concat(flips.slice(0, 5)).join('; ') || `${rule.length} cases + a 14.4..17.6 ms sweep (16.2..17.1 ms always 60 Hz, never 72)`);
    // Repeatability: the measurement run 10 more times on this one page gives one rate (and it equals the boot measurement).
    const runs = [], runs0 = h.display_runs;
    for (let i = 0; i < 10; ++i) {
      await r.page.evaluate(() => window.__lomn.engine.M._lomn_debug_remeasure());
      await r.page.waitForFunction((k) => window.__lomn.metrics.fps.display_runs >= k, runs0 + i + 1, { timeout: 90000 });
      const f = (await r.page.evaluate(() => window.__lomn.metrics)).fps;
      runs.push({ hz: f.display_hz, snapped: f.display_snapped, ms: f.display_median_ms ?? f.display_p10_ms });
    }
    summary.i2_repeat = { boot: { hz: h.display_hz, ms: h.display_median_ms ?? h.display_p10_ms }, runs, loadavg: loadavg() };
    const hzs = [h.display_hz, ...runs.map((x) => x.hz)];
    check(new Set(hzs).size === 1 && runs.every((x) => x.snapped === true),
          'I2: 10 repeated measurements on one page give the same snapped rate (and the boot measurement agrees)',
          `boot ${h.display_hz}; runs ${runs.map((x) => `${x.hz}${x.snapped === true ? '' : '?'}(${Number(x.ms).toFixed(2)}ms)`).join(' ')}`);
    await r.page.close();
    const spin = Math.round((2.0 * 1000) / h.display_hz);
    summary.i2_spin_ms = spin;
    const p = await open(base + '?hud=1&area=levt/test', `
      // ${spin} ms of main-thread work on every animation frame (independent of the page's code).
      const spin = () => { const t = performance.now(); if (window.__lomn?.state === 'area-ready') while (performance.now() - t < ${spin}) {} requestAnimationFrame(spin); };
      requestAnimationFrame(spin);`);
    await waitState(p.page, ['area-ready'], 60000);
    await waitFrames(p.page, 60, 30000);
    const m = await p.page.evaluate(() => window.__lomn.metrics);
    const f = m.fps;
    const rates = [24, 30, 48, 50, 60, 72, 75, 90, 100, 120, 144, 165, 240];
    const snapped = rates.reduce((b, x) => (Math.abs(x - 1000 / f.p50_ms) < Math.abs(b - 1000 / f.p50_ms) ? x : b), rates[0]);
    const oldExpected = (f.window_ms * snapped) / 1000;
    summary.i2 = { fps: f, old_snapped_hz: snapped, old_expected_intervals: oldExpected, counted_intervals: f.dt_samples, loadavg: loadavg() };
    // The old rule took its rate from these very frames (snapped p50): with steady drops it reconciles clean (seen at 60 Hz:
    // "p50 20.6 ms -> 48 Hz -> expected 90.1 vs counted 90"); under irregular load it at least compares against a rate
    // below the display's, so it understates the drop. Either way the defect shows; both numbers are recorded.
    const oldRatio = f.dt_samples / oldExpected, newRatio = f.dt_samples / f.expected_intervals;
    summary.i2.old_ratio = oldRatio;
    summary.i2.new_ratio = newRatio;
    check((oldRatio >= 0.95 || snapped < f.display_hz) && newRatio < oldRatio, 'I2: the old circular expectation hides (or understates) the drop',
          `old: p50 ${f.p50_ms} ms -> snapped ${snapped} Hz -> expected ${oldExpected.toFixed(1)} vs counted ${f.dt_samples} (ratio ${oldRatio.toFixed(2)}); ` +
          `new: display ${f.display_hz} Hz -> expected ${f.expected_intervals.toFixed(1)} (ratio ${newRatio.toFixed(2)})`);
    const hud = await p.page.locator('#hud').innerText();
    // The spun page measured its own display rate (before the spin starts); it is compared with that, not with another page's.
    check(f.display_hz > 0 && f.dropping === true && f.fps < 0.95 * f.display_hz && /DROPPING/.test(hud), `I2: a ${spin} ms spin per frame -> fps below 0.95 x the display rate measured on that page -> DROPPING`,
          `display ${f.display_hz} Hz, fps ${f.fps}, expected ${f.expected_intervals} intervals, dropping ${f.dropping}`);
    await p.page.close();
    const q = await open(base + `?hud=1&area=levt/test&debug=1&debug_spin=${spin}`);
    await waitState(q.page, ['area-ready'], 60000);
    await waitFrames(q.page, 60, 30000);
    const g = (await q.page.evaluate(() => window.__lomn.metrics)).fps;
    check(g.dropping === true && g.spin_ms === spin, `I2: ?debug=1&debug_spin=${spin} makes DROPPING fire`, `fps ${g.fps} vs display ${g.display_hz} Hz, spin ${g.spin_ms}`);
    await q.page.close();
  });
  // I1: textures and buffers are destroyed explicitly (not left to garbage collection): live GPUTexture/GPUBuffer counts
  // stay flat over 30 area switches (pre-fix they grow by the area's objects on every switch).
  await section('I1 explicit destroy', async () => {
    const p = await open(base + '?area=levt/test', gpuLiveHook);
    await waitState(p.page, ['area-ready'], 60000);
    const seen = [];
    for (let i = 0; i < 30; ++i) {
      const name = i % 2 ? 'levt/test' : 'levt/stat';
      await p.page.evaluate((n) => window.__lomn.showArea(n), name);
      await waitFrames(p.page, 2);
      if (name === 'levt/test') seen.push(await p.page.evaluate(() => ({ ...window.__gpuLive })));
    }
    summary.i1_gpu_live = seen;
    const flat = seen.every((x) => x.textures === seen[0].textures && x.buffers === seen[0].buffers);
    check(flat, 'I1: live GPUTexture/GPUBuffer counts flat over 30 switches', `first ${JSON.stringify(seen[0])}, last ${JSON.stringify(seen[seen.length - 1])}`);
    await p.page.close();
  });
  // M2: on boot, cached prefetch sets of other wasm builds are pruned.
  await section('M2 prune', async () => {
    const p = await open(base + '?pick=1');
    await waitState(p.page, ['need-files']);
    await p.page.evaluate(() => new Promise((res) => {
      const r = indexedDB.open('lomn-web', 1);
      r.onsuccess = () => { const t = r.result.transaction('kv', 'readwrite'); t.objectStore('kv').put('root/x\t0\t1\n', 'prefetch|fp|0000stale0000|area:levt/test'); t.oncomplete = () => { r.result.close(); res(); }; };
    }));
    await p.page.close();
    const q = await open(base + '?area=levt/test');
    await waitState(q.page, ['area-ready'], 60000);
    const left = await q.page.evaluate(() => new Promise((res) => {
      const r = indexedDB.open('lomn-web', 1);
      r.onsuccess = () => { const g = r.result.transaction('kv').objectStore('kv').getAllKeys(); g.onsuccess = () => { r.result.close(); res(g.result.filter((k) => String(k).includes('0000stale0000'))); }; };
    }));
    check(left.length === 0, 'M2: a stale build\'s cached prefetch sets are pruned at boot', JSON.stringify(left));
    await q.page.close();
  });
  // M4: two picks in a row start one engine, not two on one canvas.
  await section('M4 single engine', async () => {
    const p = await open(base + '?area=levt/test&pick=1');
    await waitState(p.page, ['need-files']);
    const r0 = srv.responses.length;
    const res = await p.page.evaluate(async () => {
      const d = await (await navigator.storage.getDirectory()).getDirectoryHandle('lomn-install');
      return Promise.all([window.__lomn.useDirectoryHandle(d), window.__lomn.useDirectoryHandle(d)]);
    });
    await waitState(p.page, ['area-ready'], 60000);
    const wasmFetches = srv.responses.slice(r0).filter((r) => /lomn_web\.[0-9a-f]+\.wasm$/.test(r.path)).length;
    check(wasmFetches === 1 && res.filter(Boolean).length === 1, 'M4: concurrent picks start exactly one engine', `wasm fetches ${wasmFetches}, results ${JSON.stringify(res)}`);
    await p.page.close();
  });
  // I3: a remembered folder that was moved or deleted leads back to the picker with a message (pre-fix: dead 'error').
  await section('I3 moved folder', async () => {
    const p = await open(base + '?area=levt/test&pick=1');
    await waitState(p.page, ['need-files']);
    await p.page.evaluate(async () => {
      const root = await navigator.storage.getDirectory();
      const cp = async (a, b) => {
        for await (const [n, h] of a.entries()) {
          if (h.kind === 'directory') await cp(h, await b.getDirectoryHandle(n, { create: true }));
          else { const w = await (await b.getFileHandle(n, { create: true })).createWritable(); await w.write(await (await h.getFile()).arrayBuffer()); await w.close(); }
        }
      };
      await cp(await root.getDirectoryHandle('lomn-install'), await root.getDirectoryHandle('lomn-moved', { create: true }));
      await window.__lomn.useDirectoryHandle(await root.getDirectoryHandle('lomn-moved'));
    });
    await waitState(p.page, ['area-ready'], 60000);
    await p.page.close();
    const q = await open(base + '?pick=1');
    await waitState(q.page, ['need-files']);
    await q.page.evaluate(async () => (await navigator.storage.getDirectory()).removeEntry('lomn-moved', { recursive: true }));
    await q.page.close();
    const r = await open(base + '?area=levt/test');
    const st = await waitState(r.page, ['need-files', 'area-ready'], 30000);
    const picker = await r.page.locator('#pick-dir').isVisible();
    check(st.state === 'need-files' && /could not be read/.test(st.message) && picker, 'I3: a moved remembered folder -> need-files with a message and the picker',
          `${st.state}: ${st.message}; picker visible ${picker}`);
    await idbDeleteHandle(r.page);
    await r.page.close();
  });
  // I7: a copy without its completion manifest (interrupted) is removed at start, with a message.
  await section('I7 partial copy', async () => {
    const p = await open(base + '?pick=1');
    await waitState(p.page, ['need-files']);
    await idbDeleteHandle(p.page);
    await p.page.evaluate(async () => (await (await navigator.storage.getDirectory()).getDirectoryHandle('lomn-install')).removeEntry('lomn-copy-complete.json'));
    await p.page.close();
    const q = await open(base);
    const st = await waitState(q.page, ['need-files'], 30000);
    const left = await opfsHas(q.page, 'lomn-install');
    check(!left && /incomplete copy/.test(st.message), 'I7: an interrupted copy is cleared at start', `dir left ${left}; ${st.message}`);
    await q.page.close();
  });
  // final I-2: files whose names hold TAB / LF (legal on macOS and Linux) under data/ are skipped with a named warning and the
  // rest of the install stays usable (pre-fix: the area list was split at the tab and the default area dead-ended in 'error').
  await section('I-2 control-character file names', async () => {
    const p = await open(base + '?size=1280x720&pick=1', undefined, { drop: false });
    await waitState(p.page, ['need-files']);
    await p.page.setInputFiles('#byo-input', tabDir);
    const st = await waitState(p.page, ['area-ready'], 120000);
    const info = await p.page.evaluate(() => ({ areas: window.__lomn.areas.map((a) => `${a.level}/${a.area}`), warnings: window.__lomn.sourceWarnings, files: window.__lomn.source?.files, validation: window.__lomn.validation?.ok }));
    const warn = (info.warnings ?? []).join('\n');
    check(st.state === 'area-ready' && info.validation === true && /control character U\+0009/.test(warn) && /control character U\+000A/.test(warn),
          'final I-2: TAB- and LF-named files are skipped with a warning naming the character, and the install is usable',
          `${st.state}: ${st.message}; warnings ${JSON.stringify(info.warnings)}`);
    check(JSON.stringify(info.areas) === JSON.stringify(['levt/stat', 'levt/test']),
          'final I-2: the area list holds only real areas (no stem split at the tab)', JSON.stringify(info.areas));
    const copied = await p.page.evaluate(async () => {
      const d = await (await (await navigator.storage.getDirectory()).getDirectoryHandle('lomn-install')).getDirectoryHandle('data');
      const names = [];
      const walk = async (h, pre) => { for await (const [n, x] of h.entries()) { names.push(pre + n); if (x.kind === 'directory') await walk(x, pre + n + '/'); } };
      await walk(d, '');
      return names;
    });
    check(!copied.some((n) => /[\x00-\x1f\x7f]/.test(n)), 'final I-2: nothing with a control character was copied into OPFS', JSON.stringify(copied.filter((n) => /[\x00-\x1f\x7f]/.test(n))));
    await p.page.evaluate(() => window.__lomn.showArea('levt/stat'));
    const sw = await p.page.evaluate(() => window.__lomn.state);
    check(sw === 'area-ready', 'final I-2: an area still switches on that install', sw);
    await p.page.close();
  });
  // C2 (decision D13): accepted exe + unknown .blk tables is soft-accepted with a visible warning and a named reason.
  await section('C2 soft accept', async () => {
    const p = await open(base + '?size=1280x720&area=levt/test&pick=1');
    await waitState(p.page, ['need-files']);
    await p.page.setInputFiles('#byo-input', foreignDataDir);
    const st = await waitState(p.page, ['area-ready', 'rejected'], 120000);
    const warn = await p.page.locator('#warning').isVisible().catch(() => false) ? await p.page.locator('#warning').innerText() : '';
    const m = await p.page.evaluate(() => window.__lomn.metrics);
    check(st.state === 'area-ready' && /game data differs from the known build — rendering may differ/i.test(warn) && m?.validation?.data === 'unknown' && /fingerprint/.test(m?.validation?.warning ?? ''),
          'C2: accepted exe + unknown tables -> shown with a warning; the reason is in metrics', `${st.state}; warning "${warn}"; metrics ${JSON.stringify(m?.validation)}`);
    await p.page.close();
  });
  // Review fix 1 (T5b-I1): a failure while a REMEMBERED source starts (here: the wasm fetch fails) must reach 'error' with the
  // Reload button, on every revisit path, with no unhandled rejection (pre-fix the page stayed at 'starting' forever).
  await section('I1 revisit failure', async () => {
    const failWasm = `
      const realFetch = window.fetch;
      window.fetch = (input, init) => (/lomn_web\\.[0-9a-f]+\\.wasm/.test(String(input?.url ?? input))
        ? Promise.reject(new Error('TEST: injected wasm fetch failure')) : realFetch.call(window, input, init));`;
    const outcome = async (p) => {
      const st = await waitState(p.page, ['need-files', 'area-ready'], 15000).catch(() => p.page.evaluate(() => ({ state: window.__lomn.state, message: window.__lomn.message })));
      const reload = await p.page.locator('#reload').isVisible().catch(() => false);
      return { ...st, reload, errors: p.w.errors.slice() };
    };
    const verdict = (o) => `${o.state}: ${o.message}; reload visible ${o.reload}; page errors ${JSON.stringify(o.errors)}`;
    const good = (o) => o.state === 'error' && /injected wasm fetch failure/.test(o.message) && o.reload && o.errors.length === 0;
    // (a) the finished OPFS copy (what C2 left): main.js `return useSource(src, 'revisit')`.
    {
      const p = await open(base + '?area=levt/test', failWasm);
      const o = await outcome(p);
      check(good(o), 'T5b-I1: OPFS revisit + wasm fetch failure -> error, Reload button, no unhandled rejection', verdict(o));
      await p.page.close();
    }
    // A remembered folder handle: picked once (OPFS directory handle = what showDirectoryPicker returns), persisted in IndexedDB.
    {
      const p = await open(base + '?area=levt/test&pick=1', undefined, { drop: false });
      await waitState(p.page, ['need-files']);
      const ok = await p.page.evaluate(async () => window.__lomn.useDirectoryHandle(await (await navigator.storage.getDirectory()).getDirectoryHandle('lomn-install')));
      await waitState(p.page, ['area-ready'], 60000);
      check(ok === true, 'T5b-I1 setup: a folder handle is remembered in IndexedDB');
      await p.page.close();
    }
    // (b) the remembered handle, permission already granted: main.js `return revisit()`.
    {
      const p = await open(base + '?area=levt/test', failWasm);
      const o = await outcome(p);
      check(good(o), 'T5b-I1: handle revisit + wasm fetch failure -> error, Reload button, no unhandled rejection', verdict(o));
      await p.page.close();
    }
    // (c) the same after the permission re-request: the #regrant onclick `revisit()`.
    {
      const p = await open(base + '?area=levt/test', `
        FileSystemHandle.prototype.queryPermission = async () => 'prompt';
        FileSystemHandle.prototype.requestPermission = async () => 'granted';
        ${failWasm}`);
      await waitState(p.page, ['need-permission']);
      await p.page.click('#regrant');
      const o = await outcome(p);
      check(good(o), 'T5b-I1: re-grant click + wasm fetch failure -> error, Reload button, no unhandled rejection', verdict(o));
      await p.page.close();
    }
    // (d) review fix N3: requestPermission() itself rejects: a message, the button stays, no unhandled rejection.
    {
      const p = await open(base + '?area=levt/test', `
        FileSystemHandle.prototype.queryPermission = async () => 'prompt';
        FileSystemHandle.prototype.requestPermission = async () => { throw new Error('TEST: requestPermission rejected'); };`);
      await waitState(p.page, ['need-permission']);
      await p.page.click('#regrant');
      await p.page.waitForFunction(() => /could not be requested/.test(window.__lomn.message), null, { timeout: 15000 }).catch(() => {});
      const o = await p.page.evaluate(() => ({ state: window.__lomn.state, message: window.__lomn.message }));
      const button = await p.page.locator('#regrant').isVisible();
      check(o.state === 'need-permission' && /could not be requested.*TEST: requestPermission rejected/.test(o.message) && button && p.w.errors.length === 0,
            'N3: a rejected requestPermission() shows a message, keeps the re-grant button, and leaves no unhandled rejection',
            `${o.state}: ${o.message}; button visible ${button}; page errors ${JSON.stringify(p.w.errors)}`);
      await p.page.close();
    }
    const q = await open(base + '?pick=1', undefined, { drop: false });
    await waitState(q.page, ['need-files']);
    await idbDeleteHandle(q.page);
    await q.page.close();
  });
  // final m-3: the remembered-folder write failing (IndexedDB quota, a foreign VersionError) after a successful start is not a fatal
  // error: the area keeps rendering, the page says the folder will not be remembered. The picker is stubbed to hand back an OPFS
  // directory handle (what showDirectoryPicker returns), and IDBObjectStore.put throws for the 'install-handle' key.
  await section('m-3 handle not saved', async () => {
    const p = await open(base + '?size=1280x720&area=levt/test&pick=1', `
      window.showDirectoryPicker = async () => (await navigator.storage.getDirectory()).getDirectoryHandle('lomn-install');
      const put = IDBObjectStore.prototype.put;
      IDBObjectStore.prototype.put = function (v, k) { if (k === 'install-handle') throw new DOMException('TEST: quota exceeded', 'QuotaExceededError'); return put.apply(this, arguments); };`);
    await waitState(p.page, ['need-files']);
    await p.page.click('#pick-dir');
    await waitState(p.page, ['area-ready'], 60000);
    const ok = await p.page.waitForFunction(() => window.__lomn.handleNotSaved !== undefined || window.__lomn.error, null, { timeout: 10000 }).then(() => true, () => false);
    const o = await p.page.evaluate(() => ({ state: window.__lomn.state, error: window.__lomn.error ?? null, notSaved: window.__lomn.handleNotSaved ?? null,
                                              reload: !document.getElementById('reload').hidden, warning: document.getElementById('warning').hidden ? '' : document.getElementById('warning').textContent }));
    check(ok && o.state === 'area-ready' && !o.error && !o.reload && o.notSaved && /will not be remembered|not remember/i.test(o.warning),
          'final m-3: a failing IndexedDB write of the folder handle is not fatal: area-ready, no error, no Reload, a warning',
          `${JSON.stringify(o)}; settled ${ok}; page errors ${JSON.stringify(p.w.errors)}`);
    await p.page.close();
    const q = await open(base + '?pick=1', undefined, { drop: false });
    await waitState(q.page, ['need-files']);
    await idbDeleteHandle(q.page);
    await q.page.close();
  });
  // C1: a lost GPU device reaches the page: state 'error', a Reload button, the loop stopped (pre-fix: stays area-ready).
  await section('C1 device lost', async () => {
    // A source of its own (copy through the input), so the step does not depend on what earlier steps left behind.
    const p = await open(base + '?area=levt/test&pick=1', undefined, { drop: false });  // the GPU process is crashed below
    await waitState(p.page, ['need-files']);
    await p.page.setInputFiles('#byo-input', inst);
    await waitState(p.page, ['area-ready'], 120000);
    await waitFrames(p.page, 3);
    const cdp = await p.page.context().newCDPSession(p.page);
    await cdp.send('Browser.crashGpuProcess').catch(() => {});
    const st = await waitState(p.page, ['error'], 15000).catch(() => p.page.evaluate(() => ({ state: window.__lomn.state, message: window.__lomn.message })));
    const reload = await p.page.locator('#reload').isVisible().catch(() => false);
    const f0 = await p.page.evaluate(() => window.__lomn.metrics?.frames ?? -1).catch(() => -1);
    p.w.expectGpuLoss = true;  // this page's [webgpu] messages are the point of the test
    check(st.state === 'error' && /device lost|GPU/i.test(st.message) && reload, 'C1: GPU process crash -> state error with the reason and a Reload button',
          `${st.state}: ${st.message}; reload visible ${reload}; frames ${f0}`);
    summary.c1 = { state: st.state, message: st.message };
    await afterLoss(p, 'C1', st, f0);
    await p.page.close();
  });
  // final I-1 (b): the same loss lands while a switch is waiting for its prefetch reads (Blob.arrayBuffer held by the test): when the
  // reads are released the switch must not flip the page to 'area-ready' on the dead device (pre-fix: it did, frozen, fps 0).
  await section('I-1 loss during a switch', async () => {
    const p = await open(base + '?size=1280x720&area=levt/test', `
      window.__hold = null; const ab = Blob.prototype.arrayBuffer;
      Blob.prototype.arrayBuffer = function () { const h = window.__hold; return h ? h.then(() => ab.call(this)) : ab.call(this); };`, { drop: false });
    await waitState(p.page, ['area-ready'], 60000);
    await waitFrames(p.page, 3);
    p.w.expectGpuLoss = true;
    // forgetCachedSet: the discovery rounds then really read (and wait on) the held arrayBuffer calls.
    await p.page.evaluate(async () => {
      await window.__lomn.forgetCachedSet('levt/stat');
      window.__hold = new Promise((r) => { window.__release = r; });
      window.__switch = window.__lomn.showArea('levt/stat');
    });
    const loading = await p.page.evaluate(() => window.__lomn.state);
    const cdp = await p.page.context().newCDPSession(p.page);
    await cdp.send('Browser.crashGpuProcess').catch(() => {});
    const st = await waitState(p.page, ['area-ready'], 15000).catch(() => p.page.evaluate(() => ({ state: window.__lomn.state, message: window.__lomn.message })));
    const f0 = await p.page.evaluate(() => window.__lomn.metrics?.frames ?? -1).catch(() => -1);
    check(loading === 'loading-area' && st.state === 'error' && /device lost|GPU/i.test(st.message),
          'I-1 setup: the GPU process crashed while the switch was waiting on its prefetch reads', `before the crash ${loading}; after ${st.state}: ${st.message}`);
    await p.page.evaluate(() => window.__release());
    const ret = await p.page.evaluate(() => window.__switch);
    const o = await p.page.evaluate(() => ({ state: window.__lomn.state, message: window.__lomn.message, area: window.__lomn.load?.area, frames: window.__lomn.metrics?.frames ?? -1,
                                              disabled: document.getElementById('area-picker').disabled, reload: !document.getElementById('reload').hidden }));
    check(ret === false && o.state === 'error' && /device lost|GPU/i.test(o.message) && o.area === 'levt/test' && o.frames === f0 && o.disabled && o.reload,
          'final I-1: a switch that finishes after the device was lost is refused: state stays error with the loss reason, the previous area stays recorded, the picker is disabled',
          `returned ${ret}; ${JSON.stringify(o)}; frames at loss ${f0}`);
    await p.page.close();
  });
  // Review fix 2: every unspun page that showed an area (sampled as it closed) was NOT DROPPING against the display rate
  // measured on that same page, and the engine's flag agrees with fps < 0.95 x that rate.
  {
    const dv = dropSamplesVerdict(dropSamples);
    summary.drop_samples = dropSamples;
    const fmt = (x) => `${x.fps.toFixed(1)}/${x.hz}Hz${x.snapped === false ? '?' : ''}`;
    check(dv.ok,
          `I2: every unspun page (${dropSamples.length} sampled) is NOT DROPPING against the display rate measured on that same page (a flagged page is re-measured once)`,
          (dv.wrong.length || dv.incoherent.length ? 'DROPPING/incoherent: ' + JSON.stringify([...dv.wrong, ...dv.incoherent]) :
            dropSamples.map((x) => (x.remeasure ? `${fmt(x)} [flagged at load ${x.loadavg}; re-measured ${fmt(x.remeasure)} at load ${x.remeasure.loadavg}]` : fmt(x))).join(' ')));
  }
  summary.loadavg = { start: loadavgStart, end: loadavg() };
  for (const w of pageWatchers.filter((x) => !x.expectGpuLoss)) {
    const gpu = w.console.filter((t) => t.includes('[webgpu]') || t.includes('[area]'));
    if (gpu.length || w.errors.length) check(false, 'no WebGPU errors or page exceptions', JSON.stringify({ gpu, errors: w.errors }));
  }
  check(pageWatchers.filter((x) => !x.expectGpuLoss).every((w) => !w.errors.length), 'no page exceptions on any page');
} catch (e) {
  check(false, 'test run', e.stack || String(e));
} finally {
  await ctx.close();
  srv.close();
}
fs.writeFileSync(path.join(work, 'results.json'), JSON.stringify({ results, summary }, null, 1));
const failed = results.filter((r) => !r.ok);
console.log(`byo_synthetic: ${results.length - failed.length}/${results.length} checks passed`);
process.exitCode = failed.length ? 1 : 0;
