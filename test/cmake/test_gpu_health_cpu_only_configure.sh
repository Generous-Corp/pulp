#!/usr/bin/env bash
set -euo pipefail

source_root="${1:?source root is required}"
work_root="$(mktemp -d "${TMPDIR:-/tmp}/pulp-gpu-health-cpu-only.XXXXXX")"
trap 'rm -rf "${work_root}"' EXIT

cmake -S "${source_root}" -B "${work_root}/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DPULP_ENABLE_GPU=OFF \
    -DPULP_BUILD_TESTS=OFF \
    -DPULP_BUILD_EXAMPLES=OFF
# `cmake --build` against a Ninja tree with no job bound is not serial: Ninja's
# own default is cores + 2, so this whole-target build claims more than the
# machine has. That is a share the governor owns, not one the hardware does --
# this case runs as a ctest on the self-hosted Studios that also host the
# required `macos` gate, beside other agents' builds. Per the wrapper's
# contract the build command carries no --parallel/-j of its own;
# CMAKE_BUILD_PARALLEL_LEVEL from the wrapper governs it.
"${source_root}/tools/ci/governed-build.sh" \
    cmake --build "${work_root}/build" \
    --target pulp-tool-gpu-health-model pulp-mcp-core

echo "gpu_health_cpu_only_configure_verified=true"
