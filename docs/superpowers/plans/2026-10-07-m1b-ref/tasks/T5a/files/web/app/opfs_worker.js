// Safari/Firefox fallback (spec §4.3): copies the chosen files into the origin-private file system, chunk by chunk
// through createSyncAccessHandle (worker-only API); never a whole-file arrayBuffer(). The completion manifest is
// written last. Messages out: {type:'progress', done, total} | {type:'done', bytes, ms} | {type:'error', message}.
import { checkRelPath } from './paths.js';

const CHUNK = 8 * 1024 * 1024;

async function dirFor(root, path) {
  const bad = checkRelPath(path);  // REF4 r2 (M12): defence in depth, the page checked the same rule
  if (bad) throw new Error(`refusing to copy "${String(path).slice(0, 80)}": ${bad}`);
  const parts = path.split('/');
  let d = root;
  for (const p of parts.slice(0, -1)) d = await d.getDirectoryHandle(p, { create: true });
  return { dir: d, name: parts[parts.length - 1] };
}

async function writeFile(root, path, blob, onBytes) {
  const { dir, name } = await dirFor(root, path);
  const fh = await dir.getFileHandle(name, { create: true });
  const access = await fh.createSyncAccessHandle();
  try {
    access.truncate(0);
    for (let pos = 0; pos < blob.size; pos += CHUNK) {
      const buf = new Uint8Array(await blob.slice(pos, Math.min(blob.size, pos + CHUNK)).arrayBuffer());
      const n = access.write(buf, { at: pos });
      if (n !== buf.length) throw new Error(`${path}: short write (${n} of ${buf.length})`);
      onBytes(buf.length);
    }
    access.flush();
    if (access.getSize() !== blob.size) throw new Error(`${path}: size after copy ${access.getSize()} != ${blob.size}`);
  } finally {
    access.close();
  }
}

self.onmessage = async ({ data }) => {
  if (data?.type !== 'copy') return;
  const t0 = performance.now();
  try {
    const root = await (await navigator.storage.getDirectory()).getDirectoryHandle(data.dir, { create: true });
    const total = data.files.reduce((n, f) => n + f.size, 0);
    let done = 0, last = 0;
    for (let i = 0; i < data.files.length; ++i) {
      await writeFile(root, data.files[i].path, data.blobs[i], (n) => {
        done += n;
        if (done - last >= CHUNK || done === total) {
          last = done;
          self.postMessage({ type: 'progress', done, total });
        }
      });
    }
    const json = new TextEncoder().encode(JSON.stringify({ ...data.done, copied_ms: Math.round(performance.now() - t0) }));
    await writeFile(root, data.doneName, new Blob([json]), () => {});
    self.postMessage({ type: 'progress', done: total, total });
    self.postMessage({ type: 'done', bytes: total, ms: performance.now() - t0 });
  } catch (err) {
    self.postMessage({ type: 'error', message: String(err?.message ?? err) });
  }
};
