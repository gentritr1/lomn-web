// Minimal static server for tests: serves one directory, records every requested path.
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm', '.json': 'application/json' };
export function serve(root) {
  const requests = [];
  const server = http.createServer((req, res) => {
    const rel = decodeURIComponent(new URL(req.url, 'http://x').pathname);
    requests.push(rel);
    const file = path.join(root, rel === '/' ? 'index.html' : rel);
    if (!file.startsWith(root) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) { res.writeHead(404); return res.end(); }
    res.writeHead(200, { 'content-type': TYPES[path.extname(file)] || 'application/octet-stream' });
    fs.createReadStream(file).pipe(res);
  });
  return new Promise((resolve) => server.listen(0, '127.0.0.1', () =>
    resolve({ url: `http://127.0.0.1:${server.address().port}/`, requests, close: () => server.close() })));
}
