// Assembles the static site: web/index.html, web/app/*.js, the wasm build and builds.json, plus asset-manifest.json
// (every file with its byte size and SHA-256; the network-discipline test allows exactly these URLs).
// Content hashing (for a later deploy, parent spec §4.6): every JS and wasm file is published as <name>.<10 hex of the
// SHA-256 of its published bytes>.<ext>, and the references to it (quoted relative specifiers in the JS, the script src in
// index.html) are rewritten first, leaves before importers, so a hash covers its dependencies' names. index.html,
// builds.json and asset-manifest.json keep stable names (they must be revalidated). There is no CSS file: the page's CSS
// is inline in index.html. Deploy headers (immutable for hashed names) are not written here: deploy is out of scope.
// Usage: node web/tools/build_site.mjs <wasm_build_dir> <out_dir> [--extra-builds file.json]
//   --extra-builds: merges test-only accepted/data/rejected entries (synthetic fixtures) into builds.json. Never used
//   for the real site (make site).
import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const web = path.resolve(here, '..');
const repo = path.resolve(web, '..');
const args = process.argv.slice(2);
const [wasmDir, outDir] = args;
if (!wasmDir || !outDir) {
  console.error('usage: build_site.mjs <wasm_build_dir> <out_dir> [--extra-builds file.json]');
  process.exit(2);
}
const extraAt = args.indexOf('--extra-builds');
const extra = extraAt > 0 ? JSON.parse(fs.readFileSync(args[extraAt + 1], 'utf8')) : null;

fs.rmSync(outDir, { recursive: true, force: true });
fs.mkdirSync(path.join(outDir, 'app'), { recursive: true });
// Sources, in hashing order: a file comes after every file it references.
const ORDER = ['app/fingerprint.js', 'app/idb.js', 'app/paths.js', 'app/opfs_worker.js', 'lomn_web.wasm', 'lomn_web.js', 'app/byo.js', 'app/engine.js', 'app/main.js'];
const srcOf = (rel) => (rel.startsWith('app/') ? path.join(web, rel) : path.join(wasmDir, rel));
const appFiles = fs.readdirSync(path.join(web, 'app')).filter((f) => f.endsWith('.js')).map((f) => 'app/' + f).sort();
const unknown = appFiles.filter((f) => !ORDER.includes(f));
if (unknown.length) throw new Error('build_site: add these to ORDER (after what they import): ' + unknown.join(', '));
const hashed = new Map();  // rel -> published rel
const relSpec = (fromRel, toRel) => {
  const r = path.posix.relative(path.posix.dirname(fromRel), toRel);
  return r.startsWith('.') ? r : './' + r;
};
const rewrite = (fromRel, text) => {
  for (const [orig, pub] of hashed) {
    const a = relSpec(fromRel, orig), b = relSpec(fromRel, pub);
    for (const q of ["'", '"']) text = text.split(q + a + q).join(q + b + q);
  }
  return text;
};
for (const rel of ORDER) {
  let bytes = fs.readFileSync(srcOf(rel));
  if (rel.endsWith('.js')) bytes = Buffer.from(rewrite(rel, bytes.toString('utf8')));
  const h = crypto.createHash('sha256').update(bytes).digest('hex').slice(0, 10);
  const ext = path.extname(rel);
  const pub = rel.slice(0, -ext.length) + '.' + h + ext;
  fs.writeFileSync(path.join(outDir, pub), bytes);
  hashed.set(rel, pub);
}
let html = fs.readFileSync(path.join(web, 'index.html'), 'utf8');
for (const [orig, pub] of hashed) html = html.split(`src="${orig}"`).join(`src="${pub}"`);
fs.writeFileSync(path.join(outDir, 'index.html'), html);
// Every reference must have been rewritten: no published file may still name an unhashed source.
for (const pub of [...hashed.values(), 'index.html']) {
  if (!pub.endsWith('.js') && pub !== 'index.html') continue;
  const text = fs.readFileSync(path.join(outDir, pub), 'utf8');
  for (const orig of hashed.keys()) {
    const spec = pub === 'index.html' ? `"${orig}"` : `'${relSpec(pub, orig)}'`;
    if (text.includes(spec)) throw new Error(`build_site: ${pub} still references ${spec}`);
  }
}
const builds = JSON.parse(fs.readFileSync(path.join(repo, 'tools', 'builds.json'), 'utf8'));
if (extra) for (const k of ['accepted', 'data', 'rejected']) builds[k] = [...(builds[k] || []), ...(extra[k] || [])];
fs.writeFileSync(path.join(outDir, 'builds.json'), JSON.stringify(builds, null, 1) + '\n');

const files = [];
const walk = (dir, prefix) => {
  for (const f of fs.readdirSync(dir).sort()) {
    const p = path.join(dir, f);
    if (fs.statSync(p).isDirectory()) walk(p, prefix + f + '/');
    else {
      const rel = prefix + f;
      const source = [...hashed].find(([, pub]) => pub === rel)?.[0];
      files.push({ path: rel, bytes: fs.statSync(p).size, sha256: crypto.createHash('sha256').update(fs.readFileSync(p)).digest('hex'), ...(source ? { source } : {}) });
    }
  }
};
walk(outDir, '');
const manifest = { format: 'lomn-asset-manifest', version: 1, files, total_bytes: files.reduce((n, f) => n + f.bytes, 0) };
const text = JSON.stringify(manifest, null, 1) + '\n';
fs.writeFileSync(path.join(outDir, 'asset-manifest.json'), text);
console.log(`site: ${files.length + 1} files, ${manifest.total_bytes + Buffer.byteLength(text)} bytes -> ${outDir}`);
