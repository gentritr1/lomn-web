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

// Console errors/warnings, page exceptions and every request URL the page makes (any origin).
export function watch(page) {
  const w = { console: [], errors: [], urls: [] };
  page.on('console', (m) => {
    if (!['error', 'warning'].includes(m.type())) return;
    if (m.location().url.endsWith('/favicon.ico')) return;  // the test server answers 404 for the favicon
    w.console.push(`${m.type()}: ${m.text()}`);
  });
  page.on('pageerror', (e) => w.errors.push(e.message));
  page.on('request', (r) => w.urls.push(r.url()));
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

// Network discipline (M1b 1.7): every response since `from` is a built asset (asset-manifest.json lists them), every
// request URL is on the test server's origin, and the bytes downloaded are at most the site's size.
export function networkCheck(srv, site, watcher, fromResponse = 0, fromUrl = 0) {
  const manifest = JSON.parse(fs.readFileSync(path.join(site, 'asset-manifest.json'), 'utf8'));
  const allowed = new Set(manifest.files.map((f) => '/' + f.path).concat(['/', '/asset-manifest.json', '/favicon.ico']));
  const buildBytes = manifest.total_bytes + fs.statSync(path.join(site, 'asset-manifest.json')).size;
  const resp = srv.responses.slice(fromResponse);
  const foreignPaths = resp.filter((r) => !allowed.has(r.path)).map((r) => r.path);
  const origin = new URL(srv.url).origin;
  const foreignUrls = watcher.urls.slice(fromUrl).filter((u) => !u.startsWith(origin + '/') && !u.startsWith('blob:') && !u.startsWith('data:'));
  const bytes = resp.reduce((n, r) => n + r.bytes, 0);
  return { ok: !foreignPaths.length && !foreignUrls.length && bytes <= buildBytes, foreignPaths, foreignUrls, bytes, buildBytes, responses: resp.length };
}
