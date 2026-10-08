// LOMN Web viewer page: bring-your-own files -> validation -> (copy) -> engine -> area viewer.
// window.__lomn: {state, message, error, build, source, areas, metrics (getter), load, boot, copy, showArea(), ...}.
// States: boot | need-files | need-permission | validating | rejected | quota-short | copying | starting | ready |
//         loading-area | area-ready | error. Tests wait on these (never on time).
import { checkBuilds } from './fingerprint.js';
import { copyToOpfs, describeWalkFailure, filesSource, handleSource, opfsClear, opfsCopy, opfsDirExists, quotaCheck, sameFiles, totalBytes, validate, WalkError } from './byo.js';
import { createEngine } from './engine.js';
import { idbDelete, idbGet, idbSet } from './idb.js';

const $ = (id) => document.getElementById(id);
const params = new URLSearchParams(location.search);
const L = (window.__lomn = window.__lomn || {});
let engine = null;
let builds = null;
let busy = false;
let starting = false;  // M4: one engine per page (a second pick while one starts is refused)
let dead = false;      // the session ended (fail(): an error, a lost GPU device, no WebGPU): nothing that starts work may run again
// M13: unfaithful or test-only switches need ?debug=1, so a pasted URL never silently renders differently.
const debug = params.get('debug') === '1';

function setState(state, message = '') {
  L.state = state;
  L.message = message;
  $('status').textContent = state;
  $('message').textContent = message;
  $('message').dataset.state = state;
}
// Any failure that ends the session: the state, the reason, and a Reload button (nothing else can recover the page). Final review
// I-1: it also disables every control that starts work (a lost device still hands out GPU objects, so a later area switch would
// "succeed" on a dead loop and flip the page back to area-ready) and shows the folder choice no more.
function fail(err, { reload = true } = {}) {
  dead = true;
  L.error = String(err?.message ?? err);
  $('loading').hidden = true;
  $('reload').hidden = !reload;
  $('byo').hidden = true;
  for (const id of ['area-picker', 'reset-camera', 'pick-dir', 'regrant']) $(id).disabled = true;
  setState('error', L.error);
}
// An extra line in the persistent warning box (the box may already hold the unknown-data warning).
function addWarning(text) {
  const w = $('warning');
  w.textContent = w.hidden || !w.textContent ? text : `${w.textContent} ${text}`;
  w.hidden = false;
}
const pickerMessage = 'Choose your BIONICLE: The Legend of Mata Nui (Beta 1.5.0) folder. Nothing is uploaded: the files are read on this computer.';
// I3/I6/I7: back to the folder choice with a reason (the picker button shown where the browser has one).
function needFiles(reason) {
  $('byo').hidden = false;
  $('viewer').hidden = true;
  if ('showDirectoryPicker' in window && params.get('fs') !== 'opfs') $('pick-dir').hidden = false;
  setState('need-files', reason ? `${reason} ${pickerMessage}` : pickerMessage);
}
const mb = (n) => (n / 1048576).toFixed(1) + ' MiB';

// Canvas: ?size=WxH fixes the drawing buffer and CSS size (tests compare pixels at 1280x720); otherwise the canvas takes
// its container's width (min(100%, 1280px)) and a 16:9 height rounded to a whole CSS pixel, and the drawing buffer is that
// size x devicePixelRatio (ResizeObserver -> lomn_resize: surface + depth buffer recreated).
function canvasSize() {
  const m = /^(\d{2,4})x(\d{2,4})$/.exec(params.get('size') || '');
  if (m) return { w: +m[1], h: +m[2], fixed: true };
  const c = $('canvas');
  const dpr = window.devicePixelRatio || 1;
  const cssW = Math.max(1, Math.round(c.parentElement.clientWidth)), cssH = Math.max(1, Math.round((cssW * 9) / 16));
  return { w: Math.round(cssW * dpr), h: Math.round(cssH * dpr), cssW, cssH, fixed: false };
}

async function start() {
  try {
    // Final review I-3: before ANY validation or copy work. A browser without WebGPU would otherwise copy ~1.1 GB into browser
    // storage and only then fail at the first GPU call. The message is final (a reload cannot change it), so no Reload button.
    if (!navigator.gpu) {
      L.noWebGpu = true;
      return fail('This browser has no WebGPU, so the viewer cannot run here. Use Chrome or Edge 113 or newer (Safari and Firefox support is planned).', { reload: false });
    }
    if (params.get('scene') === 'triangle') return startTriangle();
    setState('boot');
    const res = await fetch('builds.json');
    builds = checkBuilds(await res.json());
    if ('showDirectoryPicker' in window && params.get('fs') !== 'opfs') $('pick-dir').hidden = false;
    // ?pick=1: choose a (different) folder even when one is remembered.
    if (params.get('pick') === '1') return setState('need-files', 'Choose the game folder.');
    // Revisit 1: a folder handle the player picked earlier (Chromium).
    const handle = await idbGet('install-handle').catch(() => null);
    if (handle && params.get('fs') !== 'opfs') {
      // I3: a remembered folder that was moved or deleted is forgotten and the folder asked for again (never a dead end).
      const revisit = async () => {
        let src;
        try {
          src = await handleSource(handle, 'handle');
        } catch (err) {
          await idbDelete('install-handle').catch(() => {});
          L.forgottenHandle = String(err?.message ?? err);
          return needFiles(describeWalkFailure(handle.name, err));
        }
        return useSource(src, 'revisit');
      };
      const perm = await handle.queryPermission({ mode: 'read' });
      if (perm === 'granted') return await revisit();  // `await`: a rejection must reach this try/catch, not escape it
      $('regrant').hidden = false;
      $('regrant').textContent = `Allow access to "${handle.name}" again`;
      $('regrant').onclick = async () => {
        let granted;
        if (dead) return;
        try {
          granted = (await handle.requestPermission({ mode: 'read' })) === 'granted';
        } catch (err) {
          // requestPermission itself can reject (no user activation, a handle the browser no longer honours): say so, keep the button.
          L.permissionError = String(err?.message ?? err);
          return setState('need-permission', `Permission could not be requested (${L.permissionError}). Click the button again, or choose the folder again.`);
        }
        if (granted) {
          $('regrant').hidden = true;
          await revisit().catch(fail);
        } else {
          setState('need-permission', 'Permission was not granted. Choose the folder again.');
        }
      };
      return setState('need-permission', `The game folder "${handle.name}" needs your permission again.`);
    }
    // Revisit 2: a finished OPFS copy (Safari/Firefox path), checked against its completion manifest (I6).
    const copy = await opfsCopy();
    if (copy) {
      let src, why = '';
      try {
        src = await handleSource(copy.dir, 'opfs');
        why = sameFiles(src.entries, copy.done.files);
      } catch (err) {
        why = String(err?.message ?? err);
      }
      if (!why) return await useSource(src, 'revisit');  // `await`, as above
      await opfsClear();
      L.copyProblem = why;
      return needFiles(`The browser copy of the game files was incomplete (${why}) and was removed.`);
    }
    // I7: a copy directory without its completion manifest is an interrupted copy: free its space now.
    if (await opfsDirExists()) {
      await opfsClear();
      return needFiles('An incomplete copy (interrupted before it finished) was removed.');
    }
    needFiles('');
  } catch (err) {
    fail(err);
  }
}

async function onPickDir() {
  if (dead) return;
  try {
    const handle = await window.showDirectoryPicker({ id: 'lomn-install', mode: 'read' });
    await adoptHandle(handle);
  } catch (err) {
    if (err instanceof WalkError) setState('rejected', `This folder is not a usable game folder: ${err.message}.`);  // a cap, not a crash
    else if (err?.name !== 'AbortError') fail(err);
  }
}
// The Chromium path: the handle is read in place (no copy) and persisted for the next visit.
async function adoptHandle(handle) {
  const src = await handleSource(handle, 'handle');
  const ok = await useSource(src, 'picked');
  if (ok) {
    // Final review m-3: the area is already starting; a failing IndexedDB write (quota, a foreign VersionError) only means the folder
    // is not remembered for the next visit. Never an 'error' on a page that is rendering.
    try {
      await idbSet('install-handle', handle);
    } catch (err) {
      L.handleNotSaved = String(err?.message ?? err);
      addWarning(`The game folder could not be saved in the browser (${L.handleNotSaved}): it will not be remembered, and you will be asked for it again next time.`);
    }
  }
  return ok;
}

// Safari/Firefox path (also used by Chromium when the folder comes from the input): validate the File objects, check the
// quota, copy into OPFS in a worker, then read from the copy.
async function onInput(ev) {
  if (busy || dead) return;
  busy = true;
  try {
    const src = filesSource(ev.target.files);
    setState('validating', `Checking ${src.entries.length} files…`);
    const v = await validate(src, builds);
    L.validation = v;
    if (!v.ok) return setState('rejected', v.message);
    await opfsClear();
    const need = totalBytes(src);
    const q = await quotaCheck(need);
    L.quota = q;
    if (!q.ok)
      return setState('quota-short', `Not enough browser storage to copy the game files: ${mb(q.need)} needed, ${mb(q.free)} available` +
        ` (persistent storage ${q.persisted ? 'granted' : 'not granted'}). Free some space, or use Chrome or Edge, which read the folder in place without copying.`);
    setState('copying', `Copying ${mb(need)} into browser storage (one time)…`);
    $('copy-progress').hidden = false;
    $('copy-progress').max = need;
    const t0 = performance.now();
    const res = await copyToOpfs(src, v, (done, total) => {
      $('copy-progress').value = done;
      L.copyProgress = { done, total };
    });
    L.copy = { bytes: res.bytes, ms: performance.now() - t0 };
    $('copy-progress').hidden = true;
    const copy = await opfsCopy();
    if (!copy) throw new Error('the copy finished but its completion manifest is missing');
    // The copy holds only what was admitted, so the names skipped while reading the input (final I-2: control characters, long
    // names, case duplicates) are carried over to the source the engine starts from.
    const copied = await handleSource(copy.dir, 'opfs');
    copied.warnings = [...(src.warnings ?? []), ...copied.warnings].slice(0, 100);
    await useSource(copied, 'copied');
  } catch (err) {
    if (err instanceof WalkError) setState('rejected', `This folder is not a usable game folder: ${err.message}.`);
    else fail(err);
  } finally {
    busy = false;
    ev.target.value = '';
  }
}

async function useSource(src, how) {
  if (starting || engine) return false;  // M4: a second pick never starts a second engine on the same canvas
  starting = true;
  try {
    return await startWith(src, how);
  } finally {
    if (!engine) starting = false;
  }
}

async function startWith(src, how) {
  setState('validating', `Checking ${src.entries.length} files…`);
  const v = await validate(src, builds);
  L.validation = v;
  L.sourceWarnings = src.warnings ?? [];
  if (!v.ok) {
    setState('rejected', v.message);
    return false;
  }
  // C2 (D13): accepted exe + unknown tables: shown, with a persistent warning; the reason also goes into the metrics.
  if (v.warning) {
    $('warning').hidden = false;
    $('warning').textContent = v.warning;
  }
  L.build = v.build;
  L.source = { kind: src.kind, how, name: src.name, files: src.entries.length, bytes: totalBytes(src) };
  $('byo').hidden = true;
  $('viewer').hidden = false;
  setState('starting', `${v.message}. Starting the engine…`);
  const size = canvasSize();
  applyCanvasSize(size);
  engine = await createEngine(src, onEngineState);
  L.engine = engine;
  engine.validation = { build: v.build, data: v.data, warning: v.warning ?? null, source_warnings: (src.warnings ?? []).length };
  if (debug) {
    // Final review m-1: a page in a debug mode says so (a pasted URL must not silently render unfaithfully, HUD or not).
    const on = [];
    if (params.get('fog') === 'off') on.push('fog off');
    if (Number(params.get('debug_spin') || 0) > 0) on.push(`spin ${Number(params.get('debug_spin'))} ms`);
    if (params.get('fetch_concurrency')) on.push(`fetch concurrency ${Math.max(1, Math.min(16, Number(params.get('fetch_concurrency'))))}`);
    if (params.get('nocache') === '1') on.push('prefetch cache off');
    $('debug-badge').textContent = on.length ? `debug: ${on.join(', ')}` : 'debug';
    $('debug-badge').hidden = false;
    // ?debug=1&fog=off: the pixel gate's positive control. ?debug_spin=MS: busy-wait per frame (I2). ?fetch_concurrency=N,
    // ?nocache=1: prefetch measurement switches (M1).
    if (params.get('fog') === 'off') engine.M._lomn_set_fog(0);
    const spin = Number(params.get('debug_spin') || 0);
    if (spin > 0) engine.M._lomn_debug_spin(spin);
    engine.concurrency = Math.max(1, Math.min(16, Number(params.get('fetch_concurrency') || 1)));
    if (params.get('nocache') === '1') engine.useCache = false;
  }
  await engine.bootAndMain(size.w, size.h, `prefetch|${v.dataFp}|${engine.wasmSha}|`);
  L.boot = engine.boot;
  return true;
}

function applyCanvasSize(size) {
  const c = $('canvas');
  if (size.fixed) {
    c.parentElement.style.width = size.w + 'px';
    c.style.width = size.w + 'px';
    c.style.height = size.h + 'px';
  } else {
    c.style.height = size.cssH + 'px';  // whole CSS pixels (the 16:9 aspect-ratio alone can give x.5)
  }
  c.width = size.w;
  c.height = size.h;
}

function onEngineState(state, detail) {
  if (state === 'error') {
    // C1: a lost GPU device (or the engine's first uncaptured GPU error) ends the session: say so, offer a reload.
    L.lost = detail;
    $('loading').hidden = true;
    $('reload').hidden = false;
    return fail(new Error(detail));
  }
  if (state !== 'ready') return;
  L.adapter = detail;
  L.metricsAtReady = engine.metrics();  // the heap after the boot set, before any area (memory budget)
  L.areas = engine.areas();
  const pick = $('area-picker');
  pick.textContent = '';
  for (const a of L.areas) pick.add(new Option(`${a.level}/${a.area}`, `${a.level}/${a.area}`));
  pick.onchange = () => showArea(pick.value);
  if (!canvasSize().fixed) new ResizeObserver(() => {
    const s = canvasSize();
    $('canvas').style.height = s.cssH + 'px';
    if (s.w !== $('canvas').width || s.h !== $('canvas').height) engine.M._lomn_resize(s.w, s.h);
  }).observe($('canvas').parentElement);
  setState('ready', `${L.areas.length} areas. Pick one.`);
  startInput();
  if (params.get('hud') === '1') startHud();
  const first = params.get('area') || (L.areas[0] ? `${L.areas[0].level}/${L.areas[0].area}` : '');
  // I2: the engine measures the display rate from 130 empty frames before the first area (about 2.2 s at 60 Hz); the
  // first area waits for it (at most 6 s; without it the HUD says the rate is not measured).
  const t0 = performance.now();
  const wait = () => {
    if (engine.metrics().fps.display_hz > 0 || performance.now() - t0 > 6000) {
      if (first) showArea(first);
    } else {
      requestAnimationFrame(wait);
    }
  };
  requestAnimationFrame(wait);
}

// Area switch: the previous area keeps rendering under the loading overlay while the next one is prefetched.
async function showArea(name) {
  if (!engine || busy || dead) return false;  // dead: the device was lost (or the session failed): keep the error state and its reason
  const [level, area] = String(name).split('/');
  if (!L.areas.some((a) => a.level === level && a.area === area)) return false;
  busy = true;
  $('area-picker').value = name;
  $('area-picker').disabled = true;
  $('loading').hidden = false;
  setState('loading-area', `Loading ${name}…`);
  try {
    const load = await engine.showArea(level, area);
    // The device can be lost while the area's files are being read (the page is then in 'error' already): never flip it back.
    if (dead) return false;
    L.load = load;
    setState('area-ready', name);
    return true;
  } catch (err) {
    if (!dead) fail(err);  // after a loss the first reason (the loss) stays
    return false;
  } finally {
    busy = false;
    if (!dead) $('area-picker').disabled = false;
    $('loading').hidden = true;
  }
}

// Free-fly camera input: WASD / arrows move, Q/E down/up, Shift faster; drag (or pointer lock after a click) looks.
function startInput() {
  const c = $('canvas');
  const bits = { KeyW: 1, ArrowUp: 1, KeyS: 2, ArrowDown: 2, KeyA: 4, ArrowLeft: 4, KeyD: 8, ArrowRight: 8, KeyE: 16, KeyQ: 32, ShiftLeft: 64, ShiftRight: 64 };
  let keys = 0, dragging = false;
  const push = (dx = 0, dy = 0) => engine.M._lomn_input(keys, dx, dy);
  // M3: keys move the camera only while the canvas has focus (click it) or the pointer is locked to it; elsewhere the
  // page keeps its normal keyboard behaviour (Tab, Shift+Tab, arrows in the picker).
  const ours = () => document.activeElement === c || document.pointerLockElement === c;
  addEventListener('keydown', (e) => {
    if (!ours() || !(e.code in bits)) return;
    keys |= bits[e.code];
    push();
    e.preventDefault();
  });
  addEventListener('keyup', (e) => {
    if (!(e.code in bits)) return;
    keys &= ~bits[e.code];
    push();
  });
  addEventListener('blur', () => { keys = 0; push(); });
  c.addEventListener('mousedown', () => { dragging = true; c.focus(); });
  addEventListener('mouseup', () => { dragging = false; });
  c.addEventListener('dblclick', () => {
    const r = c.requestPointerLock?.();  // a promise in current Chromium: a refused lock is not an unhandled rejection
    if (r && r.catch) r.catch(() => {});
  });
  addEventListener('mousemove', (e) => {
    if (dragging || document.pointerLockElement === c) push(e.movementX, e.movementY);
  });
  $('reset-camera').onclick = () => engine.M._lomn_camera_reset();
}

function startHud() {
  const hud = $('hud');
  hud.hidden = false;
  const f = (v, d = 1) => (typeof v === 'number' ? v.toFixed(d) : '-');
  setInterval(() => {
    const m = engine.metrics();
    const p = m.fps, b = m.breakdown_ms, h = m.heap, lo = m.live_objects;
    const ld = L.load;
    const bd = m.build;
    hud.textContent =
      `build ${bd.type} [${bd.cxx_flags}] emscripten ${bd.emscripten} wasm ${bd.wasm_sha256.slice(0, 12)}  canvas ${m.canvas.width}x${m.canvas.height} @${m.canvas.device_pixel_ratio}x  ${m.adapter}${m.render.fog ? '' : '  FOG OFF (debug)'}\n` +
      `measured fps ${f(p.fps)} vs display ${p.display_hz > 0 ? f(p.display_hz, p.display_snapped ? 0 : 1) + ' Hz (' + (p.display_snapped ? '' : 'unsnapped, not a standard rate; ') + 'median of ' + p.display_samples + ' frames before the first area)' : 'not measured'}` +
      `${p.dropping ? '  DROPPING' : ''}${p.spin_ms > 0 ? `  (debug spin ${p.spin_ms} ms)` : ''}\n` +
      `frames ${p.frames_in_window} in ${f(p.window_ms / 1000, 2)} s; expected ${f(p.expected_intervals + (p.dt_samples ? 1 : 0))} at ${f(p.nominal_hz, 0)} Hz` +
      `  (dt samples ${p.dt_samples} = frames - 1)  dt p50 ${f(p.p50_ms, 2)} p95 ${f(p.p95_ms, 2)} ms\n` +
      `frame ms: script ${b.script}  physics ${b.physics}  replay p50 ${f(b.replay_p50, 2)}  submit p50 ${f(b.submit_p50, 2)}  cpu p50 ${f(b.frame_cpu_p50, 2)} p95 ${f(b.frame_cpu_p95, 2)}\n` +
      `draws ${m.draws}  pipelines ${m.pipelines}  textures ${m.textures} (${mb(m.texture_bytes)})  rejected ${m.rejected_last_frame}  ` +
      `objects t${lo.textures}/b${lo.buffers}/p${lo.pipelines}/s${lo.samplers}/g${lo.bind_groups}\n` +
      `heap: wasm ${mb(h.wasm_memory_bytes)}  malloc ${mb(h.malloc_in_use_bytes)}  files resident ${mb(h.fs_resident_bytes)} (boot ${mb(h.fs_pinned_bytes)}, peak ${mb(h.fs_peak_bytes)})\n` +
      (ld ? `${ld.area}: ${ld.rounds} rounds${ld.cached ? ' (cached set)' : ''}, ${mb(ld.bytes)} read in ${f(ld.fetch_ms, 0)} ms, total ${f(ld.load_total_ms, 0)} ms` : '');
  }, 250);
}

function startTriangle() {
  setState('starting');
  $('byo').hidden = true;
  $('viewer').hidden = false;
  const c = $('canvas');
  c.className = 'triangle';
  c.width = c.height = 64;
  createEngine(null, (s, d) => {
    if (s === 'ready') setState('running', d);
    else if (s === 'error') fail(new Error(d));
  }).then((e) => {
    engine = e;
    L.engine = e;
    e.M._lomn_configure(64, 64, 1);
    e.M.callMain([]);
  }, fail);
}

// Programmatic entry points (also what the Playwright tests drive; documented in web/tests/README.md).
L.showArea = showArea;
L.useDirectoryHandle = adoptHandle;
L.forget = async () => { await idbDelete('install-handle'); await opfsClear(); };
L.forgetCachedSet = (area) => engine?.forgetCachedSet(area);  // tests: force discovery for one area (I5 sweep)
Object.defineProperty(L, 'metrics', { configurable: true, get: () => (engine && engine.M._lomn_metrics_json ? engine.metrics() : null) });

$('pick-dir').onclick = onPickDir;
$('reload').onclick = () => location.reload();
$('byo-input').onchange = onInput;
start();
