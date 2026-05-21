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

Pulls React 18.3.1, ReactDOM 18.3.1, and esbuild 0.24.0 into the local
`node_modules/`. Not committed (see `.gitignore`); `package-lock.json` IS
committed for reproducible installs.

## Usage

```bash
node jsx-transform.mjs \
  --in path/to/MyInstrument.jsx \
  --out /tmp/my-instrument-bundle.js \
  [--verbose]
```

Produces:
- `<out>` — the IIFE bundle (~1 MB, React + ReactDOM + user JSX + nav shims)
- `<out>.manifest.json` — `{ componentName, sourceFile, outputBytes, ... }`

The bundle is then consumable by:
- `pulp import-design --from jsx --file <out> --mode live --emit js`
- `pulp import-design --from jsx --file <out> --mode baked --emit ir-json --snapshot-semantics accept`
- `pulp import-design --from jsx --file <out> --mode baked --emit cpp --snapshot-semantics accept`
- `pulp-screenshot --script <out> --output render.png` (works today)
- `parse_jsx_react()` in C++ via the test harness

## End-to-end smoke

```bash
tools/import-validation/jsx-roundtrip.sh
```

Runs transform → smoke test → headless render against
`planning/fixtures/jsx/chainer-instrument.jsx`.

## Why a Node preprocessor

Per Codex high-reasoning consult (2026-05-17):

> Use esbuild from a Node preprocessor, not Babel in C++. Adding a large
> embedded compiler blob inside QuickJS puts parsing/transform stack risk
> inside the runtime-import path, which already has JS size limits and
> settling complexity. Embedded esbuild-wasm/sucrase is the right long-term
> answer if "no Node required" becomes a product requirement, but it's not
> the smallest first PR.
