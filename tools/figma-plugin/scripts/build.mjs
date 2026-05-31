#!/usr/bin/env node
// Builds the Figma plugin:
//   src/code.ts     → dist/code.js       (UI plugin sandbox main)
//   src/ui.ts       → dist/ui.html       (iframe UI, JS inlined)
//   src/headless.ts → dist/headless.js   (MCP / agent-driven headless extractor,
//                                         no UI, bundle stays under 50KB so it
//                                         fits the Figma MCP `use_figma`
//                                         `code` parameter cap)
// esbuild handles all three.

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

// Headless extractor — same extract+serialize core as code.ts but no UI loop,
// no message handlers. Minified + target=es2020 (top-level await + smaller
// output) to keep the bundle under the Figma MCP `use_figma` `code` param
// 50000-char cap. The bundle's last expression resolves to the envelope
// result, which the agent's `use_figma` call returns directly.
async function buildHeadless() {
  const opts = {
    ...commonOpts,
    entryPoints: [path.join(root, "src", "headless.ts")],
    outfile: path.join(root, "dist", "headless.js"),
    target: ["es2020"],          // top-level await + smaller output
    format: "iife",              // wrap so the bundle's last expression is the result
    minify: true,                // critical for the 50KB cap
    legalComments: "none",
    external: [],
  };
  if (watch) {
    (await context(opts)).watch();
    return;
  }
  await build(opts);
  // Post-build: assert size guard so we fail loudly when the bundle creeps
  // past the MCP cap. Reserve ~1 KB for the driver-injected prelude
  // (TARGET_NODE_ID = "...").
  const bytes = await fs.readFile(path.join(root, "dist", "headless.js"));
  const LIMIT = 50000;
  const RESERVE = 1024;
  if (bytes.length > LIMIT - RESERVE) {
    throw new Error(
      `[pulp figma plugin] headless bundle is ${bytes.length} bytes; ` +
      `must stay <= ${LIMIT - RESERVE} so the agent-injected prelude fits ` +
      `the Figma MCP \`use_figma\` 50000-char \`code\` cap.`,
    );
  }
  console.log(
    `[pulp figma plugin] headless bundle ${bytes.length} bytes ` +
    `(${LIMIT - bytes.length} bytes headroom under ${LIMIT}-cap)`,
  );
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
await Promise.all([buildCode(), buildUI(), buildHeadless()]);
console.log("[pulp figma plugin]", watch ? "watching for changes…" : "built dist/");
