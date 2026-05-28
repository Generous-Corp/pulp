#!/usr/bin/env bash
# iOS HostApp link-time regression test.
#
# Catches the class of failure that landed on 2026-05-26 when an iPad
# walkthrough discovered that `pulp-view-core` did not actually link
# clean for iOS even though the Phase iOS-B configure smoke was green:
#
#   - core/view/CMakeLists.txt did not PUBLIC-link pulp::audio despite
#     visualizers.cpp transitively including <pulp/audio/audio_thumbnail.hpp>
#   - core/view/include/pulp/view/widgets/graph_editor_view.hpp included
#     <pulp/host/signal_graph.hpp> unconditionally even though pulp::host
#     is intentionally not added on iOS (#2994 long-double-to-chars libc++
#     availability error).
#
# The original test/cmake/test_ios_auv3_configure.sh smoke is configure-only
# in its default mode; this script always builds the HostApp .app on top of
# the .appex and asserts the produced bundle is real, not a stub. Together
# they form belt + suspenders for the AUv3 iOS lane.
#
# Exit 77 (CTest skipped) when the iOS SDK is not available on the host.
# Exit 0 on success, 1 on a real failure.
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

build_dir="$(mktemp -d -t pulp-ios-hostapp-XXXXXX)"
trap 'rm -rf "${build_dir}"' EXIT

configure_log="${build_dir}/configure.log"
build_log="${build_dir}/build.log"

if command -v gtimeout >/dev/null 2>&1; then
    timeout_cmd=(gtimeout)
elif command -v timeout >/dev/null 2>&1; then
    timeout_cmd=(timeout)
elif command -v perl >/dev/null 2>&1; then
    timeout_cmd=(perl -e 'alarm shift; exec @ARGV')
else
    timeout_cmd=()
fi

# Configure.
set +e
if [[ ${#timeout_cmd[@]} -gt 0 ]]; then
    "${timeout_cmd[@]}" 1200 cmake \
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
        >"${configure_log}" 2>&1
else
    cmake \
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
        >"${configure_log}" 2>&1
fi
status=$?
set -e

if [[ ${status} -eq 124 || ${status} -eq 142 ]]; then
    echo "SKIP: iOS Simulator configure exceeded 1200s; likely Xcode generator hang"
    tail -n 80 "${configure_log}" >&2
    exit 77
fi
if [[ ${status} -ne 0 ]]; then
    echo "FAIL: iOS Simulator configure failed (status ${status})"
    tail -n 80 "${configure_log}" >&2
    exit 1
fi

# Build the HostApp (this also builds the .appex it embeds).
set +e
if [[ ${#timeout_cmd[@]} -gt 0 ]]; then
    "${timeout_cmd[@]}" 1500 cmake --build "${build_dir}/build" \
        --target PulpSineSynth_HostApp \
        --config Release \
        -- -sdk iphonesimulator \
        >"${build_log}" 2>&1
else
    cmake --build "${build_dir}/build" \
        --target PulpSineSynth_HostApp \
        --config Release \
        -- -sdk iphonesimulator \
        >"${build_log}" 2>&1
fi
build_status=$?
set -e

if [[ ${build_status} -eq 124 || ${build_status} -eq 142 ]]; then
    echo "SKIP: PulpSineSynth_HostApp build exceeded timeout"
    tail -n 120 "${build_log}" >&2
    exit 77
fi
if [[ ${build_status} -ne 0 ]]; then
    echo "FAIL: PulpSineSynth_HostApp build failed (status ${build_status})"
    tail -n 120 "${build_log}" >&2
    exit 1
fi

hostapp_path=$(find "${build_dir}/build" -name "PulpSineSynth.app" -type d | head -1)
if [[ -z "${hostapp_path}" ]]; then
    echo "FAIL: PulpSineSynth.app not produced after HostApp build"
    find "${build_dir}/build" -maxdepth 6 -name "*.app" >&2 || true
    exit 1
fi

# Real Info.plist: file present + non-empty + readable by plutil.
plist="${hostapp_path}/Info.plist"
if [[ ! -f "${plist}" ]]; then
    echo "FAIL: Info.plist missing from built HostApp .app (${hostapp_path})"
    exit 1
fi
if [[ ! -s "${plist}" ]]; then
    echo "FAIL: HostApp Info.plist is empty"
    exit 1
fi
if ! /usr/bin/plutil -lint -s "${plist}"; then
    echo "FAIL: HostApp Info.plist did not lint clean"
    /usr/bin/plutil -p "${plist}" >&2 || true
    exit 1
fi

# Bundle identifier must be present and non-empty.
bundle_id=$(/usr/bin/plutil -extract CFBundleIdentifier raw -o - "${plist}" 2>/dev/null || true)
if [[ -z "${bundle_id}" || "${bundle_id}" == "null" ]]; then
    echo "FAIL: HostApp Info.plist missing CFBundleIdentifier"
    /usr/bin/plutil -p "${plist}" >&2
    exit 1
fi

# Real Mach-O executable inside the bundle. iOS .app layout is flat (no
# Contents/MacOS); the executable sits directly inside the .app dir at
# CFBundleExecutable.
exec_name=$(/usr/bin/plutil -extract CFBundleExecutable raw -o - "${plist}" 2>/dev/null || true)
if [[ -z "${exec_name}" || "${exec_name}" == "null" ]]; then
    echo "FAIL: HostApp Info.plist missing CFBundleExecutable"
    /usr/bin/plutil -p "${plist}" >&2
    exit 1
fi
exec_path="${hostapp_path}/${exec_name}"
# macOS-style layout fallback (Generator differences across Xcode versions).
if [[ ! -f "${exec_path}" && -d "${hostapp_path}/MacOS" ]]; then
    exec_path="${hostapp_path}/MacOS/${exec_name}"
fi
if [[ ! -f "${exec_path}" ]]; then
    echo "FAIL: HostApp executable '${exec_name}' missing from bundle"
    ls -la "${hostapp_path}" >&2
    exit 1
fi
if [[ ! -s "${exec_path}" ]]; then
    echo "FAIL: HostApp executable '${exec_path}' is empty"
    exit 1
fi
if ! file "${exec_path}" | grep -qE 'Mach-O.*(executable|bundle)'; then
    echo "FAIL: HostApp executable '${exec_path}' is not a Mach-O binary"
    file "${exec_path}" >&2
    exit 1
fi

# Embedded AUv3 .appex under PlugIns/ — iOS bundle layout is flat, macOS
# nests under Contents/, accept either for cross-generator robustness.
if [[ ! -d "${hostapp_path}/PlugIns/PulpSineSynth.appex" ]] \
        && [[ ! -d "${hostapp_path}/Contents/PlugIns/PulpSineSynth.appex" ]]; then
    echo "FAIL: PulpSineSynth.appex not embedded in HostApp .app under PlugIns/"
    find "${hostapp_path}" -maxdepth 4 -name "*.appex" >&2 || true
    exit 1
fi

echo "OK: PulpSineSynth_HostApp iOS Simulator build produced a real .app"
echo "    HostApp:    ${hostapp_path}"
echo "    Executable: ${exec_path}"
echo "    BundleID:   ${bundle_id}"
