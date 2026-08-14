#!/usr/bin/env bash
set -euo pipefail

root=${1:?usage: test_ios_compile_gate.sh <source-root> [build-root] [--verify-contract]}
build_root=${2:-$root/build-ios-compile-gate}
mode=${3:-}

targets=(
    pulp-timebase
    pulp-timeline
    pulp-playback
    pulp-sequence
    pulp-smf-interop
    pulp-smf-interchange
    pulp-midi
    pulp-ios-coremidi-shared-client-contract
    pulp-ios-coremidi-harness
)

static_library_targets=(
    pulp-timebase
    pulp-timeline
    pulp-playback
    pulp-sequence
    pulp-smf-interop
    pulp-smf-interchange
    pulp-midi
)

verify_contract() {
    local actual expected
    actual="${targets[*]}"
    expected="pulp-timebase pulp-timeline pulp-playback pulp-sequence pulp-smf-interop pulp-smf-interchange pulp-midi pulp-ios-coremidi-shared-client-contract pulp-ios-coremidi-harness"
    if [[ "$actual" != "$expected" ]]; then
        echo "ERROR: iOS compile target contract drifted" >&2
        echo "expected: $expected" >&2
        echo "actual:   $actual" >&2
        return 1
    fi
}

verify_contract
if [[ "$mode" == "--verify-contract" ]]; then
    echo "OK: iOS compile target contract is complete"
    exit 0
fi
if [[ -n "$mode" ]]; then
    echo "ERROR: unknown mode: $mode" >&2
    exit 2
fi

if [[ $(uname -s) != Darwin ]] || ! command -v xcrun >/dev/null 2>&1; then
    echo "SKIP: iOS compile gate requires macOS with Xcode"
    exit 77
fi
for sdk in iphonesimulator iphoneos; do
    if ! xcrun --sdk "$sdk" --show-sdk-path >/dev/null 2>&1; then
        echo "SKIP: $sdk SDK is not installed"
        exit 77
    fi
done

if command -v gtimeout >/dev/null 2>&1; then
    timeout_cmd=(gtimeout)
elif command -v timeout >/dev/null 2>&1; then
    timeout_cmd=(timeout)
elif command -v perl >/dev/null 2>&1; then
    timeout_cmd=(perl -e 'alarm shift; exec @ARGV')
else
    timeout_cmd=()
fi

run_logged() {
    local label=$1
    local seconds=$2
    local log=$3
    shift 3

    echo "iOS compile: $label"
    local status=0
    if [[ ${#timeout_cmd[@]} -gt 0 ]]; then
        "${timeout_cmd[@]}" "$seconds" "$@" >"$log" 2>&1 || status=$?
    else
        "$@" >"$log" 2>&1 || status=$?
    fi
    if [[ $status -ne 0 ]]; then
        echo "ERROR: $label failed (status $status)" >&2
        tail -n 120 "$log" >&2 || true
        return "$status"
    fi
}

mkdir -p "$build_root"

for sdk in iphonesimulator iphoneos; do
    build_dir="$build_root/$sdk"
    configure_log="$build_root/configure-$sdk.log"
    build_log="$build_root/build-$sdk.log"

    run_logged "$sdk Release configure" 1200 "$configure_log" \
        cmake -S "$root" -B "$build_dir" -G Xcode \
        -DCMAKE_SYSTEM_NAME=iOS \
        -DCMAKE_OSX_SYSROOT="$sdk" \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=16.3 \
        -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
        -DCMAKE_BUILD_TYPE=Release \
        -DPULP_BUILD_TESTS=OFF \
        -DPULP_BUILD_EXAMPLES=OFF \
        -DPULP_ENABLE_GPU=OFF \
        -DPULP_TEXT_SHAPING=OFF

    run_logged "$sdk Release target build" 1800 "$build_log" \
        "$root/tools/ci/governed-build.sh" \
        cmake --build "$build_dir" --config Release --target "${targets[@]}" \
        -- -sdk "$sdk" CODE_SIGNING_ALLOWED=NO

    for target in "${static_library_targets[@]}"; do
        if ! find "$build_dir" -type f -name "lib${target}.a" -print -quit \
            | grep -q .; then
            echo "ERROR: $sdk did not produce lib${target}.a" >&2
            exit 1
        fi
    done
    if ! find "$build_dir" -type d -name PulpCoreMidiHarness.app -print -quit \
        | grep -q .; then
        echo "ERROR: $sdk did not produce PulpCoreMidiHarness.app" >&2
        exit 1
    fi
done

simulator_udid=$(xcrun simctl list devices available -j | python3 -c '
import json, re, sys
data = json.load(sys.stdin)
devices = []
for runtime, entries in data["devices"].items():
    match = re.search(r"\.iOS-(\d+)-(\d+)$", runtime)
    if not match or tuple(map(int, match.groups())) < (16, 3):
        continue
    devices.extend(d for d in entries
                   if d.get("isAvailable") and "iPhone" in d.get("name", ""))
booted = next((d for d in devices if d.get("state") == "Booted"), None)
chosen = booted or (devices[0] if devices else None)
if chosen:
    print(chosen["udid"])
')
if [[ -z "$simulator_udid" ]]; then
    echo "ERROR: no available iPhone Simulator" >&2
    exit 1
fi

booted_here=0
if [[ $(xcrun simctl list devices -j | python3 -c '
import json, sys
udid = sys.argv[1]
data = json.load(sys.stdin)
print(next((d.get("state", "") for ds in data["devices"].values()
            for d in ds if d.get("udid") == udid), ""))
' "$simulator_udid") != "Booted" ]]; then
    xcrun simctl boot "$simulator_udid"
    booted_here=1
fi
cleanup_simulator() {
    if [[ $booted_here -eq 1 ]]; then
        xcrun simctl shutdown "$simulator_udid" >/dev/null 2>&1 || true
    fi
}
trap cleanup_simulator EXIT
xcrun simctl bootstatus "$simulator_udid" -b

harness_app=$(find "$build_root/iphonesimulator" -type d \
    -name PulpCoreMidiHarness.app -print -quit)
bundle_id=dev.pulp.tests.coremidi
xcrun simctl uninstall "$simulator_udid" "$bundle_id" >/dev/null 2>&1 || true
xcrun simctl install "$simulator_udid" "$harness_app"
container=$(xcrun simctl get_app_container "$simulator_udid" "$bundle_id" data)
result_file="$container/Documents/coremidi-result.txt"
rm -f "$result_file"
xcrun simctl launch --terminate-running-process "$simulator_udid" "$bundle_id"

for _ in {1..150}; do
    [[ -f "$result_file" ]] && break
    sleep 0.1
done
if [[ ! -f "$result_file" ]]; then
    echo "ERROR: CoreMIDI Simulator harness produced no result" >&2
    exit 1
fi
if [[ $(head -n 1 "$result_file") != "PASS" ]]; then
    echo "ERROR: CoreMIDI Simulator harness failed" >&2
    cat "$result_file" >&2
    exit 1
fi
echo "OK: iOS Simulator CoreMIDI discovery and bidirectional UMP I/O passed"

choc_root=$(sed -n 's/^FETCHCONTENT_SOURCE_DIR_CHOC:PATH=//p' \
    "$build_root/iphonesimulator/CMakeCache.txt" | head -n 1)
bash "$root/test/cmake/test_ios_source_syntax.sh" \
    "$root" "$build_root/iphonesimulator" "$choc_root"

echo "OK: iOS timeline/playback/sequence compile gate passed for simulator and device"
