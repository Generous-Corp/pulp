# Release-artifact scanner: the one forbidden-marker list
# (tools/cmake/release_artifact_forbidden.json) and the scanner over it
# (tools/cmake/PulpReleaseArtifactScan.cmake). Three things are registered:
#   * the list self-test (shape, uniqueness, every gate documented and used);
#   * the negative control: a fixture generated from the list carries every
#     symbol and string marker and must be flagged for each, a clean fixture
#     must pass, so the scanner cannot pass vacuously;
#   * class scans: every artifact class this configure builds (SDK libraries,
#     the C++ CLI, a plug-in bundle and a standalone built through
#     pulp_add_plugin) is scanned against the gates that hold for the
#     configure, so all of them answer to the same list.
# The per-gate wrappers (AssertNoJsSymbols, AssertNoTracingSymbols,
# reload_ship_symbol_check, check_control_shipping_artifact) keep their own
# registrations; they read this list too.
if(NOT UNIX)
    return()
endif()

include("${CMAKE_SOURCE_DIR}/tools/cmake/PulpReleaseArtifactScan.cmake")
pulp_release_artifact_load()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${PULP_RELEASE_ARTIFACT_FORBIDDEN_LIST}")

add_test(NAME release-artifact-forbidden-list-selftest
    COMMAND ${CMAKE_COMMAND}
        -DPULP_SOURCE_DIR=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/release_artifact_list_selftest.cmake)
set_tests_properties(release-artifact-forbidden-list-selftest PROPERTIES
    LABELS "cmake;control;ship" TIMEOUT 60)

set(_pulp_pra_fixture_src "${CMAKE_CURRENT_BINARY_DIR}/release_artifact_forbidden_fixture.cpp")
pulp_release_artifact_write_fixture("${_pulp_pra_fixture_src}")
add_executable(pulp-release-artifact-forbidden-fixture "${_pulp_pra_fixture_src}")
add_executable(pulp-release-artifact-clean-fixture fixtures/release_artifact_clean_fixture.cpp)
add_custom_target(pulp-release-artifact-fixtures)
add_dependencies(pulp-release-artifact-fixtures
    pulp-release-artifact-forbidden-fixture pulp-release-artifact-clean-fixture)

add_test(NAME release-artifact-scan-negative-control
    COMMAND ${CMAKE_COMMAND}
        -DPULP_SOURCE_DIR=${CMAKE_SOURCE_DIR}
        -DFORBIDDEN_BIN=$<TARGET_FILE:pulp-release-artifact-forbidden-fixture>
        -DCLEAN_BIN=$<TARGET_FILE:pulp-release-artifact-clean-fixture>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/release_artifact_negative_control.cmake)
set_tests_properties(release-artifact-scan-negative-control PROPERTIES
    LABELS "cmake;control;ship" TIMEOUT 120)

# Class scans. The gates come from the configure (PULP_TRACING, PULP_ENABLE_JS,
# and the dev-profile / bridge-exec options where the tree defines them);
# a configure in which no gate holds registers nothing rather than a vacuous
# pass. `control-stripped` is per-artifact and stays with the control scanner.
pulp_release_artifact_active_gates(_pulp_pra_gates)
function(_pulp_release_artifact_class_scan class target)
    if(NOT TARGET ${target})
        return()
    endif()
    get_target_property(_type ${target} TYPE)
    if(_type STREQUAL "INTERFACE_LIBRARY" OR _type STREQUAL "UTILITY")
        return()
    endif()
    string(REPLACE ";" "$<SEMICOLON>" _gates "${_pulp_pra_gates}")
    add_test(NAME release-artifact-scan-${class}-${target}
        COMMAND ${CMAKE_COMMAND}
            -DBIN=$<TARGET_FILE:${target}>
            "-DGATES=${_gates}"
            "-DLABEL=release-artifact scan [${class}]"
            -P ${CMAKE_SOURCE_DIR}/tools/cmake/PulpReleaseArtifactScan.cmake)
    set_tests_properties(release-artifact-scan-${class}-${target} PROPERTIES
        LABELS "cmake;control;ship" TIMEOUT 120
        REQUIRED_FILES $<TARGET_FILE:${target}>)
endfunction()
if(_pulp_pra_gates)
    message(STATUS "release-artifact class scans: gates ${_pulp_pra_gates}")
    # The CLI (tools/cli), the fixture plug-in (test/fixtures/ui_script_plugin,
    # built through pulp_add_plugin so it is a real bundle and standalone) and
    # PULP_SDK_TARGETS (tools/cmake/PulpInstallRules.cmake) are all defined
    # after this directory, so every class scan registers once the root
    # directory has finished. The SDK scan is one test over every installed
    # library, the first hit fatal and named.
    # cmake_language(DEFER) expands its arguments when the deferred call runs,
    # in the root scope where this file's variables are gone, so the gates
    # travel through a global property.
    set_property(GLOBAL PROPERTY PULP_RELEASE_ARTIFACT_CLASS_SCAN_GATES "${_pulp_pra_gates}")
    function(_pulp_release_artifact_register_class_scans)
        get_property(_pulp_pra_gates GLOBAL PROPERTY PULP_RELEASE_ARTIFACT_CLASS_SCAN_GATES)
        if(NOT _pulp_pra_gates)
            message(FATAL_ERROR "release-artifact scan: class-scan gates were lost before registration")
        endif()
        _pulp_release_artifact_class_scan(cli pulp-cli)
        _pulp_release_artifact_class_scan(plugin-bundle PulpUiScriptFixture_CLAP)
        _pulp_release_artifact_class_scan(standalone PulpUiScriptFixture_Standalone)
        if(NOT DEFINED PULP_SDK_TARGETS)
            message(FATAL_ERROR
                "release-artifact scan: PULP_SDK_TARGETS is undefined at the end of the root directory")
        endif()
        set(_files "")
        foreach(_target IN LISTS PULP_SDK_TARGETS)
            if(NOT TARGET ${_target})
                continue()
            endif()
            get_target_property(_type ${_target} TYPE)
            if(_type STREQUAL "INTERFACE_LIBRARY" OR _type STREQUAL "UTILITY")
                continue()
            endif()
            list(APPEND _files "$<TARGET_FILE:${_target}>")
        endforeach()
        if(NOT _files)
            message(FATAL_ERROR "release-artifact scan: PULP_SDK_TARGETS names no library")
        endif()
        string(REPLACE ";" "$<SEMICOLON>" _joined "${_files}")
        string(REPLACE ";" "$<SEMICOLON>" _gates "${_pulp_pra_gates}")
        add_test(NAME release-artifact-scan-sdk-libraries
            COMMAND ${CMAKE_COMMAND}
                "-DBINS=${_joined}"
                "-DGATES=${_gates}"
                "-DLABEL=release-artifact scan [sdk-library]"
                -P ${CMAKE_SOURCE_DIR}/tools/cmake/PulpReleaseArtifactScan.cmake)
        # Scanning every SDK library runs close to 300 s under ASan, so the
        # budget is scaled for instrumented trees; plain lanes keep 300.
        pulp_scaled_test_timeout(_pulp_pra_sdk_timeout 300)
        set_tests_properties(release-artifact-scan-sdk-libraries PROPERTIES
            LABELS "cmake;control;ship" TIMEOUT "${_pulp_pra_sdk_timeout}")
    endfunction()
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
        CALL _pulp_release_artifact_register_class_scans)
else()
    message(STATUS "release-artifact class scans: no gate holds for this configure; none registered")
endif()
