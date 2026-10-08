// Minimal IndexedDB key/value store (one database, one object store). Holds the picked directory handle (Chromium:
// FileSystemDirectoryHandle is structured-cloneable) and the per-area prefetch sets the engine discovered.
const DB = 'lomn-web';
const STORE = 'kv';

function open() {
  return new Promise((resolve, reject) => {
    const req = indexedDB.open(DB, 1);
    req.onupgradeneeded = () => req.result.createObjectStore(STORE);
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
}

async function tx(mode, fn) {
  const db = await open();
  try {
    return await new Promise((resolve, reject) => {
      const t = db.transaction(STORE, mode);
      const req = fn(t.objectStore(STORE));
      t.oncomplete = () => resolve(req?.result);
      t.onerror = () => reject(t.error);
      t.onabort = () => reject(t.error);
    });
  } finally {
    db.close();
  }
}

export const idbGet = (key) => tx('readonly', (s) => s.get(key));
export const idbSet = (key, value) => tx('readwrite', (s) => s.put(value, key));
export const idbDelete = (key) => tx('readwrite', (s) => s.delete(key));
export const idbKeys = () => tx('readonly', (s) => s.getAllKeys());
