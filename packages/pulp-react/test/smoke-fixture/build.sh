#!/usr/bin/env bash
# Build the smoke-test fixture into a self-contained JS bundle that
# tools/screenshot/pulp-screenshot can load + render. Run from repo root:
#
#   ./packages/pulp-react/test/smoke-fixture/build.sh
#   ./build/tools/screenshot/pulp-screenshot \
#       --script packages/pulp-react/test/smoke-fixture/dist/app.js \
#       --output /tmp/pulp-react-smoke.png \
#       --width 800 --height 600
#
# Pre-reqs: npm install in packages/pulp-react/ (esbuild is a devDep)

set -euo pipefail

cd "$(dirname "$0")"
SRC=app.tsx
OUT=dist/app.js

mkdir -p dist

# Bundle React + react-reconciler + scheduler + @pulp/react + the fixture
# into a single IIFE that runs against bridge globals. No DOM polyfill,
# no Babel-standalone, no asynchronous module loading.
../../node_modules/.bin/esbuild "$SRC" \
    --bundle \
    --format=iife \
    --platform=neutral \
    --target=es2020 \
    --jsx-factory=createElement \
    --jsx-fragment=Fragment \
    --define:process.env.NODE_ENV='"production"' \
    --outfile="$OUT"

echo "wrote $(pwd)/$OUT ($(wc -c < $OUT) bytes)"
