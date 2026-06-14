#!/usr/bin/env bash
# Configure-time regression test for the iOS HostApp/AUv3 bundle-id
# containment rule. A sibling extension id configures cleanly but fails later
# at `xcrun simctl install` with IXErrorDomain code=2, so the helper should
# reject it before any Xcode build or simulator install.
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

cat >"${work_dir}/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.24)
project(IosHostAppBundleGuard LANGUAGES C)

set(PULP_IOS TRUE)
include("${PULP_ROOT}/tools/cmake/PulpIosHostApp.cmake")

add_executable(BadSibling_AUv3 EXCLUDE_FROM_ALL main.c)
set_target_properties(BadSibling_AUv3 PROPERTIES
    PULP_AUV3_PLUGIN_NAME "BadSibling"
    PULP_AUV3_MANUFACTURER_NAME "Pulp"
    PULP_AUV3_MANUFACTURER_CODE "PULP"
    PULP_AUV3_SUBTYPE_CODE "BadS"
    PULP_AUV3_AU_TYPE "aumu"
    PULP_AUV3_VERSION_INT "65536"
    PULP_AUV3_BUNDLE_ID "com.example.sibling"
)

pulp_add_ios_host_app(BadSibling_HostApp
    AUV3_EXTENSION BadSibling_AUv3
    BUNDLE_ID      com.example.host
    SOURCES        "${CMAKE_CURRENT_LIST_DIR}/main.c"
)
EOF

log="${work_dir}/configure.log"
set +e
cmake -S "${work_dir}" -B "${work_dir}/build" \
    -DPULP_ROOT="${pulp_root}" >"${log}" 2>&1
status=$?
set -e

if [[ ${status} -eq 0 ]]; then
    echo "FAIL: invalid sibling AUv3 bundle id configured successfully" >&2
    cat "${log}" >&2
    exit 1
fi

if ! grep -q "must be nested under" "${log}"; then
    echo "FAIL: configure failed, but not with the HostApp bundle-id guard" >&2
    cat "${log}" >&2
    exit 1
fi

if ! grep -q "Mismatched bundle IDs" "${log}"; then
    echo "FAIL: guard message should point to the simulator install failure" >&2
    cat "${log}" >&2
    exit 1
fi

echo "OK: iOS HostApp rejects sibling AUv3 extension bundle ids"
