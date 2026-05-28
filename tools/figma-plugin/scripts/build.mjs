#!/usr/bin/env node
// Builds the Figma plugin: src/code.ts → dist/code.js (sandbox main),
// src/ui.ts → inline JS in dist/ui.html (iframe UI). esbuild handles both.

import { build, context } from "esbuild";
import { promises as fs } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, "..");
const watch = process.argv.includes("--watch");

const commonOpts = {
  bundle: true,
  platform: "browser",
  format: "iife",
  target: ["es2017"],
  logLevel: "info",
};

async function buildCode() {
  const opts = {
    ...commonOpts,
    entryPoints: [path.join(root, "src", "code.ts")],
    outfile: path.join(root, "dist", "code.js"),
    // Figma sandbox has no fetch / XHR / location etc.; mark them external so
    // esbuild doesn't try to polyfill.
    external: [],
  };
  return watch ? (await context(opts)).watch() : build(opts);
}

async function buildUI() {
  // Compile UI script first
  const uiJsOpts = {
    ...commonOpts,
    entryPoints: [path.join(root, "src", "ui.ts")],
    outfile: path.join(root, "dist", "ui.js"),
  };
  if (watch) {
    (await context(uiJsOpts)).watch();
  } else {
    await build(uiJsOpts);
  }

  // Inline the compiled UI script into ui.html
  const htmlTmpl = await fs.readFile(path.join(root, "src", "ui.html"), "utf8");
  const uiJs = await fs.readFile(path.join(root, "dist", "ui.js"), "utf8");
  const inlined = htmlTmpl.replace("/*__UI_SCRIPT__*/", uiJs);
  await fs.mkdir(path.join(root, "dist"), { recursive: true });
  await fs.writeFile(path.join(root, "dist", "ui.html"), inlined, "utf8");
}

await fs.mkdir(path.join(root, "dist"), { recursive: true });
await Promise.all([buildCode(), buildUI()]);
console.log("[pulp figma plugin]", watch ? "watching for changes…" : "built dist/");
