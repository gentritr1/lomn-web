// Spike (c): feed a real game folder to <input webkitdirectory> via setInputFiles(dir) in a persistent
// context; compare file count/bytes seen by the page with disk; time it. Run twice to see profile reuse.
import { chromium } from 'playwright';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
const dir = process.argv[2], profile = process.argv[3];
let n = 0, bytes = 0;
(function walk(d) { for (const e of fs.readdirSync(d, { withFileTypes: true })) {
  const f = path.join(d, e.name); if (e.isDirectory()) walk(f); else { n++; bytes += fs.statSync(f).size; } } })(dir);
const ctx = await chromium.launchPersistentContext(profile, { channel: 'chromium', args: ['--enable-unsafe-webgpu'], timeout: 30000 });
const page = ctx.pages()[0] ?? await ctx.newPage();
await page.setContent('<input id="d" type="file" webkitdirectory>');
const t0 = Date.now();
await page.locator('#d').setInputFiles(dir, { timeout: 120000 });
const seen = await page.evaluate(() => { const fl = [...document.getElementById('d').files];
  return { n: fl.length, bytes: fl.reduce((a, f) => a + f.size, 0), first: fl[0]?.webkitRelativePath }; });
console.log(JSON.stringify({ disk: { n, bytes }, page: seen, ms: Date.now() - t0, match: seen.n === n && seen.bytes === bytes }));
await ctx.close();
