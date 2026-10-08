// Node tests of the pure verdicts in a13_checks.mjs (T8 P12-P14): each is shown to pass the real-looking input and to fail
// the input that used to slip through. Run: cd web && timeout 60 node --test tests/a13_checks_test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { fpsVerdict, gpuLiveVerdict, overFetchVerdict, positiveControlVerdict } from './a13_checks.mjs';

// An unspun 600-frame window at a measured 60 Hz display (what a13_real reads from window.__lomn.metrics.fps).
const unspun = (over = {}) => ({ dt_samples: 600, frames_in_window: 601, window_ms: 10005, display_hz: 60, expected_intervals: 600.3, fps: 59.97, dropping: false, ...over });
// The same page with a spin of 2 display periods per frame: ~20 fps, 1/3 of the expected intervals.
const spun = (over = {}) => ({ dt_samples: 600, frames_in_window: 601, window_ms: 30000, display_hz: 60, expected_intervals: 1800, fps: 20, dropping: true, ...over });

test('fpsVerdict: a clean unspun run passes, its ratio is reported', () => {
  const v = fpsVerdict(unspun());
  assert.equal(v.ok, true, v.reasons.join('; '));
  assert.ok(v.ratio > 0.99 && v.ratio < 1.01);
});

test('fpsVerdict: the old check (dt_samples === frames_in_window - 1) holds for a spun run too; this one fails it', () => {
  const s = spun();
  assert.equal(s.dt_samples, s.frames_in_window - 1, 'the old assertion is true by construction');
  const v = fpsVerdict(s);
  assert.equal(v.ok, false);
  assert.ok(v.ratio < 0.4);
  assert.match(v.reasons.join(';'), /counted 600 of 1800\.0 expected intervals/);
  assert.match(v.reasons.join(';'), /dropping = true/);
});

test('fpsVerdict: the ratio threshold is 0.95 on its own, independent of the engine flag', () => {
  // 0.94 of the expected intervals with the engine flag (wrongly) false: still a failure by the counted/expected rule
  const v94 = fpsVerdict(unspun({ dt_samples: 564, window_ms: 10000, expected_intervals: 600 }));
  assert.equal(v94.ok, false);
  assert.match(v94.reasons.join(';'), /< 0\.95/);
  assert.equal(fpsVerdict(unspun({ dt_samples: 570, window_ms: 10000, expected_intervals: 600 })).ok, true);  // exactly 0.95
  // and the flag on its own: ratio fine but dropping true
  assert.equal(fpsVerdict(unspun({ dropping: true })).ok, false);
});

test('fpsVerdict: unmeasured display rate, too few samples, an expectation that does not reconcile, or garbage all fail', () => {
  assert.equal(fpsVerdict(unspun({ display_hz: 0, expected_intervals: 0 })).ok, false);
  assert.equal(fpsVerdict(unspun({ dt_samples: 12, window_ms: 200, expected_intervals: 12 })).ok, false);
  const bad = fpsVerdict(unspun({ expected_intervals: 300 }));  // 300 expected for a 10 s window at 60 Hz: the engine number is not window x rate
  assert.equal(bad.ok, false);
  assert.match(bad.reasons.join(';'), /!= window/);
  assert.equal(fpsVerdict(unspun({ dt_samples: NaN })).ok, false);
  assert.equal(fpsVerdict(undefined).ok, false);
  assert.equal(fpsVerdict(unspun({ dropping: undefined })).ok, false);
});

test('gpuLiveVerdict: flat non-zero counts pass; flat ZEROS (a probe that saw nothing) fail; drift fails (P13)', () => {
  const flat = Array.from({ length: 15 }, () => ({ textures: 29, buffers: 41, texture_bytes: 1000 }));
  assert.equal(gpuLiveVerdict(flat).ok, true);
  const zeros = Array.from({ length: 15 }, () => ({ textures: 0, buffers: 0, texture_bytes: 0 }));
  const z = gpuLiveVerdict(zeros);
  assert.equal(z.ok, false);
  assert.match(z.reasons.join(';'), /vacuous/);
  assert.equal(gpuLiveVerdict(flat.map((x) => ({ ...x, buffers: 0 }))).ok, false);   // textures counted, buffers hook dead
  assert.equal(gpuLiveVerdict(flat.map((x) => ({ ...x, textures: 0 }))).ok, false);  // buffers counted, textures hook dead
  const drift = flat.map((x, i) => ({ ...x, textures: 29 + i }));                     // the pre-fix leak: grows by the area's objects
  assert.equal(gpuLiveVerdict(drift).ok, false);
  assert.equal(gpuLiveVerdict([]).ok, false);
  assert.equal(gpuLiveVerdict([{ textures: 3, buffers: 3 }]).ok, false);
  assert.equal(gpuLiveVerdict([{ textures: undefined, buffers: 3 }, { textures: undefined, buffers: 3 }]).ok, false);
});

test('positiveControlVerdict: the floor is 100,000 pixels (the measured vllg value is 155,532; the old floor 10,000 passed far too little)', () => {
  assert.equal(positiveControlVerdict(155532).ok, true);
  assert.equal(positiveControlVerdict(100000).ok, true);
  assert.equal(positiveControlVerdict(99999).ok, false);
  assert.equal(positiveControlVerdict(10001).ok, false);  // passed the old > 10000 floor
  assert.equal(positiveControlVerdict(155532, { sameSize: false }).ok, false);
  assert.equal(positiveControlVerdict(NaN).ok, false);
});

test('overFetchVerdict: every sweep row must be exactly 0 B over-fetch', () => {
  const rows = [{ area: 'lev1/vllg', over_fetch: 0 }, { area: 'lev2/ghut', over_fetch: 0 }];
  assert.equal(overFetchVerdict(rows).ok, true);
  const one = overFetchVerdict([...rows, { area: 'lev4/crss', over_fetch: 4096 }]);
  assert.equal(one.ok, false);
  assert.deepEqual(one.offenders, ['lev4/crss: 4096']);
  assert.equal(overFetchVerdict([{ area: 'x', over_fetch: undefined }]).ok, false);  // a missing field is not "0 B"
  assert.equal(overFetchVerdict([{ area: 'x', over_fetch: null }]).ok, false);
  assert.equal(overFetchVerdict([]).ok, false);
});

import { dropSamplesVerdict } from './a13_checks.mjs';
const page = (fps, extra = {}) => ({ url: 'index.html?area=levt/test', dropping: fps < 57, fps, hz: 60, snapped: true, loadavg: '20.1 18.0 15.2', ...extra });

test('dropSamplesVerdict (P16): 12 clean pages pass', () => {
  assert.equal(dropSamplesVerdict(Array.from({ length: 12 }, () => page(59.9))).ok, true);
});

test('dropSamplesVerdict (P16): a page that read DROPPING under load passes only if its one re-measure is clean; both readings are kept', () => {
  const ok = Array.from({ length: 11 }, () => page(59.9));
  const flaky = page(52.7, { remeasure: page(59.8) });  // the observed load flake: 52.7 fps, clean on the second reading
  const v = dropSamplesVerdict([...ok, flaky]);
  assert.equal(v.ok, true, JSON.stringify(v));
  assert.equal(v.remeasured.length, 1);
  assert.equal(v.remeasured[0].fps, 52.7);
  assert.equal(v.remeasured[0].remeasure.fps, 59.8);
});

test('dropSamplesVerdict (P16): it still bites - DROPPING twice fails, DROPPING with no re-measure fails, and forcing dropping=true on every page fails', () => {
  const ok = Array.from({ length: 11 }, () => page(59.9));
  assert.equal(dropSamplesVerdict([...ok, page(52.7, { remeasure: page(53.2) })]).ok, false);
  assert.equal(dropSamplesVerdict([...ok, page(52.7)]).ok, false);
  // the scratch mutation "dropping = true on every page" (first and second readings both flagged, fps unchanged or low)
  const forced = Array.from({ length: 12 }, () => page(59.9, { dropping: true, remeasure: page(59.9, { dropping: true }) }));
  const v = dropSamplesVerdict(forced);
  assert.equal(v.ok, false);
  assert.equal(v.wrong.length, 12);
  assert.equal(dropSamplesVerdict(Array.from({ length: 12 }, () => page(10, { dropping: true, remeasure: page(10, { dropping: true }) }))).ok, false);
});

test('dropSamplesVerdict (P16): an engine flag that disagrees with fps < 0.95 x hz fails on either reading; too few samples fail', () => {
  const ok = Array.from({ length: 11 }, () => page(59.9));
  assert.equal(dropSamplesVerdict([...ok, page(59.9, { dropping: true })]).ok, false);  // flag true, fps fine (and no re-measure)
  assert.equal(dropSamplesVerdict([...ok, page(52.7, { remeasure: page(59.8, { dropping: true }) })]).ok, false);  // second reading incoherent
  assert.equal(dropSamplesVerdict(ok.slice(0, 7)).ok, false);
});

// ---- M1 close-out C-m3: the upper bound -----------------------------------------------------------------------------------------
// A display rate measured too LOW (30 Hz on a 60 Hz display) makes the expectation too small: a page at 40 fps then scores 1.33 and the
// lower bound alone passes it. Above 1.05 the display was under-measured: the verdict fails and says so (underMeasured), and the
// caller re-measures the display once (fpsWithRemeasure below).
test('fpsVerdict (C-m3): 40 fps against a display measured as 30 Hz (a 60 Hz display under-measured) does NOT pass', () => {
  const f = unspun({ dt_samples: 400, frames_in_window: 401, window_ms: 10000, display_hz: 30, expected_intervals: 300, fps: 40, dropping: false });
  const v = fpsVerdict(f);
  assert.ok(Math.abs(v.ratio - 400 / 300) < 1e-9);
  assert.equal(v.ok, false);
  assert.equal(v.underMeasured, true);
  assert.match(v.reasons.join(';'), /under-measured/);
});
test('fpsVerdict (C-m3): the bound is 1.05 inclusive; a low ratio is not "under-measured"', () => {
  assert.equal(fpsVerdict(unspun({ dt_samples: 630, window_ms: 10000, expected_intervals: 600 })).ok, true);   // exactly 1.05
  const over = fpsVerdict(unspun({ dt_samples: 636, window_ms: 10000, expected_intervals: 600 }));             // 1.06
  assert.equal(over.ok, false);
  assert.equal(over.underMeasured, true);
  const low = fpsVerdict(unspun({ dt_samples: 564, window_ms: 10000, expected_intervals: 600 }));
  assert.equal(low.ok, false);
  assert.notEqual(low.underMeasured, true);
  assert.notEqual(fpsVerdict(unspun()).underMeasured, true);
});
test('dropSamplesVerdict (C-m3): a page above 1.05 x its measured rate is re-measured once and passes only if the second reading is in range', () => {
  const ok = Array.from({ length: 11 }, () => page(59.9));
  const high = (extra = {}) => page(40, { dropping: false, hz: 30, ...extra });   // 60 Hz display measured as 30
  assert.equal(dropSamplesVerdict([...ok, high()]).ok, false);                       // no second reading
  assert.equal(dropSamplesVerdict([...ok, high({ remeasure: page(59.9) })]).ok, true);
  assert.equal(dropSamplesVerdict([...ok, high({ remeasure: high() })]).ok, false);  // still above 1.05
  assert.equal(dropSamplesVerdict([...ok, high({ remeasure: high() })]).wrong.length, 1);
});

// ---- M1 close-out C-m4: the re-measure-once logic a13_real.mjs and byo_synthetic.mjs share --------------------------------------
import { fpsWithRemeasure } from './a13_checks.mjs';
// A scripted page: read() returns the next scripted metrics.fps; the calls are recorded.
const script = (readings) => {
  const calls = [];
  let i = 0;
  return {
    calls,
    read: async () => { calls.push('read'); return { fps: readings[Math.min(i++, readings.length - 1)] }; },
    freshWindow: async () => { calls.push('fresh'); },
    remeasureDisplay: async () => { calls.push('display'); },
  };
};
test('fpsWithRemeasure: a clean first reading is the result; nothing is re-measured', async () => {
  const s = script([unspun()]);
  const r = await fpsWithRemeasure(s);
  assert.equal(r.v.ok, true);
  assert.equal(r.remeasure, null);
  assert.deepEqual(s.calls, ['read']);
});
test('fpsWithRemeasure: a load-flagged first reading gets exactly ONE fresh window and a second reading, which decides', async () => {
  const s = script([spun(), unspun()]);
  const r = await fpsWithRemeasure({ ...s, stamp: () => ({ loadavg: '21.0 18.0 15.0' }) });
  assert.equal(r.v.ok, true);
  assert.equal(r.remeasure.first.loadavg, '21.0 18.0 15.0');
  assert.ok(r.remeasure.first.reasons.length > 0);
  assert.equal(r.remeasure.first.ratio < 0.4, true);
  assert.deepEqual(s.calls, ['read', 'fresh', 'read']);
});
test('fpsWithRemeasure: it bites - failing twice is a failure after exactly 2 reads (never "until it passes")', async () => {
  const s = script([spun(), spun(), unspun(), unspun()]);  // a third reading would pass: the helper must not take it
  const r = await fpsWithRemeasure(s);
  assert.equal(r.v.ok, false);
  assert.deepEqual(s.calls, ['read', 'fresh', 'read']);
});
test('fpsWithRemeasure: an under-measured display re-measures the DISPLAY first, then a fresh window; still above 1.05 fails', async () => {
  const under = unspun({ dt_samples: 400, window_ms: 10000, display_hz: 30, expected_intervals: 300, fps: 40 });
  const s = script([under, unspun()]);
  const r = await fpsWithRemeasure(s);
  assert.equal(r.v.ok, true);
  assert.deepEqual(s.calls, ['read', 'display', 'fresh', 'read']);
  const s2 = script([under, under]);
  const r2 = await fpsWithRemeasure(s2);
  assert.equal(r2.v.ok, false);
  assert.deepEqual(s2.calls, ['read', 'display', 'fresh', 'read']);
  // a plain low reading does not touch the display measurement
  const s3 = script([spun(), unspun()]);
  await fpsWithRemeasure(s3);
  assert.ok(!s3.calls.includes('display'));
});
