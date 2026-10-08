// Node front end of web/app/fingerprint.js (the code the page runs): prints the exe SHA-256 and the blockfile-table
// fingerprint of an install folder as JSON. Walks the folder like the page (dot files skipped; data/ and the exe only).
// Usage: node web/tools/fingerprint_cli.mjs <install_dir>
import fs from 'node:fs';
import path from 'node:path';
import { blkTablesFingerprint, lowerAscii, sha256hex } from '../app/fingerprint.js';

const root = process.argv[2];
if (!root) {
  console.error('usage: fingerprint_cli.mjs <install_dir>');
  process.exit(2);
}
const entries = [];
const walk = (dir, prefix) => {
  for (const f of fs.readdirSync(dir)) {
    if (f.startsWith('.')) continue;
    const p = path.join(dir, f);
    const st = fs.statSync(p);
    if (st.isDirectory()) walk(p, prefix + f + '/');
    else entries.push({ path: prefix + f, size: st.size, abs: p });
  }
};
walk(root, '');
const readRange = async (e, off, len) => {
  const fd = fs.openSync(e.abs, 'r');
  try {
    const b = Buffer.alloc(len);
    const n = fs.readSync(fd, b, 0, len, off);
    return new Uint8Array(b.buffer, b.byteOffset, n);
  } finally {
    fs.closeSync(fd);
  }
};
const exe = entries.find((e) => lowerAscii(e.path) === 'legobionicle.exe');
const fp = await blkTablesFingerprint(entries, readRange);
console.log(JSON.stringify({
  exe_sha256: exe ? await sha256hex(fs.readFileSync(exe.abs)) : null,
  blk_tables_sha256: fp.fingerprint,
  blk_files: fp.blkFiles,
  table_bytes: fp.tableBytes,
}, null, 1));
