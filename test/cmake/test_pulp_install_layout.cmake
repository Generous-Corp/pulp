#[[
Install-layout regression for installed SDK CMake helper scripts
(issue-905 follow-up).

`pulp_add_binary_data` resolves its Python encoder via
${CMAKE_CURRENT_FUNCTION_LIST_DIR}/scripts/encode_binary_data.py — adjacent
to whichever `PulpUtils.cmake` is sourced. That convention only works in
the installed tree if the encoder is bundled alongside `PulpUtils.cmake`
under `lib/cmake/Pulp/scripts/`. If the install rule omits the encoder,
downstream `find_package(Pulp)` consumers see asset targets fail at
build time with a missing-script error.

This test runs `cmake --install` against a pre-configured Pulp build into a
temporary prefix, then asserts the bundled SDK contains `PulpUtils.cmake`,
helper modules it includes, and its sibling `scripts/encode_binary_data.py`.

Inputs (passed via -D):
  PULP_BUILD_DIR — path to a configured Pulp build directory.
]]

cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED PULP_BUILD_DIR)
    message(FATAL_ERROR
        "test_pulp_install_layout.cmake requires -DPULP_BUILD_DIR=<dir> "
        "pointing at a configured Pulp build.")
endif()
get_filename_component(PULP_BUILD_DIR "${PULP_BUILD_DIR}" ABSOLUTE)
if(NOT EXISTS "${PULP_BUILD_DIR}/CMakeCache.txt")
    message(FATAL_ERROR
        "PULP_BUILD_DIR=${PULP_BUILD_DIR} is not a configured CMake build "
        "directory (no CMakeCache.txt).")
endif()

set(_prefix "${CMAKE_CURRENT_BINARY_DIR}/pulp-install-layout-test-prefix")
file(REMOVE_RECURSE "${_prefix}")
file(MAKE_DIRECTORY "${_prefix}")

# `cmake --install` re-emits the install rules from the configured build.
# It does not re-invoke the build — only published outputs that were
# already produced by the previous `cmake --build` step are copied. The
# CMake-package files (PulpConfig.cmake, PulpUtils.cmake, …) are
# generated at configure time, so they are always present.
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

# Locate the installed PulpUtils.cmake — Pulp installs into
# ${CMAKE_INSTALL_LIBDIR}/cmake/Pulp, which is libdir/cmake/Pulp on
# every platform. libdir varies (lib vs lib64); glob both.
file(GLOB _pulp_utils LIST_DIRECTORIES false
    "${_prefix}/lib*/cmake/Pulp/PulpUtils.cmake")
if(NOT _pulp_utils)
    message(FATAL_ERROR
        "Installed PulpUtils.cmake not found under ${_prefix}/lib*/cmake/Pulp/.")
endif()
list(GET _pulp_utils 0 _pulp_utils)
get_filename_component(_pulp_cmake_dir "${_pulp_utils}" DIRECTORY)

set(_installed_metadata "${_pulp_cmake_dir}/PulpPluginMetadata.cmake")
if(NOT EXISTS "${_installed_metadata}")
    message(FATAL_ERROR
        "PulpPluginMetadata.cmake not bundled with the installed Pulp SDK.\n"
        "Expected: ${_installed_metadata}\n"
        "PulpUtils.cmake includes this helper, so find_package(Pulp) consumers "
        "would fail before they can call pulp_add_plugin().")
endif()
file(READ "${_pulp_utils}" _installed_utils_text)
if(NOT _installed_utils_text MATCHES "PulpPluginMetadata\\.cmake")
    message(FATAL_ERROR
        "Installed PulpUtils.cmake no longer includes PulpPluginMetadata.cmake.")
endif()

# The encoder MUST live next to PulpUtils.cmake — this is the path
# pulp_add_binary_data computes via CMAKE_CURRENT_FUNCTION_LIST_DIR.
set(_installed_encoder "${_pulp_cmake_dir}/scripts/encode_binary_data.py")
if(NOT EXISTS "${_installed_encoder}")
    message(FATAL_ERROR
        "Encoder script not bundled with the installed Pulp SDK.\n"
        "Expected: ${_installed_encoder}\n"
        "find_package(Pulp) consumers calling pulp_add_binary_data would "
        "fail at build time with a missing-script error. The install rule "
        "in CMakeLists.txt must publish "
        "tools/cmake/scripts/encode_binary_data.py under "
        "lib/cmake/Pulp/scripts/.")
endif()

# Sanity: the script we shipped must be the same one we built against.
file(SIZE "${_installed_encoder}" _installed_size)
get_filename_component(_repo_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(_source_encoder "${_repo_root}/tools/cmake/scripts/encode_binary_data.py")
if(NOT EXISTS "${_source_encoder}")
    message(FATAL_ERROR
        "Source encoder ${_source_encoder} not found — repo layout broken?")
endif()
file(SIZE "${_source_encoder}" _source_size)
if(NOT _installed_size EQUAL _source_size)
    message(FATAL_ERROR
        "Installed encoder size (${_installed_size}) differs from "
        "source encoder size (${_source_size}) — install rule may be "
        "publishing a stale or different file.")
endif()

# Min-OS floor propagation: the SDK must ship PulpMinOs.cmake AND min_os.json
# beside it, and PulpConfig.cmake must include PulpMinOs.cmake. Without all
# three, a find_package(Pulp) consumer cannot resolve Pulp's floor and its
# plugin silently inherits the build host's OS floor (e.g. a macOS-26 machine
# ships a macOS-26-only plugin instead of Pulp's 13.4).
set(_installed_minos "${_pulp_cmake_dir}/PulpMinOs.cmake")
if(NOT EXISTS "${_installed_minos}")
    message(FATAL_ERROR
        "PulpMinOs.cmake not bundled with the installed Pulp SDK.\n"
        "Expected: ${_installed_minos}\n"
        "Consumers would not pin Pulp's min-OS floor and would ship plugins "
        "targeting the build host's OS version.")
endif()
set(_installed_minos_json "${_pulp_cmake_dir}/min_os.json")
if(NOT EXISTS "${_installed_minos_json}")
    message(FATAL_ERROR
        "min_os.json not bundled next to PulpMinOs.cmake in the installed SDK.\n"
        "Expected: ${_installed_minos_json}\n"
        "PulpMinOs.cmake finds no floor data and cannot pin the consumer.")
endif()
file(GLOB _pulp_config LIST_DIRECTORIES false
    "${_pulp_cmake_dir}/PulpConfig.cmake")
if(_pulp_config)
    list(GET _pulp_config 0 _pulp_config)
    file(READ "${_pulp_config}" _installed_config_text)
    if(NOT _installed_config_text MATCHES "PulpMinOs\\.cmake")
        message(FATAL_ERROR
            "Installed PulpConfig.cmake does not include PulpMinOs.cmake — the "
            "min-OS floor would not be pinned for find_package(Pulp) consumers.")
    endif()
endif()

# pulp::osc export: the OSC sender/receiver is a public SDK subsystem, but was
# long absent from PULP_SDK_TARGETS and the header-install loop. A consumer that
# links pulp::osc (a CV-to-OSC plugin, say) then fails find_package(Pulp) with an
# unknown target, or fails to compile against a missing pulp/osc header. Assert
# both halves of the export: the public header, and the exported target.
if(NOT EXISTS "${_prefix}/include/pulp/osc/osc.hpp")
    message(FATAL_ERROR
        "pulp/osc/osc.hpp not installed under ${_prefix}/include/pulp/osc/.\n"
        "The osc subsystem is missing from the header-install loop in "
        "PulpInstallRules.cmake, so find_package(Pulp) consumers that include "
        "<pulp/osc/osc.hpp> would fail to compile.")
endif()
# The install exports the imported target under the `Pulp::` namespace in
# PulpTargets*.cmake; PulpConfig.cmake then adds the lower-case `pulp::osc`
# alias consumers actually link. Assert both halves so neither can silently
# drop: the raw `Pulp::osc` export AND `pulp-osc`'s membership in the
# lower-case alias loop's target list.
file(GLOB _pulp_targets LIST_DIRECTORIES false "${_pulp_cmake_dir}/PulpTargets*.cmake")
set(_osc_target_found FALSE)
foreach(_tf IN LISTS _pulp_targets)
    file(READ "${_tf}" _tf_text)
    if(_tf_text MATCHES "Pulp::osc")
        set(_osc_target_found TRUE)
    endif()
endforeach()
if(NOT _osc_target_found)
    message(FATAL_ERROR
        "Pulp::osc is not in the exported target set (PulpTargets*.cmake under "
        "${_pulp_cmake_dir}).\n"
        "Add pulp-osc to PULP_SDK_TARGETS in PulpInstallRules.cmake so "
        "find_package(Pulp) consumers can link pulp::osc.")
endif()
file(READ "${_pulp_config}" _installed_config_alias_text)
if(NOT _installed_config_alias_text MATCHES "pulp-osc")
    message(FATAL_ERROR
        "pulp-osc is not in the lower-case alias loop's export-target list in "
        "PulpConfig.cmake, so the `pulp::osc` alias consumers link would never "
        "be created.\n"
        "Ensure pulp-osc is in PULP_SDK_TARGETS in PulpInstallRules.cmake.")
endif()

message(STATUS
    "Install layout: PulpUtils.cmake at ${_pulp_utils}, encoder at "
    "${_installed_encoder} (${_installed_size} bytes), PulpMinOs.cmake + "
    "min_os.json bundled and wired into PulpConfig.cmake, pulp::osc header + "
    "target exported. All present.")
