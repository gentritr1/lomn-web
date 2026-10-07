// Spike (b): which headless Chromium launch config yields a working WebGPU adapter + rendered canvas?
// Serves the web spike from a tiny static server; one browser launch per config; state waits, no sleeps.
import { chromium } from 'playwright';
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
const root = process.argv[2];
const types = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm' };
const server = http.createServer((req, res) => {
  const f = path.join(root, decodeURIComponent(new URL(req.url, 'http://x').pathname));
  if (!fs.existsSync(f) || fs.statSync(f).isDirectory()) { res.writeHead(404); return res.end(); }
  res.writeHead(200, { 'content-type': types[path.extname(f)] || 'application/octet-stream' });
  fs.createReadStream(f).pipe(res);
}).listen(0);
const url = `http://127.0.0.1:${server.address().port}/index.html`;
const configs = [
  { name: 'headless-shell, no flags', opts: {} },
  { name: 'headless-shell, unsafe-webgpu', opts: { args: ['--enable-unsafe-webgpu'] } },
  { name: 'chromium new-headless, unsafe-webgpu', opts: { channel: 'chromium', args: ['--enable-unsafe-webgpu'] } },
];
for (const c of configs) {
  let line = `${c.name}: `;
  let browser;
  try {
    browser = await chromium.launch({ ...c.opts, timeout: 30000 });
    const page = await browser.newPage();
    const info = await page.goto(url).then(() => page.evaluate(async () => {
      if (!navigator.gpu) return 'no navigator.gpu';
      const a = await navigator.gpu.requestAdapter();
      return a ? `adapter ${a.info?.vendor}/${a.info?.architecture}/${a.info?.description}` : 'null adapter';
    }));
    line += info;
    await page.waitForFunction(() => window.__lomn?.state, null, { timeout: 15000 });
    const state = await page.evaluate(() => window.__lomn.state);
    const png = await page.locator('#canvas').screenshot();
    line += ` | state=${state} | png ${png.length}B`;
    fs.writeFileSync(path.join(root, `shot-${configs.indexOf(c)}.png`), png);
  } catch (e) { line += ` | ERROR ${String(e.message).split('\n')[0]}`; }
  finally { await browser?.close(); }
  console.log(line);
}
server.close();
