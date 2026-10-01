#!/usr/bin/env node
// test_bundle_threejs_offline.mjs: the Three.js bundler must not reach the
// network when PULP_OFFLINE_BUILD is set.
//
// The bundler runs inside a CMake POST_BUILD command (the iOS AUv3 lane) and
// inside the pulp_bundle_threejs_for_jsc_smoke ctest. Without esbuild beside
// it, it runs `npm install`, which makes a compile depend on the npm registry
// being reachable. With PULP_OFFLINE_BUILD set it must refuse instead, with a
// message naming the install command and exit code 3.
//
// The bundler is copied into a scratch directory with no node_modules, and a
// stub `npm` that records each launch is put first on PATH, so no case here
// touches the network or the real tools/scripts/node_modules.
//
// The first case is the control: without PULP_OFFLINE_BUILD the stub MUST be
// launched. If it is not, the stub never sat where the bundler looks for npm,
// and the "npm was not launched" assertion in the offline cases would pass
// without having observed anything.

import { spawnSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

const SELF = path.dirname(fileURLToPath(import.meta.url));
const BUNDLER = path.join(SELF, "bundle_threejs_for_jsc.mjs");
const OFFLINE_EXIT = 3;

function fail(message) {
    console.error("FAIL:", message);
    process.exit(1);
}

function assert(cond, message) {
    if (!cond) fail(message);
}

function runCase(offlineValue) {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "pulp-threejs-offline-test-"));
    try {
        const scriptsDir = path.join(dir, "scripts");
        const binDir = path.join(dir, "bin");
        fs.mkdirSync(scriptsDir);
        fs.mkdirSync(binDir);
        const bundler = path.join(scriptsDir, "bundle_threejs_for_jsc.mjs");
        fs.copyFileSync(BUNDLER, bundler);

        const marker = path.join(dir, "npm-launched");
        // The stub records its launch and fails, so the non-offline control
        // stops at "npm install exited 1" instead of downloading anything.
        fs.writeFileSync(
            path.join(binDir, "npm"),
            `#!/bin/sh\necho "$@" >> "${marker}"\nexit 1\n`,
            { mode: 0o755 },
        );
        fs.writeFileSync(
            path.join(binDir, "npm.cmd"),
            `@echo off\r\necho %* >> "${marker}"\r\nexit /b 1\r\n`,
        );

        const input = path.join(dir, "fixture.js");
        fs.writeFileSync(input, "export const x = 1;\n", "utf8");
        const output = path.join(dir, "out.js");

        const env = { ...process.env };
        delete env.PULP_OFFLINE_BUILD;
        if (offlineValue !== null) env.PULP_OFFLINE_BUILD = offlineValue;
        const pathKey = Object.keys(env).find((k) => k.toUpperCase() === "PATH") || "PATH";
        env[pathKey] = `${binDir}${path.delimiter}${env[pathKey] || ""}`;

        const result = spawnSync(process.execPath, [bundler, "--input", input, "--output", output], {
            env,
            encoding: "utf8",
            timeout: 60000,
        });
        return {
            status: result.status,
            stderr: result.stderr || "",
            npmLaunched: fs.existsSync(marker),
            nodeModulesCreated: fs.existsSync(path.join(scriptsDir, "node_modules")),
            outputWritten: fs.existsSync(output),
        };
    } finally {
        fs.rmSync(dir, { recursive: true, force: true });
    }
}

// Control: the ordinary developer path still auto-installs, so the stub runs.
{
    const r = runCase(null);
    assert(r.npmLaunched, "control: without PULP_OFFLINE_BUILD the stub npm was never launched, so this test cannot observe a launch");
    assert(r.status !== 0 && r.status !== OFFLINE_EXIT,
        `control: expected the failing stub install to fail the bundler with a non-offline status, got ${r.status}`);
    console.log("PASS: control - without PULP_OFFLINE_BUILD a missing esbuild launches npm");
}

// Explicitly disabled values behave like unset.
for (const value of ["0", "false", "off", ""]) {
    const r = runCase(value);
    assert(r.npmLaunched, `PULP_OFFLINE_BUILD=${JSON.stringify(value)} should not disable the auto-install`);
}
console.log("PASS: PULP_OFFLINE_BUILD=0/false/off/empty keeps the auto-install");

// Offline: refuse, name the fix, never launch npm, never write output.
for (const value of ["1", "true", "yes"]) {
    const r = runCase(value);
    assert(!r.npmLaunched, `PULP_OFFLINE_BUILD=${value}: npm was launched, so the build still reaches the network`);
    assert(!r.nodeModulesCreated, `PULP_OFFLINE_BUILD=${value}: node_modules appeared beside the bundler`);
    assert(r.status === OFFLINE_EXIT, `PULP_OFFLINE_BUILD=${value}: expected exit ${OFFLINE_EXIT}, got ${r.status}; stderr:\n${r.stderr}`);
    assert(r.stderr.includes("PULP_OFFLINE_BUILD"), `PULP_OFFLINE_BUILD=${value}: the error does not name PULP_OFFLINE_BUILD:\n${r.stderr}`);
    assert(r.stderr.includes("npm ci --prefix"), `PULP_OFFLINE_BUILD=${value}: the error does not name the install command:\n${r.stderr}`);
    assert(!r.outputWritten, `PULP_OFFLINE_BUILD=${value}: an output bundle was written`);
}
console.log("PASS: PULP_OFFLINE_BUILD=1/true/yes refuses without launching npm");

console.log("All bundle_threejs_for_jsc offline cases passed.");
