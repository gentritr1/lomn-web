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
  // The engine's fs::normalize reads a backslash as a separator (so "a\..\b" is "b") and a NUL ends a C string: a name with either
  // would map to a different engine path than the one the page checked (duplicate or outside-root errors that fail the install).
  if (path.includes('\\')) return 'backslash in the name (the engine treats it as a path separator)';
  if (path.includes('\0')) return 'NUL character in the name';
  // The engine's file lists are tab/newline separated: TAB, LF or any control character would split a name in two.
  const ctl = /[\x00-\x1f\x7f]/.exec(path);
  if (ctl) return `control character U+${ctl[0].charCodeAt(0).toString(16).toUpperCase().padStart(4, '0')} in the name`;
  const bytes = utf8.encode(path).length + ENGINE_PREFIX_BYTES;
  if (bytes > MAX_PATH_BYTES) return `path longer than ${MAX_PATH_BYTES} bytes (${bytes} as UTF-8)`;
  const segs = path.split('/');
  if (segs.length > MAX_DEPTH + 1) return `nested deeper than ${MAX_DEPTH} levels`;
  for (const s of segs) if (s === '' || s === '.' || s === '..') return `bad path segment "${s}"`;
  return '';
}
