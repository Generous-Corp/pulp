// serve.mjs — tiny static file server rooted at the REPO ROOT.
//
// Rooted at the repo root (not the measure dir) so a page can import both the
// harness libs (examples/web-demos/compiler-spike/measure/...) AND the shipped
// wam-runtime.mjs (core/format/src/wasm/...) AND load built wasm from anywhere
// under the tree with absolute paths. COOP/COEP are set (harmless for the
// single-thread WAM build; present so a future SAB/threaded kernel still loads).
//
// Usage: node serve.mjs [port]   — or import { startServer } from it.
import http from "node:http";
import { readFile } from "node:fs/promises";
import { extname, join, normalize, resolve, relative } from "node:path";

// repo root = five levels up from .../measure/lib/serve.mjs
export const REPO_ROOT = normalize(new URL("../../../../../", import.meta.url).pathname);

// Map a filesystem path (abs or cwd-relative) to the URL path this server serves
// it at (root = REPO_ROOT). Throws if the path escapes the repo.
export function toServedUrl(path) {
  const abs = resolve(path);
  const rel = relative(REPO_ROOT.replace(/\/$/, ""), abs);
  if (rel.startsWith("..")) throw new Error(`path is outside the repo root: ${path}`);
  return "/" + rel.split("\\").join("/");
}

const MIME = {
  ".html": "text/html", ".js": "text/javascript", ".mjs": "text/javascript",
  ".wasm": "application/wasm", ".json": "application/json", ".css": "text/css",
};

export function startServer(port = 8794, root = REPO_ROOT) {
  const rootAbs = normalize(root.endsWith("/") ? root : root + "/");
  const srv = http.createServer(async (req, res) => {
    let p = decodeURIComponent(req.url.split("?")[0]);
    if (p === "/favicon.ico") { res.writeHead(204); return res.end(); }
    if (p === "/") p = "/index.html";
    const fp = normalize(join(rootAbs, p));
    if (!fp.startsWith(rootAbs)) { res.writeHead(403); return res.end(); }
    try {
      const data = await readFile(fp);
      res.writeHead(200, {
        "content-type": MIME[extname(fp)] || "application/octet-stream",
        "cross-origin-opener-policy": "same-origin",
        "cross-origin-embedder-policy": "require-corp",
        "cache-control": "no-store",
      });
      res.end(data);
    } catch { res.writeHead(404); res.end("not found: " + p); }
  });
  return new Promise((resolve) => srv.listen(port, () => resolve(srv)));
}

// Run directly: node serve.mjs [port]
if (import.meta.url === `file://${process.argv[1]}`) {
  const port = Number(process.argv[2] || 8794);
  startServer(port).then(() => console.log(`measure server on http://localhost:${port}/ (root ${REPO_ROOT})`));
}
