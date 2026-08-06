#!/usr/bin/env bash
# Builds the timeline conformance runner to WebAssembly and runs the corpus
# through it under node.
#
# The runner's header says the same binary runs "compiled to WASM". This script
# is what turns that into a checked statement, and it deliberately does two
# runs, not one:
#
#   1. the committed corpus must PASS (exit 0);
#   2. a deliberately broken COPY of it must FAIL (exit 1) with the broken
#      fixture named.
#
# Step 2 is not decoration. A lane that only ever runs a good corpus proves the
# binary starts, not that it judges — it would stay green against a runner whose
# wasm build silently validated nothing. Breaking a copy and requiring red is
# what separates the two. The committed corpus is never written to; the copy
# lives in a temp dir that is removed on exit.
#
# Usage:
#   tools/ci/wasm-fixture-lane.sh [build-dir] [jobs]
#
# Requires `emcc`/`emcmake` on PATH (CI: mymindstorm/setup-emsdk; locally:
# `source ~/emsdk/emsdk_env.sh`) and `node`.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${1:-${repo_root}/build-wasm-fixture}"
jobs="${2:-4}"
corpus="${repo_root}/test/fixtures/timeline"
runner="${build_dir}/pulp-fixture-runner.js"

for tool in emcmake node; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "wasm-fixture-lane: '$tool' not on PATH" >&2
        exit 2
    }
done

scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT

echo "== build (emscripten) =="
emcmake cmake -S "${repo_root}/core/interchange/wasm" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" -j"$jobs"

echo
echo "== the committed corpus must pass =="
node "$runner" --corpus "$corpus"
echo "wasm-fixture-lane: corpus PASSED under wasm"

echo
echo "== a broken corpus must fail (negative control) =="
# A copy, so the committed corpus is never mutated. The edit changes a value the
# runner's manifest actually asserts on: a cosmetic edit (a project's name, say)
# is outside what a `.expect` covers and would leave this control green while
# proving nothing.
cp -R "$corpus" "$scratch/corpus"
python3 - "$scratch/corpus/v1/minimal.json" <<'PY'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
text = p.read_text()
needle = '"next_item_id":"3"'
if needle not in text:
    # The fixture moved out from under the control. Failing here is correct:
    # silently not-corrupting would turn this into a second passing run.
    sys.exit(f"negative control cannot corrupt {p}: {needle!r} not present")
p.write_text(text.replace(needle, '"next_item_id":"4"'))
PY

set +e
out="$(node "$runner" --corpus "$scratch/corpus" 2>&1)"
status=$?
set -e

if [ "$status" -eq 0 ]; then
    echo "$out"
    echo "wasm-fixture-lane: the broken corpus PASSED — this lane cannot go red" >&2
    exit 1
fi
case "$out" in
    *"FAIL v1/minimal.json"*) ;;
    *)
        echo "$out"
        echo "wasm-fixture-lane: exited $status but did not name the broken fixture" >&2
        exit 1
        ;;
esac
echo "wasm-fixture-lane: broken corpus FAILED as required (exit $status)"

echo
echo "wasm-fixture-lane: OK — the wasm runner passes a good corpus and fails a bad one"
