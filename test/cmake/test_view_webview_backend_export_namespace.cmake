cmake_minimum_required(VERSION 3.24)

# The generated export set is what `cmake --install` copies to
# lib/cmake/Pulp/PulpTargets.cmake. For a WebView-enabled build it must
# reference the whole-archive backend as `Pulp::view-webview-backend` and never
# as the in-tree alias `pulp::view-webview-backend`, which a consumer's package
# does not define.
if(NOT DEFINED PULP_BUILD_DIR)
    message(FATAL_ERROR "PULP_BUILD_DIR is required")
endif()

file(GLOB _export_files LIST_DIRECTORIES false
    "${PULP_BUILD_DIR}/CMakeFiles/Export/*/PulpTargets.cmake")
if(NOT _export_files)
    message(FATAL_ERROR "No generated PulpTargets.cmake under ${PULP_BUILD_DIR}/CMakeFiles/Export")
endif()

set(_text "")
foreach(_f IN LISTS _export_files)
    file(READ "${_f}" _t)
    string(APPEND _text "${_t}")
endforeach()

if(NOT _text MATCHES "Pulp::view-webview-backend")
    message(FATAL_ERROR "Export set does not reference Pulp::view-webview-backend")
endif()
if(_text MATCHES "pulp::view-webview-backend")
    message(FATAL_ERROR
        "Export set references the in-tree alias pulp::view-webview-backend")
endif()
message(STATUS "view-webview-backend export namespace OK")
