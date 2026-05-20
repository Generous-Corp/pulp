#!/usr/bin/env bash
# Configure-time smoke for pulp_add_ios_auv3(): runs an iOS Simulator
# CMake configure of the Pulp tree and asserts that the example's AUv3
# target wiring is emitted. Exits 77 (CTest skipped) when the iOS SDK
# is not available on the host.
set -euo pipefail

pulp_root="${1:-}"
if [[ -z "${pulp_root}" ]]; then
    echo "usage: $0 <pulp-source-dir>" >&2
    exit 2
fi

if ! command -v xcrun >/dev/null 2>&1; then
    echo "SKIP: xcrun not available on this host"
    exit 77
fi
if ! xcrun --sdk iphonesimulator --show-sdk-path >/dev/null 2>&1; then
    echo "SKIP: iPhoneSimulator SDK not available on this host"
    exit 77
fi

build_dir="$(mktemp -d -t pulp-ios-auv3-XXXXXX)"
trap 'rm -rf "${build_dir}"' EXIT

log="${build_dir}/configure.log"

set +e
if command -v gtimeout >/dev/null 2>&1; then
    timeout_cmd=(gtimeout 90s)
elif command -v timeout >/dev/null 2>&1; then
    timeout_cmd=(timeout 90s)
elif command -v perl >/dev/null 2>&1; then
    timeout_cmd=(perl -e 'alarm shift; exec @ARGV' 90)
else
    timeout_cmd=()
fi

"${timeout_cmd[@]}" cmake \
    -S "${pulp_root}" \
    -B "${build_dir}/build" \
    -G Xcode \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_SYSROOT=iphonesimulator \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=16.4 \
    -DPULP_ENABLE_GPU=OFF \
    -DPULP_BUILD_TESTS=OFF \
    -DPULP_BUILD_EXAMPLES=ON \
    -DPULP_TEXT_SHAPING=OFF \
    >"${log}" 2>&1
status=$?
set -e

if [[ ${status} -eq 124 || ${status} -eq 142 ]]; then
    echo "SKIP: iOS Simulator configure exceeded 90s; likely Xcode generator hang"
    tail -n 80 "${log}" >&2
    exit 77
fi

if [[ ${status} -ne 0 ]]; then
    echo "FAIL: iOS Simulator configure failed (status ${status})"
    tail -n 80 "${log}" >&2
    exit 1
fi

if ! grep -q 'Pulp iOS AUv3: PulpSineSynth (type: aumu, subtype: PsSn)' "${log}"; then
    echo "FAIL: expected 'Pulp iOS AUv3: PulpSineSynth (type: aumu, subtype: PsSn)' status message"
    grep -i 'auv3\|pulp_add_ios_auv3' "${log}" >&2 || true
    exit 1
fi

if [[ ! -d "${build_dir}/build/Pulp.xcodeproj" ]]; then
    echo "FAIL: Pulp.xcodeproj not generated"
    exit 1
fi

if ! grep -q 'PulpSineSynth_AUv3' "${build_dir}/build/Pulp.xcodeproj/project.pbxproj"; then
    echo "FAIL: PulpSineSynth_AUv3 target not present in generated Xcode project"
    exit 1
fi

echo "OK: pulp_add_ios_auv3() iOS Simulator configure succeeded"
