// Minimal static file server for the WebCLAP browser host.
//
// Two things a plain `python -m http.server` gets wrong for this page:
//   1. Cross-origin isolation. The WebCLAP module imports a shared
//      `WebAssembly.Memory`, which the browser only allows on a cross-origin
//      isolated page. This server sends the required COOP/COEP headers.
//   2. MIME types. ES modules (.mjs/.js) must be served as text/javascript and
//      the module as application/wasm, or the browser refuses to load them.
//
// Serves from the repo root so the page's absolute imports
// (/core/format/src/wasm/wclap-host.mjs, /examples/.../PulpGain.wasm) resolve.
//
// Usage: node serve.mjs [port]   (default 8787). Open
//   http://localhost:8787/examples/web-demos/wclap-build/browser-host/
import { createServer } from "node:http";
import { readFile } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import { extname, join, normalize } from "node:path";

const ROOT = fileURLToPath(new URL("../../../../", import.meta.url)); // repo root
const PORT = Number(process.argv[2] || 8787);

const MIME = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".mjs": "text/javascript; charset=utf-8",
  ".wasm": "application/wasm",
  ".json": "application/json",
  ".css": "text/css; charset=utf-8",
};

const server = createServer(async (req, res) => {
  // Cross-origin isolation — required for the module's shared memory.
  res.setHeader("Cross-Origin-Opener-Policy", "same-origin");
  res.setHeader("Cross-Origin-Embedder-Policy", "require-corp");
  res.setHeader("Cross-Origin-Resource-Policy", "cross-origin");

  let pathname = decodeURIComponent(new URL(req.url, "http://x").pathname);
  if (pathname.endsWith("/")) pathname += "index.html";
  // Contain the path to ROOT.
  const filePath = normalize(join(ROOT, pathname));
  if (!filePath.startsWith(ROOT)) {
    res.writeHead(403).end("forbidden");
    return;
  }
  try {
    const body = await readFile(filePath);
    res.writeHead(200, { "Content-Type": MIME[extname(filePath)] || "application/octet-stream" });
    res.end(body);
  } catch {
    res.writeHead(404).end("not found");
  }
});

server.listen(PORT, () => {
  console.log(`WebCLAP browser host: http://localhost:${PORT}/examples/web-demos/wclap-build/browser-host/`);
});
