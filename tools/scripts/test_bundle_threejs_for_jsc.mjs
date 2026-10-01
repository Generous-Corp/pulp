#!/usr/bin/env node
// test_bundle_threejs_for_jsc.mjs — smoke test for the esbuild-backed
// bundler at `tools/scripts/bundle_threejs_for_jsc.mjs`.
//
// The previous regex-based bundler emitted predictable `Vector3: Vector3`
// key/value pairs in the IIFE body, so this test grepped the output text.
// The esbuild-backed bundler emits a Module-Record-style closure with
// `__export(exports, { Vector3: () => Vector3 })`, so text patterns don't
// hold across both implementations.
//
// What stays stable across implementations — and what this test pins —
// is the BEHAVIOR contract: evaluate the emitted IIFE inside a fresh
// VM context, then assert that `globalThis.THREE` exposes every export
// the source declared. That's the contract iOS-D.3b's runtime depends
// on; the rest is internal bundler shape.
//
// Cases:
//   1. Top-level `export { ... }` block → all symbols on globalThis.THREE.
//   2. Inline `export class / const / function` → all symbols on THREE.
//   3. `export { A as B }` alias → THREE.B references A's value.
//   4. ESM `import { ... } from "./sibling.js"` → bundler resolves the
//      sibling module (this is the regression class the regex bundler
//      could not handle and esbuild fixes).
//   5. PULP_THREEJS log marker is emitted when `print()` is defined.
//   6-8. The esbuild auto-install retries a transient network failure
//      (ENOTFOUND), never retries any other npm failure, and stops after
//      its attempt budget.

import { execFileSync, spawnSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";
import vm from "node:vm";

// Use fileURLToPath rather than `new URL(import.meta.url).pathname`. On
// Windows the latter yields a `/D:/...` path whose leading slash becomes a
// backslash after path.join, and resolving that against the cwd doubles the
// drive letter (`D:\D:\a\...`), so the child `node` invocation fails with
// "Cannot find module". fileURLToPath produces a native, properly-rooted path.
const SELF = path.dirname(fileURLToPath(import.meta.url));
const BUNDLER = path.join(SELF, "bundle_threejs_for_jsc.mjs");

function assert(cond, message) {
    if (!cond) {
        console.error("FAIL:", message);
        process.exit(1);
    }
}

function mkFixture(dir, source, name = "fixture.js") {
    const inputPath = path.join(dir, name);
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

// Evaluate the IIFE inside an isolated VM context and return its
// `globalThis.THREE` namespace. Also captures any `print(...)` calls so
// the PULP_THREEJS marker assertion can run.
function evalIife(iife) {
    const printedLines = [];
    const ctx = {
        globalThis: undefined,
        print(msg) { printedLines.push(String(msg)); },
    };
    ctx.globalThis = ctx;
    vm.createContext(ctx);
    vm.runInContext(iife, ctx, { timeout: 5000 });
    return { THREE: ctx.THREE, printedLines };
}

// Case 1: top-level `export { ... }` block, all exports on globalThis.THREE.
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
    assert(iife.includes("globalScope.THREE = Object.assign"), "Case 1: THREE namespace assignment missing");
    const { THREE } = evalIife(iife);
    assert(THREE, "Case 1: globalThis.THREE not defined after IIFE eval");
    assert(typeof THREE.Vector3 === "function", "Case 1: Vector3 not surfaced as a class on globalThis.THREE");
    assert(new THREE.Vector3().x === 0, "Case 1: Vector3 constructor lost");
    assert(typeof THREE.Mesh === "function", "Case 1: Mesh not surfaced");
    assert(typeof THREE.noop === "function" && THREE.noop() === 42, "Case 1: noop not surfaced/callable");
    console.log("PASS: Case 1 — `export { ... }` block surfaced via globalThis.THREE");
});

// Case 2: inline `export class / const / function` keywords.
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
    const { THREE } = evalIife(iife);
    assert(THREE, "Case 2: globalThis.THREE not defined");
    assert(typeof THREE.Vector3 === "function" && new THREE.Vector3().x === 0,
        "Case 2: Vector3 inline-export not surfaced");
    assert(THREE.PI_3 === 3.141, "Case 2: PI_3 inline-export not surfaced");
    assert(typeof THREE.compute === "function" && THREE.compute() === 1,
        "Case 2: compute inline-export not surfaced");
    console.log("PASS: Case 2 — inline export class/const/function surfaced");
});

// Case 3: `export { A as B }` alias.
withTmpDir((dir) => {
    const inputPath = mkFixture(
        dir,
        [
            "class InternalVec { constructor() { this.kind = 'internal'; } }",
            "export { InternalVec as Vector3 };",
        ].join("\n"),
    );
    const outputPath = path.join(dir, "out.js");
    const iife = bundle(inputPath, outputPath);
    const { THREE } = evalIife(iife);
    assert(typeof THREE.Vector3 === "function",
        "Case 3: aliased export not surfaced as Vector3");
    assert(new THREE.Vector3().kind === "internal",
        "Case 3: aliased Vector3 does not point to InternalVec");
    assert(typeof THREE.InternalVec === "undefined",
        "Case 3: original name leaked onto THREE (expected only the alias)");
    console.log("PASS: Case 3 — `export { X as Y }` alias surfaced under alias only");
});

// Case 4: sibling `import` resolution. This is the regression class the
// regex bundler could not handle — `import { ... } from "./core.js"` was
// left as a bare top-level import which JSC then failed to parse.
withTmpDir((dir) => {
    mkFixture(
        dir,
        "export class Vec3 { constructor() { this.tag = 'core'; } }",
        "core.js",
    );
    const inputPath = mkFixture(
        dir,
        [
            "import { Vec3 } from './core.js';",
            "class Mesh { make() { return new Vec3(); } }",
            "export { Vec3, Mesh };",
        ].join("\n"),
        "fixture.js",
    );
    const outputPath = path.join(dir, "out.js");
    const iife = bundle(inputPath, outputPath);
    assert(!/^\s*import\s+/m.test(iife),
        "Case 4: bare `import` statement survived bundling — esbuild did not inline the sibling module");
    const { THREE } = evalIife(iife);
    assert(typeof THREE.Vec3 === "function", "Case 4: Vec3 (from sibling) not on THREE");
    assert(typeof THREE.Mesh === "function", "Case 4: Mesh not on THREE");
    assert(new THREE.Mesh().make().tag === "core",
        "Case 4: Mesh.make() does not reach Vec3 from the sibling module");
    console.log("PASS: Case 4 — sibling `import` resolution works (esbuild path)");
});

// Case 5: PULP_THREEJS log marker fires when print() is defined.
withTmpDir((dir) => {
    const inputPath = mkFixture(
        dir,
        ["export class Solo { }"].join("\n"),
    );
    const outputPath = path.join(dir, "out.js");
    const iife = bundle(inputPath, outputPath);
    const { printedLines } = evalIife(iife);
    assert(printedLines.some((line) => line.startsWith("PULP_THREEJS:")),
        "Case 5: bundler did not emit PULP_THREEJS marker via print()");
    console.log("PASS: Case 5 — PULP_THREEJS log marker emitted");
});

// Cases 6-8: the esbuild auto-install retries a transient network error
// and only that. A copy of the bundler in a scratch directory has no
// node_modules, so it must install; PULP_BUNDLE_THREEJS_NPM_JS swaps npm
// for a fake that fails a scripted number of times, then provisions a stub
// esbuild. No network and no real npm are involved.
const FAKE_NPM = `
const fs = require("node:fs");
const path = require("node:path");
const counter = path.join(process.cwd(), "npm-calls.txt");
const calls = (fs.existsSync(counter) ? Number(fs.readFileSync(counter, "utf8")) : 0) + 1;
fs.writeFileSync(counter, String(calls));
const failures = Number(process.env.FAKE_NPM_FAILURES || "0");
if (calls <= failures) {
    if (process.env.FAKE_NPM_ERROR === "network") {
        console.error("npm error code ENOTFOUND");
        console.error("npm error network request to https://registry.npmjs.org/esbuild/-/esbuild-0.25.10.tgz failed, reason: getaddrinfo ENOTFOUND registry.npmjs.org");
    } else {
        console.error("npm error code E404");
        console.error("npm error 404 Not Found - GET https://registry.npmjs.org/esbuild-typo");
    }
    process.exit(1);
}
const pkg = path.join(process.cwd(), "node_modules", "esbuild");
fs.mkdirSync(path.join(pkg, "lib"), { recursive: true });
fs.writeFileSync(path.join(pkg, "package.json"), JSON.stringify({ name: "esbuild", main: "lib/main.js" }));
fs.writeFileSync(path.join(pkg, "lib", "main.js"),
    "exports.build = async () => ({ outputFiles: [{ text: 'var __pulp_three_iife_namespace__ = { Stub: 1 };' }] });");
`;

function runInstallScenario(dir, env) {
    fs.copyFileSync(BUNDLER, path.join(dir, "bundle_threejs_for_jsc.mjs"));
    const fakeNpm = path.join(dir, "fake-npm.cjs");
    fs.writeFileSync(fakeNpm, FAKE_NPM, "utf8");
    const inputPath = mkFixture(dir, "export class Solo { }");
    const outputPath = path.join(dir, "out.js");
    // These scenarios exercise the online install path, so an inherited
    // PULP_OFFLINE_BUILD (CI sets it job-wide) must not turn them into the
    // offline refusal that test_bundle_threejs_offline.mjs covers.
    const baseEnv = { ...process.env };
    delete baseEnv.PULP_OFFLINE_BUILD;
    const result = spawnSync(process.execPath,
        [path.join(dir, "bundle_threejs_for_jsc.mjs"), "--input", inputPath, "--output", outputPath], {
            encoding: "utf8",
            env: {
                ...baseEnv,
                PULP_BUNDLE_THREEJS_NPM_JS: fakeNpm,
                PULP_BUNDLE_THREEJS_NPM_ATTEMPTS: "3",
                PULP_BUNDLE_THREEJS_NPM_BACKOFF_MS: "0",
                ...env,
            },
        });
    const counter = path.join(dir, "npm-calls.txt");
    return {
        status: result.status,
        stderr: result.stderr,
        calls: fs.existsSync(counter) ? Number(fs.readFileSync(counter, "utf8")) : 0,
        wroteOutput: fs.existsSync(outputPath),
    };
}

withTmpDir((dir) => {
    const r = runInstallScenario(dir, { FAKE_NPM_FAILURES: "2", FAKE_NPM_ERROR: "network" });
    assert(r.status === 0, `Case 6: bundler failed after transient ENOTFOUND (exit ${r.status}):\n${r.stderr}`);
    assert(r.calls === 3, `Case 6: expected 3 npm attempts (2 transient failures + success), saw ${r.calls}`);
    assert(r.wroteOutput, "Case 6: no bundle written after the retried install succeeded");
    console.log("PASS: Case 6 — transient ENOTFOUND during npm install is retried");
});

withTmpDir((dir) => {
    const r = runInstallScenario(dir, { FAKE_NPM_FAILURES: "1", FAKE_NPM_ERROR: "not-found" });
    assert(r.status !== 0, "Case 7: a non-network npm failure should fail the bundler");
    assert(r.calls === 1, `Case 7: a non-network npm failure must not be retried, saw ${r.calls} attempts`);
    assert(r.stderr.includes("not retried"), `Case 7: failure message does not say it was not retried:\n${r.stderr}`);
    console.log("PASS: Case 7 — non-network npm failure fails once, without retry");
});

withTmpDir((dir) => {
    const r = runInstallScenario(dir, { FAKE_NPM_FAILURES: "99", FAKE_NPM_ERROR: "network" });
    assert(r.status !== 0, "Case 8: a persistent network failure should fail the bundler");
    assert(r.calls === 3, `Case 8: expected exactly PULP_BUNDLE_THREEJS_NPM_ATTEMPTS=3 attempts, saw ${r.calls}`);
    assert(r.stderr.includes("persisted through 3 attempts"),
        `Case 8: failure message does not report the exhausted retries:\n${r.stderr}`);
    console.log("PASS: Case 8 — persistent network failure stops after the attempt budget");
});

console.log("\nAll bundle_threejs_for_jsc tests passed.");
