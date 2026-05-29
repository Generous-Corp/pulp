#!/usr/bin/env node
// test_bundle_threejs_for_jsc.mjs — iOS-D.3b Slice 3.
//
// Smoke test for `bundle_threejs_for_jsc.mjs`. Bundles a fixture ESM
// source that mimics the upstream `three.webgpu.js` shape, then
// asserts the emitted IIFE:
//
//   1. Wraps in `(function () { ... })();`
//   2. Strips top-level `export { ... }` blocks
//   3. Stripped inline `export class / const / function` keywords
//   4. Registers all exported identifiers on `globalThis.THREE`
//   5. Emits the `PULP_THREEJS:` log marker
//
// This protects the bundler from silent regressions: if upstream
// Three.js's ESM shape changes (or if our regex naively breaks), this
// test fires before the iOS smoke catches it post-bundle.

import { execFileSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import process from "node:process";

const SELF = path.dirname(new URL(import.meta.url).pathname);
const BUNDLER = path.join(SELF, "bundle_threejs_for_jsc.mjs");

function assert(cond, message) {
    if (!cond) {
        console.error("FAIL:", message);
        process.exit(1);
    }
}

function mkFixture(dir, source) {
    const inputPath = path.join(dir, "fixture.js");
    fs.writeFileSync(inputPath, source, "utf8");
    return inputPath;
}

function bundle(inputPath, outputPath) {
    execFileSync(process.execPath, [BUNDLER, "--input", inputPath, "--output", outputPath], {
        stdio: "inherit",
    });
    return fs.readFileSync(outputPath, "utf8");
}

function withTmpDir(fn) {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "pulp-threejs-bundle-test-"));
    try {
        return fn(dir);
    } finally {
        fs.rmSync(dir, { recursive: true, force: true });
    }
}

// Case 1: top-level `export { ... }` block stripped, exports surfaced.
withTmpDir((dir) => {
    const inputPath = mkFixture(
        dir,
        [
            "class Vector3 { constructor() { this.x = 0; } }",
            "class Mesh { }",
            "function noop() { return 42; }",
            "export { Vector3, Mesh, noop };",
        ].join("\n"),
    );
    const outputPath = path.join(dir, "out.js");
    const iife = bundle(inputPath, outputPath);
    assert(iife.includes("(function ()"), "Case 1: IIFE wrapper missing");
    assert(!/^export\s*\{/m.test(iife), "Case 1: export block not stripped");
    assert(iife.includes("Vector3: Vector3"), "Case 1: Vector3 not surfaced on globalThis.THREE");
    assert(iife.includes("Mesh: Mesh"), "Case 1: Mesh not surfaced");
    assert(iife.includes("noop: noop"), "Case 1: noop not surfaced");
    assert(iife.includes("globalScope.THREE = Object.assign"), "Case 1: THREE namespace assignment missing");
    assert(iife.includes("PULP_THREEJS:"), "Case 1: log marker missing");
    console.log("PASS: Case 1 — export { ... } block stripped, namespace registered");
});

// Case 2: inline `export class X` stripped (keyword removed but class kept).
withTmpDir((dir) => {
    const inputPath = mkFixture(
        dir,
        [
            "export class Vector3 { constructor() { this.x = 0; } }",
            "export const PI_3 = 3.141;",
            "export function compute() { return 1; }",
        ].join("\n"),
    );
    const outputPath = path.join(dir, "out.js");
    const iife = bundle(inputPath, outputPath);
    assert(!/^export\s+(class|const|function)/m.test(iife), "Case 2: inline export keyword not stripped");
    assert(iife.includes("class Vector3"), "Case 2: class body lost");
    assert(iife.includes("const PI_3"), "Case 2: const body lost");
    assert(iife.includes("function compute"), "Case 2: function body lost");
    assert(iife.includes("Vector3: Vector3"), "Case 2: Vector3 not in namespace");
    assert(iife.includes("PI_3: PI_3"), "Case 2: PI_3 not in namespace");
    assert(iife.includes("compute: compute"), "Case 2: compute not in namespace");
    console.log("PASS: Case 2 — inline export class/const/function stripped");
});

// Case 3: `export { A as B }` alias surfaced under the alias.
withTmpDir((dir) => {
    const inputPath = mkFixture(
        dir,
        [
            "class InternalVec { }",
            "export { InternalVec as Vector3 };",
        ].join("\n"),
    );
    const outputPath = path.join(dir, "out.js");
    const iife = bundle(inputPath, outputPath);
    assert(iife.includes("Vector3: Vector3"), "Case 3: alias not surfaced as Vector3");
    assert(iife.includes("var Vector3 = InternalVec") === false, "Case 3: spurious alias var (expected naive transform — accept whichever)");
    console.log("PASS: Case 3 — `export { X as Y }` alias surfaced");
});

// Case 4: no exports → bundler must FAIL LOUDLY (so an upstream
// Three.js shape change can't silently produce an empty IIFE).
withTmpDir((dir) => {
    const inputPath = mkFixture(dir, "var nothing = 1;");
    const outputPath = path.join(dir, "out.js");
    let threw = false;
    try {
        execFileSync(process.execPath, [BUNDLER, "--input", inputPath, "--output", outputPath], {
            stdio: "pipe",
        });
    } catch (err) {
        threw = true;
    }
    assert(threw, "Case 4: bundler did not fail on empty-export source");
    console.log("PASS: Case 4 — bundler fails loudly on empty-export input");
});

console.log("\nAll bundle_threejs_for_jsc tests passed.");
