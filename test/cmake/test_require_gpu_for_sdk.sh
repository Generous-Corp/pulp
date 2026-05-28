#!/usr/bin/env bash
# Configure-time smoke for PULP_REQUIRE_GPU_FOR_SDK (pulp #1817).
#
# Verifies the two behaviors that protect the release pipeline:
#   1. With PULP_REQUIRE_GPU_FOR_SDK=ON + PULP_ENABLE_GPU=ON, configure
#      MUST fail when Skia binaries are absent, and the failure message
#      must mention "Skia binaries were not found".
#   2. With PULP_REQUIRE_GPU_FOR_SDK=OFF (the default) + the same missing
#      Skia binaries, configure MUST succeed — proving the gate does not
#      fire for developer builds, only for release builds that flip it on.
#
# This catches the pulp #1817 family of regressions: a release tarball
# shipping without GPU because Skia was missing at configure time and
# nothing failed loudly enough to notice.
#
# Strategy: configure in a tmpdir with SKIA_DIR pointed at an empty
# directory so FindSkia.cmake reliably sets SKIA_FOUND=FALSE — no need
# to stash/restore the developer's real external/skia-build/ tree.
set -euo pipefail

pulp_root="${1:-}"
if [[ -z "${pulp_root}" ]]; then
    echo "usage: $0 <pulp-source-dir>" >&2
    exit 2
fi
pulp_root="$(cd "${pulp_root}" && pwd)"

tmp_root="$(mktemp -d -t pulp-require-gpu-XXXXXX)"
trap 'rm -rf "${tmp_root}"' EXIT

# Empty fake-skia dir so FindSkia.cmake's EXISTS checks all return false.
fake_skia="${tmp_root}/fake-skia"
mkdir -p "${fake_skia}"

# ── Case 1: PULP_REQUIRE_GPU_FOR_SDK=ON must hard-fail without Skia ──
case1_build="${tmp_root}/case1-require-on"
case1_log="${tmp_root}/case1.log"

set +e
cmake \
    -S "${pulp_root}" \
    -B "${case1_build}" \
    -DSKIA_DIR="${fake_skia}" \
    -DPULP_ENABLE_GPU=ON \
    -DPULP_REQUIRE_GPU_FOR_SDK=ON \
    -DPULP_BUILD_TESTS=OFF \
    -DPULP_BUILD_EXAMPLES=OFF \
    >"${case1_log}" 2>&1
case1_status=$?
set -e

if [[ ${case1_status} -eq 0 ]]; then
    echo "FAIL: PULP_REQUIRE_GPU_FOR_SDK=ON should have failed configure but it succeeded" >&2
    tail -n 60 "${case1_log}" >&2
    exit 1
fi

if ! grep -q "Skia binaries were not found" "${case1_log}"; then
    echo "FAIL: expected 'Skia binaries were not found' in configure stderr" >&2
    tail -n 80 "${case1_log}" >&2
    exit 1
fi

echo "OK case 1: PULP_REQUIRE_GPU_FOR_SDK=ON correctly failed configure when Skia is absent"

# ── Case 2: default (OFF) must allow configure to proceed ────────────
#
# With PULP_REQUIRE_GPU_FOR_SDK=OFF (the default), a missing Skia must
# NOT block configure — developers building locally without Skia get
# a CG fallback build instead of a broken configure. Note we still
# point SKIA_DIR at the empty dir so the no-Skia state is reproducible.
#
# We don't run the full configure to completion (that would build SDL3
# etc. — too slow for a CMake smoke); instead we run with --check-stamp-list
# moot and just verify the first ~150 lines of configure output do NOT
# show the FATAL_ERROR. Implementation: run with `--log-level=ERROR` and
# `--graphviz=/dev/null` no — simpler: just run configure to completion
# but parallelize tests so the cost is amortized. The expected wallclock
# for this single-pass smoke is ~30s on macOS, similar to the iOS smoke.
case2_build="${tmp_root}/case2-require-off"
case2_log="${tmp_root}/case2.log"

set +e
cmake \
    -S "${pulp_root}" \
    -B "${case2_build}" \
    -DSKIA_DIR="${fake_skia}" \
    -DPULP_ENABLE_GPU=ON \
    -DPULP_REQUIRE_GPU_FOR_SDK=OFF \
    -DPULP_BUILD_TESTS=OFF \
    -DPULP_BUILD_EXAMPLES=OFF \
    >"${case2_log}" 2>&1
case2_status=$?
set -e

if [[ ${case2_status} -ne 0 ]]; then
    echo "FAIL: default (PULP_REQUIRE_GPU_FOR_SDK=OFF) configure should succeed without Skia but exited ${case2_status}" >&2
    tail -n 80 "${case2_log}" >&2
    exit 1
fi

if grep -q "PULP_REQUIRE_GPU_FOR_SDK=ON but Skia binaries were not found" "${case2_log}"; then
    echo "FAIL: default build tripped the PULP_REQUIRE_GPU_FOR_SDK gate (it should only fire when ON)" >&2
    exit 1
fi

echo "OK case 2: PULP_REQUIRE_GPU_FOR_SDK=OFF (default) configures without Skia"

# ── Case 3: PULP_REQUIRE_GPU_FOR_SDK=ON + PULP_ENABLE_GPU=OFF contradicts ─
#
# Phase iOS-D.1 added an inverse guard (planning/2026-05-28-ios-d-gpu-auv3-
# crosscheck.md): asking the release lane to enforce GPU while disabling
# it is a misconfiguration that the SDK consumer can never recover from.
# Without this guard the previous Case 1 was the only enforcement, and a
# release lane that flipped both flags would silently ship a CG-only SDK.
case3_build="${tmp_root}/case3-require-on-gpu-off"
case3_log="${tmp_root}/case3.log"

set +e
cmake \
    -S "${pulp_root}" \
    -B "${case3_build}" \
    -DSKIA_DIR="${fake_skia}" \
    -DPULP_ENABLE_GPU=OFF \
    -DPULP_REQUIRE_GPU_FOR_SDK=ON \
    -DPULP_BUILD_TESTS=OFF \
    -DPULP_BUILD_EXAMPLES=OFF \
    >"${case3_log}" 2>&1
case3_status=$?
set -e

if [[ ${case3_status} -eq 0 ]]; then
    echo "FAIL: REQUIRE_GPU_FOR_SDK=ON + ENABLE_GPU=OFF should have failed configure but it succeeded" >&2
    tail -n 60 "${case3_log}" >&2
    exit 1
fi

if ! grep -q "PULP_ENABLE_GPU=OFF" "${case3_log}"; then
    echo "FAIL: expected the contradiction diagnostic to mention PULP_ENABLE_GPU=OFF" >&2
    tail -n 80 "${case3_log}" >&2
    exit 1
fi

echo "OK case 3: PULP_REQUIRE_GPU_FOR_SDK=ON + PULP_ENABLE_GPU=OFF correctly failed configure"
echo "OK: PULP_REQUIRE_GPU_FOR_SDK gate behaves correctly (pulp #1817 + Phase iOS-D.1)"
