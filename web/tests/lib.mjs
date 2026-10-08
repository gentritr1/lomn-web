// Shared helpers of the M1b browser tests: one persistent Chromium context per test file (real GPU: channel 'chromium'
// + --enable-unsafe-webgpu; the headless shell's SwiftShader paints blank canvases, so pixels are always checked),
// console capture, state waits (never fixed sleeps), PNG decode + pixel diff, and the network-discipline check.
import { chromium } from 'playwright';
import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';

export async function launch(profileDir) {
  return chromium.launchPersistentContext(path.resolve(profileDir), {
    channel: 'chromium', args: ['--enable-unsafe-webgpu'], timeout: 30000, viewport: { width: 1400, height: 1000 },
  });
}

// Console errors/warnings, page exceptions and every request the page makes (any origin): URL, method and request-body bytes.
// T8 P10: also records the navigations the TEST makes (page.goto, page.reload) in w.expected_navs as "path?query" keys.
// networkCheck treats every other top-level navigation (a window.open popup, a script-driven location change) as a finding.
export const navKey = (u) => { try { const x = new URL(u, 'http://x'); return x.pathname + x.search; } catch { return String(u); } };
export function watch(page) {
  const w = { console: [], errors: [], urls: [], reqs: [], expected_navs: new Set() };
  const goto = page.goto?.bind(page);
  if (goto) page.goto = (u, ...rest) => { w.expected_navs.add(navKey(u)); return goto(u, ...rest); };
  const reload = page.reload?.bind(page);
  if (reload) page.reload = (...rest) => { w.expected_navs.add(navKey(page.url())); return reload(...rest); };
  page.on('console', (m) => {
    if (!['error', 'warning'].includes(m.type())) return;
    if (m.location().url.endsWith('/favicon.ico')) return;  // the test server answers 404 for the favicon
    w.console.push(`${m.type()}: ${m.text()}`);
  });
  page.on('pageerror', (e) => w.errors.push(e.message));
  page.on('request', (r) => {
    w.urls.push(r.url());
    let body = 0;
    try { body = r.postDataBuffer()?.length ?? 0; } catch { /* no body */ }
    let top = false;
    try { top = r.isNavigationRequest() && r.frame().parentFrame() === null; } catch { /* a worker's request has no frame */ }
    w.reqs.push({ url: r.url(), method: r.method(), bytes: body, top_nav: top });
  });
  return w;
}

// Waits until window.__lomn.state is one of `states` (or 'error'); returns the state. Never a fixed sleep.
export async function waitState(page, states, timeout = 60000) {
  const want = [...states, 'error'];
  await page.waitForFunction((w) => w.includes(window.__lomn?.state), want, { timeout });
  return page.evaluate(() => ({ state: window.__lomn.state, message: window.__lomn.message, error: window.__lomn.error }));
}

export async function waitFrames(page, n, timeout = 30000) {
  await page.waitForFunction((k) => (window.__lomn?.metrics?.frames_since_show ?? 0) >= k, n, { timeout });
}

// ---- PNG (8-bit RGB/RGBA, non-interlaced: what stb_image_write and Chromium screenshots produce) ----
export function decodePng(buf) {
  if (buf.readUInt32BE(0) !== 0x89504e47) throw new Error('not a PNG');
  let off = 8, w = 0, h = 0, ct = 0;
  const idat = [];
  while (off < buf.length) {
    const len = buf.readUInt32BE(off), type = buf.toString('ascii', off + 4, off + 8);
    const data = buf.subarray(off + 8, off + 8 + len);
    if (type === 'IHDR') {
      w = data.readUInt32BE(0);
      h = data.readUInt32BE(4);
      if (data[8] !== 8 || data[12] !== 0) throw new Error('PNG: only 8-bit non-interlaced');
      ct = data[9];
    } else if (type === 'IDAT') idat.push(data);
    else if (type === 'IEND') break;
    off += 12 + len;
  }
  const bpp = ct === 6 ? 4 : ct === 2 ? 3 : 0;
  if (!bpp) throw new Error('PNG: colour type ' + ct);
  const raw = zlib.inflateSync(Buffer.concat(idat));
  const stride = w * bpp, out = Buffer.alloc(w * h * 4), prev = Buffer.alloc(stride), cur = Buffer.alloc(stride);
  for (let y = 0; y < h; ++y) {
    const f = raw[y * (stride + 1)];
    raw.copy(cur, 0, y * (stride + 1) + 1, (y + 1) * (stride + 1));
    for (let i = 0; i < stride; ++i) {
      const a = i >= bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0;
      let v = cur[i];
      if (f === 1) v += a;
      else if (f === 2) v += b;
      else if (f === 3) v += (a + b) >> 1;
      else if (f === 4) {
        const p = a + b - c, pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
        v += pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
      }
      cur[i] = v & 255;
    }
    for (let x = 0; x < w; ++x) {
      out[(y * w + x) * 4] = cur[x * bpp];
      out[(y * w + x) * 4 + 1] = cur[x * bpp + 1];
      out[(y * w + x) * 4 + 2] = cur[x * bpp + 2];
      out[(y * w + x) * 4 + 3] = bpp === 4 ? cur[x * bpp + 3] : 255;
    }
    cur.copy(prev);
  }
  return { width: w, height: h, rgba: out };
}

// RGB difference (alpha ignored: the D3D8 back buffer is X8R8G8B8). Histogram of the per-pixel max channel delta.
export function diffRgb(a, b) {
  if (a.width !== b.width || a.height !== b.height) return { sameSize: false };
  const hist = new Array(256).fill(0);
  let differing = 0, max = 0;
  for (let i = 0; i < a.width * a.height; ++i) {
    const d = Math.max(Math.abs(a.rgba[i * 4] - b.rgba[i * 4]), Math.abs(a.rgba[i * 4 + 1] - b.rgba[i * 4 + 1]),
                       Math.abs(a.rgba[i * 4 + 2] - b.rgba[i * 4 + 2]));
    hist[d]++;
    if (d) differing++;
    if (d > max) max = d;
  }
  const over = (t) => hist.slice(t + 1).reduce((n, x) => n + x, 0);
  return { sameSize: true, pixels: a.width * a.height, differing, max, over, hist };
}

// ---- M1 close-out C-N2: which built files the OPFS copy worker imports --------------------------------------------------------
// The page and opfs_worker.js are separate module graphs, so a file both import (paths.js) is requested twice on a first visit: that
// is the only legitimate repeat. The list is DERIVED from the worker's own import statements (never written down by hand), so a new
// import in the worker is picked up and a removed one stops being tolerated. A small tokenizer (comments, strings, regex literals and
// templates skipped) finds static import / re-export / literal dynamic import specifiers; anything it cannot read (a computed
// import(x), importScripts) throws instead of being guessed. tests/network_check_test.mjs checks it against V8's own parser.
function tokenize(text) {
  const out = [];
  const n = text.length;
  let i = 0;
  const regexAllowed = () => { const p = out[out.length - 1]; return !p || (p.t === 'p' && ![')', ']', '}'].includes(p.v)) || (p.t === 'w' && ['return', 'typeof', 'case', 'in', 'of', 'do', 'else'].includes(p.v)); };
  while (i < n) {
    const c = text[i];
    if (/\s/.test(c)) { ++i; continue; }
    if (c === '/' && text[i + 1] === '/') { while (i < n && text[i] !== '\n') ++i; continue; }
    if (c === '/' && text[i + 1] === '*') { const e = text.indexOf('*/', i + 2); i = e < 0 ? n : e + 2; continue; }
    if (c === '"' || c === "'") {
      let j = i + 1, v = '';
      while (j < n && text[j] !== c) { if (text[j] === '\\') { v += text[j + 1] ?? ''; j += 2; } else v += text[j++]; }
      out.push({ t: 's', v });
      i = j + 1;
      continue;
    }
    if (c === '`') {
      let j = i + 1, depth = 0, from = 0;
      while (j < n) {
        if (text[j] === '\\') { j += 2; continue; }
        if (depth === 0 && text[j] === '`') break;
        if (text[j] === '$' && text[j + 1] === '{') { if (depth++ === 0) from = j; j += 2; continue; }
        if (depth > 0 && text[j] === '}' && --depth === 0 && /\bimport\b/.test(text.slice(from, j))) throw new Error('cannot derive imports: an import inside a ${...} expression of a template literal');
        ++j;
      }
      out.push({ t: 'tmpl' });
      i = j + 1;
      continue;
    }
    if (c === '/' && regexAllowed()) {
      let j = i + 1, cls = false;
      while (j < n && (cls || text[j] !== '/') && text[j] !== '\n') { if (text[j] === '\\') ++j; else if (text[j] === '[') cls = true; else if (text[j] === ']') cls = false; ++j; }
      out.push({ t: 're' });
      i = j + 1;
      continue;
    }
    const w = /^[A-Za-z_$][\w$]*/.exec(text.slice(i, i + 64));
    if (w) { out.push({ t: 'w', v: w[0] }); i += w[0].length; continue; }
    out.push({ t: 'p', v: c });
    ++i;
  }
  return out;
}
export function importSpecifiers(text) {
  const tk = tokenize(text), out = [];
  const isP = (x, v) => x && x.t === 'p' && x.v === v;
  const isW = (x, v) => x && x.t === 'w' && x.v === v;
  for (let k = 0; k < tk.length; ++k) {
    const x = tk[k];
    if (isP(tk[k - 1], '.')) continue;  // obj.import / obj.export is a property, not a statement
    if (isW(x, 'importScripts')) throw new Error('cannot derive imports: importScripts() in a module worker');
    if (isW(x, 'import')) {
      const nx = tk[k + 1];
      if (isP(nx, '.')) continue;  // import.meta
      if (isP(nx, '(')) {
        if (tk[k + 2]?.t === 's' && isP(tk[k + 3], ')')) out.push(tk[k + 2].v);
        else throw new Error('cannot derive imports: a computed import(...)');
        continue;
      }
      if (nx?.t === 's') { out.push(nx.v); continue; }
      let j = k + 1;
      while (j < tk.length && !isW(tk[j], 'from') && !isP(tk[j], ';')) ++j;
      if (isW(tk[j], 'from') && tk[j + 1]?.t === 's') { out.push(tk[j + 1].v); k = j + 1; continue; }
      throw new Error('cannot derive imports: an import statement without a readable specifier');
    }
    if (isW(x, 'export')) {
      // `export { a } from 'x'`, `export * from 'x'`, `export * as n from 'x'` re-export; `export const|function|class|default|...` does not.
      let j = k + 1;
      while (j < tk.length && (isP(tk[j], '{') || isP(tk[j], '}') || isP(tk[j], '*') || isP(tk[j], ',') || (tk[j].t === 'w' && tk[j].v !== 'from' && !['const', 'let', 'var', 'function', 'class', 'async', 'default'].includes(tk[j].v)) )) ++j;
      if (isW(tk[j], 'from') && tk[j + 1]?.t === 's') { out.push(tk[j + 1].v); k = j + 1; }
    }
  }
  return out;
}
// The published paths (keys of asset-manifest.json files) that the OPFS copy worker of `site` imports, directly or through other
// built files. Empty when the site has no worker (a test site). Throws on a specifier that is not a built file.
export function workerSharedFiles(site, manifest) {
  const paths = new Set(manifest.files.map((f) => f.path));
  const worker = [...paths].find((p) => /(^|\/)opfs_worker(\.[0-9a-f]{10})?\.js$/.test(p));
  const shared = new Set();
  if (!worker) return shared;
  const todo = [worker];
  const seen = new Set(todo);
  while (todo.length) {
    const cur = todo.pop();
    for (const spec of importSpecifiers(fs.readFileSync(path.join(site, cur), 'utf8'))) {
      if (!spec.startsWith('./') && !spec.startsWith('../')) throw new Error(`${cur}: bare or absolute import "${spec}" cannot be a built file`);
      const target = path.posix.normalize(path.posix.join(path.posix.dirname(cur), spec));
      if (!paths.has(target)) throw new Error(`${cur}: imports "${spec}" = ${target}, which is not in asset-manifest.json`);
      shared.add(target);
      if (!seen.has(target)) { seen.add(target); todo.push(target); }
    }
  }
  return shared;
}

// Network discipline (M1b 1.7): every response since `from` is a built asset (asset-manifest.json lists them), every
// request URL is on the test server's origin, and the bytes downloaded are at most the ceiling (the built files plus the worker's shared imports once more), with per-path counts (C-N2). Review fix 3: nothing
// is uploaded: every request is a GET or HEAD with no body, seen by the browser (method, postData) AND by the server
// (method, bytes read), so an upload to a path that is in the manifest fails too ("nothing is uploaded" is a product promise).
// Review fix N2: data can also leave in the URL: any request other than the page's own top-level navigation (whose
// ?hud=1&area=... is the page's, and which the browser marks via Sec-Fetch-*) must have no query string or fragment.
// T8 P10: "the page's own top-level navigation" means one the TEST made (watch() records page.goto / page.reload URLs): a
// window.open('/index.html?d=...') popup is also a top-level navigation by Sec-Fetch-*, so any top-level navigation whose
// path+query the test did not open is a finding (`navigations`), seen server-side (all pages) and browser-side (this page).
export function networkCheck(srv, site, watcher, fromResponse = 0, fromUrl = 0) {
  const manifest = JSON.parse(fs.readFileSync(path.join(site, 'asset-manifest.json'), 'utf8'));
  const allowed = new Set(manifest.files.map((f) => '/' + f.path).concat(['/', '/asset-manifest.json', '/favicon.ico']));
  const buildBytes = manifest.total_bytes + fs.statSync(path.join(site, 'asset-manifest.json')).size;
  // C-N2: per-path accounting. A page fetches each built file at most once; the files opfs_worker.js imports (derived from the
  // worker's import statements) at most twice (page + worker), per load of the page. The page never fetches asset-manifest.json, so the ceiling is the
  // built files' size plus the shared files once more, not buildBytes (which counted the manifest file as headroom).
  const shared = workerSharedFiles(site, manifest);
  const sizeOf = new Map(manifest.files.map((f) => [f.path, f.bytes]));
  const resp = srv.responses.slice(fromResponse);
  // A page.reload() / second page.goto() inside the window loads the whole app again: every limit scales by the number of top-level
  // loads of the page (min 1). A popup is a top-level load too, but it is a `navigations` finding of its own.
  const visits = Math.max(1, resp.filter((r) => r.top_nav && (r.path === '/' || r.path === '/index.html')).length);
  const ceiling = visits * (manifest.total_bytes + [...shared].reduce((n, p) => n + sizeOf.get(p), 0));
  const counts = new Map();
  for (const r of resp) {
    const key = r.path === '/' ? '/index.html' : r.path;
    counts.set(key, (counts.get(key) ?? 0) + 1);
  }
  const repeats = [...counts]
    .filter(([p]) => sizeOf.has(p.slice(1)))
    .map(([p, count]) => ({ path: p.slice(1), count, max: visits * (shared.has(p.slice(1)) ? 2 : 1) }))
    .filter((r) => r.count > r.max);
  const foreignPaths = resp.filter((r) => !allowed.has(r.path)).map((r) => r.path);
  const origin = new URL(srv.url).origin;
  const foreignUrls = watcher.urls.slice(fromUrl).filter((u) => !u.startsWith(origin + '/') && !u.startsWith('blob:') && !u.startsWith('data:'));
  const bytes = resp.reduce((n, r) => n + r.bytes, 0);
  const isUpload = (x) => !['GET', 'HEAD'].includes(x.method) || x.bytes > 0;
  const uploads = [
    ...watcher.reqs.slice(fromUrl).filter(isUpload).map((x) => ({ side: 'browser', method: x.method, url: x.url, bytes: x.bytes })),
    ...resp.filter((r) => isUpload({ method: r.method, bytes: r.req_bytes })).map((r) => ({ side: 'server', method: r.method, url: r.path, bytes: r.req_bytes })),
  ];
  const hasData = (u) => { try { const x = new URL(u, 'http://x'); return x.search !== '' || x.hash !== ''; } catch { return true; } };
  const queries = [
    ...watcher.reqs.slice(fromUrl).filter((x) => !x.top_nav && hasData(x.url)).map((x) => ({ side: 'browser', url: x.url })),
    ...resp.filter((r) => !r.top_nav && hasData(r.url ?? r.path)).map((r) => ({ side: 'server', url: r.url })),
  ];
  const expected = watcher.expected_navs ?? new Set();
  const navigations = [
    ...watcher.reqs.slice(fromUrl).filter((x) => x.top_nav && !expected.has(navKey(x.url))).map((x) => ({ side: 'browser', url: x.url })),
    ...resp.filter((r) => r.top_nav && !expected.has(navKey(r.url ?? r.path))).map((r) => ({ side: 'server', url: r.url })),
  ];
  return { ok: !foreignPaths.length && !foreignUrls.length && !uploads.length && !queries.length && !navigations.length && !repeats.length && bytes <= ceiling,
           foreignPaths, foreignUrls, uploads, queries, navigations, repeats, bytes, buildBytes, ceiling, shared: [...shared], responses: resp.length };
}
