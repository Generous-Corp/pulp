#[[
Install-layout regression for issue-905 follow-up.

`pulp_add_binary_data` resolves its Python encoder via
${CMAKE_CURRENT_FUNCTION_LIST_DIR}/scripts/encode_binary_data.py — adjacent
to whichever `PulpUtils.cmake` is sourced. That convention only works in
the installed tree if the encoder is bundled alongside `PulpUtils.cmake`
under `lib/cmake/Pulp/scripts/`. If the install rule omits the encoder,
downstream `find_package(Pulp)` consumers see asset targets fail at
build time with a missing-script error.

This test runs `cmake --install` against a pre-configured Pulp build
into a temporary prefix, then asserts the bundled SDK contains both
`PulpUtils.cmake` and its sibling `scripts/encode_binary_data.py`.

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
        "lib/cmake/Pulp/scripts/ (issue-905 follow-up).")
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

message(STATUS
    "Install layout: PulpUtils.cmake at ${_pulp_utils}, encoder at "
    "${_installed_encoder} (${_installed_size} bytes). All present.")
