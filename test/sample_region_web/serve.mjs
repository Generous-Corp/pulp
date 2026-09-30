import http from 'node:http';
import { readFile, realpath } from 'node:fs/promises';
import { dirname, resolve, extname } from 'node:path';
import { fileURLToPath } from 'node:url';

const fixture = dirname(fileURLToPath(import.meta.url));
export async function serve({ repo, wamDir, wclapDir, baselineWamDir, baselineWclapDir }) {
  const roots = await Promise.all([repo, wamDir, wclapDir, baselineWamDir, baselineWclapDir].map(p => realpath(p)));
  const [source, wam, wclap, gainWam, gainWclap] = roots;
  const routes = new Map([
    ['/', [fixture, 'index.html']], ['/proof.mjs', [fixture, 'proof.mjs']],
    ['/region/module.wasm', [wclap, 'SampleRegionAllpass.wasm']],
    ['/gain/module.wasm', [gainWclap, 'PulpGain.wasm']],
    ['/region/wam-dsp.js', [wam, 'SampleRegionAllpassWorklet.js']],
    ['/gain/wam-dsp.js', [gainWam, 'PulpGainWorklet.js']],
  ]);
  for (const name of ['wam-plugin.js', 'wam-runtime.mjs', 'wclap-host.mjs', 'wclap-wasi.mjs'])
    routes.set(`/host/${name}`, [source, `core/format/src/wasm/${name}`]);
  for (const prefix of ['region', 'gain'])
    for (const name of ['wam-processor.js', 'wam-runtime.mjs'])
      routes.set(`/${prefix}/${name}`, [source, `core/format/src/wasm/${name}`]);
  const types = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.wasm': 'application/wasm' };
  const server = http.createServer(async (req, res) => {
    try {
      if (req.method !== 'GET' && req.method !== 'HEAD') { res.writeHead(405).end(); return; }
      const pathname = new URL(req.url, 'http://127.0.0.1').pathname;
      const route = routes.get(pathname);
      if (!route) { res.writeHead(404).end('Not found'); return; }
      const [root, relative] = route;
      const target = await realpath(resolve(root, relative));
      if (!target.startsWith(`${root}/`)) { res.writeHead(404).end('Not found'); return; }
      const bytes = await readFile(target);
      res.writeHead(200, { 'Content-Type': types[extname(target)] || 'application/octet-stream',
        'Cross-Origin-Opener-Policy': 'same-origin', 'Cross-Origin-Embedder-Policy': 'require-corp',
        'Cross-Origin-Resource-Policy': 'same-origin', 'Cache-Control': 'no-store' });
      res.end(req.method === 'HEAD' ? undefined : bytes);
    } catch { res.writeHead(404).end('Not found'); }
  });
  await new Promise((ok, fail) => { server.once('error', fail); server.listen(0, '127.0.0.1', ok); });
  return { url: `http://127.0.0.1:${server.address().port}/`, close: () => new Promise(ok => server.close(ok)) };
}
