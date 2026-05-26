#[[
Install-layout regression: macOS AUv3 framework helper sources.

PulpAuv3.cmake's _pulp_add_auv3_macos_framework() compiles
${_PULP_FORMAT_SOURCE_DIR}/au_view_controller_mac.mm — the
PulpAUMacViewController principal class that NSExtensionMain looks up
when the macOS AUv3 .appex is loaded. When the Pulp SDK is consumed via
find_package(Pulp), _PULP_FORMAT_SOURCE_DIR resolves to
<prefix>/src/pulp/format/ (see tools/cmake/PulpUtils.cmake). If the
install rule omits au_view_controller_mac.mm, downstream
pulp_add_plugin(FORMATS AUv3 ...) builds on macOS fail at the
add_library() call with a missing-source error.

This regression is the SDK packaging fix called out in
planning/2026-05-24-linux-macos-chainer-gap-closure-plan.md
(Phase 3 / Phase 0 "upstream the SDK packaging fix that installs
au_view_controller_mac.mm").

Inputs (passed via -D):
  PULP_BUILD_DIR — path to a configured Pulp build directory.
]]

cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED PULP_BUILD_DIR)
    message(FATAL_ERROR
        "test_pulp_install_format_sources.cmake requires -DPULP_BUILD_DIR=<dir> "
        "pointing at a configured Pulp build.")
endif()
get_filename_component(PULP_BUILD_DIR "${PULP_BUILD_DIR}" ABSOLUTE)
if(NOT EXISTS "${PULP_BUILD_DIR}/CMakeCache.txt")
    message(FATAL_ERROR
        "PULP_BUILD_DIR=${PULP_BUILD_DIR} is not a configured CMake build "
        "directory (no CMakeCache.txt).")
endif()

set(_prefix "${CMAKE_CURRENT_BINARY_DIR}/pulp-install-format-sources-prefix")
file(REMOVE_RECURSE "${_prefix}")
file(MAKE_DIRECTORY "${_prefix}")

execute_process(
    COMMAND "${CMAKE_COMMAND}"
            --install "${PULP_BUILD_DIR}"
            --prefix  "${_prefix}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE  _stderr)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR
        "cmake --install failed (rc=${_rc}):\n"
        "----- stdout -----\n${_stdout}\n"
        "----- stderr -----\n${_stderr}\n----- end -----")
endif()

# Helper sources are published under <prefix>/src/pulp/format/ by
# tools/cmake/PulpInstallRules.cmake. The set must include every source
# referenced by PulpUtils / PulpAuv3 / PulpPluginFormats from
# _PULP_FORMAT_SOURCE_DIR so an installed-SDK consumer can build any
# format the SDK ships.
set(_required_sources
    aax_runtime.cpp
    au_adapter.mm
    au_audio_unit.h
    au_entry.mm
    au_v2_cocoa_view.mm
    au_view_controller_ios.mm
    au_view_controller_mac.mm
    vst3_plug_view.cpp)

set(_missing "")
foreach(_src IN LISTS _required_sources)
    set(_path "${_prefix}/src/pulp/format/${_src}")
    if(NOT EXISTS "${_path}")
        list(APPEND _missing "${_src}")
    endif()
endforeach()

if(_missing)
    message(FATAL_ERROR
        "Installed Pulp SDK is missing format helper sources required by "
        "PulpAuv3.cmake / PulpPluginFormats.cmake from _PULP_FORMAT_SOURCE_DIR:\n"
        "  ${_missing}\n"
        "Expected under: ${_prefix}/src/pulp/format/\n"
        "Without these, pulp_add_plugin(FORMATS AUv3 ...) on macOS (and other "
        "format helpers) fail in installed-SDK builds. Update the install(FILES "
        "...) block in tools/cmake/PulpInstallRules.cmake.")
endif()

message(STATUS
    "Install layout: ${_prefix}/src/pulp/format/ contains all required "
    "format helper sources (${_required_sources}).")
