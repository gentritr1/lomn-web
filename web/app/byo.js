// Bring-your-own files (spec §4.3). Nothing here sends anything over the network: every byte is read locally through
// the File System Access API (Chromium: the player's folder, in place) or from OPFS (Safari/Firefox: a one-time copy).
//
// A "source" is {kind, name, entries, file(entry)}: entries = [{path, size}] relative to the install root (the folder
// that holds LEGOBionicle.exe), file(entry) -> Promise<File>. Three kinds:
//   'handle' a FileSystemDirectoryHandle the player picked (showDirectoryPicker); persisted in IndexedDB
//   'opfs'   the copy inside the origin-private file system (directory COPY_DIR)
//   'files'  the File objects of <input webkitdirectory> (only to validate and copy them; they do not survive a reload)
import { blkTablesFingerprint, classify, lowerAscii, MAX_EXE_BYTES, sha256hex } from './fingerprint.js';
import { checkRelPath, MAX_DEPTH, MAX_PATH_BYTES } from './paths.js';

export const COPY_DIR = 'lomn-install';
export const COPY_DONE = 'lomn-copy-complete.json';
export const EXE = 'legobionicle.exe';
// Caps on what a picked folder may make the page walk (real install: 529 files, depth 5, longest path 54 bytes). The path
// rules (UTF-8 bytes, depth, segments) are paths.js's, shared with the copy worker.
export const MAX_FILES = 65536;
export { MAX_DEPTH, MAX_PATH_BYTES };
const MAX_DONE_BYTES = 8 * 1024 * 1024;

const isHidden = (name) => name.startsWith('.');

// Which files the copy (and the engine's index) takes: the exe at the top and everything under data/ except dot files and
// executables/DLLs (the port never runs them). The quota check sums exactly this set.
export function wanted(path) {
  const k = lowerAscii(path);
  if (k === EXE) return true;
  if (!k.startsWith('data/')) return false;
  if (k.split('/').some(isHidden)) return false;
  return !(k.endsWith('.exe') || k.endsWith('.dll'));
}

// REF4 r2 (M11): one rule for both sources. A wanted file whose path breaks paths.js's rules, or whose name differs from
// an earlier one only in case (one engine path; possible on a case-sensitive file system), is skipped with a warning
// instead of failing the whole install. Returns true when the entry is taken.
function admit(path, seen, warnings) {
  const why = checkRelPath(path);
  if (why) {
    if (warnings.length < 100) warnings.push(`skipped ${path.slice(0, 80)}: ${why}`);
    return false;
  }
  const key = lowerAscii(path);
  if (seen.has(key)) {
    if (warnings.length < 100) warnings.push(`skipped ${path.slice(0, 80)}: same name as ${seen.get(key)} (names are case-insensitive)`);
    return false;
  }
  seen.set(key, path);
  return true;
}

// A walk that hit a cap on what a picked folder may hold: its own error type, so a caller never mistakes it for "the folder is
// gone" (a moved or deleted folder makes entries() throw a DOMException instead).
export class WalkError extends Error {
  constructor(message) {
    super(message);
    this.name = 'WalkError';
  }
}
// The sentence the page shows for a remembered folder that could not be walked.
export function describeWalkFailure(name, err) {
  if (err instanceof WalkError) return `The remembered folder "${name}" is not a usable game folder: ${err.message}.`;
  return `The remembered folder "${name}" could not be read (moved or deleted).`;
}

// Walks a picked folder like filesSource walks an <input>: only what wanted() takes is looked at. Folders beside data/ (a
// player may pick a big or deep parent folder) are never opened; names that break paths.js's rules, case duplicates and
// subtrees deeper than MAX_DEPTH are skipped with a warning; only the entry cap (MAX_FILES) is fatal, as a WalkError.
export async function walkHandle(dir) {
  const entries = [];
  const handles = new Map();
  const seen = new Map(), warnings = [];
  const warn = (m) => { if (warnings.length < 100) warnings.push(m); };
  let visited = 0;
  async function walk(d, prefix, depth) {
    for await (const [name, h] of d.entries()) {
      if (++visited > MAX_FILES) throw new WalkError(`more than ${MAX_FILES} entries in the folder`);
      if (isHidden(name)) continue;
      const path = prefix + name;
      if (h.kind === 'directory') {
        if (depth === 0 && lowerAscii(name) !== 'data') continue;  // wanted() takes nothing outside data/
        const why = checkRelPath(path) || (depth + 1 > MAX_DEPTH ? `nested deeper than ${MAX_DEPTH} levels` : '');
        if (why) warn(`skipped folder ${path.slice(0, 80)}: ${why}`);
        else await walk(h, path + '/', depth + 1);
      } else {
        if (!wanted(path) || !admit(path, seen, warnings)) continue;
        const f = await h.getFile();
        const e = { path, size: f.size };
        entries.push(e);
        handles.set(e, h);
      }
    }
  }
  await walk(dir, '', 0);
  const cache = new Map();
  return {
    entries,
    warnings,
    file: async (e) => {
      let f = cache.get(e);
      if (!f) cache.set(e, (f = await handles.get(e).getFile()));
      return f;
    },
  };
}

export async function handleSource(dir, kind) {
  const w = await walkHandle(dir);
  return { kind, name: dir.name, dir, entries: w.entries, warnings: w.warnings, file: w.file };
}

// <input webkitdirectory>: webkitRelativePath = "<picked folder>/data/...". The first segment is dropped.
export function filesSource(fileList) {
  const entries = [];
  const files = new Map();
  const seen = new Map(), warnings = [];
  let name = '';
  if (fileList.length > MAX_FILES) throw new WalkError(`more than ${MAX_FILES} files`);
  for (const f of fileList) {
    const rel = f.webkitRelativePath || f.name;
    const slash = rel.indexOf('/');
    if (!name) name = slash > 0 ? rel.slice(0, slash) : '';
    const path = slash > 0 ? rel.slice(slash + 1) : rel;
    if (!path || path.split('/').some(isHidden) || !wanted(path) || !admit(path, seen, warnings)) continue;
    const e = { path, size: f.size };
    entries.push(e);
    files.set(e, f);
  }
  return { kind: 'files', name, entries, warnings, file: async (e) => files.get(e) };
}

export async function readRange(source, e, offset, length) {
  const f = await source.file(e);
  if (offset + length > f.size) throw new Error(`${e.path}: read past the end (the file changed?)`);
  return new Uint8Array(await f.slice(offset, offset + length).arrayBuffer());
}

// exe hash + blockfile-table fingerprint -> classify() against builds.json. {ok, message, build, data, exeSha, dataFp}.
export async function validate(source, builds) {
  const exe = source.entries.find((e) => lowerAscii(e.path) === EXE);
  if (!exe) return { ok: false, message: 'LEGOBionicle.exe was not found at the top of the chosen folder. Choose the game folder itself (the one that contains LEGOBionicle.exe and data/).' };
  if (exe.size > MAX_EXE_BYTES) return { ok: false, message: 'LEGOBionicle.exe is larger than any known build: not an accepted build.' };
  const t0 = performance.now();
  const exeSha = await sha256hex(await readRange(source, exe, 0, exe.size));
  let fp;
  try {
    fp = await blkTablesFingerprint(source.entries, (e, o, l) => readRange(source, e, o, l));
  } catch (err) {
    return { ok: false, message: 'The game data could not be validated: ' + err.message, exeSha };
  }
  const v = classify(builds, exeSha, fp.fingerprint);
  return { ...v, exeSha, dataFp: fp.fingerprint, blkFiles: fp.blkFiles, tableBytes: fp.tableBytes, ms: performance.now() - t0 };
}

export const totalBytes = (source) => source.entries.reduce((n, e) => n + e.size, 0);

// Quota first (spec §4.3): persist() then estimate(); never start a copy that cannot finish.
export async function quotaCheck(needBytes) {
  let persisted = false;
  try {
    persisted = (await navigator.storage.persist?.()) ?? false;
  } catch { /* not fatal: the estimate decides */ }
  const est = await navigator.storage.estimate();
  const free = Math.max(0, (est.quota ?? 0) - (est.usage ?? 0));
  return { ok: free >= needBytes, free, need: needBytes, persisted, quota: est.quota, usage: est.usage };
}

// REF4 r2 (I6): the completion manifest's shape ('' when valid): bounded file list of {path (paths.js rules), size}.
export function checkCopyManifest(done) {
  if (done?.format !== 'lomn-opfs-copy' || done.version !== 1) return 'unknown format';
  if (!Array.isArray(done.files) || done.files.length > MAX_FILES) return 'bad files list';
  for (const f of done.files)
    if (!f || checkRelPath(f.path) || !Number.isSafeInteger(f.size) || f.size < 0) return 'bad entry ' + JSON.stringify(f).slice(0, 80);
  return '';
}
// The copy (entries walked from OPFS) against the manifest's files: '' when every listed file is there with its size.
export function sameFiles(entries, files) {
  const have = new Map(entries.map((e) => [lowerAscii(e.path), e.size]));
  for (const f of files) {
    const k = lowerAscii(f.path);
    if (!have.has(k)) return `missing ${f.path}`;
    if (have.get(k) !== f.size) return `size of ${f.path} is ${have.get(k)}, the copy wrote ${f.size}`;
  }
  return '';
}

// The completion manifest of a finished OPFS copy, or null (absent or malformed; the files themselves are compared with
// it by sameFiles once the copy is walked).
export async function opfsCopy() {
  try {
    const root = await navigator.storage.getDirectory();
    const dir = await root.getDirectoryHandle(COPY_DIR);
    const doneFile = await (await dir.getFileHandle(COPY_DONE)).getFile();
    if (doneFile.size > MAX_DONE_BYTES) return null;
    const done = JSON.parse(await doneFile.text());
    if (checkCopyManifest(done)) return null;
    return { dir, done };
  } catch {
    return null;
  }
}
// REF4 r2 (I7): a copy directory exists (with or without its completion manifest).
export async function opfsDirExists() {
  try {
    await (await navigator.storage.getDirectory()).getDirectoryHandle(COPY_DIR);
    return true;
  } catch {
    return false;
  }
}

// Removes a previous (possibly partial) copy so the estimate sees the space it held.
export async function opfsClear() {
  const root = await navigator.storage.getDirectory();
  try {
    await root.removeEntry(COPY_DIR, { recursive: true });
  } catch { /* nothing to clear */ }
}

// Copies the source into OPFS in a worker (createSyncAccessHandle, chunked). onProgress(bytesDone, bytesTotal).
// The completion manifest is written last, so an interrupted copy is never mistaken for a complete one.
export function copyToOpfs(source, verdict, onProgress) {
  return new Promise((resolve, reject) => {
    const worker = new Worker(new URL('./opfs_worker.js', import.meta.url), { type: 'module' });
    const files = source.entries.map((e) => ({ path: e.path, size: e.size }));
    Promise.all(source.entries.map((e) => source.file(e))).then((blobs) => {
      worker.onmessage = ({ data }) => {
        if (data.type === 'progress') onProgress(data.done, data.total);
        else if (data.type === 'done') { worker.terminate(); resolve(data); }
        else if (data.type === 'error') { worker.terminate(); reject(new Error(data.message)); }
      };
      worker.onerror = (ev) => { worker.terminate(); reject(new Error('copy worker failed: ' + (ev.message || 'error'))); };
      worker.postMessage({
        type: 'copy', dir: COPY_DIR, doneName: COPY_DONE, files, blobs,
        done: { format: 'lomn-opfs-copy', version: 1, build: verdict.build, data: verdict.data, exe_sha256: verdict.exeSha,
                blk_tables_sha256: verdict.dataFp, files, bytes: totalBytes(source) },
      });
    }, reject);
  });
}
