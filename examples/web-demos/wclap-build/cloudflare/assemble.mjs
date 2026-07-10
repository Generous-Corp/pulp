// Assemble a self-contained Cloudflare Pages deploy directory for the WebCLAP
// PulpGain demo.
//
// The in-repo browser host (../browser-host/{index.html,main.js}) imports the
// SDK JS modules and fetches the wasm through ABSOLUTE repo-root paths
// (/core/format/src/wasm/wclap-host.mjs, /examples/.../PulpGain.wasm). Those
// resolve under browser-host/serve.mjs (which serves from the repo root) but
// would 404 on a static host that only sees {index.html, main.js, wasm}. So
// this script gathers every module the host actually imports into ONE flat
// directory and rewrites the absolute paths to relative ones — producing a
// directory that serves standalone on Cloudflare Pages (or any static host that
// honours the sibling _headers file).
//
// Usage:
//   node assemble.mjs [--wasm <path>] [--out <dir>]
// Defaults: --wasm ../build/PulpGain.wasm  --out ./public
// Exit 0 = assembled.  Fails loudly if an expected source string is missing
// (so a refactor of the browser host can't silently ship a broken deploy).
import { readFile, writeFile, mkdir, copyFile, rm } from "node:fs/promises";
import { existsSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join, resolve } from "node:path";

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = resolve(HERE, "../../../..");
const BROWSER_HOST = resolve(HERE, "../browser-host");
const WASM_MODULES = resolve(REPO, "core/format/src/wasm");

function arg(flag, dflt) {
  const i = process.argv.indexOf(flag);
  return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : dflt;
}

// wasm source: prefer the CI build output, fall back to the packaged bundle.
const wasmCandidates = [
  arg("--wasm", null),
  resolve(HERE, "../build/PulpGain.wasm"),
  resolve(HERE, "../build/PulpGain.wclap/module.wasm"),
].filter(Boolean);
const wasmSrc = wasmCandidates.find((p) => existsSync(p));
const OUT = resolve(HERE, arg("--out", "./public"));

const die = (m) => { console.error("assemble: FAIL: " + m); process.exit(1); };
if (!wasmSrc) {
  die(`no WebCLAP wasm found. Build it first:\n` +
      `  cd ${resolve(HERE, "..")}\n` +
      `  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=../../../tools/cmake/wasi-toolchain.cmake -DCMAKE_BUILD_TYPE=Release\n` +
      `  cmake --build build\n` +
      `(needs wasi-sdk; set WASI_SDK_PREFIX if not at /opt/wasi-sdk)`);
}

// Rewrite the host's absolute imports/fetches to sibling-relative paths.
function rewriteMainJs(src) {
  const edits = [
    ['from "/core/format/src/wasm/wclap-host.mjs"', 'from "./wclap-host.mjs"'],
    ['"/examples/web-demos/wclap-build/build/PulpGain.wasm"', '"./PulpGain.wasm"'],
  ];
  let out = src;
  for (const [needle, repl] of edits) {
    if (!out.includes(needle)) {
      die(`expected string not found in browser-host/main.js:\n  ${needle}\n` +
          `The browser host changed — update assemble.mjs to match.`);
    }
    out = out.replace(needle, repl);
  }
  // Belt-and-suspenders: no absolute repo-root path may survive.
  const leaked = out.match(/["'`]\/(core|examples)\//g);
  if (leaked) die(`absolute repo-root path(s) survived rewrite: ${[...new Set(leaked)].join(", ")}`);
  return out;
}

await rm(OUT, { recursive: true, force: true });
await mkdir(OUT, { recursive: true });

// index.html already references ./main.js (relative). Inject an empty data-URI
// favicon so the static host serves no stray /favicon.ico 404 (the request would
// otherwise be a spurious non-200 subresource under COEP scrutiny).
const indexHtml = await readFile(join(BROWSER_HOST, "index.html"), "utf8");
if (!indexHtml.includes("</head>")) die("browser-host/index.html has no </head> to inject favicon into");
await writeFile(join(OUT, "index.html"),
  indexHtml.replace("</head>", '  <link rel="icon" href="data:,">\n</head>'));

// main.js — rewrite absolute paths.
const mainJs = await readFile(join(BROWSER_HOST, "main.js"), "utf8");
await writeFile(join(OUT, "main.js"), rewriteMainJs(mainJs));

// SDK JS modules the host imports (wclap-host imports ./wclap-wasi — already relative).
await copyFile(join(WASM_MODULES, "wclap-host.mjs"), join(OUT, "wclap-host.mjs"));
await copyFile(join(WASM_MODULES, "wclap-wasi.mjs"), join(OUT, "wclap-wasi.mjs"));

// The WebCLAP module.
await copyFile(wasmSrc, join(OUT, "PulpGain.wasm"));

// The headers that make it cross-origin isolated on Cloudflare Pages.
await copyFile(join(HERE, "_headers"), join(OUT, "_headers"));

const rel = (p) => p.replace(REPO + "/", "");
console.log("assemble: wrote self-contained deploy dir → " + rel(OUT));
console.log("  index.html        (verbatim)");
console.log("  main.js           (absolute paths → ./ )");
console.log("  wclap-host.mjs    (from core/format/src/wasm)");
console.log("  wclap-wasi.mjs    (from core/format/src/wasm)");
console.log("  PulpGain.wasm     (from " + rel(wasmSrc) + ")");
console.log("  _headers          (COOP/COEP/CORP + MIME)");
