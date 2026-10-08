// Minimal static server for tests: serves one directory, records every requested path and (M1b) every response's
// status and body bytes, so the network-discipline check can sum what the page downloaded.
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.wasm': 'application/wasm', '.json': 'application/json' };
export function serve(rootArg) {
  const root = path.resolve(rootArg);
  const requests = [];
  const responses = [];  // {path, status, bytes}
  const server = http.createServer((req, res) => {
    let rel;
    try { rel = decodeURIComponent(new URL(req.url, 'http://x').pathname); }
    catch { responses.push({ path: req.url, status: 400, bytes: 0 }); res.writeHead(400); return res.end(); }   // malformed %-escape: answer 400, never throw
    requests.push(rel);
    if (rel === '/') rel = '/index.html';
    const file = path.resolve(root, '.' + rel);
    const inside = file === root || file.startsWith(root + path.sep);   // `root + sep`: "site-secret" is not inside "site"
    if (!inside || !fs.existsSync(file) || fs.statSync(file).isDirectory()) {
      responses.push({ path: rel, status: 404, bytes: 0 });
      res.writeHead(404);
      return res.end();
    }
    const bytes = fs.statSync(file).size;
    responses.push({ path: rel, status: 200, bytes });
    res.writeHead(200, { 'content-type': TYPES[path.extname(file)] || 'application/octet-stream', 'content-length': bytes, 'cache-control': 'no-store' });
    fs.createReadStream(file).pipe(res);
  });
  return new Promise((resolve) => server.listen(0, '127.0.0.1', () =>
    resolve({ url: `http://127.0.0.1:${server.address().port}/`, requests, responses, close: () => { server.close(); server.closeAllConnections(); } })));
}
