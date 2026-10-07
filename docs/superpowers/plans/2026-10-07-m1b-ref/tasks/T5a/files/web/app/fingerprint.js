// Build identification for the bring-your-own-files page (spec §4.3 "validation is cheap"). Shared by the browser
// (web/app/byo.js) and Node (web/tools/fingerprint_cli.mjs, the test fixtures); the independent oracle is
// tools/byo_fingerprint.py. Everything read here is untrusted: every count and length is bounded before use.
//
// exe:  SHA-256 of LEGOBionicle.exe at the top of the chosen folder (whole file, <= MAX_EXE_BYTES).
// data: the blockfile-table fingerprint. For every file data/**/*.blk (path relative to the install root, '/'-separated,
//       ASCII lower-case, sorted by that path in UTF-16 code-unit order = byte order for ASCII):
//         header = bytes [0, 12): "BLKF", u32 version, u32 count (little-endian)
//         table  = bytes [0, 12 + count * 56)           (the ScBlock entry table: names, sizes, flags, offsets)
//         line   = `${path} ${fileSize} ${sha256hex(table)}`
//       fingerprint = sha256hex(lines.join('\n') + '\n'). A file whose header is not "BLKF" or whose table does not fit
//       is an error naming the file.
export const MAX_EXE_BYTES = 64 * 1024 * 1024;        // real exe: 6,180,412 bytes (canonical) / 6,180,xxx (installer)
export const MAX_BLK_ENTRIES = 65536;                 // real maximum: 2,608 entries (data/Art/Art.blk)
export const MAX_BLK_FILES = 4096;                    // real: 189 .blk files
export const MAX_TOTAL_TABLE_BYTES = 64 * 1024 * 1024; // real: 643,748 bytes over all 189 tables
const BLK_HEADER = 12;
const BLK_ENTRY = 56;

const hex = (buf) => Array.from(new Uint8Array(buf), (b) => b.toString(16).padStart(2, '0')).join('');
export async function sha256hex(bytes) {
  return hex(await globalThis.crypto.subtle.digest('SHA-256', bytes));
}

// Lower-case ASCII only (the engine's fs::normalize rule); other bytes are kept.
export function lowerAscii(s) {
  return s.replace(/[A-Z]/g, (c) => String.fromCharCode(c.charCodeAt(0) + 32));
}

// entries: [{path, size}] relative to the install root ('/' separators, any case). readRange(entry, offset, length)
// resolves to a Uint8Array of exactly `length` bytes. Returns {fingerprint, blkFiles, tableBytes}; throws Error with a
// message naming the file on malformed input.
export async function blkTablesFingerprint(entries, readRange) {
  const blks = entries
    .map((e) => ({ e, key: lowerAscii(e.path) }))
    .filter((x) => x.key.startsWith('data/') && x.key.endsWith('.blk'))
    .sort((a, b) => (a.key < b.key ? -1 : a.key > b.key ? 1 : 0));
  if (blks.length === 0) throw new Error('no data/**/*.blk files: this is not a LOMN install');
  if (blks.length > MAX_BLK_FILES) throw new Error(`more than ${MAX_BLK_FILES} .blk files`);
  const lines = [];
  let total = 0;
  for (const { e, key } of blks) {
    if (!(e.size >= BLK_HEADER)) throw new Error(`${e.path}: too small for a blockfile header`);
    const h = await readRange(e, 0, BLK_HEADER);
    if (h.length !== BLK_HEADER || h[0] !== 0x42 || h[1] !== 0x4c || h[2] !== 0x4b || h[3] !== 0x46)
      throw new Error(`${e.path}: not a blockfile (no BLKF magic)`);
    const count = new DataView(h.buffer, h.byteOffset, h.byteLength).getUint32(8, true);
    if (count > MAX_BLK_ENTRIES) throw new Error(`${e.path}: ${count} entries (cap ${MAX_BLK_ENTRIES})`);
    const len = BLK_HEADER + count * BLK_ENTRY;
    if (len > e.size) throw new Error(`${e.path}: entry table larger than the file`);
    total += len;
    if (total > MAX_TOTAL_TABLE_BYTES) throw new Error('blockfile tables over the size cap');
    const table = await readRange(e, 0, len);
    if (table.length !== len) throw new Error(`${e.path}: short read`);
    lines.push(`${key} ${e.size} ${await sha256hex(table)}`);
  }
  const text = new TextEncoder().encode(lines.join('\n') + '\n');
  return { fingerprint: await sha256hex(text), blkFiles: blks.length, tableBytes: total };
}

// builds.json (tools/builds.json, copied into the site): shape-checked before use.
export function checkBuilds(b) {
  const isHex = (s) => typeof s === 'string' && /^[0-9a-f]{64}$/.test(s);
  const isId = (s) => typeof s === 'string' && s.length > 0 && s.length <= 64;
  if (!b || typeof b !== 'object' || !Array.isArray(b.accepted) || b.accepted.length > 64) throw new Error('builds.json: bad accepted list');
  for (const a of b.accepted) if (!isId(a.id) || !isHex(a.exe_sha256)) throw new Error('builds.json: bad accepted entry');
  const data = b.data ?? [];
  if (!Array.isArray(data) || data.length > 64) throw new Error('builds.json: bad data list');
  for (const d of data) if (!isId(d.id) || !isHex(d.blk_tables_sha256)) throw new Error('builds.json: bad data entry');
  const rejected = b.rejected ?? [];
  if (!Array.isArray(rejected) || rejected.length > 64) throw new Error('builds.json: bad rejected list');
  for (const r of rejected)
    if (!isId(r.id) || !isHex(r.exe_sha256) || typeof r.name !== 'string' || typeof r.reason !== 'string')
      throw new Error('builds.json: bad rejected entry');
  return { accepted: b.accepted, data, rejected };
}

// The verdict for an exe hash + data fingerprint. {ok, build, data, message}; message names the build on rejection.
export function classify(builds, exeSha, dataFp) {
  const short = exeSha.slice(0, 12);
  const rej = builds.rejected.find((r) => r.exe_sha256 === exeSha);
  if (rej) return { ok: false, message: `This is ${rej.name} (LEGOBionicle.exe SHA-256 ${short}…), which is not supported: ${rej.reason}` };
  const acc = builds.accepted.find((a) => a.exe_sha256 === exeSha);
  if (!acc)
    return { ok: false, message: `Unknown build: LEGOBionicle.exe SHA-256 ${short}… is not an accepted build. Only BIONICLE: The Legend of Mata Nui Beta 1.5.0 is supported (REBUILT and other mods are not).` };
  const data = builds.data.find((d) => d.blk_tables_sha256 === dataFp);
  // REF4 r2 decision D13 (controller ruling C2, option b): an accepted exe with blockfile tables no accepted build lists is
  // SOFT-accepted: every parser is bounded, so differing tables are a fidelity risk, not a safety one (the canonical
  // build's own data has never been fingerprinted). The page shows a persistent warning and the reason goes into the
  // metrics. The owner can tighten this to a rejection by adding the canonical fingerprint to builds.json `data`.
  if (!data) {
    const warning = `Game data differs from the known build — rendering may differ (.blk table fingerprint ${dataFp.slice(0, 12)}… is not in builds.json for "${acc.id}")`;
    return { ok: true, build: acc.id, data: 'unknown', warning, message: `Accepted with a warning: ${acc.id}, unknown data` };
  }
  return { ok: true, build: acc.id, data: data.id, message: `Accepted: ${acc.id} with ${data.id}` };
}
