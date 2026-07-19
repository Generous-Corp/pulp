#!/usr/bin/env node
// Builds the Figma plugin:
//   src/code.ts     → dist/code.js             (UI plugin sandbox main)
//   src/ui.ts       → dist/ui.html             (iframe UI, JS inlined)
//   src/headless.ts → dist/headless.js         (MCP / agent-driven headless
//                                               extractor — raw minified bundle,
//                                               kept for debugging/diffing)
//                   → dist/headless.packed.js  (self-extracting deflate+base64
//                                               wrapper around headless.js; this
//                                               is what run-headless.mjs ships
//                                               through the Figma MCP `use_figma`
//                                               50000-char `code` cap)
// esbuild handles all of them.

import { build, context } from "esbuild";
import { promises as fs } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import zlib from "node:zlib";

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
// no message handlers. Minified + target=es2020, iife format so evaluating the
// bundle surfaces its result on `globalThis.__pulp_headless_result` for the
// driver's trailing `return await`.
//
// Delivery constraint: the payload ships verbatim as the `code` parameter of
// the Figma MCP `use_figma` tool, whose schema hard-caps `code` at 50000
// characters ("maxLength": 50000 — verified against the live tool schema,
// 2026-07). The raw minified bundle outgrew that cap (~63 KB after the
// extraction-coverage slices), so the shipped artifact is
// dist/headless.packed.js: a self-extracting stub (fflate inflate — already a
// runtime dependency — plus a small base64 decoder) that decompresses the
// deflated+base64'd raw bundle into `globalThis.__pulp_packed_src`. The driver
// (scripts/run-headless.mjs) then `eval`s prelude + decompressed source as ONE
// program, so `const TARGET_NODE_ID` / `FAITHFUL_VECTOR` are in the same
// program scope as the bundle — identical program text to the old raw payload.
// `eval` is available in Figma's plugin sandbox, but code eval'd there does
// NOT see the caller's lexical scope (both verified empirically via live
// `use_figma` probes, 2026-07 — hence prelude-inside-eval, not scope-chain).
async function buildHeadless() {
  const rawPath = path.join(root, "dist", "headless.js");
  const packedPath = path.join(root, "dist", "headless.packed.js");
  const opts = {
    ...commonOpts,
    entryPoints: [path.join(root, "src", "headless.ts")],
    outfile: rawPath,
    target: ["es2020"],          // smaller output than es2017
    format: "iife",              // wrap so evaluating the bundle runs it
    minify: true,                // minify before compressing — both matter
    legalComments: "none",
    external: [],
  };
  if (watch) {
    // Watch mode rebuilds only the raw bundle (same as the pre-pack behavior,
    // which also skipped the size gate). Run a full `npm run build` to refresh
    // dist/headless.packed.js before driving run-headless.mjs.
    (await context(opts)).watch();
    return;
  }
  await build(opts);
  const raw = await fs.readFile(rawPath, "utf8");
  const packed = await packHeadless(raw);
  await fs.writeFile(packedPath, packed, "utf8");

  // Size guard on the PACKED artifact — the thing that must actually fit the
  // `use_figma` cap. Reserve ~1 KB for what run-headless.mjs appends: the
  // eval(prelude + source) line (prelude carries TARGET_NODE_ID /
  // FAITHFUL_VECTOR) and the trailing `return await`.
  const LIMIT = 50000;
  const RESERVE = 1024;
  if (packed.length > LIMIT - RESERVE) {
    throw new Error(
      `[pulp figma plugin] packed headless bundle is ${packed.length} chars; ` +
      `must stay <= ${LIMIT - RESERVE} so the agent-injected prelude fits ` +
      `the Figma MCP \`use_figma\` 50000-char \`code\` cap.`,
    );
  }
  console.log(
    `[pulp figma plugin] headless bundle ${raw.length} bytes raw, ` +
    `${packed.length} chars packed ` +
    `(${LIMIT - packed.length} chars headroom under the ${LIMIT}-char cap)`,
  );
}

// Wrap the raw headless bundle in a self-extracting stub IIFE that sets
// `globalThis.__pulp_packed_src` to the decompressed source. The stub bundles
// fflate's inflateSync + strFromU8 and a bit-accumulator base64 decoder (the
// sandbox has no atob). Executing the source is the DRIVER's job
// (run-headless.mjs) — it evals prelude + __pulp_packed_src as one program.
// Exported so tooling can pack arbitrary programs through the exact shipped
// pipeline (e.g. sandbox smoke payloads).
export async function packHeadless(raw) {
  const compressed = zlib.deflateRawSync(Buffer.from(raw, "utf8"), { level: 9 });
  const b64 = compressed.toString("base64");
  // "__PULP_PACKED_B64__" survives minification as a string literal, and the
  // base64 alphabet contains no underscore, so the substitution is unambiguous.
  const stubSource = `
import { inflateSync, strFromU8 } from "fflate";
const b64 = "__PULP_PACKED_B64__".replace(/=+$/, "");
const alphabet =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
const lut = new Uint8Array(123);
for (let i = 0; i < 64; i++) lut[alphabet.charCodeAt(i)] = i;
const bytes = new Uint8Array((b64.length * 3) >> 2);
let acc = 0, bits = 0, o = 0;
for (let i = 0; i < b64.length; i++) {
  acc = (acc << 6) | lut[b64.charCodeAt(i)];
  bits += 6;
  if (bits >= 8) { bits -= 8; bytes[o++] = (acc >> bits) & 255; }
}
globalThis.__pulp_packed_src = strFromU8(inflateSync(bytes.subarray(0, o)));
`;
  const stubBuild = await build({
    ...commonOpts,
    stdin: { contents: stubSource, resolveDir: root, loader: "js" },
    target: ["es2020"],
    format: "iife",
    minify: true,
    legalComments: "none",
    write: false,
    logLevel: "silent",
  });
  const stub = stubBuild.outputFiles[0].text.replace("__PULP_PACKED_B64__", b64);

  // Round-trip the exact shipped stub in-process: run it, then assert the
  // decompressed source is byte-identical to the raw bundle. Catches base64 /
  // inflate / placeholder bugs at build time instead of inside Figma.
  new Function(stub)();
  const roundTripped = globalThis.__pulp_packed_src;
  delete globalThis.__pulp_packed_src;
  if (roundTripped !== raw) {
    throw new Error(
      "[pulp figma plugin] packed headless bundle failed the decompress " +
      "round-trip; refusing to emit a corrupt dist/headless.packed.js",
    );
  }

  return stub;
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
