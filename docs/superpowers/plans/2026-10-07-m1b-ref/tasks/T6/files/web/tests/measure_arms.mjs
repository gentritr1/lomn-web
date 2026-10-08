// A/B measurement harness (REF4 r2: M10 -O2+wasm-opt vs -O3, M1 fetch concurrency). Needs LOMN_DATA.
// Usage: node tests/measure_arms.mjs <root_dir> <work_dir> '<arms json>' <reps>
//   <root_dir> holds one built site per arm in a subdirectory (same origin, so they share the OPFS copy and IndexedDB);
//   arms = [{"name": "o3", "dir": "site-o3", "query": "fetch_concurrency=1"}, ...].
// One persistent Chromium, the real install copied into OPFS once; then reps x arms interleaved (ABAB...). Each run opens
// a fresh page with ?debug=1&nocache=1 (every area discovered, nothing cached), shows lev1/vllg, lev2/ghut, lev4/crss and
// lev0/frnt, and reads 300 frames on lev4/crss. Records load ms per area, the engine's probe/decode/upload ms, frame CPU
// p50/p95 with the reconciled fps window, wasm bytes, and the host loadavg before and after every run.
// Decision rule (TEST-003): a difference counts only if the arms' medians differ by more than the larger within-arm
// spread (max - min over the reps).
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { serve } from './serve.mjs';
import { launch, waitFrames, waitState } from './lib.mjs';

const [rootArg, workArg, armsJson, repsArg] = process.argv.slice(2);
if (!rootArg || !workArg || !armsJson) {
  console.error("usage: measure_arms.mjs <root_dir> <work_dir> '<arms json>' [reps]");
  process.exit(2);
}
if (!process.env.LOMN_DATA) {
  console.log('SKIPPED (no LOMN_DATA)');
  process.exit(77);
}
const root = path.resolve(rootArg), work = path.resolve(workArg);
const arms = JSON.parse(armsJson);
const reps = Number(repsArg || 4);
const AREAS = ['lev1/vllg', 'lev2/ghut', 'lev4/crss', 'lev0/frnt'];
const loadavg = () => os.loadavg().map((x) => x.toFixed(2)).join(' ');
fs.rmSync(work, { recursive: true, force: true });
fs.mkdirSync(work, { recursive: true });
const srv = await serve(root);
const ctx = await launch(path.join(work, 'profile'));
const runs = [];
try {
  // One OPFS copy of the install, through the first arm's page.
  {
    const page = await ctx.newPage();
    await page.goto(`${srv.url}${arms[0].dir}/index.html?pick=1`);
    await waitState(page, ['need-files']);
    await page.setInputFiles('#byo-input', path.join(process.env.LOMN_DATA, 'install'));
    await page.waitForFunction(() => ['ready', 'area-ready', 'loading-area', 'rejected', 'error'].includes(window.__lomn?.state), null, { timeout: 600000 });
    await page.close();
  }
  for (let rep = 0; rep < reps; ++rep) {
    for (let i = 0; i < arms.length; ++i) {
      const arm = arms[(rep % 2 === 0) ? i : arms.length - 1 - i];  // ABBA order per pair of reps
      const la0 = loadavg();
      const page = await ctx.newPage();
      const wasmBytes = fs.statSync(path.join(root, arm.dir, fs.readdirSync(path.join(root, arm.dir)).find((f) => f.endsWith('.wasm')))).size;
      await page.goto(`${srv.url}${arm.dir}/index.html?debug=1&nocache=1&size=1280x720&area=${AREAS[0]}&${arm.query || ''}`);
      await waitState(page, ['area-ready'], 120000);
      const per = {};
      for (const a of AREAS) {
        if (a !== AREAS[0]) await page.evaluate((n) => window.__lomn.showArea(n), a);
        const m = await page.evaluate(() => ({ load: window.__lomn.load, area: window.__lomn.metrics.area }));
        per[a] = { load_total_ms: m.load.load_total_ms, rounds: m.load.rounds, fetch_ms: m.load.fetch_ms, probe_ms: m.area.probe_ms,
                   decode_ms: m.area.decode_ms, upload_ms: m.area.upload_ms, cached: m.load.cached };
      }
      await page.evaluate(() => window.__lomn.showArea('lev4/crss'));
      await waitFrames(page, 301, 60000);
      const m = await page.evaluate(() => window.__lomn.metrics);
      runs.push({ rep, arm: arm.name, wasm_bytes: wasmBytes, build: m.build, per, frame_cpu_p50: m.breakdown_ms.frame_cpu_p50,
                  frame_cpu_p95: m.breakdown_ms.frame_cpu_p95, replay_p50: m.breakdown_ms.replay_p50,
                  fps: { frames: m.fps.frames_in_window, dt_samples: m.fps.dt_samples, window_ms: m.fps.window_ms, expected: m.fps.expected_intervals, display_hz: m.fps.display_hz, dropping: m.fps.dropping },
                  loadavg_before: la0, loadavg_after: loadavg() });
      console.log(`rep ${rep} ${arm.name}: wasm ${wasmBytes} B, loads ${AREAS.map((a) => per[a].load_total_ms.toFixed(0)).join('/')} ms, cpu p50 ${m.breakdown_ms.frame_cpu_p50} ms, loadavg ${la0}`);
      await page.close();
    }
  }
} finally {
  await ctx.close();
  srv.close();
  fs.rmSync(path.join(work, 'profile'), { recursive: true, force: true });
}
// Summary: per arm, median and spread of the total load over the 4 areas and of frame CPU p50.
const med = (v) => { const s = [...v].sort((a, b) => a - b); return s.length % 2 ? s[(s.length - 1) / 2] : (s[s.length / 2 - 1] + s[s.length / 2]) / 2; };
const summary = {};
for (const arm of arms) {
  const rs = runs.filter((r) => r.arm === arm.name);
  const load = rs.map((r) => AREAS.reduce((n, a) => n + r.per[a].load_total_ms, 0));
  const cpu = rs.map((r) => r.frame_cpu_p50);
  summary[arm.name] = { reps: rs.length, wasm_bytes: rs[0]?.wasm_bytes, load_total_ms: { median: med(load), spread: Math.max(...load) - Math.min(...load), values: load },
                        frame_cpu_p50_ms: { median: med(cpu), spread: Math.max(...cpu) - Math.min(...cpu), values: cpu } };
}
fs.writeFileSync(path.join(work, 'results.json'), JSON.stringify({ arms, runs, summary }, null, 1));
console.log(JSON.stringify(summary, null, 1));
