import assert from "node:assert/strict";
import { test } from "node:test";
import { resolve } from "node:path";

import { resolveFixtureAsset } from "./fixture-assets.mjs";

const sourceDir = resolve(new URL("..", import.meta.url).pathname);
const buildDir = resolve(sourceDir, "build-webui");
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
    resolve(buildDir, "PulpSuperConvolverUi.js"),
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
