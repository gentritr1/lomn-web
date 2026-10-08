// The pure verdicts behind a13_real.mjs's fps, GPU-object, positive-control and over-fetch checks (T8 P12-P14). They live here,
// free of Playwright, so tests/a13_checks_test.mjs can pin each one with the inputs that used to slip through:
//  - a GPU-object probe that reads zeros is flat, and passed vacuously (P13);
//  - "dt samples == frames - 1" is true by construction (the engine counts frames as samples + 1), so it checked nothing (P12);
//  - the fog-off pixel gate control asserted > 10,000 px where the global constraint says >= 100,000 (P14);
//  - over-fetch was recorded per sweep row but never asserted (P14).

export const FPS_MIN_RATIO = 0.95;          // the HUD rule: counted / expected intervals, DROPPING below it
export const FPS_MAX_RATIO = 1.05;         // C-m3: above this the display rate was measured too low (a 60 Hz display read as 30 Hz): not a pass
export const FPS_MIN_SAMPLES = 30;          // the engine's own floor for flagging DROPPING
export const POSITIVE_CONTROL_MIN_PX = 100000;  // global constraints: the fog-off control must exceed the tolerance by >= 100,000 px

// P12. f = metrics.fps of one page (unspun areas must pass; a page spun on purpose must fail). Counted intervals are
// dt_samples; expected = the window's duration at the display rate MEASURED on that page before its first area
// (expected_intervals). The engine's expected_intervals is recomputed here from window_ms and display_hz, so a reading whose
// window, rate and expectation do not reconcile fails by itself (an unverified sample count is not a measurement).
export function fpsVerdict(f, { minRatio = FPS_MIN_RATIO, maxRatio = FPS_MAX_RATIO, minSamples = FPS_MIN_SAMPLES } = {}) {
  const reasons = [];
  const num = (x) => typeof x === 'number' && Number.isFinite(x);
  if (!f || ![f.dt_samples, f.window_ms, f.display_hz, f.expected_intervals].every(num)) return { ok: false, ratio: NaN, recomputed: NaN, reasons: ['fps metrics missing or not finite'] };
  if (!(f.display_hz > 0)) reasons.push('display rate not measured');
  if (f.dt_samples < minSamples) reasons.push(`only ${f.dt_samples} dt samples (need ${minSamples})`);
  const recomputed = (f.window_ms * f.display_hz) / 1000;
  if (!(recomputed > 0) || Math.abs(recomputed - f.expected_intervals) > 0.01 * recomputed + 0.5)
    reasons.push(`expected intervals ${f.expected_intervals} != window ${f.window_ms} ms x ${f.display_hz} Hz = ${recomputed.toFixed(1)}`);
  const ratio = f.expected_intervals > 0 ? f.dt_samples / f.expected_intervals : NaN;
  if (!(ratio >= minRatio)) reasons.push(`counted ${f.dt_samples} of ${Number(f.expected_intervals).toFixed(1)} expected intervals = ${Number.isNaN(ratio) ? 'n/a' : ratio.toFixed(3)} < ${minRatio}`);
  // C-m3: the expectation is window x the MEASURED display rate, so a rate measured too low (30 Hz on a 60 Hz display) inflates the
  // ratio and used to pass a 40 fps page at 1.33. A ratio above maxRatio is not a pass: the display rate must be measured again.
  const underMeasured = ratio > maxRatio;
  if (underMeasured) reasons.push(`counted ${f.dt_samples} of ${Number(f.expected_intervals).toFixed(1)} expected intervals = ${ratio.toFixed(3)} > ${maxRatio}: the display rate (${f.display_hz} Hz) was under-measured`);
  if (f.dropping !== false) reasons.push(`engine flag dropping = ${f.dropping}`);
  return { ok: reasons.length === 0, ratio, recomputed, reasons, underMeasured };
}

// P13. seen = the GPUTexture/GPUBuffer live counts at each visit of the same area ({textures, buffers}). Flat is only a result
// when the probe saw something: zeros everywhere (a hook that never ran) are flat too.
export function gpuLiveVerdict(seen) {
  const reasons = [];
  if (!Array.isArray(seen) || seen.length < 2) return { ok: false, reasons: ['fewer than 2 samples'] };
  const vals = (x) => Number.isFinite(x?.textures) && Number.isFinite(x?.buffers);
  if (!seen.every(vals)) return { ok: false, reasons: ['a sample has no finite textures / buffers count'] };
  const zero = seen.findIndex((x) => !(x.textures > 0 && x.buffers > 0));
  if (zero >= 0) reasons.push(`sample ${zero} has textures ${seen[zero].textures}, buffers ${seen[zero].buffers}: the probe saw nothing (vacuous)`);
  const drift = seen.findIndex((x) => x.textures !== seen[0].textures || x.buffers !== seen[0].buffers);
  if (drift >= 0) reasons.push(`sample ${drift} (${seen[drift].textures}/${seen[drift].buffers}) differs from the first (${seen[0].textures}/${seen[0].buffers})`);
  return { ok: reasons.length === 0, reasons };
}

// P14. The fog-off positive control: pixels over the tolerance must reach the global-constraint floor.
export function positiveControlVerdict(overTolerance, { sameSize = true, min = POSITIVE_CONTROL_MIN_PX } = {}) {
  return { ok: sameSize === true && Number.isFinite(overTolerance) && overTolerance >= min, min };
}

// P14. Every sweep row discovers its set with no over-fetch (bytes fetched that the engine did not read): exactly 0 B.
export function overFetchVerdict(rows) {
  if (!Array.isArray(rows) || rows.length === 0) return { ok: false, offenders: [], reasons: ['no sweep rows'] };
  const offenders = rows.filter((r) => r.over_fetch !== 0).map((r) => `${r.area}: ${r.over_fetch}`);
  return { ok: offenders.length === 0, offenders, reasons: offenders };
}

// T8 P16. byo_synthetic samples every unspun page as it closes: {url, dropping, fps, hz, snapped, loadavg, remeasure?}. On a
// loaded host an unspun page can read DROPPING (52.7 and 53.2 fps at loadavg 16-25; the minimum in 20 quieter runs was 58.3),
// so a page that reads DROPPING (the engine flag, or fps < 0.95 x its display rate) is re-measured ONCE (`remeasure`, same
// fields, taken on a fresh window) and fails only if the second reading drops again. A flagged page with no second reading
// fails. The engine's flag must agree with fps < 0.95 x hz on every reading, first or second. Both readings are reported.
// C-m3: a reading above 1.05 x hz (the display rate measured too low) is flagged and re-measured the same way.
export function dropSamplesVerdict(samples, { min = 8, minRatio = FPS_MIN_RATIO } = {}) {
  // C-m3: a page above maxRatio x its measured rate had the display under-measured: re-measured once like a low page.
  const low = (r) => r.dropping === true || !(r.fps >= minRatio * r.hz) || r.fps > FPS_MAX_RATIO * r.hz;
  const incoherent = (r) => r.dropping !== (r.fps < minRatio * r.hz);
  const readings = (s) => (s.remeasure ? [s, s.remeasure] : [s]);
  const wrong = samples.filter((s) => low(s.remeasure ?? s) || (low(s) && !s.remeasure));
  const incoh = samples.filter((s) => readings(s).some(incoherent));
  const remeasured = samples.filter((s) => s.remeasure);
  return { ok: samples.length >= min && wrong.length === 0 && incoh.length === 0, wrong, incoherent: incoh, remeasured };
}

// M1 close-out C-m4: the re-measure-once rule a13_real.mjs applies to the fps of each unspun area (and that byo_synthetic.mjs reuses, C-m7),
// as a function with injectable page actions so tests/a13_checks_test.mjs can pin it without a browser: read() -> metrics ({fps}),
// freshWindow() starts a new measurement window (the area shown again), remeasureDisplay() measures the display rate again (the engine's
// own measurement), stamp() adds context (the load average) to the first reading's record. A failing first reading gets exactly ONE
// fresh window (after a display re-measure when the display was under-measured, C-m3) and exactly one more reading, which decides:
// never "until it passes". Returns {m, v, remeasure} with remeasure = null when the first reading passed.
export async function fpsWithRemeasure({ read, freshWindow, remeasureDisplay, stamp = () => ({}), verdict = fpsVerdict }) {
  let m = await read();
  let v = verdict(m.fps);
  let remeasure = null;
  if (!v.ok) {
    remeasure = { first: { fps: m.fps, ratio: v.ratio, reasons: v.reasons, underMeasured: v.underMeasured === true, ...stamp() } };
    if (v.underMeasured) await remeasureDisplay();
    await freshWindow();
    m = await read();
    v = verdict(m.fps);
  }
  return { m, v, remeasure };
}
