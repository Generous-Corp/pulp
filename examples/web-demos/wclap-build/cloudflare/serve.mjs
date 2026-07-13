#!/usr/bin/env node
// Serve the assembled site locally, with the REAL headers.
//
//   node serve.mjs [--dir public] [--port 8788]
//
// Not a convenience wrapper around `python3 -m http.server`: that would serve the pages
// and then silently break them. The WebCLAP demos need CROSS-ORIGIN ISOLATION —
// COOP: same-origin + COEP: require-corp — because SharedArrayBuffer is gated on it, and
// SharedArrayBuffer is how the audio thread reaches both the wasm heap and (on the GPU
// page) the DedicatedWorker that owns the WebGPU device. Serve them without these headers
// and `crossOriginIsolated` is false, the SAB constructor throws, and the page reports
// "not-cross-origin-isolated" — a failure that looks like a bug in the demo and is a bug
// in the server.
//
// These are the same headers `_headers` tells Cloudflare to send, which is the point:
// what you test locally is what ships.

import { createServer } from "node:http";
import { existsSync, readFileSync, statSync } from "node:fs";
import { extname, join, resolve } from "node:path";

const arg = (k, d) => {
  const i = process.argv.indexOf(k);
  return i > 0 && process.argv[i + 1] ? process.argv[i + 1] : d;
};

const DIR = resolve(arg("--dir", "public"));
const PORT = Number(arg("--port", "8788"));

if (!existsSync(DIR)) {
  console.error(`error: ${DIR} does not exist — assemble the site first:\n` +
                `  node assemble-gallery.mjs --build <wclap-build> --wam-build <…> --ui-build <…> --gpu-build <…>`);
  process.exit(2);
}

const MIME = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript", ".mjs": "text/javascript",
  ".wasm": "application/wasm",
  ".data": "application/octet-stream",
  ".json": "application/json", ".css": "text/css",
  ".png": "image/png", ".svg": "image/svg+xml", ".wav": "audio/wav",
};

createServer((req, res) => {
  const url = (req.url || "/").split("?")[0];
  let p = join(DIR, decodeURIComponent(url));
  try {
    if (existsSync(p) && statSync(p).isDirectory()) p = join(p, "index.html");
  } catch { /* fall through to the 404 */ }
  if (!existsSync(p)) {
    res.writeHead(404, { "Content-Type": "text/plain" });
    res.end("not found: " + url);
    return;
  }
  res.writeHead(200, {
    "Content-Type": MIME[extname(p)] || "application/octet-stream",
    // The three that make SharedArrayBuffer legal. Mirrors ./_headers.
    "Cross-Origin-Opener-Policy": "same-origin",
    "Cross-Origin-Embedder-Policy": "require-corp",
    "Cross-Origin-Resource-Policy": "cross-origin",
    "Cache-Control": "no-store",
  });
  res.end(readFileSync(p));
}).listen(PORT, () => {
  console.log(`serving ${DIR}`);
  console.log(`  http://localhost:${PORT}/                       the gallery`);
  console.log(`  http://localhost:${PORT}/super-convolver/wclap/ SuperConvolver (WebCLAP)`);
  console.log(`  http://localhost:${PORT}/super-convolver/wam/   SuperConvolver (WAM)`);
  console.log(`  http://localhost:${PORT}/super-convolver-gpu/   the WebGPU compute engine`);
  console.log(`\ncross-origin isolated: yes (COOP/COEP set, same as the real host)`);
});
