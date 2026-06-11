#!/usr/bin/env node
// bundle_threejs_for_jsc.mjs — Three.js → IIFE bundler for the iOS
// AUv3 lane (iOS-D.3b → iOS-D.3c).
//
// Reads `three.webgpu.js` (the ESM entry point shipped by the upstream
// Three.js repo pinned in tools/deps/manifest.json) and emits
// `three.iife.js`, a self-contained IIFE that registers `THREE` as a
// `globalThis.THREE` namespace.
//
// Why: iOS public JSC API has NO public ESM module loader
// (`JSScript.h` is private; `setModuleLoaderDelegate:` ships only
// inside the framework, not in iPhoneOS.sdk public headers). Shipping
// the ESM resolver path in a `.appex` risks App Store rejection.
// Build-time IIFE bundling + plain JSC `evaluate()` is the supported
// path. This script is the build-time transformation.
//
// Implementation: delegates to esbuild (pinned in tools/scripts/
// package.json) for proper ESM-import resolution, http: import
// stripping, and IIFE wrapping. The earlier regex-only pass (slice 2)
// could not resolve sibling `import { ... } from "./three.core.js"`
// statements that Three.js's webgpu entry depends on, which caused
// JSC parse errors at runtime ("expecting '('") on every iPad
// install. esbuild handles all of those edge cases correctly out of
// the box.
//
// Usage:
//   node bundle_threejs_for_jsc.mjs --input <three.webgpu.js> \
//                                   --output <three.iife.js>
//
// On first invocation in a fresh checkout, the script auto-installs
// its esbuild dependency via `npm install --prefix <script_dir>`
// using the pinned version in package.json. Subsequent invocations
// skip the install when node_modules/esbuild is already present.

import { spawnSync } from "node:child_process";
import { createRequire } from "node:module";
import fs from "node:fs";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

const SCRIPT_DIR = path.dirname(fileURLToPath(import.meta.url));
const REQUIRE = createRequire(import.meta.url);

function parseArgs(argv) {
    const args = { input: null, output: null, orbitControls: null };
    for (let i = 0; i < argv.length; ++i) {
        if (argv[i] === "--input") args.input = argv[++i];
        else if (argv[i] === "--output") args.output = argv[++i];
        // Optional: also bundle the OrbitControls addon and expose it as
        // THREE.OrbitControls (the iOS Three.js demo uses it for touch orbit).
        else if (argv[i] === "--orbit-controls") args.orbitControls = argv[++i];
    }
    if (!args.input || !args.output) {
        console.error("usage: bundle_threejs_for_jsc.mjs --input <three.webgpu.js> --output <three.iife.js> [--orbit-controls <OrbitControls.js>]");
        process.exit(2);
    }
    return args;
}

// Load esbuild from this script's local node_modules. If it's missing,
// run `npm install` once to populate it, then retry. Keeps the build
// reproducible against the pinned version in package.json without
// relying on a globally installed copy.
async function loadEsbuild() {
    const localEsbuildEntry = path.join(SCRIPT_DIR, "node_modules", "esbuild", "lib", "main.js");
    if (!fs.existsSync(localEsbuildEntry)) {
        process.stderr.write("[bundle_threejs] esbuild not present in tools/scripts/node_modules — running `npm install` (one-time)...\n");
        const npmBin = process.platform === "win32" ? "npm.cmd" : "npm";
        const result = spawnSync(npmBin, ["install", "--no-audit", "--no-fund"], {
            cwd: SCRIPT_DIR,
            stdio: "inherit",
            // npm.cmd is a shell script on Windows and cannot be spawned
            // directly by Node's exec/spawn path on every hosted runner.
            shell: process.platform === "win32",
        });
        if (result.error) {
            throw new Error(`failed to launch ${npmBin}: ${result.error.message}`);
        }
        if (result.status !== 0) {
            throw new Error(`${npmBin} install in ${SCRIPT_DIR} exited ${result.status}`);
        }
    }
    return REQUIRE(path.join(SCRIPT_DIR, "node_modules", "esbuild"));
}

async function main() {
    const args = parseArgs(process.argv.slice(2));
    if (!fs.existsSync(args.input)) {
        console.error(`bundle_threejs_for_jsc: input not found: ${args.input}`);
        process.exit(1);
    }

    const esbuild = await loadEsbuild();

    // esbuild plugin: strip http: imports before esbuild sees them as
    // resolves. Three.js HEAD imports a dev-only debugging helper from
    // greggman.github.io; JSC has no network resolver so leaving the
    // import in produces a parse error at runtime. Intercept the
    // resolve and load an empty stub instead.
    const stripHttpImports = {
        name: "pulp-strip-http-imports",
        setup(build) {
            build.onResolve({ filter: /^https?:\/\// }, (a) => ({
                path: a.path,
                namespace: "pulp-stripped",
            }));
            build.onLoad({ filter: /.*/, namespace: "pulp-stripped" }, () => ({
                contents: "/* stripped dev-only http: import */",
                loader: "js",
            }));
        },
    };

    const inputAbs = path.resolve(args.input);

    // Base esbuild options shared by both the plain three-only bundle and the
    // three + OrbitControls bundle.
    const buildOpts = {
        bundle: true,
        format: "iife",
        globalName: "__pulp_three_iife_namespace__",
        platform: "neutral",
        target: ["es2020"],
        write: false,
        logLevel: "warning",
        plugins: [stripHttpImports],
        supported: {
            // JSC supports top-level-await but our wrapper IIFE
            // doesn't, and Three.js's webgpu entry doesn't need it.
            "top-level-await": false,
        },
        // Keep names so iPad-device stack traces map to upstream
        // Three.js symbols. Whitespace minification is fine.
        minifyWhitespace: false,
        minifyIdentifiers: false,
        minifySyntax: false,
    };

    if (args.orbitControls) {
        const orbitAbs = path.resolve(args.orbitControls);
        if (!fs.existsSync(orbitAbs)) {
            console.error(`bundle_threejs_for_jsc: --orbit-controls not found: ${orbitAbs}`);
            process.exit(1);
        }
        // Synthetic entry that re-exports everything from three.webgpu.js plus
        // OrbitControls. OrbitControls.js imports from the bare specifier
        // 'three'; alias it to the SAME absolute three.webgpu.js path so esbuild
        // resolves both references to one module instance (no duplicate three,
        // so `OrbitControls extends Controls` uses the same class). three.webgpu
        // re-exports Controls/MOUSE/TOUCH/Spherical/etc. from three.core, which
        // is exactly OrbitControls' import set.
        buildOpts.stdin = {
            contents:
                `export * from ${JSON.stringify(inputAbs)};\n` +
                `export { OrbitControls } from ${JSON.stringify(orbitAbs)};\n`,
            resolveDir: path.dirname(inputAbs),
            sourcefile: "pulp-three-iife-entry.js",
            loader: "js",
        };
        buildOpts.alias = { three: inputAbs };
    } else {
        buildOpts.entryPoints = [inputAbs];
    }

    let result;
    try {
        result = await esbuild.build(buildOpts);
    } catch (err) {
        console.error("bundle_threejs_for_jsc: esbuild build failed");
        console.error(err.message || err);
        process.exit(1);
    }

    if (!result.outputFiles || result.outputFiles.length === 0) {
        console.error("bundle_threejs_for_jsc: esbuild produced no output files");
        process.exit(1);
    }

    const esbuildIife = result.outputFiles[0].text;

    // esbuild's iife output assigns the namespace to a top-level `var
    // __pulp_three_iife_namespace__ = ...`. We re-emit it inside a
    // Pulp wrapper that exposes the namespace at `globalThis.THREE`
    // (what iOS-D.3c's scene.js and any Pulp plugin author looks up)
    // and prints a console marker so the iPad walkthrough has one
    // line to grep for to confirm Three.js loaded.
    const wrapped =
        "// Auto-generated by tools/scripts/bundle_threejs_for_jsc.mjs (esbuild path).\n" +
        "// Do not edit by hand. Source: three.webgpu.js (pinned in tools/deps/manifest.json).\n" +
        "// IIFE wrapper resolves ESM imports across three.webgpu.js + three.core.js +\n" +
        "// sibling modules.\n" +
        esbuildIife +
        "\n" +
        "(function () {\n" +
        "    var globalScope = (typeof globalThis !== \"undefined\") ? globalThis :\n" +
        "        (typeof self !== \"undefined\") ? self :\n" +
        "        (typeof window !== \"undefined\") ? window : this;\n" +
        "    if (typeof __pulp_three_iife_namespace__ === \"undefined\") return;\n" +
        "    globalScope.THREE = Object.assign(globalScope.THREE || {}, __pulp_three_iife_namespace__);\n" +
        "    if (typeof globalScope.print === \"function\") {\n" +
        "        var count = Object.keys(__pulp_three_iife_namespace__).length;\n" +
        "        globalScope.print(\"PULP_THREEJS: globalThis.THREE available (\" + count + \" exports)\");\n" +
        "    }\n" +
        "})();\n";

    fs.writeFileSync(args.output, wrapped, "utf8");
    process.stdout.write(
        `bundle_threejs_for_jsc: wrote ${args.output} (${wrapped.length} bytes)\n`,
    );
}

main().catch((err) => {
    console.error(err.stack || err);
    process.exit(1);
});
