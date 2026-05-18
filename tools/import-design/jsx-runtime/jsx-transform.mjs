#!/usr/bin/env node
// jsx-transform.mjs — compile a single-file React JSX/TSX instrument into a
// self-contained IIFE bundle suitable for Pulp's runtime-import pipeline.
//
// Usage:
//   node jsx-transform.mjs --in chainer.jsx --out chainer-bundle.js [--external-react]
//
// Output: an IIFE bundle whose IIFE body:
//   1. installs `globalThis.React` from the bundled react module
//   2. installs `globalThis.ReactDOM` from the bundled react-dom/client module
//   3. evaluates the user's JSX file (default export must be a React component)
//   4. mounts `<UserComponent/>` into `document.getElementById('root')`
//
// Designed to be consumed by parse_jsx_react() — the C++ side wraps this
// output as a single ClaudeBundleAsset of MIME text/javascript and embeds it
// in a synthesized Claude-style HTML template.
//
// Why a Node preprocessor instead of in-process Babel:
//   * esbuild handles JSX + module bundling out of the box.
//   * Embedding Babel-standalone (~3 MB) inside QuickJS for offline import
//     puts parser-stack risk on the runtime-import lane, which already has
//     JS-size caps.
//   * Aligns with Codex's recommendation (2026-05-17 consult).
//
// Follow-up: replace this with embedded esbuild-wasm or sucrase when the
// dependency story matters for production.

import { build } from 'esbuild';
import { resolve, dirname, basename, extname } from 'node:path';
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import process from 'node:process';

const __dirname = dirname(fileURLToPath(import.meta.url));

function parseArgs(argv) {
    const out = { in: null, out: null, externalReact: false, verbose: false };
    for (let i = 0; i < argv.length; i++) {
        const a = argv[i];
        if (a === '--in') out.in = argv[++i];
        else if (a === '--out') out.out = argv[++i];
        else if (a === '--external-react') out.externalReact = true;
        else if (a === '--verbose' || a === '-v') out.verbose = true;
        else if (a === '--help' || a === '-h') {
            console.log('Usage: jsx-transform.mjs --in <jsx-file> --out <out-bundle.js> [--external-react]');
            process.exit(0);
        }
    }
    if (!out.in || !out.out) {
        console.error('Error: --in and --out are required');
        process.exit(2);
    }
    return out;
}

// Wrapper template that:
//   - exposes the bundled React/ReactDOM at globalThis
//   - imports the user's JSX as a default export
//   - mounts it on document.getElementById('root') after DOMContentLoaded
//
// The user's JSX file is bundled in via the entry point; this wrapper is
// generated as a synthetic stdin entry.
// Pre-payload sandbox shims emitted as a literal banner BEFORE esbuild's
// IIFE wrapper. ES module imports are hoisted to the top of the IIFE
// body, so an inline shim at the top of the entry source would run
// AFTER react-dom/client init — too late, because react-dom calls
// `navigator.userAgent.indexOf(...)` during its DevTools sniff at
// module-load time. Mirrors what
// `run_claude_bundle_payload_pipeline` installs before evaluating a
// Claude bundle (core/view/src/design_import.cpp:1494).
const PRE_SHIMS = `
(function () {
    var g = (typeof globalThis !== 'undefined') ? globalThis : window;
    if (typeof navigator === 'undefined' || !navigator) {
        try { g.navigator = {}; } catch (e) {}
    }
    if (g.navigator && typeof g.navigator.userAgent !== 'string') {
        try { g.navigator.userAgent = 'PulpJsxRuntime/1.0'; } catch (e) {}
    }
    if (typeof document !== 'undefined' && document) {
        if (typeof document.createElementNS !== 'function') {
            document.createElementNS = function (ns, type) {
                var el = document.createElement(type);
                try { el.namespaceURI = ns; } catch (e) {}
                return el;
            };
        }
        if (typeof document.createTextNode !== 'function') {
            document.createTextNode = function (text) {
                var el = document.createElement('#text');
                try { el.nodeValue = text; el.textContent = text; } catch (e) {}
                return el;
            };
        }
    }
    var ctorNames = ['Element','HTMLElement','Node','EventTarget',
        'HTMLIFrameElement','HTMLInputElement','HTMLTextAreaElement',
        'HTMLSelectElement','HTMLOptionElement','HTMLOptGroupElement',
        'HTMLFormElement','HTMLAnchorElement','HTMLImageElement',
        'HTMLDivElement','HTMLSpanElement','HTMLButtonElement',
        'HTMLLabelElement','HTMLCanvasElement','HTMLBodyElement',
        'HTMLDocument','SVGElement','DocumentFragment','Text','Comment'];
    for (var i = 0; i < ctorNames.length; i++) {
        var n = ctorNames[i];
        if (typeof g[n] === 'undefined') {
            try { g[n] = function(){}; } catch (e) {}
        }
    }
    // window/document/globalThis addEventListener stubs so React-DOM's
    // event-system init doesn't throw. Real dispatch comes from the
    // WidgetBridge via __pulpRuntimeImport__ pump in the runtime path.
    var listeners = (g.__pulpJsxListeners__ = g.__pulpJsxListeners__ || {});
    function stubAdd(target) {
        if (!target) return;
        if (typeof target.addEventListener !== 'function') {
            target.addEventListener = function (type, fn) {
                if (!fn) return;
                (listeners[type] || (listeners[type] = [])).push(fn);
            };
        }
        if (typeof target.removeEventListener !== 'function') {
            target.removeEventListener = function (type, fn) {
                var list = listeners[type];
                if (!list) return;
                var i = list.indexOf(fn);
                if (i >= 0) list.splice(i, 1);
            };
        }
    }
    stubAdd(g);
    if (typeof document !== 'undefined') stubAdd(document);
    if (typeof window !== 'undefined') stubAdd(window);
})();
`;

function buildEntry(userFilePath, componentName) {
    return `
import * as ReactNS from 'react';
import * as ReactDOMClient from 'react-dom/client';
import UserComponent from ${JSON.stringify(userFilePath)};

// Pin host React/ReactDOM at globalThis so any code (including bundled deps)
// that reads globalThis.React picks up the same module instance.
const _g = (typeof globalThis !== 'undefined') ? globalThis : window;
_g.React = ReactNS;
_g.ReactDOM = {
    createRoot: ReactDOMClient.createRoot,
    hydrateRoot: ReactDOMClient.hydrateRoot,
    // legacy:
    render: function legacyRender(element, container) {
        const root = ReactDOMClient.createRoot(container);
        root.render(element);
        return { unmount: () => root.unmount() };
    }
};

function mount() {
    let mountEl = (typeof document !== 'undefined' && document.getElementById) ? document.getElementById('root') : null;
    if (!mountEl && typeof document !== 'undefined') {
        mountEl = document.body || document.documentElement;
    }
    if (!mountEl) {
        const errMsg = '[pulp-jsx] no mount target — document.getElementById("root") returned null and document.body is unavailable';
        if (typeof console !== 'undefined' && console.error) console.error(errMsg);
        return;
    }
    try {
        const root = ReactDOMClient.createRoot(mountEl);
        root.render(ReactNS.createElement(UserComponent));
        _g.__pulpJsxMounted__ = ${JSON.stringify(componentName)};
    } catch (e) {
        const errMsg = '[pulp-jsx] mount failed: ' + (e && e.message ? e.message : String(e));
        if (typeof console !== 'undefined' && console.error) console.error(errMsg);
        _g.__pulpJsxError__ = errMsg;
    }
}

// Mount immediately. The runtime-import pipeline dispatches DOMContentLoaded
// after evaluating bundle assets, so we don't need to wait for it.
mount();
`;
}

function detectComponentName(jsxSource, filePath) {
    // Try a few defensive patterns for the default export name:
    //   export default function Foo() { ... }
    //   export default function ChainerInstrument() ...
    //   const Foo = () => ...; export default Foo;
    //   function Foo() ...; export default Foo;
    const patterns = [
        /export\s+default\s+function\s+([A-Z][A-Za-z0-9_]*)\s*\(/,
        /export\s+default\s+([A-Z][A-Za-z0-9_]*)\s*;/,
        /export\s+default\s+([A-Z][A-Za-z0-9_]*)\s*$/m,
    ];
    for (const re of patterns) {
        const m = jsxSource.match(re);
        if (m) return m[1];
    }
    // Fallback to filename
    return basename(filePath, extname(filePath))
        .replace(/[^a-zA-Z0-9]/g, '_')
        .replace(/^([a-z])/, (_, c) => c.toUpperCase());
}

async function main() {
    const args = parseArgs(process.argv.slice(2));

    const inPath = resolve(args.in);
    const outPath = resolve(args.out);
    const outDir = dirname(outPath);
    try { mkdirSync(outDir, { recursive: true }); } catch {}

    let jsxSource;
    try {
        jsxSource = readFileSync(inPath, 'utf8');
    } catch (e) {
        console.error(`Error: cannot read input file ${inPath}: ${e.message}`);
        process.exit(2);
    }

    const componentName = detectComponentName(jsxSource, inPath);
    if (args.verbose) console.error(`[pulp-jsx-transform] detected component: ${componentName}`);

    // Pick esbuild loader from the file extension. TypeScript inputs
    // route through esbuild's type-stripping loader (per Codex consult,
    // 2026-05-17): React `.tsx` is the dominant React file shape today,
    // and esbuild's tsx loader handles the type erasure (annotations,
    // generics, `as` casts, `type`/`interface` declarations, etc.) at
    // bundle time. No separate tsc step required.
    const ext = extname(inPath).toLowerCase();
    const isTypeScript = (ext === '.tsx' || ext === '.ts');
    const entryLoader = isTypeScript ? 'tsx' : 'jsx';
    const entrySource = isTypeScript ? '__pulp_jsx_entry__.tsx' : '__pulp_jsx_entry__.jsx';
    if (args.verbose && isTypeScript) {
        console.error(`[pulp-jsx-transform] TypeScript input — using esbuild '${entryLoader}' loader`);
    }

    const entryContents = buildEntry(inPath, componentName);

    try {
        const result = await build({
            stdin: {
                contents: entryContents,
                resolveDir: dirname(inPath),
                loader: entryLoader,
                sourcefile: entrySource,
            },
            bundle: true,
            format: 'iife',
            target: ['es2020'],
            platform: 'browser',
            globalName: 'PulpJsxApp',
            write: false,
            sourcemap: false,
            minify: false,
            jsx: 'transform',
            jsxFactory: 'React.createElement',
            jsxFragment: 'React.Fragment',
            logLevel: args.verbose ? 'info' : 'warning',
            // Per-extension loader map. The .ts/.tsx entries let
            // esbuild strip TypeScript from any imports the entry
            // pulls in (rare for a single-file instrument, but
            // forward-compatible with future multi-file fixtures).
            loader: { '.jsx': 'jsx', '.tsx': 'tsx', '.ts': 'ts' },
            // Node-resolution paths: prefer the runtime's own node_modules so we
            // get a consistent React/ReactDOM regardless of the user's package
            // tree.
            nodePaths: [
                resolve(__dirname, 'node_modules'),
                // pulp jsx-instrument-import 2026-05-17 — @pulp/react
                // externalizes react/react-reconciler/scheduler at its
                // own build time. When we re-bundle @pulp/react into the
                // user's JSX import, esbuild needs to resolve those —
                // they live in @pulp/react's node_modules.
                resolve(__dirname, '..', '..', '..', 'packages', 'pulp-react', 'node_modules'),
            ],
            // pulp jsx-instrument-import 2026-05-17 — temporarily reverted
            // the react-dom → @pulp/react alias. Re-bundling @pulp/react
            // duplicated React (esbuild emitted `useState2` for Chainer's
            // imports, distinct from @pulp/react's React) — the bundled
            // user component crashed with "Cannot read properties of null
            // (reading 'useState')" inside renderWithHooks. The dedup
            // requires a proper externalization strategy (@pulp/react's
            // build externalizes react; our re-bundle needs to externalize
            // it too, then resolve to the SAME react instance the user
            // imports). Follow-up PR scope.
            //
            // For now: use the working bundled-ReactDOM path. Static render
            // is perfect; interactivity is the next-PR scope per Codex's
            // 2-week-trap warning.
            // Banner emits BEFORE esbuild's IIFE wrapper — that's
            // where the sandbox shims must live so they run before any
            // ESM import (esp. react-dom/client's DevTools UA sniff).
            banner: {
                js: `/* pulp import-design --from jsx | component=${componentName} | esbuild bundle */\n${PRE_SHIMS}`,
            },
        });

        if (result.errors && result.errors.length) {
            for (const err of result.errors) {
                console.error(`[esbuild] ${err.text}`);
            }
            process.exit(4);
        }

        const out = result.outputFiles[0];
        writeFileSync(outPath, out.contents);

        // Emit a tiny manifest so the C++ side can know what came out
        // without re-parsing the bundle.
        const manifest = {
            componentName,
            sourceFile: inPath,
            outputBytes: out.contents.byteLength,
            generatedAt: new Date().toISOString(),
            esbuildVersion: (await import('esbuild')).default?.version || 'unknown',
        };
        writeFileSync(outPath + '.manifest.json', JSON.stringify(manifest, null, 2));

        if (args.verbose) {
            console.error(`[pulp-jsx-transform] wrote ${out.contents.byteLength} bytes to ${outPath}`);
            console.error(`[pulp-jsx-transform] manifest: ${outPath}.manifest.json`);
        }
    } catch (e) {
        console.error(`[pulp-jsx-transform] build failed: ${e.message || e}`);
        if (args.verbose && e.stack) console.error(e.stack);
        process.exit(5);
    }
}

main().catch(e => {
    console.error('[pulp-jsx-transform] unexpected error:', e);
    process.exit(1);
});
