import assert from "node:assert/strict";
import { test } from "node:test";
import { realpathSync, writeFileSync } from "node:fs";
import { mkdtemp, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { after } from "node:test";

import { resolveFixtureAsset } from "./fixture-assets.mjs";

// This is the existing Node-test entrypoint in web-plugins.yml. Import the
// shared local-server and permalink-overlay regressions here so they remain
// blocking without widening workflow ownership for this source-only fix.
await import("../../tools/local-http-security.test.mjs");
await import("../../live-kernel-spike/highlight-tokens.test.mjs");
await import("../../compiler-spike/measure/lib/serve.test.mjs");

const sourceDir = resolve(new URL("..", import.meta.url).pathname);
const buildDir = await mkdtemp(join(tmpdir(), "pulp-ui-fixture-assets-"));
writeFileSync(join(buildDir, "PulpSuperConvolverUi.js"), "fixture");
after(async () => rm(buildDir, { recursive: true, force: true }));
const fixtureRoots = { sourceDir, buildDir };

test("source-owned UI modules resolve beside pulp-ui.js", () => {
  assert.equal(
    resolveFixtureAsset("/pulp-ui.js", fixtureRoots),
    resolve(sourceDir, "pulp-ui.js"),
  );
  assert.equal(
    resolveFixtureAsset("/ir-source.js?cache=1", fixtureRoots),
    resolve(sourceDir, "ir-source.js"),
  );
});

test("generated module assets resolve from the build directory", () => {
  assert.equal(
    resolveFixtureAsset("/PulpSuperConvolverUi.js", fixtureRoots),
    realpathSync(resolve(buildDir, "PulpSuperConvolverUi.js")),
  );
});

test("fixture asset resolution rejects traversal and malformed paths", () => {
  for (const path of [
    "/../pulp-ui.js",
    "/%2e%2e%2fir-source.js",
    "/..%5cir-source.js",
    "/..",
    "/%E0%A4%A",
  ]) {
    assert.equal(resolveFixtureAsset(path, fixtureRoots), null, path);
  }
});
