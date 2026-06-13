if(NOT DEFINED PULP_SOURCE_DIR)
    message(FATAL_ERROR "PULP_SOURCE_DIR is required")
endif()

set(_utils "${PULP_SOURCE_DIR}/tools/cmake/PulpUtils.cmake")
if(NOT EXISTS "${_utils}")
    message(FATAL_ERROR "PulpUtils.cmake not found: ${_utils}")
endif()

file(READ "${_utils}" _utils_content)

if(_utils_content MATCHES "MATCHES \"_AUv3\\$\" OR PULP_IOS")
    message(FATAL_ERROR
        "_pulp_attach_plugin_runtime_manifest must not route macOS AUv3 "
        ".appex bundles to the iOS flat resource layout based on target name")
endif()

if(NOT _utils_content MATCHES "elseif\\(APPLE AND PULP_IOS\\)")
    message(FATAL_ERROR
        "_pulp_attach_plugin_runtime_manifest must route only iOS bundles "
        "through the flat resource-layout branch")
endif()

if(NOT _utils_content MATCHES "TARGET_BUNDLE_DIR:\\$\\{format_target\\}>/Resources/pulp\\.plugin-runtime\\.json")
    message(FATAL_ERROR
        "_pulp_attach_plugin_runtime_manifest must copy iOS manifests "
        "to Resources/pulp.plugin-runtime.json")
endif()

if(NOT _utils_content MATCHES "TARGET_BUNDLE_DIR:\\$\\{format_target\\}>/Contents/Resources/pulp\\.plugin-runtime\\.json")
    message(FATAL_ERROR
        "_pulp_attach_plugin_runtime_manifest must keep the desktop macOS "
        "Contents/Resources manifest path")
endif()

message(STATUS "plugin_runtime_manifest_layout_verified=true")
