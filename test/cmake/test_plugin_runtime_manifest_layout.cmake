if(NOT DEFINED PULP_SOURCE_DIR)
    message(FATAL_ERROR "PULP_SOURCE_DIR is required")
endif()

set(_utils "${PULP_SOURCE_DIR}/tools/cmake/PulpUtils.cmake")
if(NOT EXISTS "${_utils}")
    message(FATAL_ERROR "PulpUtils.cmake not found: ${_utils}")
endif()

file(READ "${_utils}" _utils_content)

if(NOT _utils_content MATCHES "MATCHES \"_AUv3\\$\" OR PULP_IOS")
    message(FATAL_ERROR
        "_pulp_attach_plugin_runtime_manifest must route AUv3/iOS bundles "
        "through a flat resource-layout branch")
endif()

if(NOT _utils_content MATCHES "TARGET_BUNDLE_DIR:\\$\\{format_target\\}>/Resources/pulp\\.plugin-runtime\\.json")
    message(FATAL_ERROR
        "_pulp_attach_plugin_runtime_manifest must copy AUv3/iOS manifests "
        "to Resources/pulp.plugin-runtime.json")
endif()

if(NOT _utils_content MATCHES "TARGET_BUNDLE_DIR:\\$\\{format_target\\}>/Contents/Resources/pulp\\.plugin-runtime\\.json")
    message(FATAL_ERROR
        "_pulp_attach_plugin_runtime_manifest must keep the desktop macOS "
        "Contents/Resources manifest path")
endif()

message(STATUS "plugin_runtime_manifest_layout_verified=true")
