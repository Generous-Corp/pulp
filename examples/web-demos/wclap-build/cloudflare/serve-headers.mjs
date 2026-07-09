// Static server that applies a Cloudflare Pages `_headers` file — so the LOCAL
// proof runs under the exact header rules Cloudflare will serve in production.
//
// Unlike browser-host/serve.mjs (which hard-codes the headers), this parses the
// assembled deploy dir's `_headers` and applies every matching rule block. If
// `_headers` is wrong (e.g. missing CORP, or the wrong MIME for .mjs/.wasm),
// the page fails here exactly as it would on Cloudflare — that's the point.
//
// Content-Type is taken ONLY from `_headers` (plus a text/html fallback for the
// document, which Cloudflare provides implicitly); anything else with no MIME
// rule is served application/octet-stream, so a subresource missing its rule
// surfaces immediately.
//
// Usage: node serve-headers.mjs [--dir <deployDir>] [--port <n>]
// Defaults: --dir ./public  --port 8790
import { createServer } from "node:http";
import { readFile } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import { dirname, extname, join, normalize, resolve } from "node:path";

const HERE = dirname(fileURLToPath(import.meta.url));
function arg(flag, dflt) {
  const i = process.argv.indexOf(flag);
  return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : dflt;
}
const DIR = resolve(HERE, arg("--dir", "./public"));
const PORT = Number(arg("--port", 8790));

// ── parse Cloudflare `_headers` ─────────────────────────────────────────────
// Format: a line beginning with `/` opens a path pattern; the indented
// `Name: value` lines under it are that pattern's headers. `#` lines and blank
// lines are ignored.
function parseHeaders(text) {
  const rules = [];
  let cur = null;
  for (const raw of text.split("\n")) {
    const line = raw.replace(/\r$/, "");
    if (!line.trim() || line.trim().startsWith("#")) continue;
    if (line.startsWith("/")) {
      cur = { pattern: line.trim(), headers: [] };
      rules.push(cur);
    } else if (cur) {
      const idx = line.indexOf(":");
      if (idx > 0) cur.headers.push([line.slice(0, idx).trim(), line.slice(idx + 1).trim()]);
    }
  }
  return rules;
}
// Cloudflare glob: `*` → any run of chars (incl. `/`), `:splat`-style ignored here.
function patternToRegex(pattern) {
  const esc = pattern.replace(/[.+?^${}()|[\]\\]/g, "\\$&").replace(/\*/g, ".*");
  return new RegExp("^" + esc + "$");
}

const rules = parseHeaders(await readFile(join(DIR, "_headers"), "utf8"))
  .map((r) => ({ ...r, re: patternToRegex(r.pattern) }));

const FALLBACK_MIME = { ".html": "text/html; charset=utf-8" };

const server = createServer(async (req, res) => {
  let pathname = decodeURIComponent(new URL(req.url, "http://x").pathname);
  if (pathname.endsWith("/")) pathname += "index.html";
  const filePath = normalize(join(DIR, pathname));
  if (!filePath.startsWith(DIR)) { res.writeHead(403).end("forbidden"); return; }

  // Collect headers from every matching `_headers` rule (later rules win per name).
  const applied = new Map();
  for (const r of rules) {
    if (r.re.test(pathname)) for (const [k, v] of r.headers) applied.set(k.toLowerCase(), [k, v]);
  }
  let contentType = null;
  for (const [lk, [k, v]] of applied) {
    if (lk === "content-type") { contentType = v; continue; }
    res.setHeader(k, v);
  }
  if (!contentType) contentType = FALLBACK_MIME[extname(filePath)] || "application/octet-stream";
  res.setHeader("Content-Type", contentType);

  try {
    const body = await readFile(filePath);
    res.writeHead(200);
    res.end(body);
  } catch {
    res.writeHead(404).end("not found");
  }
});

server.listen(PORT, () => {
  console.log(`WebCLAP deploy (under _headers): http://localhost:${PORT}/`);
});
