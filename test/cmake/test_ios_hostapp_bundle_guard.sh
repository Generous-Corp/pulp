#!/usr/bin/env bash
# Configure-time regression test for the iOS HostApp/AUv3 bundle-id
# containment rule enforced by pulp_add_ios_host_app (PulpIosHostApp.cmake).
#
# Apple requires an app-extension bundle id to be the containing app's
# bundle id plus at least one extra dot-component (host.suffix). A sibling
# id, or an id equal to the host's, configures and builds cleanly but then
# fails at `xcrun simctl install` with IXErrorDomain code=2 / "Mismatched
# bundle IDs". The helper rejects both at configure time; this test proves
# it without a full iOS build (configure-only, so it runs on any host).
set -euo pipefail

pulp_root="${1:-}"
if [[ -z "${pulp_root}" ]]; then
    echo "usage: $0 <pulp-source-dir>" >&2
    exit 2
fi

work_dir="$(mktemp -d -t pulp-ios-hostapp-bundle-guard-XXXXXX)"
trap 'rm -rf "${work_dir}"' EXIT

cat >"${work_dir}/main.c" <<'EOF'
int main(void) { return 0; }
EOF

# Configure a minimal iOS HostApp whose embedded AUv3 carries $1 as its
# bundle id, against a host id of com.example.host. Asserts configure fails
# with the containment guard. $2 is a human label for diagnostics.
assert_rejected() {
    local au_bundle_id="$1"
    local label="$2"
    local case_dir="${work_dir}/${label}"
    mkdir -p "${case_dir}"

    cat >"${case_dir}/CMakeLists.txt" <<EOF
cmake_minimum_required(VERSION 3.24)
project(IosHostAppBundleGuard LANGUAGES C)

set(PULP_IOS TRUE)
include("\${PULP_ROOT}/tools/cmake/PulpIosHostApp.cmake")

add_executable(Bad_AUv3 EXCLUDE_FROM_ALL "${work_dir}/main.c")
set_target_properties(Bad_AUv3 PROPERTIES
    PULP_AUV3_PLUGIN_NAME "Bad"
    PULP_AUV3_MANUFACTURER_NAME "Pulp"
    PULP_AUV3_MANUFACTURER_CODE "PULP"
    PULP_AUV3_SUBTYPE_CODE "BadS"
    PULP_AUV3_AU_TYPE "aumu"
    PULP_AUV3_VERSION_INT "65536"
    PULP_AUV3_BUNDLE_ID "${au_bundle_id}"
)

pulp_add_ios_host_app(Bad_HostApp
    AUV3_EXTENSION Bad_AUv3
    BUNDLE_ID      com.example.host
    SOURCES        "${work_dir}/main.c"
)
EOF

    local log="${case_dir}/configure.log"
    set +e
    cmake -S "${case_dir}" -B "${case_dir}/build" \
        -DPULP_ROOT="${pulp_root}" >"${log}" 2>&1
    local status=$?
    set -e

    if [[ ${status} -eq 0 ]]; then
        echo "FAIL[${label}]: invalid AUv3 bundle id '${au_bundle_id}' configured successfully" >&2
        cat "${log}" >&2
        exit 1
    fi
    if ! grep -q "must be nested under" "${log}"; then
        echo "FAIL[${label}]: configure failed, but not with the bundle-id containment guard" >&2
        cat "${log}" >&2
        exit 1
    fi
    if ! grep -q "Mismatched bundle IDs" "${log}"; then
        echo "FAIL[${label}]: guard message should point to the simulator install failure" >&2
        cat "${log}" >&2
        exit 1
    fi
    echo "OK[${label}]: rejected AUv3 bundle id '${au_bundle_id}'"
}

# Sibling id: shares a parent but is not nested under the host id.
assert_rejected "com.example.sibling" "sibling"
# Equal id: two bundles cannot share one identifier; must be a strict child.
assert_rejected "com.example.host" "equal"

echo "OK: iOS HostApp rejects non-nested AUv3 extension bundle ids"
