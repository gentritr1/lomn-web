// The wasm engine (lomn_web.js, an ES module factory built with INVOKE_RUN=0) and its file bridge.
//
// Prefetch by discovery (REF4 decision D2; engine side: engine/src/app/main_web.cpp): a load runs in rounds; each
// round's misses (reads whose bytes are not resident) are fetched from the player's files straight into wasm heap
// buffers, chunk by chunk (CHUNK bytes at a time: the JS-side peak is one chunk, never a whole file), and the load
// runs again until a round has no miss. The last round's reads (lomn_fs_touched) are cached in IndexedDB per
// (data fingerprint, engine build, area), so a revisit prefetches everything in one go and needs one round.
import { idbDelete, idbGet, idbKeys, idbSet } from './idb.js';
import { lowerAscii, sha256hex } from './fingerprint.js';

export const CHUNK = 8 * 1024 * 1024;
export const MAX_ROUNDS = 16;  // hard limit; measured maximum 8 over all 122 Beta 1.5.0 areas (a13_real asserts <= 12)
// REF4 r2 (M2): a cached prefetch set is a hint from IndexedDB (tamperable): over this many bytes it is ignored and the
// area is discovered instead. The largest real area set is 13,066,416 bytes (lev0/frnt incl. boot).
export const MAX_CACHED_BYTES = 64 * 1024 * 1024;

// Cached ranges that are inside an indexed file, non-empty, and together under MAX_CACHED_BYTES ([] otherwise).
export function filterCached(ranges, byNorm) {
  const ok = ranges.filter((r) => {
    const e = byNorm.get(r.path);
    return e && r.length > 0 && r.offset + r.length <= e.size;
  });
  return ok.reduce((n, r) => n + r.length, 0) > MAX_CACHED_BYTES ? [] : ok;
}
const MAX_RANGES_TEXT = 4096;  // ranges accepted from one miss list or cached set (engine cap: 4,096 misses per round)

// "path\toffset\tlength\n" lines -> [{path, offset, length}], bounded and integer-checked.
export function parseRanges(text) {
  const out = [];
  for (const line of text.split('\n')) {
    if (!line) continue;
    if (out.length >= MAX_RANGES_TEXT * 16) throw new Error('range list over the cap');
    const [path, o, l] = line.split('\t');
    const offset = Number(o), length = Number(l);
    if (!path || !Number.isSafeInteger(offset) || !Number.isSafeInteger(length) || offset < 0 || length < 0)
      throw new Error('malformed range line');
    out.push({ path, offset, length });
  }
  return out;
}

export class Engine {
  constructor(M, source, onState) {
    this.M = M;
    this.source = source;
    this.onState = onState;
    this.byNorm = new Map();  // engine path ("root/...", lower-case) -> source entry
    this.load = null;         // metrics of the last area load (JS side)
    this.boot = null;
    this.cacheKey = '';
    this.concurrency = 1;     // debug (M1 measurement): ranges fetched in parallel per round
    this.useCache = true;     // debug: false = always discover
    this.validation = null;   // the BYO verdict shown with the metrics (C2: data 'unknown' + warning)
    this.store = { get: idbGet, set: idbSet, delete: idbDelete, keys: idbKeys };  // the prefetch-set cache (IndexedDB; tests inject a Map)
  }

  str(s, fn) {
    const p = this.M.stringToNewUTF8(s);
    try {
      return fn(p);
    } finally {
      this.M._free(p);
    }
  }
  lastError() { return this.M.UTF8ToString(this.M._lomn_last_error()); }
  // Engine metrics + the measured configuration (build flavour, the SHA-256 of the wasm bytes actually instantiated,
  // canvas size); the adapter is in the engine part.
  metrics() {
    const m = JSON.parse(this.M.UTF8ToString(this.M._lomn_metrics_json()));
    if (this.validation) m.validation = this.validation;
    const c = document.getElementById('canvas');
    m.build = { ...this.build, wasm_sha256: this.wasmSha, wasm_bytes: this.wasmBytes };
    m.canvas = { width: c.width, height: c.height, css_width: c.clientWidth, css_height: c.clientHeight, device_pixel_ratio: window.devicePixelRatio };
    return m;
  }
  areas() {
    return this.M.UTF8ToString(this.M._lomn_list_areas()).split('\n').filter(Boolean).map((l) => {
      const [level, area] = l.split('\t');
      return { level, area };
    });
  }

  // Registers every file (path, size) of the source: exists()/size() answer from this index; no bytes move.
  index() {
    for (const e of this.source.entries) {
      const norm = 'root/' + lowerAscii(e.path);
      if (!this.str('Root/' + e.path, (p) => this.M._lomn_fs_add_file(p, e.size))) throw new Error('index: ' + this.lastError());
      this.byNorm.set(norm, e);
    }
  }

  // One range into the heap. Returns the bytes moved (0 when it was already resident).
  async fetchRange(r) {
    const e = this.byNorm.get(r.path);
    if (!e) throw new Error('prefetch: not in the index: ' + r.path);
    if (r.length === 0 || this.str(r.path, (p) => this.M._lomn_fs_is_resident(p, r.offset, r.length))) return 0;
    const f = await this.source.file(e);
    if (r.offset + r.length > f.size) throw new Error(`prefetch: ${e.path} is shorter than the index says (changed on disk?)`);
    const ptr = this.str(r.path, (p) => this.M._lomn_fs_reserve(p, r.offset, r.length));
    if (!ptr) throw new Error('prefetch: ' + this.lastError());
    try {
      for (let pos = 0; pos < r.length; pos += CHUNK) {
        const n = Math.min(CHUNK, r.length - pos);
        const buf = new Uint8Array(await f.slice(r.offset + pos, r.offset + pos + n).arrayBuffer());
        if (buf.length !== n) throw new Error(`prefetch: short read of ${e.path}`);
        this.M.HEAPU8.set(buf, ptr + pos);  // re-read HEAPU8 each time: a reserve may have grown the memory
      }
    } catch (err) {
      this.M._lomn_fs_abort(ptr);
      throw err;
    }
    this.M._lomn_fs_commit(ptr);
    return r.length;
  }

  // Rounds of probe() until it returns 0 (no miss). probe() returns the miss count, or < 0 on a real error.
  // Fetches a list of ranges, `concurrency` at a time; returns [bytes moved, ranges moved].
  async fetchAll(list, fetched) {
    let bytes = 0, n = 0, next = 0;
    const worker = async () => {
      while (next < list.length) {
        const r = list[next++];
        const b = await this.fetchRange(r);
        if (b) {
          bytes += b;
          ++n;
          fetched.push(r);
        }
      }
    };
    await Promise.all(Array.from({ length: Math.max(1, Math.min(this.concurrency, 16)) }, worker));
    return [bytes, n];
  }

  async rounds(probe, what, prefetch = []) {
    const t0 = performance.now();
    let fetchMs = 0, bytes = 0, ranges = 0;
    const fetched = [];  // every range this load moved (I5: over-fetch = moved but not read by the final round)
    this.fetched = fetched;
    const tf = performance.now();
    [bytes, ranges] = await this.fetchAll(prefetch, fetched);
    fetchMs += performance.now() - tf;
    for (let round = 1; round <= MAX_ROUNDS; ++round) {
      const m = probe();
      if (m === 0) return { rounds: round, fetch_ms: fetchMs, bytes, ranges, total_ms: performance.now() - t0, cached: prefetch.length > 0 };
      if (m < 0) throw new Error(this.lastError());
      const misses = parseRanges(this.M.UTF8ToString(this.M._lomn_fs_misses()));
      const t1 = performance.now();
      const [moved, nMoved] = await this.fetchAll(misses, fetched);
      ranges += nMoved;
      fetchMs += performance.now() - t1;
      bytes += moved;
      if (moved === 0 && misses.every((r) => r.length > 0)) throw new Error(`${what}: prefetch made no progress (${misses[0]?.path})`);
    }
    throw new Error(`${what}: prefetch did not converge in ${MAX_ROUNDS} rounds`);
  }

  async cached(key) {
    if (!this.useCache) return [];
    try {
      const text = await this.store.get(this.cacheKey + key);
      if (typeof text !== 'string' || text.length > 16 * 1024 * 1024) return [];
      // Validated against the index and capped before use: a stale or tampered cache can only cost a fetch, never a bad
      // read or an oversized heap.
      return filterCached(parseRanges(text), this.byNorm);
    } catch {
      return [];
    }
  }
  forgetCachedSet(area) { return this.store.delete(this.cacheKey + 'area:' + area).catch(() => {}); }
  // REF4 r2 (M2): cached sets of other wasm builds can never be used again: drop them (keys "prefetch|<fp>|<sha>|...").
  async pruneOtherBuilds() {
    const sha = this.cacheKey.split('|')[2];
    const keys = await this.store.keys().catch(() => []);
    let n = 0;
    for (const k of keys)
      if (typeof k === 'string' && k.startsWith('prefetch|') && k.split('|')[2] !== sha) {
        await this.store.delete(k).catch(() => {});
        ++n;
      }
    return n;
  }

  // Boot set before main() (spec §4.3): Art.blk header + table and Base.osi, discovered like an area, then pinned.
  async bootAndMain(width, height, cacheKey) {
    this.cacheKey = cacheKey;
    this.pruned = await this.pruneOtherBuilds();
    this.index();
    const pre = await this.cached('boot');
    this.boot = await this.rounds(() => this.M._lomn_boot_probe(), 'boot', pre);
    this.boot.pinned_bytes = this.M._lomn_boot_done();
    await this.store.set(this.cacheKey + 'boot', this.M.UTF8ToString(this.M._lomn_fs_touched())).catch(() => {});
    this.M._lomn_configure(width, height, 0);
    this.M.callMain([]);
  }

  async showArea(level, area) {
    const key = `area:${level}/${area}`;
    const t0 = performance.now();
    this.str(level, (l) => this.str(area, (a) => this.M._lomn_area_begin(l, a)));
    const pre = await this.cached(key);
    const r = await this.rounds(() => this.M._lomn_area_probe(), `${level}/${area}`, pre);
    const touched = this.M.UTF8ToString(this.M._lomn_fs_touched());
    // M1 close-out C-f2: a load that used a cached set writes back the ranges the engine actually READ (exact lines), not the resident
    // ranges it touched: one tampered whole-file range in the cache is "touched" by every read inside it and would be persisted for
    // ever. The exact reads are what a clean discovery persists (a13_real: touched == the native --manifest on 122 areas), so an honest
    // cache is rewritten unchanged and a subset cache gains what the extra rounds discovered.
    const reads = r.cached ? this.M.UTF8ToString(this.M._lomn_fs_reads()) : touched;
    const read = new Set(touched.split('\n').filter(Boolean));
    const overFetch = this.fetched.filter((x) => !read.has(`${x.path}\t${x.offset}\t${x.length}`)).reduce((n, x) => n + x.length, 0);
    if (!this.M._lomn_area_show()) {
      // lomn_area_show releases the previous area's GPU objects before it uploads the new one: a failure from here on has lost it.
      const err = new Error(this.lastError());
      err.previousAreaGone = true;
      throw err;
    }
    await this.store.set(this.cacheKey + key, reads).catch(() => {});
    this.load = { area: `${level}/${area}`, ...r, touched, over_fetch_bytes: overFetch, concurrency: this.concurrency, load_total_ms: performance.now() - t0 };
    return this.load;
  }
}

// Instantiates the module from wasm bytes fetched (once) and hashed here, so the page knows exactly which build it runs
// (metrics.build.wasm_sha256; also the prefetch-cache key). onState(state, detail) receives the engine's own states.
// The two file names below are rewritten to their content-hashed names by web/tools/build_site.mjs.
export async function createEngine(source, onState) {
  const res = await fetch(new URL('../lomn_web.wasm', import.meta.url));
  if (!res.ok) throw new Error('lomn_web.wasm: HTTP ' + res.status);
  const wasm = new Uint8Array(await res.arrayBuffer());
  const wasmSha = await sha256hex(wasm);
  const { default: createLomn } = await import('../lomn_web.js');
  const M = await createLomn({
    // Instantiate from the bytes hashed above (no second download; Emscripten 6 ignores a plain wasmBinary option).
    instantiateWasm: (imports, receive) => {
      WebAssembly.instantiate(wasm, imports).then((r) => receive(r.instance, r.module), (e) => onState('error', 'wasm instantiate: ' + e));
      return {};
    },
    onLomnState: (s, d) => onState(s, d),
    print: (t) => console.log(t),
    printErr: (t) => console.warn(t),
  });
  const e = new Engine(M, source, onState);
  e.wasmSha = wasmSha;
  e.wasmBytes = wasm.length;
  e.build = JSON.parse(M.UTF8ToString(M._lomn_build_info()));
  return e;
}
