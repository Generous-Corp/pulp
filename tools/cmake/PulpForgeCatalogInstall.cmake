# The committed catalog describes the default build. An opt-in realization
# changes the runtime projection, so its SDK must carry that build's export.
include_guard(GLOBAL)

function(pulp_generated_forge_catalog_path output)
    set(${output} "${CMAKE_BINARY_DIR}/forge-catalog/$<CONFIG>/forge-catalog.json" PARENT_SCOPE)
endfunction()

function(pulp_add_generated_forge_catalog_check)
    pulp_generated_forge_catalog_path(_catalog)
    add_test(NAME cli-forge-catalog-check
        COMMAND "${CMAKE_COMMAND}"
            "-DEXPORTER=$<TARGET_FILE:pulp-cli>"
            "-DEXPECTED=${_catalog}"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/PulpCheckForgeCatalog.cmake")
    set_tests_properties(cli-forge-catalog-check PROPERTIES
        REQUIRED_FILES "${_catalog}"
        TIMEOUT 90)
endfunction()

function(pulp_install_forge_catalog)
    # The caller is Pulp's root directory, including in add_subdirectory builds.
    set(_catalog "${CMAKE_CURRENT_SOURCE_DIR}/docs/status/forge-catalog.json")
    if(PULP_HOST_ENABLE_GPU_CONVOLUTION)
        if(CMAKE_CROSSCOMPILING OR NOT TARGET pulp-cli)
            message(FATAL_ERROR
                "GPU convolution SDK catalog requires a native pulp-cli exporter")
        endif()
        pulp_generated_forge_catalog_path(_catalog)
        set(_writer "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/PulpGenerateForgeCatalog.cmake")
        add_custom_command(OUTPUT "${_catalog}"
            COMMAND "${CMAKE_COMMAND}"
                "-DEXPORTER=$<TARGET_FILE:pulp-cli>"
                "-DOUTPUT=${_catalog}"
                -P "${_writer}"
            DEPENDS pulp-cli "${_writer}"
            COMMENT "Exporting this SDK's enabled Forge catalog"
            VERBATIM)
        add_custom_target(pulp-forge-sdk-catalog ALL DEPENDS "${_catalog}")
    elseif(NOT EXISTS "${_catalog}")
        message(FATAL_ERROR "Required Forge catalog snapshot is missing: ${_catalog}")
    endif()
    install(FILES "${_catalog}" DESTINATION "share/pulp")
endfunction()
