# jsx-runtime — Node-side compile + bundle for `pulp import-design --from jsx`

Self-contained Node + esbuild environment that compiles a single-file React
JSX instrument into a self-mounting IIFE bundle suitable for Pulp's existing
Claude-style runtime-import harness.

See `planning/2026-05-17-jsx-instrument-import.md` for the full design.

## Install (first run)

```bash
cd tools/import-design/jsx-runtime
npm install
```

Pulls React 18.3.1, ReactDOM 18.3.1, react-reconciler 0.29.2, scheduler
0.23.2, esbuild 0.24.0, `@babel/parser` 7.29.7, and `css-tree` 3.2.1 into
the local `node_modules/`. Not committed (see `.gitignore`);
`package-lock.json` IS committed for reproducible installs.
The transform aliases `@pulp/react` to the repo's `packages/pulp-react/src`
entrypoint so a fresh checkout can bundle the native React host without
publishing or linking the package first.

## Usage

```bash
node jsx-transform.mjs \
  --in path/to/MyInstrument.jsx \
  --out /tmp/my-instrument-bundle.js \
  [--verbose]
```

Produces:
- `<out>` — the IIFE bundle (~1 MB, React + @pulp/react native bridge + user JSX + nav shims)
- `<out>.manifest.json` — `{ componentName, sourceFile, outputBytes, ... }`

The bundle is then consumable by:
- `pulp import-design --from jsx --file <out> --mode live --emit js`
- `pulp import-design --from jsx --file <out> --mode baked --emit ir-json --snapshot-semantics accept`
- `pulp import-design --from jsx --file <out> --mode baked --emit cpp --snapshot-semantics accept`
- `pulp-screenshot --script <out> --output render.png` (works today)
- `parse_jsx_react()` in C++ via the test harness

The default bundle routes `react-dom` through `pulp-react-dom-shim.mjs`, which
renders through `@pulp/react` into the native bridge. Baked snapshots first try
the DOM walker, then freeze the native `WidgetBridge` tree when the bundle has
no expanded DOM; those IR envelopes record `runtime_native_snapshot` and
`root.attributes.snapshotSource = "native-view"`.

## Source-contract audit

```bash
node jsx-contract-audit.mjs \
  --in path/to/MyInstrument.jsx \
  --json /tmp/my-instrument-contract.json \
  --markdown /tmp/my-instrument-contract.md \
  --fail-on-weak-proof
```

This is a static evidence pass, not visual inference. `@babel/parser` extracts
JSX elements, props, handler closures, `.map()` rows, source spans, and inline
SVG/vector nodes. `css-tree` parses and validates style values so the importer
can normalize radius, border, color, size, overflow, and related style
contracts into Pulp-owned native attributes. The live runtime remains the
fallback when a source contract is too dynamic to lower safely.

## End-to-end smoke

```bash
tools/import-validation/jsx-roundtrip.sh
```

Runs source-contract audit → transform → smoke test → headless render against
`planning/fixtures/jsx/chainer-instrument.jsx`.

## Why a Node preprocessor

Per Codex high-reasoning consult (2026-05-17):

> Use esbuild from a Node preprocessor, not Babel in C++. Adding a large
> embedded compiler blob inside QuickJS puts parsing/transform stack risk
> inside the runtime-import path, which already has JS size limits and
> settling complexity. Embedded esbuild-wasm/sucrase is the right long-term
> answer if "no Node required" becomes a product requirement, but it's not
> the smallest first PR.
