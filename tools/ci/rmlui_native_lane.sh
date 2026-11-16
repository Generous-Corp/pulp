#!/usr/bin/env bash
# The RmlUi native adapter lane: build Vellum's rmlui/ project at the commit
# tools/deps/manifest.json pins, then build and run pulp-test-rmlui-native-surface
# against it. Used by .github/workflows/rmlui-native.yml; runnable by hand on a
# Mac with a Metal device:
#
#   tools/ci/rmlui_native_lane.sh pin                      # print the pinned commit
#   tools/ci/rmlui_native_lane.sh sdk-key                  # cache key: pin + SDK build options
#   tools/ci/rmlui_native_lane.sh sdk <vellum-dir> <prefix>  # build + install the SDK
#   tools/ci/rmlui_native_lane.sh test <prefix> [build-dir]  # configure, build, run
#
# The lane fails, rather than passes empty, when the test program is absent,
# when any case fails, or when any case skipped. PULP_REQUIRE_METAL turns a
# missing Metal device into a failure of the cases that need one, so a runner
# without Metal cannot go green on the few cases that do not.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
manifest="$repo_root/tools/deps/manifest.json"
entry="vellum-rmlui-sdk"

pin() {
    python3 - "$manifest" "$entry" <<'PY'
import json, sys
manifest, name = sys.argv[1:3]
for dep in json.load(open(manifest))["dependencies"]:
    if dep["name"] == name:
        print(dep["upstream"]["commit"])
        break
else:
    sys.exit(f"{name} is not in {manifest}")
PY
}

# Native, Metal and fonts (the adapter's tests load a face), packaged as the
# separate VellumRmlUiNative package the adapter finds.
sdk_options=(-G Ninja -DCMAKE_BUILD_TYPE=Release
    -DVELLUM_RMLUI_NATIVE=ON -DVELLUM_RMLUI_NATIVE_PACKAGE=ON -DVELLUM_RMLUI_METAL=ON
    -DVELLUM_RMLUI_FONTS=ON -DVELLUM_RMLUI_BUILD_TESTS=OFF)

# The cache key covers what the SDK build depends on, not this whole script.
sdk_key() {
    { pin; printf '%s\n' "${sdk_options[@]}"; } | shasum -a 256 | cut -c1-16
}

sdk() {
    local vellum_dir="$1" prefix="$2"
    local build_dir="$vellum_dir/.rmlui-build"
    cmake -S "$vellum_dir/rmlui" -B "$build_dir" "${sdk_options[@]}" \
        -DCMAKE_INSTALL_PREFIX="$prefix"
    "$repo_root/tools/ci/governed-build.sh" cmake --build "$build_dir"
    cmake --install "$build_dir"
    test -f "$prefix/lib/cmake/VellumRmlUiNative/VellumRmlUiNativeConfig.cmake" || {
        echo "::error::the installed Vellum RmlUi SDK has no VellumRmlUiNative package" >&2
        exit 1
    }
}

run_test() {
    local prefix="$1" build_dir="${2:-$repo_root/build-rmlui}"
    cmake -S "$repo_root" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DPULP_BUILD_EXAMPLES=OFF -DPULP_ENABLE_GPU=OFF -DPULP_ENABLE_PULP_NATIVE=ON \
        -DPULP_ENABLE_RMLUI_ADAPTER=ON -DCMAKE_PREFIX_PATH="$prefix"
    "$repo_root/tools/ci/governed-build.sh" cmake --build "$build_dir" \
        --target pulp-test-rmlui-native-surface
    local program="$build_dir/integrations/rmlui-native/tests/pulp-test-rmlui-native-surface"
    if [ ! -x "$program" ]; then
        echo "::error::pulp-test-rmlui-native-surface was not built; the adapter did not configure" >&2
        exit 1
    fi
    local report="$build_dir/rmlui-native-surface.xml"
    # Catch2's exit code is not enough: skipped cases exit 0. The timing cases
    # ([performance]) stay out, as they do from the required ctest lane.
    PULP_REQUIRE_METAL=1 "$program" "~[performance]" -r xml -o "$report" || true
    python3 - "$report" <<'PY'
import sys
import xml.etree.ElementTree as ET

root = ET.parse(sys.argv[1]).getroot()
totals = root.find("OverallResultsCases")
ok, failed, skipped = (int(totals.get(k, 0)) for k in ("successes", "failures", "skips"))
print(f"rmlui-native surface suite: {ok} passed, {failed} failed, {skipped} skipped")
if failed:
    sys.exit("::error::the rmlui-native surface suite has failing cases")
if skipped:
    sys.exit("::error::cases skipped; the lane proves nothing for them (no Metal device on "
             "this runner?)")
if ok == 0:
    sys.exit("::error::no case passed")
PY
}

# The lane must go red when Metal is missing: run the suite with the device
# check forced false and require a failure. Passes (exit 0) when the lane is red.
negative_control() {
    local build_dir="${1:-$repo_root/build-rmlui}"
    local program="$build_dir/integrations/rmlui-native/tests/pulp-test-rmlui-native-surface"
    if PULP_FORCE_NO_METAL=1 PULP_REQUIRE_METAL=1 "$program" "~[performance]" >/dev/null 2>&1; then
        echo "::error::the suite passed with the Metal check forced false; the lane would go green without a device" >&2
        exit 1
    fi
    echo "negative control: the suite fails without a Metal device, as the lane requires"
}

case "${1:-}" in
pin) pin ;;
sdk-key) sdk_key ;;
negative-control) shift; negative_control "$@" ;;
sdk) shift; sdk "$@" ;;
test) shift; run_test "$@" ;;
*) sed -n '2,14p' "${BASH_SOURCE[0]}" >&2; exit 2 ;;
esac
