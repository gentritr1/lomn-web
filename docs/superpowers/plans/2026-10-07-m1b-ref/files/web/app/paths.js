// Relative path rules shared by the page (byo.js) and the OPFS copy worker (opfs_worker.js). Paths are relative to the
// install root, '/'-separated. The byte limit is the engine's (PrefetchFileSystem::kMaxPathBytes counts UTF-8 bytes of
// "Root/" + path), so a path the page accepts can never make the engine's index reject the whole install.
export const MAX_PATH_BYTES = 1024;  // real install: 54 bytes at most
export const MAX_DEPTH = 16;         // real install: depth 5
const ENGINE_PREFIX_BYTES = 5;       // "Root/"
const utf8 = new TextEncoder();

// '' when the path is acceptable, else a short reason.
export function checkRelPath(path) {
  if (typeof path !== 'string' || path.length === 0) return 'empty path';
  const bytes = utf8.encode(path).length + ENGINE_PREFIX_BYTES;
  if (bytes > MAX_PATH_BYTES) return `path longer than ${MAX_PATH_BYTES} bytes (${bytes} as UTF-8)`;
  const segs = path.split('/');
  if (segs.length > MAX_DEPTH + 1) return `nested deeper than ${MAX_DEPTH} levels`;
  for (const s of segs) if (s === '' || s === '.' || s === '..') return `bad path segment "${s}"`;
  return '';
}
