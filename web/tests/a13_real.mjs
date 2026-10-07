// A13 browser smoke on the REAL install (needs LOMN_DATA; exits 77 = skipped otherwise). One persistent Chromium
// (real GPU), state waits only. Usage: node tests/a13_real.mjs <site_dir> <work_dir> <native lomn_view> <wasm_build_dir>
// The sweep over all 122 areas runs by default (rounds, bytes and over-fetch per area; draws == expected; heap growth);
// A13_SWEEP=0 skips it.
//      A13_FPS_FRAMES (default 600) frames per area for the fps measurement; LOMN_PIXEL_TOLERANCE (default 0).
// Steps: (1) the real folder through <input webkitdirectory> (setInputFiles with the install directory) -> exe +
// blockfile-table validation -> quota -> OPFS copy in the worker; (2) lev1/vllg, lev2/ghut, lev4/crss at the default
// camera vs native renders of the same build; (3) discovery == `lomn_view --manifest` for the three areas; (4) area
// switches with flat live-object counts; lev0/frnt (largest texture set) for the heap; (5) revisit: no copy, cached
// prefetch sets; (6) the handle path (OPFS directory handle as a picked handle) + revisit; (7) ?hud=1 fps with
// reconciliation per area; (8) network discipline on every page. The OPFS copy and the profile are removed at the end.
import { execFileSync } from 'node:child_process';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { serve } from './serve.mjs';
import { decodePng, diffRgb, launch, networkCheck, waitFrames, waitState, watch } from './lib.mjs';

const [siteArg, workArg, lomnView, wasmDir] = process.argv.slice(2);
if (!siteArg || !workArg || !lomnView || !wasmDir) {
  console.error('usage: a13_real.mjs <site_dir> <work_dir> <lomn_view> <wasm_build_dir>');
  process.exit(2);
}
if (!process.env.LOMN_DATA) {
  console.log('SKIPPED (no LOMN_DATA)');
  process.exit(77);
}
const install = path.join(process.env.LOMN_DATA, 'install');
const site = path.resolve(siteArg), work = path.resolve(workArg);
const AREAS = ['lev1/vllg', 'lev2/ghut', 'lev4/crss'];
const tolerance = Number(process.env.LOMN_PIXEL_TOLERANCE ?? 0);
// I5: measured on Beta 1.5.0 (REF4 r2 pre-fix run, all 122 areas, fresh discovery): max 8 rounds (lev1/atrm and 6 others;
// histogram 1:4 2:1 3:4 5:1 6:104 7:1 8:7). Cap 12 = 1.5x headroom, under the engine's hard limit of 16 (MAX_ROUNDS).
const ROUNDS_CAP = 12;
const fpsFrames = Number(process.env.A13_FPS_FRAMES ?? 600);
const results = [];
const check = (ok, what, detail = '') => {
  results.push({ ok: !!ok, what, detail });
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}${detail ? ' :: ' + detail : ''}`);
};
const loadavg = () => os.loadavg().map((x) => x.toFixed(2)).join(' ');
fs.rmSync(work, { recursive: true, force: true });
fs.mkdirSync(path.join(work, 'shots'), { recursive: true });
const summary = { loadavg_start: loadavg(), areas: {} };
const builtWasmSha = crypto.createHash('sha256').update(fs.readFileSync(path.join(wasmDir, 'lomn_web.wasm'))).digest('hex');
const gate = { tolerance, native_vs_browser: [] };
const kept = {};

// Native references from the same build (default camera, 1280x720, 60 frames: the M1a-3 evidence command line).
const refs = {};
for (const a of [...AREAS, 'lev0/frnt']) {
  const [level, area] = a.split('/');
  const out = path.join(work, `native_${area}.png`);
  execFileSync(lomnView, ['--install', install, '--level', level, '--area', area, '--frames', '60', '--out', out], { timeout: 120000 });
  refs[a] = decodePng(fs.readFileSync(out));
}
execFileSync(lomnView, ['--install', install, '--manifest', '1', '--out', path.join(work, 'manifest.json')], { timeout: 120000 });
const manifest = JSON.parse(fs.readFileSync(path.join(work, 'manifest.json'), 'utf8'));
const nativeRanges = (name) => manifest.areas.find((x) => x.area === name).ranges.map(([f, o, l]) => `${manifest.files[f].path}\t${o}\t${l}`).sort().join('\n');

const srv = await serve(site);
const ctx = await launch(path.join(work, 'profile'));
const base = srv.url + 'index.html';
const watchers = [];
const open = async (url, init) => {
  const page = await ctx.newPage();
  if (init) await page.addInitScript(init);
  const w = watch(page);
  watchers.push(w);
  const r0 = srv.responses.length;
  await page.goto(url, { timeout: 30000 });
  return { page, w, r0 };
};
const netOk = (p, label) => {
  const n = networkCheck(srv, site, p.w, p.r0, 0);
  check(n.ok, `network discipline: ${label}`, `${n.responses} responses, ${n.bytes} of ${n.buildBytes} build bytes` +
        (n.foreignPaths.length ? ` foreign ${n.foreignPaths}` : '') + (n.foreignUrls.length ? ` foreign urls ${n.foreignUrls}` : ''));
};
const shot = async (page, name) => {
  // A clip on whole CSS pixels (deviceScaleFactor 1): the drawing buffer exactly. A fractional box would resample.
  const box = await page.locator('#canvas').boundingBox();
  if (![box.x, box.y, box.width, box.height].every(Number.isInteger)) throw new Error('canvas not on whole pixels: ' + JSON.stringify(box));
  const png = await page.screenshot({ clip: box });
  fs.writeFileSync(path.join(work, 'shots', name), png);
  return decodePng(png);
};
const compare = (img, ref, label) => {
  const d = diffRgb(img, ref);
  if (d.sameSize) gate.native_vs_browser.push({ label, differing: d.differing, max: d.max });
  const ok = d.sameSize && d.over(tolerance) === 0;
  // M7: tolerance 0 is a same-machine (same GPU) gate; a failure names the adapter so a cross-GPU run is recognisable.
  check(ok, `pixels ${label} vs native`, (d.sameSize ?
        `${d.differing} of ${d.pixels} differ, max delta ${d.max}; over tolerance ${tolerance}: ${d.over(tolerance)}` : 'size differs') +
        (ok ? '' : `; adapter ${summary.measured_config?.adapter ?? 'unknown'} (set LOMN_PIXEL_TOLERANCE after measuring null + native-vs-browser on this GPU)`));
  return d.sameSize ? { differing: d.differing, max: d.max } : null;
};
const metrics = (page) => page.evaluate(() => window.__lomn.metrics);
const show = async (page, name) => {
  const ok = await page.evaluate((n) => window.__lomn.showArea(n), name);
  const st = await page.evaluate(() => ({ state: window.__lomn.state, error: window.__lomn.error }));
  if (!ok || st.state !== 'area-ready') throw new Error(`showArea ${name}: ${st.state} ${st.error ?? ''}`);
  await waitFrames(page, 3);
  return page.evaluate(() => window.__lomn.load);
};

try {
  // (1) + (2) + (3) + (4): first visit through the directory input (OPFS path).
  {
    const p = await open(base + '?size=1280x720&area=lev1/vllg');
    await waitState(p.page, ['need-files']);
    const t0 = Date.now();
    await p.page.setInputFiles('#byo-input', install);
    const st = await waitState(p.page, ['area-ready', 'rejected', 'quota-short'], 600000);
    check(st.state === 'area-ready', 'real install: validated, copied to OPFS, lev1/vllg shown', st.message || st.error);
    const info = await p.page.evaluate(() => ({ v: window.__lomn.validation, source: window.__lomn.source, copy: window.__lomn.copy, quota: window.__lomn.quota, boot: window.__lomn.boot, ready: window.__lomn.metricsAtReady }));
    summary.heap_at_ready = info.ready.heap;
    summary.first_visit = { wall_ms: Date.now() - t0, validation: { build: info.v.build, data: info.v.data, ms: info.v.ms, blk_files: info.v.blkFiles, table_bytes: info.v.tableBytes },
                            source: info.source, copy: info.copy, quota: info.quota, boot: info.boot };
    check(info.v.build === 'beta-1.5.0-installer' && info.v.data === 'beta-1.5.0-data', 'accepted as beta-1.5.0-installer + beta-1.5.0-data');
    const m0 = await metrics(p.page);
    summary.heap_after_first_area = m0.heap;
    for (const a of AREAS) {
      const load = a === 'lev1/vllg' ? await p.page.evaluate(() => window.__lomn.load) : await show(p.page, a);
      await waitFrames(p.page, 3);
      const m = await metrics(p.page);
      kept['first ' + a] = await shot(p.page, `first_${a.replace('/', '_')}.png`);
      const px = compare(kept['first ' + a], refs[a], `${a} (first visit, OPFS path)`);
      if (a === 'lev1/vllg') {
        summary.measured_config = { build: m.build, canvas: m.canvas, adapter: m.adapter, loadavg: loadavg() };
        check(m.build.wasm_sha256 === builtWasmSha && m.build.type === 'Release', 'measured configuration: the page runs the wasm in the build dir (Release)',
              `${m.build.type} [${m.build.cxx_flags}] wasm ${m.build.wasm_sha256.slice(0, 12)} vs built ${builtWasmSha.slice(0, 12)}; canvas ${m.canvas.width}x${m.canvas.height}; ${m.adapter}`);
      }
      const touched = load.touched.trim().split('\n').sort().join('\n');
      check(touched === nativeRanges(a), `discovery: ${a} fetched exactly its native access log`, `${load.rounds} rounds, ${load.ranges} ranges, ${load.bytes} bytes`);
      check(m.draws === m.area.draws_expected && m.rejected_last_frame === 0, `${a}: draws == the area's expected draws, 0 rejected`, `draws ${m.draws}`);
      summary.areas[a] = { first: { rounds: load.rounds, ranges: load.ranges, bytes: load.bytes, fetch_ms: load.fetch_ms, total_ms: load.load_total_ms,
                                    probe_ms: m.area.probe_ms, load_ms: m.area.load_ms, decode_ms: m.area.decode_ms, upload_ms: m.area.upload_ms },
                           pixels: px, draws: m.draws, pipelines: m.pipelines, textures: m.textures, texture_bytes: m.texture_bytes, heap: m.heap, live: m.live_objects };
    }
    // Largest texture set.
    const lf = await show(p.page, 'lev0/frnt');
    const mf = await metrics(p.page);
    summary.frnt = { load: { rounds: lf.rounds, bytes: lf.bytes, total_ms: lf.load_total_ms }, heap: mf.heap, texture_bytes: mf.texture_bytes, draws: mf.draws };
    compare(await shot(p.page, 'first_lev0_frnt.png'), refs['lev0/frnt'], 'lev0/frnt');
    // Switch cycles: flat counts per area.
    const per = {};
    for (let i = 0; i < 9; ++i) {
      const a = AREAS[i % 3];
      await show(p.page, a);
      const m = await metrics(p.page);
      const key = JSON.stringify(m.live_objects) + ` fs=${m.heap.fs_resident_bytes}`;
      if (!per[a]) per[a] = key;
      check(per[a] === key && m.release_failures === 0, `switch ${i + 1} -> ${a}: live objects and resident file bytes flat`, key);
      const own = (await p.page.evaluate(() => window.__lomn.load.touched)).trim().split('\n').reduce((n, l) => n + Number(l.split('\t')[2]), 0);
      check(m.heap.fs_resident_bytes === own, `switch ${i + 1}: resident file bytes == ${a}'s own read set (previous area released)`, `${m.heap.fs_resident_bytes} vs ${own}`);
    }
    summary.heap_after_switches = (await metrics(p.page)).heap;
    // Memory budget (spec §4.6): INITIAL_MEMORY covers boot + the largest texture set + the switches, so the heap never
    // grew in this page (growth copies the heap without pthreads).
    // M14: a same-machine regression gate. One growth step of <= 16 MiB is printed, not failed (a different malloc
    // pattern may cross the line by one step); more, or a bigger step, fails.
    const grewBy = summary.heap_after_switches.wasm_memory_bytes - info.ready.heap.wasm_memory_bytes;
    summary.memory_growth_bytes = grewBy;
    check(grewBy <= 16 * 1048576, 'memory budget: wasm memory growth from boot through vllg/ghut/crss/frnt and 9 switches <= one 16 MiB step',
          `${info.ready.heap.wasm_memory_bytes} -> ${summary.heap_after_switches.wasm_memory_bytes} bytes (growth ${grewBy})`);
    if (process.env.A13_SWEEP !== '0') {
      const all = await p.page.evaluate(() => window.__lomn.areas.map((a) => `${a.level}/${a.area}`));
      let maxMem = (await metrics(p.page)).heap.wasm_memory_bytes;
      const bad = [], grew = [], rows = [];
      const t1 = Date.now();
      for (const a of all) {
        try {
          // I5: discovery on a fresh set (no cached set: forget it first) to record the rounds every area needs.
          await p.page.evaluate((k) => window.__lomn.forgetCachedSet?.(k), a);
          const ld = await show(p.page, a);
          const own = ld.touched.trim().split('\n').reduce((n, l) => n + Number(l.split('\t')[2]), 0);
          rows.push({ area: a, rounds: ld.rounds, cached: ld.cached, bytes_fetched: ld.bytes, read_set: own, over_fetch: ld.over_fetch_bytes });
          const m = await metrics(p.page);
          if (m.heap.wasm_memory_bytes > maxMem) { maxMem = m.heap.wasm_memory_bytes; grew.push(`${a}: ${maxMem}`); }
          if (m.draws !== m.area.draws_expected || m.rejected_last_frame) bad.push(`${a}: draws ${m.draws}/${m.area.draws_expected} rejected ${m.rejected_last_frame}`);
        } catch (e) { bad.push(`${a}: ${e.message}`); }
      }
      // wasm memory never shrinks: "grew_at" lists the areas whose load grew it (none = it fits the current size).
      const maxRow = rows.reduce((b, r) => (r.rounds > b.rounds ? r : b), rows[0] ?? { rounds: 0 });
      summary.sweep = { areas: all.length, wall_ms: Date.now() - t1, max_wasm_memory: maxMem, grew_at: grew, problems: bad,
                        rounds_max: maxRow.rounds, rounds_max_at: maxRow.area, rounds_histogram: rows.reduce((h, r) => ((h[r.rounds] = (h[r.rounds] || 0) + 1), h), {}),
                        rows, final_heap: (await metrics(p.page)).heap };
      // I5: measured first (REF4 r2: every area converges in <= ROUNDS_MEASURED rounds), then capped with headroom below
      // the engine's hard limit of 16 so a loader change that deepens the chain fails here first.
      check(rows.every((r) => !r.cached) && maxRow.rounds <= ROUNDS_CAP, `sweep: every area discovers its set in <= ${ROUNDS_CAP} rounds (max ${maxRow.rounds} at ${maxRow.area})`,
            JSON.stringify(summary.sweep.rounds_histogram));
      check(bad.length === 0, `sweep: all ${all.length} areas load and draw their expected draws with 0 rejected`, bad.slice(0, 5).join('; '));
      check(grew.length === 0, 'sweep: no wasm memory growth over every area', grew.slice(0, 5).join('; '));
    }
    netOk(p, 'first visit (OPFS copy)');
    await p.page.close();
  }
  // (5) Revisit: the OPFS copy, cached prefetch sets.
  {
    const p = await open(base + '?size=1280x720&area=lev1/vllg', () => {
      // I1 probe: GPUTextures/GPUBuffers created minus explicitly destroyed (a GC'd wrapper never counts as freed).
      const live = { textures: 0, buffers: 0, texture_bytes: 0 };
      window.__gpuLive = live;
      const D = GPUDevice.prototype, ct = D.createTexture, cb = D.createBuffer;
      const bytesOf = (d) => { const s = d.size; const w = s.width ?? s[0], h = s.height ?? s[1] ?? 1; return w * h * 4 * ((d.mipLevelCount ?? 1) > 1 ? 4 / 3 : 1); };
      D.createTexture = function (d) { const t = ct.call(this, d); t.__lomnBytes = bytesOf(d); live.textures++; live.texture_bytes += t.__lomnBytes; return t; };
      D.createBuffer = function (d) { live.buffers++; return cb.call(this, d); };
      const td = GPUTexture.prototype.destroy, bd = GPUBuffer.prototype.destroy;
      GPUTexture.prototype.destroy = function () { if (!this.__lomnGone) { this.__lomnGone = 1; live.textures--; live.texture_bytes -= this.__lomnBytes ?? 0; } return td.call(this); };
      GPUBuffer.prototype.destroy = function () { if (!this.__lomnGone) { this.__lomnGone = 1; live.buffers--; } return bd.call(this); };
    });
    const st = await waitState(p.page, ['area-ready'], 120000);
    const info = await p.page.evaluate(() => ({ source: window.__lomn.source, copy: window.__lomn.copy, load: window.__lomn.load, boot: window.__lomn.boot }));
    check(st.state === 'area-ready' && info.source.kind === 'opfs' && info.source.how === 'revisit' && !info.copy, 'revisit: no picker, no copy', JSON.stringify(info.source));
    summary.revisit_boot = info.boot;
    for (const a of AREAS) {
      const load = a === 'lev1/vllg' ? info.load : await show(p.page, a);
      check(load.cached && load.rounds === 1, `revisit ${a}: cached prefetch set, 1 round`, `rounds ${load.rounds}`);
      summary.areas[a].revisit = { rounds: load.rounds, bytes: load.bytes, fetch_ms: load.fetch_ms, total_ms: load.load_total_ms };
      const img = await shot(p.page, `revisit_${a.replace('/', '_')}.png`);
      compare(img, refs[a], `${a} (revisit)`);
      const n = diffRgb(img, kept['first ' + a]);  // null control: same build, same area, two fresh pages
      (gate.null ??= []).push({ area: a, differing: n.differing, max: n.max });
      check(n.sameSize && n.max <= tolerance, `pixel gate null: ${a} first visit vs revisit page`, `${n.differing} differ, max delta ${n.max}`);
    }
    // I1: 30 switches between lev1/vllg and lev0/frnt (the largest texture set): GPU objects not explicitly destroyed
    // pile up until garbage collection; with explicit Destroy the live count is the same every time vllg is shown.
    const seen = [];
    for (let i = 0; i < 30; ++i) {
      const a = i % 2 ? 'lev1/vllg' : 'lev0/frnt';
      await show(p.page, a);
      if (a === 'lev1/vllg') seen.push(await p.page.evaluate(() => ({ ...window.__gpuLive })));
    }
    summary.i1_gpu_live_vllg = seen;
    check(seen.every((x) => x.textures === seen[0].textures && x.buffers === seen[0].buffers),
          'I1: live GPUTexture/GPUBuffer counts flat over 30 vllg<->frnt switches',
          `at vllg: first ${JSON.stringify(seen[0])}, last ${JSON.stringify(seen[seen.length - 1])}`);
    netOk(p, 'revisit');
    await p.page.close();
  }
  // (6) Handle path: the OPFS copy's directory handle adopted as a picked handle (read in place), then a revisit.
  {
    const p = await open(base + '?size=1280x720&area=lev2/ghut&pick=1');
    await waitState(p.page, ['need-files']);
    await p.page.evaluate(async () => window.__lomn.useDirectoryHandle(await (await navigator.storage.getDirectory()).getDirectoryHandle('lomn-install')));
    const st = await waitState(p.page, ['area-ready'], 120000);
    const src = await p.page.evaluate(() => window.__lomn.source);
    check(st.state === 'area-ready' && src.kind === 'handle', 'handle path: ghut from a directory handle (no copy)', JSON.stringify(src));
    compare(await shot(p.page, 'handle_lev2_ghut.png'), refs['lev2/ghut'], 'lev2/ghut (handle path)');
    await p.page.close();
    const q = await open(base + '?size=1280x720&area=lev4/crss');
    await waitState(q.page, ['area-ready'], 120000);
    const src2 = await q.page.evaluate(() => window.__lomn.source);
    check(src2.kind === 'handle' && src2.how === 'revisit', 'handle revisit from IndexedDB', JSON.stringify(src2));
    compare(await shot(q.page, 'handle_revisit_lev4_crss.png'), refs['lev4/crss'], 'lev4/crss (handle revisit)');
    netOk(q, 'handle revisit');
    await q.page.close();
  }
  // (6b) Pixel gate positive control: lev1/vllg with fog off (debug switch) vs the native faithful render.
  {
    const p = await open(base + '?size=1280x720&area=lev1/vllg&debug=1&fog=off');
    await waitState(p.page, ['area-ready'], 120000);
    await waitFrames(p.page, 3);
    const d = diffRgb(await shot(p.page, 'positive_fog_off_lev1_vllg.png'), refs['lev1/vllg']);
    gate.positive = { what: 'lev1/vllg fog off vs native', differing: d.differing, max: d.max, over_tolerance: d.over(tolerance) };
    check(d.sameSize && d.over(tolerance) > 10000, 'pixel gate positive control: vllg with fog off exceeds the tolerance', `${d.over(tolerance)} pixels over ${tolerance}, max delta ${d.max}`);
    await p.page.close();
  }
  summary.pixel_gate = gate;
  // (7) fps with reconciliation per area (?hud=1; the HUD overlays the canvas, so no pixel compare here).
  {
    const p = await open(base + '?size=1280x720&hud=1&area=lev1/vllg');
    await waitState(p.page, ['area-ready'], 120000);
    for (const a of AREAS) {
      if (a !== 'lev1/vllg') await show(p.page, a);
      const la = loadavg();
      await waitFrames(p.page, fpsFrames + 1, fpsFrames * 50 + 30000);
      const m = await metrics(p.page);
      const f = m.fps;
      check(f.dt_samples === f.frames_in_window - 1, `${a} fps reconciliation: dt samples == frames - 1`,
            `${f.frames_in_window} frames in ${f.window_ms} ms; expected ${f.expected_intervals.toFixed(1)} intervals at ${f.nominal_hz} Hz; p50 ${f.p50_ms} p95 ${f.p95_ms}`);
      summary.areas[a].fps = { ...f, breakdown: m.breakdown_ms, loadavg_before: la, loadavg_after: loadavg() };
      await p.page.screenshot({ path: path.join(work, 'shots', `hud_${a.replace('/', '_')}.png`) });
    }
    netOk(p, 'HUD');
    await p.page.close();
  }
  for (const w of watchers) {
    const gpu = w.console.filter((t) => t.includes('[webgpu]') || t.includes('[area]'));
    if (gpu.length || w.errors.length) check(false, 'no WebGPU errors / page exceptions', JSON.stringify({ gpu: gpu.slice(0, 5), errors: w.errors.slice(0, 5) }));
  }
  check(watchers.every((w) => !w.errors.length), 'no page exceptions');
} catch (e) {
  check(false, 'test run', e.stack || String(e));
} finally {
  // Remove the 1 GB OPFS copy and the handle, then the profile (game data never stays under build/).
  try {
    const p = await ctx.newPage();
    await p.goto(srv.url + 'index.html?pick=1');
    await p.evaluate(() => window.__lomn.forget());
  } catch { /* the profile is deleted below anyway */ }
  await ctx.close();
  srv.close();
  fs.rmSync(path.join(work, 'profile'), { recursive: true, force: true });
}
summary.loadavg_end = loadavg();
fs.writeFileSync(path.join(work, 'results.json'), JSON.stringify({ results, summary }, null, 1));
const failed = results.filter((r) => !r.ok);
console.log(`a13_real: ${results.length - failed.length}/${results.length} checks passed`);
process.exitCode = failed.length ? 1 : 0;
