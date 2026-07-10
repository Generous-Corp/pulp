// Assemble the shared-player WCLAP demo into the Cloudflare Pages deploy dir.
//
// WS-C2 headline: the SAME @danielraffel/web-player shell the WAM demos use, hosting a
// WCLAP plugin (PulpGain) in real time via the worklet-resident CLAP host. It
// writes a self-contained `public/player/` containing:
//   • index.html                       — the committed player/index.html (mounts
//                                          the shell with the WCLAP adapter).
//   • pulp-web-player/**                — the package src tree (shell, widgets,
//                                          theme, adapters/wclap.js, the worklet).
//   • PulpGain.wasm                     — the threaded WCLAP module.
// The sibling `_headers` (COOP/COEP/CORP + MIME) applies to nested paths too, so
// crossOriginIsolated holds under `/player/`. Runs standalone (copies `_headers`
// if the dir doesn't have it yet) or after assemble.mjs (the single-plugin proof).
//
// Usage:  node assemble-player.mjs [--wasm <path>] [--out <dir>]
// Defaults: --wasm ../build/PulpGain.wasm (or the packaged fallbacks)  --out ./public
import { readdir, mkdir, copyFile, cp } from "node:fs/promises";
import { existsSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join, resolve } from "node:path";

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = resolve(HERE, "../../../..");
const PKG_SRC = resolve(REPO, "packages/pulp-web-player/src");
const PAGE = resolve(HERE, "player/index.html");

function arg(flag, dflt) {
  const i = process.argv.indexOf(flag);
  return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : dflt;
}
const die = (m) => { console.error("assemble-player: FAIL: " + m); process.exit(1); };

const wasmCandidates = [
  arg("--wasm", null),
  resolve(HERE, "../build/PulpGain.wasm"),
  resolve(HERE, "../build/PulpGain.wclap/module.wasm"),
  resolve(HERE, "./public/PulpGain.wasm"),
].filter(Boolean);
const wasmSrc = wasmCandidates.find((p) => existsSync(p));
if (!wasmSrc) {
  die(`no WCLAP wasm found. Build it (needs wasi-sdk) or pass --wasm <path>:\n` +
      `  cd ${resolve(HERE, "..")}\n` +
      `  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=../../../tools/cmake/wasi-toolchain.cmake -DCMAKE_BUILD_TYPE=Release\n` +
      `  cmake --build build`);
}
if (!existsSync(PKG_SRC)) die(`package src not found at ${PKG_SRC}`);
if (!existsSync(PAGE)) die(`player page not found at ${PAGE}`);

const OUT = resolve(HERE, arg("--out", "./public"));
const PLAYER_OUT = join(OUT, "player");
await mkdir(PLAYER_OUT, { recursive: true });

// The package src tree (shell.js, index.js, adapters/, widgets/, theme/,
// vendor/pulp-wasm/{wclap-processor.js, wclap-abi.mjs, wam-scope.mjs, ...}).
await cp(PKG_SRC, join(PLAYER_OUT, "pulp-web-player"), { recursive: true });
await copyFile(PAGE, join(PLAYER_OUT, "index.html"));
await copyFile(wasmSrc, join(PLAYER_OUT, "PulpGain.wasm"));

// _headers lives at the deploy root and matches nested paths; copy it in if this
// script is run standalone (assemble.mjs already provides it otherwise).
if (!existsSync(join(OUT, "_headers"))) await copyFile(join(HERE, "_headers"), join(OUT, "_headers"));

// The shared-player demo is surfaced from the root landing page produced by
// assemble-landing.mjs (run last), so this script no longer injects a banner
// link into the root isolation-proof page.

async function tree(dir, base = dir) {
  let n = 0;
  for (const e of await readdir(dir, { withFileTypes: true })) {
    const p = join(dir, e.name);
    if (e.isDirectory()) n += await tree(p, base); else n++;
  }
  return n;
}
const rel = (p) => p.replace(REPO + "/", "");
const fileCount = await tree(join(PLAYER_OUT, "pulp-web-player"));
console.log("assemble-player: wrote shared-player WCLAP demo → " + rel(PLAYER_OUT));
console.log("  index.html                 (mounts @danielraffel/web-player shell + WCLAP adapter)");
console.log(`  pulp-web-player/**         (${fileCount} files: shell, widgets, theme, wclap adapter + worklet)`);
console.log("  PulpGain.wasm             (from " + rel(wasmSrc) + ")");
console.log("  _headers                   (COOP/COEP/CORP + MIME; matches /player/**)");
