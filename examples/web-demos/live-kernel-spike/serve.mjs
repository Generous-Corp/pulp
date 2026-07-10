// Static dev server for the Pulp Live Kernel S0 spike.
//
// The kernel is a SINGLE-THREAD standalone wasm (no SharedArrayBuffer, no
// threads), so — unlike the WS-C2 WebCLAP spike — this page needs NO COOP/COEP /
// cross-origin isolation. Just correct MIME types. Serves from the repo root so
// the page's imports and the /examples/... wasm path resolve.
//
// Usage: node serve.mjs [port]   (default 8793). Open
//   http://localhost:8793/examples/web-demos/live-kernel-spike/
import { createServer } from "node:http";
import { readFile } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import { extname, join, normalize } from "node:path";

const ROOT = fileURLToPath(new URL("../../../", import.meta.url)); // repo root
const PORT = Number(process.argv[2] || 8793);

const MIME = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".mjs": "text/javascript; charset=utf-8",
  ".wasm": "application/wasm",
  ".json": "application/json",
  ".css": "text/css; charset=utf-8",
};

const server = createServer(async (req, res) => {
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
  console.log(`live-kernel spike: http://localhost:${PORT}/examples/web-demos/live-kernel-spike/`);
});
