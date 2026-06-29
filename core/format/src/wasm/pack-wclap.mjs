// Package a built WebCLAP module into a `.wclap` bundle (and optional .tar.gz).
//
// The WebCLAP bundle format (free-audio/web-clap) is a DIRECTORY named
// `<name>.wclap` containing a mandatory `module.wasm` plus an arbitrary set of
// resource files (factory presets, GUI assets, wavetables). There is no required
// manifest — plugin metadata comes from the module's `clap_entry` descriptor.
// For web distribution the bundle is served as `<name>.wclap.tar.gz`, and a host
// exposes the whole directory through the WASI filesystem.
//
// This tool produces exactly that layout:
//   <out>/<name>.wclap/module.wasm
//   <out>/<name>.wclap/<resources copied from --resources>
//   <out>/<name>.wclap.tar.gz            (with --tar)
//
// Usage:
//   node pack-wclap.mjs --wasm <module.wasm> --name <PluginName> --out <dir>
//        [--resources <dir>] [--tar]
//
// Exit 0 on success; prints the bundle path.
import { mkdir, copyFile, cp, rm, readdir, stat, lstat } from "node:fs/promises";
import { existsSync } from "node:fs";
import { join, basename } from "node:path";
import { spawnSync } from "node:child_process";

function arg(flag, dflt = null) {
  const i = process.argv.indexOf(flag);
  return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : dflt;
}
const has = (flag) => process.argv.includes(flag);

const wasm = arg("--wasm");
const name = arg("--name");
const out = arg("--out", ".");
const resources = arg("--resources");
const makeTar = has("--tar");

if (!wasm || !name) {
  console.error("usage: node pack-wclap.mjs --wasm <module.wasm> --name <PluginName> --out <dir> [--resources <dir>] [--tar]");
  process.exit(2);
}
if (!existsSync(wasm)) { console.error(`FAIL: wasm not found: ${wasm}`); process.exit(1); }

const bundleName = `${name}.wclap`;
const bundleDir = join(out, bundleName);

// Fresh bundle dir.
await rm(bundleDir, { recursive: true, force: true });
await mkdir(bundleDir, { recursive: true });

// The mandatory module.wasm (always named module.wasm inside the bundle).
await copyFile(wasm, join(bundleDir, "module.wasm"));

// Optional resources: copy the directory's contents into the bundle root.
if (resources) {
  if (!existsSync(resources)) { console.error(`FAIL: resources dir not found: ${resources}`); process.exit(1); }
  for (const entry of await readdir(resources)) {
    // WebCLAP bundles must avoid symlinks (the WASI FS may not support them).
    // lstat (not stat) is required to detect a symlink — stat resolves it first.
    const src = join(resources, entry);
    if ((await lstat(src)).isSymbolicLink()) {
      console.error(`FAIL: symlink in resources not allowed: ${entry}`);
      process.exit(1);
    }
    await cp(src, join(bundleDir, entry), { recursive: true });
  }
}

console.log(`bundle: ${bundleDir}/`);
console.log(`  module.wasm  (${(await stat(join(bundleDir, "module.wasm"))).size} bytes)`);

// Optional .tar.gz for web distribution: `<name>.wclap.tar.gz` whose top-level
// entry is the `<name>.wclap/` directory.
if (makeTar) {
  const tarPath = join(out, `${bundleName}.tar.gz`);
  const r = spawnSync("tar", ["-czf", tarPath, "-C", out, bundleName], { stdio: "inherit" });
  if (r.status !== 0) { console.error("FAIL: tar failed"); process.exit(1); }
  console.log(`archive: ${tarPath}`);
}

console.log(`PASS: packaged ${basename(wasm)} → ${bundleName}`);
