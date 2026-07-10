#!/usr/bin/env node
// pack-smoke.mjs — prove @danielraffel/web-player works when installed FROM A TARBALL,
// not from the source tree. This is the dogfood-second half: `npm pack`, install
// the tarball into a fresh scratch consumer, drop a REAL built Pulp WAM plugin
// beside a page that imports `mountDemo` from the installed package alone, and
// serve it so a headless browser can drive the test seams
// (window.__start / __demo / __player / __memo) and confirm audio + a param +
// a PLST state round-trip.
//
// This script does the pack + install + scaffold + static server. Driving the
// headless browser against it is done by the harness caller (CI: puppeteer-core
// pinned; locally: any headless Chrome). It intentionally has ZERO npm deps.
//
// Usage:
//   node scripts/pack-smoke.mjs \
//     --demo /path/to/built/state-memo \   # dir with wam-dsp.js + wam-processor.js + wam-runtime.mjs
//     --out  /path/to/scratch/dir     \    # scratch consumer root (default: OS tmp)
//     --port 8791                      \   # static server port (default 8791)
//     [--serve]                            # keep the server running (else just scaffold)
//
// Env equivalents: SMOKE_DEMO, SMOKE_OUT, SMOKE_PORT.

import { execFileSync } from "node:child_process";
import { createServer } from "node:http";
import { readFile, mkdir, rm, cp, writeFile } from "node:fs/promises";
import { existsSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join, extname, resolve } from "node:path";
import { tmpdir } from "node:os";

const HERE = dirname(fileURLToPath(import.meta.url));
const PKG_DIR = resolve(HERE, "..");

// ——— args
const arg = (name, env, dflt) => {
  const i = process.argv.indexOf(`--${name}`);
  if (i >= 0 && process.argv[i + 1]) return process.argv[i + 1];
  return process.env[env] ?? dflt;
};
const DEMO = arg("demo", "SMOKE_DEMO", null);
const OUT = resolve(arg("out", "SMOKE_OUT", join(tmpdir(), "pulp-web-player-smoke")));
const PORT = +arg("port", "SMOKE_PORT", "8791");
const SERVE = process.argv.includes("--serve");

if (!DEMO || !existsSync(DEMO)) {
  console.error(`error: --demo <dir> is required and must exist (a built WAM plugin dir).`);
  console.error(`  needs: wam-dsp.js, wam-processor.js, wam-runtime.mjs`);
  process.exit(2);
}
for (const f of ["wam-dsp.js", "wam-processor.js", "wam-runtime.mjs"]) {
  if (!existsSync(join(DEMO, f))) { console.error(`error: ${DEMO} is missing ${f}`); process.exit(2); }
}

const run = (cmd, args, cwd) => {
  console.log(`   $ ${cmd} ${args.join(" ")}   (in ${cwd})`);
  return execFileSync(cmd, args, { cwd, encoding: "utf8", stdio: ["ignore", "pipe", "inherit"] });
};

async function main() {
  console.log(`==> package : ${PKG_DIR}`);
  console.log(`==> demo    : ${DEMO}`);
  console.log(`==> scratch : ${OUT}`);

  await rm(OUT, { recursive: true, force: true });
  await mkdir(OUT, { recursive: true });

  // 1. npm pack the package into the scratch dir.
  const packOut = run("npm", ["pack", "--json", "--pack-destination", OUT], PKG_DIR);
  const tarball = join(OUT, JSON.parse(packOut)[0].filename);
  console.log(`==> tarball : ${tarball}`);

  // 2. Fresh consumer, install the tarball ONLY.
  const consumer = join(OUT, "consumer");
  await mkdir(consumer, { recursive: true });
  await writeFile(join(consumer, "package.json"), JSON.stringify({
    name: "pulp-web-player-smoke-consumer", version: "0.0.0", private: true, type: "module",
  }, null, 2) + "\n");
  run("npm", ["install", "--no-audit", "--no-fund", tarball], consumer);

  // Resolve the installed package name from its manifest (so the import map is right).
  const installedPkg = JSON.parse(
    await readFile(join(consumer, "node_modules", ".package-lock.json"), "utf8").catch(() => "{}"));
  const pkgName = JSON.parse(await readFile(join(PKG_DIR, "package.json"), "utf8")).name;
  const pkgEntry = `/node_modules/${pkgName}/src/index.js`;
  console.log(`==> installed: ${pkgName} → ${pkgEntry}`);
  void installedPkg;

  // 3. The consumer page + the real built plugin, co-located at the web root.
  const pub = join(consumer, "public");
  await mkdir(pub, { recursive: true });
  for (const f of ["wam-dsp.js", "wam-processor.js", "wam-runtime.mjs"]) {
    await cp(join(DEMO, f), join(pub, f));
  }

  const html = `<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <title>pack-smoke — @danielraffel/web-player</title>
  <script type="importmap">
  { "imports": { "${pkgName}": "${pkgEntry}" } }
  </script>
</head>
<body>
<div id="app"></div>
<script type="module">
  import { mountDemo } from "${pkgName}";
  // A real built Pulp WAM plugin (state-memo): audio-effect with a Volume param
  // AND a PLST-backed text memo — exercises audio + a param + a state round-trip.
  mountDemo({
    root: document.getElementById("app"),
    title: "Pack Smoke — State Memo",
    subtitle: "Installed from the @danielraffel/web-player tarball alone.",
    dspUrl: "./wam-dsp.js", processorUrl: "./wam-processor.js",
    mode: "audio-effect", paramRows: 1,
    stateMemo: true,
    theme: "dark",
  });
  window.__smokeReady = true;
</script>
</body>
</html>
`;
  await writeFile(join(pub, "index.html"), html);

  // 4. A tiny static server with correct ES-module / wasm / font MIME types.
  const MIME = {
    ".html": "text/html; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".mjs": "text/javascript; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".json": "application/json; charset=utf-8",
    ".wasm": "application/wasm",
    ".woff2": "font/woff2",
    ".ttf": "font/ttf",
    ".png": "image/png",
    ".svg": "image/svg+xml",
  };
  // Two roots: the web root is public/ (so ./wam-* resolve), but /node_modules/*
  // is served from the consumer dir (so the bare-specifier import resolves).
  const serve = () => {
    const server = createServer(async (req, res) => {
      try {
        const url = decodeURIComponent(req.url.split("?")[0]);
        const base = url.startsWith("/node_modules/") ? consumer : pub;
        const rel = url === "/" ? "index.html" : url.replace(/^\/+/, "");
        const file = join(base, rel);
        if (!file.startsWith(base)) { res.writeHead(403).end("forbidden"); return; }
        const body = await readFile(file);
        res.writeHead(200, { "content-type": MIME[extname(file)] || "application/octet-stream" });
        res.end(body);
      } catch {
        res.writeHead(404).end("not found");
      }
    });
    server.listen(PORT, () => {
      console.log(`\n==> serving http://localhost:${PORT}/  (Ctrl-C to stop)`);
      console.log(`    test seams: window.__start(), window.__demo, window.__player, window.__memo`);
    });
  };

  console.log(`\nOK: packed + installed + scaffolded.`);
  console.log(`   consumer: ${consumer}`);
  console.log(`   page    : ${join(pub, "index.html")}`);
  if (SERVE) serve();
  else console.log(`   (re-run with --serve to start the static server)`);
}

main().catch((err) => { console.error(err); process.exit(1); });
