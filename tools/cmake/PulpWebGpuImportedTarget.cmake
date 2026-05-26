# PulpWebGpuImportedTarget.cmake — recreate the `webgpu` IMPORTED target
# for downstream find_package(Pulp).
#
# Extracted from PulpConfig.cmake.in in the 2026-05 B3 refactor.
#
# wgpu-native ships as a runtime shared library (dylib/so/dll) plus —
# on Windows — an import library for the linker. Recreate the IMPORTED
# target if it doesn't already exist and the SDK install carries both
# the runtime and headers. Multi-config Windows builds need an import
# library; this module looks for the modern v0.19+ name first and
# falls back to legacy names.
#
# Also exposes `target_copy_webgpu_binaries(<target>)` so consumer
# plugins/apps can copy the runtime DLL/dylib next to their binary
# at build time.

set(_pulp_webgpu_runtime
    "${PULP_SDK_LIBRARY_DIR}/${PULP_SDK_SHARED_LIBRARY_PREFIX}wgpu_native${PULP_SDK_SHARED_LIBRARY_SUFFIX}")
set(_pulp_webgpu_include_dir "${PULP_SDK_DIR}/external/webgpu/include")

if(NOT TARGET webgpu
   AND EXISTS "${_pulp_webgpu_runtime}"
   AND EXISTS "${_pulp_webgpu_include_dir}")

    # On Windows, MSVC needs an import library in addition to the
    # runtime DLL. Modern wgpu-native builds ship the import library
    # as `wgpu_native.dll.lib` (the MSVC IMPLIB convention since
    # wgpu-native v0.19+); older builds shipped `wgpu_native.lib`.
    # Look for either name. Fail fast with a clear message if neither
    # is present so a broken SDK install (#94) gives a useful error
    # instead of a confusing "IMPORTED_IMPLIB not set" later.
    if(WIN32)
        set(_pulp_webgpu_implib_candidates
            "${PULP_SDK_LIBRARY_DIR}/wgpu_native.dll.lib"
            "${PULP_SDK_LIBRARY_DIR}/wgpu_native.lib"
            "${PULP_SDK_LIBRARY_DIR}/${PULP_SDK_SHARED_LIBRARY_PREFIX}wgpu_native.lib")
        set(_pulp_webgpu_implib "")
        foreach(_pulp_candidate IN LISTS _pulp_webgpu_implib_candidates)
            if(EXISTS "${_pulp_candidate}")
                set(_pulp_webgpu_implib "${_pulp_candidate}")
                break()
            endif()
        endforeach()
        unset(_pulp_candidate)
        if(NOT _pulp_webgpu_implib)
            message(FATAL_ERROR
                "Pulp SDK is missing the wgpu_native import library. "
                "Looked for: ${_pulp_webgpu_implib_candidates}. "
                "This is a broken SDK install — the runtime DLL is present "
                "but the import library needed for linking is not. "
                "Reinstall the Pulp SDK (v0.2.2 or newer) to pick up the fix "
                "from https://github.com/danielraffel/pulp/issues/94.")
        endif()
    endif()

    add_library(webgpu SHARED IMPORTED)
    set_target_properties(webgpu PROPERTIES
        IMPORTED_LOCATION "${_pulp_webgpu_runtime}"
        IMPORTED_LOCATION_RELEASE "${_pulp_webgpu_runtime}"
        INTERFACE_COMPILE_DEFINITIONS "WEBGPU_BACKEND_WGPU"
        INTERFACE_INCLUDE_DIRECTORIES
            "${_pulp_webgpu_include_dir};${_pulp_webgpu_include_dir}/webgpu"
    )

    if(WIN32)
        set_property(TARGET webgpu PROPERTY IMPORTED_IMPLIB "${_pulp_webgpu_implib}")
        set_property(TARGET webgpu PROPERTY
            IMPORTED_IMPLIB_RELEASE "${_pulp_webgpu_implib}")
    elseif(UNIX AND NOT APPLE)
        set_property(TARGET webgpu PROPERTY IMPORTED_NO_SONAME TRUE)
    endif()
endif()

if(NOT COMMAND target_copy_webgpu_binaries)
    function(_pulp_target_append_unique_property target property value)
        get_target_property(_pulp_existing_values ${target} ${property})
        if(NOT _pulp_existing_values OR _pulp_existing_values STREQUAL "_pulp_existing_values-NOTFOUND")
            set(_pulp_existing_values "")
        endif()

        list(FIND _pulp_existing_values "${value}" _pulp_value_index)
        if(_pulp_value_index EQUAL -1)
            set_property(TARGET ${target} APPEND PROPERTY ${property} "${value}")
        endif()
    endfunction()

    function(target_copy_webgpu_binaries target)
        if(TARGET webgpu AND EXISTS "${_pulp_webgpu_runtime}")
            add_custom_command(
                TARGET ${target}
                POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_pulp_webgpu_runtime}"
                    $<TARGET_FILE_DIR:${target}>
                COMMENT "Copying WebGPU runtime next to ${target}"
            )

            if(APPLE)
                # wgpu-native's macOS dylib uses @rpath. Since this helper
                # copies the runtime next to the consuming binary/framework,
                # the binary should resolve it from its bundle, not from the
                # SDK install prefix used at build time. This keeps signed
                # Developer ID bundles compatible with library validation.
                set_target_properties(${target} PROPERTIES
                    BUILD_WITH_INSTALL_RPATH TRUE
                    MACOSX_RPATH TRUE
                )
                _pulp_target_append_unique_property(${target} INSTALL_RPATH "@loader_path")
            endif()
        endif()
    endfunction()
endif()
