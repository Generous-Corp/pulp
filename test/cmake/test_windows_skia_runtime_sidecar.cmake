if(NOT DEFINED PULP_SOURCE_DIR)
    message(FATAL_ERROR "PULP_SOURCE_DIR is required")
endif()

set(_helper "${PULP_SOURCE_DIR}/tools/cmake/PulpWebGpuImportedTarget.cmake")
if(NOT EXISTS "${_helper}")
    message(FATAL_ERROR "PulpWebGpuImportedTarget.cmake not found: ${_helper}")
endif()

file(READ "${_helper}" _helper_content)

if(NOT _helper_content MATCHES "if\\(WIN32 AND SKIA_FOUND\\)")
    message(FATAL_ERROR
        "The installed-SDK runtime helper must gate the Windows Skia ICU "
        "sidecar contract on WIN32 and SKIA_FOUND")
endif()

if(NOT _helper_content MATCHES
        "NOT SKIA_ICUDTL_FILE OR NOT EXISTS \\\"\\$\\{SKIA_ICUDTL_FILE\\}\\\"")
    message(FATAL_ERROR
        "The Windows Skia runtime helper must fail when icudtl.dat is absent")
endif()

if(NOT _helper_content MATCHES
        "copy_if_different[^\n]*\n[ \t]*\\\"\\$\\{SKIA_ICUDTL_FILE\\}\\\"")
    message(FATAL_ERROR
        "The Windows Skia runtime helper must copy icudtl.dat beside consumers")
endif()

if(NOT _helper_content MATCHES
        "SkUnicodes::ICU::Make\\(\\) returns null")
    message(FATAL_ERROR
        "Keep the host-crash failure mode documented beside the packaging rule")
endif()

message(STATUS "windows_skia_runtime_sidecar_contract_verified=true")
