#!/usr/bin/env bash
# Configure-time smoke for the reserved PULP_ENABLE_SWIFT option.
#
# The top-level option is kept for compatibility with older configure
# invocations, but it must not silently imply that it enables the Apple Swift
# layer. Swift support is actually wired through apple/Package.swift and target
# helpers such as pulp_add_ios_host_app(), which call enable_language(Swift)
# only when Swift sources are present.
set -euo pipefail

pulp_root="${1:-}"
if [[ -z "${pulp_root}" ]]; then
    echo "usage: $0 <pulp-source-dir>" >&2
    exit 2
fi
pulp_root="$(cd "${pulp_root}" && pwd)"

tmp_root="$(mktemp -d -t pulp-swift-option-XXXXXX)"
trap 'rm -rf "${tmp_root}"' EXIT

run_configure() {
    local swift_value="$1"
    local build_dir="$2"
    local log_file="$3"

    set +e
    cmake \
        -S "${pulp_root}" \
        -B "${build_dir}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DPULP_ENABLE_SWIFT="${swift_value}" \
        -DPULP_ENABLE_GPU=OFF \
        -DPULP_TEXT_SHAPING=OFF \
        -DPULP_BUILD_TESTS=OFF \
        -DPULP_BUILD_EXAMPLES=OFF \
        >"${log_file}" 2>&1
    local status=$?
    set -e

    if [[ ${status} -ne 0 ]]; then
        echo "FAIL: configure with PULP_ENABLE_SWIFT=${swift_value} exited ${status}" >&2
        tail -n 120 "${log_file}" >&2
        exit 1
    fi
}

on_build="${tmp_root}/on-build"
on_log="${tmp_root}/on.log"
run_configure ON "${on_build}" "${on_log}"

if ! grep -q "PULP_ENABLE_SWIFT is reserved and currently has no effect" "${on_log}"; then
    echo "FAIL: expected reserved PULP_ENABLE_SWIFT warning" >&2
    tail -n 80 "${on_log}" >&2
    exit 1
fi

if ! grep -q "pulp_add_ios_host_app" "${on_log}"; then
    echo "FAIL: warning should point at the target helper that enables Swift" >&2
    tail -n 80 "${on_log}" >&2
    exit 1
fi

off_build="${tmp_root}/off-build"
off_log="${tmp_root}/off.log"
run_configure OFF "${off_build}" "${off_log}"

if grep -q "PULP_ENABLE_SWIFT is reserved and currently has no effect" "${off_log}"; then
    echo "FAIL: reserved PULP_ENABLE_SWIFT warning should only fire when ON" >&2
    tail -n 80 "${off_log}" >&2
    exit 1
fi

echo "OK: PULP_ENABLE_SWIFT=ON warns that the option is reserved/no-op"
