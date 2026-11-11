#!/usr/bin/env bash
# Configure the tree with tests AND examples ON. Directories that both
# test/ and examples/ add (the agent tool kit synth) must be guarded by one
# include-once check, otherwise this configure fails with duplicate targets.
set -euo pipefail

source_root="${1:?source root is required}"
work_root="$(mktemp -d "${TMPDIR:-/tmp}/pulp-tests-examples-configure.XXXXXX")"
trap 'rm -rf "${work_root}"' EXIT

cmake -S "${source_root}" -B "${work_root}/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DPULP_BUILD_TESTS=ON \
    -DPULP_BUILD_EXAMPLES=ON

echo "tests_and_examples_configure_verified=true"
