#!/usr/bin/env node
// run-headless.mjs — emit the JS payload an agent should pass to the Figma
// MCP `use_figma` tool to drive a headless export of one node.
//
// The published plugin runs in Figma's UI sandbox via `figma.showUI(__html__)`
// + a message loop; that requires a human click on "Export to Pulp." The
// `dist/headless.js` bundle is the SAME extractScene + serializeExport core
// without the UI, packaged so an agent's `use_figma` call can run it and get
// the envelope + assets back as a plain object.
//
// This script does NOT call MCP itself (MCP is the agent's connector). It
// prints a self-contained `code` string the agent passes verbatim to
// `mcp__figma__use_figma`. Output goes to stdout so it can be piped or
// captured.
//
// The payload embeds dist/headless.packed.js — a self-extracting
// deflate+base64 stub (see scripts/build.mjs packHeadless()) that sets
// `globalThis.__pulp_packed_src` to the decompressed raw bundle. This script
// assigns driver inputs as data-only globals, evaluates only that trusted
// built source, and clears every staging global even if evaluation fails.
//
// Why a script rather than an inline template the agent constructs each
// time? Two reasons: (1) the packed bundle is tens of KB, copy-pasting it
// every call is painful; (2) we want a single regen point so the bundle
// version and the prelude stay coupled.
//
// Usage:
//   node scripts/run-headless.mjs <NODE_ID>
//   node scripts/run-headless.mjs --selection
//
// Example:
//   node scripts/run-headless.mjs 26:3 > /tmp/payload.js
//   # then in the agent:
//   # mcp__figma__use_figma({
//   #   fileKey: "...",
//   #   description: "headless export",
//   #   code: <contents of /tmp/payload.js>,
//   # })

import { promises as fs } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { buildHeadlessPayload } from "./headless-payload.mjs";

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, "..");

function usage(code = 2) {
  process.stderr.write(
    "usage: node scripts/run-headless.mjs <NODE_ID|--selection> [--no-faithful-vector]\n" +
    "\n" +
    "  NODE_ID               — Figma node id (e.g. '26:3'). Forwarded as TARGET_NODE_ID\n" +
    "                          to the headless bundle.\n" +
    "  --selection           — fall back to figma.currentPage.selection inside the\n" +
    "                          sandbox. Useful when running interactively in Figma\n" +
    "                          with a frame already selected.\n" +
    "  --faithful-vector     — faithful-vector lane (Plan B). DEFAULT ON: export each\n" +
    "                          frame's own SVG + auto-detect interactive overlays.\n" +
    "  --no-faithful-vector  — legacy flat, static node-tree export (opt out).\n",
  );
  process.exit(code);
}

const argv = process.argv.slice(2);
// Faithful-vector is the default; --no-faithful-vector opts out. --faithful-vector
// is still accepted (a no-op now) for backward compatibility with old invocations.
const faithfulVector = !argv.includes("--no-faithful-vector");
const arg = argv.find((a) => a !== "--faithful-vector" && a !== "--no-faithful-vector");
if (!arg) usage();

const bundlePath = path.join(root, "dist", "headless.packed.js");
let bundle;
try {
  bundle = await fs.readFile(bundlePath, "utf8");
} catch (err) {
  process.stderr.write(
    `packed headless bundle not found at ${bundlePath}.\n` +
    "Run 'npm run build' first.\n",
  );
  process.exit(1);
}

// Authored data is assigned as data before the trusted built bundle is
// evaluated. It is never concatenated into executable source, and the helper
// keeps the cleanup contract independently testable.
const payload = buildHeadlessPayload(
  bundle, arg === "--selection" ? undefined : arg, faithfulVector);

// Size guard: the Figma MCP `use_figma` `code` parameter is capped at
// 50000 characters. We fail loudly here so the agent sees the error
// before the MCP server returns InputValidationError.
const LIMIT = 50000;
if (payload.length > LIMIT) {
  process.stderr.write(
    `payload is ${payload.length} chars; exceeds the use_figma 50000-char cap.\n` +
    "Trim the bundle (see scripts/build.mjs buildHeadless()) or split the work.\n",
  );
  process.exit(1);
}

process.stdout.write(payload);
process.stderr.write(
  `[run-headless] payload ${payload.length} chars (${LIMIT - payload.length} headroom)\n`,
);
