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
cmake --build "${work_root}/build" \
    --target pulp-tool-gpu-health-model pulp-mcp-core

echo "gpu_health_cpu_only_configure_verified=true"
