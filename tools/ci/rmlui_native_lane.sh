#!/usr/bin/env bash
# The RmlUi native adapter lane: build Vellum's rmlui/ project at the commit
# tools/deps/manifest.json pins, then build and run pulp-test-rmlui-native-surface
# against it. Used by .github/workflows/rmlui-native.yml; runnable by hand on a
# Mac with a Metal device:
#
#   tools/ci/rmlui_native_lane.sh pin                      # print the pinned commit
#   tools/ci/rmlui_native_lane.sh sdk <vellum-dir> <prefix>  # build + install the SDK
#   tools/ci/rmlui_native_lane.sh test <prefix> [build-dir]  # configure, build, run
#
# The lane fails, rather than passes empty, when the test program is absent,
# when any case fails, or when every case skipped (no Metal device).
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

sdk() {
    local vellum_dir="$1" prefix="$2"
    local build_dir="$vellum_dir/.rmlui-build"
    # Native, Metal and fonts (the adapter's tests load a face), packaged as the
    # separate VellumRmlUiNative package the adapter finds.
    cmake -S "$vellum_dir/rmlui" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DVELLUM_RMLUI_NATIVE=ON -DVELLUM_RMLUI_NATIVE_PACKAGE=ON -DVELLUM_RMLUI_METAL=ON \
        -DVELLUM_RMLUI_FONTS=ON -DVELLUM_RMLUI_BUILD_TESTS=OFF \
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
    # Catch2's exit code is not enough: an all-skipped run exits 0.
    "$program" -r xml -o "$report" || true
    python3 - "$report" <<'PY'
import sys
import xml.etree.ElementTree as ET

root = ET.parse(sys.argv[1]).getroot()
totals = root.find("OverallResultsCases")
ok, failed, skipped = (int(totals.get(k, 0)) for k in ("successes", "failures", "skips"))
print(f"rmlui-native surface suite: {ok} passed, {failed} failed, {skipped} skipped")
if failed:
    sys.exit("::error::the rmlui-native surface suite has failing cases")
if ok == 0:
    sys.exit("::error::no case passed; with every case skipped the lane proved nothing "
             "(no Metal device on this runner?)")
PY
}

case "${1:-}" in
pin) pin ;;
sdk) shift; sdk "$@" ;;
test) shift; run_test "$@" ;;
*) sed -n '2,12p' "${BASH_SOURCE[0]}" >&2; exit 2 ;;
esac
