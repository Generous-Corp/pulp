#!/usr/bin/env node
// Test runner for the Figma plugin.
//
// The plugin sources are TypeScript that also import JSON
// (library-manifest.json) and reference Figma sandbox globals. Rather than
// pull in a TS test loader, we reuse esbuild (already a devDep, same as
// scripts/build.mjs) to bundle each test/*.test.ts file — together with the
// real plugin sources it imports — into a temporary .mjs, then hand the
// bundle to Node's built-in test runner (`node --test`). This exercises the
// REAL serialize / library-registry code, not a reimplementation.

import { build } from "esbuild";
import { spawnSync } from "node:child_process";
import { promises as fs } from "node:fs";
import path from "node:path";
import os from "node:os";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, "..");
const testDir = path.join(root, "test");

const entries = (await fs.readdir(testDir))
  .filter((f) => f.endsWith(".test.ts"))
  .map((f) => path.join(testDir, f));

if (entries.length === 0) {
  console.error("[pulp figma plugin] no test/*.test.ts files found");
  process.exit(1);
}

const outDir = await fs.mkdtemp(path.join(os.tmpdir(), "pulp-figma-test-"));

try {
  await build({
    bundle: true,
    platform: "node",
    format: "esm",
    target: ["node18"],
    entryPoints: entries,
    outdir: outDir,
    outExtension: { ".js": ".mjs" },
    logLevel: "warning",
    // node:test / node:assert stay external so the real built-in runner is used.
    external: ["node:test", "node:assert"],
  });

  const bundled = (await fs.readdir(outDir))
    .filter((f) => f.endsWith(".mjs"))
    .map((f) => path.join(outDir, f));

  const res = spawnSync(
    process.execPath,
    ["--test", ...bundled],
    { stdio: "inherit" },
  );
  process.exit(res.status ?? 1);
} finally {
  await fs.rm(outDir, { recursive: true, force: true });
}
