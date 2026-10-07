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
