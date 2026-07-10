// COOP/COEP dev server for the WS-C2 real-time-worklet spike.
//
// Header setup mirrors examples/web-demos/wclap-build/browser-host/serve.mjs:
// the WebCLAP module imports a shared WebAssembly.Memory (created inside the
// AudioWorklet here), which the browser only allows on a cross-origin-isolated
// page, and .mjs/.js must be text/javascript, .wasm application/wasm. The
// AudioWorklet script is a classic .js, so it also needs CORP under COEP.
//
// Serves from the repo root so absolute imports resolve; the spike itself only
// uses relative paths under realtime-spike/.
//
// Usage: node serve.mjs [port]   (default 8790). Open
//   http://localhost:8790/examples/web-demos/wclap-build/realtime-spike/
import { createServer } from "node:http";
import { readFile } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import { extname, join, normalize } from "node:path";

const ROOT = fileURLToPath(new URL("../../../../", import.meta.url)); // repo root
const PORT = Number(process.argv[2] || 8790);

const MIME = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".mjs": "text/javascript; charset=utf-8",
  ".wasm": "application/wasm",
  ".json": "application/json",
  ".css": "text/css; charset=utf-8",
};

const server = createServer(async (req, res) => {
  res.setHeader("Cross-Origin-Opener-Policy", "same-origin");
  res.setHeader("Cross-Origin-Embedder-Policy", "require-corp");
  res.setHeader("Cross-Origin-Resource-Policy", "cross-origin");

  let pathname = decodeURIComponent(new URL(req.url, "http://x").pathname);
  if (pathname.endsWith("/")) pathname += "index.html";
  const filePath = normalize(join(ROOT, pathname));
  if (!filePath.startsWith(ROOT)) { res.writeHead(403).end("forbidden"); return; }
  try {
    const body = await readFile(filePath);
    res.writeHead(200, { "Content-Type": MIME[extname(filePath)] || "application/octet-stream" });
    res.end(body);
  } catch {
    res.writeHead(404).end("not found");
  }
});

server.listen(PORT, () => {
  console.log(`WS-C2 spike: http://localhost:${PORT}/examples/web-demos/wclap-build/realtime-spike/`);
});
